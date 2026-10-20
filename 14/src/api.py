"""14/ — REST API

路由表在文件底部 ROUTES。所有接口返回 JSON。
只读接口最低角色 viewer，写接口 operator，用户管理 admin。

设计口径：
  * 平台**不参与实时闭环**。它读数据库、展示、审计；
    控制指令接口在无真实执行器时只做「记录 + 审计」，绝不假装下发成功。
  * 「实时」有两套时基（v1.5 起前端不做回放）：仿真数据集是时标 t_s
    （0..86400），现场数据集是**墙钟 Unix 秒**。前端只做「某一时刻的快照查询」，
    不提供播放 —— 回放是研发调试动作，产品界面不该有。
"""

from __future__ import annotations

import csv
import datetime
import io
import json
import os
import re
import sys
from typing import Any, Callable, Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import accessconf  # noqa: E402
import auth  # noqa: E402
import connconf  # noqa: E402
import emsdb  # noqa: E402
import engine  # noqa: E402
import pointmap  # noqa: E402
import simconf  # noqa: E402
import simproc  # noqa: E402
import simrun  # noqa: E402
import uplinkconf  # noqa: E402
import upmapconf  # noqa: E402

# 默认数据场景。只在"引擎模式也拿不出数据集"时兜底（见 scenario_of）。
DEFAULT_SCENARIO = "normal"
# 仿真总时长（秒）：24 h。用于曲线与回放的时标上界（仅单日场景适用）。
SIM_DURATION_S = 86400.0


class ApiError(Exception):
    def __init__(self, message: str, status: int = 400):
        super().__init__(message)
        self.message = message
        self.status = status


# ---------------------------------------------------------------------
# 参数辅助
# ---------------------------------------------------------------------
def q1(query: dict, key: str, default: Any = None) -> Any:
    v = query.get(key)
    if isinstance(v, list):
        return v[0] if v else default
    return v if v is not None else default


def qf(query: dict, key: str, default: float) -> float:
    try:
        return float(q1(query, key, default))
    except (TypeError, ValueError):
        return default


def qi(query: dict, key: str, default: int) -> int:
    try:
        return int(float(q1(query, key, default)))
    except (TypeError, ValueError):
        return default


def scenario_of(query: dict) -> str:
    s = str(q1(query, "scenario", DEFAULT_SCENARIO) or DEFAULT_SCENARIO).strip()
    # live 场景（07/ --record 实时入库）的 scenario_id 不是 normal/fault 白名单里的，
    # 不能在这里硬拦 —— 否则界面上选到 live 场景时，所有时序接口都静默退回 normal。
    # 合法性：只允许 [A-Za-z0-9_-]，且长度受限。所有下游查询都是参数化
    # （scenario_id = ?），这个正则只是挡掉明显异常输入。
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", s):
        return DEFAULT_SCENARIO
    return s


def resolve_scenario(conn, query: dict) -> str:
    """本次请求该读哪个数据集。

    与 scenario_of 的分工：
      * 前端**显式**带了 ?scenario=xxx  → 就用 xxx（scenario_of 做白名单校验）；
      * 前端**没带**                     → 交给引擎模式决定（engine.default_scenario）。

    ★ 为什么不能让"没带"默认成 'normal'：
      双模式下的默认数据集是**跟着引擎模式走**的 —— 运行模式看 live，
      仿真模式看最近一次成功仿真。硬编码 normal 会让运行模式打开就是
      "内置演示数据"，看起来在跑，其实一动不动。
    """
    if q1(query, "scenario") not in (None, ""):
        return scenario_of(query)
    return engine.default_scenario(conn)


def clamp(v: float, lo: float, hi: float) -> float:
    return lo if v < lo else (hi if v > hi else v)


def r3(v: Any, nd: int = 3) -> Optional[float]:
    """四舍五入用于展示。None 原样返回（不要在 API 层把 NULL 变成 0）。"""
    if v is None:
        return None
    try:
        return round(float(v), nd)
    except (TypeError, ValueError):
        return None


# ---------------------------------------------------------------------
# 系统
# ---------------------------------------------------------------------
def h_health(conn, query, body, user, params) -> tuple[int, dict]:
    counts = emsdb.table_counts(conn)
    scenes = emsdb.query(conn, "SELECT scenario_id, name, steps, log_rows,"
                               " ok, invariants_all_ok, imported_at FROM scenario")
    return 200, {
        "ok": True,
        "service": "ems-platform-api",
        "module": "14/",
        "schema_version": emsdb.get_meta(conn, emsdb.META_SCHEMA_VERSION),
        "imported_at": emsdb.get_meta(conn, emsdb.META_IMPORT_AT),
        "import_source": emsdb.get_meta(conn, emsdb.META_IMPORT_SOURCE),
        "db": os.path.basename(emsdb.DEFAULT_DB_PATH),
        "scenarios": scenes,
        "tables": counts,
    }


def h_login(conn, query, body, user, params) -> tuple[int, dict]:
    username = str(body.get("username", "")).strip()
    password = str(body.get("password", ""))
    if not username or not password:
        raise ApiError("请填写用户名与口令", 400)
    u = auth.authenticate(conn, username, password)
    if not u:
        auth.audit(conn, username, "login_failed", "session", "口令或账号错误")
        raise ApiError("用户名或口令错误", 401)
    token = auth.issue_token(conn, u["user_id"])
    auth.audit(conn, username, "login", "session", f"角色 {u['role']}")
    return 200, {"token": token, "user": u,
                 "expires_in_h": auth.TOKEN_TTL_HOURS}


def h_logout(conn, query, body, user, params) -> tuple[int, dict]:
    tok = str(body.get("token", "") or q1(query, "token", ""))
    if tok:
        auth.revoke_token(conn, tok)
    if user:
        auth.audit(conn, user["username"], "logout", "session", "")
    return 200, {"ok": True}


def h_me(conn, query, body, user, params) -> tuple[int, dict]:
    return 200, {"user": user, "role_level": auth.ROLE_LEVEL.get(
        (user or {}).get("role", ""), 0)}


# ---------------------------------------------------------------------
# 首页总览
# ---------------------------------------------------------------------
def h_overview(conn, query, body, user, params) -> tuple[int, dict]:
    sc = resolve_scenario(conn, query)
    # 取最后一拍作为「当前」；t 参数可指定任意时刻（回放用）
    t = qf(query, "t", -1.0)
    if t < 0:
        row = emsdb.query_one(
            conn, "SELECT * FROM step_record WHERE scenario_id = ?"
                  " ORDER BY t_s DESC LIMIT 1", (sc,))
    else:
        row = emsdb.query_one(
            conn, "SELECT * FROM step_record WHERE scenario_id = ?"
                  " AND t_s <= ? ORDER BY t_s DESC LIMIT 1", (sc, t))
    if not row:
        # 运行模式下的"没有数据"几乎总是「引擎还没启动」，而不是"导入出错"——
        # 所以给一条能照着做的提示，而不是通用的"请先导入"。
        if sc == engine.LIVE_SCENARIO:
            raise ApiError(
                "运行模式还没有实时数据：采集未启动（或刚启动、还没到第一个写点）。"
                "请在「设备管理」页的「二 · 站内设备接入」里点「启动采集」。", 404)
        raise ApiError(f"场景 {sc} 没有时序数据，请先导入", 404)

    econ = emsdb.query_one(conn, "SELECT * FROM economics WHERE scenario_id = ?",
                           (sc,)) or {}
    inv = emsdb.query_one(conn, "SELECT * FROM invariant WHERE scenario_id = ?",
                          (sc,)) or {}
    scn = emsdb.query_one(conn, "SELECT * FROM scenario WHERE scenario_id = ?",
                          (sc,)) or {}
    dev = emsdb.query_one(conn, "SELECT * FROM device WHERE device_id = 'DEV-BMS-01'")
    tr = emsdb.query_one(conn, "SELECT * FROM device WHERE device_id = 'DEV-TR-01'")

    # 告警分档计数
    alarm_by_level = {r["level"]: r["n"] for r in emsdb.query(
        conn, "SELECT level, COUNT(*) AS n FROM alarm WHERE scenario_id = ?"
              " GROUP BY level", (sc,))}

    return 200, {
        "scenario": sc,
        "scenario_name": scn.get("name"),
        "t_s": r3(row["t_s"], 1),
        "time": _time_label(conn, sc, row),
        "state": row["state"],
        # 四路功率（约定：储能放电为正）
        "power": {
            "grid_kw": r3(row["p_grid_kw"]),
            "pv_kw": r3(row["p_pv_kw"]),
            "load_kw": r3(row["p_load_kw"]),
            "battery_kw": r3(row["p_actual_kw"]),
            "command_kw": r3(row["p_cmd_kw"]),
            # 关口（电网接入点）限值 = 并网变压器额定容量，用于「关口功率」卡判越限
            "grid_limit_kw": r3(tr.get("rated_power_kw")) if tr else None,
            "permission": {"lower_kw": r3(row["p_lower_kw"]),
                           "upper_kw": r3(row["p_upper_kw"])},
        },
        "battery": {
            "soc": r3(row["soc"], 4),
            "temperature_c": r3(row["temp_c"]),
            "capacity_kwh": r3(dev.get("capacity_kwh") if dev else None, 1),
        },
        "economics": {
            "net_benefit_cny": r3(econ.get("net_benefit_cny"), 2),
            "saving_total_cny": r3(econ.get("saving_total_cny"), 2),
            "saving_pct": r3(econ.get("saving_pct"), 2),
            "cost_total_cny": r3(econ.get("cost_total_cny"), 2),
            "cost_total_base_cny": r3(econ.get("cost_total_base_cny"), 2),
        },
        "alarms": {
            "total": scn.get("alarms_total"),
            "fault": scn.get("alarms_fault"),
            "by_level": alarm_by_level,
        },
        "invariants": inv,
        "loop": _loop_block(
            conn, sc, row,
            emsdb.query_one(
                conn, "SELECT MIN(t_s) AS t0, MAX(t_s) AS t1 FROM step_record"
                      " WHERE scenario_id = ?", (sc,)) or {}),
        # 当前激活的运行模式（业务维度，独立于数据场景 scenario）
        "mode": _mode_row(emsdb.query_one(
            conn, "SELECT * FROM run_mode WHERE active = 1") or {}),
        # 数据来源（数据维度）：本次读到的是实时引擎还是离线仿真结果
        "engine": engine.get_mode(conn),
        "time_base": _time_base(conn, sc),
    }


def _loop_block(conn, sc: str, row: dict, span: dict) -> dict:
    """回放进度。

    ★ 两套时刻口径不能混算（这是本项目最容易错的地方）：
      sim  场景 t_s 是"一天内的秒"（0..86400），进度 = t/86400 天然成立；
      wall 场景 t_s 是 Unix 墙钟秒（~1.7e9）。同一个公式会算出 20000% 的进度，
      界面上就是一条永远满格的进度条 —— 比不显示更坏。
      所以 wall 场景不给 progress，改给 elapsed_s（已累积时长）与
      days_span（已累积天数），前端按"还在增长中"的口径展示。
    """
    if _time_base(conn, sc) == "wall":
        t0, t1 = span.get("t0"), span.get("t1")
        elapsed = (float(t1) - float(t0)) if (t0 is not None and t1 is not None) \
            else 0.0
        return {
            "t_s": r3(row["t_s"], 1),
            "time_base": "wall",
            "duration_s": None,          # 实时数据没有终点
            "progress": None,
            "elapsed_s": r3(elapsed, 1),
            "days_span": r3(elapsed / 86400.0, 4),
            "clamped": bool(row["clamped"]),
            "safety_clip": bool(row["safety_clip"]),
            "state_gated": bool(row["state_gated"]),
            "hold_last": bool(row["hold_last"]),
            "fault_bits": row["fault_bits"],
            "reason": row["reason"],
        }
    return {
        "t_s": r3(row["t_s"], 1),
        "time_base": "sim",
        "duration_s": SIM_DURATION_S,
        "progress": r3((row["t_s"] or 0) / SIM_DURATION_S, 6),
        "clamped": bool(row["clamped"]),
        "safety_clip": bool(row["safety_clip"]),
        "state_gated": bool(row["state_gated"]),
        "hold_last": bool(row["hold_last"]),
        "fault_bits": row["fault_bits"],
        "reason": row["reason"],
    }


# ---------------------------------------------------------------------
# 设备
# ---------------------------------------------------------------------
def h_devices(conn, query, body, user, params) -> tuple[int, dict]:
    rows = emsdb.query(conn, "SELECT * FROM device ORDER BY device_id")
    return 200, {"count": len(rows), "items": rows}


def h_device_one(conn, query, body, user, params) -> tuple[int, dict]:
    dev_id = params["device_id"]
    d = emsdb.query_one(conn, "SELECT * FROM device WHERE device_id = ?", (dev_id,))
    if not d:
        raise ApiError(f"设备不存在: {dev_id}", 404)
    sc = resolve_scenario(conn, query)
    last = emsdb.query_one(
        conn, "SELECT ts, power_kw, soc, temperature_c, quality_ok FROM"
              " realtime_data WHERE device_id = ? AND ts = (SELECT MAX(ts)"
              " FROM realtime_data WHERE device_id = ?)", (dev_id, dev_id))
    return 200, {"device": d, "scenario": sc, "latest_realtime": last}


# ---------------------------------------------------------------------
# 实时 / 时序 / 曲线
# ---------------------------------------------------------------------
def h_realtime(conn, query, body, user, params) -> tuple[int, dict]:
    """某一时刻的全站实时快照。

    数据来源是 10/ 的离线仿真 —— 本项目尚未接真机，
    所以这里扮演的是「设备侧进程上线后应写入的样子」。
    """
    sc = resolve_scenario(conn, query)
    t = qf(query, "t", -1.0)
    if t < 0:
        row = emsdb.query_one(
            conn, "SELECT * FROM step_record WHERE scenario_id = ?"
                  " ORDER BY t_s DESC LIMIT 1", (sc,))
    else:
        row = emsdb.query_one(
            conn, "SELECT * FROM step_record WHERE scenario_id = ?"
                  " AND t_s <= ? ORDER BY t_s DESC LIMIT 1", (sc, t))
    if not row:
        raise ApiError(f"场景 {sc} 没有时序数据", 404)

    def dev(did, power=None, soc=None, temp=None, status=None):
        return {"device_id": did, "power_kw": r3(power), "soc": r3(soc, 4),
                "temperature_c": r3(temp), "status": status,
                "quality_ok": 1}

    return 200, {
        "scenario": sc, "t_s": r3(row["t_s"], 1),
        "time": _time_label(conn, sc, row),
        "state": row["state"],
        "items": [
            dev("DEV-METER-01", row["p_grid_kw"],
                status="online" if row["p_grid_kw"] is not None else "offline"),
            dev("DEV-PV-01", row["p_pv_kw"]),
            dev("DEV-LOAD-01", row["p_load_kw"]),
            dev("DEV-PCS-01", row["p_actual_kw"], status=row["state"]),
            dev("DEV-BMS-01", None, row["soc"], row["temp_c"]),
        ],
        "reason": row["reason"],
    }


