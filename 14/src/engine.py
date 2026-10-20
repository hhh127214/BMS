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

4) **输入源是两个开关（v1.7），不是页面上的一个下拉框，也不是整站一个总闸。**
   两处都只在「设备管理」页：一端是「调度主站接入」（EMS 作从站），
   一端是「站内设备接入」（EMS 作主站）；每一端内部再分真实/模拟输入源。
   ★ 拆开的理由是对端本来就可以一头真一头假 —— 「设备已到货接真机、
     调度链路还没开通」是现场最常见的状态，一个总闸表达不了。
   ★ 采集器只认**下端**：切下端会写穿 device_conn 并落盘 active.conn；
     切上端写 upstream_link 与 active.upstream，采集器不看它（网关自己启动
     时读文件），所以采集中切上端是允许的，切下端必须先停。
   端侧进程读文件即可，不存在"平台以为连着 A、程序实际连着 B"的中间态。

5) **页面只认「有没有数据」，不认「仿真还是真实」。**
   其他页面是正式运行页面：数据来了就正常渲染，没来就指向设备管理。
   所以采集器的启停、数据源的选择，全部收在设备管理页 ——
   一个"仿真模式"的字眼都不许漂到别的页面上。

6) **sim 与 real 走同一个采集器、同一份实录契约。**
   都是 07/main_field.exe，差别只有：连谁、要不要 --control、
   谁先把设备替身拉起来。这让「接入上下端（真实或虚拟）之后
   其他页面就能正常运行」这句话是**结构性成立**的，不是靠分支拼出来的。
