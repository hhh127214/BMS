// =====================================================================
// 08/ 单元测试（assert 风格，单文件可编译）—— 周期 8 优化调度与实时协同
//
//   T17: 计划 JSON 解析（01/ 格式）+ 时间采样
//   T18: 贪心兜底优化器（含光伏余电裕度预留）
//   T19: 协调器纠偏有界 + 滚动重优化
//   T20: 24h 端到端分层协同（优化层规划 → 实时层纠偏 → 安全层兜底）
//
// 编译：g++ -std=c++17 -Wall -O2 -I src -I ../04/src -I ../05/src
//           -I ../06/src -I ../07/src tests/test_dispatch_coordinator.cpp
//           -o build/test_dispatch_coordinator.exe
// 注：T20 用 07/ 的 EmsRuntime 做端到端，故需 -I ../07/src。
// =====================================================================

#include "data_models.h"
#include "strategy_base.h"
#include "strategy_manager.h"
#include "strategy_arbiter.h"
#include "strategies_9.h"

#include "safety_engine.h"
#include "state_machine.h"
#include "plant_model.h"
#include "plan_loader.h"
#include "dispatch_coordinator.h"
#include "realtime_loop.h"

#include <cmath>
#include <cstdio>
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

#define EXPECT_NEAR(a, b, eps)                                            \
    do {                                                                  \
        double va = (a), vb = (b);                                        \
        if (std::fabs(va - vb) <= (eps)) {                                \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va << " vs " << #b << "="  \
                      << vb << " (eps " << (eps) << ")" << std::endl;     \
        }                                                                 \
    } while (0)

// =====================================================================
// T17: 计划 JSON 解析 + 时间采样
// =====================================================================
static void test_17_plan_json_parse() {
    std::cerr << "[T17] 计划 JSON 解析 ...\n";
    const char* text = R"({
      "status":"ok","strategy":"arbitrage","horizon":4,"time_step_hours":0.25,
      "final_soc":0.7,"total_profit_yuan":123.4,
      "plan":[
        {"t":0,"charge_kw":100.0,"discharge_kw":0.0,"soc":0.55,"grid_kw":50.0},
        {"t":1,"charge_kw":0.0,"discharge_kw":80.0,"soc":0.53,"grid_kw":120.0},
        {"t":2,"charge_kw":0.0,"discharge_kw":0.0,"soc":0.53,"grid_kw":200.0},
        {"t":3,"charge_kw":0.0,"discharge_kw":60.0,"soc":0.51,"grid_kw":150.0}
      ]
    })";
    DayPlan p;
    std::string err;
    EXPECT(parse_plan_json(text, p, &err));
    EXPECT(p.loaded);
    EXPECT(p.horizon == 4);
    EXPECT_NEAR(p.time_step_hours, 0.25, 1e-9);
    EXPECT_NEAR(p.step_s, 900.0, 1e-9);
    EXPECT(p.points.size() == 4);
    EXPECT(std::string(p.strategy) == "arbitrage");

    // charge_kw / discharge_kw 都是非负幅度 → p_plan = discharge − charge
    EXPECT_NEAR(p.points[0].p_plan_kw, -100.0, 1e-9);
    EXPECT_NEAR(p.points[1].p_plan_kw,   80.0, 1e-9);
    EXPECT_NEAR(p.points[2].p_plan_kw,    0.0, 1e-9);

    // 时间采样（t 为秒）
    double pp = 0.0, sr = 0.0;
    p.sample(0.0, &pp, &sr);
    EXPECT_NEAR(pp, -100.0, 1e-9);
    p.sample(900.0, &pp, &sr);
    EXPECT_NEAR(pp, 80.0, 1e-9);
    p.sample(1800.0, &pp, &sr);
    EXPECT_NEAR(pp, 0.0, 1e-9);
    // 超出 horizon → 按日周期回绕
    p.sample(3600.0, &pp, &sr);
    EXPECT_NEAR(pp, -100.0, 1e-9);

    // 计划电量统计
    EXPECT_NEAR(p.planned_charge_kwh(),    100.0 * 0.25, 1e-6);
    EXPECT_NEAR(p.planned_discharge_kwh(), 140.0 * 0.25, 1e-6);

    // 非法输入
    DayPlan bad;
    EXPECT(!parse_plan_json("not json at all", bad, &err));
}

