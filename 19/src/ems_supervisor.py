#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""19/ 现场部署 · 进程守护器（EMS 三进程按序拉起 + 存活检测 + 异常重启）。

背景（`docs/交接/下一步工作交接说明.md` §3.3 第 1、4 条）
--------------------------------------------------------
> 1. 落地部署形态：现在只有 `.exe`，没有服务化。
> 4. 开机自启与进程守护：EMS 挂了要能自己起来。

**为什么用 Python 标准库写守护器，而不是直接上 `sc.exe` / systemd**
-----------------------------------------------------------------
① 本项目是**零第三方依赖**（约定 §1）。Python 标准库跨 Windows / Linux 同一份；
   `sc.exe` 只能包 Windows、systemd 只能包 Linux。
② **真·Windows 服务不是"注册一下"就能成**：`sc.exe create` 把一个**控制台程序**
   注册成服务后，SCM 会等它调用 `StartServiceCtrlDispatcher` 报 `SERVICE_RUNNING`；
   不报，**约 30 秒后 SCM 直接杀掉进程**，而 `sc query` 里表现是 `STOPPED` / 反复重启。
   Python 侧不做 SCM 握手就只能靠第三方包装器（破坏零依赖）。
   本模块把这层留给"决定换路线时"（见 `scripts/install_service.bat` 的说明与
   `docs/README.md` §部署形态），主路线走**计划任务**。
③ 守护逻辑真正难的不是"起进程"，是**按正确顺序起 + 谁挂了要连带重启谁**。
   这部分无论外面包什么都要自己写，所以它才是主体。

三条部署顺序约束（`11/docs/README.md` §2.1，都是踩过的静默失败）
--------------------------------------------------------------
① **初始化器必须最先跑**：它的 `reset=true` 会把整个段 memset（段级连接计数一起清零）。
   顺序错了 → "连接数与真实进程数脱节"。
② **初始化器不能"跑完就退"**：Windows 下段是页面文件支撑的文件映射对象，
   最后一个句柄关闭即销毁。所以它必须常驻 —— 本守护器把它当**常驻保活进程**看待，
   它一旦退出，**下游全部作废**（段没了），必须**级联重启**。
③ **EMS 侧要等设备侧先发布 `CFG.*`**：EMS 在 `attach_device()` 时会立刻读一次；
   设备没发布就读到点表默认值（变压器 250 kVA）→ "EMS 拿错误的一次侧参数算安全边界"。
   本守护器用 `depends_on` + **就绪探测**（进程自己打的横幅）来保证这一条，
   而不是靠 `timeout /t 2` 这种"猜时间"（见下）。

★ 就绪探测为什么不能退化成 `sleep`
---------------------------------
`11/scripts/run_integration.bat` 用的是 `timeout /t 1 /nobreak`。在演示里够用，
在现场是**错的**：慢盘 / 冷启动 / 杀软扫描下，"1 秒后设备就绪"不成立；
而一旦不成立，EMS 读到的是错的一次侧参数，**进程活着、日志干净、数据是错的**。
所以本守护器每个进程带一条 `ready_pattern`，命中**进程自己打印的横幅**才算就绪：
    rtdb_initializer → `[INITIALIZER] segment`
    device_side      → `[DEVICE] connected`
    ems_side         → `[EMS] connected`
超时未命中 → 判定为**启动失败**，按退避重启（不是"就当它好了"）。

★ 关于 stdin 看门狗（上一轮实测的坑，本文件默认**关闭**）
------------------------------------------------------
"stdin 读到 EOF 就退出"是命令行工具常见的看门狗写法。**在本项目里它会让守护器
秒退**：Windows 上 `isatty(NUL)` 返回**真**，而把子进程的 stdin 重定向到 NUL
（`stdin=subprocess.DEVNULL` / `>nul <nul`）后，stdin 立即可读且读到 EOF。
实测现象：打印完 READY 就死，端口从来没 LISTENING，退出码还是 0。

所以本文件的看门狗**只由显式环境变量打开**：
    EMS_SUPERVISOR_STDIN_WATCHDOG=1   → 启用（前台调试时用）
