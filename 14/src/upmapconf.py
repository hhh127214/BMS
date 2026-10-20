"""14/ — 上端转发表（IOA ↔ 点名）的平台侧：校验、序列化、落盘

为什么需要这份模块：
    上端这一端本来就有两份输入 —— 链路参数（连得上）与 IOA 转发表（读得对）。
    但很长一段时间里只有前者有落盘通道（`uplinkconf.py` 写 `active.upstream`），
    后者只能靠现场在命令行敲 `--point-map <file>`。于是"这一次该用哪份表"
    是启动网关那个人脑子里的东西 —— 换人就换答案，而屏幕上一切正常。

    本模块补齐这一半，做法与另外三条通道完全同构：

        15/ 表单 ──PUT──▶ 14/ 校验 → 原子写 config/point-map/active.pointmap.csv
                                              └─▶ P3/iec104_gateway.exe 启动时读

★ 为什么现在可以写 —— 当初把它定成**只读**的理由是怕出现「界面上一套 IOA、
  网关启动时另一套」的两个真相源。那个担心没有失效，处置却错了：正确答案是
  把真相源**收敛成一个文件**，而不是让界面永远只读。加了约定路径之后，界面
  写的就是网关读的那一份，两份合成一份。

  反过来，**只加界面写入而网关不去读它，才是真的多出一个真相源** —— 所以
  这一次两侧必须同时改（`P3/src/access_link_conf.h` 的约定路径 + 本文件）。
  这是本项目第四条配置通道②，与 active.csv / active.conn / active.upstream
  同目录、同一套原子写、同一条"存在但非法 = 硬失败"的纪律。

★ 为什么这里用 CSV 而另外三条用 key=value：
  转发表是"一行一个点"的表格，本来就是表格；而且 CSV 是 `P3/data/
  point_map_default.csv` 已经在用的格式，现场拿到的调度转发表也是这个形状
  —— 两份文件能用同一个肉眼比对方式，比换成别的格式更不容易错。

★ 校验**逐条镜像** `P3/src/iec104_point_map.h` 的 `load_point_map_csv()` 与
  `self_check_tables()`：平台放行的表，端侧一定能加载；端侧会拒的表，平台
  一定先拒。不比端侧松（否则"保存成功"但网关启动不起来），也不比端侧严
  （否则现场照调度下发的表填了，平台却不让存）。

★ 点名集合从 `07/src/rtdb/ems_point_table.c` **解析**得来，不另抄一份 ——
  本项目在手抄点表上出过事故（值全对、测试全绿，只是调度画面 24 个点全灰，
  见 `P3/docs/README.md` §7.1）。

★ 与下端 `pointmap.py` 的一处**刻意不同**：下端点表"行序即索引"（不能删行、
  不能插行），这里没有那个约束 —— IEC104 靠 IOA 定位，不靠行号。所以
  转发表可以任意增删。两种口子很不一样，别互相套。

纯标准库，零第三方依赖 —— 与 14/ 其余部分一致。
"""

from __future__ import annotations

import os
import re
import tempfile
from typing import Any, Optional

import connconf                  # ConnConfError / _as_int：取值域校验只写一份
import pointmap

# ---------------------------------------------------------------------
# 路径约定 —— 与 active.csv / active.conn / active.upstream 同目录
# ---------------------------------------------------------------------
MAP_NAME = "active.pointmap.csv"

# 列入 9 列，顺序即落盘顺序。与 P3 的 loader 逐字对齐。
COLS = ["ioa", "kind", "group", "name", "src", "scale", "offset", "unit", "note"]

# kind → 方向。9 个 token 与 iec104::kind_from_token 的 9 个分支逐条对齐
# （大小写不敏感，parser 里统一 upper）。
KINDS = {
    "MEAS_FLOAT":      "up",
    "MEAS_SCALED":     "up",
    "MEAS_NORMALIZED": "up",
    "SINGLE_POINT":    "up",
    "DOUBLE_POINT":    "up",
    "COUNTER":         "up",
    "SET_FLOAT":       "down",
    "CMD_SINGLE":      "down",
    "CMD_DOUBLE":      "down",
}

