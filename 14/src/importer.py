"""14/ — 数据导入器：把项目的真实仿真产物装进平台数据库

数据源（都是仓库里跑出来的真产物，不是编造的数字）：
    10/build/timeseries.csv     8640 行 × 20 列（10 s 粒度 / 24 h）
    10/build/alarms.csv         告警与状态迁移
    10/build/summary.json       经济性 + 不变量 + 配置
    10/build/fault/*            故障注入场景

装进数据库之后，平台就能回答运维要问的问题：
「这个站这一天发生了什么、赚了多少、有没有越限、指令卡在哪一步」。

「真数据」的边界：本项目尚未接真机（无真实 PCS / 电表 / 调度），
所以现场实时量一律来自 10/ 的离线仿真 —— 这正是「缺真数据用模拟器」的口径。
"""

from __future__ import annotations

import csv
import json
import math
import os
import sys
import datetime
from typing import Any, Optional, Sequence

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import emsdb  # noqa: E402

# ---------------------------------------------------------------------
# 设备台账
#   额定值全部取自 10/build/summary.json 的 config / curves 段，
#   而不是手抄 —— 手抄常量出过一次「24 个点全灰」的事故。
# ---------------------------------------------------------------------
DEVICE_SPECS = [
    # (id, type, 额定功率 kW, 额定容量 kWh, 说明模板)
    ("DEV-PCS-01",   "PCS",         250.0, None,   "1# 储能变流器，额定 {p} kW"),
    ("DEV-BMS-01",   "BMS",         None,  1000.0, "1# 电池簇 BMS，额定容量 {c} kWh"),
    ("DEV-METER-01", "METER",       500.0, None,   "关口电表，双向计量，容量 {p} kW"),
    ("DEV-PV-01",    "PV",          300.0, None,   "光伏逆变器，峰值 {p} kW"),
    ("DEV-TR-01",    "TRANSFORMER", 500.0, None,   "并网变压器，容量 {p} kW"),
    ("DEV-LOAD-01",  "LOAD",        450.0, None,   "站内负荷，峰值 {p} kW"),
]

# ---------------------------------------------------------------------
# 策略清单 —— 与 04/src/strategies_9.h 的九个策略一一对应
#   mode_id 标注归属：
#     None            安全底座（L0/L1），全模式恒定启用，不归属任何单一模式
#     peak_shaving    削峰填谷 / 自用型
#     arbitrage       峰谷套利 / 卖电型
#     ancillary       辅助服务 / 卖电型
# ---------------------------------------------------------------------
STRATEGY_SPECS = [
    ("S01_BMS_FORBID",       "BmsForbidStrategy",        "L0", "BMS 禁止充放：全项目最硬的安全边界", None),
    ("S02_BMS_DERATE",       "BmsDerateStrategy",        "L1", "BMS 请求降功率（动态限值）", None),
    ("S03_TRANSFORMER_LIMIT","TransformerLimitStrategy", "L1", "变压器过载限功率（含前馈）", None),
    ("S04_DEMAND_MGMT",      "DemandMgmtStrategy",       "L2", "需量管理：削峰不超契约需量", "peak_shaving"),
    ("S05_ANTI_REVERSE",     "AntiReverseStrategy",      "L2", "防逆流：关口不倒送", "peak_shaving"),
    ("S06_PV_SMOOTHING",     "PvSmoothingStrategy",      "L2", "光伏出力平抑", None),
    ("S07_PEAK_VALLEY",      "PeakValleyStrategy",       "L3", "峰谷套利（Timed 模式）", "arbitrage"),
    ("S08_FORECAST_OPT",     "ForecastOptStrategy",      "L3", "动态预测优化 MPC（接 01/ 的 MILP 计划）", "arbitrage"),
    ("S09_DEMAND_RESPONSE",  "DemandResponseStrategy",   "L3", "需求响应（Custom 模式）", "ancillary"),
]

# ---------------------------------------------------------------------
# 运行模式 —— 独立顶层业务概念（run_mode 表）
#   四类客户/生意，一套 EMS 支持。目标函数、约束、结算口径、绑定策略各不相同。
# ---------------------------------------------------------------------
RUN_MODE_SPECS = [
    {
        "mode_id": "arbitrage",
        "name": "峰谷套利",
        "customer_type": "卖电型（独立储能电站 / 现货套利）",
        "objective": "最大化 放电量 × 峰谷价差，价低充电、价高放电",
        "constraints": ["电价时段", "SOC 上下限", "充放功率限值", "循环寿命成本"],
        "settlement": "卖电收入 = 放电量 × 上网电价（扣电池衰减成本）",
        "kpi_key": "net_benefit_cny",
        "bound_strategies": ["S07_PEAK_VALLEY", "S08_FORECAST_OPT"],
        "applicable_scene": "normal",
        "is_default": 1,
        "description": "电便宜时充电、贵时放电赚差价，卖给电网/现货市场",
    },
    {
        "mode_id": "peak_shaving",
        "name": "削峰填谷",
        "customer_type": "自用型（工厂 / 园区 / 商业楼宇）",
        "objective": "最大化 削减的最大需量 + 高峰少购电网电，压低电费",
        "constraints": ["变压器契约需量", "负荷预测", "SOC 上下限"],
        "settlement": "节省 = 少买的电网电 × 高峰电价 + 需量电费下降（间接收益）",
        "kpi_key": "saving_total_cny",
        "bound_strategies": ["S04_DEMAND_MGMT", "S05_ANTI_REVERSE"],
        "applicable_scene": "normal,fault",
        "is_default": 0,
        "description": "给自己负载用，削掉高峰需量、少从电网买高价电",
    },
    {
        "mode_id": "ancillary",
        "name": "辅助服务",
        "customer_type": "卖电型（并网储能 / 调频调峰运营商）",
        "objective": "最大化 调频/调峰/备用服务费，响应调度指令",
        "constraints": ["响应时间", "容量留备", "SOC 中位运行"],
        "settlement": "服务费 = 中标容量 × 结算单价（容量费 + 里程费）",
        "kpi_key": "net_benefit_cny",
        "bound_strategies": ["S09_DEMAND_RESPONSE"],
        "applicable_scene": "normal",
        "is_default": 0,
        "description": "参与调频/调峰/备用，赚辅助服务费，响应电网调度",
    },
    {
        "mode_id": "backup",
        "name": "保电备电",
        "customer_type": "保供型（医院 / 数据中心 / 关键负荷）",
        "objective": "最大化 停电时关键负荷保供时长，平时满充待命",
        "constraints": ["保供负荷清单", "最低 SOC 预留", "放电深度限制"],
        "settlement": "不计收益，价值 = 停电损失规避（可靠性优先）",
        "kpi_key": "soc",
        "bound_strategies": [],
        "applicable_scene": "fault",
        "is_default": 0,
        "description": "平时满充待命，停电时顶上关键负荷，保供优先",
    },
]

