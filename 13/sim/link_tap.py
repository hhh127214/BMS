#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
13/sim — link_tap：链路中间人（报文监视 + 链路扰动 + 测点注入，**运行时可控**）

--------------------------------------------------------------------------
它为什么存在（现有资产覆盖不到的三件事）
--------------------------------------------------------------------------
已经有两条保证链，本工具**不重复**它们：

    13/src/fake_modbus_slave.h   进程内假从站 → "我们的客户端符不符合协议"
    13/sim/modbus_slave.py       跨进程真从站 → "两个独立实现能不能互通"
    13/sim/link_tap.py           链路中间人   → "链路不正常时两端各是什么行为"

① **报文级可见性。** 从站日志只知道"被轮询了 N 次"，主站只知道自己报了错。
   中间那一帧**到底长什么样**没人看得见 —— 而字序 / 缩放这类问题恰恰只能从
   帧上看出：值变成天文数字或 NaN 时，两端日志里都写着"成功"。

② **运行时改工况。** modbus_slave.py 的 --bms-dis-forbid / --pcs-fault 是
   **整轮固定**的：换工况要重启进程。于是"发生 → 持续 → 恢复"这种时间窗内的
   演进**在一次会话里复现不出来** —— 而 EMS 的故障模型恰好是时间窗表达的
   （07/src 的 apply_fault 就是"发生→持续→恢复"）。本工具让工况在跑动中切换。

③ **异常链路可复现。** 间歇性丢包、回包延迟忽高忽低、瞬断黑障：现场偶发、
   实验室复现不出来，于是这类边界永远测不到。本工具把它们变成一条命令。

--------------------------------------------------------------------------
设计：透明中间人，两端零改动
--------------------------------------------------------------------------
        C++ 主站 ──► link_tap :15021 ──► modbus_slave.py :15020

主站连的是 tap 的端口，从站还是原来那个从站。**被测对象一行都不用改**，
所以这里加的东西不会污染被测代码 —— 测的仍然是 13/src 里那份真实实现。
反过来也成立：tap 自己坏掉时，拔掉它（主站直接连从站）即可隔离。

**点表只有两份**（C++ 一份、Python 从站一份，见 modbus_slave.py 顶部关于
"为什么不做成自动生成"的说明）。本工具不新增第三份：它直接 import 从站的
point_table() 当解码依据 —— 帧上显示的工程量因此恒与从站口径一致。

--------------------------------------------------------------------------
用法
--------------------------------------------------------------------------
    :: 窗口 1 —— 设备替身
    .venv\\Scripts\\python.exe sim\\modbus_slave.py --port 15020 --load 380 --pv 150

    :: 窗口 2 —— 链路中间人（主站改连 15021）
    .venv\\Scripts\\python.exe sim\\link_tap.py --listen 15021 --upstream 15020

    :: 窗口 3 —— 被测主站，连的是中间人的端口
    build\\modbus_probe.exe --port 15021 --repeat 20 --verbose

    :: 监视台（浏览器）：http://127.0.0.1:15022/

    :: 跑动中注入工况（不用重启任何东西）
    .venv\\Scripts\\python.exe sim\\link_tap.py --ctl scenario blackout   # 瞬断→恢复
    .venv\\Scripts\\python.exe sim\\link_tap.py --ctl perturb delay_ms=300
    .venv\\Scripts\\python.exe sim\\link_tap.py --ctl status

--------------------------------------------------------------------------
测点注入的三种写法（都是运行时热生效，两端代码一行不动）
--------------------------------------------------------------------------
    MEAS.P_GRID=999              按点表口径重新编码一个值（最常用）
    MEAS.P_BAT=flip              只把该点两个寄存器的字序**对调**，值不变
                                 —— 这才是字序坑的真实形态：值是对的，只是
                                 两端对"哪个字在前"的理解不一致
    MEAS.P_BAT=nan               注入 IEEE NaN（打主站的 kNonFinite 守卫）
    "MEAS.P_BAT=raw:0x0000,0x7FC0"   直接给寄存器裸值（**按该点自己的字序**，
                                 低字在前的点要把高字写在第二个）
    clear                        清空全部注入

    :: 录制帧级黄金向量（每行一个 JSON，含完整 hex）
    .venv\\Scripts\\python.exe sim\\link_tap.py --listen 15021 --record build\\golden.jsonl
