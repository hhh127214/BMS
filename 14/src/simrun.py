"""14/ — 仿真调度（「仿真模式」的执行体）

一次「仿真运行」= 跑 10/ 的离线仿真器一段时间（日 / 周 / 月），
把产物装成一份独立数据集（scenario），并留一条运行台账（sim_run）。

---------------------------------------------------------------------
几条口径，先写清楚，免得界面上出现"看着像、其实不是"的东西

1) **周 / 月仿真是"典型日重复"，不是真实多日序列。**
   10/ 的曲线是 96 点日曲线，`ForecastSeries::sample()` 对 t 取模一天，
   所以跑 30 天 = 同一个典型日重复 30 次。界面与文档都必须这么写 ——
   否则客户会以为月报反映的是真实的月度天气与负荷波动。

2) **故障注入只发生在第一天。**
   故障窗按绝对时间轴定义（08:00–08:30 等），跑 30 天时窗口只覆盖第 0 天。
   要"每天都出一次故障"需要改 10/ 的 FaultWindow 语义，本版本不做，
   但在运行台账里记了 fault=1，界面可以据此提示。

3) **粒度随时长放宽，且如实标注。**
   日 / 周用 10 s 粒度（与全项目图表默认一致），月用 60 s ——
   30 天 @10 s 是 25.9 万行，图表根本画不动，而库也没必要为"月报"
   存 10 s 级明细。log_every 存在 sim_run 表里，界面能显示真实粒度。

4) **跑完才入库。** 仿真器是批量程序，中途没有可看的部分结果。
   状态用 sim_run.status 表达（running / ok / failed），
   页面上就是"跑着 / 好了 / 失败了（带原因）"。
"""

from __future__ import annotations

import datetime
import os
import shutil
import subprocess
import sys
import threading
import time
from typing import Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import emsdb  # noqa: E402
import importer  # noqa: E402

PROJECT_ROOT = os.path.dirname(emsdb.MODULE_ROOT)
SIM_EXE = os.path.join(PROJECT_ROOT, "10", "build", "sim_demo.exe")
RUNS_DIR = os.path.join(emsdb.MODULE_ROOT, "data", "simruns")

# 三种时长。log_every 的单位是"控制拍"（dt=1 s），所以 log_every=10 → 10 s 粒度。
KINDS: dict[str, dict] = {
    "day": {
        "name": "日仿真", "short": "日", "prefix": "d",
        "duration_s": 86400.0, "dt_s": 1.0, "log_every": 10,
        "est_wall_s": 1.0,
    },
    "week": {
        "name": "周仿真", "short": "周", "prefix": "w",
        "duration_s": 7.0 * 86400.0, "dt_s": 1.0, "log_every": 10,
        "est_wall_s": 3.0,
    },
    "month": {
        "name": "月仿真", "short": "月", "prefix": "m",
        "duration_s": 30.0 * 86400.0, "dt_s": 1.0, "log_every": 60,
        "est_wall_s": 12.0,
    },
}

_LOCK = threading.RLock()
_ACTIVE: set[str] = set()


class SimRunError(Exception):
    """可预期的仿真错误，api.py 转成 4xx。"""


def kind_of(kind: str) -> dict:
    k = KINDS.get((kind or "").strip().lower())
    if not k:
        raise SimRunError(f"未知仿真时长「{kind}」，只能是 "
                          f"{' / '.join(KINDS)}（日 / 周 / 月）")
    return k


def ensure_dirs() -> str:
    os.makedirs(RUNS_DIR, exist_ok=True)
    return RUNS_DIR


def _now() -> str:
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def _next_run_id(conn, kind: str) -> str:
    prefix = KINDS[kind]["prefix"]
    n = emsdb.query_value(
        conn, "SELECT COUNT(*) FROM sim_run WHERE kind = ?", (kind,), 0)
    # 编号只增不减：即使删掉了中间某次，也不复用编号 ——
    # 复用会让"运行编号"与"数据集 id"的对应关系在审计台账里对不上。
    seq = int(n) + 1
    while True:
        rid = f"{prefix}{seq:03d}"
        exists = emsdb.query_value(
            conn, "SELECT COUNT(*) FROM sim_run WHERE run_id = ?", (rid,), 0)
        if not exists:
            return rid
        seq += 1


