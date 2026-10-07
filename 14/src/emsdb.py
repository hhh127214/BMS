"""14/ — 平台层数据库访问

只用 Python 标准库（sqlite3），零第三方依赖。
与项目其余部分一致：不引入需要联网安装的东西。

对外提供：
    connect(db_path=None)       取连接（row_factory = sqlite3.Row）
    init_db(db_path=None)       按 schema.sql 建表（幂等）
    query(conn, sql, args)      查询 -> list[dict]
    query_one(conn, sql, args)  查询 -> dict | None
    execute(conn, sql, args)    写 -> lastrowid
    table_counts(conn)          各表行数（自检与首页用）
"""

from __future__ import annotations

import os
import sqlite3
from typing import Any, Iterable, Optional, Sequence

# 14/ 模块根目录（本文件在 14/src/ 下）
MODULE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DB_PATH = os.path.join(MODULE_ROOT, "data", "ems.db")
SCHEMA_PATH = os.path.join(MODULE_ROOT, "schema.sql")

# 导入器写入的元信息键
META_SCHEMA_VERSION = "schema_version"
META_IMPORT_SOURCE = "import_source"
META_IMPORT_AT = "import_at"
SCHEMA_VERSION = "1.4"

# ---------------------------------------------------------------------
# 增量迁移（v1.2）
#
# 为什么需要：库里已经有 v1.1 的 ems.db，而 schema.sql 全是
# CREATE TABLE IF NOT EXISTS —— 对**已存在的表**不会补新列。现场不可能
# 为了加一列把数据库删了重建（里面有导入好的时序与审计），所以新列必须
# 用 ALTER TABLE 幂等补上。
#
# 写法：每项 (表, 列, 列定义)，逐个 PRAGMA table_info 检查后再 ALTER。
# 只做"加列"，不做改类型/删列 —— 那两类操作在 SQLite 上不可靠，
# 真要动就得走"建新表→搬数据→改名"的重建流程，不在本函数职责内。
# ---------------------------------------------------------------------
MIGRATIONS: list[tuple[str, str, str]] = [
    # 时刻口径：'sim' 仿真秒数 / 'wall' Unix 墙钟秒
    ("scenario", "time_base", "TEXT NOT NULL DEFAULT 'sim'"),
    # 电度电费基线 4 字段（2026-09-28 补）：10/ 早已算出，但 summary.json
    # 起就漏序列化，schema/import 也一路漏到库，前端拿不到。
    ("economics", "cost_energy_base_cny", "REAL DEFAULT 0"),
    ("economics", "cost_demand_base_cny", "REAL DEFAULT 0"),
    ("economics", "revenue_feed_in_cny", "REAL DEFAULT 0"),
    ("economics", "revenue_feed_in_base_cny", "REAL DEFAULT 0"),
]


def _table_columns(conn: sqlite3.Connection, table: str) -> set[str]:
    try:
        rows = conn.execute(f"PRAGMA table_info({table})").fetchall()
    except sqlite3.Error:
        return set()
    return {str(r[1]) for r in rows}


def migrate(conn: sqlite3.Connection) -> list[str]:
    """把旧库补齐到当前 schema。返回实际执行的 ALTER 语句（供自检打印）。"""
    done: list[str] = []
    for table, column, decl in MIGRATIONS:
        cols = _table_columns(conn, table)
        if not cols:            # 表还不存在：schema.sql 会整张建出来，无需迁移
            continue
        if column in cols:
            continue
        sql = f"ALTER TABLE {table} ADD COLUMN {column} {decl}"
        conn.execute(sql)
        done.append(sql)
    if done:
        conn.commit()
    return done


class Conn(sqlite3.Connection):
    """带库路径的连接。

    ★ 为什么要这个：平台里有一批**后台线程**要自己开连接
      （engine 的尾随线程、simrun 的仿真线程 —— sqlite3 连接不允许跨线程用）。
      它们如果只是调 `emsdb.connect()`，拿到的是**默认库**，而请求线程可能
      连的是另一个库（`init_db(path)` / 自检的临时库）。
      结果就是"请求写 A 库、后台写 B 库"，台账永远停在 running —— 而且不报错。
      所以把库路径记在连接上，后台线程一律 `emsdb.connect(emsdb.db_path_of(conn))`。
    """

    db_path: str = ""