"""

import argparse
import json
import os
import random
import selectors
import socket
import struct
import sys
import threading
import time
import urllib.error
import urllib.request
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

# ★ 复用从站的点表，**不新增第三份手写表**。
#   为什么可以 import：modbus_slave.py 对 pymodbus 是惰性导入（缺库时
#   HAVE_PYMODBUS=False 但模块照常加载），而点表 / 编码工具不依赖 pymodbus。
#   这条边界很重要 —— 否则"没装 pymodbus"会被误读成"点表读不出来"。
import modbus_slave as slv  # noqa: E402


# =====================================================================
# 点表镜像：把寄存器里的裸值翻译成工程量
# =====================================================================
class Point(object):
    """一个测点的解码/编码口径。语义与 modbus_point_map.h 的 PointBinding 一致。"""

    __slots__ = ("idx", "name", "table", "addr", "enc", "wo", "scale", "writable")

    def __init__(self, idx, name, table, addr, enc, wo, scale, writable):
        self.idx = idx
        self.name = name
        self.table = table          # 'IR' / 'HR' / 'DI'
        self.addr = addr
        self.enc = enc              # 'f32' / 'u16' / 'i16' / 'bit'
        self.wo = wo                # 'AB' / 'BA' / '-'
        self.scale = scale
        self.writable = writable

    @property
    def width(self):
        """占几个寄存器。位点不占寄存器，返回 0。"""
        if self.enc == "bit":
            return 0
        return 2 if self.enc == "f32" else 1

    def decode(self, words):
        """words → (工程量, 备注)。备注用来标出"这个值是换过字序的"。"""
        if self.enc == "f32":
            v = slv.regs_to_f32(words, self.wo == "BA")
            note = "word-swap" if self.wo == "BA" else ""
            return v, note
        if self.enc == "u16":
            return words[0] / self.scale, ""
        if self.enc == "i16":
            raw = words[0]
            if raw >= 0x8000:
                raw -= 0x10000
            return raw / self.scale, ""
        return words[0], ""

    def encode(self, value, flip=False):
        """工程量 → 寄存器。flip=True 时**故意用相反字序**（专测主站的字序守卫）。"""
        if self.enc == "f32":
            low = self.wo == "BA"
            if flip:
                low = not low
            return slv.f32_to_regs(value, low)
        if self.enc == "u16":
            return [slv.u16_scaled(value, self.scale)]
        if self.enc == "i16":
            return [slv.i16_scaled(value, self.scale)]
        raise ValueError("bit 点请走 DI 注入通道：%s" % self.name)


_POINTS = [Point(*row) for row in slv.point_table()]
_BY_NAME = dict((p.name, p) for p in _POINTS)
_REG_AT = {}    # (table, addr) -> Point
_BIT_AT = {}    # (table, addr) -> Point
for _p in _POINTS:
    if _p.enc == "bit":
        _BIT_AT[(_p.table, _p.addr)] = _p
    else:
        _REG_AT[(_p.table, _p.addr)] = _p


def decode_regs(table, start, words):
    """把一段连续寄存器的读数翻译成 [(Point, 值, 备注)]。

    ★ 刻意**只在整点落在区间内时才解码**：跨越读块边界时只解半边会给出
      一个看着合理、实际是拼出来的数 —— 那比不显示更危险。
    """
    out = []
    for (tbl, addr) in sorted(_REG_AT):
        if tbl != table:
            continue
        p = _REG_AT[(tbl, addr)]
        w = p.width
        if addr < start or addr + w > start + len(words):
            continue
        out.append((p,) + p.decode(list(words[addr - start:addr - start + w])))
    return out


def decode_bits(start, payload):
    """离散输入响应 → [(Point, 0/1)]。位在字节内 **LSB 在前**（Modbus 规定）。"""
    out = []
    for (tbl, addr) in sorted(_BIT_AT):
        if tbl != "DI":
            continue
        off = addr - start
        if off < 0:
            continue
        byte_i, bit_i = off // 8, off % 8
        if byte_i >= len(payload):
            continue
        out.append((_BIT_AT[(tbl, addr)], (payload[byte_i] >> bit_i) & 1))
    return out


# =====================================================================
# MBAP 组帧
# =====================================================================
class Frame(object):
    __slots__ = ("tid", "pid", "length", "unit", "pdu", "raw")

    def __init__(self, raw):
        self.raw = raw
        self.tid, self.pid, self.length, self.unit = struct.unpack(">HHHB", raw[:7])
        self.pdu = raw[7:]


class Framer(object):
    """TCP 字节流 → MBAP 帧。半包/粘包都在这里被吸收。"""

    __slots__ = ("buf", "bad")

    def __init__(self):
        self.buf = b""
        self.bad = 0     # 无法同步的字节数（恶帧要看得见，不能静默丢）

    def feed(self, data):
        self.buf += data
        out = []
        while len(self.buf) >= 7:
            tid, pid, length = struct.unpack(">HHH", self.buf[:6])
            # MBAP 合法性：协议号固定 0；长度域含 unit(1)+PDU，PDU 最长 253。
            if pid != 0 or length < 2 or length > 254:
                self.buf = self.buf[1:]
                self.bad += 1
                continue
            total = 6 + length
            if len(self.buf) < total:
                break
            out.append(Frame(self.buf[:total]))
            self.buf = self.buf[total:]
        return out


FC_NAME = {
    1: "读线圈", 2: "读离散输入", 3: "读保持寄存器", 4: "读输入寄存器",
    5: "写单线圈", 6: "写单寄存器", 15: "写多线圈", 16: "写多寄存器",
}
FC_TABLE = {1: "CO", 2: "DI", 3: "HR", 4: "IR", 5: "CO", 6: "HR", 15: "CO", 16: "HR"}
EXC_NAME = {
    1: "非法功能", 2: "非法地址", 3: "非法数据值", 4: "从站故障",
    5: "确认", 6: "从站忙", 8: "存储奇偶错", 10: "网关路径不可用", 11: "网关无响应",
}


def describe_request(pdu):
    """请求 PDU → (功能码, 人话, 关键字段)。"""
    if not pdu:
        return None, "空 PDU", {}
    fc = pdu[0]
    if fc in (1, 2, 3, 4) and len(pdu) >= 5:
        a, q = struct.unpack(">HH", pdu[1:5])
        tbl = FC_TABLE[fc]
        return fc, "%s %s[%d..%d] %d 个" % (FC_NAME[fc], tbl, a, a + q - 1, q), \
            {"start": a, "qty": q, "table": tbl}
    if fc in (5, 6) and len(pdu) >= 5:
        a, v = struct.unpack(">HH", pdu[1:5])
        return fc, "%s %s[%d] = %d (0x%04X)" % (FC_NAME[fc], FC_TABLE[fc], a, v, v), \
            {"start": a, "qty": 1, "table": FC_TABLE[fc], "write": True}
    if fc in (15, 16) and len(pdu) >= 6:
        a, q, bc = struct.unpack(">HHB", pdu[1:6])
        return fc, "%s %s[%d] 起 %d 个（%d 字节）" % (FC_NAME[fc], FC_TABLE[fc], a, q, bc), \
            {"start": a, "qty": q, "table": FC_TABLE[fc], "write": True}
    if fc >= 0x80:
        code = pdu[1] if len(pdu) > 1 else None
        return fc, "异常响应 码=%s" % code, {"exception": code}
    return fc, "功能码 %d" % fc, {}


def describe_response(pdu, ctx):
    """响应 PDU → (人话, 解码明细字符串, 结构化字段)。"""
    if not pdu:
        return "空 PDU", "", {}
    fc = pdu[0]
    if fc >= 0x80:
        code = pdu[1] if len(pdu) > 1 else None
        return "异常 %s %s" % (code, EXC_NAME.get(code, "")), "", {"exception": code}
    if fc in (3, 4):
        if len(pdu) < 2:
            return "畸形响应（缺字节数）", "", {}
        bc = pdu[1]
        words = list(struct.unpack(">%dH" % (bc // 2), pdu[2:2 + bc])) if bc else []
        tbl = ctx.get("table", "IR" if fc == 4 else "HR")
        start = ctx.get("start", 0)
        dec = decode_regs(tbl, start, words)
        pretty = " | ".join(
            "%s=%s%s" % (p.name, _fmt(v), (" " + n) if n else "")
            for (p, v, n) in dec)
        return "返回 %d 个寄存器" % (bc // 2), pretty, \
            {"words": words, "start": start, "table": tbl,
             "decoded": [{"name": p.name, "value": v, "note": n} for (p, v, n) in dec]}
    if fc in (1, 2):
        bc = pdu[1] if len(pdu) > 1 else 0
        payload = pdu[2:2 + bc]
        start = ctx.get("start", 0)
        dec = decode_bits(start, payload)
        pretty = " | ".join("%s=%d" % (p.name, v) for (p, v) in dec)
        return "返回 %d 字节位" % bc, pretty, \
            {"bits": payload.hex(), "start": start,
             "decoded": [{"name": p.name, "value": v, "note": ""} for (p, v) in dec]}
    if fc in (5, 6, 15, 16):
        return "写确认（回显请求）", "", {}
    return "功能码 %d" % fc, "", {}


def _fmt(v):
    if v is None:
        return "None"
    if isinstance(v, float):
        if v != v:
            return "NaN"
        if v in (float("inf"), float("-inf")):
            return "Inf"
        return "%.4g" % v
    return str(v)


# =====================================================================
# 工况预设：都设计成"跑动中生效"，用来在一次会话里跑完 发生→持续→恢复
# =====================================================================
SCENARIOS = {
    # 固定参数类
    "delay":    {"perturb": {"delay_ms": 300.0, "jitter_ms": 0.0, "drop_every": 0}},
    "jitter":   {"perturb": {"delay_ms": 50.0, "jitter_ms": 250.0, "drop_every": 0}},
    "drop":     {"perturb": {"drop_every": 3}},
    "split":    {"perturb": {"half_packet": True}},

    # 时间轴类：t=2s 断链，持续 3s，t=6s 恢复 —— 完整的"发生→持续→恢复"
    "blackout": {"perturb": {}, "timeline": [
        {"at_s": 2.0, "blackout_ms": 3000.0},
        {"at_s": 6.0, "clear_blackout": True},
    ]},
    # 间歇性断连：反复断-通，专治"现场偶发"
    "flap": {"perturb": {}, "timeline": [
        {"at_s": 1.0, "blackout_ms": 900.0},
        {"at_s": 3.0, "blackout_ms": 900.0},
        {"at_s": 5.0, "blackout_ms": 900.0},
        {"at_s": 7.0, "clear_blackout": True},
    ]},
    # 字序翻转：把 MEAS.P_BAT 按**相反字序**编码 —— 这正是文章里那个
    # "ABCD / DCBA 搞反了，读出来是天文数字"的坑，用来打主站的解码守卫。
    "wordflip": {"perturb": {}, "timeline": [
        {"at_s": 0.0, "inject": {"MEAS.P_BAT": {"flip": True}}},
    ]},
    # 非法浮点：直接塞 IEEE NaN 的两个字 —— 打的是 kNonFinite 守卫。
    # ★ 字序必须按**该点自己的**口径给：MEAS.P_BAT 是低字在前，所以
    #   0x7FC00000 在线上是 [低字 0x0000, 高字 0x7FC0]。按 AB 的顺序写成
    #   [0x7FC0, 0x0000] 会得到一个合法的极小非规格化数（约 5e-41），
    #   于是"注入 NaN"变成了"注入一个正常的小数"，断言会莫名其妙地失败。
    "nan": {"perturb": {}, "timeline": [
        {"at_s": 0.0, "inject": {"MEAS.P_BAT": {"raw_words": [0x0000, 0x7FC0]}}},
    ]},

    # 全部归零（含黑障与注入）
    "reset": {"perturb": {
        "delay_ms": 0.0, "jitter_ms": 0.0, "drop_every": 0, "drop_seq": 0,
        "req_delay_ms": 0.0, "blackout_until": 0.0, "freeze": False,
        "half_packet": False,
    }, "timeline": [], "inject_clear": True},
}


# =====================================================================
# 控制状态（HTTP 线程与链路线程共享，全部走这个锁）
# =====================================================================
class Ctl(object):
    def __init__(self, max_frames=2000):
        self.lock = threading.RLock()
        self.frames = deque(maxlen=max_frames)
        self.seq = 0
        self.t0 = time.monotonic()
        self.perturb = {
            "delay_ms": 0.0,       # 响应方向固定延迟
            "jitter_ms": 0.0,      # 响应方向随机附加延迟 [0, jitter]
            "req_delay_ms": 0.0,   # 请求方向固定延迟
            "drop_every": 0,       # 每 N 帧丢一帧（0=不丢）
            "drop_seq": 0,         # 计数器
            "blackout_until": 0.0, # 黑障截止（monotonic 秒）
            "freeze": False,       # 完全不转发（比黑障更彻底）
            "half_packet": False,  # 把响应拆成两次 TCP 写（真实半包）
        }
        self.inject = {}           # name -> {'value'|'raw_words'|'flip'}
        self.prev_words = {}       # (table, addr) -> 上一帧的寄存器值（供"写后校验"）
        self.timeline = []
        self.timeline_t0 = None
        self.timeline_done = 0
        self.stats = {"total": 0, "req": 0, "rsp": 0, "dropped": 0,
                      "errors": 0, "conns": 0, "injected": 0, "bad_bytes": 0}
        self.lat = deque(maxlen=4000)
        self.notes = []
        self.sink = None           # 录制回调（见 record 里的说明）

    # ---- 记录 -------------------------------------------------------
    # ★ 录制走**推送**（sink 回调）而不是轮询帧缓冲：
    #   轮询有两个致命缺陷 —— ① `/api/clear` 把缓冲清空后，轮询用"长度是否变长"
    #   判断新帧，长度永远回不到旧水位，录制**永久静默失效**；
    #   ② 环形缓冲写满后会丢最老的帧，轮询会漏掉它们而毫无察觉。
    #   这两种失效都不报错，只会让"录制的黄金向量"少帧 —— 而少帧的黄金向量
    #   是最糟的：拿它当基线，会把真实缺陷当成"历史行为"放过。
    def _emit(self, entry):
        sink = self.sink
        if sink is not None:
            try:
                sink(entry)
            except Exception:
                pass

    def record(self, direction, frame, text, decoded="", latency=None, note=""):
        with self.lock:
            self.seq += 1
            self.stats["total"] += 1
            if direction == "REQ":
                self.stats["req"] += 1
            elif direction == "RSP":
                self.stats["rsp"] += 1
            entry = {
                "seq": self.seq,
                "t": time.monotonic() - self.t0,
                "dir": direction,
                "tid": frame.tid if frame else None,
                "unit": frame.unit if frame else None,
                "length": frame.length if frame else None,
                "text": text,
                "decoded": decoded,
                "latency_ms": latency,
                "hex": frame.raw.hex() if frame else "",
                "note": note,
            }
            self.frames.append(entry)
        self._emit(entry)

    def record_event(self, direction, text, note="", hex_=""):
        with self.lock:
            self.seq += 1
            self.stats["total"] += 1
            if direction in ("DROP", "ERR"):
                self.stats["dropped" if direction == "DROP" else "errors"] += 1
            entry = {
                "seq": self.seq, "t": time.monotonic() - self.t0, "dir": direction,
                "tid": None, "unit": None, "length": None, "text": text,
                "decoded": "", "latency_ms": None, "hex": hex_, "note": note,
            }
            self.frames.append(entry)
        self._emit(entry)

    # ---- 扰动闸门 ---------------------------------------------------
    def gate(self, direction):
        """返回 (是否放行, 附加延迟秒, 原因)。

        ★ 这里**只做判定，不动计数**。曾经在三个分支里各 +=1 一次 dropped，
          而调用方丢弃时还会再记一条 DROP 事件（那条也 +=1），于是同一个丢弃
          被算了两遍 —— 统计里 dropped=4、实际只丢了 2 帧。
          计数只能有一个归属点：`record_event`。判定函数保持无副作用。
        """
        with self.lock:
            now = time.monotonic()
            p = self.perturb
            if p["freeze"]:
                return False, 0.0, "freeze"
            if now < p["blackout_until"]:
                return False, 0.0, "blackout(%.1fs 剩余)" % (p["blackout_until"] - now)
            if p["drop_every"] > 0 and direction == "RSP":
                p["drop_seq"] += 1
                if p["drop_seq"] % p["drop_every"] == 0:
                    return False, 0.0, "drop_every=%d" % p["drop_every"]
            d = p["req_delay_ms"] if direction == "REQ" else p["delay_ms"]
            if direction == "RSP" and p["jitter_ms"] > 0:
                d += random.uniform(0.0, p["jitter_ms"])
            return True, d / 1000.0, ""

    # ---- 测点注入 ---------------------------------------------------
    def rewrite(self, frame, ctx):
        """在飞行中改写响应帧里的寄存器。返回 (新原始字节, 备注) 或 (None, 原因)。"""
        with self.lock:
            if not self.inject:
                return None, ""
            pdu = bytearray(frame.pdu)
            if not pdu:
                return None, ""
            fc = pdu[0]
            touched = []

            if fc in (3, 4):
                bc = pdu[1]
                if len(pdu) < 2 + bc:
                    return None, ""
                table = ctx.get("table", "IR" if fc == 4 else "HR")
                start = ctx.get("start", 0)
                for name, spec in self.inject.items():
                    p = _BY_NAME.get(name)
                    if not p or p.table != table or p.enc == "bit":
                        continue
                    off = p.addr - start
                    if off < 0 or off + p.width > bc // 2:
                        continue
                    base = 2 + off * 2
                    try:
                        if spec.get("flip") and "raw_words" not in spec \
                                and "value" not in spec:
                            # ★ 「只翻转、不给值」= 就地把两个寄存器的字序对调。
                            #   这才是字序坑的**真实形态**：值本身是对的，只是
                            #   两端对"哪个字在前"的理解不一致。等价于用相反字序
                            #   重编码同一个值，但不需要事先知道那个值是多少。
                            if p.width != 2:
                                continue
                            w0, w1 = struct.unpack_from(">HH", pdu, base)
                            struct.pack_into(">HH", pdu, base, w1, w0)
                        elif "raw_words" in spec:
                            words = list(spec["raw_words"])[:p.width]
                            for k, w in enumerate(words):
                                struct.pack_into(">H", pdu, base + k * 2, w & 0xFFFF)
                        else:
                            words = p.encode(float(spec.get("value", 0.0)),
                                             bool(spec.get("flip", False)))
                            for k, w in enumerate(words):
                                struct.pack_into(">H", pdu, base + k * 2, w & 0xFFFF)
                    except (ValueError, struct.error) as e:
                        return None, "注入失败 %s: %s" % (name, e)
                    touched.append(name)
                    self.stats["injected"] += 1

            elif fc == 2:
                bc = pdu[1] if len(pdu) > 1 else 0
                start = ctx.get("start", 0)
                for name, spec in self.inject.items():
                    p = _BY_NAME.get(name)
                    if not p or p.enc != "bit":
                        continue
                    off = p.addr - start
                    if off < 0 or 2 + off // 8 >= 2 + bc:
                        continue
                    # ★ 必须直接改 pdu：`pdu[2:2+bc]` 返回的是**副本**，
                    #   在副本上置位不会写回帧里（改了等于没改，而且不报错）。
                    b, bit = 2 + off // 8, off % 8
                    if spec.get("value"):
                        pdu[b] = (pdu[b] | (1 << bit)) & 0xFF
                    else:
                        pdu[b] = pdu[b] & (~(1 << bit) & 0xFF)
                    touched.append(name)
                    self.stats["injected"] += 1
            else:
                return None, ""

            if not touched:
                return None, ""
            raw = frame.raw[:7] + bytes(pdu)
            return raw, "注入 " + ",".join(touched)

    # ---- 时间轴 -----------------------------------------------------
    def start_timeline(self, steps):
        with self.lock:
            self.timeline = list(steps)
            self.timeline_t0 = time.monotonic()
            self.timeline_done = 0
            self.notes.append("时间轴 %d 步已装载" % len(steps))

    def tick_timeline(self):
        with self.lock:
            if self.timeline_t0 is None:
                return
            el = time.monotonic() - self.timeline_t0
            while self.timeline_done < len(self.timeline):
                step = self.timeline[self.timeline_done]
                if el < float(step.get("at_s", 0.0)):
                    break
                self._apply_step(step)
                self.timeline_done += 1

    def _apply_step(self, step):
        label = []
        for k in ("delay_ms", "jitter_ms", "req_delay_ms", "drop_every", "freeze",
                  "half_packet"):
            if k in step:
                self.perturb[k] = step[k]
                label.append("%s=%s" % (k, step[k]))
        if "blackout_ms" in step:
            self.perturb["blackout_until"] = time.monotonic() + float(step["blackout_ms"]) / 1000.0
            label.append("blackout %.0fms" % step["blackout_ms"])
        if step.get("clear_blackout"):
            self.perturb["blackout_until"] = 0.0
            label.append("blackout 解除")
        if "inject" in step:
            for name, spec in step["inject"].items():
                self.inject[name] = dict(spec)
            label.append("inject " + ",".join(step["inject"]))
        if step.get("inject_clear"):
            self.inject.clear()
            label.append("inject 清空")
        if label:
            self.notes.append("t+%.1fs %s" % (step.get("at_s", 0.0), "; ".join(label)))

    # ---- 场景 -------------------------------------------------------
    def apply_scenario(self, name):
        sc = SCENARIOS.get(name)
        if not sc:
            return False, "未知场景 %s（可选：%s）" % (name, ",".join(sorted(SCENARIOS)))
        with self.lock:
            if "perturb" in sc:
                for k, v in sc["perturb"].items():
                    self.perturb[k] = v
                self.perturb["drop_seq"] = 0
            if sc.get("inject_clear"):
                self.inject.clear()
            self.timeline = list(sc.get("timeline", []))
            self.timeline_t0 = None if not self.timeline else time.monotonic()
            self.timeline_done = 0
            # ★ at_s<=0 的步必须**当场**应用：否则"改完工况立刻读一帧"会读到
            #   还没生效的旧状态，表现像"注入失败"，而其实是调度延迟。
            self.tick_timeline()
            self.notes.append("场景 %s 已生效" % name)
        return True, "场景 %s 已生效" % name

    # ---- 快照 -------------------------------------------------------
    def latency_stats(self):
        with self.lock:
            xs = sorted(self.lat)
        if not xs:
            return {"n": 0, "p50": 0.0, "p95": 0.0, "max": 0.0, "avg": 0.0}

        def q(p):
            i = min(len(xs) - 1, int(round(p * (len(xs) - 1))))
            return xs[i]
        return {"n": len(xs), "p50": q(0.50), "p95": q(0.95),
                "max": xs[-1], "avg": sum(xs) / len(xs)}

    def snapshot(self, n=200):
        with self.lock:
            # ★ 注意 `[-0:]` 等价于 `[0:]`（返回全部）而不是空 —— n=0 要显式短路，
            #   否则状态接口每次都会把整个环形缓冲序列化一遍。
            fr = [] if n <= 0 else list(self.frames)[-n:]
            p = dict(self.perturb)
            p["blackout_remaining_ms"] = max(0.0, (p["blackout_until"] - time.monotonic()) * 1000.0)
            p.pop("blackout_until", None)
            return {
                "perturb": p,
                "inject": dict(self.inject),
                "stats": dict(self.stats),
                "frames": fr,
                "latency": self.latency_stats(),
                "notes": list(self.notes)[-8:],
            }


# =====================================================================
# 链路泵：一条主站连接 = 一个 Link
# =====================================================================
class Link(object):
    def __init__(self, cs, ctl, up_host, up_port, verbose=False, up_timeout=2.0):
        self.cs = cs
        self.ctl = ctl
        self.up_host = up_host
        self.up_port = up_port
        self.verbose = verbose
        self.up_timeout = up_timeout
        self.pending = {}          # tid -> ctx
        self.delayed = []          # [(due, bytes)]
        self.sel = selectors.DefaultSelector()
        self.req_framer = Framer()
        self.rsp_framer = Framer()

    def log(self, *a):
        if self.verbose:
            print(*a, flush=True)

    def run(self):
        try:
            us = socket.create_connection((self.up_host, self.up_port),
                                          timeout=self.up_timeout)
        except OSError as e:
            # ★ 上游连不上时**立刻关门**，不要让主站在自己那边干等：
            #   现场最常见的故障就是"柜子没上电"，如果这里默认等 5 秒，
            #   主站的超时（通常 1 秒）会先触发，日志里看到的就成了"主站超时"
            #   而不是"上游不可达" —— 排查方向会被带偏。
            self.ctl.record_event("ERR", "上游从站连不上 %s:%d (%s)"
                                  % (self.up_host, self.up_port, e))
            self.cs.close()
            return
        self.cs.setblocking(False)
        us.setblocking(False)
        self.sel.register(self.cs, selectors.EVENT_READ, "cs")
        self.sel.register(us, selectors.EVENT_READ, "us")
        with self.ctl.lock:
            self.ctl.stats["conns"] += 1
        self.log("[tap] 主站接入，上游 %s:%d 已连" % (self.up_host, self.up_port))
        try:
            while True:
                self.ctl.tick_timeline()
                timeout = self._next_timeout()
                events = self.sel.select(timeout)
                for key, _ in events:
                    sk = key.fileobj
                    try:
                        data = sk.recv(65536)
                    except (BlockingIOError, InterruptedError):
                        continue
                    except OSError:
                        return
                    if not data:
                        return
                    if key.data == "cs":
                        for f in self.req_framer.feed(data):
                            self._on_request(f, us)
                    else:
                        for f in self.rsp_framer.feed(data):
                            self._on_response(f, us)
                self._flush()
        finally:
            try:
                self.sel.close()
            except Exception:
                pass
            for s in (self.cs, us):
                try:
                    s.close()
                except Exception:
                    pass
            self.log("[tap] 连接结束")

    def _next_timeout(self):
        t = 0.05                      # 有活动时间轴时靠它推进
        for (due, _b) in self.delayed:
            t = min(t, max(0.0, due - time.monotonic()))
        return t

    def _flush(self):
        now = time.monotonic()
        keep = []
        for (due, data) in self.delayed:
            if due <= now:
                try:
                    self.cs.sendall(data)
                except OSError:
                    return
            else:
                keep.append((due, data))
        self.delayed = keep

    # ---- 请求方向 ---------------------------------------------------
    def _on_request(self, f, us):
        fc, text, ctx = describe_request(f.pdu)
        self.ctl.record("REQ", f, text)
        self.log("[REQ] tid=%d %s" % (f.tid, text))
        ok, delay, why = self.ctl.gate("REQ")
        if not ok:
            self.ctl.record_event("DROP", "丢弃请求（%s）" % why, note=why, hex_=f.raw.hex())
            return
        if fc in (1, 2, 3, 4, 5, 6, 15, 16):
            ctx = dict(ctx)
            ctx["t_sent"] = time.monotonic()
            self.pending[f.tid] = ctx
        if delay > 0:
            self.delayed.append((time.monotonic() + delay, f.raw))
        else:
            try:
                us.sendall(f.raw)
            except OSError:
                pass

    # ---- 响应方向 ---------------------------------------------------
    def _on_response(self, f, us):
        ctx = self.pending.pop(f.tid, {})

        new_raw, note = self.ctl.rewrite(f, ctx)
        if new_raw:
            f = Frame(new_raw)
        text, decoded, _meta = describe_response(f.pdu, ctx)

        # ★ 先过闸门再记账：录下来的时延要等于**主站实际经历的那个值**
        #   （上游往返 + 注入的延迟）。只记上游往返会出现"日志里 0.3 ms、
        #   主站却超时"的自相矛盾 —— 口径不一致最容易被误读成抓到了 bug。
        ok, delay, why = self.ctl.gate("RSP")
        lat = None
        if "t_sent" in ctx:
            rtt = (time.monotonic() - ctx["t_sent"]) * 1000.0
            lat = rtt + (delay * 1000.0 if ok else 0.0)
            self.ctl.lat.append(lat)
        if not ok:
            note = (note + " · " if note else "") + "已丢弃(%s)" % why

        self.ctl.record("RSP", f, text, decoded=decoded, latency=lat, note=note)
        self.log("[RSP] tid=%d %s%s%s" % (f.tid, text,
                                          (" | " + decoded) if decoded else "",
                                          ("（%s）" % note) if note else ""))
        if not ok:
            self.ctl.record_event("DROP", "丢弃响应（%s）" % why, note=why,
                                  hex_=f.raw.hex())
            return

        with self.ctl.lock:
            half = self.ctl.perturb["half_packet"]
        if half and len(f.raw) > 2:
            k = len(f.raw) // 2
            now = time.monotonic()
            self.delayed.append((now + delay, f.raw[:k]))
            # ★ 真实半包：先到一半，30ms 后再到另一半 —— 测主站的组帧重组
            self.delayed.append((now + delay + 0.03, f.raw[k:]))
            self.ctl.record_event("INFO", "半包发送（%d + %d 字节）" % (k, len(f.raw) - k))
            return
        if delay > 0:
            self.delayed.append((time.monotonic() + delay, f.raw))
        else:
            try:
                self.cs.sendall(f.raw)
            except OSError:
                pass


# =====================================================================
# 控制口（HTTP）+ 监视页面
# =====================================================================
MONITOR_HTML = """<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>13/ 链路中间人 · 报文监视</title>
<style>
:root{--bg:#0e1116;--panel:#151a21;--line:#28303a;--tx:#e6edf3;--dim:#8b949e;
--req:#58a6ff;--rsp:#3fb950;--bad:#f85149;--warn:#d29922}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--tx);
font:13px/1.55 ui-monospace,Consolas,Menlo,monospace}
header{padding:10px 16px;border-bottom:1px solid var(--line);background:var(--panel);
display:flex;gap:18px;align-items:baseline;flex-wrap:wrap}
h1{font-size:14px;font-weight:500;margin:0}
.stat{color:var(--dim);font-size:12px}
.stat b{color:var(--tx);font-weight:500}
main{padding:10px 12px;max-height:calc(100vh - 110px);overflow:auto}
table{width:100%;border-collapse:collapse}
th,td{text-align:left;padding:3px 8px;border-bottom:1px solid var(--line);
vertical-align:top}
th{color:var(--dim);font-weight:400;position:sticky;top:0;background:var(--bg)}
td.n{color:var(--dim);width:48px}
td.d{width:52px}
tr.REQ td.d{color:var(--req)}tr.RSP td.d{color:var(--rsp)}
tr.DROP td.d{color:var(--bad)}tr.ERR td.d{color:var(--bad)}
tr.INFO td.d{color:var(--warn)}
.dec{color:var(--dim);font-size:12px;white-space:normal}
.dec em{color:var(--warn);font-style:normal}
.hex{color:#5b636e;font-size:11px;word-break:break-all;max-width:280px}
.note{color:var(--warn)}
footer{padding:6px 16px;color:var(--dim);font-size:12px;border-top:1px solid var(--line)}
</style></head><body>
<header>
<h1>13/ 链路中间人 · 报文监视</h1>
<span class="stat" id="s1">—</span>
<span class="stat" id="s2">—</span>
<span class="stat" id="s3">—</span>
</header>
<main><table id="t"><thead><tr>
<th>#</th><th>t(s)</th><th>方向</th><th>MBAP</th><th>内容</th><th>时延</th><th>原始帧</th>
</tr></thead><tbody></tbody></table></main>
<footer id="f">等待数据…</footer>
<script>
function esc(s){return String(s==null?'':s).replace(/&/g,'&amp;').replace(/</g,'&lt;');}
async function tick(){
  try{
    var st = await (await fetch('/api/status')).json();
    var S = st.stats, L = st.latency, P = st.perturb;
    document.getElementById('s1').innerHTML =
      '帧 <b>'+S.total+'</b> ｜ 请求 '+S.req+' / 响应 '+S.rsp+
      ' ｜ 丢弃 <b>'+S.dropped+'</b> ｜ 注入 '+S.injected+' ｜ 连接 '+S.conns;
    document.getElementById('s2').innerHTML =
      '时延 p50 <b>'+L.p50.toFixed(1)+'</b> ｜ p95 <b>'+L.p95.toFixed(1)+
      '</b> ｜ max <b>'+L.max.toFixed(1)+'</b> ms ('+L.n+' 次)';
    document.getElementById('s3').innerHTML =
      '延迟 '+P.delay_ms+'ms ｜ 抖动 '+P.jitter_ms+'ms ｜ 丢包 1/'+P.drop_every+
      ' ｜ 黑障 '+P.blackout_remaining_ms.toFixed(0)+'ms ｜ 半包 '+(P.half_packet?'开':'关');
    document.getElementById('f').innerHTML =
      '注入：'+esc(JSON.stringify(st.inject))+'<br>'+
      st.notes.map(esc).join(' ｜ ');
    var fr = await (await fetch('/api/frames?n=150')).json();
    document.querySelector('#t tbody').innerHTML = fr.frames.slice().reverse().map(function(r){
      return '<tr class="'+r.dir+'">'
        + '<td class="n">'+r.seq+'</td>'
        + '<td>'+(r.t==null?'-':r.t.toFixed(3))+'</td>'
        + '<td class="d">'+r.dir+'</td>'
        + '<td>'+(r.tid==null?'-':'tid '+r.tid+' u'+r.unit+' len'+r.length)+'</td>'
        + '<td>'+esc(r.text)+(r.note?' <span class="note">['+esc(r.note)+']</span>':'')
        + (r.decoded?'<div class="dec">'+esc(r.decoded)+'</div>':'')+'</td>'
        + '<td>'+(r.latency_ms==null?'-':r.latency_ms.toFixed(1))+'</td>'
        + '<td class="hex">'+esc(r.hex)+'</td></tr>';
    }).join('');
  }catch(e){
    document.getElementById('f').textContent = '控制口未就绪：'+e;
  }
}
tick(); setInterval(tick, 1000);
</script></body></html>
"""


class CtlHandler(BaseHTTPRequestHandler):
    ctl = None                      # 由 serve_ctl 注入

    def log_message(self, fmt, *args):    # 静音：HTTP 访问日志会淹没帧日志
        pass

    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, ensure_ascii=False)
        data = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _json_body(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
            return json.loads(self.rfile.read(n).decode("utf-8") or "{}")
        except Exception:
            return {}

    def do_GET(self):
        path = self.path.split("?")[0]
        q = {}
        if "?" in self.path:
            for kv in self.path.split("?", 1)[1].split("&"):
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    q[k] = v
        if path in ("/", "/index.html"):
            return self._send(200, MONITOR_HTML, "text/html; charset=utf-8")
        if path == "/api/status":
            snap = self.ctl.snapshot(0)
            return self._send(200, {"perturb": snap["perturb"], "inject": snap["inject"],
                                    "stats": snap["stats"], "latency": snap["latency"],
                                    "notes": snap["notes"]})
        if path == "/api/frames":
            n = int(q.get("n", "200"))
            return self._send(200, {"frames": self.ctl.snapshot(n)["frames"]})
        if path == "/api/points":
            return self._send(200, {"points": [
                {"name": p.name, "table": p.table, "addr": p.addr, "enc": p.enc,
                 "word_order": p.wo, "scale": p.scale, "writable": p.writable}
                for p in _POINTS]})
        return self._send(404, {"error": "unknown path"})

    def do_POST(self):
        path = self.path.split("?")[0]
        body = self._json_body()
        if path == "/api/perturb":
            with self.ctl.lock:
                if "blackout_ms" in body:
                    self.ctl.perturb["blackout_until"] = \
                        time.monotonic() + float(body.pop("blackout_ms")) / 1000.0
                if body.pop("clear_blackout", False):
                    self.ctl.perturb["blackout_until"] = 0.0
                if "scenario" in body:
                    ok, msg = self.ctl.apply_scenario(body.pop("scenario"))
                    return self._send(200 if ok else 400, {"ok": ok, "msg": msg})
                for k in list(body):
                    if k in self.ctl.perturb:
                        self.ctl.perturb[k] = body.pop(k)
                if body.get("inject_clear"):
                    self.ctl.inject.clear()
                self.ctl.notes.append("扰动已改：%s" % json.dumps(
                    {k: v for k, v in self.ctl.perturb.items() if k != "drop_seq"},
                    ensure_ascii=False))
            return self._send(200, {"ok": True, "perturb": self.ctl.snapshot(0)["perturb"]})
        if path == "/api/inject":
            with self.ctl.lock:
                if body.pop("clear", False):
                    self.ctl.inject.clear()
                for name, spec in body.items():
                    if name not in _BY_NAME:
                        return self._send(400, {"ok": False, "msg": "未知测点 %s" % name})
                    self.ctl.inject[name] = spec if isinstance(spec, dict) else {"value": spec}
                self.ctl.notes.append("注入已改：%s" % json.dumps(self.ctl.inject, ensure_ascii=False))
                cur = dict(self.ctl.inject)
            return self._send(200, {"ok": True, "inject": cur})
        if path == "/api/scenario":
            ok, msg = self.ctl.apply_scenario(body.get("name", ""))
            return self._send(200 if ok else 400, {"ok": ok, "msg": msg})
        if path == "/api/timeline":
            steps = body.get("steps", [])
            self.ctl.start_timeline(steps)
            return self._send(200, {"ok": True, "steps": len(steps)})
        if path == "/api/clear":
            with self.ctl.lock:
                self.ctl.frames.clear()
                self.ctl.lat.clear()
                for k in self.ctl.stats:
                    self.ctl.stats[k] = 0
                self.ctl.notes = ["统计与帧缓冲已清空"]
            return self._send(200, {"ok": True})
        return self._send(404, {"error": "unknown path"})


def serve_ctl(ctl, port, host="127.0.0.1"):
    CtlHandler.ctl = ctl
    srv = ThreadingHTTPServer((host, port), CtlHandler)
    t = threading.Thread(target=srv.serve_forever, name="ctl", daemon=True)
    t.start()
    return srv


# =====================================================================
# 录制（帧级黄金向量）
# =====================================================================
class Recorder(object):
    """把每一帧写成 JSONL。用途：事后逐字节复核、当回归基线。

    接法是 Ctl.sink = write（推送），不是轮询帧缓冲 —— 理由见 Ctl.record 里的说明。
    `buffering=1` 保证逐行落盘，所以**进程被强杀也不会丢已经发生过的帧**
    （这一点对"偶发故障抓到一半"的场景很关键）。
    """

    def __init__(self, path):
        self.path = path
        self.fh = open(path, "a", encoding="utf-8", buffering=1)
        self.lock = threading.Lock()
        self.n = 0

    def attach(self, ctl):
        ctl.sink = self.write

    def write(self, entry):
        line = json.dumps(entry, ensure_ascii=False)
        with self.lock:
            self.fh.write(line + "\n")
            self.n += 1


# =====================================================================
# 入口
# =====================================================================
def parse_args(argv=None):
    p = argparse.ArgumentParser(
        description="13/ 链路中间人：报文监视 + 链路扰动 + 测点注入",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    p.add_argument("--listen", type=int, default=15021, help="中间人监听端口（主站连这个）")
    p.add_argument("--listen-host", default="127.0.0.1")
    p.add_argument("--upstream", type=int, default=15020, help="真实从站端口")
    p.add_argument("--upstream-host", default="127.0.0.1")
    p.add_argument("--upstream-timeout", type=float, default=2.0,
                   help="连接上游从站的超时（秒）；要小于主站自己的超时，"
                        "否则日志会把「上游不可达」显示成「主站超时」")
    p.add_argument("--ctl-port", type=int, default=15022, help="HTTP 控制口 / 监视台")
    p.add_argument("--record", default="", help="把帧写成 JSONL（黄金向量）")
    p.add_argument("--verbose", "-v", action="store_true", help="逐帧打印到控制台")
    p.add_argument("--max-frames", type=int, default=2000)
    # ---- 控制客户端模式 ----
    p.add_argument("--ctl", metavar="CMD", default="",
                   help="作为控制客户端执行一条命令：status/frames/clear/perturb/"
                        "inject/scenario/timeline/points")
    p.add_argument("--ctl-host", default="127.0.0.1")
    return p.parse_args(argv)


def run_ctl_client(args, rest):
    """把 `--ctl perturb delay_ms=300` 翻译成一次 HTTP 调用。

    为什么不要求装了 curl：Windows 上 curl 有无不定，而控制口是 stdlib HTTP，
    自带的客户端最省事 —— 也让"怎么调这个工具"只出现在这一个文件里。
    """
    cmd = args.ctl
    base = "http://%s:%d" % (args.ctl_host, args.ctl_port)
    try:
        if cmd == "status":
            r = urllib.request.urlopen(base + "/api/status", timeout=5)
            obj = json.loads(r.read().decode("utf-8"))
            print("扰动  :", json.dumps(obj["perturb"], ensure_ascii=False))
            print("注入  :", json.dumps(obj["inject"], ensure_ascii=False))
            print("统计  :", json.dumps(obj["stats"], ensure_ascii=False))
            print("时延  :", json.dumps(obj["latency"], ensure_ascii=False))
            for n in obj["notes"]:
                print("  ·", n)
            return 0
        if cmd == "frames":
            n = int(rest[0]) if rest else 20
            r = urllib.request.urlopen(base + "/api/frames?n=%d" % n, timeout=5)
            for f in json.loads(r.read().decode("utf-8"))["frames"]:
                lat = "" if f["latency_ms"] is None else " %6.1fms" % f["latency_ms"]
                print("%4d %8.3f %-5s %s%s" % (f["seq"], f["t"], f["dir"], f["text"], lat))
                if f["decoded"]:
                    print("                              %s" % f["decoded"])
            return 0
        if cmd == "points":
            r = urllib.request.urlopen(base + "/api/points", timeout=5)
            for pt in json.loads(r.read().decode("utf-8"))["points"]:
                print("%-24s %s[%2d] %-4s %s  ×%g%s" % (
                    pt["name"], pt["table"], pt["addr"], pt["enc"],
                    pt["word_order"], pt["scale"], "  rw" if pt["writable"] else ""))
            return 0
        if cmd == "clear":
            body = {}
            path = "/api/clear"
        elif cmd == "scenario":
            body = {"name": rest[0] if rest else ""}
            path = "/api/scenario"
        elif cmd == "perturb":
            body = {}
            for kv in rest:
                k, _, v = kv.partition("=")
                if k in ("freeze", "half_packet"):
                    body[k] = v.lower() in ("1", "true", "on", "yes")
                elif k == "scenario":
                    body["scenario"] = v
                elif k == "clear_blackout":
                    body["clear_blackout"] = True
                else:
                    body[k] = float(v)
            path = "/api/perturb"
        elif cmd == "inject":
            body = {}
            for kv in rest:
                k, _, v = kv.partition("=")
                if k == "clear":
                    body["clear"] = True
                elif k in _BY_NAME:
                    if v == "flip":
                        spec = {"flip": True, "value": 0.0}
                    elif v.startswith("raw:"):
                        spec = {"raw_words": [int(x, 0) for x in v[4:].split(",")]}
                    else:
                        spec = {"value": float(v)}
                    body[k] = spec
            path = "/api/inject"
        elif cmd == "timeline":
            steps = []
            for spec in rest:      # 形如 2:blackout_ms=3000
                at, _, kvs = spec.partition(":")
                step = {"at_s": float(at)}
                for kv in kvs.split(","):
                    k, _, v = kv.partition("=")
                    step[k] = v if k in ("inject",) else float(v)
                steps.append(step)
            body, path = {"steps": steps}, "/api/timeline"
        else:
            print("未知命令：%s" % cmd, file=sys.stderr)
            return 2

        req = urllib.request.Request(
            base + path, data=json.dumps(body).encode("utf-8"), method="POST",
            headers={"Content-Type": "application/json"})
        r = urllib.request.urlopen(req, timeout=5)
        print(r.read().decode("utf-8"))
        return 0
    except urllib.error.URLError as e:
        print("控制口连不上（%s）—— 中间人在跑吗？端口 %d"
              % (e, args.ctl_port), file=sys.stderr)
        return 1


def main(argv=None):
    args, rest = parse_args_and_rest(argv)
    if args.ctl:
        return run_ctl_client(args, rest)

    ctl = Ctl(max_frames=args.max_frames)
    ctl.notes.append("中间人启动：%s:%d → %s:%d"
                     % (args.listen_host, args.listen, args.upstream_host, args.upstream))

    # ★ 自环防护：上游 = 自己时，accept 之后又去连自己 → 无限自我连接，
    #   表现是"进程活着、端口在听、但一帧数据都没有"，很难从现象反推。
    #   端口撞车在脚本里很容易发生（逐个取空闲端口时操作系统会重复分配
    #   刚释放的那个），所以这里必须拦一道。
    if (args.upstream_host in ("127.0.0.1", "localhost", args.listen_host)
            and args.upstream == args.listen):
        print("拒绝启动：上游端口 %d 与监听端口相同 —— 会连上自己形成自环。"
              % args.listen, file=sys.stderr)
        return 2

    serve_ctl(ctl, args.ctl_port)

    if args.record:
        rec = Recorder(args.record)
        rec.attach(ctl)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.listen_host, args.listen))
    srv.listen(8)

    print("=" * 70)
    print("  13/ 链路中间人")
    print("  主站请连   : %s:%d" % (args.listen_host, args.listen))
    print("  上游真从站 : %s:%d" % (args.upstream_host, args.upstream))
    print("  监视台     : http://127.0.0.1:%d/" % args.ctl_port)
    if args.record:
        print("  录制       : %s" % args.record)
    print("  控制       : python sim\\link_tap.py --ctl status")
    print("             : python sim\\link_tap.py --ctl scenario blackout")
    print("             : python sim\\link_tap.py --ctl inject MEAS.P_BAT=flip")
    print("=" * 70)
    sys.stdout.flush()

    try:
        while True:
            cs, addr = srv.accept()
            cs.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            t = threading.Thread(target=Link(cs, ctl, args.upstream_host,
                                             args.upstream, args.verbose,
                                             args.upstream_timeout).run,
                                 name="link", daemon=True)
            t.start()
    except KeyboardInterrupt:
        print("\n[tap] 收到中断，收尾中…")
    finally:
        try:
            srv.close()
        except Exception:
            pass
        snap = ctl.snapshot(0)
        print("[tap] 累计：%s" % json.dumps(snap["stats"], ensure_ascii=False))
        print("[tap] 时延：%s" % json.dumps(snap["latency"], ensure_ascii=False))
    return 0


def parse_args_and_rest(argv=None):
    """argparse 会把 `--ctl perturb delay_ms=300` 的 rest 当未知参数拒绝，
    所以把 `--ctl <cmd> [k=v ...]` 单独摘出来再交给 argparse。"""
    argv = list(sys.argv[1:] if argv is None else argv)
    rest = []
    for i, a in enumerate(argv):
        if a == "--ctl" and i + 1 < len(argv):
            rest = argv[i + 2:]
            argv = argv[:i + 2]
            break
    return parse_args(argv), rest


if __name__ == "__main__":
    raise SystemExit(main())