默认不启用，且**不看 `isatty`** —— 那正是骗人的那一位。
测试里有一对断言钉住这一点（默认 + NUL stdin 必须活满全程；显式打开则按 EOF 退出）。

命令
----
    python ems_supervisor.py --root <root> --check
    python ems_supervisor.py --root <root> run [--duration S]
    python ems_supervisor.py --root <root> --dump-config

进程清单来自 `<root>/conf/supervisor.json`；不存在时用内置默认（见 `--dump-config`）。
相对 `exe` 路径按 `--root` 解析 —— 现场把产物放到 `<root>/bin/` 即可。

退出码：0 正常 / 1 用法或 preflight 失败 / 2 运行期有进程异常退出且重启耗尽。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time

DEFAULT_CONFIG_REL = os.path.join("conf", "supervisor.json")
DEFAULT_LOG_REL = "log"


def default_config() -> dict:
    """内置默认：**11/ 的三进程联调形态**。

    现场安装时把 `exe` 换成安装根下的真实路径（默认 `bin/`），其余字段一般不用动。
    参数说明：
      ready_pattern   就绪判据（正则，匹配进程 stdout，见文件头"就绪探测"）
      ready_timeout_s 就绪超时；超时按启动失败处理
      depends_on      依赖的进程名（必须先 ready）
      essential       是否常驻（本项目三个都是 true；挂了就重启）
      cwd             工作目录（相对 --root）
    """
    return {
        "poll_s": 0.5,
        "backoff": {"base_s": 1.0, "factor": 2.0, "max_s": 30.0, "stable_s": 60.0},
        "log_dir": DEFAULT_LOG_REL,
        "processes": [
            {
                "name": "rtdb_initializer",
                "exe": "11/build/rtdb_initializer.exe",
                "args": ["--seconds", "864000"],
                "cwd": ".",
                "ready_pattern": r"\[INITIALIZER\] segment",
                "ready_timeout_s": 15.0,
                "depends_on": [],
                "essential": True,
            },
            {
                "name": "device_side",
                "exe": "11/build/device_side.exe",
                "args": ["--steps", "864000", "--dt", "0.1", "--speed", "1",
                         "--report", "19/build/device_report.txt"],
                "cwd": ".",
                "ready_pattern": r"\[DEVICE\] connected",
                "ready_timeout_s": 20.0,
                "depends_on": ["rtdb_initializer"],
                "essential": True,
            },
            {
                "name": "ems_side",
                "exe": "11/build/ems_side.exe",
                "args": ["--steps", "864000", "--dt", "0.1", "--speed", "1",
                         "--report", "19/build/ems_report.txt"],
                "cwd": ".",
                "ready_pattern": r"\[EMS\] connected",
                "ready_timeout_s": 30.0,
                "depends_on": ["device_side"],
                "essential": True,
            },
        ],
    }


def merge_config(base: dict, over: dict) -> dict:
    out = dict(base)
    for k, v in (over or {}).items():
        if k == "processes":
            continue
        if k == "backoff" and isinstance(v, dict):
            bo = dict(base.get("backoff", {}))
            bo.update(v)
            out["backoff"] = bo
        else:
            out[k] = v
    if isinstance(over, dict) and isinstance(over.get("processes"), list):
        procs = []
        for p in over["processes"]:
            d = {"args": [], "cwd": ".", "depends_on": [], "essential": True,
                 "ready_timeout_s": 15.0, "ready_pattern": ""}
            d.update(p)
            procs.append(d)
        out["processes"] = procs
    return out