# step_record 的列（顺序与 10/ timeseries.csv 一致）
STEP_COLUMNS = [
    "t_s", "time_str", "state", "p_load_kw", "p_pv_kw", "p_grid_kw",
    "p_cmd_kw", "p_actual_kw", "soc", "temp_c", "p_lower_kw", "p_upper_kw",
    "plan_target", "correction", "clamped", "safety_clip", "state_gated",
    "hold_last", "fault_bits", "reason",
]

# CSV 列名 -> 内部列名
CSV_MAP = {
    "t_s": "t_s", "time": "time_str", "state": "state",
    "P_load_kW": "p_load_kw", "P_pv_kW": "p_pv_kw", "P_grid_kW": "p_grid_kw",
    "P_cmd_kW": "p_cmd_kw", "P_actual_kW": "p_actual_kw",
    "SOC": "soc", "T_C": "temp_c",
    "P_lower": "p_lower_kw", "P_upper": "p_upper_kw",
    "plan_target": "plan_target", "correction": "correction",
    "clamped": "clamped", "safety_clip": "safety_clip",
    "state_gated": "state_gated", "hold_last": "hold_last",
    "fault_bits": "fault_bits", "reason": "reason",
}

FLOAT_COLS = {
    "t_s", "p_load_kw", "p_pv_kw", "p_grid_kw", "p_cmd_kw", "p_actual_kw",
    "soc", "temp_c", "p_lower_kw", "p_upper_kw", "plan_target", "correction",
}
INT_COLS = {"clamped", "safety_clip", "state_gated", "hold_last", "fault_bits"}


def _f(v: Any, default: float = 0.0) -> float:
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def _i(v: Any) -> int:
    try:
        return int(float(v))
    except (TypeError, ValueError):
        return 0


def cmd_result(rec: dict) -> str:
    """把 StepRecord 的四个布尔折成一个可读结论。

    顺序即优先级：门控 > 保持 > 安全削顶 > 区间夹紧 > 正常。
    这与 07/ `EmsRuntime::step()` 的处理顺序一致。
    """
    if rec.get("state_gated"):
        return "gated"
    if rec.get("hold_last"):
        return "hold_last"
    if rec.get("safety_clip"):
        return "safety_clip"
    if rec.get("clamped"):
        return "clamped"
    return "ok"


# ---------------------------------------------------------------------
# 台账与策略
# ---------------------------------------------------------------------
def import_devices(conn, cfg: dict, curves: dict) -> int:
    vals = {
        "pcs": _f(cfg.get("pcs_rated_kw"), 250.0),
        "bat": _f(cfg.get("battery_capacity_kwh"), 1000.0),
        "tr": _f(cfg.get("transformer_capacity_kw"), 500.0),
        "meter": _f(cfg.get("transformer_capacity_kw"), 500.0),
        "pv": _f(curves.get("pv_max_kw"), 300.0),
        "load": _f(curves.get("load_max_kw"), 450.0),
    }
    key = {"PCS": "pcs", "BMS": "bat", "METER": "meter",
           "PV": "pv", "TRANSFORMER": "tr", "LOAD": "load"}
    n = 0
    for dev_id, dtype, rated, cap, note_tpl in DEVICE_SPECS:
        v = vals[key[dtype]]
        note = note_tpl.format(p=f"{v:g}", c=f"{v:g}")
        conn.execute(
            "INSERT INTO device(device_id, device_type, manufacturer, model,"
            " rated_power_kw, capacity_kwh, status, location, note)"
            " VALUES(?,?,?,?,?,?,?,?,?)"
            " ON CONFLICT(device_id) DO UPDATE SET"
            "   device_type=excluded.device_type, rated_power_kw=excluded.rated_power_kw,"
            "   capacity_kwh=excluded.capacity_kwh, status=excluded.status, note=excluded.note",
            (dev_id, dtype, "（模拟器）", f"SIM-{dtype}", rated, cap,
             "online", "站内", note),
        )
        n += 1
    return n


def import_strategies(conn, d_target_kw: float) -> int:
    now = _now()
    common = {"contract_demand_kw": d_target_kw, "source": "04/src/strategies_9.h"}
    n = 0
    for sid, stype, prio, desc, mode_id in STRATEGY_SPECS:
        conn.execute(
            "INSERT INTO strategy_config(strategy_id, strategy_type, priority,"
            " enabled, parameters, description, updated_at, mode_id)"
            " VALUES(?,?,?,?,?,?,?,?)"
            " ON CONFLICT(strategy_id) DO UPDATE SET"
            "   strategy_type=excluded.strategy_type, priority=excluded.priority,"
            "   parameters=excluded.parameters, description=excluded.description,"
            "   updated_at=excluded.updated_at, mode_id=excluded.mode_id",
            (sid, stype, prio, 1, json.dumps(common, ensure_ascii=False), desc, now, mode_id),
        )
        n += 1
    return n


