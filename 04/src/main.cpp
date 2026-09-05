// =====================================================================
// 04/ 演示程序：完整 EMS 策略管理与仲裁链路
//
// 链路：注册 9 策略 → 加载实时数据 → tick 一次 → 仲裁 → 输出 PowerCommand
// 同时打印每层区间收敛过程，便于观察多策略协同行为。
//
// 编译：g++ -std=c++17 -I src src/main.cpp -o build/strategy_demo.exe
// =====================================================================

#include "data_models.h"
#include "strategy_base.h"
#include "strategy_manager.h"
#include "strategy_arbiter.h"
#include "strategies_9.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

using namespace ems;

static std::string fmt_time(double t) {
    int hh = int(t / 3600) % 24;
    int mm = int(t / 60) % 60;
    int ss = int(t) % 60;
    std::ostringstream os;
    os << std::setfill('0') << std::setw(2) << hh << ":"
       << std::setfill('0') << std::setw(2) << mm << ":"
       << std::setfill('0') << std::setw(2) << ss;
    return os.str();
}

static void print_result_row(const StrategyResult& r) {
    std::cout << "    "
              << std::left << std::setw(22) << r.strategy_id
              << " L" << static_cast<int>(r.priority)
              << "  active=" << (r.active ? "Y" : "N")
              << "  weight=" << std::fixed << std::setprecision(2) << r.weight
              << "  range=[" << std::setw(7) << r.p_lower << ", "
                            << std::setw(7) << r.p_upper << "]"
              << "  desired=" << std::setw(7) << r.p_desired
              << "  reason=" << r.reason
              << "\n";
}

static void print_layer_debug(const std::vector<StrategyArbiter::LayerDebug>& L) {
    static const char* names[] = {"L0_SAFETY", "L1_SAFEOP",
                                  "L2_LOCAL",  "L3_GLOBAL"};
    std::cout << "  [区间收敛过程]\n";
    for (size_t i = 0; i < L.size(); ++i) {
        const auto& d = L[i];
        if (d.ids.empty()) continue;
        std::cout << "    " << names[i]
                  << "  lower=" << std::setw(10) << d.lower
                  << "  upper=" << std::setw(10) << d.upper
                  << "  ids=[";
        for (size_t k = 0; k < d.ids.size(); ++k) {
            if (k) std::cout << ", ";
            std::cout << d.ids[k];
        }
        std::cout << "]\n";
    }
}

// 典型场景：峰谷500/需量400/需求响应300/BMS250/变压器200 → 最终200
static void run_typical_scenario(StrategyManager& mgr, StrategyArbiter& arb) {
    std::cout << "\n========== 场景 1：典型多策略同时启用 ==========\n";
    std::cout << "目标：演示区间逐层收敛，PeakValley+500 期望被压扁到 200\n\n";

    // 参数
    mgr.set_param(strategy_id::kPeakValley, "P_discharge", 500.0);
    mgr.set_param(strategy_id::kPeakValley,     "__weight__", 1.0);
    mgr.set_param(strategy_id::kForecastOpt,    "__weight__", 1.0);
    mgr.set_param(strategy_id::kDemandResponse, "__weight__", 1.0);

    DeviceLimits dev;
    dev.pcs_rated_chg_kw   = 100.0;
    dev.pcs_rated_dis_kw   = 200.0;     // PCS 200 上限
    dev.bms_chg_limit_kw   = 100.0;
    dev.bms_dis_limit_kw   = 250.0;     // BMS 250 上限
    dev.transformer_capacity_kw = 200.0;
    dev.d_target_kw        = 250.0;

    RealtimeSnapshot rt;
    rt.timestamp = 12 * 3600 + 0 * 60;   // 中午 12:00
    rt.p_grid_kw = 50.0;
    rt.p_pv_kw   = 200.0;
    rt.p_load_kw = 400.0;                // 让 ForecastOpt 期望放电
    rt.soc       = 0.5;
    rt.pricing.cur_tou_type = TouType::kPeak;
    rt.demand_window.window_s    = 900.0;
    rt.demand_window.t_elapsed_s = 450.0;
    rt.demand_window.p_avg_past_kw = 50.0;

    auto results = mgr.tick(rt, dev);
    auto layers  = arb.debug_layers(results);
    auto cmd     = arb.arbitrate(results, rt.timestamp);

    std::cout << "  [各策略输出]\n";
    for (const auto& r : results) print_result_row(r);

    print_layer_debug(layers);

    std::cout << "\n  [最终下发指令]\n"
              << "    timestamp    = " << fmt_time(rt.timestamp) << "\n"
              << "    p_bat_cmd_kw = " << std::fixed << std::setprecision(3)
                                       << cmd.p_bat_cmd_kw << "\n"
              << "    p_lower      = " << cmd.p_lower << "\n"
              << "    p_upper      = " << cmd.p_upper << "\n"
              << "    clamped      = " << (cmd.clamped ? "true" : "false") << "\n"
              << "    reason       = " << cmd.reason << "\n"
              << "    contributing = ";
    for (size_t k = 0; k < cmd.contributing.size(); ++k) {
        if (k) std::cout << ", ";
        std::cout << cmd.contributing[k];
    }
    std::cout << "\n";
}

