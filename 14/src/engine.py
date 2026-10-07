"""14/ — 运行引擎（「运行模式」的数据源）

把「数据从哪来」从"页面上的一个下拉框"提升为**平台级状态**：

    engine = 'run'   运行模式 —— 引擎在实时跑，数据边跑边入库
    engine = 'sim'   仿真模式 —— 离线批量仿真，跑完把结果装成一份数据集

★ 与 run_mode（峰谷套利 / 削峰填谷 / 辅助服务 / 保电备电）的区别，别混：
    run_mode  是**业务维度**：同一套电池在干哪种生意，切换即联动启停策略
    engine    是**数据维度**：数据是"正在发生"还是"离线算出来的"
  两者正交：运行模式下也能切四种生意；仿真模式下也能仿任何一种生意。

---------------------------------------------------------------------
设计要点（每一条都对着一个具体的坑）

1) **引擎状态落库，不只在内存里。**
   状态（模式 / 实录文件 / 行数 / 状态）写进 meta 表。服务器重启后
   重新读出来就能接着尾随 —— 否则"重启平台"= 实时数据永久断流，
   而现场进程可能还在老老实实地写实录。

2) **活跃性判据是"实录文件还在长"，不是 PID。**
   Windows 上 os.kill(pid, 0) 不是"探测存活"，它真的会去 TerminateProcess
   （退出码 0）—— 用它探活会把被探的进程杀掉。而 tasklist / OpenProcess
   要么慢、要么要 ctypes 绕一圈。更根本的是：**我们真正关心的是
   "数据还在不在长"**，进程活着但不写数据（卡住、崩在写之前）同样该报出来。
   所以：文件 N 个周期没增长 → 状态置 'stale' 并如实说明。

3) **尾随要按字节偏移增量读，不能反复全量重读。**
   实录文件跑一天是 8640 行、跑一周是 6 万行。每 10 秒重读一遍是 O(n²)，
   而且越跑越慢 —— 正好在最需要它灵敏的时候变钝。只读新增的**完整行**
   （不留半行：进程被杀时最后一行可能写了一半，半行解析出来是脏数据）。

4) **数据源是插拔的，且当前只认一个。**
   本版本的实时源是 10/ sim_live.exe（模型按墙钟跑）。
   接真机走 07/ main_field.exe --device modbus + **同一份实录契约**，
   平台的入库与界面一行都不用改。但"接真机"要下发真实指令，
   不是界面上点一下就能放开的 —— 见 SOURCES 里 device 的 available=False
   与 why 字段：**界面必须如实显示"现在跑的是模型源"**。
   任何"看起来像接了真机"的界面，都是这个项目最不能接受的东西。
"""

from __future__ import annotations

import csv as _csv
import datetime
import io
import os
import subprocess
import sys
import threading
import time
from typing import Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import emsdb  # noqa: E402
import importer  # noqa: E402

PROJECT_ROOT = os.path.dirname(emsdb.MODULE_ROOT)

# 引擎模式
ENGINE_RUN = "run"        # 运行模式
ENGINE_SIM = "sim"        # 仿真模式
ENGINES = (ENGINE_RUN, ENGINE_SIM)

# 运行模式的数据集 id（time_base='wall'）
LIVE_SCENARIO = "live"

# meta 键
M_ENGINE_MODE = "engine_mode"
M_LIVE_STATE = "live_state"
M_LIVE_CSV = "live_csv"
M_LIVE_STARTED = "live_started_at"
M_LIVE_ROWS = "live_rows"
M_LIVE_DT = "live_dt"
M_LIVE_LOG_EVERY = "live_log_every"
M_LIVE_SOURCE = "live_source"
M_LIVE_ERROR = "live_error"
M_LIVE_IMPORTED_AT = "live_imported_at"
M_LIVE_WALL_S = "live_wall_s"

# 实时引擎默认参数
DEFAULT_DT_S = 1.0          # 控制周期 = 墙钟节拍（1 秒一拍）
DEFAULT_LOG_EVERY = 10      # 每 10 拍写一行 → 10 s 粒度（全项目图表的默认粒度）
STALL_INTERVALS = 6         # 连续这么多个周期没长 → 判 stale


