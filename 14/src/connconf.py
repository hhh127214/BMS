"""14/ — 设备接入参数（平台侧：序列化、落盘）

为什么需要这份模块：
    点表已经有「配置通道①：约定文件路径」（`pointmap.py` 写 `active.csv`，
    13/ 启动时读它）。但**接入参数**（IP / 端口 / 从站号）一直只存在数据库里 ——
    用户在 15/ 界面上填完，13/ 与 07/ 却仍然必须靠命令行 `--host ... --port ...`
    才连得上。于是现场出现两种难查的不一致：
      · 界面上写着 192.168.1.10，命令行敲的是 192.168.1.1 —— 两边都不报错；
      · 现场工程师换了人，没人知道该敲哪一串参数。
    这正是本项目最忌讳的「看起来在工作」。

本模块把接入参数**也**落成端侧能直接读的文件 —— 「配置通道②」：

    15/ 表单 ──PUT──▶ 14/ 写 device_conn 表
                       └─▶ 原子写 config/point-map/active.conn
                                    └─▶ 13/ 07/ 启动时读它（不给 --host 时）

★ 与 active.csv **同目录、同一次保存、同生共死**。
  为什么同目录：一次装配 = 一组接入参数 + 一份点表（见 schema.sql 的
  device_conn 注释），两者必须来自同一次保存，否则会出现「新 IP 连旧点表」。
  为什么由 `pointmap.map_dir()` 定目录：目录约定只能有一处（环境变量
  EMS_POINT_MAP_DIR 同时覆盖两者），多一处就会分叉。

★ 文件格式刻意用 `key=value` 而不是 CSV / JSON：
  · CSV 是给「一行一个点」的表格用的，接入参数是标量键值，套表格反而绕；
  · JSON 在 C++ 侧要么引第三方库、要么手写解析器 —— 本项目通篇只用标准库，
    为一个十来行的配置引入解析器不划算。
  · `key=value` 现场肉眼可读、Excel 也能开（按 `=` 分列）。

★ 未知键**硬失败**（不忽略）：
  拼错一个键名（`hots=` / `prot=`）如果被静默忽略，程序会退回默认值去连 ——
  连的是另一台设备、另一个端口，而屏幕上一切正常。宁可当场报错。
  这是与 13/ 的 loader 逐条对齐的行为（见 13/src/device_conn_conf.h）。

纯标准库，零第三方依赖 —— 与 14/ 其余部分一致。
"""

from __future__ import annotations

import os
import re
import tempfile
from typing import Any, Optional

import pointmap

# ---------------------------------------------------------------------
# 路径约定 —— 与 active.csv 同目录
# ---------------------------------------------------------------------
CONN_NAME = "active.conn"

# 文件格式版本。将来加字段时改这里，两侧一起变（老端侧读到新格式会明确报错，
# 而不是把不认识的字段默默丢掉）。
FORMAT_TAG = "ems-device-conn/1"

# 文件的键。顺序 = 落盘顺序（现场肉眼比对时稳定，不随 dict 顺序漂移）。
KEYS = [
    "format", "device_id", "protocol", "host", "port", "unit_id",
    "poll_period_ms", "timeout_ms", "auto_reconnect", "enabled",
    "saved_at", "username",
]

# 必填键。少了任何一个，端侧都无法确定"连哪台设备" —— 一律拒绝。
REQUIRED = [
    "format", "device_id", "protocol", "host", "port", "unit_id",
    "poll_period_ms", "timeout_ms", "auto_reconnect", "enabled",
]

# 只给人看的元信息，端侧不解析（缺了不影响连接）
META = ["saved_at", "username"]

PROTOCOLS = ("modbus_tcp",)

PORT_MIN, PORT_MAX = 1, 65535
UNIT_ID_MIN, UNIT_ID_MAX = 1, 247      # 0 是广播地址，不能作为正常从站
POLL_MIN_MS, POLL_MAX_MS = 10, 60000
TIMEOUT_MIN_MS, TIMEOUT_MAX_MS = 10, 60000

_RE_HOST = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._-]{0,62}$")


class ConnConfError(Exception):
    """接入参数文件不合法。"""

    def __init__(self, message: str, status: int = 400) -> None:
        super().__init__(message)
        self.message = message
        self.status = status


