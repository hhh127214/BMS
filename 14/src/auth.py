"""14/ — 用户、会话与权限

对应架构 §24 里 E（后端·用户权限）与 F（前端·登录）两岗的要求。

三个角色（与现场实际职责对齐，不是照抄通用 RBAC）：
    viewer    只读：运行人员看状态、曲线、告警、收益
    operator  可写运行参数与下发指令：值班长
    admin     全部：含用户管理与策略启停

口令用 pbkdf2_hmac(sha256) + 每用户随机盐，不用明文、不用裸 sha256。
会话是随机 token，落在 session 表里，可撤销、有期限。
所有写操作都进 audit_log —— 「换人不靠口头交接」。
"""

from __future__ import annotations

import datetime
import hashlib
import hmac
import os
import secrets
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import emsdb  # noqa: E402

ROLE_VIEWER = "viewer"
ROLE_OPERATOR = "operator"
ROLE_ADMIN = "admin"

# 角色等级：数值越大权限越高
ROLE_LEVEL = {ROLE_VIEWER: 1, ROLE_OPERATOR: 2, ROLE_ADMIN: 3}

PBKDF2_ITERATIONS = 120_000
TOKEN_TTL_HOURS = 12

# 首次初始化时建的默认账号。**现场部署必须改口令**（见 docs/README.md §5）
DEFAULT_USERS = [
    ("admin",    "admin123",    ROLE_ADMIN,    "系统管理员"),
    ("operator", "operator123", ROLE_OPERATOR, "值班长"),
    ("viewer",   "viewer123",   ROLE_VIEWER,   "运行人员"),
]


# ---------------------------------------------------------------------
# 口令
# ---------------------------------------------------------------------
def hash_password(password: str, salt: str | None = None,
                  iterations: int = PBKDF2_ITERATIONS) -> tuple[str, str, int]:
    if salt is None:
        salt = secrets.token_hex(16)
    dk = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"),
                             bytes.fromhex(salt), iterations)
    return dk.hex(), salt, iterations


def verify_password(password: str, password_hash: str, salt: str,
                    iterations: int) -> bool:
    dk = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"),
                             bytes.fromhex(salt), int(iterations))
    return hmac.compare_digest(dk.hex(), password_hash)


# ---------------------------------------------------------------------
# 用户
# ---------------------------------------------------------------------
def ensure_default_users(conn) -> int:
    """首次运行建默认账号。已存在则不动（不会覆盖改过的口令）。"""
    n = 0
    for username, pw, role, disp in DEFAULT_USERS:
        if emsdb.query_one(conn, "SELECT 1 FROM app_user WHERE username = ?",
                           (username,)):
            continue
        create_user(conn, username, pw, role, disp)
        n += 1
    return n


def create_user(conn, username: str, password: str, role: str,
                display_name: str = "") -> int:
    if role not in ROLE_LEVEL:
        raise ValueError(f"未知角色: {role}")
    if len(password) < 6:
        raise ValueError("口令至少 6 位")
    ph, salt, iters = hash_password(password)
    uid = emsdb.execute(
        conn,
        "INSERT INTO app_user(username, password_hash, salt, iterations, role,"
        " display_name, enabled, created_at) VALUES(?,?,?,?,?,?,1,?)",
        (username, ph, salt, iters, role, display_name or username, _now()),
    )
    conn.commit()
    return uid


def set_password(conn, username: str, new_password: str) -> bool:
    ph, salt, iters = hash_password(new_password)
    cur = conn.execute(
        "UPDATE app_user SET password_hash=?, salt=?, iterations=?"
        " WHERE username = ?", (ph, salt, iters, username))
    conn.commit()
    return cur.rowcount > 0


def authenticate(conn, username: str, password: str) -> dict | None:
    u = emsdb.query_one(conn, "SELECT * FROM app_user WHERE username = ?",
                        (username,))
    if not u or not u["enabled"]:
        return None
    if not verify_password(password, u["password_hash"], u["salt"],
                           u["iterations"]):
        return None
    return {"user_id": u["user_id"], "username": u["username"],
            "role": u["role"], "display_name": u["display_name"]}


def list_users(conn) -> list[dict]:
    return emsdb.query(
        conn,
        "SELECT user_id, username, role, display_name, enabled, created_at"
        " FROM app_user ORDER BY user_id")


# ---------------------------------------------------------------------
# 会话
# ---------------------------------------------------------------------
def issue_token(conn, user_id: int, ttl_hours: int = TOKEN_TTL_HOURS) -> str:
    token = secrets.token_urlsafe(24)
    now = datetime.datetime.now()
    conn.execute(
        "INSERT INTO session(token, user_id, created_at, expires_at)"
        " VALUES(?,?,?,?)",
        (token, user_id, _now(now),
         _now(now + datetime.timedelta(hours=ttl_hours))),
    )
    conn.commit()
    return token


def resolve_token(conn, token: str) -> dict | None:
    if not token:
        return None
    row = emsdb.query_one(
        conn,
        "SELECT s.token, s.expires_at, u.user_id, u.username, u.role,"
        "       u.display_name, u.enabled"
        " FROM session s JOIN app_user u ON u.user_id = s.user_id"
        " WHERE s.token = ?", (token,))
    if not row or not row["enabled"]:
        return None
    try:
        exp = datetime.datetime.strptime(row["expires_at"], "%Y-%m-%d %H:%M:%S")
    except (TypeError, ValueError):
        return None
    if exp < datetime.datetime.now():
        conn.execute("DELETE FROM session WHERE token = ?", (token,))
        conn.commit()
        return None
    return {"user_id": row["user_id"], "username": row["username"],
            "role": row["role"], "display_name": row["display_name"]}


def revoke_token(conn, token: str) -> None:
    conn.execute("DELETE FROM session WHERE token = ?", (token,))
    conn.commit()


def purge_expired(conn) -> int:
    cur = conn.execute("DELETE FROM session WHERE expires_at < ?", (_now(),))
    conn.commit()
    return cur.rowcount


# ---------------------------------------------------------------------
# 权限判定
# ---------------------------------------------------------------------
def has_role(user: dict | None, min_role: str) -> bool:
    if not user:
        return False
    return ROLE_LEVEL.get(user.get("role", ""), 0) >= ROLE_LEVEL.get(min_role, 99)


def require(user: dict | None, min_role: str) -> tuple[bool, str]:
    """返回 (是否放行, 拒绝原因)。拒绝原因直接给前端展示。"""
    if not user:
        return False, "未登录或会话已过期"
    if not has_role(user, min_role):
        return False, f"权限不足：需要 {min_role} 及以上，当前 {user.get('role')}"
    return True, ""


# ---------------------------------------------------------------------
# 审计
# ---------------------------------------------------------------------
def audit(conn, username: str, action: str, target: str = "",
          detail: str = "") -> None:
    conn.execute(
        "INSERT INTO audit_log(ts, username, action, target, detail)"
        " VALUES(?,?,?,?,?)",
        (_now(), username or "-", action, target, detail))
    conn.commit()


def list_audit(conn, limit: int = 100) -> list[dict]:
    return emsdb.query(
        conn, "SELECT * FROM audit_log ORDER BY id DESC LIMIT ?", (int(limit),))


def _now(dt: datetime.datetime | None = None) -> str:
    return (dt or datetime.datetime.now()).strftime("%Y-%m-%d %H:%M:%S")
