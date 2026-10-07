"""14/ — Modbus 现场点表（平台侧：校验、序列化、落盘）

为什么 14/ 需要有这份模块：
    点表是「客户的设备能不能接上来」的唯一开关。13/ 现场进程是 C++、
    14/ 平台是 Python，两边必须认同一套规则。否则会出现最难查的那种故障 ——
    **界面显示"保存成功"，现场却读不了表**（或者更糟：读得进去、但读出的
    是错的点）。所以校验不放在前端、也不靠人工核对，而是落在服务端。

三件事：
  1. 基线   从 13/docs/point_map_template.csv 读默认表 —— **不另抄点名**
            （schema.sql 的约定：点表口径不另抄一份。而那份模板本身被判据
             T20 断言「与 13/ 内置默认表逐字段一致」，所以它是可信基线）
  2. 校验   逐条镜像 13/src/modbus_point_map.h 的 validate_map() 与
            derive_read_blocks()
  3. 落盘   生成 13/ 能直接加载的 CSV，写到约定路径

★ 规则一致性由判据守，不靠"碰巧一样"：
    14/tests/selftest.py 把同一批合法/非法表分别喂给本模块与
    modbus_probe.exe，断言两边判断相同。13/ 的规则若变了，测试会红。

★ 本模块**不比 13/ 更严**，这是刻意的：
    13/ 对 unit/note 两列「刻意不校验」（原话：免得现场为了写一句备注还要
    翻译我们的术语）。所以这里也不校验那两列，只做存取与展示。

纯标准库，零第三方依赖 —— 与 14/ 其余部分一致。
"""

from __future__ import annotations

import csv
import io
import os
import re
import tempfile
from typing import Any, Iterable, Optional

# ---------------------------------------------------------------------
# 路径约定
#
# 这是 14/（写）与 13/（读）之间**唯一**需要对齐的东西。两侧都按同一套
# 规则推算，不靠命令行传参、不靠文档交代：
#     <项目根>/config/point-map/          目录（环境变量 EMS_POINT_MAP_DIR 可覆盖）
#         <device_id>.csv                 每台设备的点表存档
#         active.csv                      当前启用的那一份 —— 13/ 默认读它
# 为什么要有 active.csv：13/ 的一次装配对应"一组接入参数 + 一份点表"
#   （见 schema.sql 的 device_conn 注释）。有了 active.csv，13/ 不需要知道
#   设备号就能启动；多设备并存时仍可用 --load-map 指定具体那台。
# ---------------------------------------------------------------------
MODULE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT_ROOT = os.path.dirname(MODULE_ROOT)

BASELINE_PATH = os.path.join(PROJECT_ROOT, "13", "docs", "point_map_template.csv")

ENV_MAP_DIR = "EMS_POINT_MAP_DIR"
ACTIVE_NAME = "active.csv"

# ---------------------------------------------------------------------
# 取值域 —— 与 13/src/modbus_point_map.h 的
#   table_code() / encoding_code() / word_order_code() 完全一致，不另发明写法
# ---------------------------------------------------------------------
COLS = ["name", "table", "address", "encoding", "word_order",
        "scale", "writable", "unit", "note"]

# 少一列都解析不了 —— 13/ 的 loader 要求至少 7 列（r.n < 7 直接拒绝）
MIN_COLS = 7

TABLE_ALIASES = {
    "IR": "IR", "INPUT_REG": "IR", "INPUTREG": "IR",
    "HR": "HR", "HOLDING_REG": "HR", "HOLDINGREG": "HR",
    "DI": "DI", "DISCRETE_INPUT": "DI",
    "CO": "CO", "COIL": "CO",
}
ENCODING_ALIASES = {
    "F32": "f32", "FLOAT": "f32",
    "U16": "u16",
    "I16": "i16",
    "BIT": "bit", "BOOL": "bit",
}
WORD_ORDER_ALIASES = {
    "HIGH": "high", "ABCD": "high",
    "LOW": "low", "CDAB": "low",
}
WRITABLE_ALIASES = {"RW": "rw", "RO": "ro", "R": "ro", "W": "rw"}