def h_series(conn, query, body, user, params) -> tuple[int, dict]:
    """等间隔降采样后的曲线。

    keys: 逗号分隔的 step_record 列名。默认给曲线页用的三条。
    points: 采样点数（默认 96，与 15 min 预报粒度一致）
    """
    sc = resolve_scenario(conn, query)
    keys = str(q1(query, "keys", "p_grid_kw,soc,p_cmd_kw") or "").split(",")
    keys = [k.strip() for k in keys if k.strip()]
    allowed = set(STEP_COLUMNS) - {"scenario_id", "t_s", "time_str", "state",
                                   "reason"}
    bad = [k for k in keys if k not in allowed]
    if bad:
        raise ApiError(f"不支持的曲线字段: {','.join(bad)}", 400)
    points = max(2, min(2000, qi(query, "points", 96)))

    rows = emsdb.query(
        conn, f"SELECT t_s, time_str, {','.join(keys)} FROM step_record"
              f" WHERE scenario_id = ? ORDER BY t_s", (sc,))
    if not rows:
        raise ApiError(f"场景 {sc} 没有时序数据", 404)

    n = len(rows)
    idx = [round(i * (n - 1) / (points - 1)) for i in range(points)]
    idx = sorted(set(idx))
    sampled = [rows[i] for i in idx]

    # ★ 时刻口径分支：sim 场景 t_s 是"一天内秒"，time 转成 HH:MM；
    #   wall 场景 t_s 是 Unix 墙钟秒，直接用 time_str（已是完整日期时间）。
    #   若统一用 _hhmm，墙钟秒会被 %86400 折成"一天内的第几秒"，跨日曲线全错。
    wall = _time_base(conn, sc) == "wall"
    time_labels = [r["time_str"] for r in sampled] if wall \
        else [_hhmm(r["t_s"]) for r in sampled]

    return 200, {
        "scenario": sc, "points": len(sampled), "rows_total": n,
        "keys": keys, "time_base": "wall" if wall else "sim",
        "t_s": [r3(r["t_s"], 1) for r in sampled],
        "time": time_labels,
        "series": {k: [r3(r[k], 4) for r in sampled] for k in keys},
    }


STEP_COLUMNS = [
    "t_s", "time_str", "state", "p_load_kw", "p_pv_kw", "p_grid_kw",
    "p_cmd_kw", "p_actual_kw", "soc", "temp_c", "p_lower_kw", "p_upper_kw",
    "plan_target", "correction", "clamped", "safety_clip", "state_gated",
    "hold_last", "fault_bits", "reason",
]


def _hhmm(t_s: Any) -> str:
    """sim 口径（秒）→ 人看的时刻标签。

    ★ 为什么不能 `(m//60) % 24` 了事：**`t_s = 86400`（24 h 仿真的最后一拍）
      会被折回 "00:00"**，和当天第一拍长得一模一样 —— 用户看到的就是
      「时刻永远是 0:00」，而且拖到时间轴最右端还是 0:00。
      86400 秒是**第一天的终点**，该写 "24:00"，不能写 "00:00"。

    ★ 周（604800 s）/ 月（2592000 s）仿真跨多天：折进一天会让第 2 天凌晨
      和昨天凌晨共用同一个标签，曲线自相矛盾 —— 加 `Dn ` 前缀标明第几天。
      （importer 的日聚合把 86400 判给第 0 天，这里的 D 前缀是同一套口径。）
    """
    try:
        v = float(t_s)
    except (TypeError, ValueError):
        return ""
    day, rem = divmod(int(v), 86400)
    mm = rem // 60
    hhmm = f"{mm // 60:02d}:{mm % 60:02d}"
    if day == 0:
        return hhmm                       # 第一天之内：00:00 ~ 23:59
    if rem == 0:
        return "24:00" if day == 1 else f"D{day} 24:00"
    return f"D{day + 1} {hhmm}"


def _time_label(conn, scenario_id: str, row: dict) -> str:
    """某一拍在界面上该显示的时刻。

    ★ sim 口径**不要**直接贴 `row["time_str"]`：那是 10/ 仿真写出来的 HH:MM，
      同样把 86400 折成了 "00:00"（2026-10-19 实测 sim-d002 末拍）。
      统一由 `t_s` 现算，才能与曲线横轴的 `_hhmm` 同源 —— 两处各算一套
      就会在同一屏上写出两个不同的"现在"。
      wall 口径的 time_str 是现场进程写的完整日期时间，照用（含时区语义）。
    """
    if _time_base(conn, scenario_id) == "wall":
        return row["time_str"] or ""
    return _hhmm(row["t_s"]) or (row["time_str"] or "")


def _time_base(conn, scenario_id: str) -> str:
    """场景的时刻口径：'sim'（一天内秒）/ 'wall'（Unix 墙钟秒）。"""
    row = emsdb.query_one(
        conn, "SELECT time_base FROM scenario WHERE scenario_id = ?",
        (scenario_id,))
    return (row or {}).get("time_base") or "sim"


def h_steps(conn, query, body, user, params) -> tuple[int, dict]:
    sc = resolve_scenario(conn, query)
    t0, t1 = _time_window(conn, sc, query)
    limit = max(1, min(5000, qi(query, "limit", 200)))
    offset = max(0, qi(query, "offset", 0))
    total = emsdb.query_value(
        conn, "SELECT COUNT(*) FROM step_record WHERE scenario_id = ?"
              " AND t_s BETWEEN ? AND ?", (sc, t0, t1), 0)
    rows = emsdb.query(
        conn, "SELECT * FROM step_record WHERE scenario_id = ?"
              " AND t_s BETWEEN ? AND ? ORDER BY t_s LIMIT ? OFFSET ?",
        (sc, t0, t1, limit, offset))
    return 200, {"scenario": sc, "total": total, "limit": limit,
                 "offset": offset, "items": rows}


def _time_window(conn, sc: str, query: dict) -> tuple[float, float]:
    """时序/指令接口的默认时间窗。

    ★ wall 场景（运行模式）t_s 是 Unix 墙钟秒，用 [0, 86400] 当默认窗
      会把**所有**数据过滤掉 —— 表现是"接口 200 但 items 空"，
      比报错更难查。所以 wall 场景默认开成无穷界（全量）。
      显式传 from/to 时仍按传入值（前端画"最近 N 分钟"就是靠这个）。
    """
    if _time_base(conn, sc) == "wall":
        return qf(query, "from", -1.0e18), qf(query, "to", 1.0e18)
    return qf(query, "from", 0.0), qf(query, "to", SIM_DURATION_S)


def h_commands(conn, query, body, user, params) -> tuple[int, dict]:
    sc = resolve_scenario(conn, query)
    t0, t1 = _time_window(conn, sc, query)
    limit = max(1, min(5000, qi(query, "limit", 200)))
    offset = max(0, qi(query, "offset", 0))
    result = q1(query, "result")
    where = "scenario_id = ? AND ts BETWEEN ? AND ?"
    args: list[Any] = [sc, t0, t1]
    if result:
        where += " AND result = ?"
        args.append(result)
    total = emsdb.query_value(conn, f"SELECT COUNT(*) FROM control_command"
                                    f" WHERE {where}", args, 0)
    by_result = {r["result"]: r["n"] for r in emsdb.query(
        conn, f"SELECT result, COUNT(*) AS n FROM control_command"
              f" WHERE scenario_id = ? GROUP BY result", (sc,))}
    rows = emsdb.query(
        conn, f"SELECT * FROM control_command WHERE {where}"
              f" ORDER BY ts LIMIT ? OFFSET ?", args + [limit, offset])
    return 200, {"scenario": sc, "total": total, "by_result": by_result,
                 "limit": limit, "offset": offset, "items": rows}


# ---------------------------------------------------------------------
# 告警
# ---------------------------------------------------------------------
def h_alarms(conn, query, body, user, params) -> tuple[int, dict]:
    sc = resolve_scenario(conn, query)
    level = q1(query, "level")
    source = q1(query, "source")
    limit = max(1, min(2000, qi(query, "limit", 100)))
    offset = max(0, qi(query, "offset", 0))
    where, args = "scenario_id = ?", [sc]
    if level:
        where += " AND level = ?"
        args.append(level)
    # 来源维度：与 level 同一套拼法，两者可任意组合（AND）。
    # ★ 两个 if 都靠"空串 / None 为假"跳过条件，别改成 `is not None`：
    #   前端"全部"选项传的是空串（api.js 的 url() 会直接把它丢掉，但后端不能依赖前端），
    #   一旦被当成条件，SQL 会去匹配 source='' 的行，结果恒为 0 条 ——
    #   不报错、界面只是"什么都没有"，是最难查的那种失效。
    #   另：where 拼接顺序必须与 args.append 顺序严格一致，否则条件张冠李戴。
    #
    # ★ 为什么不开放 status（active / cleared）筛选：
    #   importer.py:395 把所有告警行的 status 硬编码成 "active"，库里 290 行没有任何
    #   一条是 cleared（实测 SELECT status, COUNT(*) ... GROUP BY status → 只有 active/290）。
    #   给它一个筛选下拉 = "已恢复"永远 0 条 —— 那是假入口，比没有更糟。
    #   等有了真实的告警恢复路径（10/ 侧报 cleared）再加，届时就是下面同样的三行。
    if source:
        where += " AND source = ?"
        args.append(source)
    total = emsdb.query_value(conn, f"SELECT COUNT(*) FROM alarm WHERE {where}",
                              args, 0)
    rows = emsdb.query(
        conn, f"SELECT * FROM alarm WHERE {where} ORDER BY ts LIMIT ? OFFSET ?",
        args + [limit, offset])
    return 200, {"scenario": sc, "total": total, "limit": limit,
                 "offset": offset, "items": rows}


def h_alarm_summary(conn, query, body, user, params) -> tuple[int, dict]:
    sc = resolve_scenario(conn, query)
    level = q1(query, "level")
    source = q1(query, "source")

    # ★ 交叉筛选（drill-down）：每个维度的计数要带上"其他维度"的筛选，但不带自己。
    #     按等级 的计数 → 应用 source，不用 level
    #     按来源 的计数 → 应用 level，  不用 source
    #     24h 分布      → 两个都用（它是结果视图，不是筛选入口）
    #   为什么必须这样：若"按等级"也应用 level，选中 FAULT 之后 WARNING 就变成 0，
    #   用户再也点不回去；若两个维度都不应用任何筛选，按钮上写的 270 点进去却是
    #   12 条 —— 计数与结果对不上，是最容易被当成 bug 的一类错觉。
    def _where(use_level: bool, use_source: bool):
        w, a = "scenario_id = ?", [sc]
        if use_level and level:
            w += " AND level = ?"
            a.append(level)
        if use_source and source:
            w += " AND source = ?"
            a.append(source)
        return w, a

    wl, al = _where(False, True)
    by_level = emsdb.query(
        conn, f"SELECT level, COUNT(*) AS n FROM alarm WHERE {wl}"
              " GROUP BY level ORDER BY n DESC", al)
    # ★ 分类视图要"全谱"：没发生的等级也得占一行（n=0），不能因为 GROUP BY
    #   查不到就不显示。否则界面上只看见 FAULT，会让人以为 WARNING 这一档不存在、
    #   或者以为它被漏统计了 —— 「没告警」和「没统计到」在界面上长得一模一样。
    ALARM_LEVELS = ("INFO", "WARNING", "DERATED", "FAULT", "EMERGENCY")
    got = {r["level"]: r["n"] for r in by_level}
    by_level = [{"level": lv, "n": got.get(lv, 0)} for lv in ALARM_LEVELS]
    # 库里若出现了上面五个之外的值（新等级），也要能看见 —— 排在末尾，不静默丢掉
    for lv, n in got.items():
        if lv not in ALARM_LEVELS:
            by_level.append({"level": lv, "n": n})
    ws, asrc = _where(True, False)
    by_source = emsdb.query(
        conn, f"SELECT source, COUNT(*) AS n FROM alarm WHERE {ws}"
              " GROUP BY source ORDER BY n DESC LIMIT 20", asrc)
    wt, at = _where(True, True)
    timeline = emsdb.query(
        conn, f"SELECT CAST(ts/3600 AS INTEGER) AS hour, COUNT(*) AS n"
              f" FROM alarm WHERE {wt} GROUP BY hour ORDER BY hour", at)
    return 200, {"scenario": sc, "by_level": by_level,
                 "by_source": by_source, "timeline": timeline}


# ---------------------------------------------------------------------
# 策略配置
# ---------------------------------------------------------------------
def h_strategies(conn, query, body, user, params) -> tuple[int, dict]:
    rows = emsdb.query(conn, "SELECT * FROM strategy_config"
                             " ORDER BY priority, strategy_id")
    for r in rows:
        try:
            r["parameters"] = json.loads(r["parameters"] or "{}")
        except (TypeError, ValueError):
            r["parameters"] = {}
    return 200, {"count": len(rows), "items": rows}


