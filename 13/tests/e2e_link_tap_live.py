#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
13/tests — 真实链路端到端：C++ 主站 ──► link_tap ──► pymodbus 从站

和另外两个测试目标的分工：
    test_modbus_tcp.exe      进程内假从站 → 客户端符不符合协议
    test_modbus_bridge.exe   真从站、正常链路 → 两个独立实现能不能互通
    e2e_link_tap_live.py     真从站、**异常链路** → 链路不正常时主站是什么行为

被测对象是**真实交付物**：build/modbus_probe.exe（C++ 主站）与
13/sim/modbus_slave.py（pymodbus 从站）一行未改，中间插进 link_tap。

    python tests\\e2e_link_tap_live.py
"""

import json
import os
import re
import socket
import struct
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PY = os.path.join(ROOT, ".venv", "Scripts", "python.exe")
if not os.path.exists(PY):
    PY = sys.executable
SLAVE = os.path.join(ROOT, "sim", "modbus_slave.py")
TAP = os.path.join(ROOT, "sim", "link_tap.py")
PROBE = os.path.join(ROOT, "build", "modbus_probe.exe")
GOLDEN = os.path.join(ROOT, "build", "golden_link_tap.jsonl")
LOGDIR = os.path.join(ROOT, "build")

PASS = 0
FAIL = 0


def ck(what, cond, extra=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("  [ OK ] %s" % what, flush=True)
    else:
        FAIL += 1
        print("  [FAIL] %s %s" % (what, extra), flush=True)


def free_ports(n):
    socks, ports = [], []
    for _ in range(n):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        socks.append(s)
        ports.append(s.getsockname()[1])
    for s in socks:
        s.close()
    return ports


def decode(raw):
    if raw is None:
        return ""
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


def run_probe(port, args, timeout=60):
    """跑 C++ 主站，返回 (退出码, 输出)。"""
    cmd = [PROBE, "--port", str(port)] + list(args)
    try:
        r = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout)
        return r.returncode, decode(r.stdout)
    except subprocess.TimeoutExpired as e:
        return 124, decode(e.stdout) + "\n<探针整体超时>"


def api(ctl, path, body=None, method=None):
    url = "http://127.0.0.1:%d%s" % (ctl, path)
    if body is None and method is None:
        method = "GET"
    data = json.dumps(body).encode("utf-8") if body is not None else (
        b"" if method == "POST" else None)
    req = urllib.request.Request(url, data=data, method=method or "POST")
    if data is not None:
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=8) as r:
        return json.loads(r.read().decode("utf-8"))


def wait_tcp(port, timeout=15.0):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            s.close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def wait_ctl(port, timeout=15.0):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        try:
            api(port, "/api/status")
            return True
        except Exception:
            time.sleep(0.2)
    return False


def show(title, text, limit=1600):
    print("  ┌─ %s" % title, flush=True)
    body = "\n".join("  │ " + ln for ln in text.strip().splitlines())
    print(body[:limit] if body else "  │ (无输出)", flush=True)
    print("  └─", flush=True)


def main():
    print("=" * 74)
    print("  13/ 真实链路端到端：C++ 主站 ──► link_tap ──► pymodbus 从站")
    print("=" * 74)
    if not os.path.exists(PROBE):
        print("缺少 %s —— 先跑 scripts\\build.bat" % PROBE)
        return 1
    for f in (GOLDEN,):
        if os.path.exists(f):
            os.remove(f)

    sp, tp, cp = free_ports(3)
    print("从站 %d ｜ 中间人 %d ｜ 控制口 %d" % (sp, tp, cp), flush=True)

    slave_log = open(os.path.join(LOGDIR, "_e2e_slave.log"), "w", encoding="utf-8")
    tap_log = open(os.path.join(LOGDIR, "_e2e_tap.log"), "w", encoding="utf-8")
    slave = subprocess.Popen(
        [PY, "-u", SLAVE, "--port", str(sp), "--load", "380", "--pv", "150",
         "--bias", "40"],
        cwd=ROOT, stdout=slave_log, stderr=subprocess.STDOUT)
    tap = None
    try:
        ck("pymodbus 从站已就绪", wait_tcp(sp))
        # ★ 中间人开 --verbose 会逐帧打日志，输出必须落到**文件**而不是管道：
        #   管道没人读会写满 64KB 缓冲区，把中间人卡死 —— 表现是"跑到一半链路
        #   全超时"，很容易被误判成工具本身有 bug。
        tap = subprocess.Popen(
            [PY, "-u", TAP, "--listen", str(tp), "--upstream", str(sp),
             "--ctl-port", str(cp), "--record", GOLDEN, "--verbose"],
            cwd=ROOT, stdout=tap_log, stderr=subprocess.STDOUT)
        ck("链路中间人已就绪", wait_ctl(cp))

        # ---------------- 1. 正常链路基线 ----------------
        print("\n--- 1/5 正常链路基线（主站连的是中间人的端口）---", flush=True)
        api(cp, "/api/clear", {})
        rc, out = run_probe(tp, ["--repeat", "2", "--verbose"])
        show("modbus_probe（正常）", out)
        ck("主站退出码 = 0", rc == 0, "rc=%d" % rc)
        st = api(cp, "/api/status")
        ck("中间人记录了双向帧", st["stats"]["req"] >= 2 and st["stats"]["rsp"] >= 2,
           json.dumps(st["stats"]))
        ck("基线无丢弃", st["stats"]["dropped"] == 0)
        ck("基线时延在毫秒级", st["latency"]["max"] < 50.0, json.dumps(st["latency"]))

        # 写指令（FC16）也要穿过中间人
        rc2, out2 = run_probe(tp, ["--set-power", "120"], timeout=40)
        ck("主站下发功率指令（FC16）穿透中间人成功", rc2 == 0, "rc=%d" % rc2)
        frames = api(cp, "/api/frames?n=400")["frames"]
        ck("帧记录里出现 FC16 写指令",
           any(f["dir"] == "REQ" and "写多寄存器" in f["text"] for f in frames),
           str([f["text"] for f in frames if f["dir"] == "REQ"][:6]))

        # ---------------- 2. 回包延迟 ----------------
        print("\n--- 2/5 注入「回包延迟 400 ms」（主站超时 1000 ms）---", flush=True)
        api(cp, "/api/clear", {})
        api(cp, "/api/perturb", {"delay_ms": 400.0})
        rc, out = run_probe(tp, ["--repeat", "2", "--interval", "0.3", "--verbose"])
        show("modbus_probe（延迟 400 ms）", out)
        ck("延迟小于主站超时时仍成功", rc == 0, "rc=%d" % rc)
        st = api(cp, "/api/status")
        ck("中间人记录的时延 ≈ 400 ms（与主站感受一致）",
           st["latency"]["n"] >= 2 and 380.0 <= st["latency"]["p50"] <= 700.0,
           json.dumps(st["latency"]))

        # 把延迟推到超过主站超时：主站必须把数据标成不可信，而不是静默通过
        api(cp, "/api/perturb", {"delay_ms": 1500.0})
        rc, out = run_probe(tp, ["--repeat", "1", "--timeout", "600"])
        show("modbus_probe（延迟 1500 ms > 主站超时 600 ms）", out, limit=900)
        ck("延迟超过主站超时时，主站明确标记数据不可信（不是静默通过）",
           probe_untrusted(out) or probe_bad_items(out),
           "rc=%d；输出里没有「不可信」也没有 `--` 质量标记" % rc)
        api(cp, "/api/perturb", {"delay_ms": 0.0})

        # ---------------- 3. 间歇丢包 ----------------
        print("\n--- 3/5 注入「间歇丢包 1/3」---", flush=True)
        api(cp, "/api/clear", {})
        api(cp, "/api/perturb", {"drop_every": 3})
        rc, out = run_probe(tp, ["--repeat", "6", "--interval", "0.2", "--timeout", "500"])
        show("modbus_probe（丢包 1/3）", out, limit=900)
        st = api(cp, "/api/status")
        ck("确实丢了帧", st["stats"]["dropped"] >= 1, json.dumps(st["stats"]))
        # drop_every=3 ⇒ 每 3 个响应丢 1 个 ⇒ 长期比例约 1/3。
        # ★ 这里同时是「丢弃计数不能被重复计」的回归保护：gate 里加一次、
        #   记事件时又加一次的话，比例会变成 2/3，一眼可辨。
        ratio = st["stats"]["dropped"] / max(1, st["stats"]["rsp"])
        ck("丢包比例约 1/3（不是被双重计数成 2/3）",
           0.15 <= ratio <= 0.5,
           "dropped=%d rsp=%d ratio=%.2f" % (st["stats"]["dropped"],
                                             st["stats"]["rsp"], ratio))
        ck("主站观测到数据不可信", probe_untrusted(out) or probe_bad_items(out))
        api(cp, "/api/perturb", {"drop_every": 0})

        # ---------------- 4. 瞬断黑障：发生 → 持续 → 恢复 ----------------
        print("\n--- 4/5 瞬断黑障：t+2s 断 3s，t+6s 恢复（一次会话跑完）---", flush=True)
        api(cp, "/api/clear", {})
        api(cp, "/api/scenario", {"name": "blackout"})
        t0 = time.monotonic()
        rc, out = run_probe(tp, ["--repeat", "9", "--interval", "0.85", "--timeout", "500"],
                            timeout=60)
        show("modbus_probe（黑障）", out, limit=2400)
        st = api(cp, "/api/status")
        ck("黑障期间确有丢弃", st["stats"]["dropped"] >= 2, json.dumps(st["stats"]))
        ck("黑障有起止（不是一直断）", st["stats"]["rsp"] >= 2, json.dumps(st["stats"]))
        ck("时间轴留下了发生/解除痕迹",
           any("blackout" in n for n in st["notes"]), str(st["notes"]))
        print("  · 整个黑障过程耗时 %.1f s" % (time.monotonic() - t0), flush=True)
        api(cp, "/api/scenario", {"name": "reset"})
        rc, out = run_probe(tp, ["--repeat", "2", "--interval", "0.3"])
        ck("解除后主站恢复成功（不是锁存）", rc == 0, "rc=%d" % rc)

        # ---------------- 5. 测点注入 ----------------
        print("\n--- 5/5 测点注入：字序错 + NaN（协议层都正常，值却是错的）---",
              flush=True)
        api(cp, "/api/clear", {})
        api(cp, "/api/inject", {"clear": True})
        rc, out = run_probe(tp, ["--repeat", "1", "--verbose"])
        show("modbus_probe（对照：不注入）", out, limit=900)
        ck("对照：可正常读到 MEAS.P_BAT 且质量 ok",
           probe_point(out, "MEAS.P_BAT")[1] == "ok", str(probe_point(out, "MEAS.P_BAT")))

        # 用**确定的值**做字序注入：把 123.0 按相反字序编码出去。
        # 为什么不沿用现场那条一直在漂的 P_BAT：那样断言会变成"和上一轮不同"，
        # 既不知道真值是多少，也无法判断错成了什么 —— 断言会退化成"有变化就算过"。
        api(cp, "/api/clear", {})
        api(cp, "/api/inject", {"MEAS.P_BAT": {"value": 123.0, "flip": True}})
        rc, out = run_probe(tp, ["--repeat", "1", "--verbose"])
        show("modbus_probe（把 MEAS.P_BAT=123 按相反字序发出）", out, limit=900)
        wf = _last_decoded(api(cp, "/api/frames?n=200")["frames"], "MEAS.P_BAT")
        ck("中间人按该点应有字序解出的不是 123（字序确实被换了）",
           wf is not None and abs(wf - 123.0) > 1.0, "解出 %r" % wf)
        pv, pq = probe_point(out, "MEAS.P_BAT")
        ck("★ 主站侧把错值当成合格数据（质量 ok）—— 这才是字序错最危险的地方",
           pq == "ok", "主站质量标记=%s，读到 %s" % (pq, pv))
        ck("★ 主站读到的不是真值 123",
           pv is not None and abs(float(pv) - 123.0) > 1.0, "读到 %s" % pv)
        print("  · 结论：真值 123 kW 与主站读到的 %s（质量 %s）之间没有任何告警 ——"
              % (pv, pq), flush=True)
        print("    仅靠 kNonFinite（只挡 NaN/Inf）检不出这类错值，"
              "必须再叠加量程合理性判断。", flush=True)

        api(cp, "/api/inject", {"clear": True})
        api(cp, "/api/clear", {})
        api(cp, "/api/inject", {"MEAS.P_BAT": {"raw_words": [0x0000, 0x7FC0]}})
        rc, out = run_probe(tp, ["--repeat", "1", "--verbose"])
        show("modbus_probe（注入 IEEE NaN）", out, limit=900)
        ck("主站把该点判为质量不合格（kNonFinite 守卫生效）",
           probe_bad_items(out) or probe_untrusted(out),
           "主站读到 %s" % str(probe_point(out, "MEAS.P_BAT")))
        ck("中间人也把它标成 NaN（不伪装成正常值）",
           any("=NaN" in (f["decoded"] or "")
               for f in api(cp, "/api/frames?n=200")["frames"]))
        api(cp, "/api/inject", {"clear": True})

        # ---------------- 收尾：黄金向量与统计 ----------------
        print("\n--- 收尾：帧级黄金向量与统计 ---", flush=True)
        ck("黄金向量文件已生成", os.path.exists(GOLDEN) and os.path.getsize(GOLDEN) > 0)
        rows = [json.loads(l) for l in open(GOLDEN, encoding="utf-8") if l.strip()]
        dirs = {}
        for r in rows:
            dirs[r["dir"]] = dirs.get(r["dir"], 0) + 1
        ck("黄金向量含请求帧", dirs.get("REQ", 0) > 0, str(dirs))
        ck("黄金向量含响应帧", dirs.get("RSP", 0) > 0, str(dirs))
        ck("黄金向量含丢弃/异常事件", dirs.get("DROP", 0) + dirs.get("ERR", 0) > 0,
           str(dirs))
        ck("每行 hex 均可重新解析成合法 MBAP 帧",
           all(_mbap_ok(r["hex"]) for r in rows if r["hex"]),
           "共 %d 行" % len(rows))
        print("  · 帧方向统计：%s" % json.dumps(dirs, ensure_ascii=False), flush=True)
        print("  · 录制文件：%s（%d 行，%.1f KB）"
              % (GOLDEN, len(rows), os.path.getsize(GOLDEN) / 1024.0), flush=True)
        st = api(cp, "/api/status")
        print("  · 最终时延分布：%s" % json.dumps(st["latency"], ensure_ascii=False),
              flush=True)

    finally:
        for p in (tap, slave):
            if p is None:
                continue
            try:
                p.terminate()
                p.wait(timeout=6)
            except Exception:
                try:
                    p.kill()
                except Exception:
                    pass
        slave_log.close()
        tap_log.close()

    print("\n" + "=" * 74)
    print("  PASS=%d FAIL=%d" % (PASS, FAIL))
    if FAIL == 0:
        print("  ALL TESTS PASSED")
    print("=" * 74)
    return 0 if FAIL == 0 else 1


# ---- 小工具 ----
def probe_point(out, name):
    """从探针输出里抠出某个点的 (数值字符串, 质量标记)。

    探针那行的形状是： `  [ 2] MEAS.P_BAT   IR[ 4] f32   28.6859  ok`
    """
    m = re.search(re.escape(name) + r"\s+IR\[\s*\d+\]\s+\S+\s+(\S+)\s+(\S+)", out)
    return (m.group(1), m.group(2)) if m else (None, None)


def probe_untrusted(out):
    """探针是否把某一整块标成「**不可信**」。

    ★ 判据**不能用退出码**：实测主站读超时、丢包、解码失败时**退出码仍是 0**，
      它只在输出里把该块标成不可信、把该点质量标成 `--`。
      "退出码 0 就等于数据可用"是一个很容易犯、而且后果很重的误判 ——
      自动化联调里如果拿退出码当结论，超时会被当成成功放过。
    """
    return "不可信" in out


def probe_bad_items(out):
    """探针是否把**某个点**标成质量不合格（行尾 `--`）。"""
    return bool(re.search(r"--\s*$", out, re.M))


def _last_decoded(frames, name):
    """取该点**最后**一次出现的解码值。

    ★ 必须取最后一次、不能取第一次：帧缓冲里还留着上一轮的帧，
      取第一次会拿到"注入之前"的那个值，于是"注入没生效"的假失败就出现了。
    """
    hit = None
    for f in frames:
        for part in (f.get("decoded") or "").split(" | "):
            if part.startswith(name + "="):
                v = part.split("=", 1)[1].replace(" ★", "").strip()
                v = v.replace(" word-swap", "")
                try:
                    hit = float(v)
                except ValueError:
                    hit = None
    return hit


def _mbap_ok(hexs):
    try:
        raw = bytes.fromhex(hexs)
        if len(raw) < 7:
            return False
        _tid, pid, length = struct.unpack(">HHH", raw[:6])
        return pid == 0 and len(raw) == 6 + length
    except Exception:
        return False


if __name__ == "__main__":
    raise SystemExit(main())