# =====================================================================
class Proc:
    def __init__(self, spec: dict, root: str) -> None:
        self.name: str = spec["name"]
        self.exe: str = spec["exe"]
        self.args: list[str] = list(spec.get("args") or [])
        self.cwd: str = os.path.join(root, spec.get("cwd") or ".")
        self.ready_pattern: str = spec.get("ready_pattern") or ""
        self.ready_timeout_s: float = float(spec.get("ready_timeout_s", 15.0))
        self.depends_on: list[str] = list(spec.get("depends_on") or [])
        self.essential: bool = bool(spec.get("essential", True))

        # 运行态
        self.proc: subprocess.Popen | None = None
        self.dirty: bool = True          # 需要（重新）启动
        self.ready: bool = False
        self.ever_ready: bool = False    # 曾经就绪过（停机时 ready 会被清零，summary 用这个）
        self.starts: int = 0
        self.exits: int = 0
        self.fail_streak: int = 0
        self.next_start: float = 0.0
        self.started_at: float | None = None
        self.ready_at: float | None = None
        self.deadline: float = 0.0
        self.log_path: str = ""
        self.read_offset: int = 0

    def resolve_exe(self, root: str) -> str:
        e = self.exe
        if not os.path.isabs(e):
            e = os.path.join(root, e)
        return os.path.normpath(e)

    def cmdline(self, root: str) -> list[str]:
        return [self.resolve_exe(root)] + self.args