def h_strategy_update(conn, query, body, user, params) -> tuple[int, dict]:
    sid = params["strategy_id"]
    cur = emsdb.query_one(conn, "SELECT * FROM strategy_config"
                                " WHERE strategy_id = ?", (sid,))
    if not cur:
        raise ApiError(f"策略不存在: {sid}", 404)

    sets, args = [], []
    if "enabled" in body:
        sets.append("enabled = ?")
        args.append(1 if body["enabled"] else 0)
    if "priority" in body:
        prio = str(body["priority"])
        if prio not in ("L0", "L1", "L2", "L3"):
            raise ApiError("priority 只能是 L0/L1/L2/L3", 400)
        sets.append("priority = ?")
        args.append(prio)
    if "parameters" in body:
        if not isinstance(body["parameters"], dict):
            raise ApiError("parameters 必须是对象", 400)
        sets.append("parameters = ?")
        args.append(json.dumps(body["parameters"], ensure_ascii=False))
    if "description" in body:
        sets.append("description = ?")
        args.append(str(body["description"]))
    if not sets:
        raise ApiError("没有可更新的字段", 400)

    sets.append("updated_at = ?")
    args.append(datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
    args.append(sid)
    conn.execute(f"UPDATE strategy_config SET {','.join(sets)}"
                 f" WHERE strategy_id = ?", args)
    conn.commit()
    auth.audit(conn, user["username"], "strategy_update", sid,
               json.dumps(body, ensure_ascii=False)[:500])
    return 200, {"ok": True, "strategy": emsdb.query_one(
        conn, "SELECT * FROM strategy_config WHERE strategy_id = ?", (sid,))}


# ---------------------------------------------------------------------
# 运行模式（独立顶层业务概念）
# ---------------------------------------------------------------------
def _mode_row(row: dict) -> dict:
    """把 run_mode 行转成 API 友好结构（JSON 列反序列化）。"""
    out = dict(row)
    for key in ("constraints", "bound_strategies"):
        try:
            out[key] = json.loads(out.get(key) or "[]")
        except (TypeError, ValueError):
            out[key] = []
    out["applicable_scenes"] = [
        s.strip() for s in (out.get("applicable_scene") or "normal").split(",") if s.strip()
    ]
    out.pop("applicable_scene", None)
    return out


def h_modes(conn, query, body, user, params) -> tuple[int, dict]:
    """列出全部运行模式 + 当前激活模式。"""
    rows = emsdb.query(conn, "SELECT * FROM run_mode ORDER BY is_default DESC, mode_id")
    modes = [_mode_row(r) for r in rows]
    active = next((m for m in modes if m.get("active")), None)
    return 200, {"count": len(modes), "items": modes,
                 "active": (active or {}).get("mode_id")}


def h_mode_activate(conn, query, body, user, params) -> tuple[int, dict]:
    """切换运行模式：置 active，并联动启停绑定的策略。

    规则：
      * 安全底座策略（mode_id = NULL，即 S01/S02/S03/S06）**不受影响**，恒为启用。
      * 目标模式绑定的策略 → 启用；其余带 mode_id 的策略 → 停用。
      * 只切换「有归属」的策略，不碰 NULL 归属的安全底座，保证任何模式下安全边界都在。
    """
    mid = str(body.get("mode_id", "")).strip()
    cur = emsdb.query_one(conn, "SELECT * FROM run_mode WHERE mode_id = ?", (mid,))
    if not cur:
        raise ApiError(f"运行模式不存在: {mid}", 404)

    conn.execute("UPDATE run_mode SET active = 0")
    conn.execute("UPDATE run_mode SET active = 1 WHERE mode_id = ?", (mid,))

    try:
        bound = json.loads(cur["bound_strategies"] or "[]")
    except (TypeError, ValueError):
        bound = []
    bound = [str(s) for s in bound]

    # 先停掉所有「有归属」的策略（mode_id 非 NULL），再启用目标模式的绑定策略
    conn.execute("UPDATE strategy_config SET enabled = 0 WHERE mode_id IS NOT NULL")
    if bound:
        marks = ",".join("?" for _ in bound)
        conn.execute(f"UPDATE strategy_config SET enabled = 1"
                     f" WHERE strategy_id IN ({marks})", bound)
    conn.commit()

    auth.audit(conn, user["username"], "mode_activate", mid,
               f"联动启停策略 {bound or '（无）'}")
    modes = [_mode_row(r) for r in emsdb.query(
        conn, "SELECT * FROM run_mode ORDER BY is_default DESC, mode_id")]
    return 200, {"ok": True, "active": mid, "items": modes,
                 "enabled_strategies": [r["strategy_id"] for r in emsdb.query(
                     conn, "SELECT strategy_id FROM strategy_config"
                           " WHERE enabled = 1 ORDER BY priority")]}


# ---------------------------------------------------------------------
# 收益 / 能量 / 报表
# ---------------------------------------------------------------------
def h_economics(conn, query, body, user, params) -> tuple[int, dict]:
    rows = emsdb.query(conn, "SELECT * FROM economics")
    return 200, {"items": rows}


def h_energy(conn, query, body, user, params) -> tuple[int, dict]:
    rows = emsdb.query(conn, "SELECT * FROM energy_statistics"
                             " ORDER BY date DESC, scenario_id")
    return 200, {"items": rows}


def h_report(conn, query, body, user, params) -> tuple[int, dict]:
    """报表系统：日/周/月聚合。

    本项目目前只有仿真日数据，所以 period 只是改变聚合视角，
    不改变数据本身 —— 这一点在返回里显式标出，避免误读。
    """
    period = str(q1(query, "period", "day") or "day")
    if period not in ("day", "week", "month"):
        raise ApiError("period 只能是 day / week / month", 400)
    sc = resolve_scenario(conn, query)

    daily = emsdb.query(
        conn, "SELECT date, scenario_id, charge_kwh, discharge_kwh, pv_kwh,"
              " load_kwh, grid_kwh, export_kwh, peak_grid_kw, net_benefit_cny"
              " FROM energy_statistics WHERE scenario_id = ? ORDER BY date",
        (sc,))
    agg = emsdb.query_one(
        conn, "SELECT COUNT(*) AS days, SUM(charge_kwh) AS charge_kwh,"
              " SUM(discharge_kwh) AS discharge_kwh, SUM(pv_kwh) AS pv_kwh,"
              " SUM(load_kwh) AS load_kwh, SUM(export_kwh) AS export_kwh,"
              " MAX(peak_grid_kw) AS peak_grid_kw,"
              " SUM(net_benefit_cny) AS net_benefit_cny"
              " FROM energy_statistics WHERE scenario_id = ?", (sc,)) or {}
    econ = emsdb.query_one(conn, "SELECT * FROM economics WHERE scenario_id = ?",
                           (sc,)) or {}
    inv = emsdb.query_one(conn, "SELECT * FROM invariant WHERE scenario_id = ?",
                          (sc,)) or {}
    alarm_by_level = emsdb.query(
        conn, "SELECT level, COUNT(*) AS n FROM alarm WHERE scenario_id = ?"
              " GROUP BY level", (sc,))
    cmd_by_result = emsdb.query(
        conn, "SELECT result, COUNT(*) AS n FROM control_command"
              " WHERE scenario_id = ? GROUP BY result", (sc,))

    return 200, {
        "period": period, "scenario": sc, "days": agg.get("days") or 0,
        "data_note": "当前库内只有仿真日数据，周/月视角尚未积累真实运行日",
        "daily": daily, "aggregate": agg,
        "economics": econ, "invariants": inv,
        "alarms_by_level": alarm_by_level,
        "commands_by_result": cmd_by_result,
    }


def h_invariants(conn, query, body, user, params) -> tuple[int, dict]:
    rows = emsdb.query(conn, "SELECT * FROM invariant")
    return 200, {"items": rows,
                 "note": "全 0 才算过；all_ok=1 表示硬不变量与安全口径同时成立"}


# ---------------------------------------------------------------------
# 指令下发（模拟执行）
# ---------------------------------------------------------------------
def h_command_send(conn, query, body, user, params) -> tuple[int, dict]:
    """下发指令。

    现场没有真实执行器时，平台**不假装成功**：
    只把意图记进审计，并返回 executor 状态。
    """
    target = r3(body.get("target_power_kw"))
    if target is None:
        raise ApiError("缺少 target_power_kw", 400)
    device_id = str(body.get("device_id", "DEV-PCS-01"))
    sc = str(body.get("scenario", DEFAULT_SCENARIO))
    t = qf(body, "t_s", -1.0)

    # 与当前权限区间对照，给出「这台设备能不能动」的判断
    row = None
    if t >= 0:
        row = emsdb.query_one(
            conn, "SELECT * FROM step_record WHERE scenario_id = ? AND t_s <= ?"
                  " ORDER BY t_s DESC LIMIT 1", (sc, t))
    lo = r3(row["p_lower_kw"]) if row else None
    hi = r3(row["p_upper_kw"]) if row else None
    verdict = "unknown"
    if lo is not None and hi is not None:
        verdict = "ok" if lo <= target <= hi else "out_of_permission"
    if row and row["state_gated"]:
        verdict = "gated"

    auth.audit(conn, user["username"], "command_send", device_id,
               f"target={target} kW, verdict={verdict} (边界 {lo}..{hi})")
    return 200, {
        "accepted": False,
        "executor": "none",
        "reason": "现场尚未接入真实执行器（无真机）；本次只记录意图并审计",
        "target_power_kw": target,
        "device_id": device_id,
        "permission_range": {"lower_kw": lo, "upper_kw": hi},
        "verdict": verdict,
    }


# ---------------------------------------------------------------------
# 审计 / 用户
# ---------------------------------------------------------------------
def h_audit(conn, query, body, user, params) -> tuple[int, dict]:
    limit = max(1, min(1000, qi(query, "limit", 100)))
    return 200, {"items": auth.list_audit(conn, limit)}


def h_users(conn, query, body, user, params) -> tuple[int, dict]:
    return 200, {"items": auth.list_users(conn)}


def h_user_create(conn, query, body, user, params) -> tuple[int, dict]:
    try:
        uid = auth.create_user(conn, str(body.get("username", "")).strip(),
                               str(body.get("password", "")),
                               str(body.get("role", auth.ROLE_VIEWER)),
                               str(body.get("display_name", "")))
    except ValueError as e:
        raise ApiError(str(e), 400)
    except Exception as e:  # UNIQUE 冲突等
        raise ApiError(f"创建失败: {e}", 400)
    auth.audit(conn, user["username"], "user_create",
               body.get("username", ""), f"role={body.get('role')}")
    return 200, {"ok": True, "user_id": uid}


# ---------------------------------------------------------------------
# 导出
# ---------------------------------------------------------------------
def h_export(conn, query, body, user, params) -> tuple[int, dict]:
    """导出为 CSV 文本（由 server 层写响应体，这里返回特殊标记）。"""
    kind = params["kind"]
    sc = resolve_scenario(conn, query)
    mapping = {
        "timeseries": ("SELECT * FROM step_record WHERE scenario_id = ?"
                       " ORDER BY t_s", "timeseries"),
        "alarms": ("SELECT * FROM alarm WHERE scenario_id = ? ORDER BY ts",
                   "alarms"),
        "commands": ("SELECT * FROM control_command WHERE scenario_id = ?"
                     " ORDER BY ts", "commands"),
        "devices": ("SELECT * FROM device ORDER BY device_id", "devices"),
    }
    if kind not in mapping:
        raise ApiError(f"不支持的导出类型: {kind}，可选 "
                       f"{','.join(mapping)}", 400)
    sql, _ = mapping[kind]
    args = () if kind == "devices" else (sc,)
    rows = emsdb.query(conn, sql, args)
    buf = io.StringIO()
    if rows:
        w = csv.DictWriter(buf, fieldnames=list(rows[0].keys()),
                           lineterminator="\n")
        w.writeheader()
        w.writerows(rows)
    auth.audit(conn, user["username"], "export", kind, f"{len(rows)} 行")
    return 200, {"__raw__": True, "content_type": "text/csv; charset=utf-8",
                 # ★ 带 BOM：alarm.description / scenario.name 是中文，
                 #   现场用 Excel 双击打开才不乱码（与 :1104 点表导出同一条纪律）。
                 "body": "\ufeff" + buf.getvalue(),
                 "filename": f"{kind}_{sc}.csv"}


# =====================================================================
# 设备接入（v1.2）—— 配置通道①：约定文件路径
#
# 这一段把「客户在界面上填点表」接到「13/ 现场进程能读到」：
#     15/ 表单 ──PUT──▶ 14/ 写 device_conn + device_point_map
#                       └─▶ 原子写 config/point-map/<id>.csv 与 active.csv
#                                    └─▶ 13/ 启动时读 active.csv
#
# 两条硬纪律：
#   1. **校验在服务端**。前端能绕过，服务端不能 —— 而一张错的点表在运行期
#      表现得像个正常数字，是最难查的一类故障。
#   2. **落盘与写库同生共死**。半成功会产生「界面说保存成功、现场还是旧表」，
#      这种不一致比直接报错难查得多，所以宁可整体回滚。
# =====================================================================

# Modbus 串行链路从站地址 1..247（0 是广播地址，不能作为正常从站）
UNIT_ID_MIN, UNIT_ID_MAX = 1, 247
PORT_MIN, PORT_MAX = 1, 65535
POLL_MIN_MS = 10
# 主机名 / IPv4 的白名单。刻意不用"校验 IPv4"的强规则：现场会写设备主机名。
_RE_HOST = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._-]{0,62}$")


def _device_or_404(conn, device_id: str) -> dict:
    d = emsdb.query_one(conn, "SELECT * FROM device WHERE device_id = ?", (device_id,))
    if not d:
        raise ApiError(f"设备不存在: {device_id}", 404)
    return d


def _conn_row(conn, device_id: str) -> dict:
    """取接入参数；没有就返回一份默认值（不写库）。"""
    r = emsdb.query_one(conn, "SELECT * FROM device_conn WHERE device_id = ?", (device_id,))
    if r:
        return r
    return {
        "device_id": device_id, "protocol": "modbus_tcp",
        # 没有接入记录 ⇒ 形态按「真实源但还没填地址」算（enabled=0），
        # 这样界面上显示的是「未配置」，而不是一个凭空的「模拟源」。
        "source_kind": "real",
        "host": None, "port": 502, "unit_id": 1,
        "sim_host": connconf.SIM_HOST_DEFAULT,
        "sim_port": connconf.SIM_PORT_DEFAULT,
        "poll_period_ms": 100, "timeout_ms": 1000,
        "auto_reconnect": 1, "enabled": 0,
        "point_map_path": None, "point_map_rows": 0,
        "point_map_saved_at": None, "updated_at": None, "note": None,
    }


def _pm_rows(conn, device_id: str) -> tuple[list[dict], str]:
    """取点表行。库里没有则返回内置默认表基线，并标明来源。

    返回 (rows, source)。source ∈ {"db", "baseline"}。
    基线是只读的默认值 —— GET 不写库（读接口有副作用是最坏的接口设计）。
    """
    db_rows = emsdb.query(conn, "SELECT * FROM device_point_map"
                                " WHERE device_id = ? ORDER BY idx", (device_id,))
    if db_rows:
        return pointmap.from_db_rows(db_rows), "db"
    return pointmap.normalize(pointmap.read_baseline()), "baseline"


def _pm_summary(rows: list[dict]) -> dict:
    return pointmap.summary(rows)


# ---------------------------------------------------------------------
# 接入参数落盘（配置通道②）—— 文件与库同生共死
#
# 为什么需要：点表有 active.csv 给端侧读，接入参数此前只在库里 ——
# 于是 13/ 07/ 仍必须靠命令行 --host 才连得上，界面上写的 IP 与现场敲的
# 可能根本不是一个，而两边都不报错。
#
# 两条纪律与点表保存完全一致：
#   1. 落盘与写库同生共死（半成功 = 「界面说配好了、端侧连的还是老地址」）；
#   2. host 被清空时**删掉旧文件**，不让端侧拿一个已作废的地址去连。
# ---------------------------------------------------------------------
def _file_snapshot(paths) -> dict:
    return {p: (open(p, "rb").read() if os.path.isfile(p) else None) for p in paths}


def _file_restore(prev: dict) -> None:
    """把文件退回去，保持「库 + 文件」两侧一致。

    回滚失败**必须留下痕迹**：七处调用点都会在失败时告诉用户「已整体回滚」，
    如果这里只回滚了一半却静默吞掉 OSError，那句话就成了假话 ——
    文件是新的、库是旧的，是**最难查**的一种不一致（两边看起来都"成功过"）。
    调用方继续按"回滚成功了"往下走，所以这里只能记录，不能改返回值打破它们。
    """
    for p, data in prev.items():
        try:
            if data is None:
                if os.path.isfile(p):
                    os.unlink(p)
            else:
                pointmap._atomic_write(p, data)
        except OSError as e:
            engine.note_critical("api.rollback",
                                 f"回滚失败 文件={p}：{type(e).__name__}: {e}")


def _sync_conn_file(device_id: str, c: dict, now: str, username: str) -> dict:
    """把当前接入参数落盘给端侧。返回落盘结果（供接口回显）。

    host 为空时删除旧文件：客户把地址清掉就是「这台先不接」，
    留着一个旧地址会让端侧下一拍去连一台已经换掉的设备。
    """
    path = connconf.conn_path()
    host = str(c.get("host") or "").strip()
    # ★ 删除判据只对「真实源」成立（v1.5）：接模拟源时 host 本来就可以是空的
    #   —— 调试期常见「模拟器已经能跑、真机还没到货」。此时文件必须照写，
    #   否则端侧读不到文件、回落默认表去连一个不存在的设备。
    if not host and str(c.get("source_kind") or "real") != "sim":
        if os.path.isfile(path):
            os.unlink(path)
        return {"path": path, "bytes": 0, "removed": True}

    row = dict(c)
    row.update({"device_id": device_id, "saved_at": now, "username": username})
    saved = connconf.save(row)
    saved["removed"] = False
    return saved


def h_point_map(conn, query, body, user, params) -> tuple[int, dict]:
    """读某设备的点表（含预检统计与落盘状态）。"""
    device_id = params["device_id"]
    _device_or_404(conn, device_id)
    rows, source = _pm_rows(conn, device_id)
    c = _conn_row(conn, device_id)

    target = pointmap.device_path(device_id)
    act = pointmap.active_path()
    synced = False
    if source == "db" and os.path.isfile(act):
        try:
            disk = pointmap.normalize(pointmap.load_file(act))
            synced = disk == rows
        except pointmap.PointMapError:
            synced = False

    # 接入参数文件（配置通道②）的落盘状态：界面据此显示「已下发到端侧」
    cpath = connconf.conn_path()
    cfile_exists = os.path.isfile(cpath)
    conn_synced = False
    if cfile_exists:
        try:
            on_disk = connconf.load_file(cpath)
            conn_synced = (
                on_disk["host"] == (c.get("host") or "")
                and on_disk["port"] == (c.get("port") or 502)
                and on_disk["unit_id"] == (c.get("unit_id") or 1)
            )
        except connconf.ConnConfError:
            conn_synced = False

    return 200, {
        "device_id": device_id,
        "configured": source == "db",
        "source": source,
        "columns": pointmap.COLS,
        "rows": rows,
        "summary": _pm_summary(rows),
        "conn": c,
        "path": target,
        "active_path": act,
        "file_exists": os.path.isfile(target),
        "synced": synced,
        "conn_path": cpath,
        "conn_file_exists": cfile_exists,
        "conn_synced": conn_synced,
    }