def import_run_modes(conn) -> int:
    """写入四种运行模式。安全底座策略（mode_id=NULL）不随模式启停。"""
    n = 0
    for m in RUN_MODE_SPECS:
        conn.execute(
            "INSERT INTO run_mode(mode_id, name, customer_type, objective,"
            " constraints, settlement, kpi_key, bound_strategies,"
            " applicable_scene, is_default, active, description)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?)"
            " ON CONFLICT(mode_id) DO UPDATE SET"
            "   name=excluded.name, customer_type=excluded.customer_type,"
            "   objective=excluded.objective, constraints=excluded.constraints,"
            "   settlement=excluded.settlement, kpi_key=excluded.kpi_key,"
            "   bound_strategies=excluded.bound_strategies,"
            "   applicable_scene=excluded.applicable_scene,"
            "   description=excluded.description",
            (m["mode_id"], m["name"], m["customer_type"], m["objective"],
             json.dumps(m["constraints"], ensure_ascii=False),
             m["settlement"], m["kpi_key"],
             json.dumps(m["bound_strategies"], ensure_ascii=False),
             m["applicable_scene"], m["is_default"], 0, m["description"]),
        )
        n += 1
    # 激活默认模式（不覆盖已有的 active 标记，保证幂等）
    conn.execute("UPDATE run_mode SET active = 0")
    conn.execute("UPDATE run_mode SET active = 1 WHERE is_default = 1")
    conn.commit()
    return n


def _now() -> str:
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


