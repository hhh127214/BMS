"""14/ — REST API

路由表在文件底部 ROUTES。所有接口返回 JSON。
只读接口最低角色 viewer，写接口 operator，用户管理 admin。

设计口径：
  * 平台**不参与实时闭环**。它读数据库、展示、审计；
    控制指令接口在无真实执行器时只做「记录 + 审计」，绝不假装下发成功。
  * 「实时」= 仿真时标 t_s。数据库里 8640 拍可以按任意速度回放，
    所以前端可以在 1 秒内看完 24 小时，也可以逐拍跟踪。
"""

from __future__ import annotations

import csv
import datetime
import io
import json
import os
import re
import sys
from typing import Any, Callable, Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import auth  # noqa: E402
import connconf  # noqa: E402
import emsdb  # noqa: E402
import engine  # noqa: E402
import pointmap  # noqa: E402
import simrun  # noqa: E402

# 默认数据场景。只在"引擎模式也拿不出数据集"时兜底（见 scenario_of）。
DEFAULT_SCENARIO = "normal"
# 仿真总时长（秒）：24 h。用于曲线与回放的时标上界（仅单日场景适用）。
SIM_DURATION_S = 86400.0


class ApiError(Exception):
    def __init__(self, message: str, status: int = 400):
        super().__init__(message)
        self.message = message
        self.status = status


# ---------------------------------------------------------------------
# 参数辅助
# ---------------------------------------------------------------------
def q1(query: dict, key: str, default: Any = None) -> Any:
    v = query.get(key)
    if isinstance(v, list):
        return v[0] if v else default
    return v if v is not None else default


def qf(query: dict, key: str, default: float) -> float:
    try:
        return float(q1(query, key, default))
    except (TypeError, ValueError):
        return default


def qi(query: dict, key: str, default: int) -> int:
    try:
        return int(float(q1(query, key, default)))
    except (TypeError, ValueError):
        return default


def scenario_of(query: dict) -> str:
    s = str(q1(query, "scenario", DEFAULT_SCENARIO) or DEFAULT_SCENARIO).strip()
    # live 场景（07/ --record 实时入库）的 scenario_id 不是 normal/fault 白名单里的，
    # 不能在这里硬拦 —— 否则界面上选到 live 场景时，所有时序接口都静默退回 normal。
    # 合法性：只允许 [A-Za-z0-9_-]，且长度受限。所有下游查询都是参数化
    # （scenario_id = ?），这个正则只是挡掉明显异常输入。
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", s):
        return DEFAULT_SCENARIO
    return s


def resolve_scenario(conn, query: dict) -> str:
    """本次请求该读哪个数据集。

    与 scenario_of 的分工：
      * 前端**显式**带了 ?scenario=xxx  → 就用 xxx（scenario_of 做白名单校验）；
      * 前端**没带**                     → 交给引擎模式决定（engine.default_scenario）。

    ★ 为什么不能让"没带"默认成 'normal'：
      双模式下的默认数据集是**跟着引擎模式走**的 —— 运行模式看 live，
      仿真模式看最近一次成功仿真。硬编码 normal 会让运行模式打开就是
      "内置演示数据"，看起来在跑，其实一动不动。
    """
    if q1(query, "scenario") not in (None, ""):
        return scenario_of(query)
    return engine.default_scenario(conn)


def clamp(v: float, lo: float, hi: float) -> float:
    return lo if v < lo else (hi if v > hi else v)


def r3(v: Any, nd: int = 3) -> Optional[float]:
    """四舍五入用于展示。None 原样返回（不要在 API 层把 NULL 变成 0）。"""
    if v is None:
        return None
    try:
        return round(float(v), nd)
    except (TypeError, ValueError):
        return None


# ---------------------------------------------------------------------
# 系统
# ---------------------------------------------------------------------
def h_health(conn, query, body, user, params) -> tuple[int, dict]:
    counts = emsdb.table_counts(conn)
    scenes = emsdb.query(conn, "SELECT scenario_id, name, steps, log_rows,"
                               " ok, invariants_all_ok, imported_at FROM scenario")
    return 200, {
        "ok": True,
        "service": "ems-platform-api",
        "module": "14/",
        "schema_version": emsdb.get_meta(conn, emsdb.META_SCHEMA_VERSION),
        "imported_at": emsdb.get_meta(conn, emsdb.META_IMPORT_AT),
        "import_source": emsdb.get_meta(conn, emsdb.META_IMPORT_SOURCE),
        "db": os.path.basename(emsdb.DEFAULT_DB_PATH),
        "scenarios": scenes,
        "tables": counts,
    }


def h_login(conn, query, body, user, params) -> tuple[int, dict]:
    username = str(body.get("username", "")).strip()
    password = str(body.get("password", ""))
    if not username or not password:
        raise ApiError("请填写用户名与口令", 400)
    u = auth.authenticate(conn, username, password)
    if not u:
        auth.audit(conn, username, "login_failed", "session", "口令或账号错误")
        raise ApiError("用户名或口令错误", 401)
    token = auth.issue_token(conn, u["user_id"])
    auth.audit(conn, username, "login", "session", f"角色 {u['role']}")
    return 200, {"token": token, "user": u,
                 "expires_in_h": auth.TOKEN_TTL_HOURS}


def h_logout(conn, query, body, user, params) -> tuple[int, dict]:
    tok = str(body.get("token", "") or q1(query, "token", ""))
    if tok:
        auth.revoke_token(conn, tok)
    if user:
        auth.audit(conn, user["username"], "logout", "session", "")
    return 200, {"ok": True}


def h_me(conn, query, body, user, params) -> tuple[int, dict]:
    return 200, {"user": user, "role_level": auth.ROLE_LEVEL.get(
        (user or {}).get("role", ""), 0)}


