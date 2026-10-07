"""14/ — 平台后端自检

沿用项目其余模块的纪律：断言式、有 PASS/FAIL 计数、内容缺失时显式 SKIP。

覆盖十层：
  A. schema        —— 18 张表是否齐全（14 原有 + run_mode + 设备接入两张 + sim_run，schema v1.4）
  B. 导入          —— 行数是否与源产物一致
  C. 数据一致性    —— 入库值是否与 10/build/summary.json 逐项相符
                    （这是最有价值的一组：**它证明平台展示的不是编造的数字**）
  D. 幂等          —— 重复导入不改变结果
  E. 认证与权限    —— 口令、会话、角色
  F. REST 接口     —— 每个端点的状态码与关键字段
  G. 设备接入闭环  —— 点表校验/落盘/接入参数，含**跨语言**判据：
                    14/ 写出的 active.csv 交给 13/ 的 C++ 进程读，逐字段对拍
  H. 旧库迁移      —— v1.1 老库缺列时能否幂等补齐
  I. 实时入库      —— time_base=wall 的两套时间轴、增量幂等、台账兜底
  J. 引擎双模式    —— 运行/仿真两条数据链路：仿真调度端到端入库、
                    实时引擎真的被拉起/写数据/停掉、多日能量分摊不变量

运行：
    python 14/tests/selftest.py
"""

from __future__ import annotations

import csv
import datetime
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MODULE = os.path.dirname(HERE)
SRC = os.path.join(MODULE, "src")
sys.path.insert(0, SRC)

import api      # noqa: E402
import auth     # noqa: E402
import connconf  # noqa: E402
import emsdb    # noqa: E402
import engine   # noqa: E402
import importer  # noqa: E402
import pointmap  # noqa: E402
import simrun   # noqa: E402

PROJECT_ROOT = os.path.dirname(MODULE)
BUILD_DIR = os.path.join(PROJECT_ROOT, "10", "build")


class T:
    def __init__(self) -> None:
        self.n_pass = 0
        self.n_fail = 0
        self.n_skip = 0
        self.failures: list[str] = []

    def ok(self, cond: bool, name: str) -> bool:
        if cond:
            self.n_pass += 1
        else:
            self.n_fail += 1
            self.failures.append(name)
            print(f"  [FAIL] {name}")
        return bool(cond)

    def eq(self, got, want, name: str) -> bool:
        return self.ok(got == want, f"{name}（得到 {got!r}，期望 {want!r}）")

    def close(self, got, want, name: str, tol: float = 1e-6) -> bool:
        try:
            good = abs(float(got) - float(want)) <= tol
        except (TypeError, ValueError):
            good = False
        return self.ok(good, f"{name}（得到 {got!r}，期望 {want!r}±{tol}）")

    def skip(self, why: str) -> None:
        self.n_skip += 1
        print(f"  [SKIP] {why}")

    def report(self, title: str) -> int:
        print()
        print("=" * 64)
        print(f" {title}")
        print(f" PASS={self.n_pass}  FAIL={self.n_fail}  SKIPPED={self.n_skip}")
        if self.failures:
            print(" 失败项：")
            for f in self.failures[:20]:
                print("   -", f)
        print("=" * 64)
        return 0 if self.n_fail == 0 else 1


def _body(**kw) -> dict:
    return dict(kw)


def _q(**kw) -> dict:
    return {k: [str(v)] for k, v in kw.items()}