def db_path_of(conn: sqlite3.Connection) -> str:
    """某个连接实际连的库文件路径（拿不到时退回默认库）。"""
    return getattr(conn, "db_path", "") or DEFAULT_DB_PATH


def connect(db_path: Optional[str] = None) -> sqlite3.Connection:
    """打开数据库连接。目录不存在时自动创建。"""
    path = db_path or DEFAULT_DB_PATH
    parent = os.path.dirname(os.path.abspath(path))
    os.makedirs(parent, exist_ok=True)
    conn = sqlite3.connect(path, timeout=15.0, factory=Conn)
    conn.db_path = os.path.abspath(path)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    # 平台是「少量写 + 大量读」，WAL 让读不被导入阻塞
    conn.execute("PRAGMA journal_mode = WAL")
    conn.execute("PRAGMA synchronous = NORMAL")
    return conn


def init_db(db_path: Optional[str] = None) -> sqlite3.Connection:
    """按 schema.sql 建表，再把旧库补齐到当前版本。可重复调用。"""
    conn = connect(db_path)
    with open(SCHEMA_PATH, "r", encoding="utf-8") as f:
        conn.executescript(f.read())
    migrate(conn)               # 已存在的表：补新列（见 MIGRATIONS）
    set_meta(conn, META_SCHEMA_VERSION, SCHEMA_VERSION)
    conn.commit()
    return conn


# ---------------------------------------------------------------------
# 查询辅助
# ---------------------------------------------------------------------
def query(conn: sqlite3.Connection, sql: str,
          args: Sequence[Any] = ()) -> list[dict]:
    cur = conn.execute(sql, args)
    return [dict(r) for r in cur.fetchall()]


def query_one(conn: sqlite3.Connection, sql: str,
              args: Sequence[Any] = ()) -> Optional[dict]:
    cur = conn.execute(sql, args)
    row = cur.fetchone()
    return dict(row) if row is not None else None


def query_value(conn: sqlite3.Connection, sql: str,
                args: Sequence[Any] = (), default: Any = None) -> Any:
    row = conn.execute(sql, args).fetchone()
    if row is None or row[0] is None:
        return default
    return row[0]


def execute(conn: sqlite3.Connection, sql: str,
            args: Sequence[Any] = ()) -> int:
    cur = conn.execute(sql, args)
    return cur.lastrowid or 0


def executemany(conn: sqlite3.Connection, sql: str,
                seq: Iterable[Sequence[Any]]) -> int:
    cur = conn.executemany(sql, list(seq))
    return cur.rowcount


# ---------------------------------------------------------------------
# 元信息
# ---------------------------------------------------------------------
def set_meta(conn: sqlite3.Connection, key: str, value: str) -> None:
    conn.execute(
        "INSERT INTO meta(key, value) VALUES(?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
        (key, str(value)),
    )


def get_meta(conn: sqlite3.Connection, key: str,
             default: Optional[str] = None) -> Optional[str]:
    return query_value(conn, "SELECT value FROM meta WHERE key = ?", (key,), default)


# ---------------------------------------------------------------------
# 自检用：统计
# ---------------------------------------------------------------------
TABLES = [
    "device", "realtime_data", "strategy_config", "control_command",
    "alarm", "energy_statistics", "step_record", "scenario",
    "economics", "invariant", "app_user", "session", "audit_log", "meta",
    "run_mode",
    # v1.2 设备接入闭环
    "device_conn", "device_point_map",
    # v1.4 运行 / 仿真双模式
    "sim_run",
]


def table_counts(conn: sqlite3.Connection) -> dict[str, int]:
    out: dict[str, int] = {}
    for t in TABLES:
        try:
            out[t] = int(query_value(conn, f"SELECT COUNT(*) FROM {t}", (), 0))
        except sqlite3.Error:
            out[t] = -1  # 表不存在
    return out
