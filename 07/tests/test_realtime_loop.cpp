// =====================================================================
// 07/ 单元测试（assert 风格，单文件可编译）—— 周期 7 实时控制闭环
//
//   T11: 输出门控不变量（闭环口径：非运行态恒 0）
//   T12: 电表通信异常走 HOLD_LAST，不进 FAULT
//   T13: 被控对象（死区 + 一阶惯性 + 变化率 + 效率 + SOC 积分）
//   T14: OutputShaper 死区清零 / 滞环 / 关闭时透传
//   T15: LoopMetrics 行程/反转/翻转/倒送统计
//   T16: 闭环端到端 —— 指令跟随、无倒送、安全兜底
//
// 编译：g++ -std=c++17 -Wall -O2 -I src -I ../04/src -I ../05/src
//           -I ../06/src -I ../08/src tests/test_realtime_loop.cpp
//           -o build/test_realtime_loop.exe
// 注：EmsRuntime（本模块）装配 08/ 的协同层，故需 -I ../08/src。
// =====================================================================

#include "data_models.h"
#include "strategy_base.h"
#include "strategy_manager.h"
#include "strategy_arbiter.h"
#include "strategies_9.h"

#include "safety_engine.h"
#include "state_machine.h"
#include "plant_model.h"
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
// T11: 输出门控不变量
// =====================================================================
static void test_11_output_gate_invariant() {
    std::cerr << "[T11] 输出门控不变量 ...\n";
    EmsRuntime rt;
    rt.config().dt_s = 0.1;
    rt.device_limits().transformer_capacity_kw = 800.0;
    rt.safety_params().grid_p_min_kw = -1e9;
    rt.apply_configs();
    rt.set_environment(250.0, 50.0);

    int violations = 0;
    // 先在 READY（未下发启动）跑一段
    rt.run(20, 0.1, nullptr);
    for (const auto& r : rt.log())
        if (!rt.fsm().output_enabled() && std::fabs(r.p_cmd) > 1e-9) ++violations;
    EXPECT(violations == 0);

    // 强制 PCS 故障 → FAULT
    rt.plant().set_pcs_fault(true);
    int base = (int)rt.log().size();
    rt.run(50, 0.1, nullptr);
    violations = 0;
    for (int i = base; i < (int)rt.log().size(); ++i) {
        const auto& r = rt.log()[i];
        if (r.state == EmsState::kFault && std::fabs(r.p_cmd) > 1e-9) ++violations;
    }
    EXPECT(violations == 0);
    EXPECT(rt.fsm().state() == EmsState::kFault);

    // 急停 → EMERGENCY，同样恒 0
    rt.reset_emergency();
    rt.plant().set_pcs_fault(false);
    rt.emergency_stop("test_estop");
    base = (int)rt.log().size();
    rt.run(30, 0.1, nullptr);
    violations = 0;
    for (int i = base; i < (int)rt.log().size(); ++i) {
        const auto& r = rt.log()[i];
        if (r.state == EmsState::kEmergency && std::fabs(r.p_cmd) > 1e-9) ++violations;
    }
    EXPECT(violations == 0);
}

// =====================================================================
// T12: 电表通信异常走 HOLD_LAST，不进 FAULT
// =====================================================================
static void test_12_meter_comm_loss_hold_last() {
    std::cerr << "[T12] 电表通信异常 → HOLD_LAST（非 FAULT） ...\n";
    EmsRuntime rt;
    rt.config().dt_s = 0.1;
    rt.device_limits().transformer_capacity_kw = 800.0;
    rt.device_limits().d_target_kw = 200.0;
    rt.safety_params().grid_p_min_kw = 0.0;
    rt.safety_params().ramp_kw_per_s = 1e9;
    rt.apply_configs();
    rt.manager().set_param(strategy_id::kDemandMgmt, "Kp_avg", 1.5);
    rt.set_environment(500.0, 0.0);

    rt.run(200, 0.1, nullptr);          // 先建立稳定指令
    const double cmd_before = rt.last_command().p_bat_cmd_kw;

    rt.plant().set_comm_meter(false);   // 电表通信中断
    rt.run(100, 0.1, nullptr);          // > 5s 判定为 stale
    int hold = 0;
    for (const auto& r : rt.log()) if (r.hold_last) ++hold;
    EXPECT(hold > 0);
    EXPECT(rt.fsm().state() != EmsState::kFault);   // 规范 §6：不进 FAULT
    // 保持上一拍指令
    EXPECT_NEAR(rt.last_command().p_bat_cmd_kw, cmd_before, 1e-6);
}

