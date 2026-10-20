# -*- coding: utf-8 -*-
"""14/ — 模拟链路的进程编排（把「一条链」当成一个可启停的整体）

=====================================================================
为什么需要这一层
=====================================================================
v1.7 的「模拟输入源」只做了**配置**：把下端的 `sim_host` / `sim_port` 与上端的
`master_host` 填好，然后把表染成紫色。但配置不是数据 —— 填完之后页面上仍然
一行实时值都没有，而界面上看不出还差什么。

差的是**五个进程**。这条链一环扣一环，缺任何一环都是"屏幕上什么都没有"：

    ① 实时库初始化器  11/build/rtdb_initializer.exe    （常驻，见下面的 ★）
              ↓ 建出 RT_DB 共享内存段
    ② 设备替身 × N    13/sim/modbus_slave.py --port <该台模拟口>
              ↓ Modbus TCP（一台一个端口，端口 = port_base + 序号）
    ③ 采集器          07/build/main_field.exe --device modbus
              ↓ 写 CSV 实录（→ 平台入库 step_record）+ 写 RT_DB
    ④ 上端从站网关    P3/build/iec104_gateway.exe
              ↓ IEC 60870-5-104 上送（listen_port，默认 2404）
    ⑤ 模拟主站        P3/build/iec104_master.exe

所以「启用模拟源」= 改配置 **+ 把这条链拉起来**；「关闭」= 收掉这条链 **+ 配置
切回真实源**。只做前一半，就是 v1.7~v1.9 那个"启用了但没数据"的样子。

=====================================================================
★ ①为什么必须常驻（不能"跑完就退"）
=====================================================================
Windows 上 RT_DB 段是**页面文件映射对象**：最后一个句柄关闭即销毁。
`07/build/ems_rt_db_init.exe` 是短命工具（建完就退），它退出时段就没了 ——
于是网关起来时看到的是"共享内存不存在"。`11/build/rtdb_initializer.exe`
就是那个"建完不退、一直握着"的常驻版本。

⇒ 本模块把初始化器当**子进程**养着，它活多久链路就活多久。

=====================================================================
★ ③为什么采集器不自己 Popen
=====================================================================
采集器是**唯一入库通道**：`07/` 写 CSV → `engine.LIVE` 的尾随线程增量导入
`step_record(scenario='live')` → 其他页面才有图。自己 Popen 一个 `07/` 只能
得到一份没人读的 CSV。

⇒ 这里一律走 `engine.LIVE.start(source="sim", spawn_peer=False)`：
   · 复用它的入库、状态机、失败早退判定（"起得来 ≠ 跑得起来"）；
   · `spawn_peer=False` 是因为**替身由本模块统一起**（见下）。

=====================================================================
★ ②为什么替身由本模块起、而不是让 engine 起
=====================================================================
`engine._start_peer()` 只给**一台**设备拉替身（采集器也就只连这一台）。
本模块要把**每一台启用设备的端口都真的有人听** —— 否则面板上那 6 行端口
是"写在纸上"的，用户按表去 telnet 会发现全是空的。

⇒ 本模块为每台启用设备起一个替身，engine 那边只保留"端口通不通"的核对。

=====================================================================
★ 诚实的边界：采集器一次只连一台
=====================================================================
`active.conn` 是**单台**文件，`07/ --device modbus` 也是单目标。所以哪怕起了
6 个替身，实际被采集的只有 1 台（`engine.picked_sim()` 那一台）。

⇒ 面板上必须把"哪一台在被采"标出来，其余的写「已监听 · 未被采集」。
   这不是美化问题：写成"6 台都在跑"就是在撒谎，而这正是本项目最忌讳的
   「看起来在工作」。
"""

from __future__ import annotations

import datetime
import json
import os
import socket
import subprocess
import sys
import threading
import time
from typing import Dict, List

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import emsdb      # noqa: E402
import engine     # noqa: E402
import simconf    # noqa: E402
import uplinkconf  # noqa: E402

# ---------------------------------------------------------------------
# 角色
# ---------------------------------------------------------------------
ROLE_RTDB = "rtdb"
ROLE_SLAVE = "slave"
ROLE_COLLECTOR = "collector"
ROLE_GATEWAY = "gateway"
ROLE_MASTER = "master"