class Supervisor:
    def __init__(self, root: str, cfg: dict, out=None,
                 log_dir: str | None = None) -> None:
        self.root = os.path.abspath(root)
        self.cfg = cfg
        self.procs = [Proc(s, self.root) for s in cfg["processes"]]
        self.by_name = {p.name: p for p in self.procs}
        self.poll_s = float(cfg.get("poll_s", 0.5))
        bo = cfg.get("backoff", {})
        self.bo_base = float(bo.get("base_s", 1.0))
        self.bo_factor = float(bo.get("factor", 2.0))
        self.bo_max = float(bo.get("max_s", 30.0))
        self.stable_s = float(bo.get("stable_s", 60.0))
        self.log_dir = os.path.join(self.root, log_dir or cfg.get("log_dir") or DEFAULT_LOG_REL)
        self.out = out if out is not None else sys.stdout
        self.supervisor_log = os.path.join(self.log_dir, "supervisor.log")
        self.t0 = time.monotonic()
        self._stop = False
        self._watchdog_stop = False
        self._lock = threading.Lock()
        self._watch_thread: threading.Thread | None = None

    # ---- 日志 ----

    def _log(self, line: str) -> None:
        line = line.rstrip("\n")
        with self._lock:
            print(line, file=self.out, flush=True)
            try:
                os.makedirs(self.log_dir, exist_ok=True)
                with open(self.supervisor_log, "a", encoding="utf-8", newline="\n") as f:
                    f.write(line + "\n")
            except OSError:
                pass

    def _t(self) -> float:
        return time.monotonic() - self.t0

    # ---- preflight ----

    def preflight(self) -> list[str]:
        problems: list[str] = []
        names = [p.name for p in self.procs]
        if len(set(names)) != len(names):
            problems.append("进程名重复")
        for p in self.procs:
            if not p.name:
                problems.append("存在无名进程")
            for d in p.depends_on:
                if d not in self.by_name:
                    problems.append(f"{p.name}: 依赖不存在的进程 {d}")
                if d == p.name:
                    problems.append(f"{p.name}: 依赖自己")
            exe = p.resolve_exe(self.root)
            if not os.path.isfile(exe):
                problems.append(f"{p.name}: 可执行文件不存在 {exe}")
            if p.ready_pattern:
                try:
                    re.compile(p.ready_pattern)
                except re.error as e:
                    problems.append(f"{p.name}: ready_pattern 非法（{e}）")
            if not os.path.isdir(p.cwd):
                problems.append(f"{p.name}: cwd 不存在 {p.cwd}")
        # 环检测
        color: dict[str, int] = {}

        def dfs(n: str, stack: list[str]) -> None:
            color[n] = 1
            for d in self.by_name[n].depends_on:
                if d not in self.by_name:
                    continue
                if color.get(d) == 1:
                    problems.append("依赖成环：" + " → ".join(stack + [n, d]))
                elif color.get(d, 0) == 0:
                    dfs(d, stack + [n])
            color[n] = 2

        for n in names:
            if color.get(n, 0) == 0:
                dfs(n, [])
        return problems

    def plan_text(self) -> str:
        lines = ["  启动顺序（依赖必须先就绪）："]
        for i, p in enumerate(self.procs, 1):
            dep = ",".join(p.depends_on) or "-"
            lines.append(f"   {i}. {p.name:<20} deps={dep:<20} "
                         f"ready_timeout={p.ready_timeout_s:g}s")
            lines.append(f"        exe = {p.resolve_exe(self.root)}")
        lines.append(f"  退避：base={self.bo_base:g}s ×{self.bo_factor:g} "
                     f"cap={self.bo_max:g}s（稳定 {self.stable_s:g}s 后清零）")
        lines.append(f"  日志：{self.log_dir}")
        return "\n".join(lines)

    # ---- 生命周期 ----

    def backoff(self, streak: int) -> float:
        n = max(1, streak)
        return min(self.bo_max, self.bo_base * (self.bo_factor ** (n - 1)))

    def ensure_dirs(self) -> None:
        os.makedirs(self.log_dir, exist_ok=True)

    def _start(self, p: Proc, now: float) -> None:
        self.ensure_dirs()
        p.log_path = os.path.join(self.log_dir, f"{p.name}.out.log")
        # ★ 子进程的 stdin 给 DEVNULL 是**子进程**的事：三个 worker 都不读 stdin。
        #   这与"守护器自己的 stdin 被重定向到 NUL"是两回事，后者会让看门狗自杀
        #   （见文件头）。别把这条当成"stdin 重定向很危险"的依据。
        f = open(p.log_path, "a", encoding="utf-8", errors="replace", newline="")
        p.read_offset = f.tell()
        try:
            p.proc = subprocess.Popen(
                p.cmdline(self.root), cwd=p.cwd,
                stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT,
            )
        except OSError as e:
            f.close()
            p.proc = None
            p.fail_streak += 1
            p.next_start = now + self.backoff(p.fail_streak)
            self._log(f"[SUP] t={self._t():.3f} start_failed name={p.name} err={e}")
            return
        f.close()
        p.starts += 1
        p.started_at = now
        p.ready = False
        p.ready_at = None
        p.deadline = now + p.ready_timeout_s
        p.dirty = False
        self._log(f"[SUP] t={self._t():.3f} start name={p.name} pid={p.proc.pid} "
                  f"attempt={p.starts}")

    def _kill(self, p: Proc, why: str) -> None:
        if p.proc is None:
            return
        self._log(f"[SUP] t={self._t():.3f} kill name={p.name} pid={p.proc.pid} "
                  f"why={why}")
        try:
            p.proc.terminate()
            try:
                p.proc.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                p.proc.kill()
                p.proc.wait(timeout=3.0)
        except OSError:
            pass
        p.proc = None
        p.ready = False

    def _mark_dirty(self, p: Proc, now: float, streak: int | None = None) -> None:
        p.dirty = True
        p.ready = False
        if streak is not None:
            p.fail_streak = streak
        p.next_start = now + self.backoff(p.fail_streak)

    def _read_new_output(self, p: Proc) -> str:
        if not p.log_path or not os.path.isfile(p.log_path):
            return ""
        try:
            with open(p.log_path, "r", encoding="utf-8", errors="replace", newline="") as f:
                f.seek(p.read_offset)
                chunk = f.read()
                p.read_offset = f.tell()
            return chunk
        except OSError:
            return ""

    # ---- 主循环 ----

    def run(self, duration_s: float | None = None) -> dict:
        problems = self.preflight()
        if problems:
            for x in problems:
                self._log(f"[SUP] PREFLIGHT_FAIL {x}")
            return {"preflight_failed": True, "problems": problems}
        self._log(f"[SUP] t=0.000 supervise start root={self.root} "
                  f"procs={len(self.procs)} poll={self.poll_s:g}s")
        self._install_signals()
        self._start_watchdog()

        stop_reason = "duration"
        try:
            while True:
                now = time.monotonic()
                if self._stop:
                    stop_reason = "stop_requested"
                    break
                if duration_s is not None and (now - self.t0) >= duration_s:
                    stop_reason = "duration"
                    break
                self._reap(now)
                self._cascade(now)
                self._drive(now)
                self._probe(now)
                time.sleep(self.poll_s)
        except KeyboardInterrupt:  # pragma: no cover - 交互场景
            stop_reason = "interrupt"
        finally:
            for p in reversed(self.procs):
                self._kill(p, "shutdown")
            self._log(f"[SUP] t={self._t():.3f} stop reason={stop_reason}")
        self._watchdog_stop = True
        return self.summary(stop_reason)

    def _reap(self, now: float) -> None:
        for p in self.procs:
            if p.proc is None:
                continue
            rc = p.proc.poll()
            if rc is None:
                continue
            alive = (now - (p.started_at or now))
            was_ready = p.ready
            p.proc = None
            p.exits += 1
            # 稳定运行过再挂 → 重新从第 1 级退避开始（不是"永久指数增长"）；
            # 连续快速挂 → 累加，避免"崩溃-重启"打满 CPU。
            if was_ready and alive > self.stable_s:
                streak = 1
            else:
                streak = p.fail_streak + 1
            self._log(f"[SUP] t={self._t():.3f} exit name={p.name} rc={rc} "
                      f"alive={alive:.2f}s streak={streak}")
            self._mark_dirty(p, now, streak)

    def _cascade(self, now: float) -> None:
        """依赖不满足 → 下游作废（含传递闭包）。

        ★ 这是"初始化器不能跑完就退"那条约束的落地：段没了，EMS/设备继续跑
          只会读到死段或点表默认值 —— **进程活着但数据是错的**，比直接挂更危险。
        """
        changed = True
        while changed:
            changed = False
            for p in self.procs:
                if p.dirty:
                    continue
                for d in p.depends_on:
                    dp = self.by_name.get(d)
                    if dp is None:
                        continue
                    if dp.dirty or not dp.ready:
                        self._log(f"[SUP] t={self._t():.3f} cascade_dirty "
                                  f"name={p.name} because={d}")
                        if p.proc is not None:
                            self._kill(p, f"dep {d} not ready")
                        p.dirty = True
                        p.ready = False
                        p.fail_streak = 0
                        p.next_start = now
                        changed = True
                        break

    def _drive(self, now: float) -> None:
        for p in self.procs:
            if not p.dirty or p.proc is not None:
                continue
            if now < p.next_start:
                continue
            missing = [d for d in p.depends_on
                       if (self.by_name.get(d) is None
                           or not self.by_name[d].ready)]
            if missing:
                continue
            self._start(p, now)

    def _probe(self, now: float) -> None:
        for p in self.procs:
            if p.proc is None:
                continue
            if not p.ready:
                chunk = self._read_new_output(p)
                if p.ready_pattern and re.search(p.ready_pattern, chunk):
                    p.ready = True
                    p.ever_ready = True
                    p.ready_at = now
                    self._log(f"[SUP] t={self._t():.3f} ready name={p.name} "
                              f"in={now - (p.started_at or now):.2f}s")
                elif now > p.deadline:
                    # 超时未就绪 = 启动失败。**不能**当它好了。
                    self._log(f"[SUP] t={self._t():.3f} ready_timeout name={p.name} "
                              f"after={p.ready_timeout_s:g}s")
                    self._kill(p, "ready timeout")
                    self._mark_dirty(p, now, p.fail_streak + 1)
            else:
                if p.fail_streak and p.ready_at is not None \
                        and (now - p.ready_at) > self.stable_s:
                    self._log(f"[SUP] t={self._t():.3f} stabilized name={p.name}")
                    p.fail_streak = 0

    def summary(self, stop_reason: str = "") -> dict:
        return {
            "stop_reason": stop_reason,
            "starts": {p.name: p.starts for p in self.procs},
            "exits": {p.name: p.exits for p in self.procs},
            "ready": {p.name: p.ever_ready for p in self.procs},
            "restarts": {p.name: max(0, p.starts - 1) for p in self.procs},
        }

    # ---- 信号与看门狗 ----

    def _install_signals(self) -> None:
        def handler(signum, _frame):
            self._log(f"[SUP] signal {signum} → 请求停止")
            self._stop = True
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                signal.signal(sig, handler)
            except (ValueError, OSError):  # 非主线程 / 平台不支持
                pass

    def _start_watchdog(self) -> None:
        """stdin EOF 看门狗 —— **默认关闭**，只由环境变量显式打开。

        为什么默认关闭：见文件头。`isatty(NUL)` 在 Windows 上为真，
        而 stdin 指向 NUL 时读到的就是 EOF → 守护器"打印完 READY 就死"。
        """
        flag = os.environ.get("EMS_SUPERVISOR_STDIN_WATCHDOG", "").strip().lower()
        if flag not in ("1", "yes", "true", "on", "stdin"):
            return
        self._log(f"[SUP] stdin watchdog ENABLED (EMS_SUPERVISOR_STDIN_WATCHDOG={flag})")

        def loop():
            try:
                while not self._watchdog_stop:
                    b = sys.stdin.buffer.read(1)
                    if not b:
                        self._log("[SUP] stdin EOF → 请求停止（看门狗已启用）")
                        self._stop = True
                        return
            except (ValueError, OSError, AttributeError):
                self._stop = True

        self._watch_thread = threading.Thread(target=loop, daemon=True,
                                              name="stdin-watchdog")
        self._watch_thread.start()