# 分块推导参数 —— 与 13/ 同名同值，改一处必须两边都改（有判据盯着）
MERGE_GAP = 16          # kMergeGap
MAX_BLOCKS = 16         # kMaxReadBlocks
MAX_READ_REGS = 125     # kMaxReadRegs（FC03 / FC04）
MAX_READ_BITS = 2000    # kMaxReadBits（FC01 / FC02）

# 表的处理顺序决定块序。与 13/ 的 derive_read_blocks() 一致：
# 这样默认表推导出来的块序不会变（顺序变了 = 请求序列变了 = 真机行为变了）
TABLE_ORDER = ("IR", "HR", "DI", "CO")

ADDR_MAX = 65535
_HEADER_FIRST_COLS = {"NAME", "POINT"}


class PointMapError(Exception):
    """点表不合法。status 供 API 层直接用作 HTTP 状态码。"""

    def __init__(self, message: str, status: int = 400) -> None:
        super().__init__(message)
        self.message = message
        self.status = status


# =====================================================================
# 1. 基线（默认表）
# =====================================================================
_baseline_cache: Optional[list[dict]] = None


def parse_text(text: str) -> list[dict]:
    """把点表 CSV 文本解析成行的列表。

    与 13/ 的 load_point_map_csv() 同一套宽容度：
      · 剥 UTF-8 BOM（Excel 存出来会带）
      · 跳过空行与 `#` 注释行
      · 跳过表头（首列是 name / point / 点名）
      · 允许用引号包字段（13/ 的 split_csv_line 支持 "..." 与 "" 转义）
    """
    if text.startswith("\ufeff"):
        text = text[1:]

    rows: list[dict] = []
    header_seen = False
    for raw in text.splitlines():
        if not raw.strip():
            continue
        if raw.lstrip().startswith("#"):
            continue
        try:
            cols = next(csv.reader(io.StringIO(raw)))
        except (csv.Error, StopIteration):
            raise PointMapError(f"无法解析这一行为 CSV：{raw[:60]}", 400)
        if not cols:
            continue
        if not header_seen:
            header_seen = True
            first = cols[0].strip().upper()
            if first in _HEADER_FIRST_COLS or cols[0].strip() == "点名":
                continue
        rows.append(_row_from_cols(cols))
    return rows


def _row_from_cols(cols: list[str]) -> dict:
    """把一行 CSV 列转成内部行结构。列不足时补空，交给 validate() 报错。"""
    padded = list(cols) + [""] * (len(COLS) - len(cols))
    return {
        "name": padded[0].strip(),
        "table": padded[1].strip(),
        "address": padded[2].strip(),
        "encoding": padded[3].strip(),
        "word_order": padded[4].strip(),
        "scale": padded[5].strip(),
        "writable": padded[6].strip(),
        "unit": padded[7].strip(),
        "note": padded[8].strip(),
    }


def read_baseline(path: Optional[str] = None) -> list[dict]:
    """读默认表基线。找不到文件时抛错（**不静默**）。

    为什么不静默兜底：没有基线就没有点名清单，也就没法校验客户提交的表。
    这时接受一张"看起来没问题"的表，等于把校验关掉 —— 比报错危险得多。
    """
    global _baseline_cache
    if path is None and _baseline_cache is not None:
        return [dict(r) for r in _baseline_cache]
    p = path or BASELINE_PATH
    if not os.path.isfile(p):
        raise PointMapError(
            f"找不到点表基线 {p}。它是 14/ 校验客户点表的依据，"
            f"缺失时不能接受任何点表提交。", 500)
    with open(p, "r", encoding="utf-8-sig") as f:
        rows = parse_text(f.read())
    if not rows:
        raise PointMapError(f"点表基线 {p} 里没有数据行", 500)
    if path is None:
        _baseline_cache = [dict(r) for r in rows]
    return rows


def row_count() -> int:
    """点表应有的行数。从基线取，**不硬编码 32**。"""
    return len(read_baseline())


def baseline_names() -> list[str]:
    return [r["name"] for r in read_baseline()]