def h_point_map_put(conn, query, body, user, params) -> tuple[int, dict]:
    """保存点表：先校验 → 落盘 → 写库。任何一步失败都整体回滚。"""
    device_id = params["device_id"]
    _device_or_404(conn, device_id)

    raw_rows = body.get("rows")
    if raw_rows is None and isinstance(body.get("csv"), str):
        raw_rows = pointmap.parse_text(body["csv"])
    if not isinstance(raw_rows, list) or not raw_rows:
        raise ApiError("请求体需要 rows（行数组）或 csv（CSV 文本）", 400)

    try:
        rows = pointmap.normalize(raw_rows)
        pointmap.validate(rows)              # ← 与 13/ 的 validate_map() 同规则
    except pointmap.PointMapError as e:
        raise ApiError(e.message, e.status)

    c = _conn_row(conn, device_id)
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    meta = {
        "device_id": device_id,
        "host": c.get("host") or "-", "port": c.get("port") or "-",
        "unit_id": c.get("unit_id") or "-",
        "saved_at": now, "username": user["username"] if user else "-",
    }

    # 先把旧文件读进内存：出任何问题都能原样退回去
    target = pointmap.device_path(device_id)
    act = pointmap.active_path()
    prev = _file_snapshot((target, act, connconf.conn_path()))

    saved = None
    cp = None
    try:
        saved = pointmap.save(device_id, rows, meta)

        conn.execute("DELETE FROM device_point_map WHERE device_id = ?", (device_id,))
        emsdb.executemany(conn, pointmap.DB_INSERT_SQL,
                          pointmap.to_db_rows(device_id, rows))
        conn.execute(
            "INSERT INTO device_conn(device_id, protocol, host, port, unit_id,"
            " poll_period_ms, timeout_ms, auto_reconnect, enabled, point_map_path,"
            " point_map_rows, point_map_saved_at, updated_at)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)"
            " ON CONFLICT(device_id) DO UPDATE SET"
            "   point_map_path = excluded.point_map_path,"
            "   point_map_rows = excluded.point_map_rows,"
            "   point_map_saved_at = excluded.point_map_saved_at,"
            "   updated_at = excluded.updated_at",
            (device_id, c.get("protocol") or "modbus_tcp",
             c.get("host"), c.get("port") or 502, c.get("unit_id") or 1,
             c.get("poll_period_ms") or 100, c.get("timeout_ms") or 1000,
             int(c.get("auto_reconnect") or 0), int(c.get("enabled") or 0),
             saved["path"], len(rows), now, now))
        # 接入参数也一并落盘（配置通道②）：点表与接入参数必须来自同一次保存，
        # 否则会出现「新 IP 连旧点表」这种两侧不一致。
        cp = _sync_conn_file(device_id, c, now, user["username"] if user else "-")
        conn.commit()
    except Exception as e:
        conn.rollback()
        _file_restore(prev)                   # 文件也退回去，保持两侧一致
        if isinstance(e, ApiError):
            raise
        raise ApiError(f"保存失败，已整体回滚：{e}", 500)

    auth.audit(conn, user["username"], "point_map_save", device_id,
               f"{len(rows)} 行 → {saved['path']}")
    return 200, {"ok": True, "saved": saved, "conn_file": cp,
                 "summary": _pm_summary(rows),
                 "rows": len(rows)}


def h_point_map_csv(conn, query, body, user, params) -> tuple[int, dict]:
    """导出点表 CSV（当前表，或 ?template=1 取内置默认表当模板）。"""
    device_id = params["device_id"]
    _device_or_404(conn, device_id)

    if str(q1(query, "template", "") or "") in ("1", "true", "yes"):
        rows = pointmap.normalize(pointmap.read_baseline())
        hint = "内置默认表（模板）"
    else:
        rows, src = _pm_rows(conn, device_id)
        hint = "库内已配置点表" if src == "db" else "内置默认表（该设备尚未配置）"

    c = _conn_row(conn, device_id)
    meta = {
        "device_id": device_id,
        "host": c.get("host") or "-", "port": c.get("port") or "-",
        "unit_id": c.get("unit_id") or "-",
        "saved_at": datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "username": (user["username"] if user else "-") + f"（导出：{hint}）",
    }
    text = pointmap.to_csv_text(rows, meta)
    auth.audit(conn, user["username"], "point_map_export", device_id, hint)
    return 200, {
        "__raw__": True,
        "content_type": "text/csv; charset=utf-8",
        # ★ 带 BOM：现场用 Excel 双击打开，中文才不乱码
        "body": "\ufeff" + text,
        "filename": f"point-map_{device_id}.csv",
    }


def h_conn_put(conn, query, body, user, params) -> tuple[int, dict]:
    """保存接入参数（IP / 端口 / 从站号 / 轮询节奏）。"""
    device_id = params["device_id"]
    _device_or_404(conn, device_id)

    def _int(key, lo, hi, cur, label):
        if key not in body:
            return cur
        try:
            v = int(body[key])
        except (TypeError, ValueError):
            raise ApiError(f"{label} 应为整数，实际是 {body[key]!r}", 400)
        if not (lo <= v <= hi):
            raise ApiError(f"{label} 应在 {lo}..{hi}，实际是 {v}", 400)
        return v

    cur = _conn_row(conn, device_id)
    host = cur.get("host")
    if "host" in body:
        h = str(body["host"] or "").strip()
        if h and not _RE_HOST.match(h):
            raise ApiError(
                "设备地址应是 IPv4 或主机名（只允许字母数字与 . _ -），实际是 "
                f"「{h}」", 400)
        host = h or None

    # ---- 对端形态（v1.5）：real = 现场设备，sim = 模拟源 ----
    kind = str(cur.get("source_kind") or "real").strip().lower()
    if "source_kind" in body:
        k2 = str(body["source_kind"] or "").strip().lower()
        if k2 not in connconf.SOURCE_KINDS:
            raise ApiError(
                f"对端形态应为 {'/'.join(connconf.SOURCE_KINDS)}，实际是「{k2}」", 400)
        kind = k2
    sim_host = str(cur.get("sim_host") or connconf.SIM_HOST_DEFAULT).strip()
    if "sim_host" in body:
        s2 = str(body["sim_host"] or "").strip() or connconf.SIM_HOST_DEFAULT
        if not _RE_HOST.match(s2):
            raise ApiError(
                f"模拟源地址应是 IPv4 或主机名（只允许字母数字与 . _ -），实际是「{s2}」", 400)
        sim_host = s2
    sim_port = _int("sim_port", PORT_MIN, PORT_MAX,
                    cur.get("sim_port") or connconf.SIM_PORT_DEFAULT, "模拟源端口")
    # ★ 真实源 + 空地址**是合法状态**（「这台先不接」/ 已登记未接线），
    #   不能用 400 堵死 —— 那会让现场连"先登记、后接线"都做不到。
    #   它在界面上表现为「未配置」，端侧则因为落盘文件被删除而回落到默认表。
    #   真正的守卫在端侧：文件存在且 kind=real 却缺 host 时，13/ 的 loader 硬失败。
    # ★ 反过来，切到模拟源时**不替客户清空** host：现场常是"短暂接模拟器联调，
    #   之后切回真机"，清掉等于逼人重填。

    port = _int("port", PORT_MIN, PORT_MAX, cur.get("port") or 502, "端口")
    unit = _int("unit_id", UNIT_ID_MIN, UNIT_ID_MAX, cur.get("unit_id") or 1, "从站号")
    poll = _int("poll_period_ms", POLL_MIN_MS, 60000,
                cur.get("poll_period_ms") or 100, "轮询周期(ms)")
    tmo = _int("timeout_ms", 10, 60000, cur.get("timeout_ms") or 1000, "超时(ms)")
    en = cur.get("enabled") or 0
    if "enabled" in body:
        en = 1 if body["enabled"] else 0
    if (host or kind == "sim") and "enabled" not in body:
        en = 1                              # 有可用对端即视为启用（现场直觉）

    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    # 落盘也要跟库一起成败：先备好旧文件，任何一步失败都整体退回
    prev = _file_snapshot((connconf.conn_path(),))
    cp = None
    try:
        conn.execute(
            "INSERT INTO device_conn(device_id, protocol, host, port, unit_id,"
            " poll_period_ms, timeout_ms, auto_reconnect, enabled,"
            " point_map_path, point_map_rows, point_map_saved_at, updated_at, note,"
            " source_kind, sim_host, sim_port)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
            " ON CONFLICT(device_id) DO UPDATE SET"
            "   host = excluded.host, port = excluded.port, unit_id = excluded.unit_id,"
            "   poll_period_ms = excluded.poll_period_ms, timeout_ms = excluded.timeout_ms,"
            "   enabled = excluded.enabled, updated_at = excluded.updated_at,"
            "   source_kind = excluded.source_kind, sim_host = excluded.sim_host,"
            "   sim_port = excluded.sim_port",
            (device_id, cur.get("protocol") or "modbus_tcp", host, port, unit, poll, tmo,
             int(cur.get("auto_reconnect") or 0), en,
             cur.get("point_map_path"), cur.get("point_map_rows") or 0,
             cur.get("point_map_saved_at"), now,
             str(body.get("note") or cur.get("note") or "") or None,
             kind, sim_host, sim_port))
        # 配置通道②：接入参数落盘给 13/ 07/ 读（host 清空则删旧文件）
        cp = _sync_conn_file(
            device_id,
            dict(cur, host=host, port=port, unit_id=unit, poll_period_ms=poll,
                 timeout_ms=tmo, enabled=en, source_kind=kind,
                 sim_host=sim_host, sim_port=sim_port),
            now, user["username"] if user else "-")
        conn.commit()
    except Exception as e:
        conn.rollback()
        _file_restore(prev)
        if isinstance(e, ApiError):
            raise
        raise ApiError(f"接入参数保存失败，已整体回滚：{e}", 500)

    row = _conn_row(conn, device_id)
    try:
        tgt = connconf.target(row)
    except connconf.ConnConfError:
        tgt = None
    auth.audit(conn, user["username"], "device_conn_save", device_id,
               f"{kind} {(sim_host + ':' + str(sim_port)) if kind == 'sim' else (host or '') + ':' + str(port)}"
               f" unit={unit} poll={poll}ms enabled={en}")
    return 200, {"ok": True, "conn": row, "conn_file": cp, "target": tgt}


# =====================================================================
# 接入装配（v1.5）—— 上端（调度侧）+ 下端（设备侧）
#
# 为什么要有这一节：
#   v1.2 只解决了下端（EMS 作 Modbus 主站连设备）。上端的 IEC104 从站参数
#   （监听口 / 公共地址 / 谁连进来）一直散落在 P3 的命令行里，界面上没有任何
#   地方能看见「这台 EMS 到底往哪个调度送数」。于是两种现场都遇到过：
#     · 调度说"收不到你的数"，平台上一切正常 —— 它压根不知道上端配的是什么；
#     · 调试期拿 iec104_master 当模拟调度联调完，切真调度时忘了改回来，
#       而**没有任何地方记录过**"这一端当时接的是模拟源"。
#   所以这一版把两端的装配都摆到界面上，并且明确记下每一端接的是真实源
#   还是模拟源 —— 记录本身就是价值：它让"我以为是真机"变成一件查得到的事。
# =====================================================================

def _link_row(conn) -> dict:
    """取上端链路；没有就返回默认值（**不写库** —— 读接口没有副作用）。"""
    r = emsdb.query_one(conn, "SELECT * FROM upstream_link WHERE link_id = ?",
                        (uplinkconf.LINK_ID,))
    if r:
        return r
    return {
        "link_id": uplinkconf.LINK_ID, "kind": "real", "protocol": "iec104",
        "bind_host": uplinkconf.BIND_DEFAULT,
        "listen_port": uplinkconf.PORT_DEFAULT,
        "common_addr": 1, "master_host": None, "enabled": 1,
        "updated_at": None, "username": None, "note": None,
    }


def h_access(conn, query, body, user, params) -> tuple[int, dict]:
    """GET /api/access —— 两端接入装配总览（界面「设备管理」页的数据源）。"""
    up = _link_row(conn)
    up_file = uplinkconf.link_path()
    up_sum = uplinkconf.summary(up)

    # ★ 必须以 device 为左表：只遍历 device_conn 会漏掉「台账里有、接入参数
    #   一行都没有」的设备（现场最常见的初始状态），汇总数就少于台账台数，
    #   而界面上看不出少了谁。没配过的自然没有 source_kind，落进 unset。
    rows = emsdb.query(
        conn, "SELECT d.device_id AS device_id, c.source_kind, c.host, c.port,"
              " c.unit_id, c.sim_host, c.sim_port, c.enabled,"
              " c.poll_period_ms, c.timeout_ms"
              " FROM device d LEFT JOIN device_conn c ON c.device_id = d.device_id"
              " ORDER BY d.device_id")
    # 每台的点表行数（第二份输入）。★ 用 COUNT 现场算，不信
    #   device_conn.point_map_rows 那个缓存列 —— 缓存列一旦和真表分叉，
    #   界面就会说"已配 32 点"，而库里其实一行都没有。
    pm_rows = {r["device_id"]: int(r["n"] or 0) for r in emsdb.query(
        conn, "SELECT device_id, COUNT(*) AS n FROM device_point_map"
              " GROUP BY device_id")}
    items: list[dict] = []
    counts = {"real": 0, "sim": 0, "unset": 0, "off": 0}
    for r in rows:
        kind = str(r.get("source_kind") or "real")
        # 「配过」的判据：真实源要有地址、模拟源要有模拟器地址。光有一行记录
        # 但地址全空，等于没配 —— 不能让界面把它显示成"已接入"。
        #
        # ★ 「没配过」与「配好了但停用」**不能塌成同一个状态**：
        #   enabled=0 早先被直接折叠进 unset，于是启用模拟源之后这一行是
        #   「形态=未配置 / 目标=127.0.0.1:1702」—— 两列自相矛盾，而用户
        #   正用这两列判断"到底填好了没有"（2026-10-18 实测到）。
        #   塌掉两种状态省下一个分支，换来的是一句无法解释的界面文案。
        if kind == "real" and not str(r.get("host") or "").strip():
            state = "unset"
        elif kind == "sim" and not str(r.get("sim_host") or "").strip():
            state = "unset"
        elif not r.get("enabled"):
            state = "off"
        else:
            state = kind
        counts[state] = counts.get(state, 0) + 1
        try:
            tgt = connconf.target(r)
        except connconf.ConnConfError as e:
            tgt = {"error": e.message}
        items.append({"device_id": r["device_id"], "source_kind": kind,
                      "state": state, "target": tgt,
                      "host": r.get("host"), "port": r.get("port"),
                      "sim_host": r.get("sim_host"), "sim_port": r.get("sim_port"),
                      "unit_id": r.get("unit_id"),
                      "poll_period_ms": r.get("poll_period_ms"),
                      "timeout_ms": r.get("timeout_ms"),
                      "point_rows": pm_rows.get(r["device_id"], 0)})

    cp = connconf.conn_path()
    return 200, {
        "ok": True,
        "schema_version": emsdb.SCHEMA_VERSION,
        "dir": pointmap.map_dir(),
        "upstream": {
            "link": up, "summary": up_sum,
            "file": up_file, "file_exists": os.path.isfile(up_file),
            "reader": "P3/build/iec104_gateway.exe",
        },
        "downstream": {
            "counts": counts, "total": len(items), "items": items,
            "file": cp, "file_exists": os.path.isfile(cp),
            "reader": "13/build/modbus_probe.exe · 07/build/main_field.exe --device modbus",
        },
    }


