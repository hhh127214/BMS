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
//
// 编译：g++ -std=c++17 -Wall -O2 -I src -I ../04/src -I ../05/src
//           -I ../06/src -I ../07/src -I ../08/src
//           tests/test_sim_24h.cpp -o build/test_sim_24h.exe
// =====================================================================

#include "sim_report.h"
#include "sim_24h.h"

#include <cstdio>
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

    // 关闭前瞻：应出现瞬时倒送（固有穿越）
    EXPECT(r_off.min_grid_kw < -5.0);
    // 开启前瞻：倒送应被消除
    EXPECT(r_on.min_grid_kw > -0.5);
    EXPECT(r_on.min_grid_kw > r_off.min_grid_kw + 5.0);

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

    // 本场景确实会出现关口越限（傍晚 SOC 触底），且必须 **100% 可归因**到
    // SOC 触底/触顶 —— 否则就说明是控制失效而非能量预算问题。
    EXPECT(r.grid_breach > 0);
    EXPECT(r.grid_breach_soc_limited == r.grid_breach);
    // 越限发生在傍晚峰段（负荷最高、SOC 已触底）
    EXPECT(r.max_grid_kw > c.grid_max_required);
    // SOC 在容差内不得判越界
    EXPECT(r.econ.soc_violation == 0);

    std::cerr << "       关口越界=" << r.grid_breach
              << "（SOC 触底归因 " << r.grid_breach_soc_limited << "）"
              << " SOC=[" << r.econ.soc_min << ", " << r.econ.soc_max << "]"
              << " max_grid=" << r.max_grid_kw << " kW\n";
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

    std::cerr << "\n----------------------------------------\n";
    std::cerr << "PASS=" << g_pass << " FAIL=" << g_fail << "\n";
    if (g_fail == 0) std::cerr << "=== ALL TESTS PASSED ===\n";
    else             std::cerr << "=== TESTS FAILED ===\n";
    return g_fail == 0 ? 0 : 1;
}
