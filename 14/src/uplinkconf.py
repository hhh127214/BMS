"""14/ — 上端（调度侧）链路参数（平台侧：序列化、落盘）

为什么需要这份模块：
    下端（`connconf.py`，active.conn）早就解决了「界面上填的 IP 与现场敲的
    参数不是同一个」。上端是**同一个坑的另一半**：`P3/iec104_gateway.exe`
    只认命令行 `--port/--ca/--bind`，而客户在 15/ 界面上看到的「2404 / CA 1」
    没有任何东西保证和现场启动参数一致。于是调度那边收不到数、平台上
    一切正常 —— 本项目最忌讳的「看起来在工作」。

    所以上端也走同一条路：**配置通道②**，落成端侧直接读的文件。

    15/ 表单 ──PUT──▶ 14/ 写 upstream_link 表
                       └─▶ 原子写 config/point-map/active.upstream
                                    └─▶ P3/iec104_gateway.exe 启动时读它（不给 --port 时）

★ 与 active.conn **同目录**（`pointmap.map_dir()`）：
  一次现场装配 = 上端一条链路 + 下端一组接入参数 + 一份点表。三者必须在
  同一个目录、同一批时间点落盘，否则会出现「上端按新配置、下端读旧文件」
  这种只在联调时才暴露的不一致。目录约定只允许有一处。

★ 为什么上端是**一条**、下端是**多台**：
  下端 EMS 是主站，一轮询一个从站地址，一台设备一份参数；
  上端 EMS 是**从站**，在同一端口上等调度连进来，全站只有一条链路。
  这不是"暂时只做一条"，是两端的角色决定的（见 14/schema.sql 的
  upstream_link 注释）。

★ kind = real / sim：
  'real' = 上级调度主站连进来；'sim' = `P3/iec104_master.exe` 当模拟调度。
  两者跑的是**同一套从站代码**，差别只在"谁来连" —— 所以模拟源不是第二条
  代码路径，不需要在端侧分叉。

纯标准库，零第三方依赖 —— 与 14/ 其余部分一致。
"""

from __future__ import annotations

import os
import re
import tempfile
from typing import Any, Optional

import connconf          # ConnConfError / _as_int / _as_flag：取值域校验只写一份
import pointmap

# ---------------------------------------------------------------------
# 路径约定 —— 与 active.csv / active.conn 同目录
# ---------------------------------------------------------------------
LINK_NAME = "active.upstream"

# 文件格式版本。加字段时两侧一起变（老端侧读到新格式会明确报错，
# 而不是把不认识的字段默默丢掉）。
FORMAT_TAG = "ems-upstream-link/1"

# 落盘顺序 = 现场肉眼比对顺序
KEYS = [
    "format", "link_id", "kind", "protocol", "bind_host",
    "listen_port", "common_addr", "master_host", "enabled",
    "saved_at", "username",
]

REQUIRED = ["format", "link_id", "kind", "protocol",
            "listen_port", "common_addr", "enabled"]

META = ["saved_at", "username"]

KINDS = ("real", "sim")
PROTOCOLS = ("iec104",)

LINK_ID = "dispatch"                 # 全站唯一：一个从站只有一条链路
BIND_DEFAULT = "0.0.0.0"
PORT_DEFAULT = 2404                  # IEC 60870-5-104 标准端口
PORT_MIN, PORT_MAX = 1, 65535
CA_MIN, CA_MAX = 1, 65534            # 0 与 65535 保留
MASTER_DEFAULT = "127.0.0.1"

_RE_HOST = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._-]{0,62}$")

# 与 connconf 复用的错误类型（调用方只 catch 一种）
LinkConfError = connconf.ConnConfError


# =====================================================================
# 路径
# =====================================================================
def link_path() -> str:
    """上端链路文件路径。目录与点表 / active.conn 共用 `pointmap.map_dir()`。"""
    return os.path.join(pointmap.map_dir(), LINK_NAME)