def command_span() -> tuple[int, int]:
    """指令段的索引区间 (首, 末)。

    从点名的字面前缀推出来，不写死数字 —— 写死就会在点表调整后静默错位。
    找不到指令点时返回 (-1, -1)，validate() 会据此报错。
    """
    names = baseline_names()
    try:
        return names.index("CMD.P_BAT"), names.index("CMD.P_LOWER")
    except ValueError:
        return -1, -1


# =====================================================================
# 2. 字段解析（严格版）
#
# 为什么不用 int() / float() 带 try：它们会接受 " 12 "、"12\n"、"1_0" 之类。
# 更危险的是现场最常见的错法 —— 把地址写成 "40010"（厂家手册的 4 打头写法），
# 若被静默截断成 40010 或 0，程序照样跑，只是读错了寄存器。
# 所以这里逐字符校验，不接受任何"看起来像"的东西。
# =====================================================================
_RE_U16 = re.compile(r"^\d{1,5}$")


def parse_u16(s: str) -> int:
    t = s.strip()
    if not _RE_U16.match(t):
        raise PointMapError(f"地址应为十进制整数 0..{ADDR_MAX}，实际是「{s}」", 400)
    v = int(t)
    if v > ADDR_MAX:
        raise PointMapError(f"地址应为 0..{ADDR_MAX}，实际是 {v}", 400)
    return v


_RE_NUM = re.compile(r"^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$")


def parse_scale(s: str) -> float:
    t = s.strip()
    if not _RE_NUM.match(t):
        raise PointMapError(f"缩放应为数值，实际是「{s}」", 400)
    return float(t)


def norm_table(s: str) -> str:
    v = TABLE_ALIASES.get(s.strip().upper())
    if not v:
        raise PointMapError(f"表类型应为 IR/HR/DI/CO，实际是「{s}」", 400)
    return v


def norm_encoding(s: str) -> str:
    v = ENCODING_ALIASES.get(s.strip().upper())
    if not v:
        raise PointMapError(f"编码应为 f32/u16/i16/bit，实际是「{s}」", 400)
    return v


def norm_word_order(s: str) -> str:
    v = WORD_ORDER_ALIASES.get(s.strip().upper())
    if not v:
        raise PointMapError(f"字序应为 high/low（或 ABCD/CDAB），实际是「{s}」", 400)
    return v


def norm_writable(s: str) -> bool:
    v = WRITABLE_ALIASES.get(s.strip().upper())
    if v is None:
        raise PointMapError(f"可写性应为 rw/ro，实际是「{s}」", 400)
    return v == "rw"


def reg_width(encoding: str) -> int:
    """该编码占几个寄存器 / 位。f32 占 2，其余占 1（与 PointBinding::reg_count 一致）。"""
    return 2 if encoding == "f32" else 1


# ---------------------------------------------------------------------
# 原生类型适配
#
# 为什么需要：同一个 normalize() 要同时吃三种来源，它们的字段类型不一样 ——
#   ① CSV 文本      全是字符串（"2" / "0" / "ro"）
#   ② 前端 JSON     数字是 int/float，布尔是 true/false
#   ③ 数据库行      address 是 INTEGER、scale 是 REAL、writable 是 0/1
# 早期版本只认字符串，于是把 ② ③ 喂回去时 str(False) 得到 "False"、字面解析
# 失败 —— 表现是"基线表自己都过不了校验"。所以这里按类型分派，不强行 str()。
# ---------------------------------------------------------------------
def _as_address(v: Any) -> int:
    if isinstance(v, bool):                     # bool 是 int 的子类，先挡掉
        raise PointMapError(f"地址应为十进制整数 0..{ADDR_MAX}，实际是 {v!r}", 400)
    if isinstance(v, int):
        if 0 <= v <= ADDR_MAX:
            return v
        raise PointMapError(f"地址应为 0..{ADDR_MAX}，实际是 {v}", 400)
    return parse_u16(str(v))