# IOA 区间：**千位**必须与类型相符 —— 与 self_check_tables() 的 range_ok
# 同一个判据（`ioa / 1000`）。没列出的三个 MEAS_* 一律落在遥测段 4。
#  1001..1999 遥信   4001..4999 遥测   5001..5999 遥控
#  6001..6999 遥调   8001..8999 累计量
IOA_SEGMENT = {
    "SINGLE_POINT": 1, "DOUBLE_POINT": 1,
    "COUNTER": 8,
    "SET_FLOAT": 6,
    "CMD_SINGLE": 5, "CMD_DOUBLE": 5,
}
IOA_SEGMENT_DEFAULT = 4

IOA_MIN, IOA_MAX = 1, 999999
GROUP_MIN, GROUP_MAX = 0, 16

# 点名真相源。**不抄点名**，要的时候从它里面解析出来。
POINT_TABLE_C = os.path.join(pointmap.PROJECT_ROOT, "07", "src", "rtdb",
                             "ems_point_table.c")

UpMapError = connconf.ConnConfError      # 调用方只 catch 一种（与链路同套路）

_names_cache: Optional[list] = None


# =====================================================================
# 点名真相源
# =====================================================================
def point_names(refresh: bool = False) -> list:
    """解析 `EMS_POINT_NAMES[]` 得到全部合法内部点名。

    上送点的 `src` 必须在这个集合里 —— 否则那个点永远读到 0，而调度侧
    只是看到一个不动的数，没人会往"表填错了"上想（P3 的 self_check 第 1 条）。

    缓存一份即可：点表是编译期真相源，运行期不会变。
    """
    global _names_cache
    if _names_cache is not None and not refresh:
        return _names_cache
    try:
        with open(POINT_TABLE_C, "r", encoding="utf-8") as f:
            text = f.read()
    except OSError as e:
        raise UpMapError(f"读不到点名真相源 {POINT_TABLE_C}：{e}", 500)
    m = re.search(r"EMS_POINT_NAMES\s*\[[^\]]*\]\s*=\s*\{(.*?)\}\s*;", text, re.S)
    if not m:
        raise UpMapError(f"{POINT_TABLE_C} 里找不到 EMS_POINT_NAMES 数组定义", 500)
    # 数组体里可能带 // 注释（EXT 段那几行的分隔注释），先剥掉再取字符串
    body = re.sub(r"//[^\n]*", "", m.group(1))
    names = re.findall(r'"([^"]+)"', body)
    if not names:
        raise UpMapError(f"{POINT_TABLE_C} 的 EMS_POINT_NAMES 里没有解析到点名", 500)
    if len(set(names)) != len(names):
        dup = sorted({n for n in names if names.count(n) > 1})
        raise UpMapError(f"EMS_POINT_NAMES 自身有重复点名：{'/'.join(dup)}", 500)
    _names_cache = names
    return names


# =====================================================================
# 路径
# =====================================================================
def map_path() -> str:
    """约定路径。目录与另外三条配置通道共用 `pointmap.map_dir()`。"""
    return os.path.join(pointmap.map_dir(), MAP_NAME)


def exists() -> bool:
    return os.path.isfile(map_path())


# =====================================================================
# 取值助手
# =====================================================================
def _as_float(v: Any, label: str) -> float:
    if isinstance(v, bool):
        raise UpMapError(f"{label} 应为数值，实际是 {v!r}")
    if isinstance(v, (int, float)):
        x = float(v)
    else:
        s = str(v).strip()
        if not re.fullmatch(r"[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?", s):
            raise UpMapError(f"{label} 应为数值，实际是「{s}」")
        x = float(s)
    if x != x or x in (float("inf"), float("-inf")):
        raise UpMapError(f"{label} 不是有限数：{v!r}")
    return x


def _as_text(v: Any, label: str, allow_comma: bool = False) -> str:
    """文本字段。

    ★ 逗号：**不允许**（note 除外）。这份 CSV 是**不带引号**的裸逗号切分
    （P3 的 loader 就是这么解析的），名字里混一个逗号会让后面所有列整体
    左移一位 —— 端侧不报错，只是每个点的含义都错一格。这种错没法在结果
    上看出来，只能在入口处挡住。
    """
    s = str(v or "").strip()
    if not allow_comma and "," in s:
        raise UpMapError(f"{label} 里不能含逗号（这会截断 CSV 列）：「{s}」")
    if "\r" in s or "\n" in s:
        raise UpMapError(f"{label} 里不能含换行：「{s}」")
    return s