# ---------------------------------------------------------------------
# 场景导入
# ---------------------------------------------------------------------
def import_scenario(conn, sim_dir: str, scenario_id: str, name: str,
                    date: Optional[str] = None) -> dict:
    """导入一个仿真场景目录（含 timeseries.csv / alarms.csv / summary.json）。

    date 为 energy_statistics 的起始日（默认今天）。
      · 单日场景（duration ≤ 24 h）→ 一行，日期 = date（既有口径）
      · 多日场景（周 / 月）        → 逐自然日一行，日期递增
    场景时长取自 summary.json 的 config.duration_s，不靠 log_rows 猜。
    """
    ts_csv = os.path.join(sim_dir, "timeseries.csv")
    al_csv = os.path.join(sim_dir, "alarms.csv")
    sj = os.path.join(sim_dir, "summary.json")

    if not os.path.isfile(ts_csv) or not os.path.isfile(sj):
        raise FileNotFoundError(f"场景目录缺少产物: {sim_dir}")

    with open(sj, "r", encoding="utf-8") as f:
        summary = json.load(f)

    # ---- 场景表 ----
    conn.execute(
        "INSERT INTO scenario(scenario_id, name, source_dir, steps, log_rows,"
        " wall_s, ok, alarms_total, alarms_fault, fault_ticks,"
        " invariants_all_ok, imported_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(scenario_id) DO UPDATE SET"
        "   name=excluded.name, source_dir=excluded.source_dir, steps=excluded.steps,"
        "   log_rows=excluded.log_rows, wall_s=excluded.wall_s, ok=excluded.ok,"
        "   alarms_total=excluded.alarms_total, alarms_fault=excluded.alarms_fault,"
        "   fault_ticks=excluded.fault_ticks,"
        "   invariants_all_ok=excluded.invariants_all_ok, imported_at=excluded.imported_at",
        (scenario_id, name, os.path.relpath(sim_dir).replace("\\", "/"),
         int(summary.get("steps", 0)), int(summary.get("log_rows", 0)),
         _f(summary.get("wall_s")), 1 if summary.get("ok") else 0,
         int(summary.get("alarms", {}).get("total", 0)),
         int(summary.get("alarms", {}).get("fault", 0)),
         int(summary.get("faults", {}).get("active_ticks", 0)),
         1 if summary.get("invariants", {}).get("all_ok") else 0,
         _now()),
    )

    # ---- 经济性 ----
    e = summary.get("economics", {})
    econ_cols = [
        "e_import_kwh", "e_export_kwh", "e_charge_kwh", "e_discharge_kwh",
        "throughput_kwh", "equiv_cycles", "pv_self_use_kwh",
        "peak_grid_kw", "peak_grid_base_kw",
        "cost_energy_cny", "cost_demand_cny", "revenue_feed_in_cny",
        "cost_total_cny",
        "cost_energy_base_cny", "cost_demand_base_cny",
        "revenue_feed_in_base_cny", "cost_total_base_cny",
        "saving_energy_cny", "saving_demand_cny",
        "saving_total_cny", "cost_degradation_cny", "net_benefit_cny",
        "saving_pct",
    ]
    conn.execute(
        f"INSERT INTO economics(scenario_id, {','.join(econ_cols)})"
        f" VALUES(?{',?' * len(econ_cols)})"
        f" ON CONFLICT(scenario_id) DO UPDATE SET"
        f"   {','.join(c + '=excluded.' + c for c in econ_cols)}",
        [scenario_id] + [_f(e.get(c)) for c in econ_cols],
    )

    # ---- 不变量 ----
    inv = summary.get("invariants", {})
    inv_cols = ["out_of_interval", "over_limit", "gated_nonzero", "grid_breach",
                "grid_breach_soc_limited", "tr_breach", "soc_violation",
                "hard_ok", "safety_ok", "all_ok"]
    conn.execute(
        f"INSERT INTO invariant(scenario_id, {','.join(inv_cols)})"
        f" VALUES(?{',?' * len(inv_cols)})"
        f" ON CONFLICT(scenario_id) DO UPDATE SET"
        f"   {','.join(c + '=excluded.' + c for c in inv_cols)}",
        [scenario_id] + [_i(inv.get(c)) for c in inv_cols],
    )

    # ---- 时序明细（先清后写，保证重导入幂等）----
    conn.execute("DELETE FROM step_record WHERE scenario_id = ?", (scenario_id,))
    conn.execute("DELETE FROM control_command WHERE scenario_id = ?", (scenario_id,))

    step_rows: list[tuple] = []
    cmd_rows: list[tuple] = []
    with open(ts_csv, "r", encoding="utf-8", newline="") as f:
        for raw in csv.DictReader(f):
            rec: dict[str, Any] = {}
            for cname, internal in CSV_MAP.items():
                v = raw.get(cname)
                if internal in FLOAT_COLS:
                    rec[internal] = _f(v)
                elif internal in INT_COLS:
                    rec[internal] = _i(v)
                else:
                    rec[internal] = (v or "").strip()
            step_rows.append((scenario_id,) + tuple(rec.get(c) for c in STEP_COLUMNS))
            # 指令表：EMS 对 PCS 的下发与实际
            cmd_rows.append((
                scenario_id, rec["t_s"], "DEV-PCS-01",
                rec["p_cmd_kw"], rec["p_actual_kw"],
                rec["p_lower_kw"], rec["p_upper_kw"],
                cmd_result(rec), rec["reason"],
            ))

    conn.executemany(
        f"INSERT OR REPLACE INTO step_record(scenario_id, {','.join(STEP_COLUMNS)})"
        f" VALUES({','.join(['?'] * (len(STEP_COLUMNS) + 1))})",
        step_rows,
    )
    conn.executemany(
        "INSERT OR REPLACE INTO control_command(scenario_id, ts, device_id,"
        " target_power_kw, actual_power_kw, p_lower_kw, p_upper_kw, result, reason)"
        " VALUES(?,?,?,?,?,?,?,?,?)",
        cmd_rows,
    )

    # ---- 告警 ----
    conn.execute("DELETE FROM alarm WHERE scenario_id = ?", (scenario_id,))
    alarm_rows = []
    if os.path.isfile(al_csv):
        with open(al_csv, "r", encoding="utf-8", newline="") as f:
            for a in csv.DictReader(f):
                src = (a.get("source") or "").strip()
                alarm_rows.append((
                    _f(a.get("t_s")), (a.get("time") or "").strip(),
                    (a.get("level") or "INFO").strip(), src,
                    _source_to_device(src), src,
                    (a.get("message") or "").strip(), "active", scenario_id,
                ))
    conn.executemany(
        "INSERT INTO alarm(ts, time_str, level, source, device_id, alarm_type,"
        " description, status, scenario_id) VALUES(?,?,?,?,?,?,?,?,?)",
        alarm_rows,
    )

    # ---- 能量统计：单日整段聚合 / 多日按日拆分 ----
    duration_s = _f((summary.get("config") or {}).get("duration_s"), 86400.0)
    # 减 1e-9 再上取整：duration 恰为 86400 / 604800 时不能多算一天。
    day_count = max(1, int(math.ceil(duration_s / 86400.0 - 1e-9)))
    day0 = date or datetime.date.today().isoformat()
    energies = write_energy_days(conn, scenario_id, day0, day_count)

    conn.commit()
    return {
        "scenario_id": scenario_id,
        "steps": len(step_rows),
        "commands": len(cmd_rows),
        "alarms": len(alarm_rows),
        "day_count": day_count,
        "duration_s": duration_s,
        "energy_days": [
            # ★ scenario_id 必须带上：这是**投影**出来的新 dict，不是原样透传。
            #   漏掉它时 main() 的打印 `e['scenario_id']` 直接 KeyError，
            #   表现为 `import_sim.bat` 抛栈退出（数据已入库，但构建判 FAIL）。
            {"scenario_id": scenario_id,
             "date": e["date"], "charge_kwh": round(e["charge_kwh"], 3),
             "discharge_kwh": round(e["discharge_kwh"], 3),
             "pv_kwh": round(e["pv_kwh"], 3), "load_kwh": round(e["load_kwh"], 3),
             "grid_kwh": round(e["grid_kwh"], 3),
             "net_benefit_cny": round(_f(e.get("net_benefit_cny")), 2)}
            for e in energies
        ],
        "net_benefit_cny": _f(e.get("net_benefit_cny")),
        "all_ok": bool(inv.get("all_ok")),
    }


def _source_to_device(source: str) -> str:
    """告警来源 -> 设备号。来源是模块名（FSM/SAFETY/...），能对上就对上。"""
    s = (source or "").upper()
    if "FSM" in s or "STATE" in s:
        return "DEV-BMS-01"
    if "SAFETY" in s or "TRANSFORMER" in s:
        return "DEV-TR-01"
    if "PCS" in s:
        return "DEV-PCS-01"
    if "METER" in s or "GRID" in s:
        return "DEV-METER-01"
    if "PV" in s:
        return "DEV-PV-01"
    return "DEV-BMS-01"