# =====================================================================
# 路径
# =====================================================================
def conn_path() -> str:
    """接入参数文件路径。目录与点表共用 `pointmap.map_dir()`。"""
    return os.path.join(pointmap.map_dir(), CONN_NAME)


def conn_path_in_dir(directory: str) -> str:
    """指定目录下的接入参数路径（测试用，避免去改环境变量）。"""
    return os.path.join(directory, CONN_NAME)


# =====================================================================
# 归一化
# =====================================================================
def _as_int(v: Any, lo: int, hi: int, label: str) -> int:
    if isinstance(v, bool):
        raise ConnConfError(f"{label} 应为整数，实际是 {v!r}")
    if isinstance(v, int):
        n = v
    else:
        s = str(v).strip()
        if not re.fullmatch(r"[+-]?\d+", s):
            raise ConnConfError(f"{label} 应为整数，实际是「{s}」")
        n = int(s)
    if not (lo <= n <= hi):
        raise ConnConfError(f"{label} 应在 {lo}..{hi}，实际是 {n}")
    return n


def _as_flag(v: Any, label: str) -> int:
    if isinstance(v, bool):
        return 1 if v else 0
    s = str(v).strip().lower()
    if s in ("1", "true", "yes", "on"):
        return 1
    if s in ("0", "false", "no", "off"):
        return 0
    raise ConnConfError(f"{label} 应为 0/1，实际是「{v}」")


def normalize(conn: dict) -> dict:
    """把外部传入的接入参数（DB 行 / 前端 JSON / 文件解析结果）转成规范形式。

    三种来源的字段类型不一样（DB 是 INTEGER、前端是 int/float/bool、
    文件全是字符串），所以按类型分派，不强行 str()。**幂等**。
    """
    host = str(conn.get("host") or "").strip()
    if not host:
        raise ConnConfError("设备地址(host)为空 —— 没有它端侧无法确定连哪台设备")
    if not _RE_HOST.match(host):
        raise ConnConfError(
            f"设备地址应是 IPv4 或主机名（只允许字母数字与 . _ -），实际是「{host}」")

    proto = str(conn.get("protocol") or "modbus_tcp").strip().lower()
    if proto not in PROTOCOLS:
        raise ConnConfError(f"协议应为 {'/'.join(PROTOCOLS)}，实际是「{proto}」")

    device_id = str(conn.get("device_id") or "").strip()
    if not device_id:
        raise ConnConfError("设备号(device_id)为空")

    return {
        "format": FORMAT_TAG,
        "device_id": device_id,
        "protocol": proto,
        "host": host,
        "port": _as_int(conn.get("port", 502), PORT_MIN, PORT_MAX, "端口(port)"),
        "unit_id": _as_int(conn.get("unit_id", 1), UNIT_ID_MIN, UNIT_ID_MAX,
                           "从站号(unit_id)"),
        "poll_period_ms": _as_int(conn.get("poll_period_ms", 100),
                                  POLL_MIN_MS, POLL_MAX_MS, "轮询周期(poll_period_ms)"),
        "timeout_ms": _as_int(conn.get("timeout_ms", 1000),
                              TIMEOUT_MIN_MS, TIMEOUT_MAX_MS, "超时(timeout_ms)"),
        "auto_reconnect": _as_flag(conn.get("auto_reconnect", 1), "自动重连(auto_reconnect)"),
        "enabled": _as_flag(conn.get("enabled", 0), "启用(enabled)"),
        "saved_at": str(conn.get("saved_at") or "").strip(),
        "username": str(conn.get("username") or "").strip(),
    }