# =====================================================================
# 归一化 + 校验
# =====================================================================
def normalize(rows: Any) -> list:
    """把外部传入的转发表（前端 JSON / CSV 解析结果）转成规范形式并逐条校验。

    抛 UpMapError 且不保存 —— 半张坏表比没有表更危险：它会让人以为
    "已经配好了"，而网关启动不起来时已经是很久以后了。

    ★ 行序在 IEC104 里**没有意义**（靠 IOA 定位），所以允许任意增删改，
      与下端 device_point_map 的"行序即索引"不是一回事。
    """
    if rows is None:
        raise UpMapError("转发表为空：没有传 rows")
    if not isinstance(rows, list):
        raise UpMapError(f"转发表应为数组，实际是 {type(rows).__name__}")
    if not rows:
        # ★ 空表必须拒绝，而不是"写一个只有表头的文件"。
        #   那样网关会读到一个"存在但一行没有"的表（存在 → 加载 → 0 点），
        #   于是一个点都不上送，而平台界面上显示的是"已保存"。
        #   想回到内置默认表请调 delete_file() —— 那是一个明确的动作。
        raise UpMapError(
            "转发表一行都没有 —— IEC104 靠 IOA 定位，一张空表等于「一个点都不上送」。"
            "要恢复内置默认表请点「恢复默认」（那会删掉已保存的文件），而不是保存空表。")

    names = point_names()
    out = []
    seen = {}

    for i, raw in enumerate(rows, start=1):
        if not isinstance(raw, dict):
            raise UpMapError(f"第 {i} 行应为对象，实际是 {type(raw).__name__}")
        tag = f"第 {i} 行"

        ioa = connconf._as_int(raw.get("ioa"), IOA_MIN, IOA_MAX, f"{tag} IOA(ioa)")

        kind_raw = str(raw.get("kind") or "").strip().upper()
        if kind_raw not in KINDS:
            raise UpMapError(
                f"{tag} kind 应为 {'/'.join(KINDS)}（大小写不敏感），"
                f"实际是「{raw.get('kind')}」")
        kind = kind_raw
        direction = KINDS[kind]

        group = connconf._as_int(raw.get("group"), GROUP_MIN, GROUP_MAX,
                                 f"{tag} 总召唤分组(group)")

        scale = _as_float(raw.get("scale", 1.0), f"{tag} 系数(scale)")
        if scale == 0.0:
            # 端侧 from_protocol() 会拿它做除数 —— 这里是唯一能拦住的地方
            raise UpMapError(f"{tag} scale 不能为 0（下行换算要拿它做除数）")
        offset = _as_float(raw.get("offset", 0.0), f"{tag} 偏置(offset)")

        name = _as_text(raw.get("name"), f"{tag} 名称(name)")
        if not name:
            raise UpMapError(f"{tag} 名称(name) 为空 —— 调度侧看到的只有 IOA，"
                             f"名字是现场唯一的对照依据")
        unit = _as_text(raw.get("unit"), f"{tag} 单位(unit)")
        note = _as_text(raw.get("note"), f"{tag} 说明(note)", allow_comma=True)

        src = _as_text(raw.get("src"), f"{tag} EMS 点名(src)")
        if direction == "up":
            if not src:
                raise UpMapError(f"{tag} {name} 是上送点，必须填 EMS 点名(src) —— "
                                 f"没有点名就不知道该读哪个点的值")
            if src not in names:
                raise UpMapError(
                    f"{tag} src 点名「{src}」不在 EMS 点表里"
                    f"（真相源 {POINT_TABLE_C}）。没拦住的话这个点永远读到 0，"
                    f"而调度侧只看见一个数不动 —— 没人会想到是表填错了")
        # 下行点 src 允许为空：它的值来自调度，不来自点表（P3 内置表同理）。
        # 填了也要求存在，否则说明是照着上送随手粘过来的。
        elif src and src not in names:
            raise UpMapError(f"{tag} src 点名「{src}」不在 EMS 点表里")

        seg = IOA_SEGMENT.get(kind, IOA_SEGMENT_DEFAULT)
        if ioa // 1000 != seg:
            want = {1: "遥信(1xxx)", 4: "遥测(4xxx)", 5: "遥控(5xxx)",
                    6: "遥调(6xxx)", 8: "累计量(8xxx)"}[seg]
            raise UpMapError(
                f"{tag} IOA {ioa} 与类型 {kind} 不匹配 —— {kind} 应落在 {want} 区间。"
                f"调度按 IOA 段决定怎么解码，落在错段会被当成另一种量读走")

        if ioa in seen:
            raise UpMapError(f"{tag} IOA {ioa} 与第 {seen[ioa]} 行重复 —— "
                             f"两个点共用一个地址，调度收到的是后写入的那个，"
                             f"且没有任何东西会报错")
        seen[ioa] = i

        out.append({
            "ioa": ioa, "kind": kind, "group": group, "name": name,
            "src": src, "scale": scale, "offset": offset,
            "unit": unit, "note": note, "direction": direction,
        })

    return out