def main() -> int:
    try:
        sys.stdout.reconfigure(errors="replace")  # type: ignore[union-attr]
    except (AttributeError, ValueError):
        pass

    t = T()
    print("=" * 64)
    print(" 14/ 平台后端自检")
    print(f" 时间 {datetime.datetime.now():%Y-%m-%d %H:%M:%S}")
    print("=" * 64)

    if not os.path.isfile(os.path.join(BUILD_DIR, "summary.json")):
        t.skip(f"缺少 {BUILD_DIR}/summary.json —— 先跑 10/scripts/run_demo.bat")
        return t.report("14/ 平台后端自检（无法执行）")

    tmpdir = tempfile.mkdtemp(prefix="ems14_selftest_")
    db_path = os.path.join(tmpdir, "test.db")

    # =================================================================
    print("\n[A] schema")
    # =================================================================
    conn = emsdb.init_db(db_path)
    counts = emsdb.table_counts(conn)
    t.eq(len(emsdb.TABLES), 18, "schema 表数（v1.4 加 sim_run 后 18 张）")
    t.eq(emsdb.SCHEMA_VERSION, "1.4", "schema 版本常量")
    for tb in emsdb.TABLES:
        t.ok(counts.get(tb, -1) >= 0, f"表存在: {tb}")
    t.eq(emsdb.get_meta(conn, emsdb.META_SCHEMA_VERSION), emsdb.SCHEMA_VERSION,
         "schema 版本写入 meta")
    # v1.2 新增列：scenario.time_base（时刻口径）
    t.ok("time_base" in emsdb._table_columns(conn, "scenario"),
         "新库 scenario 含 time_base 列")
    # v1.3 新增列：economics 电度电费基线 4 字段
    for _c in ("cost_energy_base_cny", "cost_demand_base_cny",
               "revenue_feed_in_cny", "revenue_feed_in_base_cny"):
        t.ok(_c in emsdb._table_columns(conn, "economics"),
             f"新库 economics 含 {_c} 列")
    # v1.4 新增表：sim_run（仿真运行台账）。列名是**接口契约** ——
    # 15/ 的仿真页直接按这些名字渲染，改列名等于改接口。
    sim_cols = emsdb._table_columns(conn, "sim_run")
    for _c in ("run_id", "kind", "scenario_id", "fault", "duration_s", "dt_s",
               "log_every", "status", "started_at", "finished_at", "wall_s",
               "steps", "log_rows", "alarms_total", "net_benefit_cny",
               "saving_total_cny", "invariants_all_ok", "artifact_dir",
               "username", "error"):
        t.ok(_c in sim_cols, f"sim_run 含列 {_c}")

    # =================================================================
    print("\n[B] 导入")
    # =================================================================
    res = importer.import_all(BUILD_DIR, db_path)
    with open(os.path.join(BUILD_DIR, "summary.json"), "r", encoding="utf-8") as f:
        base = json.load(f)
    with open(os.path.join(BUILD_DIR, "fault", "summary.json"), "r",
              encoding="utf-8") as f:
        fault_sum = json.load(f)

    t.eq(res["devices"], 6, "设备台账条数")
    t.eq(res["strategies"], 9, "策略条数（04/ 的九个策略）")
    c = res["counts"]
    t.eq(c["device"], 6, "device 行数")
    t.eq(c["strategy_config"], 9, "strategy_config 行数")
    t.eq(c["scenario"], 2, "scenario 行数（normal + fault）")
    t.eq(c["step_record"], 8640 * 2, "step_record 行数（两场景各 8640）")
    t.eq(c["control_command"], 8640 * 2, "control_command 行数")
    t.eq(c["economics"], 2, "economics 行数")
    t.eq(c["invariant"], 2, "invariant 行数")
    t.eq(c["energy_statistics"], 2, "energy_statistics 行数")

    sc_n = emsdb.query_one(conn, "SELECT * FROM scenario WHERE scenario_id='normal'")
    t.eq(sc_n["steps"], base["steps"], "normal.steps 与 summary 相符")
    t.eq(sc_n["log_rows"], base["log_rows"], "normal.log_rows 与 summary 相符")
    t.eq(sc_n["alarms_total"], base["alarms"]["total"], "normal 告警总数相符")

    al_n = emsdb.query_value(conn, "SELECT COUNT(*) FROM alarm WHERE scenario_id='normal'")
    al_f = emsdb.query_value(conn, "SELECT COUNT(*) FROM alarm WHERE scenario_id='fault'")
    t.eq(al_n, base["alarms"]["total"], "normal 告警入库条数")
    t.eq(al_f, fault_sum["alarms"]["total"], "fault 告警入库条数")

    # =================================================================
    print("\n[C] 数据一致性（入库值 vs summary.json）")
    # =================================================================
    e_n = emsdb.query_one(conn, "SELECT * FROM economics WHERE scenario_id='normal'")
    for key in ("net_benefit_cny", "saving_total_cny", "cost_total_cny",
                "cost_total_base_cny", "e_charge_kwh", "e_discharge_kwh",
                "peak_grid_kw", "peak_grid_base_kw", "saving_pct",
                "equiv_cycles", "throughput_kwh", "cost_degradation_cny"):
        t.close(e_n[key], base["economics"][key], f"economics.normal.{key}")

    e_f = emsdb.query_one(conn, "SELECT * FROM economics WHERE scenario_id='fault'")
    for key in ("net_benefit_cny", "peak_grid_kw", "saving_total_cny"):
        t.close(e_f[key], fault_sum["economics"][key], f"economics.fault.{key}")

    iv_n = emsdb.query_one(conn, "SELECT * FROM invariant WHERE scenario_id='normal'")
    iv_f = emsdb.query_one(conn, "SELECT * FROM invariant WHERE scenario_id='fault'")
    t.eq(iv_n["all_ok"], 1, "normal 不变量全过（安全红线）")
    for k in ("out_of_interval", "over_limit", "gated_nonzero", "grid_breach",
              "tr_breach", "soc_violation"):
        t.eq(iv_n[k], 0, f"normal.{k} = 0")
    t.eq(iv_f["all_ok"], 0, "fault 场景不变量**不过**（故障注入，预期）")

    # 时序明细抽查：首行数值与 csv 一致
    first = emsdb.query_one(
        conn, "SELECT * FROM step_record WHERE scenario_id='normal'"
              " ORDER BY t_s LIMIT 1")
    t.close(first["t_s"], 10.0, "首拍 t_s")
    t.close(first["p_grid_kw"], 399.993, "首拍 关口功率")
    t.close(first["p_actual_kw"], -147.993, "首拍 储能功率（放电为正）")
    t.eq(first["state"], "NORMAL", "首拍状态")

    # 指令 result 分布应覆盖多档
    rs = {r["result"]: r["n"] for r in emsdb.query(
        conn, "SELECT result, COUNT(*) AS n FROM control_command"
              " WHERE scenario_id='normal' GROUP BY result")}
    t.ok(len(rs) >= 2, f"指令结果覆盖多档: {sorted(rs)}")
    t.eq(sum(rs.values()), 8640, "normal 指令总数")

    # 告警等级
    lv = {r["level"] for r in emsdb.query(
        conn, "SELECT DISTINCT level FROM alarm")}
    t.ok("INFO" in lv, f"告警含 INFO 级: {sorted(lv)}")

    # ---- 电量（energy_statistics）：与 10/ 的独立核算交叉验证 ----
    #
    # ★ 本节是为一次真实事故补的护栏（2026-09-25）：
    #   电量原按 Σ(功率) × (总时长 / 3600) 计算 —— 把 **每一拍** 的功率都
    #   当成持续了 24 小时，结果放大 8640 倍（光伏 2291 kWh 写成 19794361）。
    #   当时的 C 段只逐项核对 economics，**没有一条断言碰过 energy_statistics**，
    #   所以「一列 536.6 挨着一列 19794361」这种明显异常没有任何测试拦住它，
    #   最后是在浏览器里看表格才发现的。
    #
    #   三条护栏各有分工：
    #     ① 与 economics 的独立口径对账（关口电量 = e_import − e_export）
    #        —— 单位/系数错就立刻红，且它**不依赖本次实现**
    #     ② 同表五个量必须在同一数量级 —— 量级错最直接的体现
    #     ③ 光伏/负荷落在物理合理区间 —— 防"数量级对但系数仍错"
    for sc in ("normal", "fault"):
        en = emsdb.query_one(
            conn, "SELECT * FROM energy_statistics WHERE scenario_id=?", (sc,))
        ec = emsdb.query_one(
            conn, "SELECT * FROM economics WHERE scenario_id=?", (sc,))
        # 首拍 dt=0 不计入，故留 0.1% 相对余量（≈ ±5 kWh）
        t.close(en["grid_kwh"], ec["e_import_kwh"] - ec["e_export_kwh"],
                f"{sc} 关口电量 = e_import − e_export（10/ 独立口径）", tol=5.0)
        vals = [en["charge_kwh"], en["discharge_kwh"], en["pv_kwh"],
                en["load_kwh"], en["grid_kwh"]]
        t.ok(min(vals) > 0,
             f"{sc} 五个电量均为正: {[round(v, 1) for v in vals]}")
        t.ok(max(vals) / min(vals) < 100,
             f"{sc} 电量同数量级（最大/最小 = {max(vals) / min(vals):.1f} < 100）")
        t.ok(1000 < en["pv_kwh"] < 6000,
             f"{sc} 光伏电量合理（{en['pv_kwh']:.1f} kWh/日，期望 1000~6000）")
        t.ok(3000 < en["load_kwh"] < 12000,
             f"{sc} 负荷电量合理（{en['load_kwh']:.1f} kWh/日，期望 3000~12000）")

    # =================================================================
    print("\n[D] 幂等")
    # =================================================================
    r2 = importer.import_all(BUILD_DIR, db_path)
    t.eq(r2["counts"]["step_record"], c["step_record"], "重复导入 step_record 不变")
    t.eq(r2["counts"]["control_command"], c["control_command"],
         "重复导入 control_command 不变")
    t.eq(r2["counts"]["alarm"], c["alarm"], "重复导入 alarm 不变")
    t.eq(r2["counts"]["device"], c["device"], "重复导入 device 不变")
    e_n2 = emsdb.query_one(conn, "SELECT * FROM economics WHERE scenario_id='normal'")
    t.close(e_n2["net_benefit_cny"], e_n["net_benefit_cny"],
            "重复导入经济性不漂移")

    # ---- 跨日重导：一个仿真产物只对应一个运行日，不该堆积历史行 ----
    #
    # ★ 本节为第二个真实事故加的（2026-09-25）：
    #   `energy_statistics` 的主键是 (date, scenario_id)，而 date 取**导入当天**，
    #   且它当时是唯一漏了"先清后写"的表。于是跨日重导会把昨天的行留下来 ——
    #   生产库里实测出现「2026-09-25 光伏 19794361」与「2026-09-26 光伏 2291」
    #   并排显示，旧行还带着已修复的错值。
    #
    #   ★ 为什么原来的幂等测试抓不到：D 段用同一个 day 重导两次，主键完全重合
    #     → 必然不堆积。**"同一天重导幂等"不等于"跨日重导干净"**，
    #     而真实部署恰恰是每天重导一次。
    #   ★ 为什么整个自检都抓不到：selftest 用的是 tempfile 建的**全新库**，
    #     每次从空库导入 —— 只有"长期运行的生产库"才会暴露这一类问题。
    n_before = emsdb.query_value(conn, "SELECT COUNT(*) FROM energy_statistics")
    importer.import_all(BUILD_DIR, db_path, date="2099-01-02")
    n_cross = emsdb.query_value(conn, "SELECT COUNT(*) FROM energy_statistics")
    t.eq(n_cross, n_before, "跨日重导不堆积 energy_statistics 行")
    t.eq(emsdb.query_value(
        conn, "SELECT COUNT(DISTINCT date) FROM energy_statistics"), 1,
        "跨日重导后只剩一个运行日")
    importer.import_all(BUILD_DIR, db_path)          # 恢复为正常口径
    t.eq(emsdb.query_value(
        conn, "SELECT COUNT(*) FROM energy_statistics"), n_before,
        "恢复正常导入后行数不变")

    # =================================================================
    print("\n[E] 认证与权限")
    # =================================================================
    n_new = auth.ensure_default_users(conn)
    t.eq(n_new, 3, "首次建 3 个默认账号")
    t.eq(auth.ensure_default_users(conn), 0, "再次调用不再重复建账号")

    admin = auth.authenticate(conn, "admin", "admin123")
    t.ok(admin is not None, "admin 登录成功")
    t.eq(admin["role"], "admin", "admin 角色")
    t.ok(auth.authenticate(conn, "admin", "wrong-password") is None,
         "错误口令被拒")
    t.ok(auth.authenticate(conn, "admin", "") is None, "空口令被拒")
    t.ok(auth.authenticate(conn, "nosuchuser", "x") is None, "不存在的账号被拒")

    h1, s1, i1 = auth.hash_password("same-password")
    h2, s2, i2 = auth.hash_password("same-password")
    t.ok(h1 != h2, "相同口令两次哈希不同（盐随机）")
    t.eq((s1 == s2), False, "两次盐不同")
    t.ok(auth.verify_password("same-password", h1, s1, i1), "哈希可验证")
    t.ok(not auth.verify_password("other-password", h1, s1, i1),
         "错误口令验证失败")

    tok = auth.issue_token(conn, admin["user_id"])
    t.ok(len(tok) >= 24, "token 长度足够")
    u = auth.resolve_token(conn, tok)
    t.ok(u is not None and u["username"] == "admin", "token 可解析")
    t.ok(auth.resolve_token(conn, "garbage-token") is None, "伪造 token 被拒")
    auth.revoke_token(conn, tok)
    t.ok(auth.resolve_token(conn, tok) is None, "撤销后 token 失效")

    # 过期会话
    tok2 = auth.issue_token(conn, admin["user_id"])
    past = (datetime.datetime.now() - datetime.timedelta(hours=1)).strftime(
        "%Y-%m-%d %H:%M:%S")
    conn.execute("UPDATE session SET expires_at = ? WHERE token = ?", (past, tok2))
    conn.commit()
    t.ok(auth.resolve_token(conn, tok2) is None, "过期 token 被拒")

    t.ok(auth.has_role(admin, "admin"), "admin 满足 admin")
    t.ok(auth.has_role(admin, "operator"), "admin 满足 operator")
    viewer_u = auth.authenticate(conn, "viewer", "viewer123")
    operator_u = auth.authenticate(conn, "operator", "operator123")
    t.ok(not auth.has_role(viewer_u, "operator"), "viewer 不满足 operator")
    t.ok(auth.has_role(operator_u, "viewer"), "operator 满足 viewer")
    t.ok(not auth.has_role(None, "viewer"), "未登录不满足任何角色")
    ok, why = auth.require(None, "viewer")
    t.ok(not ok and why, "未登录给出拒绝原因")
    ok2, why2 = auth.require(viewer_u, "operator")
    t.ok(not ok2 and "权限不足" in why2, "越权给出可读原因")

    tok_admin = auth.issue_token(conn, admin["user_id"])
    tok_viewer = auth.issue_token(conn, viewer_u["user_id"])
    tok_operator = auth.issue_token(conn, operator_u["user_id"])

    # =================================================================
    print("\n[F] REST 接口")
    # =================================================================
    def call(method, path, body=None, user=None, **query):
        q = {k: [str(v)] for k, v in query.items()}
        return api.dispatch(conn, method, path, q, body or {}, user)

    st, p = call("GET", "/api/health")
    t.eq(st, 200, "GET /api/health 状态码")
    t.eq(p["module"], "14/", "health.module")
    t.ok(p["scenarios"] and len(p["scenarios"]) == 2, "health 报出两个场景")

    # ---- 引擎模式：本段断言看的是**仿真数据集**，所以先把引擎切到 sim ----
    # ★ v1.4 起，"不带 ?scenario= 的请求看哪个数据集"由引擎模式决定
    #   （engine.default_scenario）：run → live；sim → 最近一次成功仿真。
    #   这正是双模式的核心语义，不能靠"默认 normal"蒙过去。
    st, pe = call("GET", "/api/engine", user=viewer_u)
    t.eq(st, 200, "GET /api/engine 200")
    t.eq(pe["engine"], "run", "新库默认引擎模式 = run（运行模式）")
    t.ok(isinstance(pe.get("sources"), list) and len(pe["sources"]) >= 1,
         "engine 报出数据源清单")
    t.ok(all(("available" in s and "why" in s) for s in pe["sources"]),
         "每个数据源都如实标注 available / why")
    t.eq(engine.default_scenario(conn), "live", "运行模式默认数据集 = live")
    st, p404 = call("GET", "/api/overview", user=viewer_u)
    t.eq(st, 404, "运行模式且 live 无数据时 overview 报 404")
    t.ok("启动引擎" in (p404.get("error") or ""),
         "404 文案给出可操作指引（去启动引擎）")

    st, pm = call("POST", "/api/engine", {"engine": "sim"}, user=operator_u)
    t.eq(st, 200, "POST /api/engine 切换模式 200")
    t.eq(pm["engine"], "sim", "切换到仿真模式")
    t.eq(pm["previous"], "run", "返回上一个模式（审计用）")
    st, _ = call("POST", "/api/engine", {"engine": "nope"}, user=operator_u)
    t.eq(st, 400, "未知引擎模式返回 400")
    st, _ = call("POST", "/api/engine", {"engine": "run"}, user=viewer_u)
    t.eq(st, 403, "viewer 不能切引擎模式")
    # 切到仿真模式后，没有成功仿真记录 → 退回内置场景（不是悄悄换成 live）
    t.eq(engine.default_scenario(conn), "normal", "仿真模式无仿真记录时退回内置 normal")

    st, p = call("POST", "/api/login",
                 {"username": "admin", "password": "admin123"})
    t.eq(st, 200, "POST /api/login 成功")
    t.ok("token" in p and p["user"]["role"] == "admin", "login 返回 token 与角色")
    st, p = call("POST", "/api/login",
                 {"username": "admin", "password": "bad"})
    t.eq(st, 401, "POST /api/login 失败返回 401")
    st, p = call("POST", "/api/login", {"username": "admin"})
    t.eq(st, 400, "POST /api/login 缺字段返回 400")

    st, p = call("GET", "/api/overview", user=viewer_u)
    t.eq(st, 200, "GET /api/overview 200")
    for k in ("power", "battery", "economics", "invariants", "loop", "alarms"):
        t.ok(k in p, f"overview 含字段 {k}")
    for k in ("grid_kw", "pv_kw", "load_kw", "battery_kw", "command_kw"):
        t.ok(k in p["power"], f"overview.power 含 {k}")
    t.ok(isinstance(p["battery"]["soc"], float), "overview SOC 是数值")
    t.ok(0.0 <= p["battery"]["soc"] <= 1.0, "overview SOC 在 0..1")
    t.close(p["economics"]["net_benefit_cny"], base["economics"]["net_benefit_cny"],
            "overview 净收益与 summary 一致", tol=0.01)
    t.eq(p["scenario"], "normal", "overview 默认场景 normal")

    st, p2 = call("GET", "/api/overview", user=viewer_u, scenario="fault")
    t.eq(st, 200, "overview 可切场景 fault")
    t.ok(p2["alarms"]["total"] > p["alarms"]["total"],
         "fault 场景告警多于 normal")

    st, p3 = call("GET", "/api/overview", user=viewer_u, t=3600)
    t.eq(st, 200, "overview 支持指定时刻 t")
    t.ok(p3["t_s"] <= 3600, "指定时刻不越界")

    st, p = call("GET", "/api/series", user=viewer_u,
                 keys="p_grid_kw,soc,p_cmd_kw", points=96)
    t.eq(st, 200, "GET /api/series 200")
    t.eq(p["points"], 96, "series 点数 = 请求值")
    for k in ("p_grid_kw", "soc", "p_cmd_kw"):
        t.eq(len(p["series"][k]), p["points"], f"series.{k} 长度一致")
    t.eq(len(p["t_s"]), p["points"], "series 时标长度一致")
    t.ok(p["t_s"] == sorted(p["t_s"]), "series 时标单调递增")
    st, p = call("GET", "/api/series", user=viewer_u, keys="no_such_column")
    t.eq(st, 400, "series 非法字段返回 400")
    st, p = call("GET", "/api/series", user=viewer_u, points=1)
    t.eq(p["points"], 2, "series 点数下界被夹到 2")

    st, p = call("GET", "/api/steps", user=viewer_u, limit=50, offset=0)
    t.eq(st, 200, "GET /api/steps 200")
    t.eq(p["total"], 8640, "steps 总数")
    t.eq(len(p["items"]), 50, "steps 分页生效")
    st, p = call("GET", "/api/steps", user=viewer_u, limit=99999)
    t.ok(p["limit"] <= 5000, "steps limit 被夹到上限")

    st, p = call("GET", "/api/commands", user=viewer_u)
    t.eq(st, 200, "GET /api/commands 200")
    t.ok(p["by_result"], f"commands 有 result 分布: {p['by_result']}")
    st, p = call("GET", "/api/commands", user=viewer_u, result="ok")
    t.ok(all(i["result"] == "ok" for i in p["items"]), "commands 可按 result 过滤")

    st, p = call("GET", "/api/alarms", user=viewer_u, scenario="fault")
    t.eq(st, 200, "GET /api/alarms 200")
    t.eq(p["total"], fault_sum["alarms"]["total"], "alarms 总数与 summary 相符")
    st, p = call("GET", "/api/alarms/summary", user=viewer_u, scenario="fault")
    t.eq(st, 200, "GET /api/alarms/summary 200")
    t.ok(p["by_level"] and p["by_source"], "alarms/summary 有分档统计")
    t.eq(sum(r["n"] for r in p["by_level"]), fault_sum["alarms"]["total"],
         "by_level 合计 = 告警总数")
    t.ok(1 <= len(p["timeline"]) <= 24, "alarms 时间线小时数在 1..24")
    t.ok(all(0 <= r["hour"] <= 23 for r in p["timeline"]),
         "时间线小时值落在 0..23")

    st, p = call("GET", "/api/devices", user=viewer_u)
    t.eq(st, 200, "GET /api/devices 200")
    t.eq(p["count"], 6, "devices 数量")
    st, p = call("GET", "/api/devices/DEV-PCS-01", user=viewer_u)
    t.eq(st, 200, "GET /api/devices/{id} 200")
    t.eq(p["device"]["device_type"], "PCS", "设备类型正确")
    st, p = call("GET", "/api/devices/NOPE", user=viewer_u)
    t.eq(st, 404, "不存在的设备返回 404")

    st, p = call("GET", "/api/realtime", user=viewer_u)
    t.eq(st, 200, "GET /api/realtime 200")
    t.eq(len(p["items"]), 5, "realtime 覆盖 5 台设备")
    t.ok(all("quality_ok" in i for i in p["items"]), "realtime 每项带品质位")

    st, p = call("GET", "/api/strategies", user=viewer_u)
    t.eq(st, 200, "GET /api/strategies 200")
    t.eq(p["count"], 9, "strategies 数量")
    prios = sorted({i["priority"] for i in p["items"]})
    t.eq(prios, ["L0", "L1", "L2", "L3"], "策略优先级覆盖 L0~L3")
    t.ok(all(isinstance(i["parameters"], dict) for i in p["items"]),
         "策略 parameters 已解析为对象")

    st, p = call("GET", "/api/strategies", user=None)
    t.eq(st, 401, "未登录读接口返回 401")
    st, p = call("PUT", "/api/strategies/S05_ANTI_REVERSE",
                 {"enabled": False}, user=viewer_u)
    t.eq(st, 403, "viewer 改策略被拒（403）")
    st, p = call("PUT", "/api/strategies/S05_ANTI_REVERSE",
                 {"enabled": False}, user=operator_u)
    t.eq(st, 200, "operator 改策略放行")
    t.eq(p["strategy"]["enabled"], 0, "策略已置为停用")
    st, p = call("PUT", "/api/strategies/S05_ANTI_REVERSE",
                 {"enabled": True}, user=operator_u)
    t.eq(p["strategy"]["enabled"], 1, "策略可恢复启用")
    st, p = call("PUT", "/api/strategies/NOPE", {"enabled": True},
                 user=operator_u)
    t.eq(st, 404, "改不存在的策略返回 404")
    st, p = call("PUT", "/api/strategies/S05_ANTI_REVERSE",
                 {"priority": "L9"}, user=operator_u)
    t.eq(st, 400, "非法优先级返回 400")
    st, p = call("PUT", "/api/strategies/S05_ANTI_REVERSE", {}, user=operator_u)
    t.eq(st, 400, "空更新返回 400")

    # ---- 运行模式（schema v1.1 新增） ----
    st, p = call("GET", "/api/modes", user=None)
    t.eq(st, 401, "未登录读 modes 返回 401")
    st, p = call("GET", "/api/modes", user=viewer_u)
    t.eq(st, 200, "GET /api/modes 200")
    t.eq(p["count"], 4, "四种运行模式")
    ids = {m["mode_id"] for m in p["items"]}
    t.eq(ids, {"arbitrage", "peak_shaving", "ancillary", "backup"},
         "模式 ID 齐全")
    t.ok(p["active"] in ids, "active 指向存在的模式")
    for m in p["items"]:
        t.ok(isinstance(m["bound_strategies"], list),
             f"{m['mode_id']} bound_strategies 已反序列化")
        t.ok(isinstance(m["constraints"], list),
             f"{m['mode_id']} constraints 已反序列化")

    # 切换：削峰 → S04/S05 启用、S07/S09 停用；安全底座恒启用
    st, p = call("POST", "/api/modes/activate", {"mode_id": "peak_shaving"},
                 user=operator_u)
    t.eq(st, 200, "operator 切削峰模式放行")
    en = set(p["enabled_strategies"])
    t.ok({"S04_DEMAND_MGMT", "S05_ANTI_REVERSE"} <= en, "削峰绑定策略已启用")
    t.ok(not ({"S07_PEAK_VALLEY", "S09_DEMAND_RESPONSE"} & en),
         "非削峰业务策略已停用")
    t.ok({"S01_BMS_FORBID", "S02_BMS_DERATE", "S03_TRANSFORMER_LIMIT",
          "S06_PV_SMOOTHING"} <= en, "安全底座恒启用")
    st, p = call("POST", "/api/modes/activate", {"mode_id": "peak_shaving"},
                 user=viewer_u)
    t.eq(st, 403, "viewer 切模式被拒")
    st, p = call("POST", "/api/modes/activate", {"mode_id": "nope"},
                 user=operator_u)
    t.eq(st, 404, "切不存在的模式返回 404")

    # overview 带当前模式；切回默认套利
    st, p = call("GET", "/api/overview", user=viewer_u)
    t.eq(p["mode"]["mode_id"], "peak_shaving", "overview 反映当前模式")
    st, p = call("POST", "/api/modes/activate", {"mode_id": "arbitrage"},
                 user=operator_u)
    t.eq(st, 200, "切回默认套利模式")
    t.ok("S07_PEAK_VALLEY" in p["enabled_strategies"], "套利策略已恢复")

    st, p = call("GET", "/api/economics", user=viewer_u)
    t.eq(st, 200, "GET /api/economics 200")
    t.eq(len(p["items"]), 2, "economics 两场景")
    st, p = call("GET", "/api/energy", user=viewer_u)
    t.eq(st, 200, "GET /api/energy 200")
    t.eq(len(p["items"]), 2, "energy_statistics 两条")
    t.ok(all(i["charge_kwh"] > 0 for i in p["items"]), "能量统计含充电量")

    st, p = call("GET", "/api/report", user=viewer_u, period="day")
    t.eq(st, 200, "GET /api/report?period=day 200")
    for k in ("aggregate", "daily", "economics", "invariants",
              "alarms_by_level", "commands_by_result"):
        t.ok(k in p, f"report 含 {k}")
    t.ok(p["data_note"], "report 显式标注数据边界")
    st, p = call("GET", "/api/report", user=viewer_u, period="year")
    t.eq(st, 400, "report 非法 period 返回 400")

    st, p = call("GET", "/api/invariants", user=viewer_u)
    t.eq(st, 200, "GET /api/invariants 200")
    t.eq(len(p["items"]), 2, "invariants 两场景")

    st, p = call("POST", "/api/control/command", {"target_power_kw": 100},
                 user=operator_u)
    t.eq(st, 200, "POST /api/control/command 200")
    t.eq(p["accepted"], False, "无执行器时**不假装下发成功**")
    t.eq(p["executor"], "none", "executor 标为 none")
    st, p = call("POST", "/api/control/command", {"target_power_kw": 100},
                 user=viewer_u)
    t.eq(st, 403, "viewer 下发指令被拒")
    st, p = call("POST", "/api/control/command", {}, user=operator_u)
    t.eq(st, 400, "缺参数返回 400")

    st, p = call("GET", "/api/users", user=viewer_u)
    t.eq(st, 403, "viewer 读用户列表被拒")
    st, p = call("GET", "/api/users", user=admin)
    t.eq(st, 200, "admin 可读用户列表")
    t.eq(len(p["items"]), 3, "用户数 3")
    t.ok(all("password_hash" not in i for i in p["items"]),
         "用户列表**不泄露口令哈希**")
    st, p = call("POST", "/api/users",
                 {"username": "t_ops", "password": "t_ops_123",
                  "role": "operator"}, user=admin)
    t.eq(st, 200, "admin 可建用户")
    st, p = call("POST", "/api/users",
                 {"username": "t_ops", "password": "t_ops_123"}, user=admin)
    t.eq(st, 400, "重复用户名返回 400")
    st, p = call("POST", "/api/users",
                 {"username": "t_short", "password": "123"}, user=admin)
    t.eq(st, 400, "口令过短返回 400")

    # 审计要覆盖登录 / 改策略 / 下发 / 导出，所以先触发一次导出
    call("GET", "/api/export/alarms", user=viewer_u)

    st, p = call("GET", "/api/audit", user=admin)
    t.eq(st, 200, "GET /api/audit 200")
    actions = {i["action"] for i in p["items"]}
    t.ok("login" in actions, "审计记录了登录")
    t.ok("command_send" in actions, "审计记录了指令下发意图")
    t.ok("strategy_update" in actions, "审计记录了策略变更")
    t.ok("export" in actions, "审计记录了导出")
    st, p = call("GET", "/api/audit", user=viewer_u)
    t.eq(st, 403, "viewer 读审计被拒")

    st, p = call("GET", "/api/export/timeseries", user=viewer_u)
    t.eq(st, 200, "导出时序 200")
    t.ok(p.get("__raw__") and "t_s," in p["body"], "导出为 CSV 且含表头")
    t.eq(len(p["body"].strip().splitlines()), 8641, "导出行数 = 8640 + 表头")
    st, p = call("GET", "/api/export/nope", user=viewer_u)
    t.eq(st, 400, "非法导出类型返回 400")

    st, p = call("GET", "/api/no-such-endpoint", user=viewer_u)
    t.eq(st, 404, "未知接口返回 404")

    # 未知方法
    st, p = api.dispatch(conn, "DELETE", "/api/overview", {}, {}, viewer_u)
    t.eq(st, 404, "未注册的方法返回 404")

    # =================================================================
    print("\n[G] 设备接入闭环（点表 / 接入参数 / 落盘文件）")
    # =================================================================
    # ★ 为什么这一段单独成节：
    #   A~F 全在 14/ 自己的进程里自证 —— 自己写、自己读、自己断言。
    #   而本节的核心判据是**跨进程、跨语言**的：「14/ 写出去的那个 active.csv，
    #   13/ 的 C++ 进程能不能读，读到的字段是否就是我们以为写下去的那张表」。
    #   对端用 13/build/modbus_probe.exe，否则仍旧是自己和自己对答案。
    #
    # 隔离：把点表目录指到临时目录。**必须**做，否则本自检会覆盖现场真表
    #   （config/point-map/active.csv 是 13/ 现场启动时真正会读的文件）。
    old_map_dir = os.environ.get(pointmap.ENV_MAP_DIR)
    pmdir = os.path.join(tmpdir, "point-map")
    os.environ[pointmap.ENV_MAP_DIR] = pmdir
    t.eq(pointmap.map_dir(), pmdir,
         "EMS_POINT_MAP_DIR 生效（点表目录被隔离到临时目录）")
    t.ok(not os.path.isdir(pmdir) or not os.listdir(pmdir),
         "隔离生效：临时点表目录初始为空")

    # ---- G1. 基线与 13/ 同一份真相源 ----
    #
    # _E：把"求值"和"断言"分开。任何一步抛异常都应当记成一个 FAIL 计数，
    # **不能**让整场自检在中间崩掉 —— 否则后面的判据一个都跑不到，
    # 排查时看到的只有一条 traceback，反而比全部跑完再汇总难定位得多。
    def _E(fn):
        try:
            return fn()
        except Exception as e:          # noqa: BLE001
            print(f"  [EXC ] {type(e).__name__}: {e}")
            return None

    base_rows = pointmap.normalize(pointmap.read_baseline())
    t.eq(pointmap.row_count(), 32, "基线 32 点（与 13/ 的 EMS_EXT_BEGIN 一致）")
    t.eq(pointmap.command_span(), (7, 9), "指令点连续段 = (7, 9)")
    t.eq(len(base_rows), 32, "基线解析出 32 行")
    sm = pointmap.summary(base_rows)
    t.eq(sm["requests"], 3, "基线全表扫描 3 次请求（IR / HR / DI 各一块）")
    t.ok(sm["ok"], "基线自身通过校验")

    # ---- G2. normalize 幂等（这一条曾经是真 bug） ----
    #
    # 同一个 normalize() 要同时吃三种来源，它们的字段类型不同：
    #   ① CSV 文本    全是字符串       ② 前端 JSON  数字 + 布尔
    #   ③ 数据库行    INTEGER / REAL / 0-1
    # 早期版本一律 str() 硬转，于是把已归一化的行再喂回去时
    # str(False) → 'False'、str(2) 之外还丢语义，表现是**基线表自己都过不了校验**。
    n1 = pointmap.normalize(base_rows)
    t.eq(_E(lambda: pointmap.normalize(n1)), n1, "幂等：再归一化一次结果不变")
    t.eq(_E(lambda: pointmap.normalize(pointmap.parse_text(
        pointmap.data_csv_text(n1)))), n1, "往返一致：行 → CSV 文本 → 行")
    t.eq(_E(lambda: pointmap.normalize(json.loads(json.dumps(n1)))), n1,
         "往返一致：行 → JSON（数字/布尔）→ 行")

    # ---- G3. 校验规则镜像 13/ 的 validate_map() ----
    def _patched(idx: int, **kw) -> list[dict]:
        out = [dict(r) for r in base_rows]
        out[idx].update(kw)
        return out

    # 正控：先证明"合法改动是能过的"，否则下面"全都拒了"可能只是判据一律拒绝
    t.ok(_E(lambda: pointmap.validate(pointmap.normalize(_patched(4, scale=5000.0))))
         is not None, "正控：合法的 scale 改动通过校验")

    bad_cases: list[tuple[str, list[dict]]] = [
        ("点名被改（行序即索引）", _patched(0, name="MEAS.WRONG")),
        ("表类型非法", _patched(0, table="XX")),
        ("编码非法", _patched(1, encoding="f64")),
        ("u16 的 scale 为 0（换算会除零）", _patched(4, scale=0.0)),
        ("i16 的 scale 为负", _patched(5, scale=-1.0)),
        ("bit 的 scale 非 0", _patched(24, scale=1.0)),
        ("只读段标成可写", _patched(0, writable=True)),
        ("指令点标成只读", _patched(7, writable=False)),
        ("指令点地址不连续", _patched(9, address=6)),
        ("点位地址重叠", _patched(4, address=7)),
        ("行数少一行", base_rows[:-1]),
    ]
    for label, rows_bad in bad_cases:
        try:
            pointmap.validate(pointmap.normalize(rows_bad))
            t.ok(False, f"非法表被拒：{label}")
        except pointmap.PointMapError:
            t.ok(True, f"非法表被拒：{label}")
        except Exception as e:          # noqa: BLE001
            t.ok(False, f"非法表被拒：{label}（异常类型不对：{type(e).__name__}）")

    # ---- G4. 落盘文件判据（13/ 真正读的就是这个文件） ----
    target = pointmap.device_path("DEV-PCS-01")
    act = pointmap.active_path()
    t.eq(target, os.path.join(pmdir, "DEV-PCS-01.csv"), "设备点表名 = <id>.csv")
    t.eq(act, os.path.join(pmdir, "active.csv"), "活动表名 = active.csv")

    pointmap.save("DEV-PCS-01", base_rows, {"device_id": "DEV-PCS-01"})
    t.ok(os.path.isfile(target), "保存后 <id>.csv 存在")
    t.ok(os.path.isfile(act), "保存后 active.csv 存在")
    raw_id = open(target, "rb").read()
    raw_act = open(act, "rb").read()
    t.eq(raw_act, raw_id, "active.csv 与 <id>.csv 逐字节相同（同一份内容）")
    t.ok(raw_act.startswith(b"\xef\xbb\xbf"), "文件以 UTF-8 BOM 开头（Excel 不乱码）")
    t.eq(raw_act[3:4], b"#", "BOM 之后紧跟注释头")
    t.ok(b"\r\n" in raw_act, "行尾为 CRLF（Excel 友好）")
    t.eq(_E(lambda: pointmap.normalize(pointmap.load_file(act))), base_rows,
         "load_file 读回并归一化后 == 写下去的行")

    # ---- G5. 保存（走接口）→ 库与文件双侧落定 ----
    st, dv = call("GET", "/api/devices", user=viewer_u)
    other_id = next(i["device_id"] for i in dv["items"]
                    if i["device_id"] != "DEV-PCS-01")

    rows_a = _patched(4, scale=100.0)          # SOC（u16，scale 必须 > 0）
    st, p = call("PUT", "/api/devices/DEV-PCS-01/point-map", {"rows": rows_a},
                 user=operator_u)
    t.eq(st, 200, "PUT point-map 保存成功")
    t.eq(p["rows"], 32, "返回保存行数 32")

    st, p = call("GET", "/api/devices/DEV-PCS-01/point-map", user=viewer_u)
    t.eq(st, 200, "GET point-map 200")
    t.eq(p["source"], "db", "点表来源 = db（该设备已配置）")
    t.eq(p["configured"], True, "configured = True")
    t.eq(p["synced"], True, "synced = True（盘上文件与库内逐字段一致）")
    t.close(p["rows"][4]["scale"], 100.0, "回读 SOC scale = 100")
    t.eq(p["summary"]["requests"], 3, "改 scale 不影响分块数（仍 3 次请求）")
    t.eq(p["conn"]["point_map_rows"], 32, "device_conn.point_map_rows 已记录")

    st, p = call("GET", f"/api/devices/{other_id}/point-map", user=viewer_u)
    t.eq(st, 200, "未配置设备也能读 point-map")
    t.eq(p["source"], "baseline", "未配置设备的来源 = baseline")
    t.eq(p["configured"], False, "未配置设备 configured = False")
    t.eq(p["file_exists"], False, "未配置设备没有落盘文件")

    # ---- G6. 导出 CSV：能当模板，也能被自己读回 ----
    # 注意：查询串必须走 call(..., template=1)，不能拼进 path ——
    # dispatch 用 fullmatch 匹配路径，带上 "?..." 就匹配不上了。
    st, p = call("GET", "/api/devices/DEV-PCS-01/point-map.csv",
                 user=viewer_u, template=1)
    t.eq(st, 200, "导出模板 CSV 200")
    t.ok(p.get("__raw__"), "导出走 __raw__ 通道")
    t.ok(str(p.get("body", "")).startswith("\ufeff"), "导出体带 BOM")
    t.ok(str(p.get("content_type", "")).startswith("text/csv"),
         "content_type 为 text/csv")
    t.eq(len(_E(lambda: pointmap.normalize(
        pointmap.parse_text(p.get("body", "")))) or []), 32,
        "导出的模板 CSV 能被自己解析回 32 行")
    st, p2 = call("GET", "/api/devices/DEV-PCS-01/point-map.csv", user=viewer_u)
    t.eq(st, 200, "导出当前表 200")
    _exp = _E(lambda: pointmap.normalize(pointmap.parse_text(p2.get("body", ""))))
    t.ok(bool(_exp) and len(_exp) > 4 and abs(_exp[4]["scale"] - 100.0) < 1e-9,
         "导出当前表反映已保存的 scale=100")

    # ---- G7. 回滚：注入库写入故障，文件与库必须**一起**退回 ----
    #
    # 半成功会产生「界面说保存成功、现场还是旧表」—— 这种不一致比直接报错难查得多。
    rows_b = _patched(4, scale=200.0)
    keep_bytes = open(act, "rb").read()
    _orig_executemany = emsdb.executemany

    def _boom(*_a, **_k):
        raise RuntimeError("注入的故障：模拟库写入失败")

    emsdb.executemany = _boom
    try:
        st, p = call("PUT", "/api/devices/DEV-PCS-01/point-map", {"rows": rows_b},
                     user=operator_u)
    finally:
        emsdb.executemany = _orig_executemany
    t.eq(st, 500, "库写入失败时 PUT 返回 500（不谎报成功）")
    t.eq(open(act, "rb").read(), keep_bytes,
         "回滚后 active.csv 逐字节未变（文件也退回了）")
    db4 = emsdb.query_one(conn,
                          "SELECT * FROM device_point_map WHERE device_id=? AND idx=?",
                          ("DEV-PCS-01", 4))
    t.close(_E(lambda: db4["scale"]), 100.0,
            "回滚后库内 SOC scale 仍是 100（未被半改）")

    # 非法表提交：两侧都不该动
    st, p = call("PUT", "/api/devices/DEV-PCS-01/point-map",
                 {"rows": _patched(0, name="MEAS.WRONG")}, user=operator_u)
    t.eq(st, 400, "非法点表返回 400")
    t.eq(open(act, "rb").read(), keep_bytes, "非法表被拒后文件未变")
    _r = emsdb.query_one(
        conn, "SELECT scale FROM device_point_map WHERE device_id=? AND idx=?",
        ("DEV-PCS-01", 4))
    t.close(_E(lambda: _r["scale"]), 100.0, "非法表被拒后库内未变")
    st, p = call("PUT", "/api/devices/DEV-PCS-01/point-map", {}, user=operator_u)
    t.eq(st, 400, "既无 rows 也无 csv 返回 400")

    # ---- G8. 接入参数 ----
    st, p = call("PUT", "/api/devices/DEV-PCS-01/conn",
                 {"host": "192.168.1.20", "port": 502, "unit_id": 3}, user=operator_u)
    t.eq(st, 200, "PUT conn 保存成功")
    t.eq(p["conn"]["host"], "192.168.1.20", "host 已存")
    t.eq(p["conn"]["enabled"], 1, "填了地址即视为启用（现场直觉）")
    st, p = call("GET", "/api/devices/DEV-PCS-01/point-map", user=viewer_u)
    t.eq(p["conn"]["unit_id"], 3, "接入参数随点表接口一起返回")

    for label, body in [
        ("host 含非法字符", {"host": "192.168.1.20; rm -rf /"}),
        ("端口越界", {"port": 70000}),
        ("从站号 0（广播地址）", {"unit_id": 0}),
        ("从站号 248（超过 247）", {"unit_id": 248}),
        ("轮询周期过小", {"poll_period_ms": 1}),
        ("端口非整数", {"port": "abc"}),
    ]:
        st, p = call("PUT", "/api/devices/DEV-PCS-01/conn", body, user=operator_u)
        t.eq(st, 400, f"接入参数被拒：{label}")
    st, p = call("PUT", "/api/devices/NOPE/conn", {"host": "1.1.1.1"},
                 user=operator_u)
    t.eq(st, 404, "给不存在的设备存接入参数返回 404")

    # ---- G9. 新增设备台账（没有它，界面上没有可配置的设备） ----
    st, p = call("POST", "/api/devices",
                 {"device_id": "DEV-T-GATEWAY", "device_type": "GATEWAY"},
                 user=operator_u)
    t.eq(st, 201, "新增设备返回 201")
    st, p = call("POST", "/api/devices",
                 {"device_id": "DEV-T-GATEWAY", "device_type": "GATEWAY"},
                 user=operator_u)
    t.eq(st, 409, "重复设备号返回 409")
    st, p = call("POST", "/api/devices",
                 {"device_id": "bad id!", "device_type": "PCS"}, user=operator_u)
    t.eq(st, 400, "非法设备号返回 400")
    st, p = call("POST", "/api/devices",
                 {"device_id": "DEV-T-X", "device_type": "NOPE"}, user=operator_u)
    t.eq(st, 400, "非法设备类型返回 400")

    # ---- G10. 权限 ----
    st, p = call("PUT", "/api/devices/DEV-PCS-01/point-map", {"rows": base_rows},
                 user=viewer_u)
    t.eq(st, 403, "viewer 存点表被拒")
    st, p = call("PUT", "/api/devices/DEV-PCS-01/conn", {"host": "1.1.1.1"},
                 user=viewer_u)
    t.eq(st, 403, "viewer 存接入参数被拒")
    st, p = call("GET", "/api/devices/DEV-PCS-01/point-map", user=None)
    t.eq(st, 401, "未登录读点表返回 401")

    # ---- G11. ★ 跨语言判据：13/ 的 C++ 进程读平台写出的文件 ----
    #
    # 这是本节唯一无法在 Python 里自证的判据。它验的是配置通道①的**接头**：
    #   14/ 按约定路径写出 active.csv  →  13/ 启动时按同一约定读它
    # 两侧是两个语言、两个进程、两套解析代码。任何一侧的格式/列序/编码理解
    # 有偏差，都会在这里露出来（而 13/ 读错一个字段在运行期表现为"数字正常但
    # 含义错了"，是最难查的一类故障）。
    probe = os.path.join(PROJECT_ROOT, "13", "build", "modbus_probe.exe")
    if not os.path.isfile(probe):
        t.skip(f"缺 {probe} —— 先跑 13/scripts/build.bat")
    else:
        def _run_probe(*args) -> subprocess.CompletedProcess:
            env = dict(os.environ)
            env[pointmap.ENV_MAP_DIR] = pmdir      # 与平台同一个约定目录
            return subprocess.run([probe, *args], capture_output=True, text=True,
                                  cwd=os.path.join(PROJECT_ROOT, "13"),
                                  env=env, timeout=120)

        def _data_lines(out: str) -> list[str]:
            """取 probe 输出里的 CSV 数据段（表头行起）。"""
            lines = out.splitlines()
            for k, ln in enumerate(lines):
                if ln.startswith("name,"):
                    return [x for x in lines[k:] if x.strip()]
            return []

        def _cols(line: str) -> list[str]:
            return next(csv.reader(io.StringIO(line)))

        r = _run_probe("--dump-map")
        t.eq(r.returncode, 0, "probe 加载平台写的 active.csv 退出码 0")
        t.ok("约定路径" in r.stdout,
             "probe 报出的点表来源是「约定路径」（确实读了我们写的文件）")

        got = _data_lines(r.stdout)
        t.eq(len(got), 33, "probe 导出 33 行（表头 + 32 点）")
        # 只比前 7 列：unit / note 两侧都**刻意不解析** —— 13/ 的原话是
        # 「免得现场为了写一句备注还要翻译我们的术语」，所以它 --dump-map
        # 时这两列输出为空；模板文件里填的字纯给人看。
        want = pointmap.data_csv_text(rows_a).splitlines()
        mism = [k for k, (g, w) in enumerate(zip(got, want))
                if _cols(g)[:7] != _cols(w)[:7]]
        t.ok(not mism,
             f"probe 读到的 7 个字段与平台写下的逐字段一致（不一致行 {mism[:5]}）")
        if mism:
            k = mism[0]
            print(f"    第 {k} 行：probe={_cols(got[k])[:7]} 平台={_cols(want[k])[:7]}")

        # 反证：把 active.csv 改坏（第 2 个点名改错）→ probe **必须**失败。
        # 没有这一步，"probe 退出 0" 有可能只是因为那条判据根本没生效。
        bad_text = pointmap.data_csv_text(
            [dict(rows_a[0])] + [dict(rows_a[1], name="MEAS.WRONG")]
            + [dict(r) for r in rows_a[2:]])
        pointmap._atomic_write(act, b"\xef\xbb\xbf" + bad_text.encode("utf-8"))
        rb = _run_probe("--dump-map")
        t.ok(rb.returncode != 0, "反证：非法 active.csv 使 probe 退出码非 0")
        t.ok("MEAS.P_PV" in (rb.stdout + rb.stderr),
             "反证：失败信息指向被判为非法的点名")
        pointmap._atomic_write(act, keep_bytes)          # 复原
        t.eq(_run_probe("--dump-map").returncode, 0, "复原 active.csv 后 probe 恢复正常")

        # 反向也验一次：13/ 用 --load-map 显式读我们导出的文件
        csv_out = os.path.join(pmdir, "export_for_probe.csv")
        with open(csv_out, "wb") as f:
            f.write(b"\xef\xbb\xbf" + pointmap.to_csv_text(rows_a).encode("utf-8"))
        r2 = _run_probe("--load-map", csv_out, "--plan")
        t.eq(r2.returncode, 0, "probe --load-map 读平台导出的 CSV 退出码 0")
        t.ok("32 点" in r2.stdout, "probe 确认读到 32 点")

    # ---- G12. 配置通道②：接入参数落盘（端侧读的就是这个文件） ----
    #
    # 为什么单列一段：接入参数此前**只存在库里**，13/ 07/ 仍必须靠命令行
    # --host 才连得上 —— 界面上写的 IP 与现场敲的参数可能根本不是同一个，
    # 而两边都不报错。这一段守的就是「库与文件同源」。
    cpath = connconf.conn_path()
    t.eq(cpath, os.path.join(pmdir, "active.conn"),
         "接入参数文件名 = active.conn（与点表同目录）")

    st, p = call("PUT", "/api/devices/DEV-PCS-01/conn",
                 {"host": "192.168.1.20", "port": 502, "unit_id": 3}, user=operator_u)
    t.eq(st, 200, "PUT conn 200")
    t.ok((p.get("conn_file") or {}).get("path"), "响应里带 conn_file 落盘信息")
    t.ok(os.path.isfile(cpath), "PUT conn 后 active.conn 已落盘")

    on_disk = _E(lambda: connconf.load_file(cpath))
    t.eq(on_disk["host"], "192.168.1.20", "文件里的 host 与界面提交一致")
    t.eq(on_disk["unit_id"], 3, "文件里的从站号一致")
    t.eq(on_disk["enabled"], 1, "填了地址即启用 → 文件里 enabled=1")
    t.eq(on_disk["device_id"], "DEV-PCS-01", "文件里记录了设备号")
    t.eq(on_disk["format"], connconf.FORMAT_TAG, "文件带格式版本号")
    raw_c = open(cpath, "rb").read()
    t.ok(raw_c.startswith(b"\xef\xbb\xbf"), "接入参数文件带 BOM（Excel 不乱码）")
    t.ok(b"\r\n" in raw_c, "接入参数文件用 CRLF")
    t.ok(_E(lambda: connconf.normalize(connconf.load_file(cpath))) == on_disk,
         "解析 → 归一化是幂等的")

    # 界面回显：GET point-map 带 conn 文件状态
    st, p = call("GET", "/api/devices/DEV-PCS-01/point-map", user=viewer_u)
    t.eq(p["conn_file_exists"], True, "GET point-map 报出 conn 文件存在")
    t.eq(p["conn_synced"], True, "conn 文件与库内接入参数一致（synced）")

    # 幂等：同内容再存一次，除 saved_at 外逐字段不变
    call("PUT", "/api/devices/DEV-PCS-01/conn",
         {"host": "192.168.1.20", "port": 502, "unit_id": 3}, user=operator_u)
    again = _E(lambda: connconf.load_file(cpath))
    t.eq({k: v for k, v in again.items() if k != "saved_at"},
         {k: v for k, v in on_disk.items() if k != "saved_at"},
         "同内容重存后文件内容不变（saved_at 除外）")

    # 点表保存也会带上接入参数落盘（两者必须来自同一次保存）
    call("PUT", "/api/devices/DEV-PCS-01/point-map", {"rows": rows_a},
         user=operator_u)
    t.ok(os.path.isfile(cpath), "保存点表时接入参数也一并落盘")

    # ---- G12b. host 清空 → 旧文件必须删掉 ----
    st, p = call("PUT", "/api/devices/DEV-PCS-01/conn", {"host": ""},
                 user=operator_u)
    t.eq(st, 200, "清空 host 200")
    t.ok(not os.path.isfile(cpath), "清空 host 后 active.conn 被删除")
    st, p = call("GET", "/api/devices/DEV-PCS-01/point-map", user=viewer_u)
    t.eq(p["conn_file_exists"], False, "GET 报出 conn 文件已不存在")
    t.eq(p["conn_synced"], False, "conn_synced = False")

    # ---- G12c. 回滚：库写入故障时文件必须一起退回 ----
    st, p = call("PUT", "/api/devices/DEV-PCS-01/conn",
                 {"host": "10.0.0.9", "unit_id": 5}, user=operator_u)
    t.eq(st, 200, "重建 conn 文件（host=10.0.0.9）")
    keep_c = open(cpath, "rb").read()

    class _BoomConn:
        """只让 device_conn 的 INSERT 炸，别的照常 —— 精确模拟库写入失败。"""

        def __init__(self, real):
            self._real = real

        def execute(self, sql, *a, **k):
            # 只炸 INSERT，不炸 SELECT —— _conn_row 要读当前值，读也得能用
            if "INSERT INTO DEVICE_CONN" in sql.upper():
                raise RuntimeError("注入的故障：device_conn 写入失败")
            return self._real.execute(sql, *a, **k)

        def __getattr__(self, name):
            return getattr(self._real, name)

    real_conn = conn
    conn = _BoomConn(real_conn)
    try:
        st, p = call("PUT", "/api/devices/DEV-PCS-01/conn",
                     {"host": "10.0.0.99"}, user=operator_u)
    finally:
        conn = real_conn
    t.eq(st, 500, "库写入失败时 PUT conn 返回 500（不谎报成功）")
    t.eq(open(cpath, "rb").read(), keep_c,
         "回滚后 active.conn 逐字节未变（文件也退回了）")

    # ---- G12d. 解析器的严格性（拼错键名必须当场发现） ----
    good = _E(lambda: connconf.dumps(
        {"device_id": "DEV-X", "host": "1.1.1.1", "port": 502, "unit_id": 1}))
    base = good.replace("\r\n", "\n")
    for label, text in [
        ("键名拼错（hots=）", base + "hots=1.1.1.1\n"),
        ("不是 key=value", base + "这行没有等号\n"),
        ("缺必填键（删掉 host 行）",
         "\n".join(l for l in base.splitlines() if not l.startswith("host="))),
        ("格式版本不认识", base.replace(connconf.FORMAT_TAG, "ems-device-conn/9")),
        ("从站号越界", base.replace("unit_id=1", "unit_id=248")),
        ("端口越界", base.replace("port=502", "port=70000")),
    ]:
        try:
            connconf.parse(text)
            t.ok(False, f"接入参数被拒：{label}")
        except connconf.ConnConfError:
            t.ok(True, f"接入参数被拒：{label}")

    # ---- G13. ★ 跨语言判据：13/ 的 C++ 进程读平台写出的接入参数 ----
    #
    # 与 G11 同一个道理，守的是接入参数那一半：14/ 写出 active.conn，
    # 13/ 的 probe 启动时按同一约定读它。两个语言、两份解析代码 ——
    # 任何一侧对格式 / 键名 / 取值范围的理解有偏差都会在这里露出来。
    # 而"14 写 A、13 读 B"在现场的表现是"配置永远不生效"，两边却各自成功。
    if not os.path.isfile(probe):
        t.skip(f"缺 {probe} —— 先跑 13/scripts/build.bat")
    else:
        # 写一份有辨识度的接入参数（从站 7、端口 15099）
        st, p = call("PUT", "/api/devices/DEV-PCS-01/conn",
                     {"host": "10.11.12.13", "port": 15099, "unit_id": 7},
                     user=operator_u)
        t.eq(st, 200, "为跨语言判据写下接入参数")

        def _probe_conn(*args) -> subprocess.CompletedProcess:
            env = dict(os.environ)
            env[pointmap.ENV_MAP_DIR] = pmdir      # 与平台同一个约定目录
            return subprocess.run([probe, *args], capture_output=True, text=True,
                                  cwd=os.path.join(PROJECT_ROOT, "13"),
                                  env=env, timeout=120)

        r = _probe_conn("--dump-conn")
        t.eq(r.returncode, 0, "probe --dump-conn 退出码 0")
        t.ok("约定路径" in r.stdout,
             "probe 报出的接入参数来源是「约定路径」（确实读了平台写的文件）")
        t.ok("10.11.12.13:15099" in r.stdout, "probe 读到平台写的 host:port")
        t.ok("从站 7" in r.stdout, "probe 读到平台写的从站号")

        # 反证：把文件改坏（键名拼错）→ probe **必须**失败，且**不退回默认地址**。
        # 没有这一步，"probe 读到 10.11.12.13" 有可能只是因为默认值恰好如此。
        cpath13 = connconf.conn_path()
        keep_c13 = open(cpath13, "rb").read()
        bad_txt = keep_c13.decode("utf-8-sig").replace("unit_id=7", "unitid=7")
        pointmap._atomic_write(cpath13, b"\xef\xbb\xbf" + bad_txt.encode("utf-8"))
        rb = _probe_conn("--dump-conn")
        t.ok(rb.returncode != 0, "反证：非法 active.conn 使 probe 退出码非 0")
        t.ok("unitid" in (rb.stdout + rb.stderr),
             "反证：失败信息指向那个拼错的键名")
        pointmap._atomic_write(cpath13, keep_c13)
        t.eq(_probe_conn("--dump-conn").returncode, 0, "复原 active.conn 后 probe 恢复正常")

    # 还原环境变量，避免影响后续（以及同进程内的其他调用）
    if old_map_dir is None:
        os.environ.pop(pointmap.ENV_MAP_DIR, None)
    else:
        os.environ[pointmap.ENV_MAP_DIR] = old_map_dir

    # =================================================================
    print("\n[H] 旧库迁移（v1.1 → v1.2 → v1.3 → v1.4）")
    # =================================================================
    # ★ 为什么必须单独验：schema.sql 里全是 CREATE TABLE IF NOT EXISTS ——
    #   它会建出缺的两张**新表**，但**不会给已存在的表补列**。
    #   于是老库升上来时 scenario.time_base 一直缺着，且缺列不是在建库时炸，
    #   是在运行期第一次用到 time_base 时才炸（表现为一个 500）。
    #   兜住它的是 emsdb.MIGRATIONS 里的幂等 ALTER。
    old_db = os.path.join(tmpdir, "old_v11.db")
    oc = emsdb.connect(old_db)
    oc.execute("CREATE TABLE scenario (scenario_id TEXT PRIMARY KEY, steps INTEGER)")
    oc.execute("INSERT INTO scenario(scenario_id, steps) VALUES('normal', 8640)")
    oc.commit()
    t.ok("time_base" not in emsdb._table_columns(oc, "scenario"),
         "构造出的 v1.1 老库：scenario 缺 time_base")
    t.ok("device_conn" not in {r["name"] for r in
                               emsdb.query(oc, "SELECT name FROM sqlite_master"
                                               " WHERE type='table'")},
         "构造出的 v1.1 老库：还没有 device_conn 表")

    done = emsdb.migrate(oc)
    t.eq(len(done), 1, "迁移执行了 1 条 ALTER")
    t.ok("time_base" in emsdb._table_columns(oc, "scenario"),
         "迁移后 scenario 有 time_base")
    t.eq(emsdb.query_value(oc, "SELECT time_base FROM scenario WHERE scenario_id=?",
                           ("normal",)), "sim",
         "老数据自动带上默认值 'sim'（不丢、不改代码）")
    t.eq(emsdb.migrate(oc), [], "迁移幂等：再跑一次不再执行 ALTER")
    oc.close()

    oc2 = emsdb.init_db(old_db)             # 老库走一次正常启动路径
    t.eq(emsdb.get_meta(oc2, emsdb.META_SCHEMA_VERSION), "1.4",
         "老库经 init_db 后 meta 版本升到 1.4")
    cnt2 = emsdb.table_counts(oc2)
    t.ok(cnt2.get("device_conn", -1) >= 0, "老库经 init_db 后 device_conn 建出")
    t.ok(cnt2.get("device_point_map", -1) >= 0, "老库经 init_db 后 device_point_map 建出")
    t.ok(cnt2.get("sim_run", -1) >= 0, "老库经 init_db 后 sim_run 建出（v1.4）")
    t.eq(emsdb.query_value(oc2, "SELECT COUNT(*) FROM scenario"), 1,
         "老库原有数据未被动过")
    oc2.close()

    # ★ v1.4 的 sim_run 只是**新表**，没有需要 ALTER 的列 —— 所以 migrate()
    #   对老库依然只需要补 time_base 那一条。这条断言钉住"别为了加表
    #   顺手加一条无用的 ALTER"（无用的 ALTER 会重复执行/静默失败）。
    oc3 = emsdb.connect(old_db)
    t.eq(emsdb.migrate(oc3), [], "v1.4 老库迁移仍幂等（无多余 ALTER）")
    oc3.close()

    # =================================================================
    print("\n[I] 实时入库（07/ --record → live 场景，time_base=wall）")
    # =================================================================
    # ★ 为什么单独一节：实时数据与仿真数据是**两套时间轴**（墙钟 vs 一天内秒），
    #   若把墙钟秒按仿真口径（%86400）画曲线，跨日数据全错。本节锁死三条：
    #   ① time_base 显式 'wall' ② 增量幂等（不 DELETE 旧行）③ 台账兜底。
    live_csv = os.path.join(tmpdir, "live_record.csv")
    with open(live_csv, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t_s", "time", "state", "P_load_kW", "P_pv_kW", "P_grid_kW",
                    "P_cmd_kW", "P_actual_kW", "SOC", "T_C", "P_lower", "P_upper",
                    "plan_target", "correction", "clamped", "safety_clip",
                    "state_gated", "hold_last", "fault_bits", "reason"])
        base = 1_790_000_000.0
        for i in range(5):
            w.writerow([f"{base + i:.3f}", f"2026-09-28 08:{(i):02d}:00", "RUN",
                        "380", "150", "230", "-80", "-80", "0.55", "25",
                        "-200", "200", "0", "0", "0", "0", "0", "0", "0",
                        "ok"])

    r1 = importer.import_live_record(conn, live_csv, "live", "现场实录（live）")
    t.eq(r1["time_base"], "wall", "live 场景 time_base=wall")
    t.eq(r1["rows"], 5, "live 导入 5 行")
    tb = emsdb.query_one(conn, "SELECT time_base, steps FROM scenario WHERE scenario_id='live'")
    t.eq(tb["time_base"], "wall", "库内 scenario.time_base=wall")
    t.eq(tb["steps"], 5, "库内 steps 回填为累计行数")
    row = emsdb.query_one(conn, "SELECT t_s, time_str FROM step_record WHERE scenario_id='live' ORDER BY t_s LIMIT 1")
    t.ok(row["t_s"] > 1e9, "t_s 是 Unix 墙钟秒（>1e9），不是一天内秒")
    t.eq(row["time_str"], "2026-09-28 08:00:00", "time_str 是完整日期时间")

    # 增量幂等：再导一次，行数不翻倍（主键 (scenario_id,t_s) 覆盖）
    importer.import_live_record(conn, live_csv, "live", "现场实录（live）")
    t.eq(emsdb.query_value(conn, "SELECT COUNT(*) FROM step_record WHERE scenario_id='live'", (), 0),
         5, "增量幂等：重复导入不翻倍")

    # 台账兜底：live 导入前 device 表为空时自动补 DEV-PCS-01（外键）
    t.eq(emsdb.query_value(conn, "SELECT COUNT(*) FROM device WHERE device_id='DEV-PCS-01'", (), 0),
         1, "台账兜底：DEV-PCS-01 已建（外键不炸）")

    # scenario_of 接受 live，非法回退 normal
    t.eq(api.scenario_of({"scenario": ["live"]}), "live", "scenario_of 接受 live 场景")
    t.eq(api.scenario_of({"scenario": ["bad;id"]}), "normal", "非法 scenario_id 回退 normal")

    # h_series 走 wall 口径：time 是完整日期时间，不是 HH:MM
    st, body = api.h_series(conn, {"scenario": ["live"]}, {}, _body(), {})
    t.eq(st, 200, "live 场景 h_series 200")
    t.eq(body.get("time_base"), "wall", "h_series 标注 time_base=wall")
    t.ok(body["time"] and "2026-" in str(body["time"][0]),
         "wall 口径 time 是完整日期时间（含年份）")

    # 反证：口径分支真的在生效（把 time_base 当 sim 会得到 HH:MM 而非日期）
    orig_tb = api._time_base
    api._time_base = lambda c, s: "sim"
    _, body_sim = api.h_series(conn, {"scenario": ["live"]}, {}, _body(), {})
    api._time_base = orig_tb
    t.ok(body_sim["time"] and "2026-" not in str(body_sim["time"][0]),
         "反证：sim 口径下 time 变 HH:MM（证明分支非空判）")

    # ★ wall 场景的默认时间窗必须是"全量"，不能沿用 sim 的 [0, 86400]：
    #   墙钟秒 ~1.79e9，用 86400 当上界会把数据全过滤掉 —— 接口 200 但 items 空，
    #   比报错更难查。这条断言就是钉住这一点。
    st, sw = api.h_steps(conn, {"scenario": ["live"]}, {}, _body(), {})
    t.eq(st, 200, "live 场景 h_steps 200")
    t.eq(sw["total"], 5, "wall 场景默认时间窗覆盖全部行（不被 86400 截掉）")

    # =================================================================
    print("\n[J] 引擎双模式（run / sim）与仿真调度")
    # =================================================================
    # v1.4 的核心：把"数据从哪来"提升为平台级状态。
    #   engine = run → 实时引擎按墙钟跑，数据边跑边入库
    #   engine = sim → 离线批量仿真，跑完装成一份数据集
    # 本节锁三件事：① 仿真调度真的跑通并入库（不是只写台账）
    #              ② 实时引擎真的能被拉起、写出数据、被停掉
    #              ③ 多日能量按日拆分的"分摊不变量"成立（Σ=整段）
    t.eq(engine.get_mode(conn), "sim", "引擎模式常量读回一致：sim")
    t.ok(engine.ENGINE_RUN in engine.ENGINES and engine.ENGINE_SIM in engine.ENGINES,
         "engines 常量齐备")
    src = engine.source_of("device")
    t.ok(src["available"] is False and src["why"],
         "真机数据源如实标为不可用（不做「看起来像接了真机」的界面）")
    t.eq(engine.source_of("model")["available"], True, "模型数据源可用")

    st, sk = api.dispatch(conn, "GET", "/api/sim/runs", {}, {}, viewer_u)
    t.eq(st, 200, "GET /api/sim/runs 200")
    kinds = {k["id"]: k for k in sk["kinds"]}
    t.eq(sorted(kinds), ["day", "month", "week"], "三种仿真时长齐备（日/周/月）")
    t.eq(kinds["day"]["duration_s"], 86400.0, "日仿真时长 86400 s")
    t.eq(kinds["week"]["duration_s"], 7 * 86400.0, "周仿真时长 7 天")
    t.eq(kinds["month"]["duration_s"], 30 * 86400.0, "月仿真时长 30 天")
    t.eq(kinds["day"]["log_every"], 10, "日/周用 10 s 粒度")
    t.eq(kinds["month"]["log_every"], 60, "月仿真放宽到 60 s 粒度（30 天 @10s 画不动）")

    st, _ = api.dispatch(conn, "POST", "/api/sim/runs", {}, {"kind": "year"}, operator_u)
    t.eq(st, 400, "未知仿真时长返回 400")
    st, _ = api.dispatch(conn, "POST", "/api/sim/runs", {}, {"kind": "day"}, viewer_u)
    t.eq(st, 403, "viewer 不能发起仿真")
    st, _ = api.dispatch(conn, "DELETE", "/api/sim/runs/nope", {}, {}, operator_u)
    t.eq(st, 400, "删除不存在的仿真运行返回 400")

    # ---- ① 端到端跑一次日仿真（需要 10/build/sim_demo.exe；没有就跳过）----
    if os.path.isfile(simrun.SIM_EXE):
        st, sr = api.dispatch(conn, "POST", "/api/sim/runs", {},
                              {"kind": "day", "fault": False}, operator_u)
        t.eq(st, 200, "发起日仿真 200")
        run_id = sr["run_id"]
        t.eq(sr["status"], "running", "台账立刻是 running（不等它跑完）")
        t.eq(sr["scenario_id"], f"sim-{run_id}", "数据集 id = sim-<run_id>")
        t.ok(re.match(r"^d\d{3}$", run_id), f"日仿真编号形如 d001（得到 {run_id}）")

        for _ in range(120):                       # 最多等 60 s
            rr = simrun.get_run(conn, run_id)
            if rr["status"] != "running":
                break
            time.sleep(0.5)
        t.eq(rr["status"], "ok", f"日仿真跑完 status=ok（err={rr.get('error') or '无'}）")
        t.eq(rr["steps"], 8640, "日仿真 8640 拍")
        t.eq(rr["log_rows"], 8640, "10 s 粒度 → 8640 行日志")
        t.eq(rr["day_count"], 1, "日仿真只拆出 1 个自然日")
        t.ok(rr["invariants_all_ok"] == 1, "日仿真硬不变量全过")
        t.ok(rr["net_benefit_cny"] is not None, "台账回填了净收益（来自 economics）")
        t.ok(rr["wall_s"] > 0, "台账记了墙钟耗时")
        n = emsdb.query_value(
            conn, "SELECT COUNT(*) FROM step_record WHERE scenario_id = ?",
            (sr["scenario_id"],), 0)
        t.eq(n, 8640, "仿真结果真的入库了 8640 行（不是只写台账）")
        sc_row = emsdb.query_one(
            conn, "SELECT time_base, name FROM scenario WHERE scenario_id = ?",
            (sr["scenario_id"],))
        t.eq(sc_row["time_base"], "sim", "仿真数据集 time_base=sim（0..86400 秒）")
        # 仿真完成后，仿真模式的默认数据集应指向它
        t.eq(engine.default_scenario(conn), sr["scenario_id"],
             "仿真模式默认数据集 = 最近一次成功仿真")
        st, ov = api.dispatch(conn, "GET", "/api/overview", {}, {}, viewer_u)
        t.eq(st, 200, "仿真模式下 overview 200（默认数据集解析正确）")
        t.eq(ov["engine"], "sim", "overview 标注 engine=sim")
        t.eq(ov["time_base"], "sim", "overview 标注 time_base=sim")
        t.eq(ov["loop"]["duration_s"], 86400.0, "sim 口径 loop.duration_s = 86400")
        t.ok(0.0 <= ov["loop"]["progress"] <= 1.0, "sim 口径 progress 在 0..1")

        # 删除：数据 + 台账 + 产物目录一起清
        st, dl = api.dispatch(conn, "DELETE", f"/api/sim/runs/{run_id}", {}, {},
                              operator_u)
        t.eq(st, 200, "删除仿真运行 200")
        t.eq(dl["removed"]["step_record"], 8640, "删除清掉了 8640 行时序")
        t.eq(emsdb.query_value(conn, "SELECT COUNT(*) FROM sim_run WHERE run_id = ?",
                               (run_id,), 0), 0, "台账行已删")
        t.eq(emsdb.query_value(
            conn, "SELECT COUNT(*) FROM step_record WHERE scenario_id = ?",
            (sr["scenario_id"],), 0), 0, "数据集已删")
    else:
        print(f"  [SKIP] 未找到 {simrun.SIM_EXE}，跳过端到端仿真（先跑 10/scripts/build.bat）")

    # ---- ② 多日能量按日拆分的分摊不变量（不依赖仿真器）----
    # 造一份 2 天、每拍 600 s 的合成时序：t = 0,600,…,172800（第 0 天含端点 t=86400）
    syn = "sim-test2d"
    conn.execute("INSERT OR REPLACE INTO scenario(scenario_id, name, steps,"
                 " log_rows, time_base, imported_at) VALUES(?,?,?,?,?,?)",
                 (syn, "合成两日", 289, 289, "sim", "2026-01-01 00:00:00"))
    rows = []
    for d in (0, 1):
        for k in (range(0, 145) if d == 0 else range(1, 145)):
            rows.append((syn, d * 86400.0 + k * 600.0, "NORMAL", 90.0, 50.0,
                         100.0, -60.0, -60.0, 0.5, 25.0, -200.0, 200.0,
                         0, 0, 0, 0, 0, "ok"))
    # 合成的 t=86400 一行**只写一次**（第 0 天含端点），靠 day 归属判给第 0 天
    conn.executemany(
        "INSERT OR REPLACE INTO step_record(scenario_id, t_s, state, p_load_kw,"
        " p_pv_kw, p_grid_kw, p_cmd_kw, p_actual_kw, soc, temp_c, p_lower_kw,"
        " p_upper_kw, clamped, safety_clip, state_gated, hold_last,"
        " fault_bits, reason) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", rows)
    conn.execute("INSERT OR REPLACE INTO economics(scenario_id, net_benefit_cny)"
                 " VALUES(?,?)", (syn, 1000.0))
    conn.commit()
    t.eq(emsdb.query_value(conn, "SELECT COUNT(*) FROM step_record"
                                  " WHERE scenario_id = ?", (syn,), 0), 289,
         "合成两日共 289 行（第 0 天 145 + 第 1 天 144）")

    days = importer.write_energy_days(conn, syn, "2026-03-01", 2)
    t.eq(len(days), 2, "按日拆分出 2 行")
    t.eq([d["date"] for d in days], ["2026-03-01", "2026-03-02"], "日期逐日递增")
    # 电量：第 0 天 144 个 600 s 步长（首拍 t=0 的 raw_dt=0，不计）+ 第 1 天 144 个
    t.close(days[0]["charge_kwh"], 60.0 * 86400.0 / 3600.0, "第 0 天充电量", tol=1e-6)
    t.close(days[1]["charge_kwh"], 60.0 * 86400.0 / 3600.0, "第 1 天充电量", tol=1e-6)
    t.eq(days[0]["rows"], 145, "第 0 天 145 行（含端点 t=86400，且 t=0 不被丢到第 -1 天）")
    t.eq(days[1]["rows"], 144, "第 1 天 144 行")
    t.eq(days[0]["rows"] + days[1]["rows"], 289,
         "逐日行数之和 == 总行数（没有行掉进「第 -1 天」）")
    # ★ 分摊不变量：Σ(每日净收益) == 整段净收益（这正是"分摊"二字的定义）
    s = sum(d["net_benefit_cny"] for d in days)
    t.close(s, 1000.0, "Σ(每日分摊净收益) == 整段净收益", tol=1e-6)
    t.close(days[0]["net_benefit_cny"], 500.0, "两天吞吐量相同 → 各摊 500", tol=1e-6)
    # 单日口径必须仍然是"1 行整段聚合"（老口径逐位不变）
    one = importer.write_energy_days(conn, syn, "2026-03-01", 1)
    t.eq(len(one), 1, "day_count=1 走单日口径，只写 1 行")
    t.close(emsdb.query_value(
        conn, "SELECT net_benefit_cny FROM energy_statistics WHERE scenario_id = ?",
        (syn,), 0.0), 1000.0, "单日口径净收益直接取 economics", tol=1e-6)
    got = emsdb.query_value(conn, "SELECT COUNT(*) FROM energy_statistics"
                                  " WHERE scenario_id = ?", (syn,), 0)
    t.eq(got, 1, "先清后写：多日拆分不会被残留成多行")

    # ---- ③ 实时引擎：真的拉起 → 真的写数据 → 真的能停 ----
    if os.path.isfile(engine.source_of("model")["exe"]):
        orig_dir = engine.LIVE._live_dir
        engine.LIVE._live_dir = lambda: tmpdir      # 别往仓库 14/data/live 里写测试文件
        try:
            st, live0 = api.dispatch(conn, "GET", "/api/engine", {}, {}, viewer_u)
            t.eq(live0["live"]["state"], "stopped", "启动前引擎状态 stopped")
            st, ls = api.dispatch(conn, "POST", "/api/engine/live/start", {},
                                  {"dt_s": 1.0, "log_every": 1}, operator_u)
            t.eq(st, 200, "启动实时引擎 200")
            t.ok(ls["running"], "启动后 running=True")
            # 实录文件由**子进程**创建，Popen 返回后要等它落盘（别断言得太早）
            csv_ok = False
            for _ in range(20):
                if ls["csv"] and os.path.isfile(ls["csv"]):
                    csv_ok = True
                    break
                time.sleep(0.25)
            t.ok(csv_ok, "实录文件已创建（数据源进程已开始写）")
            t.eq(ls["dt_s"], 1.0, "dt_s 按请求生效")
            t.eq(ls["log_every"], 1, "log_every 按请求生效")
            t.ok(ls["source_is_model"], "如实标注数据源是模型（不是真机）")
            st, _ = api.dispatch(conn, "POST", "/api/engine/live/start", {}, {},
                                 operator_u)
            t.eq(st, 400, "重复启动被拒（先停再启）")
            st, _ = api.dispatch(conn, "POST", "/api/engine/live/stop", {}, {},
                                 viewer_u)
            t.eq(st, 403, "viewer 不能停引擎")

            time.sleep(3.5)                          # log_every=1 → 每秒一行
            st, mid = api.dispatch(conn, "GET", "/api/engine", {}, {}, viewer_u)
            t.ok(mid["live"]["rows"] >= 2,
                 f"实录在增长（已入库 {mid['live']['rows']} 行）")
            t.eq(mid["live"]["state"], "running", "尾随线程把状态推进到 running")
            t.ok(mid["live"]["imported_at"], "记了最后入库时间")

            st, sp = api.dispatch(conn, "POST", "/api/engine/live/stop", {}, {},
                                  operator_u)
            t.eq(st, 200, "停止实时引擎 200")
            t.eq(sp["state"], "stopped", "停止后状态 stopped")
            n_live = emsdb.query_value(
                conn, "SELECT COUNT(*) FROM step_record WHERE scenario_id = 'live'",
                (), 0)
            t.ok(n_live > 5, f"停止后 live 场景有实时数据（{n_live} 行）")
            lr = emsdb.query_one(
                conn, "SELECT time_base, steps FROM scenario WHERE scenario_id='live'")
            t.eq(lr["time_base"], "wall", "实时数据集 time_base=wall")
            t.ok(lr["steps"] >= n_live, "scenario.steps 回填为累计行数")

            # 运行模式的默认数据集是 live；此时已有数据 → overview 应 200
            engine.set_mode(conn, "run", "admin")
            t.eq(engine.default_scenario(conn), "live", "运行模式默认数据集 = live")
            st, ov2 = api.dispatch(conn, "GET", "/api/overview", {}, {}, viewer_u)
            t.eq(st, 200, "运行模式有数据后 overview 200")
            t.eq(ov2["engine"], "run", "overview 标注 engine=run")
            t.eq(ov2["time_base"], "wall", "overview 标注 time_base=wall")
            t.ok(ov2["loop"]["progress"] is None,
                 "wall 口径不给 progress（86400 会算出 20000% 的进度条）")
            t.ok(ov2["loop"]["elapsed_s"] >= 0, "wall 口径给 elapsed_s")
            t.ok(ov2["t_s"] > 1e9, "运行模式 t_s 是 Unix 墙钟秒")

            # 清空实时数据：只清数据，不动引擎模式
            st, rs = api.dispatch(conn, "POST", "/api/engine/live/reset", {}, {},
                                  operator_u)
            t.eq(st, 200, "清空实时数据 200")
            t.ok(rs["cleared"] >= n_live, f"清掉了 {rs['cleared']} 行")
            t.eq(emsdb.query_value(
                conn, "SELECT COUNT(*) FROM step_record WHERE scenario_id='live'",
                (), 0), 0, "清空后 live 无时序行")
            t.eq(engine.get_mode(conn), "run", "清空不影响引擎模式")
        finally:
            engine.LIVE._live_dir = orig_dir
            engine.LIVE.stop(conn, "admin")
            # 清掉本次测试写到临时目录里的实录（tmpdir 在 C: 盘，项目在 D: 盘 ——
            # 这恰好把 _rel_source() 的跨盘兜底路径也跑了一遍）
            for _fn in (os.listdir(tmpdir) if os.path.isdir(tmpdir) else []):
                if _fn.startswith("live_"):
                    try:
                        os.remove(os.path.join(tmpdir, _fn))
                    except OSError:
                        pass
    else:
        print("  [SKIP] 未找到 sim_live.exe，跳过实时引擎端到端（先跑 10/scripts/build.bat）")

    # 审计里应留痕（引擎模式切换 / 引擎启停 / 仿真发起）
    acts = {r["action"] for r in auth.list_audit(conn, 200)}
    for a in ("engine_mode", "sim_run_start", "sim_run_delete",
              "engine_live_start", "engine_live_stop", "engine_live_reset"):
        t.ok(a in acts, f"审计记录动作 {a}")

    conn.close()
    # 清理临时库
    for suffix in ("", "-wal", "-shm"):
        p_ = db_path + suffix
        if os.path.isfile(p_):
            try:
                os.remove(p_)
            except OSError:
                pass
    try:
        os.rmdir(tmpdir)
    except OSError:
        pass

    return t.report("14/ 平台后端自检")


if __name__ == "__main__":
    raise SystemExit(main())