class EngineError(Exception):
    """引擎相关的可预期错误，api.py 会把它转成 4xx。"""


# ---------------------------------------------------------------------
# 数据源清单
# ---------------------------------------------------------------------
def _exe(*parts: str) -> str:
    return os.path.join(PROJECT_ROOT, *parts)


SOURCES = [
    {
        "id": "model",
        "name": "实时仿真源（10/ sim_live.exe）",
        "exe": _exe("10", "build", "sim_live.exe"),
        "kind": "model",
        "available": True,
        "why": "",
        "note": "按墙钟节拍逐拍跑完整闭环（安全→仲裁→状态机→输出整形），"
                "负荷/光伏/电价按典型日曲线、相位取当天真实时刻。"
                "**这是模型，不是真实电站** —— 曲线是典型日回绕，"
                "不是真实天气序列。",
    },
    {
        "id": "device",
        "name": "现场设备（07/ main_field.exe --device modbus）",
        "exe": _exe("07", "build", "main_field.exe"),
        "kind": "device",
        "available": False,
        "why": "接真机会**真的向 PCS 下发功率指令**。放开这个入口的前置条件是："
               "① 设备管理页填好接入参数（active.conn，enabled=1）；"
               "② 先用只读模式核对点表与真实数据一致；"
               "③ 现场确认后再加 --control。"
               "本版本不提供界面入口，需要人工执行命令；命令见 07/docs/README.md。",
        "note": "接真机后，实录 CSV 的列契约与模型源**完全相同**"
                "（都走 07/src/record_csv.h），平台的入库与界面无需改动。",
    },
]


def source_of(sid: str) -> dict:
    for s in SOURCES:
        if s["id"] == sid:
            return s
    return SOURCES[0]


# ---------------------------------------------------------------------
# 引擎模式
# ---------------------------------------------------------------------
def get_mode(conn) -> str:
    m = (emsdb.get_meta(conn, M_ENGINE_MODE) or "").strip()
    return m if m in ENGINES else ENGINE_RUN


def set_mode(conn, mode: str, username: str = "") -> dict:
    if mode not in ENGINES:
        raise EngineError(f"未知引擎模式 {mode}（只能是 {' / '.join(ENGINES)}）")
    old = get_mode(conn)
    emsdb.set_meta(conn, M_ENGINE_MODE, mode)
    conn.commit()
    import auth  # 局部导入：避免与本模块的其它依赖形成环
    if username:
        auth.audit(conn, username, "engine_mode",
                   "engine", f"{old} -> {mode}")
    return {"engine": mode, "previous": old}