def h_access_upstream_put(conn, query, body, user, params) -> tuple[int, dict]:
    """PUT /api/access/upstream —— 保存上端链路参数，并落盘给 P3/ 读。"""
    cur = _link_row(conn)
    merged = dict(cur)
    for k in ("kind", "protocol", "bind_host", "listen_port",
              "common_addr", "master_host", "enabled", "note"):
        if k in body:
            merged[k] = body[k]
    try:
        n = uplinkconf.normalize(merged)
    except connconf.ConnConfError as e:
        raise ApiError(e.message, getattr(e, "status", 400) or 400)

    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    who = user["username"] if user else "-"
    # 落盘与写库同生共死：半成功会产生「界面说配好了、现场网关还在听旧端口」。
    prev = _file_snapshot((uplinkconf.link_path(),))
    try:
        conn.execute(
            "INSERT INTO upstream_link(link_id, kind, protocol, bind_host,"
            " listen_port, common_addr, master_host, enabled, updated_at,"
            " username, note) VALUES(?,?,?,?,?,?,?,?,?,?,?)"
            " ON CONFLICT(link_id) DO UPDATE SET"
            "   kind = excluded.kind, protocol = excluded.protocol,"
            "   bind_host = excluded.bind_host, listen_port = excluded.listen_port,"
            "   common_addr = excluded.common_addr, master_host = excluded.master_host,"
            "   enabled = excluded.enabled, updated_at = excluded.updated_at,"
            "   username = excluded.username, note = excluded.note",
            (n["link_id"], n["kind"], n["protocol"], n["bind_host"],
             n["listen_port"], n["common_addr"], n["master_host"], n["enabled"],
             now, who, str(body.get("note") or cur.get("note") or "") or None))
        saved = uplinkconf.save(dict(n, saved_at=now, username=who))
        conn.commit()
    except Exception as e:
        conn.rollback()
        _file_restore(prev)
        if isinstance(e, ApiError):
            raise
        raise ApiError(f"上端链路保存失败，已整体回滚：{e}", 500)

    auth.audit(conn, user["username"], "upstream_link_save", n["link_id"],
               f"{n['kind']} {n['bind_host']}:{n['listen_port']} ca={n['common_addr']}")
    row = _link_row(conn)
    return 200, {"ok": True, "upstream": {
        "link": row, "summary": uplinkconf.summary(row),
        "file": saved["path"], "file_exists": True}}


# =====================================================================
# 输入端（v1.7）：**两端各一个开关**，不是一个整站开关
#
# 为什么拆成两个：
#   v1.6 把它收敛成一个整站选择，代价是「设备已到货接真机、调度链路还没开通」
#   这种最常见的调试态表达不了 —— 一端要真、一端要假，只能整体妥协。
#   而调度主站接入（EMS 作从站）与站内设备接入（EMS 作主站）本来就是**两条
#   独立通道**，对端各接各的。一个开关写两端，等于逼着人把两件不相干的事
#   一起改，改完屏幕上只多出几个小标签。
#
# 每一端内部再分「真实输入源 / 模拟输入源」：
#   real = 对端是现场的（上级调度主站 / 站内装置）
#   sim  = 对端由本机模拟（模拟调度主站 / 设备替身）
#
# ★ 一次只切一端（end 必填）。**没有「两端一并切」的默认值** ——
#   那一步会静默把整站数据换成替身，而界面看上去一切正常。
# ★ 切换**不自动启停采集**：采集器正在按 active.conn 里的地址读写，
#   换掉它脚下的对端只能先停。
# =====================================================================
ENDS = ("upstream", "downstream")
END_META = {
    "upstream": {
        "name": "调度主站接入", "role": "从站",
        "protocol": "IEC 60870-5-104",
        "peer": {"real": "上级调度主站", "sim": "模拟调度主站"},
        "reader": "P3/build/iec104_gateway.exe",
    },
    "downstream": {
        "name": "站内设备接入", "role": "主站",
        "protocol": "Modbus TCP",
        "peer": {"real": "现场装置", "sim": "设备替身"},
        "reader": "07/build/main_field.exe --device modbus",
    },
}
KIND_NAME = {"real": "真实输入源", "sim": "模拟输入源"}

# 上端第二份输入：IOA 转发表。
# P3/ 的默认表是随包底本（现场照它改）；平台**自己那份**在 upmapconf.map_path()，
# 网关启动时按约定路径读它 —— 没有那份时才用编译进去的默认表。
UPLINK_PM_CSV = os.path.join(pointmap.PROJECT_ROOT, "P3", "data", "point_map_default.csv")
# 这三种是下行（调度 → EMS）；其余一律按上送算。与 P3 的 Kind 枚举一致。
_UPLINK_DOWN_KINDS = {"SET_FLOAT", "CMD_SINGLE", "CMD_DOUBLE"}


def _current_conn_row(conn) -> dict:
    """active.conn 是**单台**文件：选哪台的参数落盘？

    优先「当前文件里那台」——切形态时不该顺手把落盘对象换成另一台设备；
    没有文件时取第一台已启用的，再没有取第一台。
    """
    rows = emsdb.query(conn, "SELECT * FROM device_conn ORDER BY device_id")
    if not rows:
        return {}
    path = connconf.conn_path()
    if os.path.isfile(path):
        try:
            cur = connconf.load_file(path)
            for r in rows:
                if r["device_id"] == cur.get("device_id"):
                    return r
        except connconf.ConnConfError:
            pass
    for r in rows:
        if int(r.get("enabled") or 0):
            return r
    return rows[0]


def _rewrite_conn_for_kind(conn, kind: str, now: str, who: str) -> dict:
    """把 active.conn 按新形态重写一遍（或按规则删掉）。"""
    cp = connconf.conn_path()
    row = _current_conn_row(conn)
    if not row:
        return {"path": cp, "bytes": 0, "removed": not os.path.isfile(cp),
                "skipped": "库里没有任何接入记录"}
    r = dict(row)
    r["source_kind"] = kind
    if kind == "real" and not str(r.get("host") or "").strip():
        if os.path.isfile(cp):
            os.unlink(cp)
        return {"path": cp, "bytes": 0, "removed": True,
                "note": "真实源但该设备没有地址 ⇒ 不落盘（端侧读不到就当「这台先不接」）"}
    n = connconf.normalize(r)
    saved = connconf.save(dict(n, saved_at=now, username=who))
    saved["removed"] = False
    return saved


def _upstream_kind(conn) -> str:
    """上端当前形态。`_link_row()` 在没有登记行时回落 real（= 网关内置默认）。"""
    k = str(_link_row(conn).get("kind") or "real").strip().lower()
    return k if k in engine.DATA_SOURCES else "real"


def _mix_note(down_kind: str, up_kind: str, up_registered: bool) -> dict:
    """两端形态的组合提示。

    ★ 只有一种组合是**危险**的：下端模拟 + 上端真实 —— 上送调度的是替身
      算出来的数，而屏幕上一切正常。其余三种都要说清，但不吓人。
    ★ 「上端还没登记」是另一件事，不能按真实源去吓人：那只是网关会用内置
      默认值，调度链路本身可能还没开通。
    """
    if down_kind == "sim" and not up_registered:
        return {"level": "warn", "text":
                "上端还没有登记链路：网关会用内置默认（真实源 · 2404 · CA 1）。"
                "下端现在是模拟输入源 —— 联调阶段建议把上端也显式设为模拟输入源，"
                "免得哪天网关起来了、把替身数据当现场值上送。"}
    if down_kind == "sim" and up_kind == "real":
        return {"level": "bad", "text":
                "当前组合：下端模拟 → 上端真实。上送上级调度的是设备替身算出来的数，"
                "不是现场值。要联调就两端都设模拟；要把真数据上送，"
                "先把下端接上现场装置。"}
    if down_kind == "real" and up_kind == "sim":
        return {"level": "info", "text":
                "当前组合：下端真实 → 上端模拟。现场装置的数据只送到本机模拟调度，"
                "不会外发 —— 这是接入现场装置后、调度链路开通前的常规调试态。"}
    if down_kind == "sim":
        return {"level": "", "text":
                "两端都是模拟输入源：整条链路在本机闭环，不连任何现场装置与调度主站。"}
    return {"level": "", "text":
            "两端都是真实输入源：现场装置采上来，上送给上级调度。"}


def _data_source_view(conn) -> dict:
    """两端的形态、对端与实际目标 —— 界面「设备管理」页的头两块就画这个。"""
    down_kind = engine.get_data_source(conn)
    up_kind = _upstream_kind(conn)
    live = engine.LIVE.snapshot(conn)

    up = _link_row(conn)
    try:
        up_sum = uplinkconf.summary(up)
    except Exception as e:
        up_sum = {"ok": False, "error": str(e)}

    cp = connconf.conn_path()
    if os.path.isfile(cp):
        try:
            down = connconf.summary(connconf.load_file(cp))
        except Exception as e:
            down = {"ok": False, "error": str(e)}
    else:
        down = {"ok": False, "error": "还没配下端接入参数（active.conn 不存在）"}

    up_path = uplinkconf.link_path()
    up_registered = emsdb.query_one(
        conn, "SELECT link_id FROM upstream_link WHERE link_id = ?",
        (uplinkconf.LINK_ID,)) is not None

    ends = [
        {"id": "upstream",
         "name": END_META["upstream"]["name"],
         "role": END_META["upstream"]["role"],
         "protocol": END_META["upstream"]["protocol"],
         "reader": END_META["upstream"]["reader"],
         "kind": up_kind, "kind_name": KIND_NAME[up_kind],
         "peer": END_META["upstream"]["peer"][up_kind],
         "summary": up_sum,
         "file": up_path, "file_exists": os.path.isfile(up_path),
         "registered": up_registered},
        {"id": "downstream",
         "name": END_META["downstream"]["name"],
         "role": END_META["downstream"]["role"],
         "protocol": END_META["downstream"]["protocol"],
         "reader": END_META["downstream"]["reader"],
         "kind": down_kind, "kind_name": KIND_NAME[down_kind],
         "peer": END_META["downstream"]["peer"][down_kind],
         "summary": down,
         "file": cp, "file_exists": os.path.isfile(cp),
         "registered": True},
    ]

    return {
        "ok": True,
        "ends": ends,
        "kinds": [
            {"id": "real", "name": KIND_NAME["real"],
             "desc": "对端是现场的真实系统：上端为上级调度主站，下端为站内装置"},
            {"id": "sim", "name": KIND_NAME["sim"],
             "desc": "对端由本机模拟：上端为模拟调度主站，下端为设备替身"},
        ],
        "collector": {
            "source": live["source"], "name": live["source_name"],
            "is_model": live["source_is_model"],
            "state": live["state"], "running": live["running"],
            "rows": live["rows"], "error": live["error"],
            "csv": live["csv"], "started_at": live["started_at"],
            "peer_pid": live["peer_pid"], "peer_target": live["peer_target"],
        },
        "upstream": {"link": up, "summary": up_sum,
                     "file": up_path, "file_exists": os.path.isfile(up_path),
                     "kind": up_kind,
                     "reader": END_META["upstream"]["reader"]},
        "downstream": {"summary": down, "file": cp,
                       "file_exists": os.path.isfile(cp),
                       "kind": down_kind, "kind_name": KIND_NAME[down_kind],
                       "reader": END_META["downstream"]["reader"]},
        "mix": _mix_note(down_kind, up_kind, up_registered),
        "note": "其他页面只认「有没有数据」；数据是模拟还是现场，看这两部分。",
    }


def _upstream_pm_store_rows(conn) -> Optional[list]:
    """库里的存档。表不存在（老库还没跑 init_db）时返回 None，不让它拖垮视图。"""
    try:
        db = emsdb.query(conn, "SELECT * FROM upstream_point_map")
    except Exception:
        return None
    if not db:
        return []
    try:
        return upmapconf.from_db_rows(db)
    except Exception:
        return None      # 库存档有过期的坏行：宁可报"不一致"，不要整页挂掉


def _upstream_pm_view(rows, source, path, file_exists, note,
                      saved_at="", username="", bad_lines=None,
                      read_error="", stored=None) -> dict:
    """拼 GET /api/upstream/point-map 的返回体。

    `rows` 一律带 `direction` —— 界面靠它标「上送 / 下行」，不必自己按 IOA
    段去推算（那种推算迟早有一处写法不一致）。
    """
    up_n   = sum(1 for r in rows if r.get("direction") != "down")
    ioas   = [r["ioa"] for r in rows if isinstance(r.get("ioa"), int)]
    return {
        "ok": True,
        "source": source,                    # platform = 已保存；default = 内置默认
        "file": path, "file_exists": file_exists,
        "active_path": upmapconf.map_path(),
        "default_path": UPLINK_PM_CSV,
        "rows": rows,
        "counts": {"up": up_n, "down": len(rows) - up_n, "total": len(rows)},
        "dup_ioa": sorted({x for x in ioas if ioas.count(x) > 1}),
        "bad_lines": (bad_lines or [])[:5],
        "read_error": read_error,
        "saved_at": saved_at, "username": username,
        "stored": stored,                    # 库存档（可能是 None = 表不可用）
        "reader": "P3/build/iec104_gateway.exe",
        "note": note,
    }