# ---------------------------------------------------------------------
# 能量统计（按日聚合）
# ---------------------------------------------------------------------
def compute_energy(conn, scenario_id: str,
                   day_index: Optional[int] = None) -> dict:
    """聚合一个场景（或其中第 day_index 个自然日）的能量指标。**不落表**。

    day_index = None  → 聚合整个场景（单日仿真 / live 实录走这条）
    day_index = 0..N  → 只聚合第 N 个自然日（周 / 月仿真走这条）
    """
    # 电量 = Σ(该拍功率 × **该拍自身的步长**) / 3600   →  单位 kWh
    #
    # ★ 这里曾经写成 Σ(功率) × (总时长 / 3600)，是**单位错误**：
    #    Σ(功率) 是 8640 个 kW 相加，再乘 24 h，等于把每一拍的功率
    #    都当成持续了 24 小时 —— 结果放大了 8640 倍。
    #    实测：光伏 2291 kWh 被写成 19794361；关口 5180.8 kWh 被写成 44762095。
    #    之所以没被立刻发现，是因为同一张表里 `charge_kwh` / `discharge_kwh`
    #    直接取自 10/ 的经济性核算（本来就是对的值），
    #    于是「536.6 与 19794361 并排」这种明显异常沉在表格里没人算过。
    #    修好后逐场景与 10/ 的 e_import_kwh 逐位吻合（5180.7988）。
    #
    # ★ 用逐拍 dt 加权而不是「平均值 × 拍数」：稳态仿真里两者等价，
    #    但一旦将来出现变步长（真机按事件触发采样），只有加权是对的。
    # ★ 必须套两层子查询：中间层的 `dt` 是下层窗口函数算出来的列，
    #    而外层要按它加权求和。
    # ★★ 中间层必须把外层要用到的列**全部投出来**：t_s（给 MIN/MAX 与
    #    COUNT 之外的时长）与 day（给按日过滤）。SQLite 的子查询不会
    #    "自动带上" 下层的列，少投一个就是 `no such column: t_s` ——
    #    这个坑第一次跑就踩到了（energy_statistics 整张表写不进去）。
    #
    # ---- day_index（周 / 月仿真按日拆分，v1.3 新增）----
    #   None  = 不拆日，聚合**整个场景**（单日仿真与 live 实录走这条，
    #           结果是逐位不变的既有口径，不要改动它）
    #   0..N  = 只聚合第 N 个自然日。日的归属用
    #             day = MAX(0, (CAST(t_s AS INTEGER) + 86399) / 86400 - 1)
    #           （SQLite 整数除法）—— 即"含端点"的第几天：
    #             t=0    → 0（数据集的**第一行**必须落在第 0 天）
    #             t=10   → 0；t=86400 → 0（这一天的**最后一行**仍在第 0 天）；
    #             t=86410 → 1；t=172800 → 1。
    #           ★ 外面那层 MAX(0, …) 不是装饰：不加它 t=0 会算出 -1，被分到
    #             "第 -1 天" 而**从所有日子里消失** —— 日均行数少一行、
    #             日明细表里第一行凭空不见，而电量看着还是对的（那一拍 dt=0），
    #             所以只表现为"行数对不上"。踩过。
    #           为什么不用 CEIL()：SQLite 的数学函数在部分构建里没编进去，
    #           而整数除法到处都有。为什么不用 t_s/86400 直接截断：
    #           那会把 t=86400 这一行判给第 1 天，第一天的电量就少一行，
    #           而单日口径恰好包含它 —— 两套口径会差一拍，很难发现。
    #   dt 用平均步长的 2 倍夹住：正常每拍 dt 就是 log 步长，夹子不生效；
    #   只有数据里出现异常大的空档（进程重启、缺行）时才会截断，避免
    #   一次空档把一整天的电量算爆。
    dt_cap = emsdb.query_value(
        conn,
        "SELECT CASE WHEN COUNT(*) > 1"
        "            THEN (MAX(t_s) - MIN(t_s)) / (COUNT(*) - 1) * 2.0"
        "            ELSE 1e9 END FROM step_record WHERE scenario_id = ?",
        (scenario_id,), 0.0) or 0.0

    inner_where = " WHERE scenario_id = ?"
    inner_args: list = [scenario_id]
    mid_where = ""
    mid_args: list = []
    if day_index is not None:
        mid_where = " WHERE day = ?"
        mid_args = [int(day_index)]

    row = emsdb.query_one(
        conn,
        "SELECT SUM(p_actual_kw * dt) AS wh_actual,"
        "       SUM(p_pv_kw     * dt) AS wh_pv,"
        "       SUM(p_load_kw   * dt) AS wh_load,"
        "       SUM(p_grid_kw   * dt) AS wh_grid,"
        "       SUM(CASE WHEN p_actual_kw < 0 THEN -p_actual_kw * dt ELSE 0 END) AS wh_chg,"
        "       SUM(CASE WHEN p_actual_kw > 0 THEN  p_actual_kw * dt ELSE 0 END) AS wh_dis,"
        "       SUM(CASE WHEN p_grid_kw   < 0 THEN -p_grid_kw   * dt ELSE 0 END) AS wh_export,"
        "       MAX(p_grid_kw)       AS peak_grid,"
        "       COUNT(*)             AS rows_n,"
        "       MIN(t_s)             AS t_min, MAX(t_s) AS t_max"
        " FROM (SELECT p_actual_kw, p_pv_kw, p_load_kw, p_grid_kw, t_s, day,"
        "              CASE WHEN raw_dt > ? THEN ? ELSE raw_dt END AS dt"
        "         FROM (SELECT p_actual_kw, p_pv_kw, p_load_kw, p_grid_kw, t_s,"
        "                      t_s - LAG(t_s, 1, 0) OVER (ORDER BY t_s) AS raw_dt,"
        "                      MAX(0, (CAST(t_s AS INTEGER) + 86399) / 86400 - 1)"
        "                          AS day"
        "                 FROM step_record" + inner_where + ")"
        + mid_where + ")",
        [dt_cap, dt_cap] + inner_args + mid_args,
    ) or {}
    econ = emsdb.query_one(conn, "SELECT * FROM economics WHERE scenario_id = ?",
                           (scenario_id,)) or {}

    def kwh(wh) -> float:
        """W·s（= kW·s）→ kWh。"""
        return _f(wh) / 3600.0

    if day_index is None:
        # ★ 单日口径：充放电量与上网电量**直接取 10/ 经济性核算的值**。
        #   它们是 10/ 用同一套电价模型算出来的权威值；用本函数从时序反算
        #   会引入微小差异（SOC 死区、四舍五入），而两条路径本应给出同一个数。
        charge = _f(econ.get("e_charge_kwh"))
        discharge = _f(econ.get("e_discharge_kwh"))
        export = _f(econ.get("e_export_kwh"))
        net = _f(econ.get("net_benefit_cny"))
    else:
        # 多日口径：10/ 的经济性是**整段**一个值，拆不到某一天，
        #   所以按日只能从时序反算。net_benefit 由 write_energy_days() 分摊。
        charge = kwh(row.get("wh_chg"))
        discharge = kwh(row.get("wh_dis"))
        export = kwh(row.get("wh_export"))
        net = None

    return {
        "date": None, "scenario_id": scenario_id,
        "day_index": day_index,
        "charge_kwh": charge, "discharge_kwh": discharge,
        "pv_kwh": kwh(row.get("wh_pv")),
        "load_kwh": kwh(row.get("wh_load")),
        "grid_kwh": kwh(row.get("wh_grid")),
        "export_kwh": export,
        "peak_grid_kw": _f(row.get("peak_grid")),
        "net_benefit_cny": net,
        "rows": int(_f(row.get("rows_n"))),
        "t_min": _f(row.get("t_min")), "t_max": _f(row.get("t_max")),
    }