def link_path_in_dir(directory: str) -> str:
    """指定目录下的路径（测试用，避免去改环境变量）。"""
    return os.path.join(directory, LINK_NAME)


# =====================================================================
# 归一化
# =====================================================================
def normalize(link: dict) -> dict:
    """把外部传入的链路参数（DB 行 / 前端 JSON / 文件解析结果）转成规范形式。幂等。"""
    link_id = str(link.get("link_id") or LINK_ID).strip() or LINK_ID
    if link_id != LINK_ID:
        raise LinkConfError(
            f"链路号(link_id)只能是「{LINK_ID}」—— 一个从站只有一条上端链路，"
            f"实际是「{link_id}」")

    kind = str(link.get("kind") or "real").strip().lower()
    if kind not in KINDS:
        raise LinkConfError(
            f"对端形态(kind)应为 {'/'.join(KINDS)}，实际是「{kind}」")

    proto = str(link.get("protocol") or "iec104").strip().lower()
    if proto not in PROTOCOLS:
        raise LinkConfError(
            f"协议应为 {'/'.join(PROTOCOLS)}，实际是「{proto}」")

    bind = str(link.get("bind_host") or BIND_DEFAULT).strip() or BIND_DEFAULT
    if not _RE_HOST.match(bind):
        raise LinkConfError(
            f"监听网卡(bind_host)应是 IPv4 或 0.0.0.0，实际是「{bind}」")

    master = str(link.get("master_host") or "").strip() or MASTER_DEFAULT
    if not _RE_HOST.match(master):
        raise LinkConfError(
            f"模拟主站地址(master_host)应是 IPv4 或主机名，实际是「{master}」")

    return {
        "format": FORMAT_TAG,
        "link_id": link_id,
        "kind": kind,
        "protocol": proto,
        "bind_host": bind,
        "listen_port": connconf._as_int(link.get("listen_port", PORT_DEFAULT),
                                        PORT_MIN, PORT_MAX, "监听端口(listen_port)"),
        "common_addr": connconf._as_int(link.get("common_addr", 1),
                                        CA_MIN, CA_MAX, "公共地址(common_addr)"),
        "master_host": master,
        "enabled": connconf._as_flag(link.get("enabled", 1), "启用(enabled)"),
        "saved_at": str(link.get("saved_at") or "").strip(),
        "username": str(link.get("username") or "").strip(),
    }


def peer(link: dict) -> dict:
    """上端这一端到底「跟谁对接」——给界面与文件头用的一句话说清。

    与下端 `connconf.target()` 同一个思路：结论只能算一处，
    否则界面说的和端侧做的迟早会分叉。
    """
    c = link
    if c.get("format") != FORMAT_TAG or "kind" not in c:
        c = normalize(link)
    if c["kind"] == "sim":
        return {"kind": "sim",
                "who": "本地模拟调度主站（P3/iec104_master.exe）",
                "expect_host": c["master_host"],
                "port": c["listen_port"],
                "is_real": False}
    return {"kind": "real",
            "who": "上级调度主站（对方发起连接）",
            "expect_host": "",            # 由对方决定，平台不做白名单（现场常是动态地址）
            "port": c["listen_port"],
            "is_real": True}


# =====================================================================
# 序列化 / 解析
# =====================================================================
def dumps(link: dict) -> str:
    """生成落盘文本（注释头 + key=value）。注释头写给现场人看。"""
    c = normalize(link)
    pr = peer(c)
    lines = [
        "# " + "=" * 68,
        "#  上端链路参数（IEC 60870-5-104 从站）  ——  由 14/ 平台自动生成，请勿手工修改",
        "#",
        f"#  形态      {'模拟调度（★ 不是上级调度）' if c['kind'] == 'sim' else '上级调度主站'}",
        f"#  监听      {c['bind_host']}:{c['listen_port']}   公共地址 CA={c['common_addr']}",
        f"#  对端      {pr['who']}",
        f"#  保存时刻  {c['saved_at'] or '-'}",
        f"#  保存人    {c['username'] or '-'}",
        "#",
        "#  P3/ 启动时读本文件（同目录的 active.csv 是点表、active.conn 是下端接入参数）：",
        "#     P3\\build\\iec104_gateway.exe          ← 不给 --port/--ca 时用它",
        "#  命令行给 --port/--ca/--bind 时**以命令行为准**（临时核对用）。",
        "#",
        "#  改这里的正规途径是 15/ 界面的「上端接入」；**手工改会在下次保存时被覆盖。**",
        "# " + "=" * 68,
        "",
    ]
    for k in KEYS:
        v = c.get(k)
        if k in META and v == "":
            continue
        lines.append(f"{k}={v}")
    return "\r\n".join(lines) + "\r\n"