def h_upstream_point_map(conn, query, body, user, params) -> tuple[int, dict]:
    """GET /api/upstream/point-map —— 上端第二份输入：**IOA 转发表**。

    为什么这一端要单独有一份表：
        IEC 104 的调度只认两个坐标 —— CA + IOA；而 EMS 内部用业务点名
        （`MEAS.P_GRID` / `CMD.P_BAT`）。两者之间必须有一张显式映射，
        否则「调度要的第 4004 点到底是什么」永远说不清。下端是「寄存器 ↔
        点名」，上端是「IOA ↔ 点名」，两份表方向不同，不能互相顶替。

    ★ 这里返回的是**当前生效**的那份（v1.6 起）：
        platform —— 平台保存的 `active.pointmap.csv`，网关下次启动读它；
        default  —— 没有平台那份时，网关会用编译进去的内置默认表。
      以前这里只显示仓库里的默认底本，于是界面永远看着一份表，而网关其实
      可能用的是另一份 —— 这正是所有配置通道②要消灭的东西。

    存盘相关的两个字段：
        saved_at / username 从文件头里读出来（那边写在注释里给现场人看），
        不必为此单开一张表；stored 是库里的存档，用于发现"两边分叉了"。
    """
    stored = _upstream_pm_store_rows(conn)
    ap = upmapconf.map_path()

    if os.path.isfile(ap):
        rows, saved_at, username, err = [], "", "", ""
        text = ""
        try:
            with open(ap, "r", encoding="utf-8-sig") as f:
                text = f.read()
            rows = upmapconf.parse(text)
            saved_at = upmapconf.saved_at_from_text(text)
            username = upmapconf.username_from_text(text)
        except connconf.ConnConfError as e:
            err = e.message
        except OSError as e:
            err = str(e)
        return 200, _upstream_pm_view(
            rows, "platform", ap, True, saved_at=saved_at, username=username,
            read_error=err, stored=stored,
            note="这一份就是网关下次启动会读的表（配置通道②）。"
                 "要改 IOA 在下面点「编辑」保存 —— 由平台原子写出；"
                 "手工改这个文件会在下次保存时被覆盖。")

    # ---- 没有平台那份：网关会用内置默认表。这里回落到 P3/ 的默认底本展示。----
    path = UPLINK_PM_CSV
    exists = os.path.isfile(path)
    rows = []
    bad_lines = []
    read_error = ""
    if exists:
        try:
            with open(path, "r", encoding="utf-8-sig", newline="") as f:
                for raw_line in f:
                    s_line = raw_line.rstrip("\r\n")
                    if not s_line.strip() or s_line.lstrip().startswith("#"):
                        continue
                    if s_line.replace(" ", "").lower().startswith("ioa,"):
                        continue
                    # note 可含 ASCII 逗号（P3 的 loader 也是"取最后一段"），
                    # 所以先把前 8 段切出来，剩下的整体当 note。
                    parts = s_line.split(",")
                    if len(parts) < 8:
                        bad_lines.append(s_line)
                        continue
                    kind = (parts[1] or "").strip().upper()
                    rows.append({
                        "ioa": int(parts[0]) if parts[0].strip().isdigit() else parts[0].strip(),
                        "kind": kind, "group": (parts[2] or "").strip(),
                        "name": (parts[3] or "").strip(), "src": (parts[4] or "").strip(),
                        "scale": (parts[5] or "").strip(), "offset": (parts[6] or "").strip(),
                        "unit": (parts[7] or "").strip(),
                        "note": ",".join(parts[8:]).strip(),
                        "direction": "down" if kind in _UPLINK_DOWN_KINDS else "up",
                    })
        except OSError as e:
            read_error = f"读转发表失败：{e}"
    else:
        read_error = f"找不到内置默认表：{path}"

    return 200, _upstream_pm_view(
        rows, "default", path, exists, bad_lines=bad_lines, read_error=read_error,
        stored=stored,
        note="当前**没有**平台保存的表，网关启动时用编译进去的内置默认表"
             "（上送 24 / 下行 6）。要按调度下发的转发表改 IOA，就点下面「编辑」"
             "保存一份 —— 那份会原子写到 " + ap + "，网关下次启动优先读它。")


def h_upstream_point_map_put(conn, query, body, user, params) -> tuple[int, dict]:
    """PUT /api/upstream/point-map —— 保存上端 IOA 转发表（operator）。

    body: {"rows": [...]} 或 {"csv": "<9 列 CSV 文本>"}

    顺序是**先校验、再落盘、最后写库**。任一步失败都整体退回（文件有 prev
    快照、库有事务）：让人看到"保存成功"就必须意味着"文件与库说的是同一件
    事"，否则下一次有人来查账时会同时看到两个答案。

    ★ 校验规则（upmapconf.normalize）逐条镜像 P3/ 的 load_point_map_csv +
      self_check_tables —— 平台放行的网关一定能加载，网关会拒的平台先拒。
      "保存成功但网关起不来"是这里最坏的失败：人在 A 处操作，雷在 B 处炸。
    """
    raw_rows = body.get("rows")
    if raw_rows is None and isinstance(body.get("csv"), str):
        try:
            raw_rows = upmapconf.parse(body["csv"])
        except connconf.ConnConfError as e:
            raise ApiError(e.message, e.status)
    if not isinstance(raw_rows, list):
        raise ApiError("请求体需要 rows（行数组）或 csv（CSV 文本）", 400)

    try:
        rows = upmapconf.normalize(raw_rows)      # 校验：这一步不碰任何东西
    except connconf.ConnConfError as e:
        raise ApiError(e.message, e.status)

    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    who = user["username"] if user else "-"
    ap = upmapconf.map_path()
    prev = _file_snapshot((ap,))
    try:
        saved = upmapconf.save(rows, now, who)
        conn.execute("DELETE FROM upstream_point_map")
        emsdb.executemany(conn, upmapconf.DB_INSERT_SQL, upmapconf.to_db_rows(rows))
        conn.commit()
    except Exception as e:
        conn.rollback()
        _file_restore(prev)
        if isinstance(e, ApiError):
            raise
        raise ApiError(f"保存失败，已整体回滚：{e}", 500)

    c = upmapconf.counts_of(rows)
    auth.audit(conn, user["username"], "upstream_point_map_save", "dispatch",
               f"{c['total']} 行（上送 {c['up']} / 下行 {c['down']}）→ {ap}")
    return 200, {"ok": True, "rows": rows, "counts": c,
                 "path": saved["path"], "bytes": saved["bytes"],
                 "saved_at": now, "username": who,
                 "source": "platform",
                 "note": "已原子落盘。网关是**启动时**读它的 —— 已经在跑的那个"
                         "网关不会热更新，要它生效得重启网关（改 IOA 意味着调度"
                         "那边的解码全变了，悄悄换更危险）。"}


def h_upstream_point_map_delete(conn, query, body, user, params) -> tuple[int, dict]:
    """DELETE /api/upstream/point-map —— 恢复内置默认表。

    ★ 文件与库存档**一起清**：留着库存档会让"当前生效=默认表"和"平台里有
      一份存档"同时存在，下次有人点编辑看到的是那个存档，而网关跑的是默认
      表 —— 又是一个看不见的分叉。要留备份请先导出 CSV。
    """
    ap = upmapconf.map_path()
    prev = _file_snapshot((ap,))
    try:
        removed = upmapconf.delete_file()
        conn.execute("DELETE FROM upstream_point_map")
        conn.commit()
    except Exception as e:
        conn.rollback()
        _file_restore(prev)
        raise ApiError(f"删除失败，已回滚：{e}", 500)

    auth.audit(conn, user["username"], "upstream_point_map_reset", "dispatch",
               "已删除平台表" if removed else "本来就没有平台表（未产生改动）")
    return 200, {"ok": True, "removed": removed, "source": "default",
                 "active_path": ap,
                 "note": "已恢复：网关下次启动用编译进去的内置默认表"
                         "（上送 24 / 下行 6）。" if removed else
                         "本来就没有任何平台表 —— 网关一直在用内置默认表。"}


def h_upstream_point_map_csv(conn, query, body, user, params) -> tuple[int, dict]:
    """GET /api/upstream/point-map.csv —— 导出 CSV。

    ?template=1 → P3 随包默认底本原样输出（当模板去改）；
    否则导出当前生效的表（有平台那份就用它，没有就用默认底本）。
    """
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    who = user["username"] if user else "-"
    if str(q1(query, "template", "") or "") in ("1", "true", "yes"):
        if not os.path.isfile(UPLINK_PM_CSV):
            raise ApiError(f"找不到内置默认表：{UPLINK_PM_CSV}", 404)
        with open(UPLINK_PM_CSV, "r", encoding="utf-8-sig") as f:
            text = f.read()
        hint = "P3 内置默认表（模板）"
        filename = "upstream-point-map_template.csv"
    else:
        ap = upmapconf.map_path()
        if os.path.isfile(ap):
            rows = upmapconf.load_file(ap)
            hint = "平台已保存的表"
        else:
            rows = upmapconf.parse(open(UPLINK_PM_CSV, encoding="utf-8-sig").read())
            hint = "内置默认表（尚无平台表）"
        text = upmapconf.dumps(rows, now, who + f"（导出：{hint}）")
        filename = "upstream-point-map.csv"

    auth.audit(conn, user["username"], "upstream_point_map_export", "dispatch", hint)
    return 200, {
        "__raw__": True,
        "content_type": "text/csv; charset=utf-8",
        "body": "\ufeff" + text,      # BOM：现场用 Excel 双击打开，中文不乱码
        "filename": filename,
    }


# ---------------------------------------------------------------------
# 接入调度主站九步向导（v1.14）
#
# ★ 为什么字段定义在后端（accessconf.STEPS）而不是在前端写死：
#   同一份清单如果要「界面渲染 + 完成校验 + CSV 导出」三处各写一遍，
#   迟早不一致 —— 那时候导出件看着完整，实际漏了界面新加的那一列。
#   前端只负责按 schema 画，不自己维护字段名。
#
# ★ 「填了就存」与「完成」是两回事：
#   PUT 不校验必填（现场资料常有没拿到的，带空缺往下走是常态）；
#   只有 completed=true 才校验，缺项一律 400 并**把缺项清单带回去**，
#   让界面能指出"第几步缺哪一格"，而不是只说"还有没填的"。
# ---------------------------------------------------------------------
def _access_ctx(conn) -> dict:
    """九步向导里那些「现状」数字 —— 全从库里读，界面与本步导出共用这一份。

    ★ 为什么不在前端算：转发表行数、当前监听口 / CA 都是从各表拼出来的，
      前端算一遍、导出再算一遍，两处迟早不一致；而这一页的用处正是
      「我说的和它看到的是同一个数」。
    ★ 取不到的一律留空（界面显示「—」），不猜默认值。
    """
    up = _link_row(conn) or {}
    rows: list = []
    try:
        _, pmv = h_upstream_point_map(conn, {}, {}, {"username": "-"}, {})
        rows = pmv.get("rows") or []
    except Exception:
        rows = []
    ups = [r for r in rows if str(r.get("direction")) != "down"]
    downs = [r for r in rows if str(r.get("direction")) == "down"]

    # 采集周期：各台一致才给一个数，不一致就直说 —— 报"某台 1000 ms"会误导。
    poll = ""
    try:
        ps = {int(r["poll_period_ms"]) for r in emsdb.query(
            conn, "SELECT poll_period_ms FROM device_conn WHERE poll_period_ms IS NOT NULL")}
        if len(ps) == 1:
            poll = str(ps.pop())
        elif len(ps) > 1:
            poll = "各台不同"
    except Exception:
        pass

    # 上端转发表文件**到底有没有落过盘** —— 这一条直接决定「改了界面会不会生效」：
    # 没有这个文件时网关退回编译进去的内置默认表（main_gateway.cpp ③ 分支），
    # 界面上改的 IOA 根本没人读。不写出来，人会以为保存了就等于生效了。
    try:
        if upmapconf.exists():
            pmfile = "已落盘（%d 上送 + %d 下行）" % (len(ups), len(downs))
        else:
            pmfile = "未落盘 · 网关用内置默认表"
    except Exception:
        pmfile = ""

    return {
        "today": accessconf._now()[:10],
        "port": up.get("listen_port") or "",
        "ca": up.get("common_addr") if up.get("common_addr") is not None else "",
        "bind": up.get("bind_host") or "",
        "upstream": "监听 %s:%s · CA %s" % (up.get("bind_host") or "-",
                                            up.get("listen_port") or "-",
                                            up.get("common_addr") if up.get("common_addr") is not None else "-"),
        "pmfile": pmfile,
        "up": len(ups), "down": len(downs),
        "up_first": (ups[0].get("name") if ups else ""),
        "down_first": (downs[0].get("name") if downs else ""),
        "poll": poll,
        "pm": [{"ioa": r.get("ioa"), "kind": r.get("kind"), "name": r.get("name"),
                "src": r.get("src"), "scale": r.get("scale"),
                "offset": r.get("offset"), "unit": r.get("unit"),
                "direction": r.get("direction")} for r in rows],
    }


def _access_steps_view(conn, draft: dict) -> list:
    """把 STEPS 与每步的 mats / tips / assist / export 拼在一起。

    ★ 本步表单里的值也并进占位符（系统状态优先）：材料清单里「现状」那一列
      常常就是"你刚才填了什么"，不并进去就会整列显示「—」。
    """
    base = _access_ctx(conn)
    out = []
    for st in accessconf.STEPS:
        row = (draft.get("steps") or {}).get(st["key"]) or {}
        c = {k: v for k, v in row.items() if v not in (None, "")}
        c.update({k: v for k, v in base.items() if v not in (None, "")})
        s2 = dict(st)
        s2["doc"] = accessconf.detail(st["num"], c)
        s2["assist"] = accessconf.assist(st["num"], c)
        s2["export"] = accessconf.export_spec(st["num"])
        out.append(s2)
    return out


def h_access_draft(conn, query, body, user, params) -> tuple[int, dict]:
    """GET /api/access-draft —— 草稿 + 字段定义 + 进度 + 缺项 + 每步材料清单。"""
    d = accessconf.load_draft(conn)
    return 200, {
        "ok": True,
        "steps": _access_steps_view(conn, d),
        "draft": d,
        "progress": accessconf.progress(d["steps"]),
        "missing": accessconf.missing(d["steps"]),
    }


def h_access_draft_put(conn, query, body, user, params) -> tuple[int, dict]:
    """PUT /api/access-draft —— 保存草稿；completed=true 时才校验必填。

    body: {"steps": {...}, "current_step": 3, "completed": true|false}
    """
    steps = body.get("steps")
    if steps is not None and not isinstance(steps, dict):
        raise ApiError("steps 必须是对象（以步骤号为键）", 400)
    want_done = body.get("completed")
    cur = body.get("current_step")

    # 先按「提交上来的内容」算缺项 —— 不能拿库里的旧草稿算，
    # 否则"这一页刚填完就点完成"会被上一版库里内容判成缺。
    probe = accessconf._clean(steps if steps is not None
                              else accessconf.load_draft(conn)["steps"])
    miss = accessconf.missing(probe)
    if want_done is True and miss:
        # 只回一句话，不回结构化清单：判定逻辑只有后端这一份，
        # 前端收到 400 后重新 GET 拿 missing —— 多一次请求，换掉"两边各算一遍"。
        raise ApiError(
            f"还有 {len(miss)} 项必填未填，不能标记完成："
            + "；".join(f"第 {m['step']} 步「{m['step_title']}」的{m['label']}"
                        for m in miss[:6])
            + ("…" if len(miss) > 6 else ""), 400)

    d = accessconf.save_draft(conn, steps, cur, user["username"] if user else "-",
                              completed=None if want_done is None else bool(want_done))
    if want_done is True:
        auth.audit(conn, user["username"], "access_draft_complete", "dispatch",
                   f"九步填报完成（{accessconf.progress(d['steps'])['required']} 项必填齐）")
    return 200, {
        "ok": True, "draft": d,
        "progress": accessconf.progress(d["steps"]),
        "missing": accessconf.missing(d["steps"]),
    }


def h_access_draft_delete(conn, query, body, user, params) -> tuple[int, dict]:
    """DELETE /api/access-draft —— 清空九步填写记录。

    ★ 只清这份草稿：**不动** active.upstream（监听口 / CA）与
      active.pointmap.csv（转发表）。那两样是已经生效的配置，
      跟"台账填到一半"是两件事，混在一起清会把线上配置一起抹掉。
    """
    accessconf.clear_draft(conn)
    auth.audit(conn, user["username"], "access_draft_reset", "dispatch",
               "清空九步填报记录（未动链路参数与转发表）")
    return 200, {"ok": True, "cleared": True,
                 "note": "九步填报记录已清空。链路参数（active.upstream）与"
                         "转发表（active.pointmap.csv）**没有**改动。"}