# ---------------------------------------------------------------------
# 查询
# ---------------------------------------------------------------------
def list_runs(conn, limit: int = 50) -> dict:
    rows = emsdb.query(
        conn,
        "SELECT r.*, s.name AS scenario_name, s.time_base,"
        "       (SELECT COUNT(*) FROM step_record t"
        "         WHERE t.scenario_id = r.scenario_id) AS rows_now"
        " FROM sim_run r LEFT JOIN scenario s ON s.scenario_id = r.scenario_id"
        " ORDER BY r.started_at DESC, r.run_id DESC LIMIT ?",
        (int(limit),))
    for r in rows:
        r["kind_name"] = KINDS.get(r.get("kind", ""), {}).get("name", r.get("kind"))
        r["duration_days"] = round((r.get("duration_s") or 0) / 86400.0, 2)
        r["est_wall_s"] = KINDS.get(r.get("kind", ""), {}).get("est_wall_s")
        # 跑着的那几次：库里的 wall_s 只有跑完才有，这里按 started_at 现算耗时，
        # 界面才能显示"已经跑了 N 秒"。完成/失败的用库里的 wall_s。
        r["elapsed_s"] = None
        if r.get("status") == "running":
            try:
                t0 = datetime.datetime.strptime(r["started_at"], "%Y-%m-%d %H:%M:%S")
                r["elapsed_s"] = round(
                    (datetime.datetime.now() - t0).total_seconds(), 1)
            except (TypeError, ValueError, KeyError):
                pass
    return {"count": len(rows), "items": rows,
            "kinds": [{"id": k, **{kk: vv for kk, vv in v.items() if kk != "prefix"}}
                      for k, v in KINDS.items()]}


def get_run(conn, run_id: str) -> dict:
    r = emsdb.query_one(
        conn,
        "SELECT r.*, s.name AS scenario_name, s.time_base, s.log_rows,"
        "       s.alarms_total AS scenario_alarms, s.ok AS scenario_ok,"
        "       e.saving_total_cny, e.cost_total_cny, e.cost_total_base_cny"
        " FROM sim_run r"
        " LEFT JOIN scenario s ON s.scenario_id = r.scenario_id"
        " LEFT JOIN economics e ON e.scenario_id = r.scenario_id"
        " WHERE r.run_id = ?", (run_id,))
    if not r:
        raise SimRunError(f"找不到仿真运行 {run_id}")
    r["kind_name"] = KINDS.get(r.get("kind", ""), {}).get("name", r.get("kind"))
    r["duration_days"] = round((r.get("duration_s") or 0) / 86400.0, 2)
    days = emsdb.query(
        conn, "SELECT date, charge_kwh, discharge_kwh, pv_kwh, load_kwh,"
              " grid_kwh, export_kwh, peak_grid_kw, net_benefit_cny"
              " FROM energy_statistics WHERE scenario_id = ? ORDER BY date",
        (r["scenario_id"],))
    r["energy_days"] = days
    r["day_count"] = len(days)
    r["rows_now"] = emsdb.query_value(
        conn, "SELECT COUNT(*) FROM step_record WHERE scenario_id = ?",
        (r["scenario_id"],), 0)
    return r


# ---------------------------------------------------------------------
# 启动一次仿真
# ---------------------------------------------------------------------
def start(conn, kind: str, fault: bool = False, username: str = "",
          date: Optional[str] = None) -> dict:
    k = kind_of(kind)
    if not os.path.isfile(SIM_EXE):
        raise SimRunError(f"仿真器不存在：{SIM_EXE}\n先编译：10/scripts/build.bat")

    with _LOCK:
        run_id = _next_run_id(conn, kind)
    scenario_id = f"sim-{run_id}"
    out_dir = os.path.join(ensure_dirs(), run_id)
    os.makedirs(out_dir, exist_ok=True)

    started = _now()
    conn.execute(
        "INSERT INTO sim_run(run_id, kind, scenario_id, fault, duration_s,"
        " dt_s, log_every, status, started_at, artifact_dir, username)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?)",
        (run_id, kind, scenario_id, 1 if fault else 0, k["duration_s"],
         k["dt_s"], k["log_every"], "running", started,
         os.path.relpath(out_dir, PROJECT_ROOT).replace("\\", "/"), username))
    conn.commit()
    import auth
    auth.audit(conn, username or "-", "sim_run_start", "engine",
               f"{run_id} kind={kind} fault={int(bool(fault))}")

    th = threading.Thread(target=_runner, name=f"ems-sim-{run_id}",
                          args=(run_id, scenario_id, kind, bool(fault),
                                out_dir, date, emsdb.db_path_of(conn)), daemon=True)
    with _LOCK:
        _ACTIVE.add(run_id)
    th.start()
    return get_run(conn, run_id)