# ---------------------------------------------------------------------
# 首页总览
# ---------------------------------------------------------------------
def h_overview(conn, query, body, user, params) -> tuple[int, dict]:
    sc = resolve_scenario(conn, query)
    # 取最后一拍作为「当前」；t 参数可指定任意时刻（回放用）
    t = qf(query, "t", -1.0)
    if t < 0:
        row = emsdb.query_one(
            conn, "SELECT * FROM step_record WHERE scenario_id = ?"
                  " ORDER BY t_s DESC LIMIT 1", (sc,))
    else:
        row = emsdb.query_one(
            conn, "SELECT * FROM step_record WHERE scenario_id = ?"
                  " AND t_s <= ? ORDER BY t_s DESC LIMIT 1", (sc, t))
    if not row:
        # 运行模式下的"没有数据"几乎总是「引擎还没启动」，而不是"导入出错"——
        # 所以给一条能照着做的提示，而不是通用的"请先导入"。
        if sc == engine.LIVE_SCENARIO:
            raise ApiError(
                "运行模式还没有实时数据：实时引擎未启动（或刚启动、还没到第一个写点）。"
                "请在页面顶部的「实时引擎」状态条点「启动引擎」。", 404)
        raise ApiError(f"场景 {sc} 没有时序数据，请先导入", 404)

    econ = emsdb.query_one(conn, "SELECT * FROM economics WHERE scenario_id = ?",
                           (sc,)) or {}
    inv = emsdb.query_one(conn, "SELECT * FROM invariant WHERE scenario_id = ?",
                          (sc,)) or {}
    scn = emsdb.query_one(conn, "SELECT * FROM scenario WHERE scenario_id = ?",
                          (sc,)) or {}
    dev = emsdb.query_one(conn, "SELECT * FROM device WHERE device_id = 'DEV-BMS-01'")
    tr = emsdb.query_one(conn, "SELECT * FROM device WHERE device_id = 'DEV-TR-01'")

    # 告警分档计数
    alarm_by_level = {r["level"]: r["n"] for r in emsdb.query(
        conn, "SELECT level, COUNT(*) AS n FROM alarm WHERE scenario_id = ?"
              " GROUP BY level", (sc,))}

    return 200, {
        "scenario": sc,
        "scenario_name": scn.get("name"),
        "t_s": r3(row["t_s"], 1),
        "time": row["time_str"],
        "state": row["state"],
        # 四路功率（约定：储能放电为正）
        "power": {
            "grid_kw": r3(row["p_grid_kw"]),
            "pv_kw": r3(row["p_pv_kw"]),
            "load_kw": r3(row["p_load_kw"]),
            "battery_kw": r3(row["p_actual_kw"]),
            "command_kw": r3(row["p_cmd_kw"]),
            # 关口（电网接入点）限值 = 并网变压器额定容量，用于「关口功率」卡判越限
            "grid_limit_kw": r3(tr.get("rated_power_kw")) if tr else None,
            "permission": {"lower_kw": r3(row["p_lower_kw"]),
                           "upper_kw": r3(row["p_upper_kw"])},
        },
        "battery": {
            "soc": r3(row["soc"], 4),
            "temperature_c": r3(row["temp_c"]),
            "capacity_kwh": r3(dev.get("capacity_kwh") if dev else None, 1),
        },
        "economics": {
            "net_benefit_cny": r3(econ.get("net_benefit_cny"), 2),
            "saving_total_cny": r3(econ.get("saving_total_cny"), 2),
            "saving_pct": r3(econ.get("saving_pct"), 2),
            "cost_total_cny": r3(econ.get("cost_total_cny"), 2),
            "cost_total_base_cny": r3(econ.get("cost_total_base_cny"), 2),
        },
        "alarms": {
            "total": scn.get("alarms_total"),
            "fault": scn.get("alarms_fault"),
            "by_level": alarm_by_level,
        },
        "invariants": inv,
        "loop": _loop_block(
            conn, sc, row,
            emsdb.query_one(
                conn, "SELECT MIN(t_s) AS t0, MAX(t_s) AS t1 FROM step_record"
                      " WHERE scenario_id = ?", (sc,)) or {}),
        # 当前激活的运行模式（业务维度，独立于数据场景 scenario）
        "mode": _mode_row(emsdb.query_one(
            conn, "SELECT * FROM run_mode WHERE active = 1") or {}),
        # 数据来源（数据维度）：本次读到的是实时引擎还是离线仿真结果
        "engine": engine.get_mode(conn),
        "time_base": _time_base(conn, sc),
    }


def _loop_block(conn, sc: str, row: dict, span: dict) -> dict:
    """回放进度。

    ★ 两套时刻口径不能混算（这是本项目最容易错的地方）：
      sim  场景 t_s 是"一天内的秒"（0..86400），进度 = t/86400 天然成立；
      wall 场景 t_s 是 Unix 墙钟秒（~1.7e9）。同一个公式会算出 20000% 的进度，
      界面上就是一条永远满格的进度条 —— 比不显示更坏。
      所以 wall 场景不给 progress，改给 elapsed_s（已累积时长）与
      days_span（已累积天数），前端按"还在增长中"的口径展示。
    """
    if _time_base(conn, sc) == "wall":
        t0, t1 = span.get("t0"), span.get("t1")
        elapsed = (float(t1) - float(t0)) if (t0 is not None and t1 is not None) \
            else 0.0
        return {
            "t_s": r3(row["t_s"], 1),
            "time_base": "wall",
            "duration_s": None,          # 实时数据没有终点
            "progress": None,
            "elapsed_s": r3(elapsed, 1),
            "days_span": r3(elapsed / 86400.0, 4),
            "clamped": bool(row["clamped"]),
            "safety_clip": bool(row["safety_clip"]),
            "state_gated": bool(row["state_gated"]),
            "hold_last": bool(row["hold_last"]),
            "fault_bits": row["fault_bits"],
            "reason": row["reason"],
        }
    return {
        "t_s": r3(row["t_s"], 1),
        "time_base": "sim",
        "duration_s": SIM_DURATION_S,
        "progress": r3((row["t_s"] or 0) / SIM_DURATION_S, 6),
        "clamped": bool(row["clamped"]),
        "safety_clip": bool(row["safety_clip"]),
        "state_gated": bool(row["state_gated"]),
        "hold_last": bool(row["hold_last"]),
        "fault_bits": row["fault_bits"],
        "reason": row["reason"],
    }


# ---------------------------------------------------------------------
# 设备
# ---------------------------------------------------------------------
def h_devices(conn, query, body, user, params) -> tuple[int, dict]:
    rows = emsdb.query(conn, "SELECT * FROM device ORDER BY device_id")
    return 200, {"count": len(rows), "items": rows}


def h_device_one(conn, query, body, user, params) -> tuple[int, dict]:
    dev_id = params["device_id"]
    d = emsdb.query_one(conn, "SELECT * FROM device WHERE device_id = ?", (dev_id,))
    if not d:
        raise ApiError(f"设备不存在: {dev_id}", 404)
    sc = resolve_scenario(conn, query)
    last = emsdb.query_one(
        conn, "SELECT ts, power_kw, soc, temperature_c, quality_ok FROM"
              " realtime_data WHERE device_id = ? AND ts = (SELECT MAX(ts)"
              " FROM realtime_data WHERE device_id = ?)", (dev_id, dev_id))
    return 200, {"device": d, "scenario": sc, "latest_realtime": last}


# ---------------------------------------------------------------------
# 实时 / 时序 / 曲线
# ---------------------------------------------------------------------
def h_realtime(conn, query, body, user, params) -> tuple[int, dict]:
    """某一时刻的全站实时快照。

    数据来源是 10/ 的离线仿真 —— 本项目尚未接真机，
    所以这里扮演的是「设备侧进程上线后应写入的样子」。
    """
    sc = resolve_scenario(conn, query)
    t = qf(query, "t", -1.0)
    if t < 0:
        row = emsdb.query_one(
            conn, "SELECT * FROM step_record WHERE scenario_id = ?"
                  " ORDER BY t_s DESC LIMIT 1", (sc,))
    else:
        row = emsdb.query_one(
            conn, "SELECT * FROM step_record WHERE scenario_id = ?"
                  " AND t_s <= ? ORDER BY t_s DESC LIMIT 1", (sc, t))
    if not row:
        raise ApiError(f"场景 {sc} 没有时序数据", 404)

    def dev(did, power=None, soc=None, temp=None, status=None):
        return {"device_id": did, "power_kw": r3(power), "soc": r3(soc, 4),
                "temperature_c": r3(temp), "status": status,
                "quality_ok": 1}

    return 200, {
        "scenario": sc, "t_s": r3(row["t_s"], 1), "time": row["time_str"],
        "state": row["state"],
        "items": [
            dev("DEV-METER-01", row["p_grid_kw"],
                status="online" if row["p_grid_kw"] is not None else "offline"),
            dev("DEV-PV-01", row["p_pv_kw"]),
            dev("DEV-LOAD-01", row["p_load_kw"]),
            dev("DEV-PCS-01", row["p_actual_kw"], status=row["state"]),
            dev("DEV-BMS-01", None, row["soc"], row["temp_c"]),
        ],
        "reason": row["reason"],
    }


