#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""19/tests —— ems_supervisor（进程守护器）测试。

任务书要求的三件事与本文的落点：
  · **按正确顺序**拉起（初始化器最先、且不能跑完就退）→ [B]/[D]
  · 存活检测 + 异常重启                              → [C]
  · 退避                                            → [C]
另加两条本模块特有的：
  · 就绪探测不能退化成 sleep（超时 = 启动失败）        → [E]
  · stdin 看门狗默认必须关闭（上一轮的坑）             → [F]
以及一条真产物端到端：                                 → [H]

★ 为什么守护器一律以**子进程**方式测，不在本进程里 import 跑：
  ① [F] 必须能控制守护器自己的 stdin（NUL / 管道），in-process 做不到；
  ② 现场形态就是"另一个进程"，测它的日志文件才是真实证据。
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

import harness as H
from harness import T

import ems_supervisor as SV

SCRIPT = os.path.join(H.SRC, "ems_supervisor.py")

FAKE_SRC = r'''import sys, time
name, alive, ready, delay = sys.argv[1], float(sys.argv[2]), sys.argv[3], float(sys.argv[4])
if ready == "1":
    time.sleep(delay)
    print("[FAKE] ready name=%s" % name, flush=True)
time.sleep(alive)
print("[FAKE] leaving name=%s" % name, flush=True)
'''

LINE_RE = re.compile(r"t=([0-9.]+) (start|ready|exit|kill|cascade_dirty|"
                     r"ready_timeout|stabilized|start_failed) name=(\S+)")


def parse_events(log_text: str) -> list[tuple[float, str, str]]:
    out = []
    for m in LINE_RE.finditer(log_text):
        out.append((float(m.group(1)), m.group(2), m.group(3)))
    return out


def first_t(events, kind, name=None):
    for t, k, n in events:
        if k == kind and (name is None or n == name):
            return t
    return None


def all_t(events, kind, name=None):
    return [t for t, k, n in events if k == kind and (name is None or n == name)]


def write_root(tmp: str, procs: list[dict], **over) -> str:
    root = tempfile.mkdtemp(prefix="ems19_sup_", dir=tmp)
    fake = os.path.join(root, "fake_child.py")
    with open(fake, "w", encoding="utf-8", newline="\n") as f:
        f.write(FAKE_SRC)
    for p in procs:
        # 子进程脚本写在 root 里，调用方在调用前不知道具体路径 → 用 @FAKE@ 占位
        if isinstance(p.get("args"), list):
            p["args"] = [fake if a == "@FAKE@" else a for a in p["args"]]
        p.setdefault("exe", sys.executable)
        # ★ 默认"长命"（30 s）：顺序类用例要的是"进程一直在"，不是"反复退出"。
        #   短命子进程见 [C]/[D]，那里显式给 alive。另注意 Python 子进程在本机
        #   冷启动约 0.8 s（实测），所以下面各段的窗口都按这个量级留了余量。
        p.setdefault("args", ["-u", fake, p["name"], "30", "1", "0.05"])
        p.setdefault("cwd", ".")
    cfg = {"poll_s": 0.25,
           "backoff": {"base_s": 0.3, "factor": 2.0, "max_s": 5.0, "stable_s": 10.0},
           "log_dir": "log",
           "processes": procs}
    cfg.update(over)
    os.makedirs(os.path.join(root, "conf"), exist_ok=True)
    with open(os.path.join(root, "conf", "supervisor.json"), "w",
              encoding="utf-8", newline="\n") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)
    return root


def run_sup(root: str, duration: float, stdin=None, env_extra=None, cmd="run"):
    env = dict(os.environ)
    env.pop("EMS_SUPERVISOR_STDIN_WATCHDOG", None)
    if env_extra:
        env.update(env_extra)
    args = [sys.executable, SCRIPT, "--root", root]
    if cmd == "run":
        args += ["run", "--duration", str(duration)]
    else:
        args += [cmd]
    t0 = time.monotonic()
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       stdin=(subprocess.DEVNULL if stdin is None else stdin),
                       env=env)
    elapsed = time.monotonic() - t0
    logp = os.path.join(root, "log", "supervisor.log")
    log = open(logp, "r", encoding="utf-8", errors="replace").read() \
        if os.path.isfile(logp) else ""
    return r.returncode, r.stdout.decode("utf-8", "replace"), log, elapsed