def h_access_draft_csv(conn, query, body, user, params) -> tuple[int, dict]:
    """GET /api/access-draft.csv —— 导出。

    `?step=N` 导出**那一步自己的**那份（第 2 步 = 远动信息参数表、
    第 3 步 = 号段对照表、第 8 步 = 传动记录、第 9 步 = 投运清单 + 九步汇总）；
    不带 step 仍是九步总台账。
    ★ 两份都带 BOM：现场用 Excel 双击打开，不带 BOM 中文就是乱码。
    """
    d = accessconf.load_draft(conn)
    # ★ query 是 parse_qs 的产物（值是 list），直接用 get 拿到的是 ['2']，
    #   str() 一下变成 "['2']" —— 这里必须走 q1()，别自己取。
    step = str(q1(query, "step", "") or "").strip()
    if step:
        try:
            n = int(step)
            accessconf._step(n)
        except Exception:
            raise ApiError("step 只能是 1~9（第几步的导出件）", 400)
        fname, text = accessconf.export_step(n, d["steps"], _access_ctx(conn))
        auth.audit(conn, user["username"], "access_draft_export", "dispatch",
                   f"第 {n} 步导出：{accessconf.export_spec(n)['name']}")
    else:
        fname, text = "access-to-dispatch-ledger.csv", accessconf.to_csv(d)
        auth.audit(conn, user["username"], "access_draft_export", "dispatch",
                   "九步台账 CSV")
    return 200, {
        "__raw__": True,
        "content_type": "text/csv; charset=utf-8",
        "body": "\ufeff" + text,
        "filename": fname,
    }


def h_data_source(conn, query, body, user, params) -> tuple[int, dict]:
    """GET /api/data-source —— 两端的形态、对端与实际目标。"""
    return 200, _data_source_view(conn)


def h_data_source_set(conn, query, body, user, params) -> tuple[int, dict]:
    """POST /api/data-source —— 切**一端**的输入源，并落盘给端侧。

    body: {"end": "upstream"|"downstream", "kind": "sim"|"real"}

    ★ 只有下端会动采集器（它按 active.conn 读写）；上端参数由网关在**自己
      启动时**读，改它不影响正在跑的采集，也就不会半途换对端。
    """
    end = str(body.get("end", "")).strip().lower()
    kind = str(body.get("kind", "")).strip().lower()
    if end not in ENDS:
        raise ApiError(
            "要指明切哪一端：end 只能是 upstream（调度主站接入）/ "
            "downstream（站内设备接入）。两端是各自独立的开关，一次只切一端 —— "
            "不做「两端一并切掉」。", 400)
    if kind not in engine.DATA_SOURCES:
        raise ApiError("输入源只能是 " + " / ".join(engine.DATA_SOURCES), 400)

    if end == "downstream" and engine.LIVE.snapshot(conn)["running"]:
        raise ApiError("采集正在运行：先停止采集再切换数据源 —— "
                       "运行中换对端，采集器还在按旧参数读写。", 409)

    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    who = user["username"] if user else "-"
    cp = connconf.conn_path()
    up_path = uplinkconf.link_path()
    prev = _file_snapshot((cp,) if end == "downstream" else (up_path,))
    conn_file = up_file = None
    try:
        if end == "downstream":
            conn.execute("UPDATE device_conn SET source_kind = ?, updated_at = ?",
                         (kind, now))
            # meta 只登记**下端**：它是采集器该走哪条路线的判据。
            # 上端的形态就记在 upstream_link.kind 上，不另抄一份 ——
            # 同一件事存两处，迟早分叉。
            emsdb.set_meta(conn, engine.M_DATA_SOURCE, kind)
            conn.commit()
            conn_file = _rewrite_conn_for_kind(conn, kind, now, who)
        else:
            # ★ 上端用 **upsert**，不能用裸 UPDATE：库里还没有这一行时 UPDATE
            #   影响 0 行，而 `_link_row()` 会回落成默认值（`kind='real'`）⇒
            #   界面出现「文件说模拟、上端卡说现场」。那正是"两半不同步"的静默错。
            #   默认值不是编出来的：`_link_row()` 在没有行时本来就把这套值当作
            #   生效配置，这里只是把它落成一行，让 **库 / 视图 / 文件** 三者一致。
            up_base = _link_row(conn)
            conn.execute(
                "INSERT INTO upstream_link(link_id, kind, protocol, bind_host,"
                " listen_port, common_addr, master_host, enabled, updated_at,"
                " username, note) VALUES(?,?,?,?,?,?,?,?,?,?,?)"
                " ON CONFLICT(link_id) DO UPDATE SET kind = excluded.kind,"
                " updated_at = excluded.updated_at",
                (uplinkconf.LINK_ID, kind, up_base["protocol"],
                 up_base["bind_host"], up_base["listen_port"],
                 up_base["common_addr"], up_base["master_host"],
                 up_base["enabled"], now, who, up_base["note"]))
            conn.commit()
            up = _link_row(conn)
            up_n = uplinkconf.normalize(dict(up, kind=kind))
            up_file = uplinkconf.save(dict(up_n, saved_at=now, username=who))
    except Exception as e:
        conn.rollback()
        _file_restore(prev)
        if isinstance(e, ApiError):
            raise
        raise ApiError(f"切换输入源失败，已整体回滚：{e}", 500)

    wrote = conn_file if end == "downstream" else up_file
    auth.audit(conn, user["username"], "data_source_set",
               END_META[end]["name"],
               f"end={end} kind={kind} file={os.path.basename(wrote['path'])}")
    out = _data_source_view(conn)
    if conn_file:
        out["conn_file"] = conn_file
    if up_file:
        out["upstream_file"] = up_file
    return 200, out


# =====================================================================
# 模拟输入源总开关（v1.7）—— 一个开关管两端 + 一键换一套
#
# 为什么不做成"每台自己填"：联调时人要做的事是「先让整条链跑起来」。
# ★ 本模块唯一要守的红线：**真实值一列都不动**（详见 simconf.py 文件头）。
# =====================================================================
def _sim_view(conn) -> dict:
    """模拟源的当前状态。两端形态都从各自的真相源读，不另抄一份。"""
    idx = simconf.index_read(conn)
    p = simconf.preset_of(idx)
    down_kind = engine.get_data_source(conn)
    up_kind = _upstream_kind(conn)
    chain = simproc.CHAIN.status(conn)
    # 「表换了、进程没换」是本项目最典型的静默错（两套并行、都自称正常）：
    # 换一套只改配置，进程还跑在旧端口段上。这里把它算成一张显式的脸。
    chain["preset_matches"] = (not chain["running"]) or (
        chain.get("preset_name") == p["name"])
    return {
        "ok": True,
        # 两端都切到 sim 才算"开着"；只开一端是另一件事（那走 /api/data-source）。
        "enabled": down_kind == "sim" and up_kind == "sim",
        "downstream": down_kind,
        "upstream": up_kind,
        "preset": p["id"],
        "preset_name": p["name"],
        "preset_count": simconf.preset_count(),
        "port_base": p["port_base"],
        "plan": simconf.plan(conn, idx),
        "note": simconf.note(),
        "chain": chain,
    }


def h_sim_source(conn, query, body, user, params) -> tuple[int, dict]:
    """GET /api/sim-source —— 模拟源总开关的状态与当前这一套。"""
    v = _sim_view(conn)
    v["collector"] = engine.LIVE.snapshot(conn)
    return 200, v


def h_sim_source_set(conn, query, body, user, params) -> tuple[int, dict]:
    """POST /api/sim-source —— 启用 / 关闭 / 换一套。

    body: {"enable": true|false, "rotate": true}
      · enable=true   两端切 sim，并按当前套号把模拟专用字段填好
      · enable=false  两端切回 real（真实值从未被动过，直接显示即可）
      · rotate=true   套号 +1（循环），重新填一遍并重写两份落盘文件

    ★ 返回 202（enable 给了）：**配置已经改好，链路在后台拉/收**，
      成败看 `out["chain"]["job"]`（`running` 期间是"在办"，`ok` 才是结果）。
      返回 200（只 rotate）：没动进程，办完了。
      详见 §92b —— 以前是让 HTTP 请求挂着等 6~7 s，用户盯着转圈干等。

    ★ 采集中不许动下端：运行中换对端，采集器还在按旧参数读写（与
      /api/data-source 同一条判据，409 的文案也保持一致）。
    """
    want = body.get("enable")
    rotate = bool(body.get("rotate"))
    if want is None and not rotate:
        raise ApiError("要说明干什么：enable=true 启用 / false 关闭，"
                       "或 rotate=true 换一套。", 400)
    if want is not None and not isinstance(want, bool):
        raise ApiError("enable 只能是 true / false", 400)

    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    who = user["username"] if user else "-"
    # ★ rotate 而没给 enable ⇒ **沿用两端各自的形态**，只换套号与模拟列。
    #   写成 `kind = "sim" if want else "real"` 会在 want=None 时落到 real ——
    #   那一下「换一套」等于把模拟源关了。
    #   （2026-10-18 浏览器实测：点「换一套」后徽章从「模拟中」变回「真实源」，
    #    而页面上唯一的解释是"套 3"—— 没有任何地方说形态被改了。）
    kind = None if want is None else ("sim" if want else "real")

    if want and engine.LIVE.snapshot(conn)["running"]:
        raise ApiError("采集正在运行：先停止采集再启用模拟源 —— "
                       "运行中换对端，采集器还在按旧参数读写。", 409)

    idx = simconf.next_index(conn) if rotate else simconf.index_read(conn)
    idx = simconf.index_write(conn, idx)

    cp = connconf.conn_path()
    up_path = uplinkconf.link_path()
    prev = _file_snapshot((cp, up_path))
    try:
        _apply_sim(conn, kind, idx, now, who)
        conn.commit()
    except Exception as e:
        conn.rollback()
        _file_restore(prev)
        if isinstance(e, ApiError):
            raise
        raise ApiError(f"切换模拟源失败，已整体回滚：{e}", 500)

    # ---- 配置改完，把**链路**也真的拉起来 / 收掉 ----
    # ★ 只改配置不碰进程，就是 v1.7~v1.9 那个"启用了但一行数据都没有"的样子。
    #   用户看到的是"点了启用，页面还是空的"，而界面上没有任何地方说还差进程。
    #
    # ★ 进程操作**不再在这里等**（§92b）：拉起整条链路要 6~7 s，以前是让 HTTP
    #   请求挂着等 —— 用户盯着转圈干等，还看不到"这事办完没有"。现在交给后台
    #   线程，本请求只记下"已经开始了"（HTTP 202）。
    #   ★ 代价：到这里 `ok` **不等于**"链路已经起来"，要看 `out["job"]`：
    #     `job.running` 期间是"在办"，`job.ok` 才是结果。前端轮询它。
    chain_err = ""
    if want is not None:            # ★ want=None = 只换套，**不动进程**（见 §85）
        job = simproc.CHAIN.submit(conn, "start" if want else "stop",
                                   username=who)
        if job is None:
            # 上一次链路操作还在跑。这里**不排队** —— 排两次等于让用户等两倍
            # 时间，而后一次还会把前一次的结果盖掉。
            chain_err = ("上一次链路操作还没完，这次没启动 —— "
                         "等它结束再点。（排队只会让两边的结果互相覆盖）")

    act = ("换到" + simconf.preset_of(idx)["name"]) if rotate else (
        "启用模拟源" if want else "关闭模拟源")
    auth.audit(conn, who, "sim_source_set", "模拟输入源",
               f"{act} kind={kind or '不变'} "
               f"preset={simconf.preset_of(idx)['name']}")
    out = _sim_view(conn)
    out["acted"] = act
    if chain_err:
        out["chain_error"] = chain_err
        out["chain"]["error"] = out["chain"].get("error") or chain_err
    # want=None（只换套）⇒ 没动进程 ⇒ 200"已经办完"；
    # 否则进程还在后台跑 ⇒ 202"收下了，还没完"（看 `out["chain"]["job"]`）。
    return (202 if want is not None else 200), out


def h_sim_proc(conn, query, body, user, params) -> tuple[int, dict]:
    """POST /api/sim-source/proc —— 模拟链路的进程管理（不管配置）。

    body: {"action": "start" | "stop" | "restart"}
      · start    按**当前这一套**把整条链拉起来（已经在跑就是 no-op）
      · stop     收掉整条链（采集器一并停 —— 它由 engine 管）
      · restart  「换一个进程」：全停掉，再按当前这一套重新拉起

    ★ 与 /api/sim-source 的分工：
        /api/sim-source        改的是**表**（端口段、形态）
        /api/sim-source/proc   改的是**进程**（谁在跑、跑在哪个端口段）
      「换一套」之后再点「换一个进程」，进程才真的搬到新端口段上。
      两者分开是有意的：换套不必打断正在看的采集；但界面必须提示"进程没跟"。

    ★ 返回 **202**：请求只是"收下这件事"，拉/收链路在后台线程跑。
      成败与最终 up 数写在 `out["chain"]["job"]` 里（`running` 期间是"在办"，
      `ok` 才是结果），由前端轮询 —— 详见 §92b。
      ★ 已经有一个在跑 ⇒ 409（不排队：排两次让用户等两倍时间，
        而后一次还会把前一次的结果盖掉）。
    """
    action = str(body.get("action") or "").strip().lower()
    if action not in ("start", "stop", "restart"):
        raise ApiError("action 只能是 start / stop / restart", 400)

    # ★ 下端是现场源时不许起链路：链路里的采集器只会去连**替身端口**，
    #   而当前数据源说"跟真机说话" —— 两者矛盾。真起起来的样子是"进程都在、
    #   页面没数"（2026-10-19 实测：直接 POST start 拿到 8/9，采集器报
    #   「端口 1802 没人应答」）。stop 不设这条门槛 —— 任何时候都该收得掉。
    if action in ("start", "restart") and engine.get_data_source(conn) != "sim":
        raise ApiError("下端当前是「现场数据源」，起链路没有意义 —— "
                       "链路里的采集器只会去连替身端口。先点「启用模拟源」。", 409)

    who = user["username"] if user else "-"
    # ★ 进程操作交给后台线程（§92b）：这一行**不再等**拉链路的 6~7 s。
    #   请求只是"收下这件事"（HTTP 202），真正的成败写在 `out["job"]` 里，
    #   由前端轮询 —— 详见 `simproc.CHAIN.submit` 的说明。
    #   ★ 审计挪到线程里写（`_run_job`）：那里才知道最终 up 多少、报没报错。
    job = simproc.CHAIN.submit(conn, action, username=who)
    if job is None:
        raise ApiError("上一次链路操作还没完，这次没启动 —— "
                       "等它结束再点。（排队只会让两边的结果互相覆盖）", 409)
    out = _sim_view(conn)
    out["acted"] = {"start": "启动模拟链路", "stop": "停止模拟链路",
                    "restart": "换一个进程"}[action]
    return 202, out