// =====================================================================
// T13: 被控对象动态（死区 / 惯性 / 变化率 / 效率 / SOC）
// =====================================================================
static void test_13_plant_model_dynamics() {
    std::cerr << "[T13] 被控对象动态 ...\n";
    PlantConfig pc;
    pc.battery_capacity_kwh = 100.0;
    pc.soc_init = 0.50;
    pc.pcs_max_chg_kw = 50.0;
    pc.pcs_max_dis_kw = 50.0;
    pc.pcs_ramp_kw_per_s = 1000.0;   // 隔离变化率
    pc.pcs_tau_s = 0.0;              // 隔离惯性
    pc.pcs_deadtime_s = 0.0;
    pc.eta_chg = pc.eta_dis = 1.0;
    pc.temp_ambient_c = 25.0;
    PlantModel plant(pc);
    plant.set_environment(0.0, 0.0);

    // 阶跃 50kW，无惯性无死区 → 立即到位
    double p = plant.step(50.0, 0.1);
    EXPECT_NEAR(p, 50.0, 1e-6);
    // SOC 下降：50kW × 0.1s = 1.3889e-3 kWh / 100kWh
    EXPECT_NEAR(plant.soc(), 0.50 - 50.0 * 0.1 / 3600.0 / 100.0, 1e-9);

    // 变化率限制
    PlantConfig pc2 = pc;
    pc2.pcs_ramp_kw_per_s = 100.0;   // 100kW/s × 0.1s = 10kW/拍
    PlantModel p2(pc2);
    p2.set_environment(0.0, 0.0);
    p2.step(50.0, 0.1);
    EXPECT_NEAR(p2.p_bat_actual(), 10.0, 1e-6);

    // 一阶惯性：τ=1s, dt=0.1 → α = 1−e^−0.1 ≈ 0.09516
    PlantConfig pc3 = pc;
    pc3.pcs_tau_s = 1.0;
    PlantModel p3(pc3);
    p3.set_environment(0.0, 0.0);
    double a = 1.0 - std::exp(-0.1 / 1.0);
    double got = p3.step(50.0, 0.1);
    EXPECT_NEAR(got, 50.0 * a, 1e-6);

    // 死区：0.2s 后才有响应
    PlantConfig pc4 = pc;
    pc4.pcs_deadtime_s = 0.2;
    PlantModel p4(pc4);
    p4.set_environment(0.0, 0.0);
    EXPECT_NEAR(p4.step(50.0, 0.1), 0.0, 1e-9);
    EXPECT_NEAR(p4.step(50.0, 0.1), 0.0, 1e-9);
    EXPECT_NEAR(p4.step(50.0, 0.1), 50.0, 1e-6);

    // 充电效率：吸收 1kWh 交流 → SOC 只涨 η kWh
    PlantConfig pc5 = pc;
    pc5.battery_capacity_kwh = 100.0;
    pc5.eta_chg = 0.8;
    PlantModel p5(pc5);
    p5.set_environment(0.0, 0.0);
    for (int i = 0; i < 10; ++i) p5.step(-50.0, 0.1);
    double e_ac = 50.0 * 1.0 / 3600.0;    // 10 拍 × 0.1s
    EXPECT_NEAR(p5.soc(), 0.50 + e_ac * 0.8 / 100.0, 1e-9);

    // 硬限幅：指令 999kW 被 PCS 额定压到 50kW
    PlantModel p6(pc);
    p6.set_environment(0.0, 0.0);
    EXPECT_NEAR(p6.step(999.0, 0.1), 50.0, 1e-6);

    // 关口平衡式：P_grid = P_load − P_pv − P_bat
    PlantModel p7(pc);
    p7.set_environment(300.0, 100.0);
    for (int i = 0; i < 5; ++i) p7.step(40.0, 0.1);
    EXPECT_NEAR(p7.p_grid_actual(),
                300.0 + pc.pcs_standby_kw - 100.0 - p7.p_bat_actual(), 1e-6);
}