# ---------------------------------------------------------------------
# 运行模式的实时引擎
# ---------------------------------------------------------------------
class LiveEngine:
    """实时引擎：拉起数据源进程 + 尾随实录文件增量入库。

    一个进程一份实例（模块级 LIVE）。所有跨线程共享的字段都过 _lock，
    但**不**在持锁时做 IO 或 DB 操作 —— 那些动辄几十毫秒，
    持锁会让 /api/engine 这类只读接口跟着卡顿。
    """

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._thread: Optional[threading.Thread] = None
        self._stop_evt = threading.Event()
        self.proc: Optional[subprocess.Popen] = None
        # 尾随状态
        self.csv_path: str = ""
        self.byte_pos: int = 0
        self.header_seen: bool = False
        self.dt_s: float = DEFAULT_DT_S
        self.log_every: int = DEFAULT_LOG_EVERY
        self.source: str = "model"
        self.started_at: str = ""
        self.rows: int = 0
        self.last_growth_ts: float = 0.0
        self.imported_at: str = ""
        # 尾随线程要自己开连接（sqlite3 不允许跨线程共用一个连接），
        # 必须**继承请求连接的库路径**，否则会写到默认库上（见 emsdb.Conn）。
        self.db_path: str = ""

    # ------------------------- 状态读写 -------------------------
    def _persist(self, conn, **kv) -> None:
        for k, v in kv.items():
            emsdb.set_meta(conn, k, "" if v is None else str(v))

    def snapshot(self, conn) -> dict:
        """给接口用的状态快照。以**库里的 meta** 为准，内存只补进程句柄。"""
        state = (emsdb.get_meta(conn, M_LIVE_STATE) or "stopped").strip()
        rows_db = emsdb.query_value(
            conn, "SELECT COUNT(*) FROM step_record WHERE scenario_id = ?",
            (LIVE_SCENARIO,), 0)
        with self._lock:
            running = self._thread is not None and self._thread.is_alive()
            pid = self.proc.pid if (self.proc and self.proc.poll() is None) else None
        src = source_of((emsdb.get_meta(conn, M_LIVE_SOURCE) or "model").strip())
        return {
            "state": state,
            "running": running,
            "attached": running,
            "pid": pid,
            "scenario_id": LIVE_SCENARIO,
            "time_base": "wall",
            "source": src["id"],
            "source_name": src["name"],
            "source_is_model": src["kind"] == "model",
            "source_note": src["note"],
            "csv": emsdb.get_meta(conn, M_LIVE_CSV) or "",
            "started_at": emsdb.get_meta(conn, M_LIVE_STARTED) or "",
            "imported_at": emsdb.get_meta(conn, M_LIVE_IMPORTED_AT) or "",
            "dt_s": _f(emsdb.get_meta(conn, M_LIVE_DT), DEFAULT_DT_S),
            "log_every": int(_f(emsdb.get_meta(conn, M_LIVE_LOG_EVERY),
                                DEFAULT_LOG_EVERY)),
            "rows": int(rows_db),
            "imported_rows": int(_f(emsdb.get_meta(conn, M_LIVE_ROWS), 0)),
            "wall_s": _f(emsdb.get_meta(conn, M_LIVE_WALL_S), 0.0),
            "error": emsdb.get_meta(conn, M_LIVE_ERROR) or "",
            "stale_after_s": STALL_INTERVALS * max(1.0, _f(
                emsdb.get_meta(conn, M_LIVE_LOG_EVERY), DEFAULT_LOG_EVERY)
                * _f(emsdb.get_meta(conn, M_LIVE_DT), DEFAULT_DT_S)),
        }

    def _live_dir(self) -> str:
        d = os.path.join(emsdb.MODULE_ROOT, "data", "live")
        os.makedirs(d, exist_ok=True)
        return d

    # ------------------------- 启动 / 停止 -------------------------
    def start(self, conn, source: str = "model", username: str = "",
              dt_s: float = DEFAULT_DT_S,
              log_every: int = DEFAULT_LOG_EVERY) -> dict:
        src = source_of(source)
        if not src["available"]:
            raise EngineError(
                f"数据源「{src['name']}」本版本不可用：{src['why']}")
        exe = src["exe"]
        if not os.path.isfile(exe):
            raise EngineError(
                f"数据源可执行文件不存在：{exe}\n"
                f"先编译：先跑 10/scripts/build.bat")

        cur = self.snapshot(conn)
        if cur["running"]:
            raise EngineError(
                f"实时引擎已经在运行（启动于 {cur['started_at']}，"
                f"已入库 {cur['rows']} 行）。先停止再启动。")

        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        csv_path = os.path.join(self._live_dir(), f"live_{stamp}.csv")
        cmd = [exe, "--record", csv_path,
               "--dt", repr(float(dt_s)),
               "--log-every", str(int(log_every)),
               "--duration-s", "0",          # 一直跑，直到被停
               "--progress-every", "0",
               "--quiet"]
        try:
            proc = subprocess.Popen(
                cmd, cwd=PROJECT_ROOT,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                creationflags=_no_window_flags())
        except OSError as e:
            raise EngineError(f"拉不起数据源进程：{e}")

        with self._lock:
            self.proc = proc
            self.csv_path = csv_path
            self.byte_pos = 0
            self.header_seen = False
            self.dt_s = float(dt_s)
            self.log_every = int(log_every)
            self.source = src["id"]
            self.started_at = _now()
            self.rows = 0
            self.last_growth_ts = time.time()
            self.db_path = emsdb.db_path_of(conn)
            self._stop_evt.clear()

        self._persist(
            conn,
            live_state="starting", live_csv=csv_path,
            live_started_at=self.started_at, live_rows=0,
            live_dt=float(dt_s), live_log_every=int(log_every),
            live_source=src["id"], live_error="", live_wall_s=0.0,
        )
        conn.commit()
        self._spawn_tailer()
        import auth
        if username:
            auth.audit(conn, username, "engine_live_start", "engine",
                       f"source={src['id']} dt={dt_s} log_every={log_every} "
                       f"csv={os.path.basename(csv_path)}")
        return self.snapshot(conn)

    def stop(self, conn, username: str = "") -> dict:
        with self._lock:
            proc = self.proc
            self._stop_evt.set()
            th = self._thread
        if th is not None:
            th.join(timeout=5.0)

        killed = False
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                proc.kill()
                killed = True
        # 被外部拉起（服务器重启后接上的情形）没有句柄：只能等它自己结束，
        # 所以这里如实说明，而不是假装"已停止"。
        unattached = proc is None and \
            (emsdb.get_meta(conn, M_LIVE_STATE) or "") in ("running", "starting")

        # 收尾前把剩下的完整行吃掉
        n = self.poll_once(conn)

        with self._lock:
            self._thread = None
            self.proc = None
        self._persist(conn, live_state="stopped")
        conn.commit()
        import auth
        if username:
            auth.audit(conn, username, "engine_live_stop", "engine",
                       f"rows={n}" + ("（进程由外部管理，未持有句柄）"
                                      if unattached else ""))
        snap = self.snapshot(conn)
        snap["stopped_rows"] = n
        snap["killed"] = killed
        snap["unattached"] = unattached
        return snap

    # ------------------------- 尾随 -------------------------
    def _spawn_tailer(self) -> None:
        with self._lock:
            if self._thread is not None and self._thread.is_alive():
                return
            self._stop_evt.clear()
            th = threading.Thread(target=self._loop, name="ems-live-tailer",
                                  daemon=True)
            self._thread = th
            th.start()

    def attach_if_needed(self, conn) -> bool:
        """服务器重启后接上仍在跑的引擎。返回是否启动了尾随线程。

        判据不是 PID，而是「实录文件还在长」—— 见模块头注释第 2 条。
        """
        state = (emsdb.get_meta(conn, M_LIVE_STATE) or "stopped").strip()
        csv_path = emsdb.get_meta(conn, M_LIVE_CSV) or ""
        if state not in ("running", "starting"):
            return False
        if not csv_path or not os.path.isfile(csv_path):
            self._persist(conn, live_state="stale",
                          live_error="实录文件不存在，无法接上")
            conn.commit()
            return False
        age = time.time() - os.path.getmtime(csv_path)
        if age > STALL_INTERVALS * max(1.0, _f(emsdb.get_meta(conn, M_LIVE_DT),
                                               DEFAULT_DT_S)
                                       * _f(emsdb.get_meta(conn, M_LIVE_LOG_EVERY),
                                            DEFAULT_LOG_EVERY)):
            self._persist(conn, live_state="stale",
                          live_error=f"实录文件已 {age:.0f} s 没有更新，"
                                     f"进程可能已退出")
            conn.commit()
            return False
        with self._lock:
            self.csv_path = csv_path
            self.dt_s = _f(emsdb.get_meta(conn, M_LIVE_DT), DEFAULT_DT_S)
            self.log_every = int(_f(emsdb.get_meta(conn, M_LIVE_LOG_EVERY),
                                    DEFAULT_LOG_EVERY))
            self.source = (emsdb.get_meta(conn, M_LIVE_SOURCE) or "model").strip()
            self.started_at = emsdb.get_meta(conn, M_LIVE_STARTED) or _now()
            # 从库里已有的行数接着数；byte_pos 从 0 起也没关系 ——
            # 增量入库是 INSERT OR REPLACE 幂等的，重放不会产生重复行。
            self.byte_pos = 0
            self.header_seen = False
            self.rows = 0
            self.last_growth_ts = time.time()
            self.db_path = emsdb.db_path_of(conn)
        self._spawn_tailer()
        return True

    def _interval(self) -> float:
        with self._lock:
            return max(1.0, float(self.log_every) * float(self.dt_s))

    def _loop(self) -> None:
        interval = self._interval()
        idle = 0
        while not self._stop_evt.is_set():
            grew = 0
            try:
                conn = self._connect()
                try:
                    grew = self.poll_once(conn)
                    if grew:
                        idle = 0
                    else:
                        idle += 1
                        if idle >= STALL_INTERVALS:
                            self._persist(
                                conn, live_state="stale",
                                live_error=f"实录文件 {idle} 个周期"
                                           f"（约 {idle * interval:.0f} s）"
                                           f"没有增长，进程可能已退出")
                            conn.commit()
                            break
                finally:
                    conn.close()
            except Exception as e:  # 尾随线程不能把服务器带崩
                try:
                    conn = self._connect()
                    try:
                        self._persist(conn, live_state="failed",
                                      live_error=f"{type(e).__name__}: {e}")
                        conn.commit()
                    finally:
                        conn.close()
                except Exception:
                    pass
                break
            self._stop_evt.wait(interval)

    def _connect(self):
        """尾随线程自己的连接 —— 连**同一个库**，不是默认库。"""
        with self._lock:
            path = self.db_path
        return emsdb.connect(path or None)

    def poll_once(self, conn) -> int:
        """读实录文件的新增完整行并入库。返回本次入库行数。"""
        with self._lock:
            path, pos = self.csv_path, self.byte_pos
        if not path or not os.path.isfile(path):
            return 0

        try:
            size = os.path.getsize(path)
        except OSError:
            return 0
        if size <= pos:
            return 0

        with open(path, "rb") as f:
            f.seek(pos)
            chunk = f.read()
        # 只吃**完整行**：末尾那半行（进程正在写 / 被杀在写一半）留到下次。
        # 解析半行不会报错，只会写进一条脏数据 —— 而脏数据比缺数据更坏。
        nl = chunk.rfind(b"\n")
        if nl < 0:
            return 0
        data = chunk[:nl + 1]
        consumed = len(data)

        text = data.decode("utf-8", errors="replace")
        cols = list(importer.CSV_MAP.keys())
        recs = []
        for parts in _csv.reader(io.StringIO(text)):
            if not parts or parts[0] == "t_s" or len(parts) < len(cols):
                continue
            recs.append(importer.csv_row_to_rec(dict(zip(cols, parts))))

        with self._lock:
            self.byte_pos += consumed
            self.last_growth_ts = time.time()
            n_rows_total = self.rows + len(recs)
            self.rows = n_rows_total

        if recs:
            importer.import_live_rows(
                conn, recs, LIVE_SCENARIO,
                "运行模式实时数据（模型源）" if self.source == "model"
                else "运行模式实时数据（现场设备）",
                _rel_source(path))
        self._persist(conn, live_state="running", live_rows=n_rows_total,
                      live_imported_at=_now())
        conn.commit()
        return len(recs)