def _insert_energy(conn, date: str, e: dict) -> None:
    conn.execute(
        "INSERT OR REPLACE INTO energy_statistics(date, scenario_id, charge_kwh,"
        " discharge_kwh, pv_kwh, load_kwh, grid_kwh, export_kwh, peak_grid_kw,"
        " net_benefit_cny) VALUES(?,?,?,?,?,?,?,?,?,?)",
        (date, e["scenario_id"], e["charge_kwh"], e["discharge_kwh"],
         e["pv_kwh"], e["load_kwh"], e["grid_kwh"], e["export_kwh"],
         e["peak_grid_kw"], _f(e.get("net_benefit_cny"))),
    )


def aggregate_energy(conn, scenario_id: str, date: str,
                     day_index: Optional[int] = None) -> dict:
    """[单场景口径] 计算 + 落表，并返回展示用的圆整值。

    ★ 先清掉该场景的**所有日期**行：一个仿真产物只对应一个"运行日"，
      而 day 取的是**导入当天**。跨日重导时若不清理，(date, scenario_id)
      主键会把**旧日期**的行留下来 —— 表现为界面「按日 × 场景」表里并排
      出现今天与昨天两套同场景数据。生产库上实测踩到：修好电量公式后，
      前一天那两行仍带着放大 8640 倍的旧值显示在界面上。
      本函数是唯一漏了"先清后写"的地方 —— step_record / control_command /
      alarm 都按 scenario_id 清过。
    """
    e = compute_energy(conn, scenario_id, day_index)
    conn.execute("DELETE FROM energy_statistics WHERE scenario_id = ?", (scenario_id,))
    _insert_energy(conn, date, e)
    conn.commit()
    return {
        "date": date, "scenario_id": scenario_id,
        "charge_kwh": round(e["charge_kwh"], 3),
        "discharge_kwh": round(e["discharge_kwh"], 3),
        "pv_kwh": round(e["pv_kwh"], 3), "load_kwh": round(e["load_kwh"], 3),
        "grid_kwh": round(e["grid_kwh"], 3),
        "peak_grid_kw": round(e["peak_grid_kw"], 3),
    }


def write_energy_days(conn, scenario_id: str, date: str,
                      day_count: int = 1) -> list[dict]:
    """按日写 energy_statistics。

    day_count <= 1 → 走 aggregate_energy 的既有口径（整段聚合，逐位不变）。
    day_count >  1 → 逐自然日聚合（日 / 周 / 月仿真的真入口）。

    ★ net_benefit_cny 在周期口径下是**整段一个值**（10/ 的经济性核算不对
      某一天负责），拆到每天只能分摊。分摊依据取**当天吞吐量占比**
      （充+放电量），因为净收益的本质就是吞吐量带来的峰谷价差与需量削减。
      不变量：Σ(每日分摊) == 整段净收益 —— 由 14/tests/selftest.py 断言。
      界面上这一列必须标注"按日分摊"，不能让人误以为它是独立算出来的。
    """
    if day_count <= 1:
        return [aggregate_energy(conn, scenario_id, date, None)]

    days = [compute_energy(conn, scenario_id, d) for d in range(int(day_count))]
    total_thr = sum(d["charge_kwh"] + d["discharge_kwh"] for d in days)
    econ = emsdb.query_one(conn, "SELECT net_benefit_cny FROM economics"
                                 " WHERE scenario_id = ?", (scenario_id,)) or {}
    econ_net = _f(econ.get("net_benefit_cny"))

    conn.execute("DELETE FROM energy_statistics WHERE scenario_id = ?", (scenario_id,))
    out: list[dict] = []
    base = datetime.date.fromisoformat(date)
    for i, d in enumerate(days):
        if total_thr > 1e-9:
            share = (d["charge_kwh"] + d["discharge_kwh"]) / total_thr
        else:
            share = 1.0 / max(1, len(days))
        d["net_benefit_cny"] = econ_net * share
        d["date"] = (base + datetime.timedelta(days=i)).isoformat()
        d["day_index"] = i
        _insert_energy(conn, d["date"], d)
        out.append(d)
    conn.commit()
    return out