def _as_scale(v: Any) -> float:
    if isinstance(v, bool):
        raise PointMapError(f"缩放应为数值，实际是 {v!r}", 400)
    if isinstance(v, (int, float)):
        f = float(v)
        if f != f or f in (float("inf"), float("-inf")):
            raise PointMapError(f"缩放应是有限数值，实际是 {v!r}", 400)
        return f
    return parse_scale(str(v))


def _as_writable(v: Any) -> bool:
    if isinstance(v, bool):
        return v
    s = str(v).strip().upper()
    if s in ("TRUE", "1", "YES", "Y"):
        return True
    if s in ("FALSE", "0", "NO", "N"):
        return False
    return norm_writable(str(v))


def _as_text(v: Any) -> str:
    return "" if v is None else str(v).strip()


# =====================================================================
# 3. 归一化 + 校验
# =====================================================================
def normalize(rows: Iterable[dict]) -> list[dict]:
    """把外部传入的行转成规范形式。任一字段不合法即抛错。

    **幂等**：已归一化的行再喂进来结果不变。这条是必需的 ——
    校验、落盘、写库、回读各环节都会调用它，不幂等就会层层变形。
    """
    out: list[dict] = []
    for i, r in enumerate(rows):
        try:
            enc = norm_encoding(_as_text(r.get("encoding")))
            out.append({
                "name": _as_text(r.get("name")),
                "table": norm_table(_as_text(r.get("table"))),
                "address": _as_address(r.get("address")),
                "encoding": enc,
                "word_order": norm_word_order(_as_text(r.get("word_order"))),
                "scale": _as_scale(r.get("scale")),
                "writable": _as_writable(r.get("writable")),
                "unit": _as_text(r.get("unit")),
                "note": _as_text(r.get("note")),
            })
        except PointMapError as e:
            raise PointMapError(f"第 {i + 1} 行：{e.message}", e.status)
    return out