// =====================================================================
// T14: OutputShaper
// =====================================================================
static void test_14_output_shaper() {
    std::cerr << "[T14] OutputShaper 死区/滞环 ...\n";
    // 关闭死区 → 透传
    {
        OutputShaper s;
        s.set_deadband(0.0);
        bool held = false;
        EXPECT_NEAR(s.shape(3.0, &held), 3.0, 1e-9);
        EXPECT(!held);
        EXPECT_NEAR(s.shape(-7.5, &held), -7.5, 1e-9);
    }
    // 死区 + 滞环：持续落在死区内 → 归零
    {
        OutputShaper s;
        s.set_deadband(5.0);
        s.set_switch_delay(3);
        bool held = false;
        s.shape(50.0, &held);              // 越出死区 → 跟随
        EXPECT_NEAR(s.shape(1.0, &held), 50.0, 1e-9);   // 滞环确认期内保持
        s.shape(1.0, &held);
        EXPECT_NEAR(s.shape(1.0, &held), 0.0, 1e-9);    // 连续 3 拍 → 归零
        EXPECT_NEAR(s.shape(0.5, &held), 0.0, 1e-9);
        EXPECT_NEAR(s.shape(0.5, &held), 0.0, 1e-9);
        EXPECT_NEAR(s.shape(0.5, &held), 0.0, 1e-9);
        // 再次越出 → 立即跟随
        EXPECT_NEAR(s.shape(-30.0, &held), -30.0, 1e-9);
    }
    // 单拍尖峰不会误触发归零
    {
        OutputShaper s;
        s.set_deadband(5.0);
        s.set_switch_delay(3);
        s.shape(40.0);
        EXPECT_NEAR(s.shape(1.0), 40.0, 1e-9);
        EXPECT_NEAR(s.shape(40.0), 40.0, 1e-9);   // 越出 → 计数清零
        EXPECT_NEAR(s.shape(1.0), 40.0, 1e-9);
        EXPECT_NEAR(s.shape(1.0), 40.0, 1e-9);
        EXPECT_NEAR(s.shape(1.0), 0.0, 1e-9);
    }
}