def _rel_source(path: str) -> str:
    """实录文件 → 记进 scenario.source_dir 的展示路径。

    ★ 必须容忍"实录文件不在项目盘上"：Windows 上 os.path.relpath 跨盘符
      （C: 与 D:）会直接抛 ValueError: path is on mount 'C:', start on mount 'D:'。
      这个异常发生在尾随线程里 → 整条实时链路被判 failed，而**数据其实是好的**
      （只是台账写不进去）。测试用系统临时目录跑就正好踩到。
    """
    try:
        return os.path.relpath(path, PROJECT_ROOT).replace("\\", "/")
    except ValueError:
        return path.replace("\\", "/")


def _no_window_flags() -> int:
    """Windows 下不要弹出控制台黑窗（被 spawn 的子进程不该抢焦点）。"""
    if os.name == "nt":
        return getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)
    return 0


def _now() -> str:
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def _f(v, default: float) -> float:
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


# ---------------------------------------------------------------------
# 模块级单例
# ---------------------------------------------------------------------
LIVE = LiveEngine()


# ---------------------------------------------------------------------
# 面向接口层的聚合视图
# ---------------------------------------------------------------------
def default_scenario(conn) -> str:
    """当前引擎模式下，页面默认该看哪个数据集。

    运行模式 → live（实时入库的那份）
    仿真模式 → 最近一次**成功**的仿真运行；没有就退回内置默认场景

    ★ 这里**不做"live 没数据就悄悄换 normal"** 的事：那会让"平台没在跑"
      和"跑着但还没数据"看起来一模一样，而这正是运行模式最需要说清的事。
      没数据时由 h_overview 报一条明确的 404（去启动引擎）。
    """
    if get_mode(conn) == ENGINE_RUN:
        return LIVE_SCENARIO
    latest = emsdb.query_one(
        conn, "SELECT scenario_id FROM sim_run WHERE status = 'ok'"
              " ORDER BY started_at DESC LIMIT 1")
    if not latest:
        latest = emsdb.query_one(
            conn, "SELECT scenario_id FROM scenario WHERE scenario_id LIKE 'sim-%'"
                  " ORDER BY imported_at DESC LIMIT 1")
    if latest:
        return latest["scenario_id"]
    return "normal"


