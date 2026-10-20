"""14/ — 平台后端自检

沿用项目其余模块的纪律：断言式、有 PASS/FAIL 计数、内容缺失时显式 SKIP。

覆盖十一层：
  A. schema        —— 20 张表是否齐全（14 原有 + run_mode + 设备接入两张
                     + sim_run + 上端链路 + 上端转发表，schema v1.6）
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
  K. 告警分类      —— 等级 × 来源 双维筛选：条数对独立 SQL、交叉计数的
                    三条求和恒等式、以及"空串 = 不筛选"的静默失效防护
  L. 跨域(CORS)    —— 静态层（AST 扫 14/src 不许出现字面量 '*' 作 ACAO 实参）
                    + 端到端层（真起服务发请求：默认无 ACAO、白名单回显指定源）

运行：
    python 14/tests/selftest.py
"""

from __future__ import annotations

# ---------------------------------------------------------------------
# 内置场景数与每场景步数
#
# ★ 新增内置场景（在 14/src/importer.py import_all() 里加一个），必须同步
#   这里的 N_SCENES —— 下面十几条行数断言全部以它为基准。用常量而不是
#   散落的字面量，是为了让"加了场景却忘了改断言"只可能错一处，
#   而且错的那一处会有明确的失败项，不会静默变成"少算一个场景"。
#
#   stress（异常工况日，10/ --stress）是 SOC / TEMP / TRANSFORMER 三类
#   告警**唯一**的数据来源：默认典型日里它们恒为 0，因为系统健康
#   （关口被需量控制压在 400、变压器留 100 裕度、SOC 稳在带内）。
# ---------------------------------------------------------------------
N_SCENES = 3            # normal / fault / stress
STEPS_PER_SCENE = 8640  # 24 h × 10 s 日志粒度

import ast
import csv
import datetime
import http.client
import inspect
import io
import json
import os
import re
import socket
import subprocess
import sys
import shutil
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MODULE = os.path.dirname(HERE)
SRC = os.path.join(MODULE, "src")
sys.path.insert(0, SRC)

# ★ 自检**绝不真的拉模拟链路的进程**（simproc 认这个开关）。
#
# 为什么必须在 import 之前就设好、而且是全局的：
#   [G9] 的 `{"enable": True}` 会顺手把链路拉起来（v1.10 的语义）。第一次跑
#   这一段时没设这个开关，结果自检真的起了 6 个替身 + 采集器 + 网关 + 主站，
#   采集器又把实录导入**临时库的 live 场景** ⇒ 后面的 [I] 实时入库拿到 23 行
#   而不是 5 行，三条断言假红。**测试的副作用污染了测试自己的前提。**
#
# 真起进程这件事已由命令行 + 浏览器实测覆盖（CHANGES §86），不该混进自检：
# 自检要的是快、可重复、不占端口。
os.environ["EMS_SIMPROC_OFF"] = "1"

