#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
13/tests — link_tap 的断言测试（L01~L12）

为什么它必须是**自包含**的：
    13/sim/link_tap.py 是"测试夹具的夹具"——它坏了，表现不是报错而是
    **静默地少测**（比如帧没记录全、注入没生效、延迟没加上）。这类失效
    靠"跑一遍看起来没问题"根本发现不了。所以它自己必须有断言。

刻意不依赖的东西：
    · 不依赖 pymodbus —— 本文件自带一个**最小假从站**（只为让链路有对端）
    · 不依赖 C++ 构建产物 —— 用原始 socket 当主站，只看协议字节
    · 不依赖网络端口固定 —— 端口用 bind(0) 现取，避免与常驻服务撞车
这样它能在任何装了解释器的机器上秒级跑完，是真正的"随时可复跑"。

    python tests\\test_link_tap.py          （用 13/.venv 或任意 python3）
"""

import faulthandler
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SIM = os.path.join(ROOT, "sim")
for p in (SIM,):
    if p not in sys.path:
        sys.path.insert(0, p)

# ★ 这个测试会起子进程、开监听端口、用真实时钟，任何一步失手都是"挂住"而不是
#   报错 —— 而挂住时报错信息为零，最难查。所以默认挂一个看门狗：超时就把
#   **所有线程的栈**打出来再退出。设 TAP_TEST_WATCHDOG=0 可关闭。
_WD = float(os.environ.get("TAP_TEST_WATCHDOG", "60"))
if _WD > 0:
    faulthandler.dump_traceback_later(_WD, exit=True)

import modbus_slave as slv  # noqa: E402  （只用它的点表与编码工具，不起服务）

PY = sys.executable
TAP = os.path.join(SIM, "link_tap.py")

PASS = 0
FAIL = 0
CUR = ""


# =====================================================================
# 极简断言框架（与 13/ 其余测试目标输出口径一致：PASS=n FAIL=0）
# =====================================================================
def case(name):
    global CUR
    CUR = name
    print("\n--- %s ---" % name, flush=True)


def ck(what, cond, extra=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("  [ OK ] %s" % what)
    else:
        FAIL += 1
        print("  [FAIL] %s %s" % (what, extra))


# =====================================================================
# 最小假从站：只实现被测需要的 4 个功能码
# =====================================================================
class FakeSlave(threading.Thread):
    """一个只为"让链路有对端"而存在的 Modbus TCP 从站。

    ★ 它不做任何工程语义（不做 SOC 积分、不做一阶惯性）—— 那些在
      modbus_slave.py 里已经测过了。这里只要寄存器值可控、能读能写，
      当"链路的另一端"就够了。少了工程语义，故障诊断时反而是优点：
      看到异常值就能确定是链路干的，不是模型干的。
    """

    def __init__(self, port):
        threading.Thread.__init__(self, name="fake-slave", daemon=True)
        self._halt = False
        self.ir = [0] * 64
        self.hr = [0] * 64
        self.di = [0] * 8
        self.writes = []
        self.nreq = 0
        self._fill()
        # ★ 在 __init__ 里就 bind+listen，不留给 run()：
        #   ① 放进 run() 的话，绑定失败只会让子线程**悄悄死掉**，主测试毫不知情；
        #   ② start() 之后立刻连接会撞上"线程还没开始 listen"的窗口。
        #   两种情况的表象都是"链路不通"，会被误判成中间人的问题。
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port))
        self.srv.listen(8)
        self.srv.settimeout(0.2)

    def _fill(self):
        g = slv.f32_to_regs
        self.ir[0:2] = g(300.0)                       # MEAS.P_LOAD
        self.ir[2:4] = g(120.0)                       # MEAS.P_PV
        self.ir[4:6] = g(-50.0, low_word_first=True)  # MEAS.P_BAT ★ 低字在前
        self.ir[6:8] = g(232.0)                       # MEAS.P_GRID
        self.ir[8] = slv.u16_scaled(0.55, 10000.0)    # MEAS.SOC
        self.ir[9] = slv.i16_scaled(25.0, 10.0)       # MEAS.T_C
        self.ir[10] = slv.u16_scaled(0.98, 10000.0)   # MEAS.SOH
        cfgs = [1000.0, 200.0, 200.0, 180.0, 190.0, 250.0, 250.0,
                3.0, 50.0, 2.0, 0.96, 0.96, 0.05, 0.95]
        for i, v in enumerate(cfgs):
            self.ir[12 + i * 2: 14 + i * 2] = g(v)
        self.di[0] = self.di[1] = self.di[2] = 1      # 三条通信 OK
        self.di[5] = 1                                # DATA_VALID
        self.di[7] = 1                                # ★ BMS_DIS_FORBID

    def run(self):
        while not self._halt:
            try:
                cs, _ = self.srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self._serve, args=(cs,), daemon=True).start()
        try:
            self.srv.close()
        except OSError:
            pass

    def _serve(self, cs):
        buf = b""
        try:
            while True:
                d = cs.recv(4096)
                if not d:
                    return
                buf += d
                while len(buf) >= 7:
                    tid, _pid, length = struct.unpack(">HHH", buf[:6])
                    total = 6 + length
                    if len(buf) < total:
                        break
                    frame, buf = buf[:total], buf[total:]
                    unit = frame[6]
                    resp = self._on_pdu(frame[7:])
                    if resp is not None:
                        cs.sendall(struct.pack(">HHHB", tid, 0, len(resp) + 1, unit) + resp)
        except OSError:
            return
        finally:
            try:
                cs.close()
            except OSError:
                pass

    def _on_pdu(self, pdu):
        """★ 方法名**不能**叫 `_handle`：Python 3.13 起 `threading.Thread`
        自带一个 `self._handle`（`_ThreadHandle` 实例），实例属性会遮蔽
        同名方法 —— 于是 `self._handle(pdu)` 变成"调用一个不可调用对象"，
        报 `TypeError: '_thread._ThreadHandle' object is not callable`。
        这类"基类悄悄占了你想要的名字"的坑，只在 3.13+ 才出现。"""
        self.nreq += 1
        if not pdu:
            return None
        fc = pdu[0]
        if fc in (3, 4):
            a, q = struct.unpack(">HH", pdu[1:5])
            regs = (self.ir if fc == 4 else self.hr)[a:a + q]
            return bytes([fc, len(regs) * 2]) + struct.pack(">%dH" % len(regs), *regs)
        if fc == 2:
            a, q = struct.unpack(">HH", pdu[1:5])
            nb = (q + 7) // 8
            out = bytearray(nb)
            for i in range(q):
                if a + i < len(self.di) and self.di[a + i]:
                    out[i // 8] |= (1 << (i % 8))
            return bytes([fc, nb]) + bytes(out)
        if fc == 6:
            a, v = struct.unpack(">HH", pdu[1:5])
            self.hr[a] = v
            self.writes.append((a, v))
            return pdu[:5]
        if fc == 16:
            a, q, bc = struct.unpack(">HHB", pdu[1:6])
            vals = struct.unpack(">%dH" % q, pdu[6:6 + bc])
            for i, v in enumerate(vals):
                self.hr[a + i] = v
            self.writes.append((a, list(vals)))
            return pdu[:5]
        return bytes([fc | 0x80, 1])


# =====================================================================
# 主站侧（原始 socket）与控制口客户端
# =====================================================================
def free_ports(n):
    """一次取 n 个**互不相同**的可用端口。

    ★ 必须一次取：逐个 `bind(0)` 后立刻 close 再取下一个，操作系统很可能
      把刚释放的那个端口再发一次 —— 于是"上游端口"和"监听端口"撞成同一个，
      中间人就会**连上自己**，accept→再连自己→无限自我连接。
      这个坑的表现是"测试挂住不动"，不是报错，极难从现象反推。
      做法：先把 n 个 socket 都 bind 住（都占着），拿完端口再一起放。
    """
    socks, ports = [], []
    for _ in range(n):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        socks.append(s)
        ports.append(s.getsockname()[1])
    for s in socks:
        s.close()
    return ports


def mbap(tid, unit, pdu):
    return struct.pack(">HHHB", tid, 0, len(pdu) + 1, unit) + pdu


def recv_frame(sock, timeout=3.0):
    sock.settimeout(timeout)
    buf = b""
    deadline = time.monotonic() + timeout
    while True:
        if len(buf) >= 7:
            _t, _p, length = struct.unpack(">HHH", buf[:6])
            if len(buf) >= 6 + length:
                return buf[:6 + length]
        if time.monotonic() > deadline:
            raise socket.timeout("recv_frame 超时（已收 %d 字节）" % len(buf))
        d = sock.recv(4096)
        if not d:
            raise IOError("对端关闭")
        buf += d


class Master(object):
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.tid = 0

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def _tid(self):
        self.tid += 1
        return self.tid

    def read(self, fc, addr, qty, timeout=3.0):
        """返回 (原始帧, 往返毫秒)；超时抛 socket.timeout。"""
        tid = self._tid()
        t0 = time.monotonic()
        self.sock.sendall(mbap(tid, 1, struct.pack(">BHH", fc, addr, qty)))
        raw = recv_frame(self.sock, timeout)
        return raw, (time.monotonic() - t0) * 1000.0

    def read_regs(self, fc, addr, qty, timeout=3.0):
        raw, dt = self.read(fc, addr, qty, timeout)
        pdu = raw[7:]
        if pdu[0] & 0x80:
            raise IOError("异常响应 %d" % pdu[1])
        bc = pdu[1]
        return list(struct.unpack(">%dH" % (bc // 2), pdu[2:2 + bc])), dt

    def write_regs(self, addr, values):
        tid = self._tid()
        pdu = struct.pack(">BHHB", 16, addr, len(values), len(values) * 2) + \
            struct.pack(">%dH" % len(values), *values)
        self.sock.sendall(mbap(tid, 1, pdu))
        return recv_frame(self.sock, 3.0)


def api(ctl_port, path, body=None, method=None):
    url = "http://127.0.0.1:%d%s" % (ctl_port, path)
    if body is None and method is None:
        method = "GET"
    data = json.dumps(body).encode("utf-8") if body is not None else (
        b"" if method == "POST" else None)
    req = urllib.request.Request(url, data=data, method=method or "POST")
    if data is not None:
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=6) as r:
        return json.loads(r.read().decode("utf-8"))


def wait_ready(port, timeout=12.0):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        try:
            api(port, "/api/status")
            return True
        except Exception:
            time.sleep(0.15)
    return False


class Tap(object):
    """把 link_tap.py 当**外部进程**起 —— 这样测的是真交付物，不是它的副本。"""

    def __init__(self, listen, upstream, ctl, record=""):
        cmd = [PY, TAP, "--listen", str(listen), "--upstream", str(upstream),
               "--ctl-port", str(ctl), "--max-frames", "800"]
        if record:
            cmd += ["--record", record]
        self.proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True,
                                     encoding="utf-8", errors="replace")
        self.ctl = ctl
        self.ready = wait_ready(ctl)

    def stop(self):
        try:
            self.proc.terminate()
            self.proc.wait(timeout=6)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass

    def out(self):
        """取子进程输出。

        ★ 只有进程已经结束才能读：stdout 是管道，`read()` 会一直阻塞到 EOF，
          进程还活着时调用就是把测试挂死（实测在第一步就挂，且零输出可查）。
        """
        if self.proc.poll() is None:
            return "(子进程仍在运行，未取输出)"
        try:
            return self.proc.stdout.read() or ""
        except Exception:
            return ""


# =====================================================================
# 用例
# =====================================================================
def main():
    print("=" * 72)
    print("  13/ link_tap 断言测试（L01~L12）")
    print("  python : %s" % PY)
    print("  目标   : %s" % TAP)
    print("=" * 72)

    slave_port, tap_port, ctl_port = free_ports(3)
    ck("三个端口互不相同（否则中间人会连上自己）",
       len({slave_port, tap_port, ctl_port}) == 3,
       "%d/%d/%d" % (slave_port, tap_port, ctl_port))
    slave = FakeSlave(slave_port)
    slave.start()

    rec_path = os.path.join(ROOT, "build", "_tap_golden_test.jsonl")
    if os.path.exists(rec_path):
        os.remove(rec_path)
    tap = Tap(tap_port, slave_port, ctl_port, record=rec_path)

    m = None
    try:
        ck("中间人进程已就绪（控制口可应答）", tap.ready)
        if not tap.ready:
            tap.stop()
            print("  子进程输出：\n%s" % tap.out())
        ck("控制页 / 可访问（内嵌监视台）", _html_ok(ctl_port))

        m = Master(tap_port)

        # ---------------- L01 正常通路 + 帧记录 ----------------
        case("L01 正常通路：透过中间人读写，帧被完整记录")
        regs, dt0 = m.read_regs(4, 0, 12)
        ck("FC04 读 IR[0..11] 成功", len(regs) == 12 and regs[0] != 0)
        m.write_regs(0, [400, 250, 200])
        st = api(ctl_port, "/api/status")
        ck("统计：请求数 ≥ 2", st["stats"]["req"] >= 2, str(st["stats"]))
        ck("统计：响应数 ≥ 2", st["stats"]["rsp"] >= 2, str(st["stats"]))
        ck("统计：无丢弃", st["stats"]["dropped"] == 0, str(st["stats"]))
        frames, _ = _frames(ctl_port)
        ck("帧序列以 REQ 开头且方向交替",
           frames[0]["dir"] == "REQ" and frames[1]["dir"] == "RSP",
           str([f["dir"] for f in frames[:4]]))
        ck("每帧都带完整原始 hex 且可重新解析",
           all(_reparse(f["hex"]) for f in frames))

        # ---------------- L02/L03 解码正确性 ----------------
        case("L02 FC04 响应按点表解码成工程量")
        # ★ CFG 区在地址 12~39，**不在** L01 那次 IR[0..11] 里 —— 不补这一读，
        #   断言会找不到 CFG 点，看起来像"解码漏了点"，其实是测试自己没读。
        cfgregs, _dt = m.read_regs(4, 12, 28)
        ck("CFG 区读到 28 个寄存器（14 个 f32 点）", len(cfgregs) == 28)
        frames, _ = _frames(ctl_port)
        ck("解出 MEAS.P_LOAD 在 280~320", any(
            d["name"] == "MEAS.P_LOAD" and 280 < d["value"] < 320
            for f in frames if f["decoded"] for d in _decoded_of(f, "MEAS.P_LOAD")),
            _dump_decoded(frames, "MEAS.P_LOAD"))
        ck("解出 MEAS.P_GRID = 232", any(
            d["name"] == "MEAS.P_GRID" and abs(d["value"] - 232.0) < 1e-3
            for f in frames for d in _decoded_of(f, "MEAS.P_GRID")))
        ck("解出 MEAS.SOC = 0.55", any(
            d["name"] == "MEAS.SOC" and abs(d["value"] - 0.55) < 1e-6
            for f in frames for d in _decoded_of(f, "MEAS.SOC")))
        ck("解出 MEAS.SOH = 0.98", any(
            d["name"] == "MEAS.SOH" and abs(d["value"] - 0.98) < 1e-6
            for f in frames for d in _decoded_of(f, "MEAS.SOH")))
        # ★ 低字在前的那个点：整条链路最容易被静默读错的地方
        ck("解出 MEAS.P_BAT = -50（低字在前，若字序搞反会是天文数字）", any(
            d["name"] == "MEAS.P_BAT" and abs(d["value"] + 50.0) < 1e-3
            for f in frames for d in _decoded_of(f, "MEAS.P_BAT")),
            _dump_decoded(frames, "MEAS.P_BAT"))
        ck("该点被标注 word-swap", any(
            d["note"] == "word-swap"
            for f in frames for d in _decoded_of(f, "MEAS.P_BAT")))
        ck("解出 14 个 CFG 点中的 CFG.TRANSFORMER_KVA = 250", any(
            d["name"] == "CFG.TRANSFORMER_KVA" and abs(d["value"] - 250.0) < 1e-3
            for f in frames for d in _decoded_of(f, "CFG.TRANSFORMER_KVA")))

        case("L03 FC02 离散输入按位解码")
        raw, _dt = m.read(2, 0, 8)
        frames, _ = _frames(ctl_port)
        f2 = [f for f in frames if f["text"].startswith("返回 1 字节位")]
        ck("存在位读响应帧", bool(f2))
        ck("解出 STA.BMS_DIS_FORBID = 1", any(
            d["name"] == "STA.BMS_DIS_FORBID" and d["value"] == 1
            for f in f2 for d in _decoded_of(f, "STA.BMS_DIS_FORBID")))
        ck("解出 STA.DATA_VALID = 1", any(
            d["name"] == "STA.DATA_VALID" and d["value"] == 1
            for f in f2 for d in _decoded_of(f, "STA.DATA_VALID")))

        # ---------------- L04 延迟 ----------------
        case("L04 运行时注入「回包延迟」：主站实测往返 ≥ 指定值")
        api(ctl_port, "/api/clear", {})   # POST；先清基线，否则 p50 被正常样本稀释
        api(ctl_port, "/api/perturb", {"delay_ms": 250.0})
        _regs, dt = m.read_regs(4, 0, 12)
        ck("主站实测往返 ≥ 250 ms", dt >= 250.0, "实测 %.1f ms" % dt)
        st = api(ctl_port, "/api/status")
        ck("中间人记录到样本", st["latency"]["n"] >= 1, json.dumps(st["latency"]))
        ck("中间人时延口径 = 上游往返 + 注入延迟（与主站实测差 < 120 ms）",
           abs(st["latency"]["p50"] - dt) < 120.0,
           "中间人 %.1f vs 主站 %.1f" % (st["latency"]["p50"], dt))
        api(ctl_port, "/api/perturb", {"delay_ms": 0.0})

        # ---------------- L05 丢包 ----------------
        case("L05 运行时注入「丢包」：响应被丢，主站超时")
        before = api(ctl_port, "/api/status")["stats"]["dropped"]
        api(ctl_port, "/api/perturb", {"drop_every": 1})
        timed_out = False
        try:
            m.read(4, 0, 12, timeout=0.7)
        except (socket.timeout, IOError):
            timed_out = True
        ck("主站读到超时（异常被观测到，不是静默通过）", timed_out)
        after = api(ctl_port, "/api/status")["stats"]["dropped"]
        ck("丢弃计数增加", after > before, "%d → %d" % (before, after))
        api(ctl_port, "/api/perturb", {"drop_every": 0})

        # ---------------- L06 黑障与恢复（发生→持续→恢复） ----------------
        case("L06 黑障：瞬断后自动恢复（不是锁存）")
        api(ctl_port, "/api/perturb", {"blackout_ms": 500.0, "delay_ms": 0.0})
        down = False
        try:
            m.read(4, 0, 12, timeout=0.6)
        except (socket.timeout, IOError):
            down = True
        ck("黑障期间请求被丢弃（主站超时）", down)
        time.sleep(0.8)
        ok = False
        try:
            _r, dt = m.read_regs(4, 0, 12, timeout=2.0)
            ok = dt < 200.0
        except (socket.timeout, IOError):
            pass
        ck("黑障结束后通信自动恢复", ok)

        # ---------------- L07 字序注入 ----------------
        case("L07 测点注入：故意翻转字序（文章里那个天文数字的坑）")
        base, _dt = m.read_regs(4, 0, 12)
        p_bat_base = base[4:6]
        ck("注入前 MEAS.P_BAT 两字 = 低字在前编码",
           slv.regs_to_f32(p_bat_base, low_word_first=True) == -50.0,
           str(p_bat_base))
        api(ctl_port, "/api/perturb", {"scenario": "wordflip"})
        inj, _dt = m.read_regs(4, 0, 12)
        ck("注入后寄存器字节确实变了", inj[4:6] != p_bat_base,
           "%s → %s" % (p_bat_base, inj[4:6]))
        ck("两字恰好是对调，值本身没被改动",
           inj[4:6] == [p_bat_base[1], p_bat_base[0]],
           "%s → %s" % (p_bat_base, inj[4:6]))
        wrong = slv.regs_to_f32(inj[4:6], low_word_first=True)
        right = slv.regs_to_f32(inj[4:6], low_word_first=False)
        # ★ 字序错的**症状不止一种**：可能是天文数字、可能是 NaN、也可能是一个
        #   合法的极小非规格化数（本例 -50 交换后得到约 7e-41）。
        #   ⇒ 光靠"非有限 / 值过大"检不出来，必须再加**量程合理性**判断。
        #   这条对主站侧的 kNonFinite 守卫同样成立：它只挡 NaN/Inf，
        #   挡不住 7e-41 这种"看着像正常小数"的错值。
        ck("按该点应有字序解出的**不是**真值 -50（差 > 1 kW）",
           abs(wrong + 50.0) > 1.0, "解出 %r" % wrong)
        ck("该错值落在一个不可能的量程里（|值| < 1e-3 或 > 1e3 或非有限）",
           (not _finite(wrong)) or abs(wrong) > 1e3 or abs(wrong) < 1e-3,
           "解出 %r" % wrong)
        ck("按相反字序解出的才是原值 -50（证明就是字序被换了）",
           abs(right + 50.0) < 1e-3, "解出 %r" % right)
        api(ctl_port, "/api/perturb", {"scenario": "reset"})
        back, _dt = m.read_regs(4, 0, 12)
        ck("reset 后恢复原值", back[4:6] == p_bat_base, str(back[4:6]))

        # ---------------- L08 NaN 注入 ----------------
        case("L08 测点注入：塞入 IEEE NaN（打 kNonFinite 守卫）")
        # ★ raw_words 必须按**该点自己的字序**给：MEAS.P_BAT 低字在前，
        #   所以 NaN（0x7FC00000）在线上是 [低字 0x0000, 高字 0x7FC0]。
        #   写成 [0x7FC0, 0x0000] 会得到一个合法的小数（约 5e-41），
        #   断言会莫名失败而看不出原因。
        api(ctl_port, "/api/inject",
            {"MEAS.P_BAT": {"raw_words": [0x0000, 0x7FC0]}})
        nan_regs, _dt = m.read_regs(4, 0, 12)
        ck("寄存器里放的是该点字序下的 NaN 两字",
           nan_regs[4:6] == [0x0000, 0x7FC0], str(nan_regs[4:6]))
        ck("按该点自身字序重组的正是 NaN 位型 0x7FC00000",
           not _finite(slv.regs_to_f32(nan_regs[4:6], low_word_first=True)),
           repr(slv.regs_to_f32(nan_regs[4:6], low_word_first=True)))
        frames, _ = _frames(ctl_port)
        ck("中间人解码显示为 NaN（不会被伪装成正常值）", any(
            d["name"] == "MEAS.P_BAT" and (not _finite(d["value"]))
            for f in frames for d in _decoded_of(f, "MEAS.P_BAT")),
           _dump_decoded(frames, "MEAS.P_BAT"))
        api(ctl_port, "/api/inject", {"clear": True})

        # ---------------- L09 真实半包 ----------------
        case("L09 真实半包：响应拆成两次 TCP 写，主站仍能组帧")
        api(ctl_port, "/api/perturb", {"half_packet": True})
        regs, dt = m.read_regs(4, 0, 12, timeout=3.0)
        ck("半包下仍读到完整 12 个寄存器", len(regs) == 12)
        ck("半包下 MEAS.P_GRID 仍正确", abs(slv.regs_to_f32(regs[6:8]) - 232.0) < 1e-3)
        frames, _ = _frames(ctl_port)
        ck("中间人记录了半包事件", any(f["dir"] == "INFO" for f in frames))
        api(ctl_port, "/api/perturb", {"half_packet": False})

        # ---------------- L10 时间轴：一次会话跑完 发生→持续→恢复 ----------------
        case("L10 时间轴：工况在一次会话内自动演进")
        api(ctl_port, "/api/timeline", {"steps": [
            {"at_s": 0.3, "delay_ms": 350.0},
            {"at_s": 1.6, "delay_ms": 0.0},
        ]})
        t0 = time.monotonic()
        _r, dt_early = m.read_regs(4, 0, 12, timeout=3.0)
        ck("t≈0s 时无延迟（<150 ms）", dt_early < 150.0, "%.1f ms" % dt_early)
        time.sleep(max(0.0, 0.45 - (time.monotonic() - t0)))
        _r, dt_mid = m.read_regs(4, 0, 12, timeout=3.0)
        ck("t≈0.5s 时延迟已生效（≥350 ms）", dt_mid >= 350.0, "%.1f ms" % dt_mid)
        time.sleep(max(0.0, 1.75 - (time.monotonic() - t0)))
        _r, dt_late = m.read_regs(4, 0, 12, timeout=3.0)
        ck("t≈1.8s 时延迟已恢复（<150 ms）", dt_late < 150.0, "%.1f ms" % dt_late)
        st = api(ctl_port, "/api/status")
        ck("时间轴步进留痕在 notes 里", any("t+" in n for n in st["notes"]),
           str(st["notes"]))

        # ---------------- L11 黄金向量可回放 ----------------
        case("L11 帧级黄金向量：录制内容可逐字段复核")
        ck("录制文件已产生", os.path.exists(rec_path) and os.path.getsize(rec_path) > 0)
        rows = _read_jsonl(rec_path)
        ck("录制的每行都有 seq/dir/hex 三要素",
           rows and all(("seq" in r and "dir" in r and "hex" in r) for r in rows),
           "%d 行" % len(rows))
        reqs = [r for r in rows if r["dir"] == "REQ"]
        ck("录到的请求帧可解析出功能码", all(
            struct.unpack(">B", bytes.fromhex(r["hex"])[7:8])[0] in (1, 2, 3, 4, 5, 6, 15, 16)
            for r in reqs), "%d 个请求帧" % len(reqs))
        ck("录到的请求含 FC16（写指令）",
           any(bytes.fromhex(r["hex"])[7] == 16 for r in reqs))
        # 这条断言是回归保护：录制曾经用"轮询帧缓冲"实现，被 /api/clear 一清
        # 就**永久静默失效**（长度永远回不到旧水位）。L07 的注入发生在清空之后，
        # 它能出现在录制里，就证明推送式录制不再受清空影响。
        ck("录制覆盖到清空之后的注入帧（推送式录制不受 /api/clear 影响）",
           any("注入" in (r.get("note") or "") for r in rows),
           "共 %d 行，%d 个请求" % (len(rows), len(reqs)))

        m.close()
        m = None

        # ---------------- L12 上游不可达：快速失败不挂住 ----------------
        case("L12 上游从站不可达时：不挂住，快速失败")
        dead, l2, c2 = free_ports(3)
        tap2 = Tap(l2, dead, c2)
        try:
            ck("第二个中间人实例就绪", tap2.ready)
            t0 = time.monotonic()
            closed = False
            try:
                mm = Master(l2)
                mm.sock.settimeout(8.0)
                try:
                    # recv 返回空 = 对端主动关闭；超时 = 干等（不想要的）
                    closed = (mm.sock.recv(64) == b"")
                except socket.timeout:
                    closed = False
                except OSError:
                    closed = True
                mm.close()
            except Exception:
                closed = True
            dt = (time.monotonic() - t0) * 1000.0
            # ★ 判据用"不等主站自己的超时"，不用"几毫秒内"：中间人对上游的连接
            #   超时是 2 s（--upstream-timeout），且某些环境下系统会对不可达端口
            #   直接丢包而不是回 RST，所以"立刻关闭"不是可靠现象。真正要保证的是
            #   "中间人自己认账并关门"，而不是"让主站一直等到自己超时"。
            ck("连接被中间人主动关闭（不是干等到主站超时）", closed, "%.0f ms" % dt)
            ck("关闭发生在 4 s 内", dt < 4000.0, "%.0f ms" % dt)
            err = False
            for _ in range(10):
                frames2, _ = _frames(c2)
                if any(f["dir"] == "ERR" for f in frames2):
                    err = True
                    break
                time.sleep(0.2)
            ck("留下 ERR 记录（说明是上游不可达，不是被当成主站超时）", err,
               str([f["dir"] for f in _frames(c2)[0]]))
        finally:
            tap2.stop()

    finally:
        if m:
            m.close()
        tap.stop()
        slave._halt = True

    print("\n" + "=" * 72)
    print("  PASS=%d FAIL=%d" % (PASS, FAIL))
    if FAIL == 0:
        print("  ALL TESTS PASSED")
    print("=" * 72)
    return 0 if FAIL == 0 else 1


# ---- 小工具 ----
def _frames(ctl_port, n=400):
    """取帧，并把 decoded 文本**重新拆成结构化**便于断言。

    ★ 为什么中间人不直接返回结构化：监视台只需要字符串，每帧多带一份
      结构化明细会让帧体积翻倍。测试端按需重建即可 —— 重建用的切分很窄
      （点名=值，值里不可能出现空格或竖线），不会误切。
    """
    raw = api(ctl_port, "/api/frames?n=%d" % n)["frames"]
    for f in raw:
        dec = []
        for part in (f.get("decoded") or "").split(" | "):
            part = part.replace(" ★", "").strip()
            if "=" not in part:
                continue
            k, _, v = part.partition("=")
            note = ""
            if v.endswith("word-swap"):
                v = v[:-len("word-swap")].strip()
                note = "word-swap"
            if v == "NaN":
                fv = float("nan")
            elif v == "Inf":
                fv = float("inf")
            elif v == "-Inf":
                fv = float("-inf")
            else:
                try:
                    fv = float(v)
                except ValueError:
                    fv = v
            dec.append({"name": k.strip(), "value": fv, "note": note})
        f["_dec"] = dec
    return raw, None


def _decoded_of(f, name):
    return [d for d in (f.get("_dec") or []) if d["name"] == name]


def _dump_decoded(frames, name):
    for f in frames:
        for d in _decoded_of(f, name):
            return "seq%d %s=%r" % (f["seq"], name, d["value"])
    return "(未找到 %s 的解码项)" % name


def _reparse(hexs):
    try:
        raw = bytes.fromhex(hexs)
        if len(raw) < 7:
            return False
        tid, pid, length = struct.unpack(">HHH", raw[:6])
        return pid == 0 and len(raw) == 6 + length
    except Exception:
        return False


def _finite(v):
    if isinstance(v, float):
        return v == v and abs(v) != float("inf")
    return True


def _read_jsonl(path):
    rows = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def _html_ok(ctl_port):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d/" % ctl_port, timeout=5) as r:
            body = r.read().decode("utf-8", "replace")
        return "报文监视" in body and "/api/status" in body
    except Exception:
        return False


if __name__ == "__main__":
    raise SystemExit(main())