def status(conn) -> dict:
    """GET /api/engine 的主体。"""
    mode = get_mode(conn)
    live = LIVE.snapshot(conn)
    rows_live = live["rows"]
    rows_sim = emsdb.query_value(
        conn, "SELECT COUNT(*) FROM sim_run", (), 0)
    return {
        "engine": mode,
        "engines": [
            {
                "id": ENGINE_RUN, "name": "运行模式",
                "desc": "数据边跑边来：引擎按墙钟节拍实时跑，界面跟着长",
                "dataset": LIVE_SCENARIO,
                "rows": rows_live,
                "ready": rows_live > 0,
            },
            {
                "id": ENGINE_SIM, "name": "仿真模式",
                "desc": "离线批量仿真：日 / 周 / 月，跑完装成一份数据集",
                "dataset": default_scenario(conn) if mode == ENGINE_SIM else "",
                "rows": emsdb.query_value(
                    conn,
                    "SELECT COUNT(*) FROM step_record WHERE scenario_id = ?",
                    (default_scenario(conn),), 0) if mode == ENGINE_SIM else 0,
                "runs": int(rows_sim),
                "ready": int(rows_sim) > 0,
            },
        ],
        "scenario": default_scenario(conn),
        "live": live,
        "sources": [{k: v for k, v in s.items() if k != "exe"} for s in SOURCES],
        "note": "运行模式的数据源当前是**实时仿真源（模型）**，"
                "不是真实电站；接真机后改用 07/ --device modbus，"
                "实录契约与入库路径不变。",
    }