ROLE_NAMES = {
    ROLE_RTDB: "实时库初始化器",
    ROLE_SLAVE: "设备替身",
    ROLE_COLLECTOR: "采集器",
    ROLE_GATEWAY: "上端从站网关",
    ROLE_MASTER: "模拟调度主站",
}

PROJECT = engine.PROJECT_ROOT
RTDB_INIT = os.path.join(PROJECT, "11", "build", "rtdb_initializer.exe")
GATEWAY_EXE = os.path.join(PROJECT, "P3", "build", "iec104_gateway.exe")
MASTER_EXE = os.path.join(PROJECT, "P3", "build", "iec104_master.exe")

# 初始化器握住段的时长。给一个足够大的数，停止时由 stop() terminate。
RTDB_HOLD_S = 86400

# 网关周期上报（秒）。比默认 5 s 快，联调时不用干等。
GATEWAY_PERIODIC_S = 2

# 模拟主站跑多久。★ 这里**不能写 0**：网关的 `--seconds 0` 是"一直跑"，
# 但主站的循环条件是 `now - t0 < seconds*1000`，0 会让它一拍都不跑、立刻退出
# （2026-10-19 实测：面板上显示"连不上从站"，而日志里其实已经收到 ASDU 了）。
# 给一天，停止时由 stop() terminate。
MASTER_RUN_S = 86400

# ---- 启动探测窗口（秒）----
# ★ 这些数字是**整组共用**的，不是"每个进程各等这么久"。
#   替身 5 台时，旧写法是 5 × 2.5 = 12.5 s 的 sleep；现在整组只付一次。
# 设 EMS_CHAIN_TIMING=1 时，start/stop 会把分段耗时（ms）放进 status 的
# timing_ms —— 排查"点一下要等十几秒"这种问题用的，默认不开。
CHAIN_TIMING = os.environ.get("EMS_CHAIN_TIMING") == "1"

SLAVE_LISTEN_S = 3.0       # 替身：等端口 listen 的上限（连得上就提前返回；
                           #   同一窗口内也抓"启动即退出"，不再另开一段）
GATEWAY_LISTEN_S = 2.5     # 等网关 2404 listen 的上限（同上）
MASTER_PROBE_S = 1.0       # 主站是客户端、没有 listen 端口，只能看它退没退


def _now() -> str:
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def _log_dir() -> str:
    d = os.path.join(emsdb.MODULE_ROOT, "data", "live", "sim")
    os.makedirs(d, exist_ok=True)
    return d


def _tail(path: str, n: int = 3) -> str:
    """取日志最后几行 —— 进程"启动即退出"时，原因全在它自己的输出里。"""
    try:
        with open(path, "rb") as f:
            text = f.read().decode("utf-8", "replace")
        lines = [ln.strip() for ln in text.strip().splitlines() if ln.strip()]
        return " / ".join(lines[-n:])
    except OSError:
        return ""


def _db_path_of(conn) -> str:
    """从一个连接问出它连的是哪个文件 —— 后台线程要按它自己的句柄重开。

    ★ 为什么要问而不是用 `DEFAULT_DB_PATH`：服务可能带了 `--db <别的库>`。
      后台线程连错库，等于把链路写到一个没人读的地方。
    """
    try:
        row = conn.execute("PRAGMA database_list").fetchone()
        if row and row[2]:
            return row[2]
    except Exception:
        pass
    return emsdb.DEFAULT_DB_PATH


def _port_open(port, host: str = "127.0.0.1", timeout: float = 0.15) -> bool:
    """这个端口现在连得上吗 —— 判断"服务已经在 listen"的**直接**判据。

    ★ 比 sleep 一段固定时间好在哪：sleep 是在赌"这么久够它起来了"，
      赌错了就要么白等、要么误判。连一次端口是**问它本人**。
    """
    try:
        with socket.create_connection((host, int(port)), timeout=timeout):
            return True
    except (OSError, ValueError, TypeError):
        return False


def _no_spawn() -> bool:
    """自检 / 离线环境用：只算「该起什么」，**不真的拉进程**。

    为什么用环境变量而不是加参数：调用方是 api.py 的两个 handler，为测试多穿
    一层参数会把"测试专用的东西"渗进生产签名里；而自检的 `call()` 是**同进程**
    调 dispatch，环境变量最省事，也最容易在测试里开/关。

    ★ 默认关闭（= 真拉进程）。只有显式设成 1/true/on/yes 才不拉。
    """
    return os.environ.get("EMS_SIMPROC_OFF", "").strip().lower() in (
        "1", "true", "on", "yes")