def validate(rows: list[dict]) -> list[dict]:
    """逐条镜像 13/ 的 validate_map()。返回归一化后的行；不合法即抛 PointMapError。

    规则清单（编号对应 13/ 源码里的注释）：
      ① 行数恰好等于基线行数；位置 == 索引，点名逐字等于基线
      ② 编码与表类型必须匹配；缩放规则随编码而定
      ③ 段的可写性由点表段决定（量测/状态/配置只读；指令可写）
      ④ 指令三点必须同表且地址连续（原子下发的前提）
      ⑤ 分块必须覆盖到每一个点
    """
    baseline = read_baseline()
    want = len(baseline)

    if len(rows) != want:
        raise PointMapError(
            f"点表应为 {want} 行，实际 {len(rows)} 行"
            f"（少一行 = 该点恒为默认值，静默；多出来的行会被丢弃，所以一并拒绝）", 400)

    cmd_a, cmd_b = command_span()
    if cmd_a < 0:
        raise PointMapError(
            "点表基线里找不到指令点 CMD.P_BAT / CMD.P_LOWER，无法校验", 500)

    for i, r in enumerate(rows):
        truth = baseline[i]["name"]
        # ① 位置 == 索引，点名逐字相等。点位靠索引寻址，插错一行后面全体错位。
        if r["name"] != truth:
            raise PointMapError(
                f"第 {i + 1} 行的点名应为 {truth}，实际是「{r['name']}」"
                f"（行序即索引，不能乱序、不能删行）", 400)

        enc, tab = r["encoding"], r["table"]

        # ② 编码与表类型匹配；缩放规则随编码而定
        if enc == "bit":
            if tab not in ("DI", "CO"):
                raise PointMapError(
                    f"第 {i + 1} 行 {r['name']}：位类型只能落在离散输入(DI)或线圈(CO)表，"
                    f"实际是 {tab}", 400)
            if r["scale"] != 0.0:
                raise PointMapError(
                    f"第 {i + 1} 行 {r['name']}：位类型的缩放必须为 0，实际是 {r['scale']:g}", 400)
        elif enc == "f32":
            if tab not in ("IR", "HR"):
                raise PointMapError(
                    f"第 {i + 1} 行 {r['name']}：浮点只能落在输入寄存器(IR)或保持寄存器(HR)表，"
                    f"实际是 {tab}", 400)
        else:  # u16 / i16
            if not (r["scale"] > 0.0):
                raise PointMapError(
                    f"第 {i + 1} 行 {r['name']}：{enc} 的缩放必须大于 0"
                    f"（0 会在换算时除零），实际是 {r['scale']:g}", 400)
            if tab not in ("IR", "HR"):
                raise PointMapError(
                    f"第 {i + 1} 行 {r['name']}：整数编码只能落在输入寄存器(IR)或保持寄存器(HR)表，"
                    f"实际是 {tab}", 400)

        # ③ 段的可写性
        if cmd_a <= i <= cmd_b:
            if not r["writable"]:
                raise PointMapError(
                    f"第 {i + 1} 行 {r['name']}：指令点必须可写(rw)", 400)
            if tab != "HR":
                raise PointMapError(
                    f"第 {i + 1} 行 {r['name']}：指令点必须在保持寄存器(HR)表，实际是 {tab}", 400)
        elif r["writable"]:
            raise PointMapError(
                f"第 {i + 1} 行 {r['name']}：属于只读段，不能标为可写(rw)", 400)

    # ④ 指令三点必须同表且地址连续
    ca, cb, cc = rows[cmd_a], rows[cmd_a + 1], rows[cmd_a + 2]
    if not (ca["table"] == cb["table"] == cc["table"] == "HR" and
            ca["address"] + 2 == cb["address"] and
            cb["address"] + 2 == cc["address"]):
        raise PointMapError(
            f"指令点 {ca['name']}/{cb['name']}/{cc['name']} 必须同表且地址连续"
            f"（否则权限区间无法与指令原子下发，会出现「新功率 + 旧权限区间」的中间态）。"
            f"实际地址：{ca['address']} / {cb['address']} / {cc['address']}", 400)

    # ⑤ 分块推导（内含地址重叠检查），并验证覆盖
    blocks = derive_blocks(rows)
    for i, r in enumerate(rows):
        w = reg_width(r["encoding"])
        end = r["address"] + w
        if not any(b["table"] == r["table"] and
                   b["start"] <= r["address"] and end <= b["start"] + b["count"]
                   for b in blocks):
            raise PointMapError(
                f"第 {i + 1} 行 {r['name']} 没有被任何分块覆盖"
                f"（该点会恒为默认值），点表排得太散", 400)

    return rows


def derive_blocks(rows: list[dict]) -> list[dict]:
    """逐条镜像 13/ 的 derive_read_blocks()。

    按表分组 → 表内按地址升序 → 间隙 ≤ MERGE_GAP 且不超协议上限则合并。
    地址重叠即报错。

    返回 [{"table","start","count"}, ...]，顺序与 13/ 一致（IR → HR → DI → CO）。
    """
    blocks: list[dict] = []
    for table in TABLE_ORDER:
        members = [r for r in rows if r["table"] == table]
        # 插入排序在 C++ 侧是为了避免依赖 <algorithm>；Python 侧用同一定序即可
        members.sort(key=lambda r: r["address"])

        lim = MAX_READ_BITS if table in ("DI", "CO") else MAX_READ_REGS
        cur: Optional[dict] = None

        for r in members:
            w = reg_width(r["encoding"])
            if cur is None:
                cur = {"table": table, "start": r["address"], "count": w}
                continue
            end = cur["start"] + cur["count"]
            if r["address"] < end:
                raise PointMapError(
                    f"点 {r['name']} 的地址 {r['address']} 落在同表前一个点的区间内"
                    f"（{table} {cur['start']}..{end - 1}，地址重叠）", 400)
            gap = r["address"] - end
            span = end + gap + w - cur["start"]
            if gap <= MERGE_GAP and span <= lim:
                cur["count"] = span
            else:
                if len(blocks) >= MAX_BLOCKS:
                    raise PointMapError(
                        f"分块数超过上限 {MAX_BLOCKS}"
                        f"（点表排得太散，一次快照要发太多次请求）", 400)
                blocks.append(cur)
                cur = {"table": table, "start": r["address"], "count": w}

        if cur is not None:
            if len(blocks) >= MAX_BLOCKS:
                raise PointMapError(
                    f"分块数超过上限 {MAX_BLOCKS}"
                    f"（点表排得太散，一次快照要发太多次请求）", 400)
            blocks.append(cur)
    return blocks