// =====================================================================
// T18: 贪心兜底优化器
// =====================================================================
static void test_18_greedy_planner() {
    std::cerr << "[T18] 贪心兜底优化器 ...\n";
    const int n = 96;
    std::vector<double> price(n), load(n), pv(n);
    for (int i = 0; i < n; ++i) {
        double h = i * 0.25;
        price[i] = (h < 7.0) ? 0.30 : (h < 9.0 ? 0.60 : (h < 12.0 ? 1.00 :
                   (h < 14.0 ? 0.60 : (h < 17.0 ? 1.00 : (h < 21.0 ? 1.20 :
                   (h < 23.0 ? 0.60 : 0.30))))));
        load[i] = 180.0 + 120.0 * std::exp(-std::pow(h - 10.0, 2.0) / 8.0)
                        + 160.0 * std::exp(-std::pow(h - 19.0, 2.0) / 6.0);
        pv[i] = (h >= 6.0 && h <= 18.0)
                    ? 300.0 * std::sin(3.14159265358979 * (h - 6.0) / 12.0) : 0.0;
    }
    DayPlan p = build_plan_greedy(price, load, pv, 0.50, 0.10, 0.90,
                                  1000.0, 250.0, 250.0, 0.25, 0);
    EXPECT(p.loaded);
    EXPECT((int)p.points.size() == n);
    EXPECT_NEAR(p.step_s, 900.0, 1e-9);

    // 功率不越界
    for (const auto& pt : p.points) {
        EXPECT(pt.p_plan_kw >= -250.0 - 1e-6);
        EXPECT(pt.p_plan_kw <=  250.0 + 1e-6);
    }
    // SOC 始终在安全区间
    for (const auto& pt : p.points) {
        EXPECT(pt.soc_ref >= 0.10 - 1e-6);
        EXPECT(pt.soc_ref <= 0.90 + 1e-6);
    }
    // 不允许倒送：放电不超过净负荷
    for (int i = 0; i < n; ++i) {
        double net = load[i] - pv[i];
        if (p.points[i].p_plan_kw > 0.0)
            EXPECT(p.points[i].p_plan_kw <= std::max(0.0, net) + 1e-6);
    }
    // 计划里有充电也有放电（套利行为）
    EXPECT(p.planned_charge_kwh() > 100.0);
    EXPECT(p.planned_discharge_kwh() > 100.0);

    // 光伏余电裕度预留：光伏大发前的 SOC 不应顶到上限
    double soc_at_11h = p.points[44].soc_ref;   // 11:00
    EXPECT(soc_at_11h < 0.90);

    // 从任意起始时段滚动重算，仍规划完整日周期
    DayPlan p2 = build_plan_greedy(price, load, pv, 0.60, 0.10, 0.90,
                                   1000.0, 250.0, 250.0, 0.25, 50);
    EXPECT((int)p2.points.size() == n);
    EXPECT(p2.planned_charge_kwh() > 100.0);
}

// =====================================================================
// T19: 协调器纠偏有界 + 滚动重优化
// =====================================================================
static void test_19_coordinator_bounded_correction() {
    std::cerr << "[T19] 协调器纠偏有界 + 滚动重优化 ...\n";
    DispatchCoordinator coord;
    CoordinatorConfig cc;
    cc.reopt_period_s = 900.0;
    cc.total_correction_max_kw = 100.0;
    coord.set_config(cc);
    coord.set_device_params(0.10, 0.90, 1000.0, 250.0, 250.0);

    HeuristicOptimizer opt;
    coord.set_optimizer(&opt);

    ForecastSeries fc;
    fc.step_s = 900.0;
    const int n = 96;
    for (int i = 0; i < n; ++i) {
        double h = i * 0.25;
        fc.price.push_back((h < 7.0) ? 0.30 : 1.0);
        fc.p_load_kw.push_back(300.0);
        fc.p_pv_kw.push_back((h >= 6.0 && h <= 18.0) ? 200.0 : 0.0);
    }
    fc.loaded = true;
    coord.set_forecast(fc);

    // 制造 SOC 偏差 → 纠偏非零但有界
    RealtimeSnapshot rt;
    rt.soc = 0.85;
    rt.p_bat_actual_kw = 0.0;
    rt.p_load_kw = 300.0;
    rt.p_pv_kw = 0.0;
    coord.update(rt, 0.0, 0.1);          // 首拍触发首次重优化
    EXPECT(coord.plan_valid());
    EXPECT(std::fabs(coord.total_correction_kw()) <= cc.total_correction_max_kw + 1e-9);
    EXPECT(coord.reopt_count() == 1);

    // 900s 后再次重优化
    for (double t = 0.1; t <= 901.0; t += 0.1) coord.update(rt, t, 0.1);
    EXPECT(coord.reopt_count() == 2);

    // 纠偏幅度极端时仍被限幅
    RealtimeSnapshot rt2 = rt;
    rt2.soc = 0.10;
    coord.update(rt2, 1000.0, 0.1);
    EXPECT(std::fabs(coord.total_correction_kw()) <= cc.total_correction_max_kw + 1e-9);

    // coordinated_target = plan + correction
    EXPECT_NEAR(coord.coordinated_target_kw(),
                coord.plan_target_kw() + coord.total_correction_kw(), 1e-9);
}