def parse(text: str) -> dict:
    """把落盘文本解析成链路参数。任一环节不合法即抛 LinkConfError。

    宽容度与严格性与 connconf.parse / 13/ 的 loader 一致：
      · 剥 UTF-8 BOM、跳过空行与 `#` 注释行
      · **未知键硬失败**（拼错的键被忽略 ⇒ 拿默认 2404 去监听，而调度连的是别的口）
    """
    if text.startswith("\ufeff"):
        text = text[1:]

    raw: dict[str, str] = {}
    for lineNo, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise LinkConfError(f"第 {lineNo} 行不是 key=value 形式：{line[:60]}")
        k, _, v = line.partition("=")
        k, v = k.strip(), v.strip()
        if not k:
            raise LinkConfError(f"第 {lineNo} 行的键为空：{line[:60]}")
        if k not in KEYS:
            raise LinkConfError(
                f"第 {lineNo} 行出现不认识的键「{k}」（认识的键：{'/'.join(KEYS)}）。"
                f"拼错的键若被忽略，从站会拿默认端口去监听 —— 所以这里直接拒绝。")
        if k in raw:
            raise LinkConfError(f"第 {lineNo} 行键「{k}」重复出现")
        raw[k] = v

    missing = [k for k in REQUIRED if k not in raw]
    if missing:
        raise LinkConfError(
            f"上端链路参数缺少必填键：{'/'.join(missing)}"
            f"（文件可能被手工截断，或来自不同版本的端侧）")

    if raw["format"] != FORMAT_TAG:
        raise LinkConfError(
            f"文件格式版本不匹配：期望「{FORMAT_TAG}」，实际是「{raw['format']}」")

    return normalize(raw)


# =====================================================================
# 落盘
# =====================================================================
def save(link: dict) -> dict:
    """把链路参数落盘。返回 {"path", "bytes"}。写入是**原子**的。

    端侧是另一个进程，可能在任意时刻读它：直接覆盖会让它读到写了一半的
    文件 —— 半个配置能通过语法解析，只是端口读成了空，这是最坏的失败。
    """
    c = normalize(link)
    path = link_path()
    payload = b"\xef\xbb\xbf" + dumps(c).encode("utf-8")   # BOM：Excel 打开不乱码
    _atomic_write(path, payload)
    return {"path": path, "bytes": len(payload)}


def _atomic_write(path: str, payload: bytes) -> None:
    d = os.path.dirname(os.path.abspath(path))
    os.makedirs(d, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".uplink_", suffix=".tmp", dir=d)
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


def load_file(path: Optional[str] = None) -> dict:
    p = path or link_path()
    if not os.path.isfile(p):
        raise LinkConfError(f"上端链路文件不存在：{p}", 404)
    with open(p, "r", encoding="utf-8-sig") as f:
        return parse(f.read())


def summary(link: dict) -> dict:
    """给界面用的一小组信息。不合法时把原因带回来，而不是抛。"""
    try:
        c = normalize(link)
    except connconf.ConnConfError as e:
        return {"ok": False, "error": e.message}
    pr = peer(c)
    return {"ok": True, "kind": c["kind"], "bind_host": c["bind_host"],
            "listen_port": c["listen_port"], "common_addr": c["common_addr"],
            "master_host": c["master_host"], "peer": pr["who"],
            "enabled": bool(c["enabled"])}