def full_scan_requests(rows: list[dict]) -> int:
    """「读全表一次」要发多少次请求 = 分块数。现场核对时最关心的一个数。"""
    return len(derive_blocks(rows))


# =====================================================================
# 4. 序列化 —— 生成 13/ 能直接加载的 CSV
# =====================================================================
def _fmt_scale(enc: str, scale: float) -> str:
    """与 13/ 的 export_point_map_csv() 逐字对齐：bit 恒写 "0"，其余用 %g。

    用 %g 而不是 %s 十进制展开，是为了让 10000 写成 "10000" 而不是
    "10000.0"，让 0 写成 "0" 而不是 "0.0" —— 现场人工比对时少一层噪音。
    """
    if enc == "bit":
        return "0"
    return "%g" % scale


def data_csv_text(rows: list[dict]) -> str:
    """只含表头 + 数据行，与 13/ --dump-map 的输出同构（不含注释头）。"""
    out = [",".join(COLS)]
    for r in rows:
        out.append(",".join([
            r["name"], r["table"], str(r["address"]), r["encoding"],
            r["word_order"], _fmt_scale(r["encoding"], r["scale"]),
            "rw" if r["writable"] else "ro",
            _qs(r["unit"]), _qs(r["note"]),
        ]))
    return "\n".join(out) + "\n"


def _qs(s: str) -> str:
    """必要时给字段加引号。13/ 的 split_csv_line 支持引号与 "" 转义，
    所以备注里带逗号也不会破坏往返。"""
    if any(c in s for c in (",", '"', "\n", "\r")):
        return '"' + s.replace('"', '""') + '"'
    return s


def to_csv_text(rows: list[dict], meta: Optional[dict] = None) -> str:
    """完整落盘文本 = 注释头 + 表头 + 数据。

    注释头是给现场人看的（13/ 会跳过 `#` 行）。里面写清三件事：
    这文件是谁生成的、13/ 从哪儿读它、以及「别手工改这个文件」。
    最后一条很重要 —— 手工改会在下次界面保存时被无声覆盖。
    """
    m = meta or {}
    lines = [
        "# " + "=" * 68,
        "#  Modbus 现场点表  ——  由 14/ 平台自动生成，请勿手工修改",
        "#",
        f"#  设备      {m.get('device_id', '-')}"
        f"   （{m.get('host', '-')}:{m.get('port', '-')}  从站 {m.get('unit_id', '-')}）",
        f"#  保存时刻  {m.get('saved_at', '-')}",
        f"#  保存人    {m.get('username', '-')}",
        f"#  数据行数  {len(rows)}",
        "#",
        "#  13/ 默认从这里读：config/point-map/active.csv",
        "#  保存前预检（不发报文，只看表合不合法、要发几次请求）：",
        "#     13\\build\\modbus_probe.exe --load-map 本文件.csv --plan",
        "#  连真机核对（盯最右边的 ok 列，-- 表示该点本拍不可信）：",
        "#     13\\build\\modbus_probe.exe --host <设备IP> --port 502 --unit <从站号> \\",
        "#                                --load-map 本文件.csv",
        "#",
        "#  改这张表的正规途径是 15/ 界面的「设备接入配置」，或者用 Excel 改完后",
        "#  在界面上传。**手工改这个文件会在下次界面保存时被覆盖。**",
        "# " + "=" * 68,
        "",
    ]
    return "\r\n".join(lines) + data_csv_text(rows).replace("\n", "\r\n")


# =====================================================================
# 5. 落盘
# =====================================================================
def map_dir() -> str:
    return os.environ.get(ENV_MAP_DIR) or os.path.join(PROJECT_ROOT, "config", "point-map")