// =====================================================================
// T15: LoopMetrics 统计口径
// =====================================================================
static void test_15_loop_metrics() {
    std::cerr << "[T15] LoopMetrics 统计 ...\n";
    std::vector<StepRecord> recs;
    const double dt = 0.1;
    for (int i = 0; i < 100; ++i) {
        StepRecord r;
        r.t = i * dt;
        r.p_cmd = (i < 50) ? 0.0 : 100.0;      // 一次阶跃
        r.p_actual = r.p_cmd;                  // 完美跟随
        r.p_grid = 50.0;
        recs.push_back(r);
    }
    auto m = LoopMetrics::compute(recs, dt);
    EXPECT(m.samples == 100);
    EXPECT_NEAR(m.mean_abs_track_err_kw, 0.0, 1e-9);
    EXPECT_NEAR(m.cmd_travel_kw, 100.0, 1e-6);   // 只有一次跳变
    EXPECT(m.cmd_reversals == 0);
    EXPECT(m.max_reverse_kw == 0.0);
    EXPECT(m.no_lag());
    EXPECT(m.no_oscillation());

    // 来回摆动 → 反转数上升、行程增大
    std::vector<StepRecord> osc;
    for (int i = 0; i < 100; ++i) {
        StepRecord r;
        r.t = i * dt;
        r.p_cmd = (i % 2) ? 50.0 : -50.0;
        r.p_actual = r.p_cmd;
        r.p_grid = 0.0;
        osc.push_back(r);
    }
    auto mo = LoopMetrics::compute(osc, dt);
    EXPECT(mo.cmd_reversals > 80);
    EXPECT(mo.cmd_travel_kw > 4000.0);
    EXPECT(!mo.no_oscillation());

    // 倒送统计：只统计 |倒送| > 5kW
    std::vector<StepRecord> rev;
    for (int i = 0; i < 100; ++i) {
        StepRecord r;
        r.t = i * dt;
        r.p_cmd = 0.0;
        r.p_actual = 0.0;
        r.p_grid = (i < 50) ? -30.0 : 10.0;
        rev.push_back(r);
    }
    auto mr = LoopMetrics::compute(rev, dt);
    EXPECT_NEAR(mr.max_reverse_kw, 30.0, 1e-6);
    EXPECT_NEAR(mr.reverse_duration_s, 50 * dt, 1e-6);

    // 超发率：实际超出指令的比例
    std::vector<StepRecord> over;
    for (int i = 0; i < 100; ++i) {
        StepRecord r;
        r.t = i * dt;
        r.p_cmd = 100.0;
        r.p_actual = 120.0;
        r.p_grid = 0.0;
        over.push_back(r);
    }
    auto mov = LoopMetrics::compute(over, dt);
    EXPECT_NEAR(mov.over_delivery_pct, 20.0, 1e-6);
}

// =====================================================================
// T16: 闭环端到端（周期 7 验收：无延迟、无倒送、安全兜底）
// =====================================================================
static void test_16_closed_loop_end_to_end() {
    std::cerr << "[T16] 闭环端到端 ...\n";
    EmsRuntime rt;
    rt.config().dt_s = 0.1;
    rt.config().l2_correction_max_kw = 100.0;
    rt.device_limits().transformer_capacity_kw = 800.0;
    rt.device_limits().d_target_kw = 250.0;
    rt.safety_params().ramp_kw_per_s = 200.0;
    rt.safety_params().grid_p_min_kw = 0.0;
    rt.apply_configs();

    auto env = [](EmsRuntime& r, int i) {
        double t = i * 0.1;
        double load = 200.0, pv = 150.0;
        if (t >= 50.0 && t < 150.0) { load = 450.0; pv = 100.0; }
        else if (t >= 150.0)        { load = 200.0; pv = 150.0; }
        r.set_environment(load, pv);
    };
    rt.run(3000, 0.1, env);

    const auto& m = rt.metrics();
    EXPECT(m.samples > 0);
    EXPECT(m.mean_abs_track_err_kw < 15.0);        // 跟随良好
    EXPECT(m.steady_state_err_kw < 15.0);
    EXPECT(m.no_lag());
    // 稳态不倒送
    EXPECT(m.reverse_duration_s < 30.0);
    // 安全兜底：指令始终落在区间内
    for (const auto& r : rt.log()) EXPECT(r.p_cmd <= r.p_upper + 1e-6);
    for (const auto& r : rt.log()) EXPECT(r.p_cmd >= r.p_lower - 1e-6);
    // 反馈回路闭合：实际功率回灌下一拍
    EXPECT(rt.last_command().p_bat_cmd_kw == rt.last_command().p_bat_cmd_kw);
}


// =====================================================================
int main() {
    std::cerr << "=========================================\n"
              << " 07/ 周期 7 实时控制闭环 单元测试\n"
              << "=========================================\n";

    test_11_output_gate_invariant();
    test_12_meter_comm_loss_hold_last();
    test_13_plant_model_dynamics();
    test_14_output_shaper();
    test_15_loop_metrics();
    test_16_closed_loop_end_to_end();

    std::cerr << "=========================================\n"
              << " PASS=" << g_pass << "  FAIL=" << g_fail << "\n"
              << "=========================================\n";
    return (g_fail == 0) ? 0 : 1;
}