# ---------------------------------------------------------------------
# 实时入库（07/ 现场进程 --record 写出的 CSV）
# ---------------------------------------------------------------------
def csv_row_to_rec(raw: dict) -> dict[str, Any]:
    """实录 CSV 的一行 → step_record 的内部列名。"""
    rec: dict[str, Any] = {}
    for cname, internal in CSV_MAP.items():
        v = raw.get(cname)
        if internal in FLOAT_COLS:
            rec[internal] = _f(v)
        elif internal in INT_COLS:
            rec[internal] = _i(v)
        else:
            rec[internal] = (v or "").strip()
    return rec


def _ensure_device_ledger(conn) -> None:
    """设备台账兜底。

    control_command 有外键 REFERENCES device(device_id)。live 导入若在
    仿真导入之前单独跑（现场首次上线时很可能如此），device 表是空的，
    直接插指令会撞外键。这里用默认额定值补最小台账（与 DEVICE_SPECS 一致），
    已有台账则 ON CONFLICT 不动。
    """
    have_dev = emsdb.query_value(
        conn, "SELECT COUNT(*) FROM device WHERE device_id = 'DEV-PCS-01'", (), 0)
    if have_dev == 0:
        import_devices(conn, {}, {})


_STEP_INSERT = (
    f"INSERT OR REPLACE INTO step_record(scenario_id, {','.join(STEP_COLUMNS)})"
    f" VALUES({','.join(['?'] * (len(STEP_COLUMNS) + 1))})")
_CMD_INSERT = (
    "INSERT OR REPLACE INTO control_command(scenario_id, ts, device_id,"
    " target_power_kw, actual_power_kw, p_lower_kw, p_upper_kw, result, reason)"
    " VALUES(?,?,?,?,?,?,?,?,?)")


def import_live_rows(conn, recs: Sequence[dict], scenario_id: str,
                     name: str, src: str) -> dict:
    """把**已解析**的实录行增量入库（14/ 引擎尾随文件时走这条）。

    为什么要有这个函数，而不是让引擎反复调 import_live_record：
      import_live_record 每次都从头读完整个 CSV。实时跑一整天，文件会长到
      8640 行；若每 10 秒重读一遍，一天要做 8640 次全量解析 —— 数据越长越慢，
      典型的 O(n²)。引擎自己维护文件偏移量，只把**新增的完整行**交给这里。

    仍然是幂等的：主键 (scenario_id, t_s) + INSERT OR REPLACE，
    引擎崩溃重启后重放同一段也不会产生重复行。
    """
    if not recs:
        return {"scenario_id": scenario_id, "time_base": "wall", "rows": 0,
                "cumulative": emsdb.query_value(
                    conn, "SELECT COUNT(*) FROM step_record WHERE scenario_id = ?",
                    (scenario_id,), 0)}

    _ensure_device_ledger(conn)
    # 场景表：live 场景的 time_base 必须显式 'wall'
    conn.execute(
        "INSERT INTO scenario(scenario_id, name, source_dir, time_base,"
        " imported_at) VALUES(?,?,?,?,?)"
        " ON CONFLICT(scenario_id) DO UPDATE SET"
        "   name=excluded.name, source_dir=excluded.source_dir,"
        "   time_base=excluded.time_base, imported_at=excluded.imported_at",
        (scenario_id, name, src, "wall", _now()),
    )

    step_rows = [(scenario_id,) + tuple(r.get(c) for c in STEP_COLUMNS) for r in recs]
    cmd_rows = [(scenario_id, r["t_s"], "DEV-PCS-01",
                 r["p_cmd_kw"], r["p_actual_kw"],
                 r["p_lower_kw"], r["p_upper_kw"],
                 cmd_result(r), r["reason"]) for r in recs]
    conn.executemany(_STEP_INSERT, step_rows)
    conn.executemany(_CMD_INSERT, cmd_rows)

    # 回填 steps / log_rows（累计口径，不是本次新增）
    total = emsdb.query_value(
        conn, "SELECT COUNT(*) FROM step_record WHERE scenario_id = ?",
        (scenario_id,), 0)
    conn.execute(
        "UPDATE scenario SET steps=?, log_rows=? WHERE scenario_id=?",
        (total, total, scenario_id),
    )
    conn.commit()
    return {"scenario_id": scenario_id, "time_base": "wall",
            "rows": len(step_rows), "cumulative": total}


def import_live_record(conn, csv_path: str, scenario_id: str,
                       name: str) -> dict:
    """把 07/ 现场进程 --record 写出的 CSV **增量**入库。

    与 import_scenario 的关键区别：
      · time_base='wall'：t_s 是 Unix 墙钟秒、time_str 是完整日期时间，
        不再是"一天内的第几秒"。两套时间轴不可混算（见 schema.sql 头注释）。
      · **增量追加**：不 DELETE 旧行。现场进程边跑边追加写 CSV，
        本函数每次读到的都是"截至此刻"的完整文件，用 INSERT OR REPLACE
        （主键 (scenario_id, t_s)）天然幂等 —— 重复导入同一批数据不产生重复行。
      · 没有 summary.json：经济性/不变量要等收尾才能算，live 阶段只进
        step_record 与 control_command（曲线/时序/指令三个页面立即可看）。

    适用场景：**一次性**把一个实录文件导进来（import_live.bat 与自检走这条）。
    持续尾随请用 import_live_rows()，别在这里做 O(n²) 的全量重读。
    """
    if not os.path.isfile(csv_path):
        raise FileNotFoundError(f"实录文件不存在: {csv_path}")

    try:
        src = os.path.relpath(csv_path).replace("\\", "/")
    except ValueError:
        # 跨盘符（如 CSV 在 C:、cwd 在 D:）时 relpath 抛错，退化为绝对路径。
        src = os.path.abspath(csv_path).replace("\\", "/")

    with open(csv_path, "r", encoding="utf-8", newline="") as f:
        recs = [csv_row_to_rec(raw) for raw in csv.DictReader(f)]
    return import_live_rows(conn, recs, scenario_id, name, src)