def _runner(run_id: str, scenario_id: str, kind: str, fault: bool,
            out_dir: str, date: Optional[str], db_path: str = "") -> None:
    """后台线程：跑仿真器 → 入库 → 更新台账。

    线程自己开连接：14/ 的其余部分都是"一请求一连接"，
    这里也必须自成一体，否则会和请求线程抢同一个 sqlite3 连接
    （sqlite3 连接默认不允许跨线程使用，会用时抛 ProgrammingError）。
    ★ db_path 必须**从请求连接上继承**，不能图省事用默认库 ——
      自检用临时库、将来运维可能用 --db 指定别的库，写错库是不报错的。
    """
    k = KINDS[kind]
    title = (f"{k['name']} · 典型日（{'故障注入' if fault else '正常'}）"
             f" · {run_id}")
    cmd = [SIM_EXE,
           "--out", out_dir,
           "--title", title,
           "--duration-s", repr(k["duration_s"]),
           "--log-every", str(k["log_every"])]
    if fault:
        cmd.append("--fault")

    t0 = time.time()
    status, err, wall_s = "ok", "", 0.0
    proc = None
    try:
        proc = subprocess.Popen(cmd, cwd=PROJECT_ROOT,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT,
                                creationflags=_no_window_flags())
        out, _ = proc.communicate(timeout=1800)
        wall_s = time.time() - t0
        text = (out or b"").decode("utf-8", errors="replace")
        if proc.returncode != 0:
            status = "failed"
            tail = "\n".join([ln for ln in text.strip().splitlines()[-6:]])
            err = f"仿真器退出码 {proc.returncode}：{tail}"
        elif "[SIM OK]" not in text:
            status = "failed"
            err = "仿真器没报 [SIM OK]（硬不变量可能违规）：" + \
                  "\n".join([ln for ln in text.strip().splitlines()[-6:]])
    except subprocess.TimeoutExpired:
        status = "failed"
        err = "仿真超时（30 分钟）"
        if proc:
            proc.kill()
    except Exception as e:  # 兜底：线程死掉会让台账永远停在 running
        status = "failed"
        err = f"{type(e).__name__}: {e}"

    conn = emsdb.connect(db_path or None)
    try:
        if status != "ok":
            conn.execute(
                "UPDATE sim_run SET status='failed', finished_at=?, error=?,"
                " wall_s=? WHERE run_id=?", (_now(), err, wall_s, run_id))
            conn.commit()
            return

        try:
            res = importer.import_scenario(
                conn, out_dir, scenario_id,
                f"{k['name']}{run_id} · 典型日（{'故障注入' if fault else '正常'}）",
                date)
        except Exception as e:
            conn.execute(
                "UPDATE sim_run SET status='failed', finished_at=?, error=?,"
                " wall_s=? WHERE run_id=?",
                (_now(), f"入库失败 {type(e).__name__}: {e}", wall_s, run_id))
            conn.commit()
            return

        econ = emsdb.query_one(
            conn, "SELECT net_benefit_cny, saving_total_cny FROM economics"
                  " WHERE scenario_id = ?", (scenario_id,)) or {}
        conn.execute(
            "UPDATE sim_run SET status='ok', finished_at=?, wall_s=?,"
            " steps=?, log_rows=?, alarms_total=?, net_benefit_cny=?,"
            " saving_total_cny=?, invariants_all_ok=?, error='' WHERE run_id=?",
            (_now(), wall_s, res["steps"], res["steps"],
             res["alarms"], econ.get("net_benefit_cny"),
             econ.get("saving_total_cny"), 1 if res["all_ok"] else 0, run_id))
        conn.commit()
    finally:
        with _LOCK:
            _ACTIVE.discard(run_id)
        conn.close()


def _no_window_flags() -> int:
    if os.name == "nt":
        return getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)
    return 0


# ---------------------------------------------------------------------
# 删除
# ---------------------------------------------------------------------
def delete(conn, run_id: str, username: str = "") -> dict:
    r = emsdb.query_one(conn, "SELECT * FROM sim_run WHERE run_id = ?", (run_id,))
    if not r:
        raise SimRunError(f"找不到仿真运行 {run_id}")
    with _LOCK:
        if run_id in _ACTIVE:
            raise SimRunError(f"{run_id} 正在运行，先等它结束再删")
    sid = r["scenario_id"]
    counts = {}
    for t in ("step_record", "control_command", "alarm", "economics",
              "invariant", "energy_statistics"):
        counts[t] = emsdb.query_value(
            conn, f"SELECT COUNT(*) FROM {t} WHERE scenario_id = ?", (sid,), 0)
        conn.execute(f"DELETE FROM {t} WHERE scenario_id = ?", (sid,))
    conn.execute("DELETE FROM scenario WHERE scenario_id = ?", (sid,))
    conn.execute("DELETE FROM sim_run WHERE run_id = ?", (run_id,))
    conn.commit()

    art = r.get("artifact_dir") or ""
    removed_dir = False
    if art:
        p = os.path.normpath(os.path.join(PROJECT_ROOT, art))
        # 只删自己目录下的产物目录，且必须确实是本模块建的 runs 目录 ——
        # 别让一个写错的 artifact_dir 把仓库别处删了。
        if p.startswith(os.path.normpath(RUNS_DIR)) and os.path.isdir(p):
            shutil.rmtree(p, ignore_errors=True)
            removed_dir = True

    import auth
    if username:
        auth.audit(conn, username, "sim_run_delete", "engine",
                   f"{run_id} 场景 {sid}（清了 {counts.get('step_record', 0)} 行时序）")
    return {"run_id": run_id, "scenario_id": sid, "removed": counts,
            "removed_dir": removed_dir}


def active() -> list[str]:
    with _LOCK:
        return sorted(_ACTIVE)