def h_series(conn, query, body, user, params) -> tuple[int, dict]:
    """等间隔降采样后的曲线。

    keys: 逗号分隔的 step_record 列名。默认给曲线页用的三条。
    points: 采样点数（默认 96，与 15 min 预报粒度一致）
    """
    sc = resolve_scenario(conn, query)
    keys = str(q1(query, "keys", "p_grid_kw,soc,p_cmd_kw") or "").split(",")
    keys = [k.strip() for k in keys if k.strip()]
    allowed = set(STEP_COLUMNS) - {"scenario_id", "t_s", "time_str", "state",
                                   "reason"}
    bad = [k for k in keys if k not in allowed]
    if bad:
        raise ApiError(f"不支持的曲线字段: {','.join(bad)}", 400)
    points = max(2, min(2000, qi(query, "points", 96)))

    rows = emsdb.query(
        conn, f"SELECT t_s, time_str, {','.join(keys)} FROM step_record"
              f" WHERE scenario_id = ? ORDER BY t_s", (sc,))
    if not rows:
        raise ApiError(f"场景 {sc} 没有时序数据", 404)

    n = len(rows)
    idx = [round(i * (n - 1) / (points - 1)) for i in range(points)]
    idx = sorted(set(idx))
    sampled = [rows[i] for i in idx]

    # ★ 时刻口径分支：sim 场景 t_s 是"一天内秒"，time 转成 HH:MM；
    #   wall 场景 t_s 是 Unix 墙钟秒，直接用 time_str（已是完整日期时间）。
    #   若统一用 _hhmm，墙钟秒会被 %86400 折成"一天内的第几秒"，跨日曲线全错。
    wall = _time_base(conn, sc) == "wall"
    time_labels = [r["time_str"] for r in sampled] if wall \
        else [_hhmm(r["t_s"]) for r in sampled]

    return 200, {
        "scenario": sc, "points": len(sampled), "rows_total": n,
        "keys": keys, "time_base": "wall" if wall else "sim",
        "t_s": [r3(r["t_s"], 1) for r in sampled],
        "time": time_labels,
        "series": {k: [r3(r[k], 4) for r in sampled] for k in keys},
    }


STEP_COLUMNS = [
    "t_s", "time_str", "state", "p_load_kw", "p_pv_kw", "p_grid_kw",
    "p_cmd_kw", "p_actual_kw", "soc", "temp_c", "p_lower_kw", "p_upper_kw",
    "plan_target", "correction", "clamped", "safety_clip", "state_gated",
    "hold_last", "fault_bits", "reason",
]