def device_path(device_id: str) -> str:
    return os.path.join(map_dir(), f"{_safe_id(device_id)}.csv")


def active_path() -> str:
    return os.path.join(map_dir(), ACTIVE_NAME)


def _safe_id(device_id: str) -> str:
    """设备号要进文件名，先做一次白名单过滤，防目录穿越。"""
    s = re.sub(r"[^0-9A-Za-z_\-]", "_", str(device_id or ""))
    if not s:
        raise PointMapError("设备号为空，无法命名点表文件", 400)
    return s


def save(device_id: str, rows: list[dict], meta: Optional[dict] = None,
         make_active: bool = True) -> dict:
    """把点表落盘。返回 {"path", "active_path", "bytes"}。

    写入是**原子**的：先写同目录临时文件再 os.replace。
    为什么较真这一点：13/ 是另一个进程，可能在任意时刻读这个文件。
    直接覆盖会让它读到一个写了一半的表 —— 而半张表能通过语法解析、
    只是后半段的点全错，这是最坏的一种失败。
    """
    d = map_dir()
    os.makedirs(d, exist_ok=True)
    text = to_csv_text(rows, meta)
    # Excel 友好：带 BOM，中文不乱码。13/ 的 loader 会剥掉它（已实测）。
    payload = b"\xef\xbb\xbf" + text.encode("utf-8")

    target = device_path(device_id)
    _atomic_write(target, payload)

    act = None
    if make_active:
        act = active_path()
        _atomic_write(act, payload)

    return {"path": target, "active_path": act, "bytes": len(payload)}


def _atomic_write(path: str, payload: bytes) -> None:
    d = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(prefix=".pmap_", suffix=".tmp", dir=d)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(payload)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def load_file(path: str) -> list[dict]:
    """读回落盘的点表（界面上「已保存的点表」用它）。"""
    if not os.path.isfile(path):
        raise PointMapError(f"点表文件不存在：{path}", 404)
    with open(path, "r", encoding="utf-8-sig") as f:
        return parse_text(f.read())


# =====================================================================
# 6. 与数据库行互转
# =====================================================================
DB_COLS = ["device_id", "idx", "name", "table_kind", "address", "encoding",
           "word_order", "scale", "writable", "unit", "note"]


def to_db_rows(device_id: str, rows: list[dict]) -> list[tuple]:
    return [
        (device_id, i, r["name"], r["table"], r["address"], r["encoding"],
         r["word_order"], float(r["scale"]), 1 if r["writable"] else 0,
         r["unit"], r["note"])
        for i, r in enumerate(rows)
    ]


DB_INSERT_SQL = (
    "INSERT INTO device_point_map(" + ",".join(DB_COLS) + ") "
    "VALUES(" + ",".join(["?"] * len(DB_COLS)) + ")"
)


def from_db_rows(db_rows: list[dict]) -> list[dict]:
    """把库里的行还原成内部行结构（按 idx 升序，行序即索引）。"""
    out = []
    for r in sorted(db_rows, key=lambda x: int(x["idx"])):
        out.append({
            "name": r["name"],
            "table": r["table_kind"],
            "address": int(r["address"]),
            "encoding": r["encoding"],
            "word_order": r["word_order"],
            "scale": float(r["scale"]),
            "writable": bool(r["writable"]),
            "unit": r["unit"] or "",
            "note": r["note"] or "",
        })
    return out


def summary(rows: list[dict]) -> dict:
    """给界面「预检」用的一小组统计。不合法时把原因带回来，而不是抛。"""
    try:
        norm = normalize(rows)
        validate(norm)
        blocks = derive_blocks(norm)
    except PointMapError as e:
        return {"ok": False, "error": e.message}
    per_table: dict[str, int] = {}
    for b in blocks:
        per_table[b["table"]] = per_table.get(b["table"], 0) + 1
    return {
        "ok": True,
        "rows": len(norm),
        "blocks": len(blocks),
        "requests": len(blocks),
        "blocks_per_table": per_table,
        "writable_points": sum(1 for r in norm if r["writable"]),
    }