def _apply_sim(conn, kind: str | None, idx: int, now: str, who: str) -> None:
    """把形态与模拟专用字段落到库里，并重写两份落盘文件。

    ★ 这个函数**只碰模拟专用列**：
      下端 → `sim_host` / `sim_port`（`host` / `port` 是真实地址，一个字都不改）；
      上端 → `master_host`（`listen_port` / `common_addr` 是链路属性，真实与模拟共用）。

    ★ `kind=None` = **不动形态**（「换一套」且没给 enable 时的语义）：
      换套换的是"哪一组模拟数据"，不是"要不要用模拟源"。所以两端的形态
      各自沿用当前值 —— 上端与下端可能不一致，那就各留各的，不强行拉齐。
    """
    up_kind = kind or _upstream_kind(conn)
    dn_kind = kind or engine.get_data_source(conn)

    # ---- 上端：形态 + 模拟主站地址 ----
    up_base = _link_row(conn)
    conn.execute(
        "INSERT INTO upstream_link(link_id, kind, protocol, bind_host,"
        " listen_port, common_addr, master_host, enabled, updated_at,"
        " username, note) VALUES(?,?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(link_id) DO UPDATE SET"
        "   kind = excluded.kind, master_host = excluded.master_host,"
        "   updated_at = excluded.updated_at, username = excluded.username",
        (uplinkconf.LINK_ID, up_kind, up_base["protocol"], up_base["bind_host"],
         up_base["listen_port"], up_base["common_addr"],
         simconf.plan_upstream(conn, idx)["master_host"],
         up_base["enabled"], now, who, up_base["note"]))
    conn.commit()
    up_n = uplinkconf.normalize(dict(_link_row(conn), kind=up_kind))
    uplinkconf.save(dict(up_n, saved_at=now, username=who))

    # ---- 下端：形态 + 每台的模拟源地址 ----
    # ★ 用 **upsert** 而不是裸 UPDATE：台账里有 6 台、device_conn 里可能只有 1 行
    #   （其余是"先建台账、后接线"）。裸 UPDATE 影响不到它们 ⇒ 那 5 台一台都没
    #   被填，界面上是"启用成功了但表还是空的"。这正是 §79.5 那个 bug 的形状。
    # ★ 只写 sim_host / sim_port 两列：**host / port 一个字都不碰**
    #   （那两列是现场地址，联调完关掉模拟源还要用）。
    for r in simconf.plan_downstream(conn, idx):
        conn.execute(
            "INSERT INTO device_conn(device_id, source_kind, sim_host, sim_port,"
            " updated_at) VALUES(?,?,?,?,?)"
            " ON CONFLICT(device_id) DO UPDATE SET"
            "   source_kind = excluded.source_kind, sim_host = excluded.sim_host,"
            "   sim_port = excluded.sim_port, updated_at = excluded.updated_at",
            (r["device_id"], dn_kind, r["sim_host"], r["sim_port"], now))
    emsdb.set_meta(conn, engine.M_DATA_SOURCE, dn_kind)
    conn.commit()
    _rewrite_conn_for_kind(conn, dn_kind, now, who)


def h_device_create(conn, query, body, user, params) -> tuple[int, dict]:
    """新增设备台账。没有它，客户在界面上就没有可配置的设备（FK 约束）。"""
    did = str(body.get("device_id") or "").strip()
    if not did or not re.fullmatch(r"[0-9A-Za-z][0-9A-Za-z_-]{0,31}", did):
        raise ApiError("设备号只允许字母数字与 _ -，且以字母数字开头（1..32 位）", 400)
    if emsdb.query_one(conn, "SELECT device_id FROM device WHERE device_id = ?", (did,)):
        raise ApiError(f"设备已存在: {did}", 409)
    dtype = str(body.get("device_type") or "").strip().upper()
    if dtype not in ("PCS", "BMS", "METER", "PV", "TRANSFORMER", "LOAD", "GATEWAY"):
        raise ApiError(
            "设备类型应为 PCS/BMS/METER/PV/TRANSFORMER/LOAD/GATEWAY", 400)
    conn.execute(
        "INSERT INTO device(device_id, device_type, manufacturer, model,"
        " rated_power_kw, capacity_kwh, status, location, note)"
        " VALUES(?,?,?,?,?,?,?,?,?)",
        (did, dtype, body.get("manufacturer"), body.get("model"),
         body.get("rated_power_kw"), body.get("capacity_kwh"),
         "offline", body.get("location"), body.get("note")))
    conn.commit()
    auth.audit(conn, user["username"], "device_create", did, dtype)
    return 201, {"ok": True, "device": emsdb.query_one(
        conn, "SELECT * FROM device WHERE device_id = ?", (did,))}


# ---------------------------------------------------------------------
# 路由表
# ---------------------------------------------------------------------
Route = tuple[str, re.Pattern, Callable, Optional[str]]

# ---------------------------------------------------------------------
# 引擎（运行 / 仿真 双模式）
# ---------------------------------------------------------------------
def h_engine(conn, query, body, user, params) -> tuple[int, dict]:
    """GET /api/engine —— 当前引擎模式 + 实时引擎状态 + 可选数据源。

    这是前端"顶栏模式切换器"的数据源，也是"现在跑的是模型不是真机"
    这句话的唯一出处（sources[] 里 device 的 available=False + why）。
    """
    st = engine.status(conn)
    st["sim"] = {"active": simrun.active(), "kinds": [
        {"id": k, **{kk: vv for kk, vv in v.items() if kk != "prefix"}}
        for k, v in simrun.KINDS.items()]}
    return 200, st


def h_engine_set(conn, query, body, user, params) -> tuple[int, dict]:
    """POST /api/engine —— 切换引擎模式（运行 / 仿真）。

    只切模式，**不**顺手启停引擎：切到运行模式不会偷偷拉起实时引擎，
    切到仿真模式也不会把正在跑的实时引擎停掉。理由是可预测性 ——
    一个"切换"附带启动进程，用户没法预判自己的点击到底做了什么。
    启停都在运行模式页里由人显式点。
    """
    try:
        return 200, engine.set_mode(conn, str(body.get("engine", "")).strip(),
                                    user["username"])
    except engine.EngineError as e:
        raise ApiError(str(e), 400)


def h_live_start(conn, query, body, user, params) -> tuple[int, dict]:
    """POST /api/engine/live/start —— 启动采集（拉采集进程 + 开始尾随入库）。

    ★ 不传 source 时**按设备管理页选的数据源**启动（sim / real）——
      这是"接入后其他页面就能正常运行"的落点：页面不需要知道 source。
      传 source 是给测试与排障用的显式覆盖。
    """
    try:
        return 200, engine.LIVE.start(
            conn,
            source=str(body.get("source", "") or ""),
            username=user["username"],
            dt_s=qf(body, "dt_s", engine.DEFAULT_DT_S),
            log_every=qi(body, "log_every", engine.DEFAULT_LOG_EVERY))
    except engine.EngineError as e:
        raise ApiError(str(e), 400)


def h_live_stop(conn, query, body, user, params) -> tuple[int, dict]:
    """POST /api/engine/live/stop —— 停止实时引擎并收尾入库。"""
    try:
        return 200, engine.LIVE.stop(conn, user["username"])
    except engine.EngineError as e:
        raise ApiError(str(e), 400)


def h_live_reset(conn, query, body, user, params) -> tuple[int, dict]:
    """POST /api/engine/live/reset —— 清空实时数据集（保留台账与引擎模式）。"""
    try:
        return 200, engine.reset(conn, user["username"])
    except engine.EngineError as e:
        raise ApiError(str(e), 400)


# ---------------------------------------------------------------------
# 仿真（仿真模式的执行体）
# ---------------------------------------------------------------------
def h_sim_runs(conn, query, body, user, params) -> tuple[int, dict]:
    """GET /api/sim/runs —— 仿真运行台账 + 可选时长（日 / 周 / 月）。"""
    return 200, simrun.list_runs(conn, qi(query, "limit", 50))


def h_sim_start(conn, query, body, user, params) -> tuple[int, dict]:
    """POST /api/sim/runs —— 发起一次仿真。

    body: {kind: day|week|month, fault: bool, date?: 'YYYY-MM-DD'}
    ★ 立刻返回（202 语义也是 200：台账行已经落库、状态 running），
      进度靠 GET /api/sim/runs 轮询 —— 日仿真约 1 s、月仿真约十几秒，
      同步等会让前端在这十几秒里完全没反应。
    """
    kind = str(body.get("kind", "") or "").strip().lower()
    fault = bool(body.get("fault", False))
    date = body.get("date") or None
    if date is not None:
        date = str(date).strip() or None
        if date and not re.fullmatch(r"\d{4}-\d{2}-\d{2}", date):
            raise ApiError("date 只能是 YYYY-MM-DD", 400)
    try:
        return 200, simrun.start(conn, kind, fault=fault,
                                 username=user["username"], date=date)
    except simrun.SimRunError as e:
        raise ApiError(str(e), 400)


def h_sim_run_one(conn, query, body, user, params) -> tuple[int, dict]:
    """GET /api/sim/runs/{run_id} —— 单次仿真详情（含逐日能量明细）。"""
    try:
        return 200, simrun.get_run(conn, params["run_id"])
    except simrun.SimRunError as e:
        raise ApiError(str(e), 404)


def h_sim_run_delete(conn, query, body, user, params) -> tuple[int, dict]:
    """DELETE /api/sim/runs/{run_id} —— 删掉一次仿真（数据 + 台账 + 产物目录）。"""
    try:
        return 200, simrun.delete(conn, params["run_id"], user["username"])
    except simrun.SimRunError as e:
        raise ApiError(str(e), 400)


ROUTES: list[Route] = [
    ("GET",  re.compile(r"/api/health"), h_health, None),
    ("POST", re.compile(r"/api/login"), h_login, None),
    ("POST", re.compile(r"/api/logout"), h_logout, None),
    ("GET",  re.compile(r"/api/me"), h_me, None),

    ("GET",  re.compile(r"/api/overview"), h_overview, "viewer"),
    ("GET",  re.compile(r"/api/energy-flow"), h_overview, "viewer"),

    ("GET",  re.compile(r"/api/devices"), h_devices, "viewer"),
    ("POST", re.compile(r"/api/devices"), h_device_create, "operator"),

    # 设备接入（v1.2）—— 具体路径放在 /api/devices/{id} 之前，读起来更清楚
    ("GET",  re.compile(r"/api/devices/(?P<device_id>[\w\-]+)/point-map\.csv"),
     h_point_map_csv, "viewer"),
    ("GET",  re.compile(r"/api/devices/(?P<device_id>[\w\-]+)/point-map"),
     h_point_map, "viewer"),
    ("PUT",  re.compile(r"/api/devices/(?P<device_id>[\w\-]+)/point-map"),
     h_point_map_put, "operator"),
    ("PUT",  re.compile(r"/api/devices/(?P<device_id>[\w\-]+)/conn"),
     h_conn_put, "operator"),

    # 接入装配（v1.5）：上端 = 调度侧 IEC104 从站；下端 = 设备侧 Modbus 主站
    ("GET",  re.compile(r"/api/data-source"), h_data_source, "viewer"),
    ("POST", re.compile(r"/api/data-source"), h_data_source_set, "operator"),
    ("GET",  re.compile(r"/api/access"), h_access, "viewer"),
    ("PUT",  re.compile(r"/api/access/upstream"), h_access_upstream_put, "operator"),
    # 模拟输入源总开关（v1.7）：一个开关管两端 + 一键换一套
    ("GET",  re.compile(r"/api/sim-source"), h_sim_source, "viewer"),
    ("POST", re.compile(r"/api/sim-source"), h_sim_source_set, "operator"),
    # 进程管理（start / stop / restart）与配置分开：见 h_sim_proc 的 docstring。
    ("POST", re.compile(r"/api/sim-source/proc"), h_sim_proc, "operator"),
    # 上端第二份输入：IOA 转发表（v1.6：可保存，落到约定路径给网关读）
    ("GET",  re.compile(r"/api/upstream/point-map\.csv"),
     h_upstream_point_map_csv, "viewer"),
    ("GET",  re.compile(r"/api/upstream/point-map"), h_upstream_point_map, "viewer"),
    ("PUT",  re.compile(r"/api/upstream/point-map"),
     h_upstream_point_map_put, "operator"),
    ("DELETE", re.compile(r"/api/upstream/point-map"),
     h_upstream_point_map_delete, "operator"),

    # 接入调度主站九步向导（v1.14）：草稿 + 台账导出。字段定义在 accessconf。
    # ★ .csv 必须排在不带后缀的那条之前 —— 虽然都是 fullmatch、顺序无关，
    #   但读代码的人不该被这个绕一次。
    ("GET",  re.compile(r"/api/access-draft\.csv"), h_access_draft_csv, "viewer"),
    ("GET",  re.compile(r"/api/access-draft"), h_access_draft, "viewer"),
    ("PUT",  re.compile(r"/api/access-draft"), h_access_draft_put, "operator"),
    ("DELETE", re.compile(r"/api/access-draft"), h_access_draft_delete, "operator"),

    ("GET",  re.compile(r"/api/devices/(?P<device_id>[\w\-]+)"),
     h_device_one, "viewer"),

    ("GET",  re.compile(r"/api/realtime"), h_realtime, "viewer"),
    ("GET",  re.compile(r"/api/series"), h_series, "viewer"),
    ("GET",  re.compile(r"/api/steps"), h_steps, "viewer"),
    ("GET",  re.compile(r"/api/commands"), h_commands, "viewer"),

    ("GET",  re.compile(r"/api/alarms/summary"), h_alarm_summary, "viewer"),
    ("GET",  re.compile(r"/api/alarms"), h_alarms, "viewer"),

    ("GET",  re.compile(r"/api/strategies"), h_strategies, "viewer"),
    ("PUT",  re.compile(r"/api/strategies/(?P<strategy_id>[\w\-]+)"),
     h_strategy_update, "operator"),

    ("GET",  re.compile(r"/api/modes"), h_modes, "viewer"),
    ("POST", re.compile(r"/api/modes/activate"), h_mode_activate, "operator"),

    # 引擎（数据维度，v1.5）：运行模式 = 实时引擎在跑；仿真模式 = 离线批量仿真
    # 注意路由顺序：具体路径写在 /api/engine 之前无所谓（都是 fullmatch），
    # 但 /api/sim/runs/{id} 必须能被 fullmatch 命中，\w- 不含 '/'，安全。
    ("GET",  re.compile(r"/api/engine"), h_engine, "viewer"),
    ("POST", re.compile(r"/api/engine"), h_engine_set, "operator"),
    ("POST", re.compile(r"/api/engine/live/start"), h_live_start, "operator"),
    ("POST", re.compile(r"/api/engine/live/stop"), h_live_stop, "operator"),
    ("POST", re.compile(r"/api/engine/live/reset"), h_live_reset, "operator"),

    ("GET",  re.compile(r"/api/sim/runs"), h_sim_runs, "viewer"),
    ("POST", re.compile(r"/api/sim/runs"), h_sim_start, "operator"),
    ("GET",  re.compile(r"/api/sim/runs/(?P<run_id>[\w\-]+)"),
     h_sim_run_one, "viewer"),
    ("DELETE", re.compile(r"/api/sim/runs/(?P<run_id>[\w\-]+)"),
     h_sim_run_delete, "operator"),

    ("GET",  re.compile(r"/api/economics"), h_economics, "viewer"),
    ("GET",  re.compile(r"/api/energy"), h_energy, "viewer"),
    ("GET",  re.compile(r"/api/report"), h_report, "viewer"),
    ("GET",  re.compile(r"/api/invariants"), h_invariants, "viewer"),

    ("POST", re.compile(r"/api/control/command"), h_command_send, "operator"),

    ("GET",  re.compile(r"/api/audit"), h_audit, "admin"),
    ("GET",  re.compile(r"/api/users"), h_users, "admin"),
    ("POST", re.compile(r"/api/users"), h_user_create, "admin"),

    ("GET",  re.compile(r"/api/export/(?P<kind>\w+)"), h_export, "viewer"),
]


def dispatch(conn, method: str, path: str, query: dict, body: dict,
             user: Optional[dict]) -> tuple[int, dict]:
    for m, rx, fn, role in ROUTES:
        if m != method:
            continue
        mo = rx.fullmatch(path)
        if not mo:
            continue
        if role:
            ok, why = auth.require(user, role)
            if not ok:
                return 403 if user else 401, {"error": why}
        try:
            return fn(conn, query, body, user, mo.groupdict())
        except ApiError as e:
            return e.status, {"error": e.message}
        except (engine.EngineError, simrun.SimRunError) as e:
            # 领域层"可预期的错误"（数据源不可用 / 仿真器缺失 / 运行编号不存在…）
            # 一律 400：它们都是"你这么请求不行"，而不是平台内部炸了。
            return 400, {"error": str(e)}
        except Exception as e:  # 兜底：不要把栈泄露给前端
            return 500, {"error": f"内部错误: {type(e).__name__}: {e}"}
    return 404, {"error": f"未知接口 {method} {path}"}