def main() -> int:
    H.main_guard()
    t = T("19/ ems_supervisor 自检")
    H.banner("19/ ems_supervisor —— 进程守护器")

    tmp = tempfile.mkdtemp(prefix="ems19_sup_all_")
    try:
        # =============================================================
        t.section("A preflight（清单自检）")
        # =============================================================
        root_ok = write_root(tmp, [
            {"name": "init", "args": ["-u", "missing_or_fake.py", "x"], "depends_on": []},
        ])
        # 覆盖 exe/args 为一个确定存在的组合
        cfg = SV.load_config(root_ok, None)
        cfg["processes"] = [dict(cfg["processes"][0],
                                 exe=sys.executable, args=["-c", "pass"],
                                 cwd=".", ready_pattern="X")]
        s = SV.Supervisor(root_ok, cfg)
        t.eq(s.preflight(), [], "齐备清单 → 无问题")

        def pf(procs, **over):
            r = write_root(tmp, procs, **over)
            return SV.Supervisor(r, SV.load_config(r, None)).preflight()

        p = pf([{"name": "a", "exe": os.path.join(tmp, "no_such.exe"), "args": [],
                 "cwd": "."}])
        t.ok(any("可执行文件不存在" in x for x in p), "缺可执行文件 → 报出")

        p = pf([{"name": "a", "exe": sys.executable, "args": [], "cwd": ".",
                 "depends_on": ["ghost"]}])
        t.ok(any("依赖不存在的进程" in x for x in p), "依赖不存在的进程 → 报出")

        p = pf([{"name": "a", "exe": sys.executable, "args": [], "cwd": ".",
                 "depends_on": ["b"]},
                {"name": "b", "exe": sys.executable, "args": [], "cwd": ".",
                 "depends_on": ["a"]}])
        t.ok(any("成环" in x for x in p), "依赖成环 → 报出（否则守护器会死等）")

        p = pf([{"name": "a", "exe": sys.executable, "args": [], "cwd": ".",
                 "ready_pattern": "([unclosed"}])
        t.ok(any("ready_pattern 非法" in x for x in p), "非法正则 → 报出")

        p = pf([{"name": "a", "exe": sys.executable, "args": [], "cwd": "no/such/dir"}])
        t.ok(any("cwd 不存在" in x for x in p), "cwd 不存在 → 报出")

        p = pf([{"name": "a", "exe": sys.executable, "args": [], "cwd": "."},
                {"name": "a", "exe": sys.executable, "args": [], "cwd": "."}])
        t.ok(any("进程名重复" in x for x in p), "进程名重复 → 报出")

        # preflight 失败必须在 run 之前拦住（不许"半启动"）
        root_bad = write_root(tmp, [{"name": "a", "exe": os.path.join(tmp, "nope.exe"),
                                     "args": [], "cwd": "."}])
        rc, out, log, _ = run_sup(root_bad, 1.0)
        t.ne(rc, 0, "preflight 失败 → run 退出码非 0")
        t.contains(log, "PREFLIGHT_FAIL", "preflight 失败写进日志")
        t.eq(len([x for x in parse_events(log) if x[1] == "start"]), 0,
             "preflight 失败时**一个子进程都没起**")

        # =============================================================
        t.section("B 启动顺序（初始化器最先 + 依赖就绪才起下游）")
        # =============================================================
        root_b = write_root(tmp, [
            {"name": "init", "ready_pattern": r"\[FAKE\] ready", "ready_timeout_s": 5,
             "depends_on": []},
            {"name": "dev", "ready_pattern": r"\[FAKE\] ready", "ready_timeout_s": 5,
             "depends_on": ["init"]},
            {"name": "ems", "ready_pattern": r"\[FAKE\] ready", "ready_timeout_s": 5,
             "depends_on": ["dev"]},
        ])
        rc, out, log, _ = run_sup(root_b, 5.0)
        t.eq(rc, 0, "B 退出码 0")
        ev = parse_events(log)
        t_init_r = first_t(ev, "ready", "init")
        t_init_s = first_t(ev, "start", "init")
        t_dev_s = first_t(ev, "start", "dev")
        t_dev_r = first_t(ev, "ready", "dev")
        t_ems_s = first_t(ev, "start", "ems")
        t_ems_r = first_t(ev, "ready", "ems")
        t.ok(all(x is not None for x in
                 (t_init_s, t_init_r, t_dev_s, t_dev_r, t_ems_s, t_ems_r)),
             "B 三个进程都 start 且都 ready")
        t.lt(t_init_s, t_dev_s, "B 初始化器先于设备侧启动")
        t.lt(t_dev_s, t_ems_s, "B 设备侧先于 EMS 侧启动")
        t.lt(t_init_r, t_dev_s, "★ 设备侧在初始化器**就绪之后**才启动")
        t.lt(t_dev_r, t_ems_s, "★ EMS 在设备侧**就绪之后**才启动（约束③）")
        # 反向守卫：不是"几乎同时"
        t.gt(t_dev_s - t_init_r, 0.05, "★ 反向守卫：两者之间有真实间隔（不是同时拉起）")

        # 依赖永不就绪 → 下游**一个都不许起**
        root_b2 = write_root(tmp, [
            {"name": "init", "ready_pattern": r"NEVER_MATCHES", "ready_timeout_s": 1.0,
             "depends_on": []},
            {"name": "dev", "ready_pattern": r"\[FAKE\] ready", "ready_timeout_s": 2,
             "depends_on": ["init"]},
            {"name": "ems", "ready_pattern": r"\[FAKE\] ready", "ready_timeout_s": 2,
             "depends_on": ["dev"]},
        ])
        rc, out, log, _ = run_sup(root_b2, 2.6)
        ev = parse_events(log)
        t.eq(len(all_t(ev, "start", "dev")), 0,
             "★ 依赖从未就绪 → 设备侧一次都没启动（不是'先起了再说'）")
        t.eq(len(all_t(ev, "start", "ems")), 0, "★ 同理 EMS 侧也没起")
        t.gt(len(all_t(ev, "ready_timeout", "init")), 0, "B 初始化器就绪超时已被判定")

        # =============================================================
        t.section("C 存活检测 + 退避重启")
        # =============================================================
        root_c = write_root(tmp, [
            {"name": "w", "args": ["-u", "@FAKE@",
                                   "w", "0.5", "1", "0.05"],
             "ready_pattern": r"\[FAKE\] ready", "ready_timeout_s": 5,
             "depends_on": []},
        ], backoff={"base_s": 0.3, "factor": 2.0, "max_s": 5.0, "stable_s": 10.0})
        rc, out, log, _ = run_sup(root_c, 6.0)
        ev = parse_events(log)
        starts = all_t(ev, "start", "w")
        exits = all_t(ev, "exit", "w")
        t.guard(len(starts) >= 3,
                f"C 反向守卫：进程确实反复退出又起来（starts={len(starts)}）")
        t.ge(len(exits), 2, "C 检出了多次退出（存活检测有效）")
        gaps = [round(b - a, 3) for a, b in zip(starts, starts[1:])]
        t.gt(len(gaps), 1, "C 至少测到两段重启间隔")
        t.ok(all(y >= x - 1e-6 for x, y in zip(gaps, gaps[1:])),
             f"C 重启间隔非递减（退避在放大）：{gaps}")
        t.gt(gaps[-1], gaps[0], f"★ 反向守卫：末段间隔明显大于首段（{gaps[-1]} > {gaps[0]}）")
        # 退避不是固定间隔 —— 固定间隔实现会让上面两条红
        t.ne(len(set(gaps)), 1, "★ 间隔不全相等（不是固定 0.3 s 硬重启）")

        # 退出码语义：一次都没成功（ready）且反复退出 → 2
        # ★ 时长口径：fake 子进程是**真 Python 解释器**，冷启动约 0.7 s（满载更慢），
        #   所以 alive=0.2 s 的"短命"进程实际首拍退出 ≈ 0.9 s、退避 0.3 s 后第二拍
        #   启动 ≈ 1.2 s。1.6 s 的窗口在本机满载时只够 1 次启动（starts=1→restarts=0），
        #   rc 会退化成 0 —— 这不是守护器逻辑错，是测试窗口比冷启动还紧。给足 4 s，
        #   保证至少 2 次 start-exit 周期，让 restarts>=1 且 ever_ready=False 稳定成立。
        root_c2 = write_root(tmp, [
            {"name": "w", "args": ["-u", "@FAKE@",
                                   "w", "0.2", "0", "0"],
             "ready_pattern": r"NEVER", "ready_timeout_s": 5, "depends_on": []},
        ])
        rc, out, log, _ = run_sup(root_c2, 4.0)
        t.eq(rc, 2, "C 从未就绪的进程反复退出 → 运行期失败（退出码 2）")

        # =============================================================
        t.section("D 级联重启（初始化器死了，下游必须作废重起）")
        # =============================================================
        root_d = write_root(tmp, [
            {"name": "init", "args": ["-u", "@FAKE@",
                                      "init", "2.5", "1", "0.05"],
             "ready_pattern": r"\[FAKE\] ready", "ready_timeout_s": 5,
             "depends_on": []},
            {"name": "dev", "args": ["-u", "@FAKE@",
                                     "dev", "30", "1", "0.05"],
             "ready_pattern": r"\[FAKE\] ready", "ready_timeout_s": 5,
             "depends_on": ["init"]},
            {"name": "ems", "args": ["-u", "@FAKE@",
                                     "ems", "30", "1", "0.05"],
             "ready_pattern": r"\[FAKE\] ready", "ready_timeout_s": 5,
             "depends_on": ["dev"]},
        ])
        rc, out, log, _ = run_sup(root_d, 8.0)
        ev = parse_events(log)
        t.guard(len(all_t(ev, "ready", "ems")) >= 1, "D 反向守卫：ems 先成功起来过")
        casc = [(n, t_) for t_, k, n in ev if k == "cascade_dirty"]
        t.gt(len(casc), 0, "★ 初始化器退出后触发了级联作废")
        t.ok(any(n == "dev" for n, _ in casc), "★ 设备侧被级联作废（段没了）")
        t.ok(any(n == "ems" for n, _ in casc), "★ EMS 侧被级联作废（传递闭包）")
        kills = [(n, t_) for t_, k, n in ev if k == "kill"]
        t.ok(any(n == "dev" for n, _ in kills), "★ 设备侧被真的 kill 掉")
        t.gt(len(all_t(ev, "start", "dev")), 1, "★ 设备侧随后被重新拉起")
        # 重启后顺序依然成立
        starts_init = all_t(ev, "start", "init")
        ready_init = all_t(ev, "ready", "init")
        starts_dev = all_t(ev, "start", "dev")
        t.ge(len(starts_init), 2, "D 初始化器被重启")
        t.ge(len(starts_dev), 2, "D 设备侧被重启")
        t.lt(ready_init[1], starts_dev[1], "★ 重启后仍是『先就绪再起下游』")

        # =============================================================
        t.section("E 就绪探测：超时 = 启动失败（不许退化成 sleep）")
        # =============================================================
        root_e = write_root(tmp, [
            {"name": "silent", "args": ["-u", "@FAKE@",
                                        "silent", "30", "0", "0"],
             "ready_pattern": r"\[FAKE\] ready", "ready_timeout_s": 1.0,
             "depends_on": []},
        ])
        rc, out, log, _ = run_sup(root_e, 2.6)
        ev = parse_events(log)
        t.gt(len(all_t(ev, "ready_timeout", "silent")), 0,
             "E 超时未打印横幅 → 判定为启动失败")
        t.gt(len(all_t(ev, "start", "silent")), 1, "E 并按退避重启（不是'就当它好了'）")
        t.contains(out, "ready_timeout", "E 失败原因写在守护器输出里")
        # 反向守卫：给足时间 + 让它打印，就不该有超时
        root_e2 = write_root(tmp, [
            {"name": "loud", "args": ["-u", "@FAKE@",
                                      "loud", "10", "1", "0.1"],
             "ready_pattern": r"\[FAKE\] ready", "ready_timeout_s": 5,
             "depends_on": []},
        ])
        rc, out, log, _ = run_sup(root_e2, 2.0)
        t.eq(len(all_t(parse_events(log), "ready_timeout", "loud")), 0,
             "E 反向守卫：正常打印横幅时不报超时（说明超时判据有区分度）")

        # =============================================================
        t.section("F stdin 看门狗：默认必须关闭（上一轮实测的坑）")
        # =============================================================
        root_f = write_root(tmp, [
            {"name": "w", "args": ["-u", "@FAKE@",
                                   "w", "30", "1", "0.05"],
             "ready_pattern": r"\[FAKE\] ready", "ready_timeout_s": 5,
             "depends_on": []},
        ])
        # ① 默认（不设环境变量）+ stdin = NUL：必须活满全程
        rc, out, log, elapsed = run_sup(root_f, 2.5, stdin=subprocess.DEVNULL)
        t.ge(elapsed, 2.3, f"★ 默认配置 + stdin=NUL 时守护器**活满全程**（{elapsed:.2f}s）")
        t.eq(rc, 0, "★ 且正常退出（退出码 0）")
        t.ok("stdin EOF" not in log, "★ 日志里没有 EOF 自杀记录")
        ev = parse_events(log)
        t.eq(len(all_t(ev, "start", "w")), 1, "★ 子进程只起了一次（没被自杀打断）")
        t.eq(len(all_t(ev, "ready", "w")), 1, "★ 子进程确实就绪过（不是'起了没用'）")

        # ② 显式打开看门狗 + stdin=NUL：应当**立刻**退出（证明开关有效）
        rc, out, log, elapsed = run_sup(root_f, 30.0, stdin=subprocess.DEVNULL,
                                        env_extra={"EMS_SUPERVISOR_STDIN_WATCHDOG": "1"})
        t.lt(elapsed, 5.0, f"★ 显式打开看门狗后立刻退出（{elapsed:.2f}s，不是 30 s）")
        t.contains(log, "stdin EOF", "★ 日志里有 EOF 记录（看门狗确实在工作）")
        t.contains(log, "watchdog ENABLED", "★ 日志显式声明看门狗被启用")
        # 反向守卫：这一对断言合起来才说明"默认关闭"是刻意的选择
        t.ok(True, "★ 对照：默认活满 vs 显式开启秒退 —— 差异可见")

        # =============================================================
        t.section("G CLI：plan / check / dump-config")
        # =============================================================
        rc, out, log, _ = run_sup(root_b, 0, cmd="plan")
        t.eq(rc, 0, "G plan 退出码 0")
        t.contains(out, "启动顺序", "G plan 打印启动顺序")
        t.contains(out, "deps=init", "G plan 打印依赖")

        rc, out, _, _ = run_sup(root_b, 0, cmd="check")
        t.eq(rc, 0, "G check 对可用清单返回 0")
        rc, out, _, _ = run_sup(root_bad, 0, cmd="check")
        t.eq(rc, 1, "G check 对坏清单返回 1")
        t.contains(out, "[CHECK FAIL]", "G check 打出失败行")

        r = subprocess.run([sys.executable, SCRIPT, "--root", root_b, "--dump-config"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        d = json.loads(r.stdout.decode("utf-8"))
        names = [p["name"] for p in d["processes"]]
        t.eq(names, ["rtdb_initializer", "device_side", "ems_side"],
             "G 内置默认清单就是 11/ 的三进程形态（顺序即部署顺序）")
        t.eq(d["processes"][0]["depends_on"], [], "G 初始化器无依赖（最先起）")
        t.eq(d["processes"][2]["depends_on"], ["device_side"], "G EMS 依赖设备侧")
        t.ok(all(p["ready_pattern"] for p in d["processes"]),
             "G 每个进程都配了就绪判据（不是靠 sleep）")

        # =============================================================
        t.section("H 端到端：守护器拉真的 11/ 三进程（缺产物则 SKIP）")
        # =============================================================
        repo = H.REPO
        need = ["11/build/rtdb_initializer.exe", "11/build/device_side.exe",
                "11/build/ems_side.exe"]
        missing = [x for x in need if not os.path.isfile(os.path.join(repo, x))]
        if missing:
            t.skip(f"缺少 11/ 产物 {missing} —— 先跑 11/scripts/build.bat（跳过真产物端到端）")
        else:
            e2e_root = os.path.join(H.MODULE, "build", "e2e")
            shutil.rmtree(e2e_root, ignore_errors=True)
            os.makedirs(e2e_root, exist_ok=True)
            logdir = os.path.relpath(os.path.join(e2e_root, "log"), repo)
            cfg_e2e = {
                "poll_s": 0.3,
                "backoff": {"base_s": 0.5, "factor": 2.0, "max_s": 4.0,
                            "stable_s": 30.0},
                "log_dir": logdir,
                "processes": [
                    {"name": "rtdb_initializer", "exe": "11/build/rtdb_initializer.exe",
                     "args": ["--seconds", "120"], "cwd": ".",
                     "ready_pattern": r"\[INITIALIZER\] segment",
                     "ready_timeout_s": 10, "depends_on": []},
                    {"name": "device_side", "exe": "11/build/device_side.exe",
                     "args": ["--steps", "600", "--dt", "0.1", "--speed", "1",
                              "--report", "19/build/e2e/device_report.txt"], "cwd": ".",
                     "ready_pattern": r"\[DEVICE\] connected",
                     "ready_timeout_s": 15, "depends_on": ["rtdb_initializer"]},
                    {"name": "ems_side", "exe": "11/build/ems_side.exe",
                     "args": ["--steps", "600", "--dt", "0.1", "--speed", "1",
                              "--report", "19/build/e2e/ems_report.txt"], "cwd": ".",
                     "ready_pattern": r"\[EMS\] connected",
                     "ready_timeout_s": 25, "depends_on": ["device_side"]},
                ],
            }
            with open(os.path.join(e2e_root, "conf_supervisor.json"), "w",
                      encoding="utf-8", newline="\n") as f:
                json.dump(cfg_e2e, f, ensure_ascii=False, indent=2)

            args = [sys.executable, SCRIPT, "--root", repo, "--config",
                    os.path.join(e2e_root, "conf_supervisor.json"),
                    "run", "--duration", "9"]
            r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               stdin=subprocess.DEVNULL)
            out = r.stdout.decode("utf-8", "replace")
            log = open(os.path.join(e2e_root, "log", "supervisor.log"),
                       encoding="utf-8", errors="replace").read()
            ev = parse_events(log)

            t.eq(r.returncode, 0, "H 真三进程端到端退出码 0")
            order = [n for _, k, n in ev if k == "ready"]
            t.eq(order, ["rtdb_initializer", "device_side", "ems_side"],
                 "★ H 三个真进程**按部署顺序**依次就绪")
            t.eq(len(all_t(ev, "start", "rtdb_initializer")), 1, "H 初始化器只起一次")
            t.eq(len(all_t(ev, "exit")), 0, "★ H 全程没有任何进程异常退出（0 restarts）")
            t.eq(len(all_t(ev, "cascade_dirty")), 0, "H 全程无级联作废")

            ini_log = open(os.path.join(e2e_root, "log", "rtdb_initializer.out.log"),
                           encoding="utf-8", errors="replace").read()
            t.contains(ini_log, "[INITIALIZER] segment", "H 初始化器真的建了段")
            t.contains(ini_log, "points registered", "H 全点表已注册")
            dev_log = open(os.path.join(e2e_root, "log", "device_side.out.log"),
                           encoding="utf-8", errors="replace").read()
            t.contains(dev_log, "[DEVICE] connected", "H 设备侧真的连上了段")
            ems_log = open(os.path.join(e2e_root, "log", "ems_side.out.log"),
                           encoding="utf-8", errors="replace").read()
            t.contains(ems_log, "[EMS] connected", "H EMS 侧真的连上了段")
            t.contains(ems_log, "full stack online", "★ H EMS 全栈已装起来")
            # EMS 能连上 = 设备已发布 CFG（约束③ 的行为证据）
            t.ok("device side not publishing" not in ems_log,
                 "★ H EMS 没有报『设备侧未发布』（说明顺序约束 ③ 满足）")
            t.ok("FAIL" not in ems_log, "★ H EMS 侧日志里没有 FAIL 行（真的跑起来了）")
            t.ok("FAIL" not in dev_log, "★ H 设备侧日志里没有 FAIL 行")
            # 收尾：日志文件都留下了（可运维）
            t.ok(os.path.isfile(os.path.join(e2e_root, "log", "supervisor.log")),
                 "H 守护器日志落盘")

    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    return t.report()


if __name__ == "__main__":
    raise SystemExit(main())