// =====================================================================
// T20: 24h 端到端分层协同
// =====================================================================
static void test_20_layered_coordination_24h() {
    std::cerr << "[T20] 24h 分层协同端到端 ...\n";
    EmsRuntime rt;
    rt.config().dt_s = 0.1;
    rt.config().log_every = 100;
    rt.config().l2_correction_max_kw = 100.0;

    PlantConfig pc;
    pc.battery_capacity_kwh = 1000.0;
    pc.soc_init = 0.50;
    pc.pcs_max_chg_kw = pc.pcs_max_dis_kw = 250.0;
    pc.pcs_ramp_kw_per_s = 400.0;
    pc.pcs_tau_s = 0.3;
    pc.pcs_deadtime_s = 0.1;
    rt.configure_plant(pc);

    rt.device_limits().pcs_rated_chg_kw = 250.0;
    rt.device_limits().pcs_rated_dis_kw = 250.0;
    rt.device_limits().bms_chg_limit_kw = 250.0;
    rt.device_limits().bms_dis_limit_kw = 250.0;
    rt.device_limits().transformer_capacity_kw = 700.0;
    rt.device_limits().d_target_kw = 350.0;
    rt.safety_params().soc_min = 0.10;
    rt.safety_params().soc_max = 0.90;
    rt.safety_params().ramp_kw_per_s = 300.0;
    rt.safety_params().grid_p_min_kw = 0.0;
    rt.safety_params().grid_p_max_kw = 350.0;
    rt.coordinator_config().reopt_period_s = 900.0;
    rt.apply_configs();

    // 预测序列（与演示一致）
    ForecastSeries fc;
    fc.step_s = 900.0;
    for (int i = 0; i < 96; ++i) {
        double h = i * 0.25;
        double pr = (h < 7.0) ? 0.30 : (h < 9.0 ? 0.60 : (h < 12.0 ? 1.00 :
                    (h < 14.0 ? 0.60 : (h < 17.0 ? 1.00 : (h < 21.0 ? 1.20 :
                    (h < 23.0 ? 0.60 : 0.30))))));
        double ld = 180.0 + 120.0 * std::exp(-std::pow(h - 10.0, 2.0) / 8.0)
                            + 160.0 * std::exp(-std::pow(h - 19.0, 2.0) / 6.0)
                            + 40.0 * std::sin(2.0 * 3.14159265358979 * h / 24.0);
        double pv = (h >= 6.0 && h <= 18.0)
                        ? 300.0 * std::sin(3.14159265358979 * (h - 6.0) / 12.0) : 0.0;
        fc.price.push_back(pr);
        fc.p_load_kw.push_back(ld);
        fc.p_pv_kw.push_back(pv);
    }
    fc.loaded = true;
    rt.set_forecast(fc);

    const int steps = 24 * 3600 * 10;   // 24h @ 10Hz
    rt.run(steps, 0.1, [&fc](EmsRuntime& r, int i) {
        double load = 0.0, pv = 0.0, pr = 0.0;
        fc.sample(i * 0.1, &load, &pv, &pr);
        r.set_environment(load, pv);
    });

    const auto& m = rt.metrics();
    const auto& st = rt.coordinator().stats();

    // ① 优化层：滚动重优化 96 次（每 15 min）
    EXPECT(rt.coordinator().reopt_count() == 96);
    // ② 实时层：纠偏有界
    EXPECT(st.mean_abs_correction() <= 100.0 + 1e-6);
    // ③ 跟踪：闭环跟随良好
    EXPECT(m.mean_abs_track_err_kw < 5.0);
    EXPECT(m.rmse_track_kw < 20.0);
    EXPECT(m.no_lag());
    EXPECT(m.no_oscillation());
    // ④ 安全层兜底：无倒送、契约需量不突破、SOC 不越界
    EXPECT_NEAR(m.max_reverse_kw, 0.0, 1e-6);
    EXPECT(m.max_grid_kw <= 350.0 + 1e-6);
    EXPECT(m.state_gate_count == 0);
    EXPECT(rt.plant().soc() >= 0.10 - 1e-6);
    EXPECT(rt.plant().soc() <= 0.90 + 1e-6);
    // 电池确实在套利（有充有放）
    EXPECT(rt.plant().energy_charged_kwh() > 100.0);
    EXPECT(rt.plant().energy_discharged_kwh() > 100.0);
}

// =====================================================================
int main() {
    std::cerr << "=========================================\n"
              << " 08/ 周期 8 优化调度与实时协同 单元测试\n"
              << "=========================================\n";

    test_17_plan_json_parse();
    test_18_greedy_planner();
    test_19_coordinator_bounded_correction();
    test_20_layered_coordination_24h();

    std::cerr << "=========================================\n"
              << " PASS=" << g_pass << "  FAIL=" << g_fail << "\n"
              << "=========================================\n";
    return (g_fail == 0) ? 0 : 1;
}