# =====================================================================
def load_config(root: str, path: str | None) -> dict:
    base = default_config()
    p = path or os.path.join(root, DEFAULT_CONFIG_REL)
    if os.path.isfile(p):
        with open(p, "r", encoding="utf-8-sig") as f:
            over = json.load(f)
        return merge_config(base, over)
    return base


def write_default_config(root: str, path: str | None = None) -> str:
    p = path or os.path.join(root, DEFAULT_CONFIG_REL)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(default_config(), ensure_ascii=False, indent=2) + "\n")
    return p


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="19/ 现场部署进程守护器")
    ap.add_argument("--root", default=os.environ.get("EMS_SITE_ROOT", "."),
                    help="安装根目录（默认 EMS_SITE_ROOT 或当前目录）")
    ap.add_argument("--config", default=None, help="进程清单 JSON（默认 conf/supervisor.json）")
    ap.add_argument("--log-dir", default=None)
    ap.add_argument("--dump-config", action="store_true",
                    help="打印内置默认进程清单并退出")
    ap.add_argument("--write-config", action="store_true",
                    help="把内置默认进程清单写到 conf/supervisor.json")
    sub = ap.add_subparsers(dest="cmd")
    p_run = sub.add_parser("run", help="跑守护器")
    p_run.add_argument("--duration", type=float, default=None,
                       help="只跑 N 秒（测试/演练用；默认一直跑）")
    sub.add_parser("check", help="只做 preflight 与启动顺序检查")
    sub.add_parser("plan", help="打印启动计划")

    args = ap.parse_args(argv)
    root = os.path.abspath(args.root)

    if args.dump_config:
        print(json.dumps(default_config(), ensure_ascii=False, indent=2))
        return 0
    if args.write_config:
        print(f"[OK] 已写出 {write_default_config(root, args.config)}")
        return 0

    try:
        cfg = load_config(root, args.config)
    except (OSError, ValueError) as e:
        print(f"[FAIL] 读取进程清单失败：{e}")
        return 1

    sup = Supervisor(root, cfg, log_dir=args.log_dir)
    cmd = args.cmd or "check"

    if cmd == "plan":
        print(f"安装根：{root}")
        print(sup.plan_text())
        return 0

    if cmd == "check":
        problems = sup.preflight()
        print(f"安装根：{root}")
        print(sup.plan_text())
        if problems:
            print(f"[CHECK FAIL] {len(problems)} 项：")
            for x in problems:
                print("  -", x)
            return 1
        print("[CHECK OK] 进程清单可用（可执行文件齐、依赖不成环）")
        return 0

    if cmd == "run":
        res = sup.run(duration_s=args.duration)
        if res.get("preflight_failed"):
            print("[RUN FAIL] preflight 未通过")
            return 1
        print(f"[RUN DONE] stop={res['stop_reason']} starts={res['starts']} "
              f"exits={res['exits']}")
        # 有进程在一次都没成功稳定运行的情况下退出过 → 判为运行期失败
        bad = [n for n, k in res["restarts"].items() if k and not res["ready"].get(n)]
        return 0 if not bad else 2

    return 1


if __name__ == "__main__":
    raise SystemExit(main())