def counts_of(rows: list) -> dict:
    up = sum(1 for r in rows if r.get("direction") != "down")
    return {"up": up, "down": len(rows) - up, "total": len(rows)}


# =====================================================================
# 序列化 / 解析
# =====================================================================
def dumps(rows: list, saved_at: str = "", username: str = "") -> str:
    """生成落盘文本。不带引号的裸逗号切分 —— 与 P3 的 loader 同一口径。

    scale/offset 用 `%g` 打印（与 13/ 的 export_point_map_csv 一致）：
    `100.0` 写成 `100`，现场肉眼比对时不会因为 "100.0" vs "100" 起疑。
    """
    rs = normalize(rows)
    c = counts_of(rs)
    lines = [
        "# " + "=" * 68,
        "#  上端 IOA 转发表（调度主站接入 · EMS 作 IEC104 从站）",
        "#  由 14/ 平台自动生成，请勿手工修改 —— 手工改会在下次保存时被覆盖。",
        "#",
        f"#  点数      上送 {c['up']} 点 + 下行 {c['down']} 点 = {c['total']} 行",
        f"#  保存时刻  {saved_at or '-'}",
        f"#  保存人    {username or '-'}",
        "#",
        "#  读取方    P3\\build\\iec104_gateway.exe（启动时，列在 --point-map 之后）",
        "#  列顺序    ioa,kind,group,name,src,scale,offset,unit,note",
        "#  同一目录下的另外三个文件：active.csv 是下端设备点表、",
        "#            active.conn 是下端接入参数、active.upstream 是上端链路参数。",
        "#",
        "#  IOA 区间  1001..1999 遥信   4001..4999 遥测   5001..5999 遥控",
        "#            6001..6999 遥调   8001..8999 累计量",
        "# " + "=" * 68,
        ",".join(COLS),
    ]
    for r in rs:
        lines.append(",".join([
            str(r["ioa"]), r["kind"], str(r["group"]), r["name"], r["src"],
            "%g" % r["scale"], "%g" % r["offset"], r["unit"], r["note"],
        ]))
    return "\r\n".join(lines) + "\r\n"


def parse(text: str) -> list:
    """把落盘文本（或现场照着填的 CSV）解析成行并校验。

    宽容度与 P3 的 loader 一致：
      · 剥 UTF-8 BOM、跳过空行与 `#` 注释行
      · 首列名小写后 == "ioa" 的那行当表头跳过
      · 前 8 个逗号切字段，剩下的整体当 note（note 允许含逗号）
    """
    if text.startswith("\ufeff"):
        text = text[1:]

    raw = []
    first = True
    for lineNo, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(",")
        head = parts[0].replace(" ", "").replace("\t", "").lower()
        if first and head == "ioa":
            first = False
            continue
        first = False
        if len(parts) < len(COLS):
            raise UpMapError(f"第 {lineNo} 行只有 {len(parts)} 列，应为 {len(COLS)} 列：{line[:60]}")
        f = parts[:8] + [",".join(parts[8:])]
        raw.append(dict(zip(COLS, [x.strip() for x in f])))
    return normalize(raw)