"""

from __future__ import annotations

import csv as _csv
import datetime
import io
import os
import socket
import sqlite3
import subprocess
import sys
import threading
import time
from typing import Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import emsdb  # noqa: E402
import importer  # noqa: E402

PROJECT_ROOT = os.path.dirname(emsdb.MODULE_ROOT)

# 「最后一次通知用户」也通知不出去时的兜底出口。
# 上面那三处 _persist 的语义是：把「链路为什么停了」写进库里，好让界面说出来。
# 可一旦**写库本身**失败，那个 except 就把整件事咽了 —— 界面会停在旧状态上，
# 用户看到的是「数据不动了，但哪儿都没说坏了」，排障方向全跑偏
# （2026-10-19 体检发现：operational 错误期间这里静默，与 §91 修的是同一类病灶，
#  §91 只管了「别把可恢复错误判死」，没管「连状态都写不进去」这最后一层）。
# 所以状态写不进去时必须留下痕迹：先 stderr，再落文件 —— 落文件是因为后端
# 常以窗口/后台方式拉起，stderr 经常没有接的人。
CRIT_LOG_MAX_B = 256 * 1024      # 超过就砍前半段，别把磁盘写满


def note_critical(tag: str, msg: str) -> None:
    """留下一条「界面说不出来」的记录。本函数自身**永远不抛异常**。"""
    line = "%s [%s] %s\n" % (datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                             tag, msg)
    try:
        sys.stderr.write(line)
        sys.stderr.flush()
    except Exception:
        pass
    try:
        p = os.path.join(emsdb.MODULE_ROOT, "data", "critical.log")
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if os.path.isfile(p) and os.path.getsize(p) > CRIT_LOG_MAX_B:
            with io.open(p, "r", encoding="utf-8", errors="replace") as f:
                tail = f.read()[-(CRIT_LOG_MAX_B // 2):]
            with io.open(p, "w", encoding="utf-8") as f:
                f.write(tail)
        with io.open(p, "a", encoding="utf-8") as f:
            f.write(line)
    except Exception:
        pass

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
M_DATA_SOURCE = "data_source_kind"
M_PEER_PID = "live_peer_pid"
M_PEER_TARGET = "live_peer_target"

# 实时引擎默认参数
DEFAULT_DT_S = 1.0          # 控制周期 = 墙钟节拍（1 秒一拍）
DEFAULT_LOG_EVERY = 10      # 每 10 拍写一行 → 10 s 粒度（全项目图表的默认粒度）
STALL_INTERVALS = 6         # 连续这么多个周期没长 → 判 stale

# 判"采集器起来了"的**兜底上限**（秒）—— 不是固定开销：CSV 写出第一拍就走。
# 现场源的失败早退是「连续 3 拍读不到」（约 3 s），上限要比它长一点才判得出来；
# 模型源参数错会立刻退出，1.2 s 足够。
COLLECTOR_UP_SIM_S = 3.6
COLLECTOR_UP_MODEL_S = 1.2


def _is_transient_db_error(e: BaseException) -> bool:
    """写库失败里**可能自己好**的那一部分。

    `sqlite3.OperationalError` 覆盖锁竞争、`attempt to write a readonly
    database`、磁盘瞬时 I/O —— 都是等一拍再试就可能成功的。

    ★ 代码类错误（ProgrammingError / IntegrityError / TypeError）再试多少次
      都还是错的，必须立刻停 —— 那种「停」才有信息量：它说的是「这里有 bug」，
      而不是「再等等」。把两类混成一句「实时链路失败」，排障方向就全跑了。
    """
    return isinstance(e, sqlite3.OperationalError)


class EngineError(Exception):
    """引擎相关的可预期错误，api.py 会把它转成 4xx。"""


# ---------------------------------------------------------------------
# 数据源清单
# ---------------------------------------------------------------------
def _exe(*parts: str) -> str:
    return os.path.join(PROJECT_ROOT, *parts)


SIM_SLAVE = _exe("13", "sim", "modbus_slave.py")
VENV_PY = _exe("13", ".venv", "Scripts", "python.exe")

# 模拟数据源的替身工况（与 13/sim/modbus_slave.py 的默认一致）
SIM_LOAD_KW = 380.0
SIM_PV_KW = 150.0

SOURCES = [
    {
        "id": "sim",
        "name": "模拟数据源 · 设备替身闭环（13/ 替身 → 07/ 主站）",
        "kind": "model",
        "exe": _exe("07", "build", "main_field.exe"),
        "available": True,
        "why": "",
        "control": True,
        "peer": True,
        "note": "下端连 13/ 的设备替身（模拟 PCS / BMS / 电表），"
                "07/ 主站对它跑闭环并写实录；上端由 P3/ 从站网关对模拟调度上送。"
                "★ 数据是模型算的，不是现场装置。",
    },
    {
        "id": "real",
        "name": "现场数据源 · 真实设备（只读监视）",
        "kind": "device",
        "exe": _exe("07", "build", "main_field.exe"),
        "available": True,
        "why": "",
        "control": False,
        "peer": False,
        "note": "下端连 active.conn 指定的真实设备，**只读、不下发任何指令**；"
                "上端连上级调度。数据来自真实装置。",
    },
    {
        "id": "model",
        "name": "模拟数据源 · 本机模型（降级）",
        "kind": "model",
        "exe": _exe("10", "build", "sim_live.exe"),
        "available": True,
        "why": "",
        "control": True,
        "peer": False,
        "note": "不接任何设备，10/ 按典型日曲线逐拍算出完整闭环。"
                "仅在本机起不来设备替身时作为降级路径。",
    },
]

# 设备管理页上可供选择的两种数据源（model 是降级路径，不给选）
DATA_SOURCES = ("sim", "real")
DATA_SOURCE_NAMES = {"sim": "模拟数据源", "real": "现场数据源"}


def source_of(sid: str) -> dict:
    for s in SOURCES:
        if s["id"] == sid:
            return s
    return SOURCES[0]


def get_data_source(conn) -> str:
    """下端（站内设备接入）当前的输入源。未设置过一律当 sim ——
    开箱即用，且它不会假装接了真机。

    ★ 这一层只管**下端**：上端（调度主站接入）的形态记在
      `upstream_link.kind` 上，采集器不看它（网关在自己启动时读
      `config/point-map/active.upstream`）。
    ★ 真相源是落盘文件 `active.conn` —— 采集器读的就是它。meta 只是同一件事
      的另一份登记，只在文件不在时兜底。两者不一致时**以文件为准**：
      反过来会出现「平台说模拟、采集器照着文件连了真机」，而两边都不报错。
    """
    try:
        import connconf
        path = connconf.conn_path()
        if os.path.isfile(path):
            k = str(connconf.load_file(path).get("source_kind") or "").strip()
            if k in DATA_SOURCES:
                return k
    except Exception:
        pass          # 文件坏了就退到 meta：启动时会由 _real_conn 给出明确报错
    k = (emsdb.get_meta(conn, M_DATA_SOURCE) or "").strip()
    return k if k in DATA_SOURCES else DATA_SOURCES[0]


def _loopback(host: str) -> bool:
    h = (host or "").strip().lower()
    return h in ("127.0.0.1", "localhost", "::1", "0.0.0.0")


def _python_with_pymodbus() -> tuple:
    """能 import pymodbus 的解释器。优先 13/.venv（构建脚本一直用它）。"""
    cands = [VENV_PY, sys.executable]
    last = ""
    for py in cands:
        if not py or not os.path.isfile(py):
            continue
        try:
            r = subprocess.run([py, "-c", "import pymodbus"],
                               capture_output=True, timeout=25)
            if r.returncode == 0:
                return py, ""
            last = (r.stderr or b"").decode("utf-8", "replace").strip().splitlines()
            last = last[-1] if last else ""
        except Exception as e:      # 解释器坏了 / 超时
            last = f"{type(e).__name__}: {e}"
    return "", (f"本机没有装了 pymodbus 的 Python（试过 13/.venv 与当前解释器）"
                + (f"：{last}" if last else ""))


def _wait_tcp(host: str, port: int, timeout: float, proc=None) -> tuple:
    """等端口可用。**判据是"能连上"，不是"进程还活着"** ——
    进程活着但没 listen（崩在绑定之前、端口被占）同样是起失败。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc is not None and proc.poll() is not None:
            return False, f"替身进程启动即退出（exit={proc.returncode}）"
        try:
            with socket.create_connection((host, int(port)), 0.6):
                return True, ""
        except OSError:
            time.sleep(0.15)
    return False, f"替身端口 {host}:{port} 在 {timeout:.0f} s 内没有起来"


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
        # 数据源顺带拉起的替身进程（模拟数据源才会有）。停止时按逆序回收。
        self.peers: list = []
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
            "data_source": get_data_source(conn),
            "data_source_name": DATA_SOURCE_NAMES.get(get_data_source(conn), ""),
            "peer_pid": int(_f(emsdb.get_meta(conn, M_PEER_PID), 0)),
            "peer_target": emsdb.get_meta(conn, M_PEER_TARGET) or "",
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
    def _sim_conn(self, conn) -> dict:
        """模拟数据源要连的那台替身。

        取库里第一台「形态=sim 且已启用」的设备；没有就退到第一台，再没有用默认。
        ★ 数据源选了模拟，下端就按替身算 —— 不让"数据源=模拟、某台设备=真实"
          这种组合存在：那会让采集器连到真机上跑闭环。

        ★ 兜底**只从已启用的台里挑**。2026-10-19 实测踩到：关机再开模拟源后
          6 台形态全是 real（「关闭模拟源」会把形态写回 real），于是落到 `rows[0]`
          = DEV-BMS-01，而那台 `enabled=0` **本来就不起替身** ⇒ 采集器报
          「端口 1802 没人应答」，面板 8/9，页面一行数据都没有。
          ★ 停用设备不该被选成采集目标：它的端口上**注定没人**。
        """
        import connconf
        rows = emsdb.query(conn, "SELECT * FROM device_conn ORDER BY device_id")
        pick = None
        for r in rows:
            if str(r.get("source_kind") or "real") == "sim" and int(r.get("enabled") or 0):
                pick = r
                break
        if pick is None:
            for r in rows:
                if int(r.get("enabled") or 0):
                    pick = r
                    break
        if pick is None and rows:
            pick = rows[0]
        raw = dict(pick) if pick else {"device_id": "DEV-BMS-01", "host": "127.0.0.1"}
        raw["source_kind"] = "sim"
        return connconf.normalize(raw)

    def _real_conn(self) -> dict:
        """现场数据源的前置条件：下端配置存在、且**形态是 real**。"""
        import connconf
        path = connconf.conn_path()
        if not os.path.isfile(path):
            raise EngineError(
                "现场数据源要求下端已填好接入参数，但 %s 不存在。\n"
                "先去「设备管理 → 下端接入」填设备地址 / 端口 / 从站号并保存。" % path)
        try:
            c = connconf.load_file(path)
        except Exception as e:
            raise EngineError(f"接入参数文件非法，拒绝启动：{e}")
        if str(c.get("source_kind") or "real") != "real":
            raise EngineError(
                "下端的接入形态是「模拟源」，与所选的「现场数据源」不一致。\n"
                "把数据源切到「模拟数据源」，或把下端改成真实源并填上设备地址。\n"
                "★ 不自动按数据源改写接入参数 —— 那会把「配置选错了」这件事藏起来。")
        return c

    def _start_peer(self, conn, sim: dict) -> tuple:
        """拉起 13/ 设备替身。返回 (ok, why, target_text)。"""
        host = str(sim.get("sim_host") or "127.0.0.1")
        port = int(sim.get("sim_port") or 1502)
        target = f"{host}:{port}"
        if not _loopback(host):
            # 替身在别人机器上：不去拉，直接连。如实写清"不是平台拉起的"。
            return True, "", f"{target}（外部替身，平台未拉起）"
        if not os.path.isfile(SIM_SLAVE):
            return False, f"设备替身脚本不存在：{SIM_SLAVE}", ""
        py, why = _python_with_pymodbus()
        if not py:
            return False, why, ""
        cmd = [py, SIM_SLAVE, "--port", str(port),
               "--load", repr(SIM_LOAD_KW), "--pv", repr(SIM_PV_KW)]
        try:
            proc = subprocess.Popen(
                cmd, cwd=PROJECT_ROOT,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                creationflags=_no_window_flags())
        except OSError as e:
            return False, f"拉不起设备替身：{e}", ""
        ok, why2 = _wait_tcp("127.0.0.1", port, 12.0, proc)
        if not ok:
            try:
                proc.kill()
            except Exception:
                pass
            return False, why2, ""
        with self._lock:
            self.peers.append(("设备替身（13/ modbus_slave.py）", proc))
        self._persist(conn, live_peer_pid=proc.pid, live_peer_target=target)
        conn.commit()
        return True, "", f"{target}（平台已拉起设备替身）"

    def _kill_peers(self) -> list:
        killed = []
        with self._lock:
            peers = list(self.peers)
            self.peers = []
        for name, p in reversed(peers):
            if p is None or p.poll() is not None:
                continue
            try:
                p.terminate()
                try:
                    p.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    p.kill()
                killed.append(name)
            except Exception:
                pass
        return killed

    @staticmethod
    def _wait_first_row(proc, csv_path: str, timeout: float) -> bool:
        """等采集器写出第一拍。**看证据，不赌时间**。

        True  = 它确实在跑（CSV 已经有内容了）；
        False = 进程已经退出（调用方据此去取日志报错）。

        ★ 等满了但进程还活着也返回 True —— 与原逻辑一致：原来也是睡够之后
          看 `poll()`，活着就算起来了。这里只是不再把整段时间**白睡掉**。
        """
        deadline = time.time() + timeout
        while True:
            if proc.poll() is not None:
                return False
            try:
                if os.path.getsize(csv_path) > 0:
                    return True
            except OSError:
                pass
            if time.time() >= deadline:
                return proc.poll() is None
            time.sleep(0.12)

    def start(self, conn, source: str = "", username: str = "",
              dt_s: float = 0.0, log_every: int = 0,
              steps: int = 0, spawn_peer: bool = True) -> dict:
        kind = get_data_source(conn)
        sid = (str(source or "").strip() or kind)
        if sid not in {s["id"] for s in SOURCES}:
            raise EngineError(
                f"未知数据源「{sid}」（可选："
                + " / ".join(s["id"] for s in SOURCES) + "）")

        cur = self.snapshot(conn)
        if cur["running"]:
            raise EngineError(
                f"采集已经在运行（启动于 {cur['started_at']}，"
                f"已入库 {cur['rows']} 行）。先停止再启动。")

        degraded = ""
        sim = None
        peer_text = ""
        if sid == "sim":
            sim = self._sim_conn(conn)
            if spawn_peer:
                ok, why, peer_text = self._start_peer(conn, sim)
                if not ok:
                    # 降级：本机模型。**原因留在 live_error 里**，界面照实显示。
                    degraded = f"设备替身起不来，已降级为本机模型：{why}"
                    sid = "model"
                    sim = None
            else:
                # ★ 替身由**模拟链路**（simproc）统一托管：这里只核对端口，
                #   不再拉一个 —— 否则同一个端口上会有两个替身抢着 bind，
                #   后者启动即退出，看起来像"替身坏了"。
                #   `spawn_peer=False` 只用于 simproc；单独点「启动采集」时仍是
                #   默认 True（自己拉一个，一条命令跑通）。
                tgt = f"{sim.get('sim_host') or '127.0.0.1'}:{int(sim.get('sim_port') or 1502)}"
                ok, why = _wait_tcp("127.0.0.1", int(sim.get("sim_port") or 1502), 3.0)
                if not ok:
                    raise EngineError(
                        "下端模拟源端口没人应答：%s\n%s\n"
                        "模拟链路（设备替身）没有起来 —— 先在「模拟输入源」里"
                        "点「换一个进程」，或改成现场数据源。" % (tgt, why))
                peer_text = f"{tgt}（由模拟链路托管，平台未单拉）"
                self._persist(conn, live_peer_target=tgt)
                conn.commit()
        elif sid == "real":
            c = self._real_conn()  # 形态不一致就直接拒，不悄悄连替身
            import connconf
            tgt = connconf.target(c)
            ok, why = _wait_tcp(tgt["host"], tgt["port"], 2.5)
            if not ok:
                raise EngineError(
                    "现场设备连不上：%s:%s（从站 %s）。\n%s\n"
                    "先去现场确认设备上电、网线通、IP/端口/从站号与厂家手册一致；"
                    "\n用 13\\build\\modbus_probe.exe --host %s --port %s 单独探一下。"
                    % (tgt["host"], tgt["port"], tgt.get("unit_id") or 1, why,
                       tgt["host"], tgt["port"]))

        src = source_of(sid)
        if not src["available"]:
            raise EngineError(f"数据源「{src['name']}」不可用：{src['why']}")
        exe = src["exe"]
        if not os.path.isfile(exe):
            raise EngineError(f"数据源可执行文件不存在：{exe}\n先编译对应模块。")

        # 07/ 每拍写一行（没有 log_every），所以 10 s 粒度靠 --dt 本身；
        # 10/ 是"每 log_every 拍写一行"。两者都保持 10 s 一行的观感。
        if sid == "model":
            dt = float(dt_s) if dt_s else DEFAULT_DT_S
            log_every = int(log_every) if log_every else DEFAULT_LOG_EVERY
            n_steps = 0
        else:
            dt = float(dt_s) if dt_s else 1.0
            log_every = 1
            n_steps = int(steps) if steps else 864000   # 10 天，够跑

        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        csv_path = os.path.join(self._live_dir(), f"live_{stamp}.csv")
        log_path = os.path.join(self._live_dir(), f"collector_{stamp}.log")

        if sid == "model":
            cmd = [exe, "--record", csv_path,
                   "--dt", repr(float(dt)),
                   "--log-every", str(int(log_every)),
                   "--duration-s", "0",
                   "--progress-every", "0",
                   "--quiet"]
        else:
            cmd = [exe, "--device", "modbus",
                   "--dt", repr(float(dt)),
                   "--steps", str(int(n_steps)),
                   "--record", csv_path,
                   # ★ 上端 IEC104 的数据来源（2026-10-19 补）：
                   #   不写的话，07/ 只落实录 CSV，RT_DB 的 MEAS.* 停在初值，
                   #   网关照发 24 点、品质 GOOD —— 主站看到"通道正常、值全是 0"。
                   #   放在采集进程里写，是因为 RT_DB 的纪律是「一个进程一条连接」，
                   #   同一个值也只该有一个写入者（详见 07/src/rtdb/rtdb_mirror.h）。
                   "--mirror-rtdb"]
            if src["control"]:
                cmd.append("--control")
            if sid == "sim" and sim is not None:
                cmd += ["--host", str(sim.get("sim_host") or "127.0.0.1"),
                        "--port", str(int(sim.get("sim_port") or 1502)),
                        "--unit", str(int(sim.get("unit_id") or 1))]

        try:
            logf = open(log_path, "wb")
        except OSError:
            logf = None
        try:
            proc = subprocess.Popen(
                cmd, cwd=PROJECT_ROOT,
                stdout=logf or subprocess.DEVNULL,
                stderr=subprocess.STDOUT,
                creationflags=_no_window_flags())
        except OSError as e:
            if logf:
                logf.close()
            self._kill_peers()
            raise EngineError(f"拉不起采集进程：{e}")
        finally:
            if logf:
                logf.close()

        # ★ 采集器"起得来"不等于"跑得起来"：接不上设备、点表非法、enabled=0
        #   都会让 07/ 在 1 s 内退出。等一小会儿再判，把它的输出去取回来 ——
        #   否则界面上只会看到"已启动、然后一直没有数据"，排障方向全跑偏。
        #
        # ★ 这里原来是**死等** 3.6 s（现场源）/ 1.2 s（模型源）。它是"点一下
        #   启用模拟源要等十几秒"里最大的一块 —— 2026-10-19 实测：链路启动
        #   10.3 s，其中采集器段 3.67 s，而 3.6 s 就是这一行睡掉的。
        #   改成**看证据**：CSV 写出第一拍 ⇒ 它确实在跑，立刻走；
        #   进程自己退了 ⇒ 立刻报。两个数现在只是**兜底上限**，不是固定开销。
        self._wait_first_row(
            proc, csv_path,
            COLLECTOR_UP_MODEL_S if sid == "model" else COLLECTOR_UP_SIM_S)
        if proc.poll() is not None:
            tail = _tail_text(log_path, 12)
            self._kill_peers()
            self._persist(conn, live_state="failed",
                          live_error=f"采集进程启动即退出（exit={proc.returncode}）",
                          live_csv=csv_path)
            conn.commit()
            raise EngineError(
                "采集进程启动即退出（exit=%s）。它的输出：\n%s"
                % (proc.returncode, tail or "（无输出）"))

        with self._lock:
            self.proc = proc
            self.csv_path = csv_path
            self.byte_pos = 0
            self.header_seen = False
            self.dt_s = float(dt)
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
            live_dt=float(dt), live_log_every=int(log_every),
            live_source=src["id"], live_error=degraded, live_wall_s=0.0,
        )
        conn.commit()
        self._spawn_tailer()
        import auth
        if username:
            auth.audit(conn, username, "engine_live_start", "engine",
                       f"source={src['id']} dt={dt} log_every={log_every} "
                       f"peer={peer_text or '-'} "
                       f"csv={os.path.basename(csv_path)}")
        snap = self.snapshot(conn)
        snap["degraded"] = degraded
        return snap

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
        peers = self._kill_peers()

        with self._lock:
            self._thread = None
            self.proc = None
        self._persist(conn, live_state="stopped", live_peer_pid=0,
                      live_peer_target="")
        conn.commit()
        import auth
        if username:
            auth.audit(conn, username, "engine_live_stop", "engine",
                       f"rows={n} peers={len(peers)}"
                       + ("（进程由外部管理，未持有句柄）" if unattached else ""))
        snap = self.snapshot(conn)
        snap["stopped_rows"] = n
        snap["killed"] = killed
        snap["unattached"] = unattached
        snap["peers_stopped"] = peers
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
        write_err = 0           # 连续写库失败次数（只数「可能自己好」的那类）
        while not self._stop_evt.is_set():
            grew = 0
            # ★ 先看采集进程还在不在。只看"文件没长"会把两件事混成一句
            #   "可能已退出"：进程崩了 / 进程在跑但没数据。前者要立刻报，
            #   后者等 STALL_INTERVALS 再说 —— 排障方向完全不同。
            with self._lock:
                p = self.proc
            if p is not None and p.poll() is not None:
                try:
                    conn = self._connect()
                    try:
                        self._persist(
                            conn, live_state="failed",
                            live_error=f"采集进程已退出（exit={p.returncode}）")
                        conn.commit()
                    finally:
                        conn.close()
                except Exception as _e:
                    note_critical("live.failed",
                                  f"采集进程已退出（exit={p.returncode}），"
                                  f"但这条状态也写不进库（界面因此看不到原因）："
                                  f"{type(_e).__name__}: {_e}")
                break
            try:
                conn = self._connect()
                try:
                    grew = self.poll_once(conn)
                    if grew:
                        idle = 0
                        write_err = 0      # 入库又通了 → 之前那次确实是瞬时的
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
                # ★ 不是每一种异常都该让整条实时链路**永久**停摆。
                #   数据库瞬时不可写（锁 / readonly / 磁盘瞬时 I/O）是**可恢复**的：
                #   一上来就 break，系统会停在「采集进程还在跑、实录还在长、
                #   但一行都进不了库」的假活状态，而且不会自愈 —— 界面上表现为
                #   「数据越等越旧，却没有一个地方说入库已经断了」
                #   （2026-10-19 实测：OperationalError: attempt to write a
                #    readonly database，一次就把整条链路判死，持续 5 分钟无人知）。
                #   保住线程继续重试，才有自己好的可能；失败次数要写在状态里，
                #   否则「一直重试」看起来和「一直在跑」没有区别。
                if _is_transient_db_error(e):
                    write_err += 1
                    try:
                        conn = self._connect()
                        try:
                            self._persist(
                                conn, live_state="stale",
                                live_error=f"写库失败，已连续重试 {write_err} 次"
                                           f"（数据停在最后一次成功入库的时刻）："
                                           f"{type(e).__name__}: {e}")
                            conn.commit()
                        finally:
                            conn.close()
                    except Exception as _e2:
                        note_critical("live.stale",
                                      f"已连续写库失败 {write_err} 次，"
                                      f"且「失败」这条状态也写不进去："
                                      f"{type(e).__name__}: {e} → "
                                      f"{type(_e2).__name__}: {_e2}")
                    self._stop_evt.wait(interval)
                    continue
                try:
                    conn = self._connect()
                    try:
                        self._persist(conn, live_state="failed",
                                      live_error=f"{type(e).__name__}: {e}")
                        conn.commit()
                    finally:
                        conn.close()
                except Exception as _e:
                    note_critical("live.failed",
                                  f"链路因 {type(e).__name__}: {e} 停摆，"
                                  f"但这条状态写不进库（界面看不到原因）："
                                  f"{type(_e).__name__}: {_e}")
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

        # 读到新字节就更新「最后一次长起来」的时刻 —— 它判的是**采集进程**
        #   还活不活，跟这一批有没有写进库无关。
        with self._lock:
            self.last_growth_ts = time.time()

        if recs:
            importer.import_live_rows(
                conn, recs, LIVE_SCENARIO,
                "运行模式实时数据（模型源）"
                if source_of(self.source)["kind"] == "model"
                else "运行模式实时数据（现场设备）",
                _rel_source(path))

        # ★ 断点只在**写成功之后**才推进。
        #   原来它在写库之前就推进了：写库一失败（哪怕只是数据库瞬时不可写），
        #   这批行就再也不会被读到 —— 数据静默丢失，而且没有任何报错。
        #   放在写之后，失败时下一拍原样重放；入库走 INSERT OR REPLACE 是幂等的，
        #   重放不会产生重复行。
        with self._lock:
            self.byte_pos += consumed
            self.rows = self.rows + len(recs)
            n_rows_total = self.rows
        # ★ 有数据长起来就把上一次的 live_error 清掉：它已经被事实推翻了。
        #   不清的话，降级提示会一直挂在横幅上，而横幅的判据是
        #   `running && !error` —— 数据明明在进，界面却一直报异常。
        self._persist(conn, live_state="running", live_rows=n_rows_total,
                      live_imported_at=_now(), live_error="")
        conn.commit()
        return len(recs)


