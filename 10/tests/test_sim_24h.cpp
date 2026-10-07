// =====================================================================
// 10/ 单元测试 —— 周期 10：EMS 24h 离线仿真测试
//
//   T101 日曲线导入（内置典型日）
//   T102 CSV 导入与内置曲线一致性
//   T103 24h 正常日：不变量全通过 / SOC 不越界
//   T104 经济性：两部制核算 / 峰谷套利方向 / 需量削减
//   T105 跟踪质量：无振荡 / RMSE 合理
//   T106 告警：正常日无 FAULT
//   T107 故障注入：故障窗内门控为 0，恢复后重新出力
//   T108 产物落盘：4 个文件写出且非空
//   T109 日志降采样：不变量结论与粒度无关
//   T112 实时源（sim_live）与离线仿真**逐列一致**
//   T113 多日仿真：时刻戳带天号 / 时长参数生效
//
// 编译：g++ -std=c++17 -Wall -O2 -I src -I ../04/src -I ../05/src
//           -I ../06/src -I ../07/src -I ../08/src
//           tests/test_sim_24h.cpp -o build/test_sim_24h.exe
// =====================================================================

#include "sim_report.h"
#include "sim_24h.h"
#include "sim_live.h"
#include "record_csv.h"          // 07/ —— 实时源写出的实录格式

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace ems;

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT(cond)                                                      \
    do {                                                                  \
        if (cond) {                                                       \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #cond << std::endl;                     \
        }                                                                 \
    } while (0)