def _fake(role: str, key: str, label: str, target: str, note: str) -> Dict:
    return {"role": role, "key": key, "label": label, "target": target,
            "proc": None, "log": "", "note": note, "skip": True}


# ---------------------------------------------------------------------
# 平台重启后接管遗留进程
# ---------------------------------------------------------------------
# 为什么需要：进程是**后端拉起来的子进程**，后端一重启，`_rows` 里的 Popen 对象
# 就没了，但那些进程还在跑（替身在听、采集在写、网关在上送）。此时如果面板说
# "链路未启动"，而数据明明在涨 —— 就是本项目最忌讳的那种自相矛盾。
# 更糟的是「换一个进程」会再拉一套，两个替身抢同一个端口。
#
# 办法：把 PID 记进 meta，重启后按 PID 重建一张"只认 pid 的进程表"，
# 停止时用 taskkill 收掉。判断存活用一次 `tasklist`（3 秒缓存）。
M_CHAIN = "sim_chain_procs"

_PID_CACHE = {"t": 0.0, "pids": set()}


def _live_pids() -> set:
    now = time.time()
    if now - _PID_CACHE["t"] > 3.0:
        pids = set()
        try:
            out = subprocess.run(["tasklist", "/NH", "/FO", "CSV"],
                                 capture_output=True, timeout=6)
            text = out.stdout.decode("mbcs", "replace")
            for line in text.splitlines():
                parts = line.split('","')
                if len(parts) > 1:
                    try:
                        pids.add(int(parts[1]))
                    except ValueError:
                        pass
        except Exception:
            pids = _PID_CACHE["pids"]      # 取不到就沿用上一次，别把表清空
        _PID_CACHE.update(t=now, pids=pids)
    return _PID_CACHE["pids"]


class _PidProc:
    """没有 Popen 对象时的替身：只认 pid。"""

    def __init__(self, pid: int) -> None:
        self.pid = int(pid)

    def poll(self):
        return None if self.pid in _live_pids() else 1

    def terminate(self) -> None:
        try:
            subprocess.run(["taskkill", "/F", "/PID", str(self.pid)],
                           capture_output=True, timeout=10)
        except Exception:
            pass

    kill = terminate

    def wait(self, timeout=None) -> int:
        return 0


def collector_alive(conn=None) -> bool:
    """采集器在不在跑。

    ★ 判据要用 engine 的状态机，不是看有没有子进程对象：服务器重启后
      本模块的 proc 是 None，但采集器可能还在跑（engine 会 attach 上去）。
      conn 为 None 时只能退到"看进程对象"，仅供没有库句柄的场合。
    """
    if conn is not None:
        try:
            return bool(engine.LIVE.snapshot(conn).get("running"))
        except Exception:
            pass
    p = getattr(engine.LIVE, "proc", None)
    return p is not None and p.poll() is None