// 场景 2：24 小时滚动仿真，每 15 分钟打印一次状态
static void run_24h_rolling_demo(StrategyManager& mgr, StrategyArbiter& arb) {
    std::cout << "\n========== 场景 2：24 小时滚动仿真（96 步 × 15min） ==========\n";

    // 简化参数
    mgr.set_param(strategy_id::kPeakValley, "P_discharge", 80.0);
    mgr.set_param(strategy_id::kPeakValley, "P_charge",    60.0);

    DeviceLimits dev;
    dev.pcs_rated_chg_kw   = 100.0;
    dev.pcs_rated_dis_kw   = 100.0;
    dev.bms_chg_limit_kw   = 100.0;
    dev.bms_dis_limit_kw   = 100.0;
    dev.transformer_capacity_kw = 250.0;
    dev.d_target_kw        = 250.0;

    std::cout << "  时段          电价       负荷    光伏    "
              << "下发     区间                reason\n";

    double soc = 0.5;
    const double cap_kWh = 1000.0;

    for (int step = 0; step < 96; ++step) {
        Timestamp t = step * 900.0;  // 每步 15 min

        // 简化：电价时段
        int h = int(t / 3600) % 24;
        TouType tou;
        if      (h < 7)  tou = TouType::kValley;
        else if (h < 9)  tou = TouType::kFlat;
        else if (h < 11) tou = TouType::kPeak;
        else if (h < 14) tou = TouType::kFlat;
        else if (h < 17) tou = TouType::kPeak;
        else if (h < 19) tou = TouType::kSharp;
        else if (h < 22) tou = TouType::kFlat;
        else             tou = TouType::kValley;

        // 简化：负荷/光伏模型
        RealtimeSnapshot rt;
        rt.timestamp = t;
        rt.p_load_kw = 200.0 + 100.0 * std::sin((h - 12) / 24.0 * 6.28);
        rt.p_pv_kw   = (h >= 6 && h <= 18) ? 200.0 * std::sin((h - 6) / 12.0 * 3.14)
                                            : 0.0;
        rt.p_grid_kw = rt.p_load_kw - rt.p_pv_kw;
        rt.soc       = soc;
        rt.pricing.cur_tou_type = tou;

        auto results = mgr.tick(rt, dev);
        auto cmd = arb.arbitrate(results, rt.timestamp);

        // 更新 SOC
        double p_bat = cmd.p_bat_cmd_kw;   // kW
        double dsoc = -p_bat / cap_kWh * 0.25;  // 15min = 0.25h
        soc += dsoc;
        if (soc < 0.05) soc = 0.05;
        if (soc > 0.95) soc = 0.95;

        // 仅打印关键时段
        if (step % 4 == 0) {
            std::cout << "  " << fmt_time(t)
                      << "  " << std::left << std::setw(10) << tou_type_name(tou)
                      << "  " << std::setw(6) << (int)rt.p_load_kw
                      << "  " << std::setw(6) << (int)rt.p_pv_kw
                      << "  " << std::setw(7) << std::fixed << std::setprecision(2) << cmd.p_bat_cmd_kw
                      << "  [" << std::setw(7) << cmd.p_lower << ","
                      << std::setw(7) << cmd.p_upper << "]"
                      << "    " << cmd.reason
                      << "\n";
        }
    }
}

// 场景 3：故障注入（BMS 禁放 → 区间立即收敛）
static void run_fault_injection(StrategyManager& mgr, StrategyArbiter& arb) {
    std::cout << "\n========== 场景 3：故障注入（t=10s BMS 禁止放电） ==========\n";

    DeviceLimits dev;
    dev.pcs_rated_chg_kw   = 100.0;
    dev.pcs_rated_dis_kw   = 100.0;
    dev.bms_chg_limit_kw   = 100.0;
    dev.bms_dis_limit_kw   = 100.0;
    dev.transformer_capacity_kw = 250.0;
    dev.d_target_kw        = 250.0;

    std::cout << "  时刻    BMS状态         上界     下发     reason\n";

    for (int i = 0; i < 5; ++i) {
        Timestamp t = i * 1.0;

        // t = 2..4 时禁止放电
        if (i >= 2) {
            dev.bms_dis_forbidden = true;
        } else {
            dev.bms_dis_forbidden = false;
        }

        RealtimeSnapshot rt;
        rt.timestamp = t;
        rt.p_load_kw = 200.0;
        rt.p_pv_kw   = 100.0;
        rt.p_grid_kw = 100.0;
        rt.soc       = 0.5;
        rt.pricing.cur_tou_type = TouType::kPeak;
        mgr.set_param(strategy_id::kPeakValley, "P_discharge", 80.0);

        auto results = mgr.tick(rt, dev);
        auto cmd = arb.arbitrate(results, rt.timestamp);

        std::cout << "  " << std::fixed << std::setprecision(1) << std::setw(5) << t
                  << "s   " << (dev.bms_dis_forbidden ? "禁放" : "正常")
                  << "         "
                  << std::setw(7) << cmd.p_upper
                  << "  " << std::setw(7) << cmd.p_bat_cmd_kw
                  << "  " << cmd.reason << "\n";
    }
}

int main() {
    std::cout << "============================================================\n"
              << " 04/ 策略管理层 & 仲裁器 — 演示\n"
              << " 对应设计方案：周期 3（策略管理器） + 周期 4（策略仲裁器）\n"
              << "============================================================\n";

    StrategyManager mgr;
    StrategyArbiter arb;
    arb.set_deadband(2.0);
    arb.set_switch_delay(3);

    register_all_9_strategies(mgr);
    mgr.start_all();

    std::cout << "  已注册 " << mgr.size() << " 个策略：\n";
    for (const auto& id : mgr.list_ids()) {
        std::cout << "    - " << id << "\n";
    }

    run_typical_scenario(mgr, arb);
    run_24h_rolling_demo(mgr, arb);
    run_fault_injection(mgr, arb);

    std::cout << "\n============================================================\n"
              << " 演示结束\n"
              << "============================================================\n";
    return 0;
}