def _hhmm(t_s: Any) -> str:
    try:
        v = float(t_s)
    except (TypeError, ValueError):
        return ""
    m = int(v // 60)
    return f"{(m // 60) % 24:02d}:{m % 60:02d}"


def _time_base(conn, scenario_id: str) -> str:
    """场景的时刻口径：'sim'（一天内秒）/ 'wall'（Unix 墙钟秒）。"""
    row = emsdb.query_one(
        conn, "SELECT time_base FROM scenario WHERE scenario_id = ?",
        (scenario_id,))
    return (row or {}).get("time_base") or "sim"


def h_steps(conn, query, body, user, params) -> tuple[int, dict]:
    sc = resolve_scenario(conn, query)
    t0, t1 = _time_window(conn, sc, query)
    limit = max(1, min(5000, qi(query, "limit", 200)))
    offset = max(0, qi(query, "offset", 0))
    total = emsdb.query_value(
        conn, "SELECT COUNT(*) FROM step_record WHERE scenario_id = ?"
              " AND t_s BETWEEN ? AND ?", (sc, t0, t1), 0)
    rows = emsdb.query(
        conn, "SELECT * FROM step_record WHERE scenario_id = ?"
              " AND t_s BETWEEN ? AND ? ORDER BY t_s LIMIT ? OFFSET ?",
        (sc, t0, t1, limit, offset))
    return 200, {"scenario": sc, "total": total, "limit": limit,
                 "offset": offset, "items": rows}


def _time_window(conn, sc: str, query: dict) -> tuple[float, float]:
    """时序/指令接口的默认时间窗。

    ★ wall 场景（运行模式）t_s 是 Unix 墙钟秒，用 [0, 86400] 当默认窗
      会把**所有**数据过滤掉 —— 表现是"接口 200 但 items 空"，
      比报错更难查。所以 wall 场景默认开成无穷界（全量）。
      显式传 from/to 时仍按传入值（前端画"最近 N 分钟"就是靠这个）。
    """
    if _time_base(conn, sc) == "wall":
        return qf(query, "from", -1.0e18), qf(query, "to", 1.0e18)
    return qf(query, "from", 0.0), qf(query, "to", SIM_DURATION_S)


def h_commands(conn, query, body, user, params) -> tuple[int, dict]:
    sc = resolve_scenario(conn, query)
    t0, t1 = _time_window(conn, sc, query)
    limit = max(1, min(5000, qi(query, "limit", 200)))
    offset = max(0, qi(query, "offset", 0))
    result = q1(query, "result")
    where = "scenario_id = ? AND ts BETWEEN ? AND ?"
    args: list[Any] = [sc, t0, t1]
    if result:
        where += " AND result = ?"
        args.append(result)
    total = emsdb.query_value(conn, f"SELECT COUNT(*) FROM control_command"
                                    f" WHERE {where}", args, 0)
    by_result = {r["result"]: r["n"] for r in emsdb.query(
        conn, f"SELECT result, COUNT(*) AS n FROM control_command"
              f" WHERE scenario_id = ? GROUP BY result", (sc,))}
    rows = emsdb.query(
        conn, f"SELECT * FROM control_command WHERE {where}"
              f" ORDER BY ts LIMIT ? OFFSET ?", args + [limit, offset])
    return 200, {"scenario": sc, "total": total, "by_result": by_result,
                 "limit": limit, "offset": offset, "items": rows}


# ---------------------------------------------------------------------
# 告警
# ---------------------------------------------------------------------
def h_alarms(conn, query, body, user, params) -> tuple[int, dict]:
    sc = resolve_scenario(conn, query)
    level = q1(query, "level")
    limit = max(1, min(2000, qi(query, "limit", 100)))
    offset = max(0, qi(query, "offset", 0))
    where, args = "scenario_id = ?", [sc]
    if level:
        where += " AND level = ?"
        args.append(level)
    total = emsdb.query_value(conn, f"SELECT COUNT(*) FROM alarm WHERE {where}",
                              args, 0)
    rows = emsdb.query(
        conn, f"SELECT * FROM alarm WHERE {where} ORDER BY ts LIMIT ? OFFSET ?",
        args + [limit, offset])
    return 200, {"scenario": sc, "total": total, "limit": limit,
                 "offset": offset, "items": rows}


def h_alarm_summary(conn, query, body, user, params) -> tuple[int, dict]:
    sc = resolve_scenario(conn, query)
    by_level = emsdb.query(
        conn, "SELECT level, COUNT(*) AS n FROM alarm WHERE scenario_id = ?"
              " GROUP BY level ORDER BY n DESC", (sc,))
    by_source = emsdb.query(
        conn, "SELECT source, COUNT(*) AS n FROM alarm WHERE scenario_id = ?"
              " GROUP BY source ORDER BY n DESC LIMIT 20", (sc,))
    timeline = emsdb.query(
        conn, "SELECT CAST(ts/3600 AS INTEGER) AS hour, COUNT(*) AS n"
              " FROM alarm WHERE scenario_id = ? GROUP BY hour ORDER BY hour",
        (sc,))
    return 200, {"scenario": sc, "by_level": by_level,
                 "by_source": by_source, "timeline": timeline}


# ---------------------------------------------------------------------
# 策略配置
# ---------------------------------------------------------------------
def h_strategies(conn, query, body, user, params) -> tuple[int, dict]:
    rows = emsdb.query(conn, "SELECT * FROM strategy_config"
                             " ORDER BY priority, strategy_id")
    for r in rows:
        try:
            r["parameters"] = json.loads(r["parameters"] or "{}")
        except (TypeError, ValueError):
            r["parameters"] = {}
    return 200, {"count": len(rows), "items": rows}


def h_strategy_update(conn, query, body, user, params) -> tuple[int, dict]:
    sid = params["strategy_id"]
    cur = emsdb.query_one(conn, "SELECT * FROM strategy_config"
                                " WHERE strategy_id = ?", (sid,))
    if not cur:
        raise ApiError(f"策略不存在: {sid}", 404)

    sets, args = [], []
    if "enabled" in body:
        sets.append("enabled = ?")
        args.append(1 if body["enabled"] else 0)
    if "priority" in body:
        prio = str(body["priority"])
        if prio not in ("L0", "L1", "L2", "L3"):
            raise ApiError("priority 只能是 L0/L1/L2/L3", 400)
        sets.append("priority = ?")
        args.append(prio)
    if "parameters" in body:
        if not isinstance(body["parameters"], dict):
            raise ApiError("parameters 必须是对象", 400)
        sets.append("parameters = ?")
        args.append(json.dumps(body["parameters"], ensure_ascii=False))
    if "description" in body:
        sets.append("description = ?")
        args.append(str(body["description"]))
    if not sets:
        raise ApiError("没有可更新的字段", 400)

    sets.append("updated_at = ?")
    args.append(datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
    args.append(sid)
    conn.execute(f"UPDATE strategy_config SET {','.join(sets)}"
                 f" WHERE strategy_id = ?", args)
    conn.commit()
    auth.audit(conn, user["username"], "strategy_update", sid,
               json.dumps(body, ensure_ascii=False)[:500])
    return 200, {"ok": True, "strategy": emsdb.query_one(
        conn, "SELECT * FROM strategy_config WHERE strategy_id = ?", (sid,))}


# ---------------------------------------------------------------------
# 运行模式（独立顶层业务概念）
# ---------------------------------------------------------------------
def _mode_row(row: dict) -> dict:
    """把 run_mode 行转成 API 友好结构（JSON 列反序列化）。"""
    out = dict(row)
    for key in ("constraints", "bound_strategies"):
        try:
            out[key] = json.loads(out.get(key) or "[]")
        except (TypeError, ValueError):
            out[key] = []
    out["applicable_scenes"] = [
        s.strip() for s in (out.get("applicable_scene") or "normal").split(",") if s.strip()
    ]
    out.pop("applicable_scene", None)
    return out


def h_modes(conn, query, body, user, params) -> tuple[int, dict]:
    """列出全部运行模式 + 当前激活模式。"""
    rows = emsdb.query(conn, "SELECT * FROM run_mode ORDER BY is_default DESC, mode_id")
    modes = [_mode_row(r) for r in rows]
    active = next((m for m in modes if m.get("active")), None)
    return 200, {"count": len(modes), "items": modes,
                 "active": (active or {}).get("mode_id")}


def h_mode_activate(conn, query, body, user, params) -> tuple[int, dict]:
    """切换运行模式：置 active，并联动启停绑定的策略。

    规则：
      * 安全底座策略（mode_id = NULL，即 S01/S02/S03/S06）**不受影响**，恒为启用。
      * 目标模式绑定的策略 → 启用；其余带 mode_id 的策略 → 停用。
      * 只切换「有归属」的策略，不碰 NULL 归属的安全底座，保证任何模式下安全边界都在。
    """
    mid = str(body.get("mode_id", "")).strip()
    cur = emsdb.query_one(conn, "SELECT * FROM run_mode WHERE mode_id = ?", (mid,))
    if not cur:
        raise ApiError(f"运行模式不存在: {mid}", 404)

    conn.execute("UPDATE run_mode SET active = 0")
    conn.execute("UPDATE run_mode SET active = 1 WHERE mode_id = ?", (mid,))

    try:
        bound = json.loads(cur["bound_strategies"] or "[]")
    except (TypeError, ValueError):
        bound = []
    bound = [str(s) for s in bound]

    # 先停掉所有「有归属」的策略（mode_id 非 NULL），再启用目标模式的绑定策略
    conn.execute("UPDATE strategy_config SET enabled = 0 WHERE mode_id IS NOT NULL")
    if bound:
        marks = ",".join("?" for _ in bound)
        conn.execute(f"UPDATE strategy_config SET enabled = 1"
                     f" WHERE strategy_id IN ({marks})", bound)
    conn.commit()

    auth.audit(conn, user["username"], "mode_activate", mid,
               f"联动启停策略 {bound or '（无）'}")
    modes = [_mode_row(r) for r in emsdb.query(
        conn, "SELECT * FROM run_mode ORDER BY is_default DESC, mode_id")]
    return 200, {"ok": True, "active": mid, "items": modes,
                 "enabled_strategies": [r["strategy_id"] for r in emsdb.query(
                     conn, "SELECT strategy_id FROM strategy_config"
                           " WHERE enabled = 1 ORDER BY priority")]}


# ---------------------------------------------------------------------
# 收益 / 能量 / 报表
# ---------------------------------------------------------------------
def h_economics(conn, query, body, user, params) -> tuple[int, dict]:
    rows = emsdb.query(conn, "SELECT * FROM economics")
    return 200, {"items": rows}


def h_energy(conn, query, body, user, params) -> tuple[int, dict]:
    rows = emsdb.query(conn, "SELECT * FROM energy_statistics"
                             " ORDER BY date DESC, scenario_id")
    return 200, {"items": rows}


def h_report(conn, query, body, user, params) -> tuple[int, dict]:
    """报表系统：日/周/月聚合。

    本项目目前只有仿真日数据，所以 period 只是改变聚合视角，
    不改变数据本身 —— 这一点在返回里显式标出，避免误读。
    """
    period = str(q1(query, "period", "day") or "day")
    if period not in ("day", "week", "month"):
        raise ApiError("period 只能是 day / week / month", 400)
    sc = resolve_scenario(conn, query)

    daily = emsdb.query(
        conn, "SELECT date, scenario_id, charge_kwh, discharge_kwh, pv_kwh,"
              " load_kwh, grid_kwh, export_kwh, peak_grid_kw, net_benefit_cny"
              " FROM energy_statistics WHERE scenario_id = ? ORDER BY date",
        (sc,))
    agg = emsdb.query_one(
        conn, "SELECT COUNT(*) AS days, SUM(charge_kwh) AS charge_kwh,"
              " SUM(discharge_kwh) AS discharge_kwh, SUM(pv_kwh) AS pv_kwh,"
              " SUM(load_kwh) AS load_kwh, SUM(export_kwh) AS export_kwh,"
              " MAX(peak_grid_kw) AS peak_grid_kw,"
              " SUM(net_benefit_cny) AS net_benefit_cny"
              " FROM energy_statistics WHERE scenario_id = ?", (sc,)) or {}
    econ = emsdb.query_one(conn, "SELECT * FROM economics WHERE scenario_id = ?",
                           (sc,)) or {}
    inv = emsdb.query_one(conn, "SELECT * FROM invariant WHERE scenario_id = ?",
                          (sc,)) or {}
    alarm_by_level = emsdb.query(
        conn, "SELECT level, COUNT(*) AS n FROM alarm WHERE scenario_id = ?"
              " GROUP BY level", (sc,))
    cmd_by_result = emsdb.query(
        conn, "SELECT result, COUNT(*) AS n FROM control_command"
              " WHERE scenario_id = ? GROUP BY result", (sc,))

    return 200, {
        "period": period, "scenario": sc, "days": agg.get("days") or 0,
        "data_note": "当前库内只有仿真日数据，周/月视角尚未积累真实运行日",
        "daily": daily, "aggregate": agg,
        "economics": econ, "invariants": inv,
        "alarms_by_level": alarm_by_level,
        "commands_by_result": cmd_by_result,
    }


def h_invariants(conn, query, body, user, params) -> tuple[int, dict]:
    rows = emsdb.query(conn, "SELECT * FROM invariant")
    return 200, {"items": rows,
                 "note": "全 0 才算过；all_ok=1 表示硬不变量与安全口径同时成立"}


# ---------------------------------------------------------------------
# 指令下发（模拟执行）
# ---------------------------------------------------------------------
def h_command_send(conn, query, body, user, params) -> tuple[int, dict]:
    """下发指令。

    现场没有真实执行器时，平台**不假装成功**：
    只把意图记进审计，并返回 executor 状态。
    """
    target = r3(body.get("target_power_kw"))
    if target is None:
        raise ApiError("缺少 target_power_kw", 400)
    device_id = str(body.get("device_id", "DEV-PCS-01"))
    sc = str(body.get("scenario", DEFAULT_SCENARIO))
    t = qf(body, "t_s", -1.0)

    # 与当前权限区间对照，给出「这台设备能不能动」的判断
    row = None
    if t >= 0:
        row = emsdb.query_one(
            conn, "SELECT * FROM step_record WHERE scenario_id = ? AND t_s <= ?"
                  " ORDER BY t_s DESC LIMIT 1", (sc, t))
    lo = r3(row["p_lower_kw"]) if row else None
    hi = r3(row["p_upper_kw"]) if row else None
    verdict = "unknown"
    if lo is not None and hi is not None:
        verdict = "ok" if lo <= target <= hi else "out_of_permission"
    if row and row["state_gated"]:
        verdict = "gated"

    auth.audit(conn, user["username"], "command_send", device_id,
               f"target={target} kW, verdict={verdict} (边界 {lo}..{hi})")
    return 200, {
        "accepted": False,
        "executor": "none",
        "reason": "现场尚未接入真实执行器（无真机）；本次只记录意图并审计",
        "target_power_kw": target,
        "device_id": device_id,
        "permission_range": {"lower_kw": lo, "upper_kw": hi},
        "verdict": verdict,
    }


# ---------------------------------------------------------------------
# 审计 / 用户
# ---------------------------------------------------------------------
def h_audit(conn, query, body, user, params) -> tuple[int, dict]:
    limit = max(1, min(1000, qi(query, "limit", 100)))
    return 200, {"items": auth.list_audit(conn, limit)}


def h_users(conn, query, body, user, params) -> tuple[int, dict]:
    return 200, {"items": auth.list_users(conn)}


def h_user_create(conn, query, body, user, params) -> tuple[int, dict]:
    try:
        uid = auth.create_user(conn, str(body.get("username", "")).strip(),
                               str(body.get("password", "")),
                               str(body.get("role", auth.ROLE_VIEWER)),
                               str(body.get("display_name", "")))
    except ValueError as e:
        raise ApiError(str(e), 400)
    except Exception as e:  # UNIQUE 冲突等
        raise ApiError(f"创建失败: {e}", 400)
    auth.audit(conn, user["username"], "user_create",
               body.get("username", ""), f"role={body.get('role')}")
    return 200, {"ok": True, "user_id": uid}


# ---------------------------------------------------------------------
# 导出
# ---------------------------------------------------------------------
def h_export(conn, query, body, user, params) -> tuple[int, dict]:
    """导出为 CSV 文本（由 server 层写响应体，这里返回特殊标记）。"""
    kind = params["kind"]
    sc = resolve_scenario(conn, query)
    mapping = {
        "timeseries": ("SELECT * FROM step_record WHERE scenario_id = ?"
                       " ORDER BY t_s", "timeseries"),
        "alarms": ("SELECT * FROM alarm WHERE scenario_id = ? ORDER BY ts",
                   "alarms"),
        "commands": ("SELECT * FROM control_command WHERE scenario_id = ?"
                     " ORDER BY ts", "commands"),
        "devices": ("SELECT * FROM device ORDER BY device_id", "devices"),
    }
    if kind not in mapping:
        raise ApiError(f"不支持的导出类型: {kind}，可选 "
                       f"{','.join(mapping)}", 400)
    sql, _ = mapping[kind]
    args = () if kind == "devices" else (sc,)
    rows = emsdb.query(conn, sql, args)
    buf = io.StringIO()
    if rows:
        w = csv.DictWriter(buf, fieldnames=list(rows[0].keys()),
                           lineterminator="\n")
        w.writeheader()
        w.writerows(rows)
    auth.audit(conn, user["username"], "export", kind, f"{len(rows)} 行")
    return 200, {"__raw__": True, "content_type": "text/csv; charset=utf-8",
                 "body": buf.getvalue(),
                 "filename": f"{kind}_{sc}.csv"}


# =====================================================================
# 设备接入（v1.2）—— 配置通道①：约定文件路径
#
# 这一段把「客户在界面上填点表」接到「13/ 现场进程能读到」：
#     15/ 表单 ──PUT──▶ 14/ 写 device_conn + device_point_map
#                       └─▶ 原子写 config/point-map/<id>.csv 与 active.csv
#                                    └─▶ 13/ 启动时读 active.csv
#
# 两条硬纪律：
#   1. **校验在服务端**。前端能绕过，服务端不能 —— 而一张错的点表在运行期
#      表现得像个正常数字，是最难查的一类故障。
#   2. **落盘与写库同生共死**。半成功会产生「界面说保存成功、现场还是旧表」，
#      这种不一致比直接报错难查得多，所以宁可整体回滚。
# =====================================================================

# Modbus 串行链路从站地址 1..247（0 是广播地址，不能作为正常从站）
UNIT_ID_MIN, UNIT_ID_MAX = 1, 247
PORT_MIN, PORT_MAX = 1, 65535
POLL_MIN_MS = 10
# 主机名 / IPv4 的白名单。刻意不用"校验 IPv4"的强规则：现场会写设备主机名。
_RE_HOST = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._-]{0,62}$")


def _device_or_404(conn, device_id: str) -> dict:
    d = emsdb.query_one(conn, "SELECT * FROM device WHERE device_id = ?", (device_id,))
    if not d:
        raise ApiError(f"设备不存在: {device_id}", 404)
    return d


def _conn_row(conn, device_id: str) -> dict:
    """取接入参数；没有就返回一份默认值（不写库）。"""
    r = emsdb.query_one(conn, "SELECT * FROM device_conn WHERE device_id = ?", (device_id,))
    if r:
        return r
    return {
        "device_id": device_id, "protocol": "modbus_tcp",
        "host": None, "port": 502, "unit_id": 1,
        "poll_period_ms": 100, "timeout_ms": 1000,
        "auto_reconnect": 1, "enabled": 0,
        "point_map_path": None, "point_map_rows": 0,
        "point_map_saved_at": None, "updated_at": None, "note": None,
    }


def _pm_rows(conn, device_id: str) -> tuple[list[dict], str]:
    """取点表行。库里没有则返回内置默认表基线，并标明来源。

    返回 (rows, source)。source ∈ {"db", "baseline"}。
    基线是只读的默认值 —— GET 不写库（读接口有副作用是最坏的接口设计）。
    """
    db_rows = emsdb.query(conn, "SELECT * FROM device_point_map"
                                " WHERE device_id = ? ORDER BY idx", (device_id,))
    if db_rows:
        return pointmap.from_db_rows(db_rows), "db"
    return pointmap.normalize(pointmap.read_baseline()), "baseline"


def _pm_summary(rows: list[dict]) -> dict:
    return pointmap.summary(rows)


# ---------------------------------------------------------------------
# 接入参数落盘（配置通道②）—— 文件与库同生共死
#
# 为什么需要：点表有 active.csv 给端侧读，接入参数此前只在库里 ——
# 于是 13/ 07/ 仍必须靠命令行 --host 才连得上，界面上写的 IP 与现场敲的
# 可能根本不是一个，而两边都不报错。
#
# 两条纪律与点表保存完全一致：
#   1. 落盘与写库同生共死（半成功 = 「界面说配好了、端侧连的还是老地址」）；
#   2. host 被清空时**删掉旧文件**，不让端侧拿一个已作废的地址去连。
# ---------------------------------------------------------------------
def _file_snapshot(paths) -> dict:
    return {p: (open(p, "rb").read() if os.path.isfile(p) else None) for p in paths}


def _file_restore(prev: dict) -> None:
    for p, data in prev.items():
        try:
            if data is None:
                if os.path.isfile(p):
                    os.unlink(p)
            else:
                pointmap._atomic_write(p, data)
        except OSError:
            pass


def _sync_conn_file(device_id: str, c: dict, now: str, username: str) -> dict:
    """把当前接入参数落盘给端侧。返回落盘结果（供接口回显）。

    host 为空时删除旧文件：客户把地址清掉就是「这台先不接」，
    留着一个旧地址会让端侧下一拍去连一台已经换掉的设备。
    """
    path = connconf.conn_path()
    host = str(c.get("host") or "").strip()
    if not host:
        if os.path.isfile(path):
            os.unlink(path)
        return {"path": path, "bytes": 0, "removed": True}

    row = dict(c)
    row.update({"device_id": device_id, "saved_at": now, "username": username})
    saved = connconf.save(row)
    saved["removed"] = False
    return saved


def h_point_map(conn, query, body, user, params) -> tuple[int, dict]:
    """读某设备的点表（含预检统计与落盘状态）。"""
    device_id = params["device_id"]
    _device_or_404(conn, device_id)
    rows, source = _pm_rows(conn, device_id)
    c = _conn_row(conn, device_id)

    target = pointmap.device_path(device_id)
    act = pointmap.active_path()
    synced = False
    if source == "db" and os.path.isfile(act):
        try:
            disk = pointmap.normalize(pointmap.load_file(act))
            synced = disk == rows
        except pointmap.PointMapError:
            synced = False

    # 接入参数文件（配置通道②）的落盘状态：界面据此显示「已下发到端侧」
    cpath = connconf.conn_path()
    cfile_exists = os.path.isfile(cpath)
    conn_synced = False
    if cfile_exists:
        try:
            on_disk = connconf.load_file(cpath)
            conn_synced = (
                on_disk["host"] == (c.get("host") or "")
                and on_disk["port"] == (c.get("port") or 502)
                and on_disk["unit_id"] == (c.get("unit_id") or 1)
            )
        except connconf.ConnConfError:
            conn_synced = False

    return 200, {
        "device_id": device_id,
        "configured": source == "db",
        "source": source,
        "columns": pointmap.COLS,
        "rows": rows,
        "summary": _pm_summary(rows),
        "conn": c,
        "path": target,
        "active_path": act,
        "file_exists": os.path.isfile(target),
        "synced": synced,
        "conn_path": cpath,
        "conn_file_exists": cfile_exists,
        "conn_synced": conn_synced,
    }


def h_point_map_put(conn, query, body, user, params) -> tuple[int, dict]:
    """保存点表：先校验 → 落盘 → 写库。任何一步失败都整体回滚。"""
    device_id = params["device_id"]
    _device_or_404(conn, device_id)

    raw_rows = body.get("rows")
    if raw_rows is None and isinstance(body.get("csv"), str):
        raw_rows = pointmap.parse_text(body["csv"])
    if not isinstance(raw_rows, list) or not raw_rows:
        raise ApiError("请求体需要 rows（行数组）或 csv（CSV 文本）", 400)

    try:
        rows = pointmap.normalize(raw_rows)
        pointmap.validate(rows)              # ← 与 13/ 的 validate_map() 同规则
    except pointmap.PointMapError as e:
        raise ApiError(e.message, e.status)

    c = _conn_row(conn, device_id)
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    meta = {
        "device_id": device_id,
        "host": c.get("host") or "-", "port": c.get("port") or "-",
        "unit_id": c.get("unit_id") or "-",
        "saved_at": now, "username": user["username"] if user else "-",
    }

    # 先把旧文件读进内存：出任何问题都能原样退回去
    target = pointmap.device_path(device_id)
    act = pointmap.active_path()
    prev = _file_snapshot((target, act, connconf.conn_path()))

    saved = None
    cp = None
    try:
        saved = pointmap.save(device_id, rows, meta)

        conn.execute("DELETE FROM device_point_map WHERE device_id = ?", (device_id,))
        emsdb.executemany(conn, pointmap.DB_INSERT_SQL,
                          pointmap.to_db_rows(device_id, rows))
        conn.execute(
            "INSERT INTO device_conn(device_id, protocol, host, port, unit_id,"
            " poll_period_ms, timeout_ms, auto_reconnect, enabled, point_map_path,"
            " point_map_rows, point_map_saved_at, updated_at)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)"
            " ON CONFLICT(device_id) DO UPDATE SET"
            "   point_map_path = excluded.point_map_path,"
            "   point_map_rows = excluded.point_map_rows,"
            "   point_map_saved_at = excluded.point_map_saved_at,"
            "   updated_at = excluded.updated_at",
            (device_id, c.get("protocol") or "modbus_tcp",
             c.get("host"), c.get("port") or 502, c.get("unit_id") or 1,
             c.get("poll_period_ms") or 100, c.get("timeout_ms") or 1000,
             int(c.get("auto_reconnect") or 0), int(c.get("enabled") or 0),
             saved["path"], len(rows), now, now))
        # 接入参数也一并落盘（配置通道②）：点表与接入参数必须来自同一次保存，
        # 否则会出现「新 IP 连旧点表」这种两侧不一致。
        cp = _sync_conn_file(device_id, c, now, user["username"] if user else "-")
        conn.commit()
    except Exception as e:
        conn.rollback()
        _file_restore(prev)                   # 文件也退回去，保持两侧一致
        if isinstance(e, ApiError):
            raise
        raise ApiError(f"保存失败，已整体回滚：{e}", 500)

    auth.audit(conn, user["username"], "point_map_save", device_id,
               f"{len(rows)} 行 → {saved['path']}")
    return 200, {"ok": True, "saved": saved, "conn_file": cp,
                 "summary": _pm_summary(rows),
                 "rows": len(rows)}


def h_point_map_csv(conn, query, body, user, params) -> tuple[int, dict]:
    """导出点表 CSV（当前表，或 ?template=1 取内置默认表当模板）。"""
    device_id = params["device_id"]
    _device_or_404(conn, device_id)

    if str(q1(query, "template", "") or "") in ("1", "true", "yes"):
        rows = pointmap.normalize(pointmap.read_baseline())
        hint = "内置默认表（模板）"
    else:
        rows, src = _pm_rows(conn, device_id)
        hint = "库内已配置点表" if src == "db" else "内置默认表（该设备尚未配置）"

    c = _conn_row(conn, device_id)
    meta = {
        "device_id": device_id,
        "host": c.get("host") or "-", "port": c.get("port") or "-",
        "unit_id": c.get("unit_id") or "-",
        "saved_at": datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "username": (user["username"] if user else "-") + f"（导出：{hint}）",
    }
    text = pointmap.to_csv_text(rows, meta)
    auth.audit(conn, user["username"], "point_map_export", device_id, hint)
    return 200, {
        "__raw__": True,
        "content_type": "text/csv; charset=utf-8",
        # ★ 带 BOM：现场用 Excel 双击打开，中文才不乱码
        "body": "\ufeff" + text,
        "filename": f"point-map_{device_id}.csv",
    }


def h_conn_put(conn, query, body, user, params) -> tuple[int, dict]:
    """保存接入参数（IP / 端口 / 从站号 / 轮询节奏）。"""
    device_id = params["device_id"]
    _device_or_404(conn, device_id)

    def _int(key, lo, hi, cur, label):
        if key not in body:
            return cur
        try:
            v = int(body[key])
        except (TypeError, ValueError):
            raise ApiError(f"{label} 应为整数，实际是 {body[key]!r}", 400)
        if not (lo <= v <= hi):
            raise ApiError(f"{label} 应在 {lo}..{hi}，实际是 {v}", 400)
        return v

    cur = _conn_row(conn, device_id)
    host = cur.get("host")
    if "host" in body:
        h = str(body["host"] or "").strip()
        if h and not _RE_HOST.match(h):
            raise ApiError(
                "设备地址应是 IPv4 或主机名（只允许字母数字与 . _ -），实际是 "
                f"「{h}」", 400)
        host = h or None

    port = _int("port", PORT_MIN, PORT_MAX, cur.get("port") or 502, "端口")
    unit = _int("unit_id", UNIT_ID_MIN, UNIT_ID_MAX, cur.get("unit_id") or 1, "从站号")
    poll = _int("poll_period_ms", POLL_MIN_MS, 60000,
                cur.get("poll_period_ms") or 100, "轮询周期(ms)")
    tmo = _int("timeout_ms", 10, 60000, cur.get("timeout_ms") or 1000, "超时(ms)")
    en = cur.get("enabled") or 0
    if "enabled" in body:
        en = 1 if body["enabled"] else 0
    if host and "enabled" not in body:
        en = 1                              # 填了地址即视为启用（现场直觉）

    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    # 落盘也要跟库一起成败：先备好旧文件，任何一步失败都整体退回
    prev = _file_snapshot((connconf.conn_path(),))
    cp = None
    try:
        conn.execute(
            "INSERT INTO device_conn(device_id, protocol, host, port, unit_id,"
            " poll_period_ms, timeout_ms, auto_reconnect, enabled,"
            " point_map_path, point_map_rows, point_map_saved_at, updated_at, note)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
            " ON CONFLICT(device_id) DO UPDATE SET"
            "   host = excluded.host, port = excluded.port, unit_id = excluded.unit_id,"
            "   poll_period_ms = excluded.poll_period_ms, timeout_ms = excluded.timeout_ms,"
            "   enabled = excluded.enabled, updated_at = excluded.updated_at",
            (device_id, cur.get("protocol") or "modbus_tcp", host, port, unit, poll, tmo,
             int(cur.get("auto_reconnect") or 0), en,
             cur.get("point_map_path"), cur.get("point_map_rows") or 0,
             cur.get("point_map_saved_at"), now,
             str(body.get("note") or cur.get("note") or "") or None))
        # 配置通道②：接入参数落盘给 13/ 07/ 读（host 清空则删旧文件）
        cp = _sync_conn_file(
            device_id,
            dict(cur, host=host, port=port, unit_id=unit, poll_period_ms=poll,
                 timeout_ms=tmo, enabled=en),
            now, user["username"] if user else "-")
        conn.commit()
    except Exception as e:
        conn.rollback()
        _file_restore(prev)
        if isinstance(e, ApiError):
            raise
        raise ApiError(f"接入参数保存失败，已整体回滚：{e}", 500)

    auth.audit(conn, user["username"], "device_conn_save", device_id,
               f"{host}:{port} unit={unit} poll={poll}ms enabled={en}")
    return 200, {"ok": True, "conn": _conn_row(conn, device_id), "conn_file": cp}


def h_device_create(conn, query, body, user, params) -> tuple[int, dict]:
    """新增设备台账。没有它，客户在界面上就没有可配置的设备（FK 约束）。"""
    did = str(body.get("device_id") or "").strip()
    if not did or not re.fullmatch(r"[0-9A-Za-z][0-9A-Za-z_-]{0,31}", did):
        raise ApiError("设备号只允许字母数字与 _ -，且以字母数字开头（1..32 位）", 400)
    if emsdb.query_one(conn, "SELECT device_id FROM device WHERE device_id = ?", (did,)):
        raise ApiError(f"设备已存在: {did}", 409)
    dtype = str(body.get("device_type") or "").strip().upper()
    if dtype not in ("PCS", "BMS", "METER", "PV", "TRANSFORMER", "LOAD", "GATEWAY"):
        raise ApiError(
            "设备类型应为 PCS/BMS/METER/PV/TRANSFORMER/LOAD/GATEWAY", 400)
    conn.execute(
        "INSERT INTO device(device_id, device_type, manufacturer, model,"
        " rated_power_kw, capacity_kwh, status, location, note)"
        " VALUES(?,?,?,?,?,?,?,?,?)",
        (did, dtype, body.get("manufacturer"), body.get("model"),
         body.get("rated_power_kw"), body.get("capacity_kwh"),
         "offline", body.get("location"), body.get("note")))
    conn.commit()
    auth.audit(conn, user["username"], "device_create", did, dtype)
    return 201, {"ok": True, "device": emsdb.query_one(
        conn, "SELECT * FROM device WHERE device_id = ?", (did,))}


# ---------------------------------------------------------------------
# 路由表
# ---------------------------------------------------------------------
Route = tuple[str, re.Pattern, Callable, Optional[str]]

# ---------------------------------------------------------------------
# 引擎（运行 / 仿真 双模式）
# ---------------------------------------------------------------------
def h_engine(conn, query, body, user, params) -> tuple[int, dict]:
    """GET /api/engine —— 当前引擎模式 + 实时引擎状态 + 可选数据源。

    这是前端"顶栏模式切换器"的数据源，也是"现在跑的是模型不是真机"
    这句话的唯一出处（sources[] 里 device 的 available=False + why）。
    """
    st = engine.status(conn)
    st["sim"] = {"active": simrun.active(), "kinds": [
        {"id": k, **{kk: vv for kk, vv in v.items() if kk != "prefix"}}
        for k, v in simrun.KINDS.items()]}
    return 200, st


def h_engine_set(conn, query, body, user, params) -> tuple[int, dict]:
    """POST /api/engine —— 切换引擎模式（运行 / 仿真）。

    只切模式，**不**顺手启停引擎：切到运行模式不会偷偷拉起实时引擎，
    切到仿真模式也不会把正在跑的实时引擎停掉。理由是可预测性 ——
    一个"切换"附带启动进程，用户没法预判自己的点击到底做了什么。
    启停都在运行模式页里由人显式点。
    """
    try:
        return 200, engine.set_mode(conn, str(body.get("engine", "")).strip(),
                                    user["username"])
    except engine.EngineError as e:
        raise ApiError(str(e), 400)


def h_live_start(conn, query, body, user, params) -> tuple[int, dict]:
    """POST /api/engine/live/start —— 启动实时引擎（拉数据源进程 + 开始尾随入库）。

    默认参数来自 engine 模块（dt=1 s / log_every=10 → 10 s 粒度）。
    source 只接受 engine.SOURCES 里 available=True 的那一个；device 会被拒。
    """
    try:
        return 200, engine.LIVE.start(
            conn,
            source=str(body.get("source", "model") or "model"),
            username=user["username"],
            dt_s=qf(body, "dt_s", engine.DEFAULT_DT_S),
            log_every=qi(body, "log_every", engine.DEFAULT_LOG_EVERY))
    except engine.EngineError as e:
        raise ApiError(str(e), 400)


def h_live_stop(conn, query, body, user, params) -> tuple[int, dict]:
    """POST /api/engine/live/stop —— 停止实时引擎并收尾入库。"""
    try:
        return 200, engine.LIVE.stop(conn, user["username"])
    except engine.EngineError as e:
        raise ApiError(str(e), 400)


def h_live_reset(conn, query, body, user, params) -> tuple[int, dict]:
    """POST /api/engine/live/reset —— 清空实时数据集（保留台账与引擎模式）。"""
    try:
        return 200, engine.reset(conn, user["username"])
    except engine.EngineError as e:
        raise ApiError(str(e), 400)


# ---------------------------------------------------------------------
# 仿真（仿真模式的执行体）
# ---------------------------------------------------------------------
def h_sim_runs(conn, query, body, user, params) -> tuple[int, dict]:
    """GET /api/sim/runs —— 仿真运行台账 + 可选时长（日 / 周 / 月）。"""
    return 200, simrun.list_runs(conn, qi(query, "limit", 50))


def h_sim_start(conn, query, body, user, params) -> tuple[int, dict]:
    """POST /api/sim/runs —— 发起一次仿真。

    body: {kind: day|week|month, fault: bool, date?: 'YYYY-MM-DD'}
    ★ 立刻返回（202 语义也是 200：台账行已经落库、状态 running），
      进度靠 GET /api/sim/runs 轮询 —— 日仿真约 1 s、月仿真约十几秒，
      同步等会让前端在这十几秒里完全没反应。
    """
    kind = str(body.get("kind", "") or "").strip().lower()
    fault = bool(body.get("fault", False))
    date = body.get("date") or None
    if date is not None:
        date = str(date).strip() or None
        if date and not re.fullmatch(r"\d{4}-\d{2}-\d{2}", date):
            raise ApiError("date 只能是 YYYY-MM-DD", 400)
    try:
        return 200, simrun.start(conn, kind, fault=fault,
                                 username=user["username"], date=date)
    except simrun.SimRunError as e:
        raise ApiError(str(e), 400)


def h_sim_run_one(conn, query, body, user, params) -> tuple[int, dict]:
    """GET /api/sim/runs/{run_id} —— 单次仿真详情（含逐日能量明细）。"""
    try:
        return 200, simrun.get_run(conn, params["run_id"])
    except simrun.SimRunError as e:
        raise ApiError(str(e), 404)


def h_sim_run_delete(conn, query, body, user, params) -> tuple[int, dict]:
    """DELETE /api/sim/runs/{run_id} —— 删掉一次仿真（数据 + 台账 + 产物目录）。"""
    try:
        return 200, simrun.delete(conn, params["run_id"], user["username"])
    except simrun.SimRunError as e:
        raise ApiError(str(e), 400)


ROUTES: list[Route] = [
    ("GET",  re.compile(r"/api/health"), h_health, None),
    ("POST", re.compile(r"/api/login"), h_login, None),
    ("POST", re.compile(r"/api/logout"), h_logout, None),
    ("GET",  re.compile(r"/api/me"), h_me, None),

    ("GET",  re.compile(r"/api/overview"), h_overview, "viewer"),
    ("GET",  re.compile(r"/api/energy-flow"), h_overview, "viewer"),

    ("GET",  re.compile(r"/api/devices"), h_devices, "viewer"),
    ("POST", re.compile(r"/api/devices"), h_device_create, "operator"),

    # 设备接入（v1.2）—— 具体路径放在 /api/devices/{id} 之前，读起来更清楚
    ("GET",  re.compile(r"/api/devices/(?P<device_id>[\w\-]+)/point-map\.csv"),
     h_point_map_csv, "viewer"),
    ("GET",  re.compile(r"/api/devices/(?P<device_id>[\w\-]+)/point-map"),
     h_point_map, "viewer"),
    ("PUT",  re.compile(r"/api/devices/(?P<device_id>[\w\-]+)/point-map"),
     h_point_map_put, "operator"),
    ("PUT",  re.compile(r"/api/devices/(?P<device_id>[\w\-]+)/conn"),
     h_conn_put, "operator"),

    ("GET",  re.compile(r"/api/devices/(?P<device_id>[\w\-]+)"),
     h_device_one, "viewer"),

    ("GET",  re.compile(r"/api/realtime"), h_realtime, "viewer"),
    ("GET",  re.compile(r"/api/series"), h_series, "viewer"),
    ("GET",  re.compile(r"/api/steps"), h_steps, "viewer"),
    ("GET",  re.compile(r"/api/commands"), h_commands, "viewer"),

    ("GET",  re.compile(r"/api/alarms/summary"), h_alarm_summary, "viewer"),
    ("GET",  re.compile(r"/api/alarms"), h_alarms, "viewer"),

    ("GET",  re.compile(r"/api/strategies"), h_strategies, "viewer"),
    ("PUT",  re.compile(r"/api/strategies/(?P<strategy_id>[\w\-]+)"),
     h_strategy_update, "operator"),

    ("GET",  re.compile(r"/api/modes"), h_modes, "viewer"),
    ("POST", re.compile(r"/api/modes/activate"), h_mode_activate, "operator"),

    # 引擎（数据维度，v1.5）：运行模式 = 实时引擎在跑；仿真模式 = 离线批量仿真
    # 注意路由顺序：具体路径写在 /api/engine 之前无所谓（都是 fullmatch），
    # 但 /api/sim/runs/{id} 必须能被 fullmatch 命中，\w- 不含 '/'，安全。
    ("GET",  re.compile(r"/api/engine"), h_engine, "viewer"),
    ("POST", re.compile(r"/api/engine"), h_engine_set, "operator"),
    ("POST", re.compile(r"/api/engine/live/start"), h_live_start, "operator"),
    ("POST", re.compile(r"/api/engine/live/stop"), h_live_stop, "operator"),
    ("POST", re.compile(r"/api/engine/live/reset"), h_live_reset, "operator"),

    ("GET",  re.compile(r"/api/sim/runs"), h_sim_runs, "viewer"),
    ("POST", re.compile(r"/api/sim/runs"), h_sim_start, "operator"),
    ("GET",  re.compile(r"/api/sim/runs/(?P<run_id>[\w\-]+)"),
     h_sim_run_one, "viewer"),
    ("DELETE", re.compile(r"/api/sim/runs/(?P<run_id>[\w\-]+)"),
     h_sim_run_delete, "operator"),

    ("GET",  re.compile(r"/api/economics"), h_economics, "viewer"),
    ("GET",  re.compile(r"/api/energy"), h_energy, "viewer"),
    ("GET",  re.compile(r"/api/report"), h_report, "viewer"),
    ("GET",  re.compile(r"/api/invariants"), h_invariants, "viewer"),

    ("POST", re.compile(r"/api/control/command"), h_command_send, "operator"),

    ("GET",  re.compile(r"/api/audit"), h_audit, "admin"),
    ("GET",  re.compile(r"/api/users"), h_users, "admin"),
    ("POST", re.compile(r"/api/users"), h_user_create, "admin"),

    ("GET",  re.compile(r"/api/export/(?P<kind>\w+)"), h_export, "viewer"),
]


def dispatch(conn, method: str, path: str, query: dict, body: dict,
             user: Optional[dict]) -> tuple[int, dict]:
    for m, rx, fn, role in ROUTES:
        if m != method:
            continue
        mo = rx.fullmatch(path)
        if not mo:
            continue
        if role:
            ok, why = auth.require(user, role)
            if not ok:
                return 403 if user else 401, {"error": why}
        try:
            return fn(conn, query, body, user, mo.groupdict())
        except ApiError as e:
            return e.status, {"error": e.message}
        except (engine.EngineError, simrun.SimRunError) as e:
            # 领域层"可预期的错误"（数据源不可用 / 仿真器缺失 / 运行编号不存在…）
            # 一律 400：它们都是"你这么请求不行"，而不是平台内部炸了。
            return 400, {"error": str(e)}
        except Exception as e:  # 兜底：不要把栈泄露给前端
            return 500, {"error": f"内部错误: {type(e).__name__}: {e}"}
    return 404, {"error": f"未知接口 {method} {path}"}