# =====================================================================
# 序列化 / 解析
# =====================================================================
def dumps(conn: dict) -> str:
    """生成落盘文本（注释头 + key=value）。

    注释头是给现场人看的（13/ 的 loader 会跳过 `#` 行）：写清这文件谁生成的、
    端侧从哪儿读它、以及「别手工改这个文件」。
    """
    c = normalize(conn)
    lines = [
        "# " + "=" * 68,
        "#  设备接入参数  ——  由 14/ 平台自动生成，请勿手工修改",
        "#",
        f"#  设备      {c['device_id']}",
        f"#  地址      {c['host']}:{c['port']}  从站 {c['unit_id']}",
        f"#  保存时刻  {c['saved_at'] or '-'}",
        f"#  保存人    {c['username'] or '-'}",
        "#",
        "#  13/ 与 07/ 启动时读本文件（同目录的 active.csv 是点表）：",
        "#     13\\build\\modbus_probe.exe            ← 不给 --host 时用它",
        "#     07\\build\\main_field.exe --device modbus",
        "#  命令行给 --host/--port/--unit 时**以命令行为准**（临时核对用）。",
        "#",
        "#  改接入参数的正规途径是 15/ 界面的「设备接入配置」；",
        "#  **手工改这个文件会在下次界面保存时被覆盖。**",
        "# " + "=" * 68,
        "",
    ]
    for k in KEYS:
        v = c.get(k)
        if k in META and v == "":
            continue                       # 元信息为空就不写这一行
        lines.append(f"{k}={v}")
    return "\r\n".join(lines) + "\r\n"


def parse(text: str) -> dict:
    """把落盘文本解析成接入参数 dict。任一环节不合法即抛 ConnConfError。

    与 13/ 的 loader 同一套宽容度与同一套严格性：
      · 剥 UTF-8 BOM（Excel 存出来会带）
      · 跳过空行与 `#` 注释行
      · **未知键硬失败**（拼错键名必须当场发现）
    """
    if text.startswith("\ufeff"):
        text = text[1:]

    raw: dict[str, str] = {}
    for lineNo, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ConnConfError(
                f"第 {lineNo} 行不是 key=value 形式：{line[:60]}")
        k, _, v = line.partition("=")
        k, v = k.strip(), v.strip()
        if not k:
            raise ConnConfError(f"第 {lineNo} 行的键为空：{line[:60]}")
        if k not in KEYS:
            raise ConnConfError(
                f"第 {lineNo} 行出现不认识的键「{k}」"
                f"（认识的键：{'/'.join(KEYS)}）。"
                f"拼错的键若被忽略，程序会拿默认值去连另一台设备 —— 所以这里直接拒绝。")
        if k in raw:
            raise ConnConfError(f"第 {lineNo} 行键「{k}」重复出现")
        raw[k] = v

    missing = [k for k in REQUIRED if k not in raw]
    if missing:
        raise ConnConfError(
            f"接入参数缺少必填键：{'/'.join(missing)}"
            f"（文件可能被手工截断，或来自不同版本的端侧）")

    if raw["format"] != FORMAT_TAG:
        raise ConnConfError(
            f"文件格式版本不匹配：期望「{FORMAT_TAG}」，实际是「{raw['format']}」")

    return normalize(raw)


# =====================================================================
# 落盘
# =====================================================================
def save(conn: dict) -> dict:
    """把接入参数落盘。返回 {"path", "bytes"}。

    写入是**原子**的（先写同目录临时文件再 os.replace）：13/ 是另一个进程，
    可能在任意时刻读它。直接覆盖会让它读到写了一半的文件 —— 半个配置
    能通过语法解析、只是 host 读成了空，这是最坏的一种失败。
    """
    c = normalize(conn)
    path = conn_path()
    payload = b"\xef\xbb\xbf" + dumps(c).encode("utf-8")   # BOM：Excel 打开不乱码
    _atomic_write(path, payload)
    return {"path": path, "bytes": len(payload)}


def _atomic_write(path: str, payload: bytes) -> None:
    d = os.path.dirname(os.path.abspath(path))
    os.makedirs(d, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".conn_", suffix=".tmp", dir=d)
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
    """读回接入参数（界面上「已保存的接入参数」用它）。"""
    p = path or conn_path()
    if not os.path.isfile(p):
        raise ConnConfError(f"接入参数文件不存在：{p}", 404)
    with open(p, "r", encoding="utf-8-sig") as f:
        return parse(f.read())


def summary(conn: dict) -> dict:
    """给界面用的一小组统计。不合法时把原因带回来，而不是抛。"""
    try:
        c = normalize(conn)
    except ConnConfError as e:
        return {"ok": False, "error": e.message}
    return {"ok": True, "host": c["host"], "port": c["port"],
            "unit_id": c["unit_id"], "enabled": bool(c["enabled"])}