# ---------------------------------------------------------------------
# 总入口
# ---------------------------------------------------------------------
def import_all(build_dir: str, db_path: Optional[str] = None,
               date: Optional[str] = None) -> dict:
    """导入 10/build 下的全部场景。返回导入摘要。"""
    conn = emsdb.init_db(db_path)

    with open(os.path.join(build_dir, "summary.json"), "r", encoding="utf-8") as f:
        base = json.load(f)

    n_dev = import_devices(conn, base.get("config", {}), base.get("curves", {}))
    n_str = import_strategies(conn, _f(base.get("config", {}).get("d_target_kw"), 400.0))
    n_mode = import_run_modes(conn)

    scenes = [("normal", os.path.join(build_dir), "24h 全场景（无故障）")]
    fault_dir = os.path.join(build_dir, "fault")
    if os.path.isdir(fault_dir):
        scenes.append(("fault", fault_dir, "24h 故障注入场景"))

    day = date or datetime.date.today().isoformat()
    results = []
    for sid, d, nm in scenes:
        results.append(import_scenario(conn, d, sid, nm, day))

    # 能量统计已经由 import_scenario 按 summary 的 duration_s 写好
    # （单日 → 整段一行；多日 → 逐自然日一行）。
    # ★ 这里**不要**再调一次 aggregate_energy —— 那是"整个场景合成一行"的口径，
    #   会把周/月场景的逐日拆分覆盖成一行，而且不报错。
    energies = [e for r in results for e in r.get("energy_days", [])]

    emsdb.set_meta(conn, emsdb.META_IMPORT_SOURCE,
                   os.path.abspath(build_dir).replace("\\", "/"))
    emsdb.set_meta(conn, emsdb.META_IMPORT_AT, _now())
    conn.commit()

    return {
        "db": emsdb.DEFAULT_DB_PATH if db_path is None else db_path,
        "devices": n_dev, "strategies": n_str, "run_modes": n_mode,
        "scenarios": results, "energy": energies,
        "counts": emsdb.table_counts(conn),
    }


def main() -> int:
    # 中文 Windows 控制台（CP936）下，个别字符可能编不出来；宁可显示成 ? 也不要崩
    try:
        sys.stdout.reconfigure(errors="replace")  # type: ignore[union-attr]
    except (AttributeError, ValueError):
        pass

    # --live <csv> [scenario_id]：07/ 现场实录增量入库
    if len(sys.argv) >= 2 and sys.argv[1] == "--live":
        if len(sys.argv) < 3:
            print("[FAIL] --live 需要实录 CSV 路径：--live <csv> [scenario_id]")
            return 2
        csv_path = sys.argv[2]
        scenario_id = sys.argv[3] if len(sys.argv) > 3 else "live"
        db_path = sys.argv[4] if len(sys.argv) > 4 else None
        conn = emsdb.init_db(db_path)
        r = import_live_record(conn, csv_path, scenario_id,
                               f"现场实录（{scenario_id}）")
        print(f"[OK] live 入库: 场景 {r['scenario_id']} (time_base=wall)")
        print(f"     本次 {r['rows']} 行，累计 {r['cumulative']} 行")
        print(f"     库: {emsdb.DEFAULT_DB_PATH if db_path is None else db_path}")
        return 0

    root = os.path.dirname(emsdb.MODULE_ROOT)
    default_build = os.path.join(root, "10", "build")
    build_dir = sys.argv[1] if len(sys.argv) > 1 else default_build
    db_path = sys.argv[2] if len(sys.argv) > 2 else None

    if not os.path.isdir(build_dir):
        print(f"[FAIL] 仿真产物目录不存在: {build_dir}")
        print("       先跑 10/scripts/run_demo.bat 生成产物。")
        return 1

    r = import_all(build_dir, db_path)
    print(f"[OK] 数据库: {r['db']}")
    print(f"     设备 {r['devices']} 台 / 策略 {r['strategies']} 条 / 运行模式 {r['run_modes']} 种")
    for s in r["scenarios"]:
        print(f"     场景 {s['scenario_id']:6s} 时序 {s['steps']:6d} 行"
              f" / 指令 {s['commands']:6d} 行 / 告警 {s['alarms']:3d} 条"
              f" / 净收益 {s['net_benefit_cny']:9.2f} 元"
              f" / 不变量全过 {s['all_ok']}")
    for e in r["energy"]:
        # 单位分开印：电量 kWh、峰值功率 kW。以前写成「kWh·kW」，
        # 看不出哪个数是电量、哪个是功率 —— 而这一次的 8640 倍电量事故，
        # 恰恰就是「单位没人算过」造成的。
        # ★ 日期也要印：周 / 月场景每个场景有 N 行（逐自然日），只印场景名分不清是哪天。
        print(f"     电量 {e['scenario_id']:6s} {str(e.get('date') or '—'):10s}"
              f" 充 {e['charge_kwh']:8.3f} /"
              f" 放 {e['discharge_kwh']:8.3f} kWh"
              f" ｜ 光伏 {e['pv_kwh']:8.3f} / 负荷 {e['load_kwh']:9.3f} /"
              f" 关口 {e['grid_kwh']:9.3f} kWh")
    print("     表行数: " + ", ".join(
        f"{k}={v}" for k, v in r["counts"].items() if v > 0))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