def _tail_text(path: str, n: int = 12) -> str:
    """取日志文件最后 n 行（失败时给排障用）。"""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = [ln.rstrip() for ln in f if ln.strip()]
        return "\n".join(lines[-n:])
    except OSError:
        return ""


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


def picked_sim(conn) -> dict:
    """采集器会连的那台替身（模拟数据源的**唯一解析点**，对外暴露）。

    为什么要有这个包装：模拟链路（simproc）要在面板上标"哪一台真被采集"，
    而选设备的规则只能有一份 —— 另抄一份就会出现「面板说采 A、采集器连 B」，
    两边都不报错。所以统一走 `LiveEngine._sim_conn()`。
    """
    return LIVE._sim_conn(conn)



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
        # ★ 数据源（设备管理页那个开关）与 engine（数据维度）是两件事：
        #   engine 说"数据是正在发生还是离线算出来的"，
        #   data_source 说"正在发生的数据是谁给的"。页面只认 scenario，不认它俩。
        "data_source": get_data_source(conn),
        "data_source_name": DATA_SOURCE_NAMES.get(get_data_source(conn), ""),
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
        "note": "运行模式的数据源由「设备管理 → 数据源」决定："
                "模拟数据源 = 设备替身闭环，现场数据源 = 真实设备只读监视。"
                "两者走同一个 07/ 采集器、同一份实录契约，页面不需要区分。",
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