def ensure_started(conn) -> dict:
    """服务器启动时调一次：接上仍在跑的实时引擎（若有）。"""
    attached = LIVE.attach_if_needed(conn)
    return {"attached": attached, **LIVE.snapshot(conn)}


def reset(conn, username: str = "") -> dict:
    """清空实时数据集（保留运行台账）。

    为什么需要：live 数据是**累积**的，跑几天之后想对比"从零开始"
    或者库里被写进了一段跑歪的数据，唯一出路是整体清掉。
    只清数据，不动 engine_mode —— 清完还能接着启动引擎。
    """
    if LIVE.snapshot(conn)["running"]:
        raise EngineError("引擎正在运行，先停止再清空实时数据。")
    n = emsdb.query_value(
        conn, "SELECT COUNT(*) FROM step_record WHERE scenario_id = ?",
        (LIVE_SCENARIO,), 0)
    for t in ("step_record", "control_command", "alarm"):
        conn.execute(f"DELETE FROM {t} WHERE scenario_id = ?", (LIVE_SCENARIO,))
    conn.execute("DELETE FROM scenario WHERE scenario_id = ?", (LIVE_SCENARIO,))
    conn.commit()
    import auth
    if username:
        auth.audit(conn, username, "engine_live_reset", "engine",
                   f"清除 {n} 行实时数据")
    return {"cleared": int(n), "scenario_id": LIVE_SCENARIO}