class SimChain:
    """模拟链路。一个进程一份实例（模块级 CHAIN）。"""

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._rows: List[Dict] = []
        self.started_at = ""
        self.error = ""
        self.preset_name = ""
        self.attached = False       # True = 进程是上次后端拉起的，本进程只有 pid
        self.timing = {}            # 分段耗时（仅 EMS_CHAIN_TIMING=1 时填）
        self.job = {                # 后台执行的那一次操作（见 submit）
            "running": False, "action": "", "username": "", "detail": "",
            "started_at": "", "finished_at": "", "ok": None, "error": "",
        }

    # -----------------------------------------------------------------
    # 内部：PID 落库 / 重启后接管
    # -----------------------------------------------------------------
    def _persist_rows(self, conn) -> None:
        items = [{
            "role": r["role"], "key": r["key"], "label": r["label"],
            "target": r["target"],
            "pid": (r["proc"].pid if r.get("proc") is not None else 0),
            "skip": bool(r.get("skip")),
        } for r in self._rows]
        emsdb.set_meta(conn, M_CHAIN, json.dumps(items, ensure_ascii=False))
        emsdb.set_meta(conn, M_CHAIN + "_at", self.started_at or "")
        emsdb.set_meta(conn, M_CHAIN + "_preset", self.preset_name or "")
        conn.commit()

    def attach(self, conn) -> bool:
        """后端重启后，按 meta 里的 PID 把进程表重建出来。

        为什么必须做：进程是后端拉起的子进程，后端一重启 `_rows` 就空了，
        但替身/采集/网关都还在跑。此时面板若说"链路未启动"，而数据在涨，
        就是自相矛盾；再点「换一个进程」还会拉起第二套，抢同一个端口。
        """
        with self._lock:
            if self._rows or conn is None:
                return False
            raw = emsdb.get_meta(conn, M_CHAIN) or ""
            if not raw:
                return False
            try:
                items = json.loads(raw)
            except Exception:
                return False
            if not isinstance(items, list) or not items:
                return False
            alive_pids = _live_pids()
            rows, any_alive = [], False
            for it in items:
                role = it.get("role")
                pid = int(it.get("pid") or 0)
                proc = None
                if pid > 0 and role != ROLE_COLLECTOR:
                    proc = _PidProc(pid)
                    any_alive = any_alive or (pid in alive_pids)
                rows.append({"role": role, "key": it.get("key"),
                             "label": it.get("label"),
                             "target": it.get("target"), "proc": proc,
                             "log": "", "note": "", "skip": bool(it.get("skip"))})
            if not any_alive and not collector_alive(conn):
                # 一个都不在了（重启过机器 / 全崩了）：别拿一张全红的表骗人
                emsdb.set_meta(conn, M_CHAIN, "[]")
                conn.commit()
                return False
            self._rows = rows
            self.started_at = emsdb.get_meta(conn, M_CHAIN + "_at") or ""
            self.preset_name = emsdb.get_meta(conn, M_CHAIN + "_preset") or ""
            self.attached = True
            return True

    # -----------------------------------------------------------------
    # 内部
    # -----------------------------------------------------------------
    def _spawn(self, role: str, key: str, label: str, target: str,
               cmd: List[str], log_name: str) -> Dict:
        if _no_spawn():
            return _fake(role, key, label, target, "（自检模式）未真正拉起进程")
        path = os.path.join(_log_dir(), log_name)
        try:
            logf = open(path, "wb")
        except OSError:
            logf = None
        try:
            proc = subprocess.Popen(
                cmd, cwd=PROJECT,
                stdout=logf if logf else subprocess.DEVNULL,
                stderr=subprocess.STDOUT if logf else subprocess.DEVNULL,
                creationflags=engine._no_window_flags())
        except OSError as e:
            return {"role": role, "key": key, "label": label, "target": target,
                    "proc": None, "log": "", "note": f"拉不起来：{e}"}
        finally:
            if logf:
                logf.close()
        return {"role": role, "key": key, "label": label, "target": target,
                "proc": proc, "log": path, "note": ""}

    @staticmethod
    def _wait(proc, seconds: float) -> bool:
        """等一小会儿看它还在不在。True = 还活着。"""
        if proc is None:
            return False
        deadline = time.time() + seconds
        while time.time() < deadline:
            if proc.poll() is not None:
                return False
            time.sleep(0.12)
        return proc.poll() is None

    # -----------------------------------------------------------------
    # 整组等待（★ 不要用"每个进程各等一次"）
    #
    # 原来 5 台替身各 `_wait(2.5)`，串行付了 12.5 s —— 而"启动即退出"和
    # "端口 listen"这两件事对整组进程是**同时发生**的，一个等待窗口足够
    # 覆盖全部。串行等只是把同一段 2.5 s 重复付了 N 遍（2026-10-19 实测：
    # 一次「启用模拟源」HTTP 请求卡 25.4 s 才返回，用户看到的就是"点了没
    # 反应，过一会才有反应"）。
    # -----------------------------------------------------------------
    @staticmethod
    def _wait_ready(rows: List[Dict], ports: List[int],
                    seconds: float, step: float = 0.1) -> List[Dict]:
        """一次性等一组进程。**返回已经退出的那些行**（正常时是空表）。

        ports 给了就多一个提前返回的理由：所有端口都连得上 ⇒ 服务已在
        listen，不必再等。ports 为空时只看进程有没有退（客户端进程没有
        listen 端口，只能这么判）。
        """
        live = [r for r in rows if r.get("proc") is not None]
        if not live:
            return []
        deadline = time.time() + seconds
        while True:
            exited = [r for r in live if r["proc"].poll() is not None]
            if exited:
                # 有一个退了，被它连累的可能正跟着退 —— 再给一拍，别漏报
                time.sleep(0.2)
                return [r for r in live if r["proc"].poll() is not None]
            if ports and all(_port_open(p) for p in ports):
                return []
            if time.time() >= deadline:
                return []
            time.sleep(step)

    @staticmethod
    def _alive(row: Dict) -> bool:
        p = row.get("proc")
        return p is not None and p.poll() is None

    def _row(self, role: str, key: str, label: str, target: str,
             note: str = "", skip: bool = False) -> Dict:
        """skip=True = **本来就不该起**（该台停用 / 程序未构建）。

        ★ 为什么要有这个标记：把它算进分母，"9/10" 看起来像"掉了一个"，
          而真相是"那台本来就没启"。两种情况的处置完全不同 ——
          前者要去查日志，后者什么都不用做。
        """
        return {"role": role, "key": key, "label": label, "target": target,
                "proc": None, "log": "", "note": note, "skip": skip}

    # -----------------------------------------------------------------
    # 状态
    # -----------------------------------------------------------------
    def running(self, conn=None) -> bool:
        self.attach(conn)
        with self._lock:
            if any(self._alive(r) for r in self._rows):
                return True
        return collector_alive(conn)

    def status(self, conn=None) -> Dict:
        """给界面看的进程清单。**以进程真实存活为准**，不看"曾经起过"。"""
        self.attach(conn)
        with self._lock:
            rows = list(self._rows)
        live = collector_alive(conn)
        out, up, skipped = [], 0, 0
        for r in rows:
            alive = live if r["role"] == ROLE_COLLECTOR else self._alive(r)
            if alive:
                up += 1
            if r.get("skip"):
                skipped += 1
            out.append({
                "role": r["role"],
                "role_name": ROLE_NAMES.get(r["role"], r["role"]),
                "key": r["key"],
                "label": r["label"],
                "target": r["target"],
                "pid": (r["proc"].pid if r.get("proc") is not None else None),
                "alive": alive,
                "skip": bool(r.get("skip")),
                "note": r.get("note") or "",
            })
        # expected = 应该起得来的本数（停用 / 未构建的不算分母）
        expected = len(out) - skipped
        return {
            "running": up > 0,
            "started_at": self.started_at,
            "preset_name": self.preset_name,
            "error": self.error,
            "attached": self.attached,
            "procs": out,
            "counts": {"up": up, "expected": expected, "total": len(out)},
            # 后台那一次操作的进度 —— 前端轮询它来决定"进度条该不该收"。
            "job": self.job_state(),
            # 排查"点一下要等十几秒"用的分段耗时；默认是个空表，界面不读它。
            "timing_ms": dict(self.timing) if CHAIN_TIMING else {},
        }

    # -----------------------------------------------------------------
    # 启动
    # -----------------------------------------------------------------
    def start(self, conn, username: str = "") -> Dict:
        """把整条链拉起来。已经在跑就直接返回现状（不重复起）。"""
        with self._lock:
            self.attach(conn)          # 上次后端拉起的那些，先认领回来
            # 判"已经整条在跑"要同时看**采集**与**替身**：
            # 只有网关活着、采集早退出，是"半截链路"——那种状态必须收干净重来，
            # 否则面板会显示"运行中"，而页面上依然一行数据都没有。
            slaves_up = any(self._alive(r) for r in self._rows
                            if r["role"] == ROLE_SLAVE)
            if collector_alive(conn) and slaves_up:
                return self.status(conn)
            if slaves_up or collector_alive(conn):
                self.stop(conn, username=username)   # RLock 可重入

            idx = simconf.index_read(conn)
            preset = simconf.preset_of(idx)
            self._rows = []
            self.error = ""
            self.preset_name = preset["name"]
            self.started_at = _now()
            notes: List[str] = []
            t0 = time.time()
            # 保留 stop_ 段（本次若先停过），只清 start 自己的
            self.timing = {k: v for k, v in self.timing.items()
                           if k.startswith("stop_")}

            def mark(name: str) -> None:
                if CHAIN_TIMING:
                    self.timing[name] = int((time.time() - t0) * 1000)

            # ---- ① 实时库初始化器（常驻） ----
            if os.path.isfile(RTDB_INIT):
                row = self._spawn(ROLE_RTDB, "rtdb", "RT_DB 初始化器",
                                  "共享内存段",
                                  [RTDB_INIT, "--seconds", str(RTDB_HOLD_S)],
                                  "rtdb_init.log")
                self._rows.append(row)
                rtdb_row = row
            else:
                rtdb_row = None
                self._rows.append(self._row(
                    ROLE_RTDB, "rtdb", "RT_DB 初始化器", "共享内存段",
                    "未构建（11/build/rtdb_initializer.exe 不存在）", skip=True))
                notes.append("实时库初始化器未构建")

            # ---- ② 设备替身：每台启用设备一个 ----
            en = {r["device_id"]: int(r.get("enabled") or 0)
                  for r in emsdb.query(
                      conn, "SELECT device_id, enabled FROM device_conn")}
            py, why_py = engine._python_with_pymodbus()
            slaves: List[Dict] = []          # 真 spawn 出来的替身（统一探测用）
            slave_ports: List[int] = []
            for it in simconf.plan_downstream(conn, idx):
                did = it["device_id"]
                port = int(it["sim_port"])
                target = "%s:%d" % (it["sim_host"], port)
                if not en.get(did, 1):
                    self._rows.append(self._row(
                        ROLE_SLAVE, did, "替身 %s" % did, target,
                        "该台已停用（enabled=0）⇒ 不起替身", skip=True))
                    continue
                if not py:
                    self._rows.append(self._row(
                        ROLE_SLAVE, did, "替身 %s" % did, target, why_py))
                    notes.append(why_py)
                    continue
                row = self._spawn(
                    ROLE_SLAVE, did, "替身 %s" % did, target,
                    [py, engine.SIM_SLAVE, "--port", str(port),
                     "--load", repr(engine.SIM_LOAD_KW),
                     "--pv", repr(engine.SIM_PV_KW)],
                    "slave_%s.log" % did.replace("/", "_"))
                self._rows.append(row)
                if row["proc"] is not None:
                    slaves.append(row)
                    slave_ports.append(port)

            # ---- ②′ 统一探测：初始化器 + 全部替身 ----
            # ★ 这些进程是**同时**起的，"启动即退出"也几乎同时发生，所以一个
            #   窗口覆盖整组就够 —— 逐个 `_wait(2.5)` 只是把同一段等待重复付
            #   N 遍（5 台替身 = 12.5 s 纯 sleep，用户看到的就是"点了没反应"）。
            # ★ 一个窗口同时做两件事：抓"启动即退出" + 等端口 listen。
            #   分成两段各等一次，等于把同一段等待付两遍。
            probe = ([rtdb_row] if rtdb_row is not None
                     and rtdb_row.get("proc") is not None else []) + slaves
            for r in self._wait_ready(probe, slave_ports, SLAVE_LISTEN_S):
                t = _tail(r.get("log", ""), 2)
                if r["role"] == ROLE_RTDB:
                    # 段已被另一个常驻实例握着也是"能用"—— 不判死，
                    # 只把原因写在行上；网关随后会自己说清楚能不能打开。
                    r["note"] = "初始化器已退出（段可能已被别的常驻实例握着）" \
                                + ("：" + t if t else "")
                else:
                    r["note"] = "启动即退出：" + (t or "（无输出）")
                    notes.append("替身 %s 起不来" % r["key"])
            mark("listen")

            # ---- ③ 采集器（走 engine.LIVE，唯一入库通道） ----
            cole = self._row(ROLE_COLLECTOR, "collector", "采集器（07/）", "")
            self._rows.append(cole)
            try:
                picked = engine.picked_sim(conn)
                cole["target"] = "%s:%s" % (
                    picked.get("sim_host") or "127.0.0.1",
                    picked.get("sim_port") or "?")
                cole["label"] = "采集器（07/ · 采 %s）" % picked.get("device_id", "?")
                if _no_spawn():
                    cole["note"] = "（自检模式）未真正拉起进程"
                else:
                    snap = engine.LIVE.start(conn, source="sim", username=username,
                                             spawn_peer=False)
                    cole["note"] = "运行中，已入库 %s 行" % snap.get("rows", 0)
                    # 记下 pid：后端重启后要靠它把采集器也收掉
                    # （engine 那边没句柄时只会如实说 unattached，不会去杀）
                    cole["proc"] = getattr(engine.LIVE, "proc", None)
            except Exception as e:                    # EngineError / 其它
                cole["note"] = "没起来：%s" % e
                self.error = "采集器没起来：%s" % e
            mark("collector")

            # ---- ④ 上端从站网关 ----
            if os.path.isfile(GATEWAY_EXE):
                row = self._spawn(ROLE_GATEWAY, "gateway", "IEC104 从站网关",
                                  "listen_port（见 active.upstream）",
                                  [GATEWAY_EXE, "--seconds", "0",
                                   "--periodic", str(GATEWAY_PERIODIC_S)],
                                  "gateway.log")
                self._rows.append(row)
                if row["proc"] is not None:
                    # ★ 等 2404 **真的在 listen**，而不是赌"2 秒够它起来了"。
                    #   连得上就提前走，连不上才等满上限（那时进程多半已退出）。
                    try:
                        lp = int(uplinkconf.load_file().get("listen_port")
                                 or uplinkconf.PORT_DEFAULT)
                    except Exception:
                        lp = uplinkconf.PORT_DEFAULT
                    for r in self._wait_ready([row], [lp], GATEWAY_LISTEN_S):
                        t = _tail(r.get("log", ""), 3)
                        r["note"] = "启动即退出：" + (t or "（无输出）")
                        self.error = self.error or \
                            "上端网关没起来：%s" % (t or "见日志")
            else:
                self._rows.append(self._row(
                    ROLE_GATEWAY, "gateway", "IEC104 从站网关", "2404",
                    "未构建（P3/build/iec104_gateway.exe 不存在）", skip=True))
            mark("gateway")

            # ---- ⑤ 模拟主站（调度侧对端） ----
            if os.path.isfile(MASTER_EXE):
                row = self._spawn(ROLE_MASTER, "master", "模拟调度主站",
                                  simconf.UP_MASTER,
                                  [MASTER_EXE, "--seconds", str(MASTER_RUN_S),
                                   "--gi-interval", "10"],
                                  "master.log")
                self._rows.append(row)
                # 主站是客户端：连不上会立刻退出。给一点缓冲，
                # 别让"从站刚 listen、主站早退了半拍"看起来像网关坏了。
                # （客户端没有 listen 端口可探，只能看它退没退。）
                if row["proc"] is not None:
                    for r in self._wait_ready([row], [], MASTER_PROBE_S):
                        t = _tail(r.get("log", ""), 2)
                        r["note"] = "已退出：" + (t or "（无输出）")
            else:
                self._rows.append(self._row(
                    ROLE_MASTER, "master", "模拟调度主站", simconf.UP_MASTER,
                    "未构建（P3/build/iec104_master.exe 不存在）", skip=True))

            mark("master")
            if notes:
                self.error = self.error or "；".join(notes[:3])
            self._persist_rows(conn)
            return self.status(conn)

    # -----------------------------------------------------------------
    # 停止（逆序：主站 → 网关 → 采集 → 替身 → 初始化器）
    # -----------------------------------------------------------------
    def stop(self, conn=None, username: str = "") -> Dict:
        t0 = time.time()
        self.timing = {}

        def mark(name: str) -> None:
            if CHAIN_TIMING:
                self.timing["stop_" + name] = int((time.time() - t0) * 1000)
        with self._lock:
            order = [ROLE_MASTER, ROLE_GATEWAY, ROLE_COLLECTOR,
                     ROLE_SLAVE, ROLE_RTDB]
            rows = sorted(self._rows, key=lambda r: order.index(r["role"])
                          if r["role"] in order else 99)

            # 采集器由 engine 管：先停它（它还在写 CSV/RT_DB 时别急着收替身）
            if conn is not None and collector_alive(conn):
                try:
                    engine.LIVE.stop(conn, username=username)
                except Exception as e:
                    self.error = "停采集时报错：%s" % e
            mark("collector")

            for r in rows:
                p = r.get("proc")
                if p is None or p.poll() is not None:
                    continue
                try:
                    p.terminate()
                    try:
                        p.wait(timeout=4.0)
                    except subprocess.TimeoutExpired:
                        p.kill()
                except Exception:
                    pass

            self._rows = rows
            self.attached = False
            if conn is not None:
                # 表已经全收了，meta 里的 pid 记录必须一起清掉 ——
                # 留着它，下次启后端 attach() 会认领一堆早已死掉的 pid。
                emsdb.set_meta(conn, M_CHAIN, "[]")
                conn.commit()
            mark("procs")
        return self.status(conn)

    # -----------------------------------------------------------------
    # 后台执行 —— 让 POST 立刻回来（§92b）
    #
    # 为什么要有这一层：就算把 §92 的 19 s 压到 6.9 s，用户点下去还是得盯着
    # 一个转圈的请求等 7 s —— 期间"这个请求还在不在、会不会超时"完全不可见。
    # 所以改成：**POST 收下就算**（几十毫秒返回），进程操作丢给后台线程，
    # 前端轮询 `chain.job` 看它好了没有。
    #
    # ★ 代价必须写进返回值里：**返回成功 ≠ 链路已经起来**。
    #   `job.ok` 才是结果；`job.running` 期间只是"在办"。
    # -----------------------------------------------------------------
    def job_state(self) -> Dict:
        with self._lock:
            return dict(self.job)

    def submit(self, conn, action: str, username: str = "") -> Dict:
        """把 start / stop / restart 交给后台线程，**立即返回** job 状态。

        已经有一个在跑 ⇒ 返回 None（调用方转 409）。
        ★ 为什么**不排队**：链路操作不是可以累加的东西 —— 排队两次，
          用户要等两倍时间，而第二次的结果还会把第一次盖掉。

        ★ 自检模式（`_no_spawn()`）下**同步跑完**再返回：自检本来就不拉进程
          （毫秒级），而异步会让"操作有没有进审计"变成竞态 —— 断言不该摇。
        """
        if action not in ("start", "stop", "restart"):
            raise ValueError("action 只能是 start / stop / restart")
        with self._lock:
            if self.job.get("running"):
                return None
            self.job = {"running": True, "action": action,
                        "username": username, "detail": "",
                        "started_at": _now(), "finished_at": "",
                        "ok": None, "error": ""}
        db_path = _db_path_of(conn)
        if _no_spawn():
            self._run_job(db_path, action, username)
            return self.job_state()
        t = threading.Thread(target=self._run_job,
                             args=(db_path, action, username),
                             daemon=True, name="simchain-" + action)
        t.start()
        return self.job_state()

    def _run_job(self, db_path: str, action: str, username: str) -> None:
        """后台线程的主体。★ 自己开连接 —— sqlite 连接不能跨线程用。"""
        ok, err, detail = True, "", ""
        conn = None
        try:
            conn = emsdb.connect(db_path)
            st = {"start": self.start, "stop": self.stop,
                  "restart": self.restart}[action](conn, username=username)
            c = (st or {}).get("counts", {}) or {}
            detail = "%s up=%s/%s" % (action, c.get("up"), c.get("expected"))
            err = (st or {}).get("error") or ""
            ok = not err
            import auth
            auth.audit(conn, username or "-", "sim_proc", "模拟输入源",
                       detail + (" err=%s" % err if err else ""))
            conn.commit()
        except Exception as e:
            ok, err = False, "%s 没办成：%s" % (action, e)
        finally:
            if conn is not None:
                try:
                    conn.close()
                except Exception:
                    pass
        with self._lock:
            self.job.update(running=False, finished_at=_now(),
                            ok=ok, error=err, detail=detail)

    def restart(self, conn, username: str = "") -> Dict:
        """「换一个进程」：按**当前这一套**重新拉起整条链。

        ★ 与「换一套」的分工：「换一套」只改配置（表上写的端口段），
          进程还跑在旧套上；「换一个进程」才让进程跟着表走。
          两者分开是有意为之 —— 换套不必打断正在看的采集，
          但界面必须提示"进程还没跟"，否则就是两套并行、都自称正常。
        """
        self.stop(conn, username=username)
        time.sleep(0.4)          # 让端口真的释放
        return self.start(conn, username=username)


CHAIN = SimChain()