import api      # noqa: E402
import auth     # noqa: E402
import connconf  # noqa: E402
import emsdb    # noqa: E402
import engine   # noqa: E402
import importer  # noqa: E402
import pointmap  # noqa: E402
import simconf  # noqa: E402
import simproc  # noqa: E402
import simrun   # noqa: E402
import upmapconf  # noqa: E402

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
    t.eq(len(emsdb.TABLES), 20,
         "schema 表数（v1.5 加上端链路、v1.6 加上端转发表后 20 张）")
    t.eq(emsdb.SCHEMA_VERSION, "1.6", "schema 版本常量")
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
    t.eq(c["scenario"], N_SCENES, "scenario 行数（normal + fault + stress）")
    t.eq(c["step_record"], STEPS_PER_SCENE * N_SCENES,
         f"step_record 行数（{N_SCENES} 场景各 {STEPS_PER_SCENE}）")
    t.eq(c["control_command"], STEPS_PER_SCENE * N_SCENES, "control_command 行数")
    t.eq(c["economics"], N_SCENES, "economics 行数")
    t.eq(c["invariant"], N_SCENES, "invariant 行数")
    t.eq(c["energy_statistics"], N_SCENES, "energy_statistics 行数")

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

    # ★ 2026-10-08 改写：这里原先断言的是 `iv_f["all_ok"] == 0`
    #   （"故障注入 ⇒ 不变量必须不过"）。那是一条**负向不变量**：
    #   只有安全层真的失效、红线被突破时它才会变绿 —— 谁为了让它转绿去改系统，
    #   就是在把安全层改坏。
    #
    #   与设计意图对照，它是反的：
    #     · 10/src/main.cpp:44-48 写明故障注入的目的是验证「安全响应」
    #       —— 应门控到 0 / 降级 / 保守，而不是去突破红线；
    #     · 10/ 自己的 T107 判据就是「故障窗非零 = 0（应全 0）」，同一语义。
    #   且与本段紧邻上方的口径自相矛盾：本段叫「数据一致性（入库值 vs summary.json）」，
    #   2 行之上要求 economics 必须等于 summary.json，而 summary.json 的
    #   invariants.all_ok 就是 true —— 不可能既等于它、又是 0。
    #
    #   改为两件事，把原来想守的东西拆开守：
    #     ① 不变量仍与 summary.json 逐项相符（含 all_ok）—— 守住"没抄错数"；
    #     ② 故障注入确实生效（fault 告警显著多于 normal）—— 守住"黄瓜确实注进去了"。
    #   第 ② 条不可省：只留 ① 的话，"故障窗变成空窗、什么都没发生"也能全绿，
    #   那正是本项目最忌讳的静默失效。
    iv_ref = fault_sum["invariants"]
    t.eq(iv_f["all_ok"], 1 if iv_ref["all_ok"] else 0,
         "fault.all_ok 与 summary.json 相符（故障下仍守住红线）")
    for k in ("out_of_interval", "over_limit", "gated_nonzero", "grid_breach",
              "tr_breach", "soc_violation"):
        t.eq(iv_f[k], iv_ref[k], f"fault.{k} 与 summary.json 相符")
    t.ok(al_f > al_n,
         f"fault 告警多于 normal（故障注入确实生效：{al_f} > {al_n}）")

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
    t.ok(p["scenarios"] and len(p["scenarios"]) == N_SCENES,
         f"health 报出 {N_SCENES} 个场景")

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
    t.ok("启动采集" in (p404.get("error") or ""),
         "★ 404 文案给出可操作指引：去「设备管理 → 二 · 站内设备接入 →"
         " 启动采集」（顶部状态条删掉之后，这是唯一的启动入口）")

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
    t.eq(len(p["items"]), N_SCENES, f"economics {N_SCENES} 个场景")
    st, p = call("GET", "/api/energy", user=viewer_u)
    t.eq(st, 200, "GET /api/energy 200")
    t.eq(len(p["items"]), N_SCENES, f"energy_statistics {N_SCENES} 条")
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
    t.eq(len(p["items"]), N_SCENES, f"invariants {N_SCENES} 个场景")

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
    # ★ 数据导出与点表导出同一条纪律：带 BOM，Excel 双击中文不乱码。
    t.ok(p["body"].startswith("\ufeff"), "数据导出体带 UTF-8 BOM")
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

    # ---- G5b. 两端接入（v1.5）：下端对端形态 + 上端链路 ----
    #
    # 这一段最容易退化成摆设的地方是：界面上多了两个「模拟源 / 真实源」开关，
    # 而它们既不落盘、端侧也不读 —— 那就是纯装饰。所以这里逐条验到底：
    # 库里记下 → 落成端侧读的文件 → 文件里写明「这不是真机」。
    st, ac = call("GET", "/api/access", user=viewer_u)
    t.eq(st, 200, "GET /api/access 200（viewer 可读）")
    t.ok("upstream" in ac and "downstream" in ac, "返回上端与下端两段")
    _up0 = dict(ac["upstream"]["link"])
    t.eq(ac["upstream"]["link"]["listen_port"], 2404, "上端默认监听口 2404")
    t.eq(ac["upstream"]["link"]["kind"], "real", "上端默认真实源（不凭空当模拟）")
    t.eq(sum(ac["downstream"]["counts"][k]
             for k in ("real", "sim", "unset", "off")),
         len(ac["downstream"]["items"]),
         "★ 下端四态计数之和 = 设备接入记录数（不漏账）"
         "——「已停用」是第四态，不能塌进「未配置」")

    st, cp = call("PUT", "/api/devices/DEV-PCS-01/conn",
                  {"source_kind": "sim", "sim_host": "127.0.0.1", "sim_port": 1602},
                  user=operator_u)
    t.eq(st, 200, "下端切「模拟源」保存成功")
    t.eq(cp["conn"]["source_kind"], "sim", "库里记下 source_kind=sim")
    t.eq(cp["target"]["port"], 1602, "★ 端侧实际连 sim_port，不是 port(502)")
    t.ok(os.path.isfile(cp["conn_file"]["path"]),
         "★ 模拟源也要落盘（host 为空不等于「这台先不接」）")
    _ctxt = open(cp["conn_file"]["path"], encoding="utf-8-sig").read()
    t.ok("模拟源" in _ctxt, "落盘文件的文件头写明这是模拟源")
    t.ok("1602" in _ctxt, "落盘文件里的对端是 1602")

    # ★ 真实源 + 空地址**是合法状态**（「已登记、未接线」）—— 不能用 400 堵死，
    #   那会让现场连「先登记、后接线」都做不到。真正的守卫在端侧：文件存在且
    #   kind=real 却缺 host 时，13/ 的 loader 硬失败。
    st, _ = call("PUT", "/api/devices/DEV-PCS-01/conn", {"source_kind": "real"},
                 user=operator_u)
    t.eq(st, 200,
         "★ 切回真实源但没地址 ⇒ 仍然 200（「已登记、未接线」是合法态）")
    t.ok(not os.path.isfile(cp["conn_file"]["path"]),
         "★ 真实源且没地址 ⇒ 落盘文件被删（端侧回落默认表，不是去连空地址）")

    st, up = call("PUT", "/api/access/upstream",
                  {"kind": "sim", "listen_port": 2405, "common_addr": 3},
                  user=operator_u)
    t.eq(st, 200, "PUT /api/access/upstream 200")
    t.eq(up["upstream"]["link"]["listen_port"], 2405, "上端监听口改成 2405")
    t.eq(up["upstream"]["summary"]["kind"], "sim", "上端形态 = sim")
    t.ok(os.path.isfile(up["upstream"]["file"]), "上端参数已落盘给 P3/ 网关")
    _utxt = open(up["upstream"]["file"], encoding="utf-8-sig").read()
    t.ok("listen_port=2405" in _utxt, "落盘文件里有 listen_port=2405")
    t.ok("模拟调度" in _utxt, "文件头写明这是模拟调度（★ 不是上级调度）")

    st, _ = call("PUT", "/api/access/upstream", {"common_addr": 0}, user=operator_u)
    t.eq(st, 400, "CA=0 被拒（0 与 65535 是保留值）")
    st, _ = call("PUT", "/api/access/upstream", {"listen_port": 70000}, user=operator_u)
    t.eq(st, 400, "监听口越界被拒")
    st, _ = call("PUT", "/api/access/upstream", {"kind": "sim"}, user=viewer_u)
    t.eq(st, 403, "改上端 = operator（viewer 不许动）")

    st, ac2 = call("GET", "/api/access", user=viewer_u)
    t.eq(st, 200, "保存后仍可读")
    t.ok(ac2["downstream"]["counts"]["unset"] >= 1,
         "清空地址的那台计入 unset（界面的「未配置」来自这里，不是前端自己推）")

    # 复原上端：这一段改了监听口与形态，后面 [G8] 与 §79.5 的空表回归都从
    # 「上端默认现场」出发。不同步复原的话，它们红的理由会是这一段留下的。
    call("PUT", "/api/access/upstream",
         {"kind": _up0["kind"], "listen_port": _up0["listen_port"],
          "common_addr": _up0["common_addr"], "bind_host": _up0["bind_host"],
          "master_host": _up0["master_host"], "enabled": _up0["enabled"]},
         user=operator_u)
    st, ac3 = call("GET", "/api/access", user=viewer_u)
    t.eq(ac3["upstream"]["link"]["kind"], _up0["kind"], "上端参数已复原（形态）")
    t.eq(ac3["upstream"]["link"]["listen_port"], _up0["listen_port"],
         "上端参数已复原（监听口）")

    # ---- G5c. 数据源（v1.6）：两端各一个开关，一次只切一端 ----
    #
    # 这一段守的是「仿真只是设备管理页的一个数据源」的**后端**一半：切换必须
    # 同时改库、重写端侧读的那份文件，否则界面切了、端侧还在连旧对端，而两边
    # 都不报错。它也是 §81 那次改动的判据：两端是各自独立的开关，不做「一并切掉」。
    st, ds0 = call("GET", "/api/data-source", user=viewer_u)
    t.eq(st, 200, "GET /api/data-source 200")
    t.eq(len(ds0["ends"]), 2, "★ 两端各一个开关：调度主站接入 / 站内设备接入")
    t.eq(len(ds0["kinds"]), 2, "可选形态两种：现场 / 模拟")
    t.eq(ds0["downstream"]["kind"], "sim",
         "★ 下端默认 = 模拟源（开箱即用，不假装接了真机）")

    st, _ = call("POST", "/api/data-source", {"kind": "sim"}, user=operator_u)
    t.eq(st, 400, "★ 不指明切哪一端 ⇒ 400（两端各是各的开关，不做「一并切掉」）")
    st, _ = call("POST", "/api/data-source",
                 {"end": "downstream", "kind": "nope"}, user=operator_u)
    t.eq(st, 400, "未知形态被拒")
    st, _ = call("POST", "/api/data-source",
                 {"end": "downstream", "kind": "sim"}, user=viewer_u)
    t.eq(st, 403, "viewer 不能切数据源")

    st, ds1 = call("POST", "/api/data-source",
                   {"end": "downstream", "kind": "sim"}, user=operator_u)
    t.eq(st, 200, "切下端到模拟源 200")
    t.eq(ds1["downstream"]["kind"], "sim", "回显下端形态 = sim")
    _dpath = ds1["conn_file"]["path"]
    t.ok(os.path.isfile(_dpath), "★ 下端 active.conn 已重写（端侧读文件，不读库）")
    t.ok("模拟源" in open(_dpath, encoding="utf-8-sig").read(),
         "active.conn 的文件头写明形态是模拟源")
    # ★ 判「每一台都跟着切」要查 device_conn，不能查 /api/access 的 items ——
    #   items 对**没有接入记录**的设备回显 real（那是"未登记"，不是"没跟着切"）。
    t.eq({r["source_kind"] for r in
          emsdb.query(conn, "SELECT source_kind FROM device_conn")}, {"sim"},
         "★ 下端的每一台都跟着切（不是只改当前那台）")
    t.eq(ds1["upstream"]["kind"], _up0["kind"],
         "★ 切下端**不动**上端（两端各是各的开关）")

    # 正在采集时不许切下端：运行中换对端，采集器还在按旧参数读写。
    _orig_snap = engine.LIVE.snapshot
    engine.LIVE.snapshot = lambda c: dict(_orig_snap(c), running=True)
    try:
        st, _ = call("POST", "/api/data-source",
                     {"end": "downstream", "kind": "real"}, user=operator_u)
        t.eq(st, 409, "★ 采集中切下端被拒（409）—— 不是「尽力而为」地半切")
    finally:
        engine.LIVE.snapshot = _orig_snap

    st, ds2 = call("POST", "/api/data-source",
                   {"end": "upstream", "kind": "sim"}, user=operator_u)
    t.eq(st, 200, "切上端到模拟调度 200")
    _upath = ds2["upstream_file"]["path"]
    t.ok(os.path.isfile(_upath), "★ 上端 active.upstream 已重写（网关启动时读它）")
    t.ok("模拟调度" in open(_upath, encoding="utf-8-sig").read(),
         "active.upstream 的文件头写明对端是模拟调度")
    t.eq(ds2["upstream"]["kind"], "sim", "★ 上端形态跟着写穿（不是只改下端一半）")

    # 收尾：把 DEV-PCS-01 的地址补回去 —— 下面「切回现场」要凭它落盘，
    # 而 [G] 后面的用例也用它做「清空 host」。
    call("PUT", "/api/devices/DEV-PCS-01/conn",
         {"source_kind": "real", "host": "192.168.1.20"}, user=operator_u)
    st, ds3 = call("POST", "/api/data-source",
                   {"end": "downstream", "kind": "real"}, user=operator_u)
    t.eq(st, 200, "切回现场数据源 200")
    t.ok(os.path.isfile(_dpath)
         and "现场设备" in open(_dpath, encoding="utf-8-sig").read(),
         "active.conn 的文件头写明形态是现场设备")
    st, _ = call("POST", "/api/data-source",
                 {"end": "upstream", "kind": "real"}, user=operator_u)
    t.eq(st, 200, "上端切回上级调度 200")
    st, ac_ds2 = call("GET", "/api/access", user=viewer_u)
    t.eq(ac_ds2["upstream"]["link"]["kind"], "real", "上端也切回现场")
    t.eq({r["source_kind"] for r in
          emsdb.query(conn, "SELECT source_kind FROM device_conn")}, {"real"},
         "下端全部切回现场")

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
    # =================================================================
    print("\n[G8] 上端第二份输入：IOA 转发表（v1.6 起**可保存**）")
    # =================================================================
    # ★ 这一段守的是「图里说两端各两份输入，页面上必须真的有两份」：
    #   上端的第二份是 IOA ↔ 点名，而下端的第二份（寄存器 ↔ 点名）走
    #   /api/devices/{id}/point-map。两者方向不同，不能互相顶替。
    #
    # ★ v1.6 的翻转：这一段**原来守的是「只读」**（断言 PUT 返回 404）。
    #   那个担心（怕出现「界面一套 IOA、网关启动时另一套」两个真相源）是对的，
    #   处置却错了 —— 正确答案是**把真相源收敛成一个文件**：平台写
    #   config/point-map/active.pointmap.csv，P3 网关启动时按约定路径读它。
    #   这样界面写的就是网关读的那一份，而不是"平台另外还有一份"。
    #   于是本节改成正向判据，并留一条**新的**反向守卫代替原来那条：
    #   平台绝不回写 P3/ 仓库里的默认底本 —— 那是随包样例，被回写之后
    #   下一次拿它当基准对照就失效了。
    #
    # 隔离：把约定目录指到临时目录。**必须**做，否则本节会把
    #   config/point-map/active.pointmap.csv 写脏 —— 那是网关真会读的文件。
    old_map_dir8 = os.environ.get(pointmap.ENV_MAP_DIR)
    pmdir8 = os.path.join(tmpdir, "point-map-upstream")
    os.environ[pointmap.ENV_MAP_DIR] = pmdir8
    ap8 = upmapconf.map_path()
    t.eq(ap8, os.path.join(pmdir8, "active.pointmap.csv"),
         "EMS_POINT_MAP_DIR 生效（转发表目录被隔离到临时目录）")
    _operator = operator_u["username"]

    try:
        st, up_pm = call("GET", "/api/upstream/point-map", user=viewer_u)
        t.eq(st, 200, "viewer 可读上端转发表")
        t.ok(up_pm.get("ok") is True, "转发表接口 ok")
        t.eq(up_pm.get("source"), "default",
             "★ 没保存过 → 生效的是内置默认表（不是编出来的）")
        t.ok(str(up_pm.get("active_path") or "").endswith("active.pointmap.csv"),
             "active_path 指向约定那只文件")
        cnt = up_pm.get("counts") or {}
        t.eq(cnt.get("total"), 30, "转发表 30 行（= 24 上送 + 6 下行）")
        t.eq(cnt.get("up"), 24, "★ 上送 24 点")
        t.eq(cnt.get("down"), 6, "★ 下行 6 点")
        t.eq(len(up_pm.get("rows") or []), 30, "rows 行数 = 30")
        t.eq(up_pm.get("dup_ioa"), [], "IOA 无重复")
        t.eq(up_pm.get("bad_lines"), [], "没有解析不了的行")
        ioas = [r["ioa"] for r in up_pm["rows"]]
        t.ok(all(isinstance(x, int) for x in ioas), "IOA 全是整数")
        t.eq(len(set(ioas)), len(ioas), "IOA 互不相同（调度靠它定位，重复即歧义）")
        t.eq(sorted(ioas)[:1], [1001], "最小 IOA = 1001（遥信段起点）")
        t.eq(sorted(ioas)[-1:], [6004], "最大 IOA = 6004（遥调段）")

        first = up_pm["rows"][0]
        t.eq(first["ioa"], 4001, "首行 IOA 4001")
        t.eq(first["src"], "MEAS.P_LOAD", "首行源点名 = MEAS.P_LOAD")
        t.eq(first["direction"], "up", "首行方向 = 上送")
        # 上送点必须有源点名（否则网关不知道该取哪个量）；下行点反过来必须留空
        # （它们不是从 RT_DB 读的，而是调度写进来的）。
        up_rows = [r for r in up_pm["rows"] if r["direction"] == "up"]
        down_rows = [r for r in up_pm["rows"] if r["direction"] == "down"]
        t.ok(all(r["src"] for r in up_rows), "★ 每个上送点都有源点名")
        t.ok(all(not r["src"] for r in down_rows), "★ 下行点的源点名为空（只由调度写入）")
        t.eq(sorted({r["kind"] for r in down_rows}),
             ["CMD_SINGLE", "SET_FLOAT"], "下行只有遥控/遥调两种类型")
        up_file = up_pm.get("file") or ""
        t.ok("P3" in up_file and "point_map_default.csv" in up_file,
             "没保存过时回落展示 P3/ 的默认转发表 CSV")

        # ---- G8a. 保存链路（v1.6）：校验 → 落盘 → 写库 ----
        pm_cols = ("ioa", "kind", "group", "name", "src",
                   "scale", "offset", "unit", "note")
        base_rows = [{k: r[k] for k in pm_cols} for r in up_pm["rows"]]

        def _put(rows, user=None):
            return call("PUT", "/api/upstream/point-map", {"rows": rows},
                        user=user or operator_u)

        st, saved = _put(base_rows)
        t.eq(st, 200, "★ operator 保存转发表成功")
        t.eq(saved.get("source"), "platform", "保存后 source = platform")
        t.eq((saved.get("counts") or {}).get("total"), 30, "保存了 30 行")
        t.ok(os.path.isfile(ap8), "★ 约定那只文件已落盘")

        with open(ap8, "r", encoding="utf-8-sig") as fh:
            disk = fh.read()
        data_lines = [ln for ln in disk.splitlines()
                      if ln.strip() and not ln.lstrip().startswith("#")
                      and not ln.replace(" ", "").lower().startswith("ioa,")]
        t.eq(len(data_lines), 30, "落盘文件里 30 个数据行")
        t.eq(len(data_lines[0].split(",")), 9, "★ 每行 9 列（与 P3 loader 同口径）")
        t.ok(data_lines[0].split(",")[0] == "4001", "首行第一列是 IOA 4001")

        st, pm2 = call("GET", "/api/upstream/point-map", user=viewer_u)
        t.eq(pm2.get("source"), "platform", "GET 之后报 platform")
        t.eq(pm2.get("saved_at"), saved.get("saved_at"),
             "★ 保存时刻能从文件头读回来（不必为此单开一张表）")
        t.eq(pm2.get("username"), _operator, "★ 保存人能从文件头读回来")
        t.eq((pm2.get("counts") or {}).get("total"), 30, "读回来仍是 30 行")

        rows_db = emsdb.query(conn, "SELECT * FROM upstream_point_map")
        t.eq(len(rows_db), 30, "★ 库存档 30 行（文件之外还有一份持久存档）")

        # ---- G8a-2. 校验：错表一律 400，且**不许碰盘上的文件** ----
        before = open(ap8, "rb").read()

        def _clone(mut):
            rows = [{k: r[k] for k in pm_cols} for r in up_pm["rows"]]
            mut(rows)
            return rows

        def m_dup(rows):
            rows[1]["ioa"] = rows[0]["ioa"]

        def m_kind(rows):
            rows[0]["kind"] = "NOT_A_KIND"

        def m_group(rows):
            rows[0]["group"] = 17

        def m_scale(rows):
            rows[0]["scale"] = 0

        def m_src(rows):
            rows[0]["src"] = "MEAS.NO_SUCH_POINT"

        def m_src_empty(rows):
            rows[0]["src"] = ""

        def m_comma(rows):
            rows[0]["name"] = "负,荷"

        def m_seg(rows):
            rows[0]["ioa"] = 6001

        for nm, mut in [("IOA 重复", m_dup), ("未知 kind", m_kind),
                        ("group 越界", m_group), ("scale=0（下行换算除零）", m_scale),
                        ("src 不在 EMS 点表", m_src), ("上送点 src 为空", m_src_empty),
                        ("名称含逗号（会截断 CSV 列）", m_comma),
                        ("IOA 落在错误区段", m_seg)]:
            st_e, perr = _put(_clone(mut))
            t.eq(st_e, 400, "★ 拒绝：" + nm)
            t.ok(perr.get("error"), "  拒绝时说清了原因：" + nm)

        st_e, _ = call("PUT", "/api/upstream/point-map", {"rows": []}, user=operator_u)
        t.eq(st_e, 400, "★ 拒绝：空表（保存空表 = 一个点都不上送，还不报错）")
        t.eq(open(ap8, "rb").read(), before,
             "★ 校验失败时盘上的文件一字未动（半张坏表比没表危险）")

        # 权限：viewer 不能写
        st_v, _ = _put(base_rows, user=viewer_u)
        t.eq(st_v, 403, "viewer 不可保存转发表")
        t.eq(call("DELETE", "/api/upstream/point-map", user=viewer_u)[0], 403,
             "viewer 不可删除转发表")

        # CSV 导出
        st_c, csvp = call("GET", "/api/upstream/point-map.csv", user=viewer_u)
        t.eq(st_c, 200, "CSV 导出 200")
        t.eq(csvp.get("filename"), "upstream-point-map.csv", "导出文件名")
        t.eq(csvp.get("body", "")[:1], "\ufeff", "导出带 BOM（Excel 打开不乱码）")
        st_t, tpl = call("GET", "/api/upstream/point-map.csv", user=viewer_u,
                         template=1)
        t.eq(st_t, 200, "模板导出 200")
        t.eq(tpl.get("filename"), "upstream-point-map_template.csv", "模板文件名")

        # ---- G8a-3. DELETE：恢复内置默认（必须与 PUT 成对）----
        st_d, dele = call("DELETE", "/api/upstream/point-map", user=operator_u)
        t.eq(st_d, 200, "operator 可恢复内置默认")
        t.eq(dele.get("removed"), True, "确实删掉了那一份")
        t.ok(not os.path.isfile(ap8), "恢复后文件不在了")
        t.eq(emsdb.query(conn, "SELECT * FROM upstream_point_map"), [],
             "★ 库存档一并清空（否则「当前生效=默认表」与「平台还有一份」同时存在）")
        st_p3, pm3 = call("GET", "/api/upstream/point-map", user=viewer_u)
        t.eq(pm3.get("source"), "default", "★ 恢复后回到内置默认表")
        st_d2, dele2 = call("DELETE", "/api/upstream/point-map", user=operator_u)
        t.eq(dele2.get("removed"), False, "再删一次：如实说没有东西可删（不装作成功）")

        # ---- 负向：默认底本不在时**必须**仍然 200 且说清楚，不能 500 ----
        keep_csv = api.UPLINK_PM_CSV
        api.UPLINK_PM_CSV = os.path.join(tmpdir, "no-such-point-map.csv")
        try:
            st_m, miss = call("GET", "/api/upstream/point-map", user=viewer_u)
            t.eq(st_m, 200, "转发表文件缺失时不报 500")
            t.eq(miss.get("file_exists"), False, "缺失时 file_exists=False")
            t.eq((miss.get("counts") or {}).get("total"), 0, "缺失时 0 行")
            t.eq(miss.get("source"), "default", "缺失时仍然说是内置默认（不是报错）")
        finally:
            api.UPLINK_PM_CSV = keep_csv

        # ★ 新的反向守卫：平台只写**自己**那只约定文件，绝不回写 P3/ 的底本
        t.eq(api.UPLINK_PM_CSV,
             os.path.join(pointmap.PROJECT_ROOT, "P3", "data", "point_map_default.csv"),
             "UPLINK_PM_CSV 仍指向 P3/ 的默认底本（读，不写）")
        t.ok("return 200" not in inspect.getsource(api.h_upstream_point_map)
             or api.h_upstream_point_map is not api.h_upstream_point_map_put,
             "读与写是两个处理函数，不是同一个里加个 if")
    finally:
        # 还原环境变量，别让隔离泄漏到后面的节
        if old_map_dir8 is None:
            os.environ.pop(pointmap.ENV_MAP_DIR, None)
        else:
            os.environ[pointmap.ENV_MAP_DIR] = old_map_dir8

    print("\n[G9] 模拟输入源总开关（v1.7：页尾一个面板管两端）")
    # =================================================================
    # ★ 这一段守的是**两条不能破的承诺**：
    #
    #   ① 「换一套」只换那一套数据，**不动形态**。
    #      第一版写成 `kind = "sim" if want else "real"`，而 rotate 时
    #      want 是 None ⇒ kind 落到 real ⇒ **点一下「换一套」就把模拟源关了**。
    #      浏览器实测抓到的：徽章从「模拟中」变回「真实源」，而页面上唯一的
    #      解释是"套 3"—— 没有任何地方说形态被改了。
    #      ⇒ 现在 `kind=None` 表示"沿用两端各自的形态"，本节逐条守住。
    #
    #   ② 真实值**一个字都不动**。
    #      下端 host/port、上端 listen_port/common_addr 是现场配的，
    #      联调完关掉模拟源还要用。模拟只写 sim_host / sim_port / master_host。
    #      ⇒ 本节把真实值在启用前抄一份，经历「启用 → 换两套 → 关闭」
    #        之后逐字段对拍。
    #
    #   ③ 「未配置」与「已停用」不塌成同一个状态。
    #      enabled=0 早先被直接折叠进 unset，于是启用模拟源之后那一行是
    #      「形态=未配置 / 目标=127.0.0.1:1702」—— 两列自相矛盾。
    #
    # 隔离：三只落盘文件（active.conn / active.upstream / active.pointmap.csv）
    #   都住在 `pointmap.map_dir()` 下，改一个环境变量就全隔离了。
    old_map_dir9 = os.environ.get(pointmap.ENV_MAP_DIR)
    pmdir9 = os.path.join(tmpdir, "point-map-sim")
    os.environ[pointmap.ENV_MAP_DIR] = pmdir9

    def _conn_of(did):
        """从 /api/access 取某一台的接入记录。

        ★ 不能拿 POST /api/sim-source 的返回当视图：它那里的
          `downstream` 是形态字符串（"sim"），不是接入明细。
          两处同名字段不同语义，是最容易顺手写错的一种。
        """
        v = call("GET", "/api/access", user=viewer_u)[1]
        for r in (v.get("downstream", {}).get("items") or []):
            if r["device_id"] == did:
                return r
        return {}

    try:
        # ---- 准备：给两台设备填真实地址（它们就是本节要保护的那份真实值） ----
        REAL = {"DEV-PCS-01": ("192.168.1.30", 15020),
                "DEV-METER-01": ("192.168.1.31", 15021)}
        for did, (h_, p_) in REAL.items():
            st, _ = call("PUT", f"/api/devices/{did}/conn",
                         {"source_kind": "real", "host": h_, "port": p_,
                          "enabled": True}, user=operator_u)
            t.eq(st, 200, f"准备：{did} 填好真实地址 {h_}:{p_}")

        # 上端链路参数没有单独的 GET：它在 /api/access 里（与下端同一个视图）
        real_up = {k: call("GET", "/api/access", user=viewer_u)[1]
                   ["upstream"]["link"][k]
                   for k in ("listen_port", "common_addr")}

        # ---- G9a. 初始状态 ----
        st, s0 = call("GET", "/api/sim-source", user=viewer_u)
        t.eq(st, 200, "viewer 可读模拟源状态")
        t.eq(s0.get("enabled"), False, "初始不是模拟态（两端没都被切过去）")
        t.ok(s0.get("preset_name"), "报出当前套名")
        t.eq(s0.get("preset_count"), simconf.preset_count(), "套数由 simconf 一处定义")
        t.ok(s0.get("note"), "★ 提示文案带回来（真实值不会被覆盖这句话要给出来）")
        plan0 = s0["plan"]["downstream"]
        n_dev = emsdb.query_value(conn, "SELECT COUNT(*) FROM device", (), 0)
        t.eq(len(plan0), n_dev,
             f"★ 下端计划按**台账**排 {n_dev} 台（不是按已有接入记录排）"
             " —— 台账里有、接入参数一行都没有的设备也必须排到")
        t.ok(all(r["sim_host"] and r["sim_port"] for r in plan0),
             "每台都排到了地址与端口")

        # ---- G9b. 启用 → 两端都切过去，模拟列被填好 ----
        st, s1 = call("POST", "/api/sim-source", {"enable": True},
                      user=operator_u)
        # ★ 202 不是"失败"，是"收下了，还在办"（§92b）：拉/收链路在后台线程跑，
        #   请求不再挂着等 6~7 s。contract：**返回成功 ≠ 链路已经起来**，
        #   要看 `chain.job`（`running` 期间是"在办"，`ok` 才是结果）。
        t.eq(st, 202, "★ operator 启用模拟源 → 202（收下即返回，链路后台拉）")
        j1 = (s1.get("chain") or {}).get("job") or {}
        t.ok("action" in j1, "返回体带 job（前端靠它决定何时收进度条）")
        t.eq(s1.get("enabled"), True, "启用后 enabled=True（两端都切了）")
        t.eq(s1.get("downstream"), "sim", "下端形态 = sim")
        t.eq(s1.get("upstream"), "sim", "上端形态 = sim")
        t.ok("启用" in (s1.get("acted") or ""), "acted 说清做了什么")

        for did, (h_, p_) in REAL.items():
            r = _conn_of(did) or {}
            t.eq(r.get("host"), h_, f"★ 启用后 {did} 的真实地址仍在")
            t.eq(r.get("port"), p_, f"★ 启用后 {did} 的真实端口仍在")
            t.eq(r.get("source_kind"), "sim", f"{did} 形态已切 sim")
        up1 = call("GET", "/api/access", user=viewer_u)[1]["upstream"]["link"]
        t.eq(up1["listen_port"], real_up["listen_port"],
             "★ 上端监听口是链路属性，模拟态下不变")
        t.eq(up1["common_addr"], real_up["common_addr"],
             "★ 公共地址 CA 同样是链路属性，不变")

        # ---- G9c. ★ 换一套：只换数据，不换形态 ----
        p1 = s1["preset"]
        st, s2 = call("POST", "/api/sim-source", {"rotate": True},
                      user=operator_u)
        t.eq(st, 200, "换一套 200")
        t.eq(s2.get("downstream"), "sim",
             "★ 换一套之后下端**仍是**模拟源（不是被悄悄切回真实）")
        t.eq(s2.get("upstream"), "sim",
             "★ 换一套之后上端**仍是**模拟源")
        t.eq(s2.get("enabled"), True,
             "★ 换一套之后整体仍是模拟态（这是本节存在的原因）")
        t.ok(s2["preset"] != p1, f"套号确实前进了（{p1} → {s2['preset']}）")
        t.eq(s2["port_base"], simconf.preset_of(s2["preset"])["port_base"],
             "端口段与套号一致（不是两处各算一遍）")
        p2 = s2["preset"]

        st, s3 = call("POST", "/api/sim-source", {"rotate": True},
                      user=operator_u)
        t.eq(s3.get("enabled"), True, "连换两次之后仍在模拟态")
        t.ok(s3["preset"] != p2, "第二次换套套号继续前进")

        # 真实值在两次换套之后仍然一字未动
        for did, (h_, p_) in REAL.items():
            r = _conn_of(did) or {}
            t.eq(r.get("host"), h_, f"★ 换两套后 {did} 真实地址一字未动")
            t.eq(r.get("port"), p_, f"★ 换两套后 {did} 真实端口一字未动")

        # ---- G9d. 关闭 → 两端回真实，真实值原样还在 ----
        st, s4 = call("POST", "/api/sim-source", {"enable": False},
                      user=operator_u)
        t.eq(st, 202, "关闭模拟源 → 202（收链路同样交给后台）")
        t.eq(s4.get("enabled"), False, "关闭后 enabled=False")
        t.eq(s4.get("downstream"), "real", "下端回到真实源")
        t.eq(s4.get("upstream"), "real", "上端回到真实源")
        for did, (h_, p_) in REAL.items():
            r = _conn_of(did) or {}
            t.eq(r.get("host"), h_, f"★ 关闭后 {did} 真实地址仍在（没被清零）")
            t.eq(r.get("port"), p_, f"★ 关闭后 {did} 真实端口仍在（没被清零）")
            t.ok(r.get("sim_host") and r.get("sim_port"),
                 f"★ {did} 的模拟值也保留着（关掉不留痕 ≠ 把填过的都删了）")
        up4 = call("GET", "/api/access", user=viewer_u)[1]["upstream"]["link"]
        t.eq(up4["listen_port"], real_up["listen_port"], "关闭后监听口仍是原值")
        t.eq(up4["common_addr"], real_up["common_addr"], "关闭后 CA 仍是原值")

        # ---- G9e. 套号循环：转满一圈回到起点，不越界 ----
        idx_before = simconf.index_read(conn)
        for _ in range(simconf.preset_count()):
            call("POST", "/api/sim-source", {"rotate": True}, user=operator_u)
        t.eq(simconf.index_read(conn), idx_before,
             f"★ 转满一圈（{simconf.preset_count()} 次）回到原套号（模回绕，不越界）")
        t.eq(simconf.preset_of(simconf.preset_count())["port_base"],
             simconf.PRESETS[0]["port_base"], "★ 套号取模不越界（preset_of 自带回绕）")

        # ---- G9f. 真实态下 rotate：不该把人悄悄推进模拟态 ----
        call("POST", "/api/sim-source", {"enable": False}, user=operator_u)
        st, sr = call("POST", "/api/sim-source", {"rotate": True},
                      user=operator_u)
        t.eq(sr.get("enabled"), False,
             "★ 真实态下换一套：**不会**被悄悄切进模拟态")
        t.eq(sr.get("downstream"), "real", "真实态下换一套，下端还是 real")

        # ---- G9g. 参数与权限 ----
        st, _ = call("POST", "/api/sim-source", {}, user=operator_u)
        t.eq(st, 400, "空 body 400（要说明 enable 还是 rotate）")
        st, _ = call("POST", "/api/sim-source", {"enable": "yes"},
                     user=operator_u)
        t.eq(st, 400, "★ enable 非布尔 400（'yes' 不是 true）")
        st, _ = call("POST", "/api/sim-source", {"enable": True}, user=viewer_u)
        t.eq(st, 403, "viewer 不能启用模拟源（operator 起）")
        st, _ = call("POST", "/api/sim-source", {"rotate": True}, user=viewer_u)
        t.eq(st, 403, "viewer 不能换一套")

        # ---- G9h. ★ 「已停用」与「未配置」不塌成同一个状态 ----
        call("PUT", "/api/devices/DEV-PV-01/conn",
             {"source_kind": "sim", "sim_host": "127.0.0.1", "sim_port": 1502,
              "enabled": False}, user=operator_u)
        st, av = call("GET", "/api/access", user=viewer_u)
        pv = _conn_of("DEV-PV-01") or {}
        t.eq(pv.get("state"), "off",
             "★ 地址齐了但 enabled=0 ⇒ state='off'（不是被塌成 unset）")
        t.ok(pv.get("target", {}).get("host"),
             "★ 这一行有可连的目标 —— 与「已停用」自洽，不与「未配置」打架")
        c9 = av["downstream"]["counts"]
        t.eq(sum(c9[k] for k in ("real", "sim", "unset", "off")),
             len(av["downstream"]["items"]),
             "★ 四态计数之和 = 接入记录数（加了 off 之后仍不漏账）")
        t.eq(c9.get("off"), 1, "off 计数 = 1（只有 DEV-PV-01）")
        # 复原，别把停用状态留给后面的节
        call("PUT", "/api/devices/DEV-PV-01/conn",
             {"source_kind": "real", "enabled": True}, user=operator_u)

        # ---- G9i. 落盘：模拟态下两份文件的对端都写明是模拟 ----
        call("POST", "/api/sim-source", {"enable": True}, user=operator_u)
        # ★ 不能用模块名 uplinkconf：main() 后面有一处**局部** import uplinkconf，
        #   Python 会把同名变量判成局部变量，这里就成了 UnboundLocalError。
        #   用别名绕开，不改那一处（它夹在旧库迁移那节里，动它不划算）。
        import uplinkconf as _ulc
        dn_file = os.path.join(pmdir9, connconf.CONN_NAME)
        up_file9 = os.path.join(pmdir9, _ulc.LINK_NAME)
        if os.path.isfile(dn_file):
            t.ok("模拟源" in io.open(dn_file, encoding="utf-8-sig").read(),
                 "★ active.conn 文件头写明对端是模拟源")
        if os.path.isfile(up_file9):
            t.ok("模拟调度" in io.open(up_file9, encoding="utf-8-sig").read(),
                 "★ active.upstream 文件头写明对端是模拟调度")
        call("POST", "/api/sim-source", {"enable": False}, user=operator_u)
    finally:
        if old_map_dir9 is None:
            os.environ.pop(pointmap.ENV_MAP_DIR, None)
        else:
            os.environ[pointmap.ENV_MAP_DIR] = old_map_dir9

    print("\n[G10] 模拟链路的进程（v1.10：把「表」和「进程」分开）")
    # =================================================================
    # ★ 这一段守的是**一个区分**：
    #   「换一套」改的是**表**（端口段），「换一个进程」改的是**真的在跑的进程**。
    #   两者混成一个按钮，就会重演用户实测到的那一幕：点了启用，表全填好了、
    #   颜色也变了，可页面上仍然一行数据都没有 —— 因为没有任何进程在应答。
    #
    # ★ 自检里**不真拉进程**（EMS_SIMPROC_OFF=1）：一次起 9 个进程、占 6 个端口，
    #   还要写实录入库。这里验的是"该起什么、报什么、端口对不对"；
    #   "真起来能跑出数据"已由 2026-10-19 的浏览器 + 命令行实测（见 CHANGES §86）。
    old_map_dir10 = os.environ.get(pointmap.ENV_MAP_DIR)
    pmdir10 = os.path.join(tmpdir, "point-map-proc")
    os.environ[pointmap.ENV_MAP_DIR] = pmdir10
    try:
        # ---- G10a. 形状：进程表 + "表/进程是否同一套"的判据 ----
        st, s1 = call("GET", "/api/sim-source", user=viewer_u)
        t.eq(st, 200, "GET /api/sim-source 带进程状态")
        t.ok(isinstance(s1.get("chain"), dict), "chain 字段存在（进程与配置同一张脸）")
        t.ok("procs" in (s1.get("chain") or {}), "chain.procs 是清单")
        t.ok(isinstance(s1["chain"].get("counts"), dict)
             and set(s1["chain"]["counts"]) == {"up", "expected", "total"},
             "counts 有 up / expected / total（不搞一个模糊的「健康度」）")
        t.ok("preset_matches" in s1["chain"],
             "★ 带「表与进程是否同一套」的判据")
        t.eq(s1["chain"]["preset_matches"], True,
             "★ 进程没在跑时不算「不匹配」（否则一进页面就挂个黄条）")
        t.ok("collector" in s1, "旧的 collector 字段保留（向后兼容）")

        # ---- G10b. 停用一台 → 它不该起替身，但**要留一行说明为什么** ----
        call("PUT", "/api/devices/DEV-BMS-01/conn",
             {"source_kind": "sim", "enabled": False}, user=operator_u)
        call("POST", "/api/sim-source", {"enable": True}, user=operator_u)

        st, s2 = call("POST", "/api/sim-source/proc",
                      {"action": "start"}, user=operator_u)
        t.eq(st, 202, "POST /api/sim-source/proc start → 202（不是等它起来的 200）")
        t.eq(((s2.get("chain") or {}).get("job") or {}).get("action"), "start",
             "job.action 记下这一次的动作")
        procs = s2["chain"]["procs"]
        roles = [r["role"] for r in procs]
        for role in ("rtdb", "collector", "gateway", "master"):
            t.ok(role in roles, f"进程表含 {role}")
        t.eq(s2["chain"]["counts"]["up"], 0,
             "自检模式不真起进程（up=0，不会偷偷占端口）")

        n_dev10 = emsdb.query_value(conn, "SELECT COUNT(*) FROM device", (), 0)
        t.eq(roles.count("slave"), n_dev10,
             "★ 每台设备一行替身 —— 停用的那台也要有一行（说明为什么不启）")
        bms = [r for r in procs if r["key"] == "DEV-BMS-01"][0]
        t.ok("已停用" in bms["note"],
             "★ 停用的设备写明「不起替身」，不是静默少一行")
        t.eq(bms.get("skip"), True,
             "★ 而且标了 skip —— 否则「9/10」会被读成「掉了一个」，"
             "而实际那台本来就停用（处置完全不同：一个要查日志，一个什么都不用做）")
        t.eq(s2["chain"]["counts"]["expected"],
             len(procs) - len([r for r in procs if r.get("skip")]),
             "★ 分母 = 总行数 − skip 行（停用 / 未构建的不算「该起没起」）")

        # ---- G10c. ★ 替身端口必须与「表」逐台一致 ----
        # 这一条是本节的核心：进程与表各说各话时，屏幕上永远看不出来。
        plan_d = {p["device_id"]: p["sim_port"] for p in s2["plan"]["downstream"]}
        for r in procs:
            if r["role"] == "slave":
                t.eq(r["target"], "127.0.0.1:%d" % plan_d[r["key"]],
                     f"★ 替身端口与表一致：{r['key']}")

        # ---- G10d. 采集器只采一台，必须写在脸上 ----
        col = [r for r in procs if r["role"] == "collector"][0]
        t.ok("采 " in col["label"],
             "★ 采集器行写明采的是哪一台（单机采集是架构边界，不是故障）")

        # ---- G10e. 审计与权限 ----
        rows = emsdb.query(
            conn, "SELECT action FROM audit_log WHERE action = 'sim_proc'"
                  " ORDER BY id DESC LIMIT 3")
        t.ok(rows, "进程操作进审计（sim_proc）")
        t.eq(call("POST", "/api/sim-source/proc",
                  {"action": "start"}, user=viewer_u)[0], 403,
             "viewer 不能动进程")
        t.eq(call("POST", "/api/sim-source/proc",
                  {"action": "bogus"}, user=operator_u)[0], 400,
             "非法 action 给 400（不静默当成 stop）")

        # ---- G10f. 换一套只改表；进程没跟要能看出来 ----
        call("POST", "/api/sim-source/proc", {"action": "stop"}, user=operator_u)
        call("POST", "/api/sim-source", {"rotate": True}, user=operator_u)
        st, s3 = call("GET", "/api/sim-source", user=viewer_u)
        t.eq(s3["chain"]["counts"]["up"], 0, "stop 之后 up=0")
        t.eq(s3["chain"]["preset_matches"], True,
             "★ 进程没在跑时，「换一套」不算表/进程不一致")

        # ---- G10g. 复原 ----
        call("PUT", "/api/devices/DEV-BMS-01/conn",
             {"source_kind": "real", "enabled": True}, user=operator_u)
        call("POST", "/api/sim-source", {"enable": False}, user=operator_u)

        # ---- G10h. 下端是现场源时不许起链路 ----
        # ★ 2026-10-19 实测踩到：直接 POST start（数据源=现场）也会拉起整条链，
        #   采集器于奔着替身端口去，报「端口 1802 没人应答」，面板 8/9、页面没数。
        t.eq(call("POST", "/api/sim-source/proc",
                  {"action": "start"}, user=operator_u)[0], 409,
             "★ 下端现场源时 start 给 409（采集器只会去连替身，跟当前数据源矛盾）")
        t.eq(call("POST", "/api/sim-source/proc",
                  {"action": "stop"}, user=operator_u)[0], 202,
             "★ stop 不设门槛（任何时候都该收得掉）")

        # ---- G10i. 采集目标不许是停用设备 ----
        # 它的端口上**注定没人**：替身本来就不起，采集器只会报"没人应答"。
        # （此时 6 台形态刚被 G10g 恢复成 real，兜底分支正好是这一步。）
        conn.execute("UPDATE device_conn SET enabled = 0"
                     " WHERE device_id = 'DEV-BMS-01'")
        conn.commit()
        pick = engine.picked_sim(conn)
        t.ok(pick.get("device_id") != "DEV-BMS-01",
             "★ 采集目标跳过停用设备（DEV-BMS-01 enabled=0 ⇒ 不选它）")
        conn.execute("UPDATE device_conn SET enabled = 1"
                     " WHERE device_id = 'DEV-BMS-01'")
        conn.commit()

        # =================================================================
        # G11. 平台重启后按 PID 认领（v1.10 · §86.12）
        # -----------------------------------------------------------------
        print("\n[G11] 平台重启后按 PID 认领（后端重启 ≠ 现场进程退出）")
        # ★ 为什么这一段不去"真重启后端"：本机把后端与它的子进程放在同一个作业
        #   对象里，杀掉后端会连子进程一起回收 —— 重启等于先把要认领的东西杀掉，
        #   于是这条路径永远测不到（2026-10-19 实测，见 CHANGES §86.12）。
        #   所以直接造一份 pid 表：一条用**本进程自己的 pid**（必然活着），
        #   一条用一个不可能存在的 pid。认领必须能同时说"这个在"和"这个掉了"，
        #   而不是整表要么全绿、要么全红。
        me, ghost = os.getpid(), 999999
        emsdb.set_meta(conn, simproc.M_CHAIN, json.dumps([
            {"role": "rtdb", "key": "rtdb", "label": "RT_DB 初始化器",
             "target": "共享内存段", "pid": me, "skip": False},
            {"role": "gateway", "key": "gw", "label": "上端从站网关",
             "target": "2404", "pid": ghost, "skip": False},
        ], ensure_ascii=False))
        emsdb.set_meta(conn, simproc.M_CHAIN + "_preset", "套 4")
        conn.commit()

        fresh = simproc.SimChain()          # 全新实例 = 内存里没有 _rows
        t.eq(fresh.attach(conn), True,
             "★ 只有 pid 表也能把进程认回来（后端重启 ≠ 现场进程退出）")
        st11 = fresh.status(conn)
        t.eq(st11["attached"], True, "status 回 attached=True（界面可据此说明是认领来的）")
        t.eq(st11["counts"]["total"], 2, "认领回两行")
        alive11 = {r["role"]: r["alive"] for r in st11["procs"]}
        t.eq(alive11.get("rtdb"), True, "活着的那条认成 alive")
        t.eq(alive11.get("gateway"), False,
             "★ 已经死掉的那条认成掉了 —— 不许整表当成在跑")
        t.eq(st11["preset_name"], "套 4",
             "★ 认领时把套名一起带回来（丢了它，preset_matches 会误判成「进程没跟」）")

        # 一个都不在了：不许拿一张全红的表骗人，meta 必须一起清掉
        emsdb.set_meta(conn, simproc.M_CHAIN, json.dumps([
            {"role": "rtdb", "key": "rtdb", "label": "x", "target": "",
             "pid": ghost, "skip": False}], ensure_ascii=False))
        conn.commit()
        t.eq(simproc.SimChain().attach(conn), False,
             "★ 全死 ⇒ attach 不认领（返回 False）")
        t.eq(emsdb.get_meta(conn, simproc.M_CHAIN), "[]",
             "★ 全死 ⇒ 顺手清掉 meta（留着它，下次重启会认领一堆死 pid）")
    finally:
        if old_map_dir10 is None:
            os.environ.pop(pointmap.ENV_MAP_DIR, None)
        else:
            os.environ[pointmap.ENV_MAP_DIR] = old_map_dir10

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
    t.eq(emsdb.get_meta(oc2, emsdb.META_SCHEMA_VERSION), emsdb.SCHEMA_VERSION,
         "老库经 init_db 后 meta 版本升到当前（不要写死数字，否则每次改 schema 都要连改两处）")
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
    # ★ v1.5 三档重建之后，"device" 这个 id 已经没有了：
    #   sim（设备替身，可控）/ real（现场装置，只读）/ model（本机模型，降级）。
    #   所以这条判据改判 **control** —— 比"可不可用"更贴题：真机源能不能
    #   下发指令，决定了界面上该不该出现"下发"这类按钮。
    src = engine.source_of("real")
    t.ok(src["id"] == "real" and src["kind"] == "device",
         "现场数据源存在（id=real，kind=device）")
    t.ok(src["control"] is False and src["peer"] is False,
         "★ 现场数据源**只读**：不给下发指令的能力（control/peer 都是假）")
    t.ok(engine.source_of("sim")["control"] is True,
         "★ 设备替身源可控（control=True）—— 两者的差别在能不能下发，不在数据真假")
    t.ok(engine.source_of("device")["id"] == "sim",
         "旧 id「device」被兜底到 sim（v1.5 改名后别让老调用静默拿到空）")

    # ---- §79.5 上端链路空表回归（+5）----
    #   那个 bug 的形状：库里**没有** upstream_link 行时，切换上端形态用的是裸
    #   UPDATE（影响 0 行），于是文件说"模拟调度"、库里没行、回显按默认 real。
    #   修法是 upsert。这里把行删掉重跑一遍同样的动作，盯住那五个后果。
    try:
        keep_link = emsdb.query(conn, "SELECT * FROM upstream_link")
    except Exception:
        keep_link = []
    conn.execute("DELETE FROM upstream_link")
    conn.commit()
    try:
        st_ds, _ = call("POST", "/api/data-source",
                        {"end": "upstream", "kind": "sim"}, user=operator_u)
        t.eq(st_ds, 200, "★ 上端空表时切换数据源不报错（upsert 而不是裸 UPDATE）")
        rows_up = emsdb.query(conn, "SELECT * FROM upstream_link")
        t.eq(len(rows_up), 1, "★ 空表切换后库里**多了一行**（不是 0 行）")
        t.eq((rows_up[0]["kind"] if rows_up else None), "sim",
             "★ 那一行的 kind 跟着变（不落行就不会跟着变）")
        st_dg, dg = call("GET", "/api/data-source", user=viewer_u)
        t.eq(((dg.get("upstream") or {}).get("kind")), "sim",
             "★ 回显的上端形态不是默认 real（界面上有第二处答案就是这条挂了）")
        try:
            import uplinkconf
            txt = ""
            if os.path.isfile(uplinkconf.link_path()):
                with open(uplinkconf.link_path(), "r", encoding="utf-8-sig") as fh:
                    txt = fh.read()
            t.ok("模拟调度" in txt, "★ 落盘文件里写的是「模拟调度」")
        except OSError:
            t.ok(False, "★ 落盘文件里写的是「模拟调度」")
    finally:
        # 复原：后续段落（[H] 与 [G8]）要用这套值
        for r in keep_link:
            cols = [k for k in r.keys()]
            conn.execute(
                "INSERT OR REPLACE INTO upstream_link(" + ",".join(cols) + ") "
                "VALUES(" + ",".join(["?"] * len(cols)) + ")",
                tuple(r[c] for c in cols))
        conn.commit()
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
            # ★ 显式传 source=model：不传时按设备管理页选的数据源走，
            #   而本自检的环境里真机源不可用 —— 那会让这一段测的不再是模型源。
            st, ls = api.dispatch(conn, "POST", "/api/engine/live/start", {},
                                  {"dt_s": 1.0, "log_every": 1,
                                   "source": "model"}, operator_u)
            t.eq(st, 200,
                 "启动实时引擎 200"
                 + ("：" + str((ls or {}).get("error"))[:120] if st != 200 else ""))
            # ★ 起不来就别往下走：后面每一条都依赖这个进程在跑。
            #   直接取 ls["running"] 会让「现场环境问题」崩成一个 KeyError，
            #   看起来像测试脚本坏了 —— 那是最费排查时间的一类错。
            live_ok = st == 200 and bool(ls.get("running"))
            t.ok(live_ok, "启动后 running=True")
            if live_ok:
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

    # =================================================================
    # K. 告警分类（等级 × 来源 双维筛选 + 交叉计数）
    #
    # 为什么值得单独一层：告警中心是"出事后第一个翻开的页面"，筛选错了就会漏看
    # 故障。而这类 bug **静默** —— SQL 条件拼错不报错，只是条数为 0；界面上
    # "筛选后没有"和"真的没有告警"长得一模一样，没人会发现。
    #
    # 断言一律**不写死条数**（数据集会随导入变），而是拿一条**独立 SQL** 当标尺：
    #       处理器给出的 total  ==  直接查库数出来的行数
    # 交叉筛选再用三条"求和恒等式"证明：每个维度的计数带的是**另一个**维度，
    # 不带自己那份 —— 否则选中 FAULT 之后 WARNING 就变 0，用户再也点不回去。
    # =================================================================
    print("\n[K] 告警分类（等级 × 来源）")
    LV_ALL = ("INFO", "WARNING", "DERATED", "FAULT", "EMERGENCY")

    def _sql_n(sc, **cond):
        """独立标尺：绕过 api.py，直接查库数行数。"""
        w, a = "scenario_id = ?", [sc]
        if cond.get("level"):
            w += " AND level = ?"
            a.append(cond["level"])
        if cond.get("source"):
            w += " AND source = ?"
            a.append(cond["source"])
        return emsdb.query_value(conn, f"SELECT COUNT(*) FROM alarm WHERE {w}", a, -1)

    scs = [r["scenario_id"] for r in emsdb.query(
        conn, "SELECT DISTINCT scenario_id FROM alarm ORDER BY scenario_id")]
    t.ok(len(scs) > 0, f"alarm 表有数据可测（场景 {scs}）")

    st_a, _ = call("GET", "/api/alarms", user=admin, scenario=scs[0] if scs else "normal")
    t.eq(st_a, 200, "GET /api/alarms 状态码")
    st_s, _ = call("GET", "/api/alarms/summary", user=admin, scenario=scs[0] if scs else "normal")
    t.eq(st_s, 200, "GET /api/alarms/summary 状态码")

    for sc in scs:
        lvs = [r["level"] for r in emsdb.query(
            conn, "SELECT DISTINCT level FROM alarm WHERE scenario_id=? ORDER BY level", (sc,))]
        srcs = [r["source"] for r in emsdb.query(
            conn, "SELECT DISTINCT source FROM alarm WHERE scenario_id=? ORDER BY source", (sc,))]
        # 四个组合族：不筛 / 只等级 / 只来源 / 两者都筛
        combos = ([{}] + [{"level": x} for x in lvs] + [{"source": y} for y in srcs]
                  + [{"level": x, "source": y} for x in lvs for y in srcs])

        for c in combos:
            tag = f"[{sc}] {c or '不筛'}"
            _, al = call("GET", "/api/alarms", user=admin, scenario=sc, limit=50, **c)
            t.eq(al.get("total"), _sql_n(sc, **c), f"h_alarms total {tag} 与直接查库一致")

            _, sm = call("GET", "/api/alarms/summary", user=admin, scenario=sc, **c)
            # ① 全谱：五个等级必须都在（没发生的占 0）—— 否则界面上看不出
            #    "这一档没告警"和"这一档没统计到"的区别，两者长得一模一样。
            lvmap = {r["level"]: r["n"] for r in sm["by_level"]}
            t.ok(all(x in lvmap for x in LV_ALL), f"by_level {tag} 五档齐全（含 0 值档）")

            # ② 交叉恒等式：by_level 带的是**来源**，不带自己那份等级
            t.eq(sum(r["n"] for r in sm["by_level"]),
                 _sql_n(sc, **({"source": c["source"]} if "source" in c else {})),
                 f"by_level 之和 {tag} = 只按来源筛的条数（交叉：不带自己）")
            # ③ by_source 带的是**等级**（后端 SQL 有 LIMIT 20，超了会截断，故加前提）
            if len(srcs) <= 20:
                t.eq(sum(r["n"] for r in sm["by_source"]),
                     _sql_n(sc, **({"level": c["level"]} if "level" in c else {})),
                     f"by_source 之和 {tag} = 只按等级筛的条数（交叉：不带自己）")
            # ④ 24h 分布是**结果视图**，两个维度都应用
            t.eq(sum(r["n"] for r in sm["timeline"]), _sql_n(sc, **c),
                 f"timeline 之和 {tag} = 两维都应用后的条数")

        # 空串 = 不筛选。前端"全部"选项传的就是空串，若被当成条件会去匹配
        # source='' 的行 —— 结果恒为 0，不报错，界面只是"什么都没有"。
        _, b3 = call("GET", "/api/alarms", user=admin, scenario=sc, level="", source="", limit=1)
        t.eq(b3.get("total"), _sql_n(sc), f"[{sc}] 空串筛选 = 不筛选（total 不变）")

    # 审计里应留痕（引擎模式切换 / 引擎启停 / 仿真发起）
    acts = {r["action"] for r in auth.list_audit(conn, 200)}
    for a in ("engine_mode", "sim_run_start", "sim_run_delete",
              "engine_live_start", "engine_live_stop", "engine_live_reset"):
        t.ok(a in acts, f"审计记录动作 {a}")

    # =================================================================
    # L. 跨域(CORS) 守卫 —— 静态层 + 端到端层
    #
    # 为什么值得单独一层：§61 把无条件的 `Access-Control-Allow-Origin: *`
    # 收紧成了"默认不回 / 显式开白名单"，但**那次没有断言守着它** ——
    # 自检全程走 api.dispatch，根本不经过 HTTP 层，所以 ACAO 头写成什么样
    # 都不会有任何一条判据变红。
    #   没有断言守着的收紧，等于没有收紧：下次有人为了"页面调不通"顺手改回
    # `*`，全仓库一片绿，而洞已经开了。
    #
    # 两层是互补的，缺一层都不够：
    #   静态层（AST）   —— 抓"源码里又出现了字面量 *"。无论写在哪个文件、
    #                      是传给 send_header 还是赋给 allow_origin，都跑不掉。
    #   端到端层（真起服务发请求）—— 抓"头到底有没有发出去"。这一层静态
    #                      层证明不了：比如把 `if ao:` 改成恒真、或默认给个
    #                      非空兜底值，源码里一个 `*` 都没有，洞却还在。
    # =================================================================
    print("\n[L] 跨域(CORS) 守卫（静态 + 端到端）")
    import server as _srvmod

    SERVER_PY = os.path.join(SRC, "server.py")

    def _acao_literal_hits(path: str) -> list[tuple[str, str]]:
        """AST 扫一个 .py，只收**字面量**，返回 [(出处, 字面量), ...]。

        收两处：① send_header("Access-Control-Allow-Origin", X) 里的 X；
                ② *.allow_origin = X 里的 X。
        走变量的（正常实现就是变量 `ao`）不收 —— 那正是我们要的写法。
        """
        with io.open(path, encoding="utf-8") as f:
            tree = ast.parse(f.read(), filename=path)
        out: list[tuple[str, str]] = []
        for nd in ast.walk(tree):
            if isinstance(nd, ast.Call):
                fn = nd.func
                if (isinstance(fn, ast.Attribute) and fn.attr == "send_header"
                        and len(nd.args) >= 2
                        and isinstance(nd.args[0], ast.Constant)
                        and nd.args[0].value == "Access-Control-Allow-Origin"
                        and isinstance(nd.args[1], ast.Constant)
                        and isinstance(nd.args[1].value, str)):
                    out.append(("send_header", nd.args[1].value))
            if isinstance(nd, ast.Assign):
                for tg in nd.targets:
                    if (isinstance(tg, ast.Attribute) and tg.attr == "allow_origin"
                            and isinstance(nd.value, ast.Constant)
                            and isinstance(nd.value.value, str)):
                        out.append(("allow_origin", nd.value.value))
        return out

    t.ok(os.path.isfile(SERVER_PY), f"server.py 存在: {SERVER_PY}")

    # 扫整个 14/src，不只是 server.py —— 换个文件再开一个洞同样要被抓到
    star: list[str] = []
    for _fn in sorted(os.listdir(SRC)):
        if _fn.endswith(".py"):
            for _where, _v in _acao_literal_hits(os.path.join(SRC, _fn)):
                if _v == "*":
                    star.append(f"{_fn}({_where})")
    t.eq(star, [],
         "★ 14/src 下没有任何一处把字面量 '*' 当 ACAO 实参或 allow_origin 赋值")

    # 开关必须还在：想开能开，才不会有人去改源码
    t.eq(inspect.signature(_srvmod.create_server).parameters["allow_origin"].default,
         None, "create_server.allow_origin 默认 None（不传 = 同源模式）")
    _srv_txt = io.open(SERVER_PY, encoding="utf-8").read()
    t.ok('"--allow-origin"' in _srv_txt or "'--allow-origin'" in _srv_txt,
         "server.py 仍保留 --allow-origin 开关（不必改源码也能开白名单）")

    # ---- 端到端：真起服务、真发 HTTP 请求、真看响应头 ----
    cors_db = os.path.join(tmpdir, "cors.db")

    def _probe(srv) -> tuple[int, dict]:
        th = threading.Thread(target=srv.serve_forever, daemon=True)
        th.start()
        try:
            c = http.client.HTTPConnection("127.0.0.1",
                                           srv.server_address[1], timeout=5)
            c.request("GET", "/api/health")
            r = c.getresponse()
            r.read()
            return r.status, {k.lower(): v for k, v in r.getheaders()}
        finally:
            srv.shutdown()
            srv.server_close()

    srv_d = _srvmod.create_server("127.0.0.1", 0, db_path=cors_db, static_dir=tmpdir)
    t.eq(getattr(srv_d, "allow_origin", None), None,
         "默认起服务 = 同源模式（srv.allow_origin is None）")
    st_d, hd_d = _probe(srv_d)
    t.eq(st_d, 200, "同源模式下 GET /api/health 200（收紧没弄坏正常访问）")
    t.ok("access-control-allow-origin" not in hd_d,
         "★ 默认响应头**不带** ACAO（第三方页面无法直接调 /api/*）")

    OA = "http://127.0.0.1:5173"
    srv_o = _srvmod.create_server("127.0.0.1", 0, db_path=cors_db,
                                  static_dir=tmpdir, allow_origin=OA)
    t.eq(srv_o.allow_origin, OA, "带 --allow-origin 起服务时 srv.allow_origin 被记下")
    st_o, hd_o = _probe(srv_o)
    t.eq(st_o, 200, "白名单模式下 GET /api/health 200")
    t.eq(hd_o.get("access-control-allow-origin"), OA,
         f"白名单模式回显指定源 {OA}（且只此一个，不是 *）")
    t.ok("access-control-allow-methods" in hd_o,
         "白名单模式同时回 Allow-Methods（预检可用）")

    # =================================================================
    # G12. 入库断线：不丢数据 + 能自愈（2026-10-19 实测缺陷）
    # -----------------------------------------------------------------
    # 现场实录：尾随线程写库时撞上一次 OperationalError
    # （attempt to write a readonly database）就 break —— 采集进程还在跑、
    # 实录还在长，但一行都进不了库，而且**不会自愈**。界面上表现为
    # 「数据越等越旧，没有一个地方说入库已经断了」，用户读作「点了没反应」。
    # 更糟的是 byte_pos 在写库**之前**就推进了：那批数据连重放的机会都没有。
    print("\n[G12] 入库断线：不丢数据 + 能自愈")
    import sqlite3 as _sq
    import engine as _eng
    import importer as _imp
    _here = os.path.dirname(os.path.abspath(__file__))
    _m14 = os.path.dirname(_here)

    t.ok(_eng._is_transient_db_error(
             _sq.OperationalError("attempt to write a readonly database")),
         "★ 数据库瞬时不可写要判成「可恢复」—— 否则一次就把整条链路判死")
    t.ok(not _eng._is_transient_db_error(
             _sq.IntegrityError("UNIQUE constraint failed")),
         "★ 约束冲突不是瞬时错误：重试多少次都还是错的，必须立刻停")
    t.ok(not _eng._is_transient_db_error(TypeError("bad arg")),
         "★ 代码类错误不重试 —— 重试只会把 bug 藏进无限循环里")

    g12_db = os.path.join(tmpdir, "g12.db")
    conn12 = emsdb.init_db(g12_db)
    try:
        # 拿一段真实实录做素材：格式由 07/ 与 importer 共同约定，手造容易偏。
        live_dir = os.path.join(_m14, "data", "live")
        pick = None
        if os.path.isdir(live_dir):
            for fn in sorted(os.listdir(live_dir), reverse=True):
                if fn.startswith("live_") and fn.endswith(".csv"):
                    p_ = os.path.join(live_dir, fn)
                    if os.path.getsize(p_) > 200:
                        pick = p_
                        break
        if not pick:
            t.skip("没有可用的实录 CSV 做素材（14/data/live/live_*.csv）")
        else:
            with io.open(pick, "r", encoding="utf-8", errors="replace") as f:
                rows12 = [ln for ln in f if ln.strip()][:3]
            csv12 = os.path.join(tmpdir, "g12.csv")
            with io.open(csv12, "w", encoding="utf-8", newline="") as f:
                f.writelines(rows12)

            lv = _eng.LiveEngine()
            lv.csv_path = csv12
            lv.byte_pos = 0
            lv.rows = 0
            lv.source = "sim"
            lv.db_path = g12_db

            n1 = lv.poll_once(conn12)
            pos_ok = lv.byte_pos
            t.ok(n1 > 0 and pos_ok > 0,
                 "正常入库：进 %d 行，断点推进到 %d 字节" % (n1, pos_ok))

            # 追加一批，并让写库抛「瞬时」错误
            with io.open(csv12, "a", encoding="utf-8", newline="") as f:
                f.writelines(rows12[1:])
            orig_imp = _imp.import_live_rows

            def _boom(*a, **k):
                raise _sq.OperationalError("attempt to write a readonly database")
            _imp.import_live_rows = _boom
            pos_before = lv.byte_pos
            threw = False
            try:
                lv.poll_once(conn12)
            except _sq.OperationalError:
                threw = True
            finally:
                _imp.import_live_rows = orig_imp
            t.ok(threw, "★ 写库抛错要真的冒到 _loop —— 不冒出来就没有「重试」这一说")
            t.eq(lv.byte_pos, pos_before,
                 "★ 写库失败时断点不许推进：推进了这批数据就再也读不回来（静默丢数据）")

            n2 = lv.poll_once(conn12)
            t.ok(n2 > 0 and lv.byte_pos > pos_before,
                 "★ 库一恢复，同一批数据要能补回来（幂等重放，不丢也不重）")
    finally:
        try:
            conn12.close()
        except Exception:
            pass

    with io.open(os.path.join(_m14, "src", "engine.py"), encoding="utf-8") as f:
        src12 = f.read()
    i12 = src12.index("if _is_transient_db_error(e):")
    j12 = src12.index("continue", i12)
    t.ok("break" not in src12[i12:j12],
         "★ 可恢复错误的分支里不许出现 break —— 一 break 就再也不会重试")
    t.ok('live_state="stale"' in src12[i12:j12] and "连续重试" in src12[i12:j12],
         "★ 重试期间要标 stale 并写出重试次数：不然「一直重试」和「一直在跑」看不出区别")

    # =================================================================
    # M. 静态目录穿越：前缀相同的**兄弟目录**也不许读
    # -----------------------------------------------------------------
    # 2026-10-19 体检发现的真口子：原来的判据是
    #     full.startswith(normpath(root))
    # 而 root = ...\15 时，`...\15_backup\secret` 也以 `...\15` 开头 ——
    # 归一化 `..` 之后请求 `/../15_backup/secret` 就能读到站点根之外的兄弟目录。
    # 本机目录名恰好没有 `15xxx`，所以一直没被踩到；只要有一个人在
    # 旁边建个 `15_bak`（备份目录是最常见的），这行就成了读任意文件的入口。
    print("\n[M] 静态目录穿越（兄弟目录前缀陷阱）")
    _root = os.path.join(tmpdir, "srvo")
    _sib = os.path.join(tmpdir, "srvo-x")     # ★ 与 root **前缀相同**的兄弟
    os.makedirs(_root, exist_ok=True)
    os.makedirs(_sib, exist_ok=True)
    with open(os.path.join(_root, "ok.txt"), "w", encoding="utf-8") as f:
        f.write("PUBLIC")
    with open(os.path.join(_sib, "secret.txt"), "w", encoding="utf-8") as f:
        f.write("SECRET-MUST-NOT-LEAK")

    srv_m = _srvmod.create_server("127.0.0.1", 0, db_path=cors_db,
                                  static_dir=_root)
    threading.Thread(target=srv_m.serve_forever, daemon=True).start()

    def _get(srv, path):
        import http.client
        c = http.client.HTTPConnection("127.0.0.1", srv.server_address[1],
                                       timeout=5)
        try:
            c.request("GET", path)
            r = c.getresponse()
            return r.status, r.read().decode("utf-8", "replace")
        finally:
            c.close()

    st_m, body_m = _get(srv_m, "/../srvo-x/secret.txt")
    t.eq(st_m, 403,
         "★ /../<前缀相同的兄弟目录>/x 必须 403（旧判据 startswith 会放行）")
    t.ok("SECRET" not in body_m,
         "★ 而且**一个字节都不能回**（403 但把文件内容写进 body 更糟）")
    st_k, body_k = _get(srv_m, "/ok.txt")
    t.eq(st_k, 200, "同目录下的正常文件仍然 200（收紧没把静态服务弄坏）")
    t.eq(body_k, "PUBLIC", "正常文件内容原样返回")
    srv_m.shutdown()
    srv_m.server_close()

    for _p in (os.path.join(_root, "ok.txt"), os.path.join(_sib, "secret.txt")):
        try:
            os.remove(_p)
        except OSError:
            pass
    for _d in (_root, _sib):
        try:
            os.rmdir(_d)
        except OSError:
            pass

    # 临时库要清掉：tmpdir 后面要 os.rmdir，留文件会让它静默失败
    for _sfx in ("", "-wal", "-shm"):
        _p = cors_db + _sfx
        if os.path.isfile(_p):
            try:
                os.remove(_p)
            except OSError:
                pass

    # ------------------------------------------------------------------
    # [N] 链路启动：等待窗口**整组共用**，且能靠证据提前返回
    #
    # 为什么单列一段：2026-10-19 实测，点一次「启用模拟源」HTTP 请求要 25.4 s
    # 才返回 —— 5 台设备替身**各**盲等 2.5 s（12.5 s），加上采集器死睡
    # 3.6 s、主站 3.0 s。用户看到的就是"点了没反应，过一会才有反应"，反馈了
    # 三次才查到这里。这一段守的是「一个窗口覆盖整组 + 端口/CSV 可连就走」，
    # 而不是"把 2.5 调成 1.2"那种改法 —— 后者多加几台设备就又慢回去。
    # ------------------------------------------------------------------
    print("\n[N] 链路启动：整组一次等待（不是每台各等一遍）")

    class _FakeProc:
        def __init__(self, alive=True):
            self.alive = alive

        def poll(self):
            return None if self.alive else 1

    def _frows(n):
        return [{"proc": _FakeProc(), "key": "k%d" % i, "role": "slave"}
                for i in range(n)]

    # ★ backlog 要够大：探测是"每 0.1 s 连一次"，连接进 accept 队列后没人
    #   accept 就一直占着位置，队列满了就会连不上（真实服务端会 accept，
    #   这里只是个探针 socket）。这不是被测代码的问题，是桩子自己的。
    _srv = socket.socket()
    _srv.bind(("127.0.0.1", 0))
    _srv.listen(128)
    _port = _srv.getsockname()[1]
    try:
        W = simproc.SimChain._wait_ready
        t.eq(simproc._port_open(_port), True,
             "端口在 listen ⇒ _port_open 说通（判据是连得上，不是睡够了）")
        t.eq(simproc._port_open(_port + 1), False,
             "没人 listen 的端口 ⇒ False")
        t.eq(W([], [], 0.2), [], "空进程表：立刻返回")
        t.eq(W([{"proc": None}], [], 0.2), [], "没有进程句柄：立刻返回")

        # ★ 核心：5 台一起等，端口一通就走 —— 不是 5 × 2.5 = 12.5 s
        _t0 = time.time()
        _got = W(_frows(5), [_port], 3.0)
        _dt = time.time() - _t0
        t.eq(_got, [], "5 台都活着、端口已通 ⇒ 没有要报的")
        t.ok(_dt < 0.5, f"★ 5 台整组只等一次：{_dt:.2f}s < 0.5s")

        # 有一台秒退 ⇒ 必须抓到（不能因为"不等了"就漏报）
        _bad = _frows(3)
        _bad[1]["proc"].alive = False
        _t0 = time.time()
        _got = W(_bad, [], 3.0)
        t.eq([r["key"] for r in _got], ["k1"],
             "★ 有一台启动即退出 ⇒ 照样抓得到")
        t.ok(time.time() - _t0 < 1.0, "抓到秒退就提前返回，不必等满")

        # 端口一直不通 ⇒ 等满上限（不谎报"起来了"）
        _t0 = time.time()
        _got = W(_frows(2), [_port + 1], 0.5)
        t.eq(_got, [], "端口不通但进程都活着 ⇒ 不报退出")
        t.ok(time.time() - _t0 >= 0.4, "端口不通 ⇒ 等满上限才返回")

        # 采集器：CSV 写出第一拍就走，不再死睡 3.6 s
        FR = engine.LiveEngine._wait_first_row
        _csv = os.path.join(tmpdir, "first_row.csv")
        open(_csv, "w").write("t,P\n0,1\n")
        t.eq(FR(_FakeProc(), _csv, 3.6), True, "CSV 已有第一拍 ⇒ 判它跑起来了")
        _t0 = time.time()
        FR(_FakeProc(), _csv, 3.6)
        t.ok(time.time() - _t0 < 0.3,
             f"★ 有第一拍就不再等：{time.time() - _t0:.2f}s < 0.3s"
             f"（原来是死睡 3.6 s）")
        t.eq(FR(_FakeProc(False), _csv, 3.6), False,
             "进程已退出 ⇒ 立刻报 False（调用方去取日志）")
        t.eq(FR(_FakeProc(), os.path.join(tmpdir, "no_such.csv"), 0.3), True,
             "等满仍活着 ⇒ 算起来了（与原逻辑一致：活着就算）")

        # 常量层面：这些数是**上限**，不是每台必付的固定开销
        t.ok(simproc.SLAVE_LISTEN_S <= 3.0, "替身等待窗口 ≤ 3 s（它是上限）")
        t.eq(engine.COLLECTOR_UP_SIM_S, 3.6, "采集器兜底上限仍是 3.6 s（判死需要）")
    finally:
        _srv.close()
        try:
            os.remove(os.path.join(tmpdir, "first_row.csv"))
        except OSError:
            pass

    conn.close()

    # ---------------------------------------------------------------
    # [P] 「最后一次通知用户」不许静默
    #
    # ★ 这一节守的不是某个具体功能，是一条**纪律**：
    #   凡ซึ่ง「告诉用户发生了什么」的代码自己失败时，必须留下痕迹。
    #   三次 me Too 的病灶都在这里：界面说「一切正常」，底下其实早就断了。
    # ---------------------------------------------------------------
    import engine as _o_eng

    # ---- P1. note_critical 自身的两条硬约束 ----
    t.ok(hasattr(_o_eng, "note_critical"), "engine 有兜底告警出口 note_critical")
    _o_logdir = tempfile.mkdtemp(prefix="ems_crit_")
    _o_realroot = _o_eng.emsdb.MODULE_ROOT
    try:
        _o_eng.emsdb.MODULE_ROOT = os.path.join(_o_logdir, "mod")
        _o_eng.note_critical("t.unit", "自检写入的一行")
        _o_p = os.path.join(_o_logdir, "mod", "data", "critical.log")
        t.ok(os.path.isfile(_o_p), "★ note_critical 一定落盘"
                                 "（后端常以后台方式拉起，stderr 常常没人接）")
        _o_txt = io.open(_o_p, encoding="utf-8").read() if os.path.isfile(_o_p) else ""
        t.ok("t.unit" in _o_txt and "自检写入的一行" in _o_txt,
             "落盘内容带上 tag 与正文")
        t.ok(len(_o_txt.splitlines()) == 1, "一次调用只写一行")
        # 再写一条：必须**追加**，不能覆盖上一条
        _o_eng.note_critical("t.unit2", "第二条")
        _o_txt2 = io.open(_o_p, encoding="utf-8").read()
        t.ok("自检写入的一行" in _o_txt2 and "第二条" in _o_txt2, "多次调用是追加不是覆盖")

        # ② 自身永远不抛 —— 因为调用它的地方全是在 except 里
        _o_eng.emsdb.MODULE_ROOT = os.path.join(_o_logdir, "\0非法路径")
        try:
            _o_eng.note_critical("t.unit3", "路径不可用时也不能炸")
            t.ok(True, "★ 落盘失败时 note_critical 自己吞掉、不上抛"
                       "（调用点全在 except 里，一炸就把原异常顶掉）")
        except Exception as e:
            t.ok(False, f"note_critical 不该抛异常，却抛了 {type(e).__name__}")
    finally:
        _o_eng.emsdb.MODULE_ROOT = _o_realroot
        shutil.rmtree(_o_logdir, ignore_errors=True)

    # ---- P2. 三处「状态写不进去」不再是 pass ----
    _o_esrc = io.open(os.path.join(SRC, "engine.py"), encoding="utf-8").read()
    _o_ncrit = len(re.findall(r"note_critical\(", _o_esrc))
    t.ok(_o_ncrit >= 3, f"★ _persist 失败的三处都被记下来（找到 {_o_ncrit} 处调用）")
    t.ok(not re.search(r"conn\.close\(\)\s*\n\s*except Exception:\s*\n\s*pass", _o_esrc),
         "★ 不许再出现「写完库 close 之后 except: pass」"
         "（那是把「为什么停了」咽掉，界面会停在旧状态上装作没事）")

    # ---- P3. 客户端断开不许刷满日志 ----
    _o_ssrc = io.open(os.path.join(SRC, "server.py"), encoding="utf-8").read()
    t.ok("class ApiServer(ThreadingHTTPServer)" in _o_ssrc,
         "★ 有 ApiServer 子类（要覆盖的是 **Server** 的 handle_error，不是 Handler 的）")
    t.ok("def handle_error" in _o_ssrc, "覆盖了 handle_error")
    t.ok("ConnectionResetError" in _o_ssrc and "BrokenPipeError" in _o_ssrc,
         "★ 客户端先断开（WinError 10054）不再打堆栈"
         "—— 实测日志 1228 行里 52 个 Traceback 全是它，真错误被淹没")
    t.ok("ApiServer((host, port), Handler)" in _o_ssrc,
         "create_server 实际用的就是 ApiServer（覆盖了但没用上等于没改）")

    # ---- P4. 文件回滚失败不许冒充「已整体回滚」 ----
    _o_asrc = io.open(os.path.join(SRC, "api.py"), encoding="utf-8").read()
    _o_m = re.search(r"def _file_restore\(prev: dict\) -> None:\s*\n(.*?)(?=\ndef |\nclass )",
                   _o_asrc, re.S)
    _o_frbody = _o_m.group(1) if _o_m else ""
    t.ok(len(_o_frbody) > 0, "_file_restore 存在")
    t.ok("note_critical" in _o_frbody,
         "★ 回滚失败必须留痕：调用方会对用户说「已整体回滚」，"
         "文件没退回去却静默 = 那句话是假话（两边看起来都成功过）")
    t.ok(not re.search(r"except OSError:\s*\n\s*pass", _o_frbody),
         "_file_restore 里不再有裸的 except OSError: pass")

    # ---------------------------------------------------------------
    # [Q] 时刻标签：一天的最后一拍不许折回 00:00
    #
    # ★ 起因是用户原话「实时检控永远是 0:00」（2026-10-19）。两层原因：
    #   ① `default_scenario` 跟引擎模式走，默认看的是离线日仿真 sim-d002；
    #   ② 那份数据的末一拍 t_s = 86400，老写法 `(m//60) % 24` 把它折成 "00:00"，
    #      和当天第一拍一模一样 —— 读数是"不动的 0:00"，看着就是系统卡了。
    #   ★ 所以这一段的判据盯着"标签会不会骗人"，不是盯着"函数存在不存在"。
    # ---------------------------------------------------------------
    import api as _o_apimod

    t.eq(_o_apimod._hhmm(86400), "24:00",
         "★ 24 h 仿真的末一拍 t_s=86400 → 24:00（老写法折回 00:00，与首拍同标签）")
    t.eq(_o_apimod._hhmm(0), "00:00", "第 0 拍仍是 00:00（起点没被改坏）")
    t.eq(_o_apimod._hhmm(43200), "12:00", "12:00 不受影响")
    t.eq(_o_apimod._hhmm(86399), "23:59", "23:59 不受影响")
    t.eq(_o_apimod._hhmm(86401), "D2 00:00",
         "★ 跨过午夜带 D 前缀：周 / 月仿真不能再被压进同一天")
    t.eq(_o_apimod._hhmm(604800), "D7 24:00", "★ 周仿真末拍 = 第 7 天 24:00")
    t.eq(_o_apimod._hhmm(None), "", "坏值返回空串，不抛异常")

    # ★ 只查 _hhmm **函数体**：注释里为了说明问题不得不写出那个旧式子，
    #   全文件扫会被自己的注释命中（本次就是这么翻的车）。
    _o_hm = re.search(r"def _hhmm\(t_s: Any\) -> str:\s*\n(.*?)(?=\ndef |\nclass )",
                      _o_asrc, re.S)
    _o_hbody = _o_hm.group(1) if _o_hm else ""
    # ★ 连 docstring 一起剥掉：本次的两连败都栽在"注释 / 文档串里为了说明
    #   问题必须写出那个旧式子"—— 判据要打**实际执行的那几行**，不是整个块。
    _o_hcode = re.sub(r'""".*?"""', "", _o_hbody, flags=re.S)
    t.ok(len(_o_hcode.strip()) > 0, "_hhmm 存在")
    t.ok(not re.search(r"%\s*24\b", _o_hcode),
         "★ _hhmm 的可执行代码里不再有 %24 折回（折回即「末拍 = 首拍」）")
    t.ok(_o_asrc.count("_time_label(conn, sc, row)") >= 2,
         "★ realtime / overview 两处的时刻都走 _time_label 由 t_s 现算")
    t.ok('"time": row["time_str"]' not in _o_asrc,
         "★ 没有残留的 time: row[time_str] —— 那份 HH:MM 是 10/ 写的，"
         "86400 这一拍在它里面就是 00:00")


    # =================================================================
    print("\n[R] 接入调度主站九步向导")
    # =================================================================
    # 这一段守的不是"有没有向导"，是那几件**做错了会骗人**的事：
    #   · 九步清单只有 accessconf 一份（界面渲染 + 完成校验 + CSV 导出都用它）；
    #   · 不预填结论（预填「通过」= 系统替人签字）；
    #   · 「填了就存」与「完成」分开：PUT 不拦，completed=true 才校验；
    #   · 缺项被拒时要说清第几步缺什么，且**不能半路落库**；
    #   · 「重置」只清草稿，不动链路参数与转发表。
    import accessconf as _ac

    # ★ [P] 段收尾时把 conn 关了（那段只做源码判据，不需要连接）。
    #   这一段要真读写库，重新开一条 —— 别去改那段的收尾，它关连接是有意的。
    conn = emsdb.connect(db_path)

    t.eq(len(_ac.STEPS), 9, "九步定义是 9 步（改步数是产品决定，要同步改文档与前端文案）")
    t.eq([x["num"] for x in _ac.STEPS], list(range(1, 10)),
         "步骤号 1..9 连续（上一步/下一步靠它算，缺口会跳错页）")
    t.ok(all(x.get("key") and x.get("title") and x.get("fields") for x in _ac.STEPS),
         "每一步都有 key / title / fields")
    t.eq([x["num"] for x in _ac.STEPS if x.get("config")], [6],
         "★ 只有第 6 步标了 config —— 只有它改软件配置，其余八步是台账")

    _allf = [f for x in _ac.STEPS for f in x["fields"]]
    t.ok(not any(f.get("default") == "pass" for f in _allf),
         "★ 没有字段预填「通过」（预填结论就是系统替人签字）")
    t.ok(not any(f.get("default") in ("no", "yes") for f in _allf),
         "★ 也没有预填是/否（替用户断言「还没做」同样是假记录）")

    _blank = _ac._blank()
    t.ok(not _blank["completed"] and _blank["current_step"] == 1,
         "新草稿是未完成、停在第 1 步")
    _pg = _ac.progress(_blank["steps"])
    t.ok(_pg["required"] > 0 and not _pg["ready"], "空草稿必填未齐 ⇒ ready=False")
    t.eq(_pg["filled"], 1, "空草稿只有「监听端口 2404」这一个客观默认值算已填")
    _miss = _ac.missing(_blank["steps"])
    t.eq(len(_miss), _pg["required"] - 1, "缺项数 = 必填数 − 已填数（两个口径必须一致）")
    t.eq(_miss[0]["step"], 1, "第一个缺项落在第 1 步（跟阅读顺序一致，才好跳回去）")

    # 条件必填要跨步生效：第 4 步的「通道 B」依赖第 1 步的「通道数」
    _f = dict(_ac._blank()["steps"])
    _f["1"] = dict(_f["1"]); _f["4"] = dict(_f["4"])
    _chb = [x for x in _ac.STEPS[3]["fields"] if x["k"] == "ch_b"][0]
    _f["1"]["channels"] = "双通道"
    t.ok(_ac._required(_chb, _f["4"], _f),
         "★ 选「双通道」后第 4 步的「通道 B」变必填（跨步依赖真的生效）")
    _f["1"]["channels"] = "单通道"
    t.ok(not _ac._required(_chb, _f["4"], _f),
         "★ 选「单通道」后它不再是必填（不是一律必填）")

    # 脏数据不许静默进库
    _cl = _ac._clean({"1": {"dispatch": "X", "zzz": "脏"}, "99": {"a": 1}})
    t.ok("zzz" not in _cl["1"] and "99" not in _cl,
         "★ _clean 只留 schema 里的键（未知键与未知步骤一律丢掉）")
    t.eq(_cl["1"]["dispatch"], "X", "_clean 保留合法值")

    # 台账 CSV
    _csv = _ac.to_csv({"steps": _blank["steps"], "username": "t",
                       "completed": False, "updated_at": "2026-10-20 10:00:00"})
    t.ok("接入调度主站资料台账" in _csv, "台账 CSV 有标题")
    t.ok("管辖权与接入方案" in _csv, "台账 CSV 带步骤名")
    t.ok("缺项合计" in _csv, "★ 台账 CSV 末尾列缺项合计（导出件自己要看得出缺什么）")

    # 路由四条
    _routes = {(m, pat.pattern) for m, pat, _h, _r in api.ROUTES}
    for _m, _pat in (("GET", r"/api/access-draft"), ("PUT", r"/api/access-draft"),
                     ("DELETE", r"/api/access-draft"),
                     (r"GET", r"/api/access-draft\.csv")):
        t.ok((_m, _pat) in _routes, "路由存在：" + _m + " " + _pat)

    # 缺项时不许标记完成，而且不能半路落库
    _u = {"username": "t"}
    try:
        api.h_access_draft_put(conn, {}, {"steps": _blank["steps"],
                                          "current_step": 2, "completed": True}, _u, {})
        t.ok(False, "★ 缺项时 completed=true 必须被拒（400）")
    except api.ApiError as _e:
        t.ok(_e.status == 400 and "项必填未填" in _e.message,
             "★ 缺项时 completed=true 被 400 拒，且说清缺多少项")
        t.ok("第 1 步" in _e.message, "★ 提示里点名是第几步缺什么（不是只说\"还有没填的\"）")
    t.ok(not _ac.load_draft(conn)["completed"],
         "★ 被拒之后库里 completed 仍是 False（没有半路落库）")

    # 填了就存：不要求填齐
    _st, _out = api.h_access_draft_put(conn, {}, {"steps": _blank["steps"],
                                                  "current_step": 3}, _u, {})
    t.eq(_st, 200, "★ 没填齐也能存（PUT 不拦 —— 现场资料常常还没拿到）")
    t.eq(_out["draft"]["current_step"], 3, "停在哪一页也存下来了（断点续填靠它）")

    # 全填 -> 可以完成
    _full = _ac._blank()["steps"]
    for _s in _ac.STEPS:
        for _fd in _s["fields"]:
            _ty = _fd["type"]
            _v = ("3" if _ty == "int" else "2026-10-20" if _ty == "date"
                  else "2026-10-20T10:00" if _ty == "time"
                  else "yes" if _ty == "yesno"
                  else _fd["opts"][0][0] if _ty == "select" else
                  ([_fd["opts"][0][0]] if _ty == "multi" else "v"))
            _full[_s["key"]][_fd["k"]] = _v
    _st, _out = api.h_access_draft_put(conn, {}, {"steps": _full, "current_step": 9,
                                                  "completed": True}, _u, {})
    t.eq(_st, 200, "九步必填全齐后可以标记完成")
    t.ok(_out["draft"]["completed"] is True, "completed 落库为 True")
    t.ok(bool(_out["draft"]["completed_at"]), "★ 完成时记了时间（将来查得到是哪天填完的）")
    t.eq(_out["missing"], [], "全填后缺项为空")

    # 改内容 -> 完成状态必须失效（PUT completed=false 清 completed_at）
    _st, _out = api.h_access_draft_put(conn, {}, {"steps": _full, "current_step": 9,
                                                  "completed": False}, _u, {})
    t.ok(_out["draft"]["completed"] is False and _out["draft"]["completed_at"] == "",
         "★ 退回未完成时 completed_at 一并清掉（留着就是个说不清的假记录）")

    # 重置：只清草稿，话里要说清没动配置
    _st, _out = api.h_access_draft_delete(conn, {}, {}, _u, {})
    t.eq(_st, 200, "DELETE 返回 200")
    t.ok("active.upstream" in _out.get("note", "") and "pointmap" in _out.get("note", ""),
         "★ 重置返回的话里写明没动链路参数与转发表（否则会被当成恢复默认配置）")
    _after = _ac.load_draft(conn)
    t.ok(_after["updated_at"] == "" and _after["current_step"] == 1,
         "★ 重置是真空了（不是只清了个标志位）")

    # 坏 JSON 不许把页面带崩
    emsdb.set_meta(conn, _ac.DRAFT_KEY, "{ not json")
    conn.commit()
    t.ok(_ac.load_draft(conn)["current_step"] == 1,
         "★ 库里草稿存坏了 ⇒ 退回空白草稿，不抛（一个坏 JSON 不该让整页打不开）")

    # ---- v1.15：每步的材料清单 / 建议值 / 本步导出（八步不再只是空表单） ----
    t.eq(len(_ac.DOCS), 9, "九步每步都有一份材料清单定义（DOCS）")
    t.ok(all(d.get("mats") and len(d["mats"]["cols"]) >= 2
             and len(d["mats"]["rows"]) >= 3 for d in _ac.DOCS.values()),
         "★ 每步的清单都有 ≥2 列、≥3 行 —— 一页只有两三个输入框，那不叫详细")
    t.ok(all(d.get("export", {}).get("name") for d in _ac.DOCS.values()),
         "★ 九步各有自己的导出件名字（不再只有一个总台账）")

    _ctx = {"today": "2026-10-20", "port": 2404, "ca": 1, "bind": "0.0.0.0",
            "up": 24, "down": 6, "poll": "1000", "down_first": "有功功率设定",
            "pm": [{"ioa": 4001, "kind": "MEAS_FLOAT", "name": "负荷有功",
                    "src": "MEAS.P_LOAD", "scale": "1.0", "offset": "0.0",
                    "unit": "kW", "direction": "up"},
                   {"ioa": 5001, "kind": "MEAS_FLOAT", "name": "有功功率设定",
                    "src": "CMD.P_BAT", "scale": "1.0", "offset": "0.0",
                    "unit": "kW", "direction": "down"}]}
    _d2 = _ac.detail(2, _ctx)
    t.ok("24 上送" in _d2["mats"]["rows"][0][-1],
         "★ 清单第三列「现状」是真数字（从转发表读的 24 上送 / 6 下行）")
    # 填不上的占位符必须显示「-」，不能把 {pmfile} 这种字面量画到界面上。
    # ★ 原来拿第 1 步的 {channels} 验这条；第 1 步末列已改成「找谁确认」（不含占位符），
    #   所以换到第 6 步第 3 行的 {pmfile} —— 性质不变，落点跟着数据走。
    t.eq(_ac.detail(6, {})["mats"]["rows"][2][-1], "-",
         "★ 填不上的占位符显示「-」，不许把 {pmfile} 这种字面量画到界面上")
    t.ok(any("CT 变比" in r[0] for r in _d2["mats"]["rows"]),
         "第 2 步清单里点名了变比（DL/T 516-2017 7.2 里最常被漏掉的那几项）")

    # 第 6 步：这张转发表容易让人以为是「交给调度的文件」——
    #   它其实是本机的对照表，而 IOA 号是调度定的。清单里必须把这两点写清，
    #   还得把「文件到底落没落盘」显示出来（没落盘时网关读的是内置默认表）。
    _d6 = _ac.detail(6, _ctx)
    t.eq(_d6["mats"]["cols"][-1], "现状",
         "★ 第 6 步也有「现状」列 —— 否则人不知道这几个文件落没落盘")
    t.ok("{pmfile}" in str(_ac.DOCS["6"]["mats"]),
         "★ 第 6 步暴露 {pmfile}：转发表没落盘时网关用的是内置表，"
         "不写出来人会以为界面保存了就等于生效")
    # 第 1/3/4/5 步的清单末列原来回显草稿（{dispatch} / {cert_exp} / {vlan} …），
    # 而草稿要等自动保存 + 重开才刷新 ⇒ 整列常年「-」。
    # 一整列空着的格子会被读成「软件没存住我刚敲的字」，比没有这一列更糟。
    for _n in (1, 3, 4, 5):
        _dd = _ac.detail(_n, {})
        t.ok(all(str(r[-1]).strip() != "-" for r in _dd["mats"]["rows"]),
             "★ 第 %d 步清单末列不靠草稿回显 —— 空 ctx 下也不许整列显示「-」" % _n)
    t.ok(any("不是交给调度的文件" in x for x in _ac.DOCS["6"]["tips"]),
         "★ 明写「转发表不是交付件，交付件是第 2 步的《远动信息参数表》」")
    t.ok(any("下端 Modbus 点表" in x for x in _ac.DOCS["6"]["tips"]),
         "★ 提醒 active.csv 是下端点表，与 active.pointmap.csv 只差一个点号（改错打坏采集）")

    _c6 = api._access_ctx(conn)
    t.ok("pmfile" in _c6, "_access_ctx 要给出 pmfile（网关此刻读的是哪份表）")
    t.ok(("未落盘" in _c6["pmfile"]) or ("已落盘" in _c6["pmfile"]),
         "★ pmfile 只能落在这两种状态之一，不能是空串或占位符字面量")
    if upmapconf.exists():
        t.ok("已落盘" in _c6["pmfile"], "文件名存在 ⇒ 状态必须是「已落盘」")
    else:
        t.ok("未落盘" in _c6["pmfile"],
             "★ 文件不存在 ⇒ 必须报「未落盘 · 网关用内置默认表」，"
             "这一条对应 main_gateway.cpp 的 ③ 回退分支")

    _a3 = _ac.assist(3, _ctx)
    t.ok([x["k"] for x in _a3] == ["up_count", "down_count"]
         and _a3[0]["v"] == "24",
         "★ 「审定后点数」直接给转发表实际行数 —— 数得出来的不让人手抄")
    t.eq(_ac.assist(8, {}), [],
         "★ 依据缺失时不给建议（转发表里没有下行点就不该凭空推荐传动对象）")
    t.ok(not any(x["v"] in ("pass", "通过") for _n in range(1, 10)
                 for x in _ac.assist(_n, _ctx)),
         "★ 建议值里绝不出现「通过」这类结论（那是替人签字）")

    _fn, _csv = _ac.export_step(2, {}, _ctx)
    t.ok(_fn.startswith("远动信息参数表"), "第 2 步导出的是《远动信息参数表》，不是通用台账")
    t.ok("CT 变比" in _csv and "遥测满度" in _csv,
         "★ 参数表里留出 CT/PT 变比与遥测满度三列（本软件不产生，空着就是提醒）")
    t.ok(_csv.count("\n") >= 5, "参数表是逐点展开的（每点一行）")
    _fn3, _csv3 = _ac.export_step(3, {}, _ctx)
    t.ok("号段对照表" in _csv3 and "4xxx" in _csv3,
         "★ 第 3 步导出号段对照表：按现行规则（ioa // 1000）推出号段给对点用")
    _f9 = _ac.export_step(9, _ac._blank()["steps"], _ctx)[1]
    t.ok("九步汇总" in _f9 and "缺" in _f9,
         "★ 第 9 步导出带九步汇总 + 缺项：一页看清哪一步没填")
    t.ok("缺项合计" in _ac.export_step(1, {}, _ctx)[1],
         "★ 每份导出末尾都写缺项合计（导出件不许看起来比草稿更完整）")

    # 中文文件名进响应头要按 RFC 5987（否则 latin-1 抛错、浏览器收到空响应）
    import server as _srv
    _cd = _srv._cd("远动信息参数表_第2步.csv")
    t.ok("filename*=UTF-8''" in _cd and all(ord(c) < 128 for c in _cd),
         "★ 中文文件名走 RFC 5987（直塞响应头会 UnicodeEncodeError ⇒ 空响应）")
    t.ok(_srv._cd("access.csv") == 'attachment; filename="access.csv"',
         "ASCII 文件名照旧，不加多余的 filename*")
    _ac.clear_draft(conn)
    conn.close()

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