#define EXPECT_NEAR(a, b, tol)                                            \
    do {                                                                  \
        const double _a = (a), _b = (b), _t = (tol);                      \
        if (std::fabs(_a - _b) <= _t) {                                   \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : |" << #a << " - " << #b << "| = "            \
                      << std::fabs(_a - _b) << " > " << _t                \
                      << "  (" << _a << " vs " << _b << ")" << std::endl; \
        }                                                                 \
    } while (0)

static bool file_nonempty(const std::string& p) {
    std::ifstream f(p.c_str(), std::ios::binary);
    if (!f.is_open()) return false;
    f.seekg(0, std::ios::end);
    return f.tellg() > 0;
}

// 跑一次正常日 24h（全模块共享，避免重复计算）
static Sim24hConfig g_cfg;
static Sim24hResult g_res;
static bool         g_ran = false;

static void ensure_normal_run() {
    if (g_ran) return;
    g_cfg = make_default_24h_config();
    g_cfg.out_dir = "build";
    g_cfg.title = "EMS 24h 离线仿真 · 测试";
    g_res = run_sim_24h(g_cfg);
    g_ran = true;
}

// =====================================================================
// T101 日曲线导入
// =====================================================================
static void t101_curves() {
    std::cerr << "[T101] 日曲线导入（内置典型日）...\n";
    ForecastSeries fc = make_typical_day_curves();
    EXPECT(fc.loaded);
    EXPECT(fc.size() == 96);
    EXPECT_NEAR(fc.step_s, 900.0, 1e-9);

    CurveStats st = analyze_curves(fc);
    EXPECT(st.points == 96);
    // 负荷：基础 250，双峰抬升 → 峰值应在 400~460
    EXPECT(st.load_max_kw > 400.0);
    EXPECT(st.load_max_kw < 470.0);
    EXPECT(st.load_min_kw > 240.0);
    EXPECT(st.load_avg_kw > 250.0);
    EXPECT(st.load_avg_kw < 350.0);
    // 光伏峰值 300
    EXPECT_NEAR(st.pv_max_kw, 300.0, 5.0);
    // 电价四档
    EXPECT_NEAR(st.price_min, 0.30, 1e-9);
    EXPECT_NEAR(st.price_max, 1.20, 1e-9);
    // 电量量级
    EXPECT(st.e_load_kwh > 6000.0 && st.e_load_kwh < 9000.0);
    EXPECT(st.e_pv_kwh > 1800.0 && st.e_pv_kwh < 2600.0);

    // 阶梯保持采样：0:00 与 0:14 同槽，0:15 进入下一槽
    double l1 = 0, l2 = 0, l3 = 0;
    fc.sample(0.0, &l1, nullptr, nullptr);
    fc.sample(899.0, &l2, nullptr, nullptr);
    fc.sample(900.0, &l3, nullptr, nullptr);
    EXPECT_NEAR(l1, l2, 1e-9);
    EXPECT(std::fabs(l3 - l1) > 1e-9);
}

// =====================================================================
// T102 CSV 导入一致性
// =====================================================================
static void t102_csv() {
    std::cerr << "[T102] CSV 导入与内置曲线一致性...\n";
    ForecastSeries csv;
    std::string err;
    const bool ok = load_day_curves_csv("data/typical_day_96.csv", csv, 900.0, &err);
    EXPECT(ok);
    if (!ok) {
        std::cerr << "       (csv load err: " << err << ")\n";
        return;
    }
    EXPECT(csv.loaded);
    EXPECT(csv.size() == 96);

    ForecastSeries builtin = make_typical_day_curves();
    double max_dl = 0, max_dpv = 0, max_dpr = 0;
    for (int i = 0; i < 96; ++i) {
        max_dl  = std::max(max_dl,  std::fabs(csv.p_load_kw[i] - builtin.p_load_kw[i]));
        max_dpv = std::max(max_dpv, std::fabs(csv.p_pv_kw[i]  - builtin.p_pv_kw[i]));
        max_dpr = std::max(max_dpr, std::fabs(csv.price[i]    - builtin.price[i]));
    }
    // CSV 是 2 位小数舍入，差异应在 0.01 量级
    EXPECT(max_dl  < 0.02);
    EXPECT(max_dpv < 0.02);
    EXPECT(max_dpr < 1e-4);

    // 不存在的文件必须失败（错误处理）
    ForecastSeries dummy;
    EXPECT(!load_day_curves_csv("data/__no_such_file__.csv", dummy, 900.0, &err));
}

// =====================================================================
// T103 24h 正常日：不变量
// =====================================================================
static void t103_invariants() {
    std::cerr << "[T103] 24h 正常日不变量...\n";
    ensure_normal_run();
    EXPECT(g_res.ok);
    EXPECT(g_res.steps == 86400);
    EXPECT(g_res.log_rows > 8000);      // log_every=10 → 8640 行

    // 硬不变量：任何一条被破坏都是架构级问题
    EXPECT(g_res.out_of_interval == 0);
    EXPECT(g_res.over_limit == 0);
    EXPECT(g_res.gated_nonzero == 0);
    // 安全不变量（稳态窗口）
    EXPECT(g_res.grid_breach == 0);
    EXPECT(g_res.tr_breach == 0);
    // SOC
    EXPECT(g_res.econ.soc_violation == 0);
    EXPECT(g_res.econ.soc_min >= g_cfg.safety.soc_min - 1e-9);
    EXPECT(g_res.econ.soc_max <= g_cfg.safety.soc_max + 1e-9);

    EXPECT(g_res.invariants_ok());

    // 关口应在契约需量内
    EXPECT(g_res.max_grid_kw <= g_cfg.limits.d_target_kw * 1.02 + 1e-6);
    // 储能确实动作了（否则测试没有意义）
    EXPECT(g_res.econ.e_discharge_kwh > 100.0);
    EXPECT(g_res.econ.e_charge_kwh > 100.0);
}

// =====================================================================
// T104 经济性
// =====================================================================
static void t104_economics() {
    std::cerr << "[T104] 经济性核算...\n";
    ensure_normal_run();
    const auto& e = g_res.econ;

    // 电量守恒：购电 + 放电 = 负荷 + 充电 − 光伏 + PCS 空载损耗
    //   （空载损耗 pcs_standby_kw 全天在线，是不可忽略的 48 kWh@2kW·24h）
    const double standby = g_cfg.plant.pcs_standby_kw * g_cfg.duration_s / 3600.0;
    const double lhs = e.e_import_kwh + e.e_discharge_kwh;
    const double rhs = g_res.curves.e_load_kwh + e.e_charge_kwh
                     - g_res.curves.e_pv_kwh + standby;
    EXPECT(std::fabs(lhs - rhs) < 0.01 * std::max(1.0, rhs));

    // 峰谷套利方向：谷段充电、峰/尖段放电
    EXPECT(g_res.e_chg_valley_kwh > 50.0);
    EXPECT(g_res.e_dis_peak_kwh  > 50.0);

    // 需量削减：含储能的 15min 平均需量应低于无储能
    EXPECT(e.peak_grid_kw <= e.peak_grid_base_kw + 1e-6);
    EXPECT(e.peak_grid_base_kw > 350.0);

    // 费用结构
    EXPECT(e.cost_energy_cny > 0.0);
    EXPECT(e.cost_demand_cny > 0.0);
    EXPECT_NEAR(e.cost_total_cny,
                e.cost_energy_cny + e.cost_demand_cny - e.revenue_feed_in_cny, 1e-6);

    // 节省与净收益
    EXPECT(e.saving_energy_cny > 0.0);
    EXPECT(e.saving_total_cny > 0.0);
    EXPECT(e.cost_degradation_cny > 0.0);
    // 净收益 = 节省 − 衰减
    EXPECT_NEAR(e.net_benefit_cny, e.saving_total_cny - e.cost_degradation_cny, 1e-6);
    // 度电衰减成本 = 投资 / (寿命 × 效率)
    EXPECT_NEAR(g_cfg.econ.degradation_cny_per_kwh_dis(),
                1200.0 / (6000.0 * 0.90), 1e-9);
    // 衰减成本应远小于节省（否则方案不成立）
    EXPECT(e.cost_degradation_cny < e.saving_total_cny);

    // 等效循环：一天不超过 1.5 次（单充单放 + 余量）
    EXPECT(e.equiv_cycles > 0.1);
    EXPECT(e.equiv_cycles < 1.5);

    // 回收期：有限且合理
    const double capex = g_cfg.econ.battery_capex_cny_per_kwh *
                         g_cfg.plant.battery_capacity_kwh;
    const double pb = e.payback_years(capex);
    EXPECT(pb > 0.0);
    EXPECT(pb < 20.0);

    std::cerr << "       节省=" << e.saving_total_cny << " 元 净收益="
              << e.net_benefit_cny << " 元 回收期=" << pb << " 年\n";
}

// =====================================================================
// T105 跟踪质量
// =====================================================================
static void t105_tracking() {
    std::cerr << "[T105] 跟踪质量...\n";
    ensure_normal_run();
    const auto& m = g_res.metrics;
    EXPECT(m.samples > 0);
    // 指令 vs 实际：一阶惯性 + 死区时间，RMSE 应远小于额定功率
    EXPECT(m.rmse_track_kw < 0.25 * g_cfg.limits.pcs_rated_dis_kw);
    EXPECT(m.mean_abs_track_err_kw < 0.20 * g_cfg.limits.pcs_rated_dis_kw);
    // 无振荡
    EXPECT(m.no_oscillation(0.20, 1.00));
    // 状态迁移次数：正常日不应频繁跳变
    EXPECT(m.state_changes <= 8);
    std::cerr << "       " << m.to_string() << "\n";
}

// =====================================================================
// T106 告警
// =====================================================================
static void t106_alarms() {
    std::cerr << "[T106] 告警日志...\n";
    ensure_normal_run();
    // 正常日不应有 FAULT 级告警
    EXPECT(g_res.fault_count == 0);
    // 告警数量应有限（去抖生效，不是逐拍刷屏）
    EXPECT(g_res.alarm_count < 500);
    // 每条告警都要有来源与说明
    for (const auto& a : g_res.alarms) {
        EXPECT(!a.source.empty());
        EXPECT(!a.message.empty());
        EXPECT(a.t >= 0.0 && a.t <= g_cfg.duration_s + 1e-6);
    }
}

// =====================================================================
// T107 故障注入
//
// 语义要点（06/ 状态机的安全设计）：
//   · 进入 FAULT 会**撤销运行许可**，故障恢复后停在 READY 待机，
//     必须由上层显式重新下发启动命令才回到 NORMAL —— 不允许自动带载。
//   所以本用例分两种模式验证：
//     ① auto_restart=false（默认）：故障窗内门控为 0，且**恢复后不得自动出力**
//     ② auto_restart=true（模拟上层保持许可）：恢复后能重新带载
// =====================================================================
static void t107_fault() {
    std::cerr << "[T107] 故障注入...\n";
    Sim24hConfig c = make_default_24h_config();
    c.dt_s = 2.0;               // 加速（故障语义与步长无关）
    c.log_every = 5;
    c.title = "故障注入测试";
    c.fault_windows = {
        {16.0 * 3600.0, 16.5 * 3600.0, 4, "PCS 故障"},
        {20.0 * 3600.0, 20.0833 * 3600.0, 2, "电表通信中断"},
    };

    auto count = [](const Sim24hResult& r, double t0, double t1,
                    int* n, int* nz, double thr) {
        *n = 0; *nz = 0;
        for (const auto& s : r.log) {
            if (s.t >= t0 && s.t < t1) { ++(*n); if (std::fabs(s.p_cmd) > thr) ++(*nz); }
        }
    };

    // 故障窗（跳过前 60 s 的故障检测/降级过渡）与恢复窗
    // 恢复窗选 17:30–19:00 —— 该时段计划本身要求满功率放电，
    // 因此"有没有重新出力"可以被明确区分出来。
    const double win0 = 16.0 * 3600.0 + 60.0, win1 = 16.5 * 3600.0;
    const double aft0 = 17.5 * 3600.0,        aft1 = 19.0 * 3600.0;

    // ---------- ① 默认：故障恢复后不自动带载 ----------
    {
        Sim24hResult r = run_sim_24h(c);
        EXPECT(r.ok);
        EXPECT(r.fault_ticks > 0);

        int wn = 0, wnz = 0, an = 0, anz = 0;
        count(r, win0, win1, &wn, &wnz, 1e-6);
        count(r, aft0, aft1, &an, &anz, 5.0);
        EXPECT(wn > 0);
        EXPECT(wnz == 0);              // 故障窗内指令必须为 0（门控）
        EXPECT(an > 0);
        EXPECT(anz == 0);              // 恢复后**不得自动出力**（安全要求）

        // 状态机必须记录故障与恢复
        bool has_fault_soe = false, has_recover = false;
        for (const auto& a : r.alarms) {
            if (a.source == "FSM" && a.level == "FAULT") has_fault_soe = true;
            if (a.source == "FSM" && a.message.find("fault_recovered") != std::string::npos)
                has_recover = true;
        }
        EXPECT(has_fault_soe);
        EXPECT(has_recover);

        // 硬不变量在故障日同样不得被破坏（架构级要求）
        EXPECT(r.hard_invariants_ok());
        EXPECT(r.out_of_interval == 0);
        EXPECT(r.over_limit == 0);
        EXPECT(r.gated_nonzero == 0);

        // 安全不变量若越限，必须可归因到"能量预算"而非"控制失效"：
        // 设备故障改变了当日能量轨迹 → 储能提前触底 → 傍晚无容量削峰。
        if (r.grid_breach > 0) {
            EXPECT(r.grid_breach_soc_limited == r.grid_breach);
            EXPECT(r.max_grid_kw > 0.0);
        }

        std::cerr << "       默认模式：故障窗采样=" << wn << " 非零=" << wnz
                  << " 恢复后非零=" << anz << "/" << an << "（应全 0）\n"
                  << "       关口越界=" << r.grid_breach << "（其中 SOC 触底归因 "
                  << r.grid_breach_soc_limited << "）\n";
    }

    // ---------- ② 上层保持许可：恢复后重新带载 ----------
    {
        Sim24hConfig c2 = c;
        c2.auto_restart_after_fault = true;
        Sim24hResult r = run_sim_24h(c2);
        EXPECT(r.ok);

        int wn = 0, wnz = 0, an = 0, anz = 0;
        count(r, win0, win1, &wn, &wnz, 1e-6);
        count(r, aft0, aft1, &an, &anz, 5.0);
        EXPECT(wn > 0);
        EXPECT(wnz == 0);              // 故障窗内仍必须为 0
        EXPECT(an > 0);
        EXPECT(anz > an / 2);          // 恢复后必须重新出力

        // 恢复后仍不得破坏不变量
        EXPECT(r.out_of_interval == 0);
        EXPECT(r.over_limit == 0);
        EXPECT(r.gated_nonzero == 0);

        std::cerr << "       保持许可：故障窗非零=" << wnz
                  << " 恢复后非零=" << anz << "/" << an << "\n";
    }
}

// =====================================================================
// T108 产物落盘
// =====================================================================
static void t108_reports() {
    std::cerr << "[T108] 产物落盘...\n";
    ensure_normal_run();
    const int n = write_all_reports("build", g_res, g_cfg, &g_res.forecast);
    EXPECT(n == 4);
    EXPECT(file_nonempty("build/timeseries.csv"));
    EXPECT(file_nonempty("build/alarms.csv"));
    EXPECT(file_nonempty("build/summary.json"));
    EXPECT(file_nonempty("build/report.html"));

    // 时序 CSV：表头 + 行数
    {
        std::ifstream f("build/timeseries.csv");
        std::string line;
        int lines = 0;
        while (std::getline(f, line)) ++lines;
        EXPECT(lines == g_res.log_rows + 1);
    }
    // 汇总 JSON：含关键字段
    {
        std::ifstream f("build/summary.json");
        std::string all((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        EXPECT(all.find("\"net_benefit_cny\"") != std::string::npos);
        EXPECT(all.find("\"invariants\"") != std::string::npos);
        EXPECT(all.find("\"all_ok\": true") != std::string::npos);
    }
    // HTML：内联 SVG、无外链 JS
    {
        std::ifstream f("build/report.html");
        std::string all((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        EXPECT(all.find("<svg") != std::string::npos);
        EXPECT(all.find("<script") == std::string::npos);
        EXPECT(all.find("功率曲线") != std::string::npos);
    }
}

// =====================================================================
// T109 日志降采样一致性
// =====================================================================
static void t109_decimation() {
    std::cerr << "[T109] 日志降采样一致性...\n";
    Sim24hConfig a = make_default_24h_config();
    a.dt_s = 2.0; a.log_every = 5;      // 10 s
    Sim24hConfig b = a;
    b.log_every = 30;                    // 60 s

    Sim24hResult ra = run_sim_24h(a);
    Sim24hResult rb = run_sim_24h(b);
    EXPECT(ra.ok && rb.ok);

    // 不变量结论与日志粒度无关（同一物理过程）
    EXPECT(ra.invariants_ok() == rb.invariants_ok());
    EXPECT(ra.out_of_interval == rb.out_of_interval);
    EXPECT(ra.over_limit == rb.over_limit);
    EXPECT(ra.gated_nonzero == rb.gated_nonzero);
    EXPECT(rb.log_rows < ra.log_rows);
    EXPECT(rb.log_rows > ra.log_rows / 10);

    // 电量核算对粒度不敏感（误差 < 5%）
    const double ea = ra.econ.e_import_kwh, eb = rb.econ.e_import_kwh;
    EXPECT(std::fabs(ea - eb) < 0.05 * std::max(1.0, ea));
    const double sa = ra.econ.saving_total_cny, sb = rb.econ.saving_total_cny;
    EXPECT(std::fabs(sa - sb) < 0.10 * std::max(1.0, std::fabs(sa)));
}

// =====================================================================
// T110 并网前瞻有效性（防止 §5.1 缺陷回归）
//
// 机制：预报是 15 min 阶梯，base = P_load − P_pv 会在阶梯边界一拍内突降；
//   上一拍按旧（更松）上界下发的指令仍在 PCS 死区/惯性里执行 → 瞬时倒送。
//   安全层取 min(base_now, base_next) 作上界即可消除。
//   本用例必须用 **单拍日志**（log_every=1），否则尖峰被降采样掩盖。
//
// ★ 2026-09-28 修正（plan_target 毛刺修复后）：
//   原先断言「关闭前瞻 min_grid < -5（固有穿越）」。但复盘发现，那个倒送的
//   能量来源其实是 **08/ 计划槽边界 plan_target 毛刺**（错误放电 157 kW 级别），
//   不是「阶梯边界穿越」本身——无毛刺时 base 突降仅 12.5 kW，而并网上界
//   `base − grid_p_min` 本就把放电钳在净负荷内，正常不会倒送。
//   plan_target 毛刺修复（重优化对齐槽边界）后，该错误放电消失，倒送自然归零。
//   故本用例改为：关闭前瞻**也不得**再出现毛刺型倒送；前瞻开启不得更差、
//   不得破坏任何不变量。前瞻机制本身仍由"开启时 min_grid ≥ 关闭时"守住。
// =====================================================================
static void t110_lookahead() {
    std::cerr << "[T110] 并网前瞻有效性...\n";

    Sim24hConfig off = make_default_24h_config();
    off.dt_s = 1.0;
    off.log_every = 1;                 // 必须单拍，否则看不见尖峰
    off.duration_s = 12.0 * 3600.0;    // 覆盖 10:45 的阶梯跳变
    off.safety.grid_lookahead_max_drop_kw = 0.0;   // 关闭前瞻

    Sim24hConfig on = off;
    on.safety.grid_lookahead_max_drop_kw = 50.0;   // 开启前瞻

    Sim24hResult r_off = run_sim_24h(off);
    Sim24hResult r_on  = run_sim_24h(on);
    EXPECT(r_off.ok && r_on.ok);

    // 修复后：关闭前瞻也不得再出现毛刺型倒送（原断言 < -5 是毛刺的假象）
    EXPECT(r_off.min_grid_kw > -0.5);
    EXPECT(r_on.min_grid_kw > -0.5);
    // 前瞻开启不得使最小关口功率更差（前瞻只收紧上界，不会放大倒送）
    EXPECT(r_on.min_grid_kw >= r_off.min_grid_kw - 0.5);

    // 前瞻不得破坏任何不变量
    EXPECT(r_on.out_of_interval == 0);
    EXPECT(r_on.over_limit == 0);
    EXPECT(r_on.gated_nonzero == 0);
    EXPECT(r_on.grid_breach == 0);
    EXPECT(r_on.tr_breach == 0);

    std::cerr << "       关闭前瞻 min_grid=" << r_off.min_grid_kw
              << " kW  →  开启前瞻 min_grid=" << r_on.min_grid_kw << " kW\n";
}

// =====================================================================
// T111 故障日的"能量预算"结论必须可归因
//
// 复现演示场景：08:00–08:30 PCS 故障 + 上层保持运行许可。
// 故障使当日能量轨迹改变 → 储能提前触底 → 傍晚无容量削峰 → 关口越限。
// 这是**有效场景结论**（储能容量/能量管理问题），不是控制失效。
// 本用例要求：硬不变量必须全过；关口越限必须 100% 可归因到 SOC 触底。
//
// ★ 2026-09-28 修正（plan_target 毛刺修复后）：
//   原先断言 `grid_breach > 0`（傍晚 SOC 触底越限）。但复盘发现，那个越限的
//   能量来源同样是 08/ 计划槽边界 plan_target 毛刺（错误放电 157 kW 级）——
//   毛刺让储能提前放空 → 傍晚无容量 → 越限。修复重优化对齐槽边界后，滚动
//   重优化以实测 SOC 自适应重规划，30 min PCS 故障的能量损失被后续时段自动
//   补偿，储能不再提前触底（SOC 最低 0.11），傍晚也不再越限。
//   故本用例改为：硬不变量全过 + SOC 不越界，且**架构不变量**（越界若发生
//   必须 100% 归因 SOC 触底）仍被保留为条件性断言 —— 修复后越界为 0，该
//   断言自然成立，但语义仍是"任何越界都必须可归因"，防止将来控制失效被
//   误当成能量预算问题。原"30min 故障必越限"的场景结论已失效，需重新设计
//   更重的能量剥夺型故障才能重新触发（另立待办，不在本次 4 项修复范围内）。
// =====================================================================
static void t111_fault_energy_budget() {
    std::cerr << "[T111] 故障日能量预算归因...\n";
    Sim24hConfig c = make_default_24h_config();
    c.dt_s = 1.0;
    c.log_every = 10;
    c.auto_restart_after_fault = true;
    // 与演示场景一致：PCS 故障 30 min + BMS 通信中断 10 min + 电表中断 5 min
    c.fault_windows = {
        {8.0 * 3600.0,  8.5 * 3600.0,    4, "PCS 故障"},
        {14.0 * 3600.0, 14.1667 * 3600.0, 1, "BMS 通信中断"},
        {20.0 * 3600.0, 20.0833 * 3600.0, 2, "电表通信中断"},
    };

    Sim24hResult r = run_sim_24h(c);
    EXPECT(r.ok);

    // 硬不变量：架构级要求，故障日同样必须全过
    EXPECT(r.hard_invariants_ok());
    EXPECT(r.out_of_interval == 0);
    EXPECT(r.over_limit == 0);
    EXPECT(r.gated_nonzero == 0);

    // 变压器在稳态窗口内不得越限
    EXPECT(r.tr_breach == 0);

    // ★ 架构不变量：越界若发生，必须 100% 归因到 SOC 触底/触顶（不能是控制
    //   失效）。plan_target 修复后本场景不再越界（能量管理自适应补偿），此
    //   断言在 0 越界下自然成立，但语义保留，防控制失效被误判成能量预算。
    EXPECT(r.grid_breach_soc_limited == r.grid_breach);
    // SOC 在容差内不得判越界
    EXPECT(r.econ.soc_violation == 0);

    std::cerr << "       关口越界=" << r.grid_breach
              << "（SOC 触底归因 " << r.grid_breach_soc_limited << "）"
              << " SOC=[" << r.econ.soc_min << ", " << r.econ.soc_max << "]"
              << " max_grid=" << r.max_grid_kw << " kW\n";
}

// =====================================================================
// T112 实时源与离线仿真逐列一致
//
//   这是本文件里**最要紧的一条新断言**。10/ 现在有两条时间纪律：
//     批量 run_sim_24h —— 离线仿真，跑完看结论
//     实时 run_sim_live —— 运行模式的数据源，按墙钟一拍一拍跑
//   两者共用 assemble_runtime()，但"共用"是**声明**，不是事实。
//   事实由这里锁定：同起点（相位 0）、同拍数、同 dt、同 log_every 下，
//   实时源写出的实录与离线仿真的日志**除两列时刻外逐字节相同**。
//
//   为什么只比"除时刻外"：两列时刻是**故意不同**的 ——
//     离线 t_s = 模型秒（0..86400），time = "HH:MM"
//     实时 t_s = Unix 墙钟秒，  time = "YYYY-MM-DD HH:MM:SS"
//   这正是 14/ 用 scenario.time_base 区分 'sim' / 'wall' 的原因。
// =====================================================================
static std::string strip_time_columns(const std::string& line) {
    // 去掉前两列（t_s / time）。前两列不含引号与逗号，按前两个逗号切即可；
    // reason 列虽然可能含逗号，但它在**最后**，不受影响。
    const size_t p1 = line.find(',');
    if (p1 == std::string::npos) return line;
    const size_t p2 = line.find(',', p1 + 1);
    if (p2 == std::string::npos) return line;
    return line.substr(p2 + 1);
}

static std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream f(path.c_str());
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

static void t112_live_matches_batch() {
    std::cerr << "[T112] 实时源与离线仿真逐列一致...\n";

    const double dur = 600.0;
    Sim24hConfig bcfg = make_default_24h_config();
    bcfg.duration_s = dur;
    bcfg.dt_s = 1.0;
    bcfg.log_every = 10;
    Sim24hResult bres = run_sim_24h(bcfg);
    EXPECT(bres.ok);
    EXPECT(bres.log_rows == 60);

    const std::string live_csv = "build/_t112_live.csv";
    Sim24hConfig lcfg = make_default_24h_config();
    lcfg.duration_s = dur;
    lcfg.dt_s = 1.0;
    lcfg.log_every = 10;
    SimLiveOptions lopt;
    lopt.record_path = live_csv;
    lopt.phase_auto = false;      // 固定相位 0 → 与离线同起点
    lopt.phase_s = 0.0;
    lopt.pace = false;            // 全速跑，测试不该真等 600 秒
    lopt.log_every = 10;
    lopt.quiet = true;
    // ★★ 这里踩过一次真坑，务必看清两个 duration_s 是**两个不同的字段**：
    //     Sim24hConfig::duration_s   —— 离线仿真的"跑多久"
    //     SimLiveOptions::duration_s —— 实时源的"跑多久"，默认 0 = **一直跑到被停**
    //   刚开始只设了 lcfg.duration_s，实时源便按 0 处理、永不停机，
    //   一路往 build/_t112_live.csv 里写了 **1.8 GB** 才被发现（测试表现是"卡在 T112"）。
    //   所以：时长必须设在 lopt 上；max_wall_s 是第二道防线 ——
    //   万一以后再有人漏设，也是 60 s 后停下来（断言会失败），而不是灌满磁盘。
    lopt.duration_s = dur;        // ← 这一行是必须的
    lopt.max_wall_s = 60.0;       // ← 护栏
    SimLiveResult lres = run_sim_live(lcfg, lopt);
    EXPECT(lres.ok);
    EXPECT(!lres.stopped_by_wall_limit);   // 被护栏截停 = 时长没设对，必须报出来
    EXPECT(lres.rows == 60);
    EXPECT(lres.ticks == 600);
    EXPECT_NEAR(lres.phase0_s, 0.0, 1e-9);

    std::vector<std::string> lines = read_lines(live_csv);
    EXPECT(!lines.empty());
    EXPECT(lines.size() == 61);   // 表头 + 60 行
    EXPECT(strip_time_columns(lines[0]) == strip_time_columns(record::csv_header()));

    int diff = 0;
    for (size_t i = 0; i < bres.log.size() && i + 1 < lines.size(); ++i) {
        // 批量侧用同一个序列化器生成同样的一行（时刻给个占位值）
        const std::string want = strip_time_columns(record::csv_row(bres.log[i], 0.0));
        const std::string got  = strip_time_columns(lines[i + 1]);
        if (want != got) {
            if (diff == 0) {
                std::cerr << "        首处不同 @行" << i << "\n"
                          << "          批量: " << want << "\n"
                          << "          实时: " << got  << "\n";
            }
            ++diff;
        }
    }
    EXPECT(diff == 0);
    std::cerr << "       比对 " << bres.log_rows << " 行 × 18 列，差异 " << diff << " 处\n";

    // 全速跑时 t_s 必须是**严格递增**的：入库主键是 (scenario_id, t_s)，
    // 撞时刻会被 INSERT OR REPLACE 静默吃掉一行（曲线只是少个点，看不出来）。
    double prev = -1e30;
    int non_increasing = 0;
    for (size_t i = 1; i < lines.size(); ++i) {
        const double t = std::atof(lines[i].c_str());
        if (t <= prev) ++non_increasing;
        prev = t;
    }
    EXPECT(non_increasing == 0);
    std::remove(live_csv.c_str());
}

// =====================================================================
// T113 多日仿真：时刻戳与时长
// =====================================================================
static void t113_multi_day() {
    std::cerr << "[T113] 多日仿真（时刻戳 / 时长）...\n";

    // ---- 时刻戳：单日口径必须与既有产物逐字一致 ----
    EXPECT(sim_report_detail::stamp(0.0, false) == "00:00");
    EXPECT(sim_report_detail::stamp(3660.0, false) == "01:01");
    // ★ 24 h 仿真的最后一行恰好 t=86400 —— 单日口径必须仍是 "00:00"
    EXPECT(sim_report_detail::stamp(86400.0, false) == "00:00");
    // ---- 多日口径：带天号，否则 30 天日志全叫 "08:00" ----
    EXPECT(sim_report_detail::stamp(0.0, true) == "D0 00:00");
    EXPECT(sim_report_detail::stamp(86400.0, true) == "D1 00:00");
    EXPECT(sim_report_detail::stamp(86400.0 + 8 * 3600.0, true) == "D1 08:00");

    // ---- 时长参数生效：2 天 = 172800 拍 / 17280 行（log_every=10）----
    Sim24hConfig cfg = make_default_24h_config();
    cfg.duration_s = 2.0 * 86400.0;
    cfg.dt_s = 1.0;
    cfg.log_every = 10;
    Sim24hResult r = run_sim_24h(cfg);
    EXPECT(r.ok);
    EXPECT(r.steps == 172800);
    EXPECT(r.log_rows == 17280);
    EXPECT_NEAR(r.log.back().t, 172800.0, 1e-6);
    // 曲线按 96 点日曲线**回绕**：第二天的同一时刻应与第一天同值
    double l0 = 0, l1 = 0;
    r.forecast.sample(8 * 3600.0, &l0, nullptr, nullptr);
    r.forecast.sample(86400.0 + 8 * 3600.0, &l1, nullptr, nullptr);
    EXPECT_NEAR(l0, l1, 1e-9);

    // 电量应是单日的两倍量级（同一曲线重复两天）
    Sim24hConfig cfg1 = make_default_24h_config();
    cfg1.duration_s = 86400.0;
    cfg1.dt_s = 1.0;
    cfg1.log_every = 10;
    Sim24hResult r1 = run_sim_24h(cfg1);
    const double ratio = r.econ.e_import_kwh / r1.econ.e_import_kwh;
    EXPECT(ratio > 1.9 && ratio < 2.1);
    std::cerr << "       2 日购电 " << r.econ.e_import_kwh
              << " kWh / 1 日 " << r1.econ.e_import_kwh
              << " kWh，比值 " << ratio << "\n";
}

// =====================================================================
int main() {
    std::cerr << "=== 10/ 周期 10 单元测试（EMS 24h 离线仿真）===\n";
    t101_curves();
    t102_csv();
    t103_invariants();
    t104_economics();
    t105_tracking();
    t106_alarms();
    t107_fault();
    t108_reports();
    t109_decimation();
    t110_lookahead();
    t111_fault_energy_budget();
    t112_live_matches_batch();
    t113_multi_day();

    std::cerr << "\n----------------------------------------\n";
    std::cerr << "PASS=" << g_pass << " FAIL=" << g_fail << "\n";
    if (g_fail == 0) std::cerr << "=== ALL TESTS PASSED ===\n";
    else             std::cerr << "=== TESTS FAILED ===\n";
    return g_fail == 0 ? 0 : 1;
}