# =====================================================================
# 落盘 / 删除
# =====================================================================
def save(rows: list, saved_at: str = "", username: str = "") -> dict:
    """校验 → 原子落盘。返回 {"path","bytes","rows","counts"}。

    写入必须是原子的：网关在另一个进程、可能在任意时刻读它 —— 直接覆盖会
    让它读到写了一半的文件，而半张表能通过"表头检查"，只是少几个点。
    """
    rs = normalize(rows)
    path = map_path()
    payload = b"\xef\xbb\xbf" + dumps(rs, saved_at, username).encode("utf-8")  # BOM：Excel 打开不乱码
    _atomic_write(path, payload)
    return {"path": path, "bytes": len(payload), "rows": rs, "counts": counts_of(rs)}


def _atomic_write(path: str, payload: bytes) -> None:
    d = os.path.dirname(os.path.abspath(path))
    os.makedirs(d, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".upmap_", suffix=".tmp", dir=d)
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


def delete_file() -> bool:
    """删掉已保存的表 —— "恢复内置默认"的唯一正规途径。

    这与保存空表是两件事：文件不存在时网关用编译进去的默认表（24 上送 + 6
    下行）；文件存在且合法就用它。**不存在 ≠ 没配置**，这是另外三条
    配置通道共用的那条纪律。
    """
    p = map_path()
    if not os.path.isfile(p):
        return False
    os.unlink(p)
    return True


def load_file(path: Optional[str] = None) -> list:
    p = path or map_path()
    if not os.path.isfile(p):
        raise UpMapError(f"转发表文件不存在：{p}", 404)
    with open(p, "r", encoding="utf-8-sig") as f:
        return parse(f.read())


# =====================================================================
# 落库 —— 库是平台侧的持久存档，文件是给端侧的那一份
#
# ★ 为什么不像链路参数那样只落文件：这份表是现场对照的依据，30 行 IOA
#   一旦丢了就得对着调度给的纸质转发表重填一遍。而 `config/point-map/`
#   整个目录是被 gitignore 的运行时产物 —— 它承担不了"存档"这件事。
#
# ★ 为什么列名叫 grp / offset_v：group 与 offset 在 SQL 里是保留字/易混
#   词，给它们套引号不如换个不会撞的名字。这与 pointmap.py 把 table 列存成
#   table_kind 是同一个原因。
# =====================================================================
DB_COLS = ["ioa", "kind", "grp", "name", "src", "scale", "offset_v",
           "unit", "note"]

DB_INSERT_SQL = (
    "INSERT INTO upstream_point_map(" + ",".join(DB_COLS) + ") "
    "VALUES(" + ",".join(["?"] * len(DB_COLS)) + ")"
)


def to_db_rows(rows: list) -> list:
    rs = normalize(rows)
    return [(r["ioa"], r["kind"], int(r["group"]), r["name"], r["src"],
             float(r["scale"]), float(r["offset"]), r["unit"], r["note"])
            for r in rs]


def from_db_rows(db_rows: list) -> list:
    """把库里的行还原成内部行结构（按 IOA 升序 —— 只为了稳定，行序本身无意义）。"""
    raw = [{
        "ioa": int(r["ioa"]), "kind": r["kind"], "group": int(r["grp"]),
        "name": r["name"], "src": r["src"], "scale": float(r["scale"]),
        "offset": float(r["offset_v"]), "unit": r.get("unit") or "",
        "note": r.get("note") or "",
    } for r in sorted(db_rows, key=lambda x: int(x["ioa"]))]
    return normalize(raw)


def saved_at_from_text(text: str) -> str:
    """从文件注释头里取出保存时刻。

    这点元信息不值得单开一张表，但界面要显示"上次谁改的、什么时候改的" ——
    写在文件头里，读文件的人（现场拿记事本打开的那种）也看得见。
    """
    m = re.search(r"^#\s*保存时刻\s+(\S+(?:\s+\S+)?)", text, re.M)
    return m.group(1).strip() if m else ""


def username_from_text(text: str) -> str:
    m = re.search(r"^#\s*保存人\s+(\S+)", text, re.M)
    return m.group(1).strip() if m else ""


def summary(rows: list) -> dict:
    """给界面用的一小组信息。不合法时把原因带回来，而不是抛。"""
    try:
        rs = normalize(rows)
    except connconf.ConnConfError as e:
        return {"ok": False, "error": e.message}
    return dict({"ok": True}, **counts_of(rs))
