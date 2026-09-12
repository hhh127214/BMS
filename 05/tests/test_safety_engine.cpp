// =====================================================================
// 05/ 单元测试（assert 风格，单文件可编译）—— 周期 5 安全约束引擎
//
//   T01: 9 类约束逐条区间语义（禁充/禁放/SOC/温度/PCS/变压器/并网）
//   T02: 多约束区间求交 + binding trace（谁在限）
//   T03: 区间矛盾 → [0,0] + derated（**不得**判为紧急）
//   T04: 变化率走 slew limiter 后置限速，不参与求交 → 无假矛盾
//   T05: 并网约束区间推导（base−g_min / base−g_max）
//   T06: 并网边界量测低通滤波（边界去抖）
//
// 编译：g++ -std=c++17 -Wall -O2 -I src -I ../04/src -I ../06/src
//           tests/test_safety_engine.cpp -o build/test_safety_engine.exe
//
// 注：T03 末尾用 06/ 的状态机交叉验证「矛盾 → DERATED 且仍允许输出」，
//     故编译需 -I ../06/src。头文件层面**无环**：state_machine.h →
//     safety_engine.h（单向）。
// =====================================================================

#include "data_models.h"
#include "strategy_base.h"
#include "safety_engine.h"
#include "state_machine.h"

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

// ---------------------------------------------------------------------
// 公共夹具
// ---------------------------------------------------------------------
namespace {

DeviceLimits dev_basic() {
    DeviceLimits d;
    d.pcs_rated_chg_kw = 200.0;
    d.pcs_rated_dis_kw = 200.0;
    d.bms_chg_limit_kw = 200.0;
    d.bms_dis_limit_kw = 200.0;
    d.transformer_capacity_kw = 1000.0;
    d.d_target_kw = 250.0;
    return d;
}

RealtimeSnapshot rt_basic() {
    RealtimeSnapshot rt;
    rt.timestamp  = 0.0;
    rt.p_load_kw  = 300.0;
    rt.p_pv_kw    = 100.0;
    rt.p_bat_actual_kw = 0.0;
    rt.p_grid_kw  = 200.0;
    rt.soc        = 0.50;
    rt.temperature_c = 25.0;
    rt.demand_window.t_elapsed_s = 300.0;
    rt.demand_window.window_s    = 900.0;
    rt.demand_window.p_avg_past_kw = 200.0;
    return rt;
}

const ConstraintResult* find_item(const SafetyVerdict& v, const char* name) {
    for (const auto& it : v.items) if (it.name == name) return &it;
    return nullptr;
}

bool has_binding(const SafetyVerdict& v, const char* name) {
    for (const auto& b : v.binding) if (b == name) return true;
    return false;
}

}  // namespace

// =====================================================================
// T01: 9 类约束逐条区间语义
// =====================================================================
static void test_01_per_constraint_intervals() {
    std::cerr << "[T01] 9 类约束逐条区间语义 ...\n";
    SafetyEngine eng;
    SafetyParams p;
    p.ramp_kw_per_s = 1e9;          // 隔离变化率
    p.grid_p_min_kw = -1e9;         // 隔离并网
    eng.set_params(p);

    DeviceLimits dev = dev_basic();
    GridQuality grid;

    // --- BMS 禁充 → p_lower = 0 ---
    {
        DeviceLimits d = dev; d.bms_chg_forbidden = true;
        auto v = eng.evaluate(rt_basic(), d, grid, 0.0, 0.1);
        const auto* c = find_item(v, SafetyEngine::kBmsForbid);
        EXPECT(c != nullptr);
        EXPECT(c && c->active);
        EXPECT_NEAR(c ? c->p_lower : -1.0, 0.0, 1e-9);
        EXPECT_NEAR(c ? c->p_upper : 0.0, 1e18, 1e15);
    }
    // --- BMS 禁放 → p_upper = 0 ---
    {
        DeviceLimits d = dev; d.bms_dis_forbidden = true;
        auto v = eng.evaluate(rt_basic(), d, grid, 0.0, 0.1);
        const auto* c = find_item(v, SafetyEngine::kBmsForbid);
        EXPECT(c && c->active);
        EXPECT_NEAR(c ? c->p_upper : 1.0, 0.0, 1e-9);
    }
    // --- BMS 禁充 + 禁放 → [0, 0] ---
    {
        DeviceLimits d = dev;
        d.bms_chg_forbidden = d.bms_dis_forbidden = true;
        auto v = eng.evaluate(rt_basic(), d, grid, 0.0, 0.1);
        EXPECT_NEAR(v.p_lower, 0.0, 1e-9);
        EXPECT_NEAR(v.p_upper, 0.0, 1e-9);
        EXPECT(v.l0_hard);
    }
    // --- SOC 触及下限 → 禁放（p_upper = 0） ---
    {
        SafetyEngine e2; e2.set_params(p);
        RealtimeSnapshot rt = rt_basic(); rt.soc = 0.08;
        auto v = e2.evaluate(rt, dev, grid, 0.0, 0.1);
        const auto* c = find_item(v, SafetyEngine::kSocLimit);
        EXPECT(c && c->active);
        EXPECT_NEAR(c ? c->p_upper : 1.0, 0.0, 1e-9);
        EXPECT(std::string(c ? c->reason : "") == "soc_min_reached");
        EXPECT(!c->counts_as_derate);   // 硬禁闭，不是降额
    }
    // --- SOC 触及上限 → 禁充（p_lower = 0） ---
    {
        SafetyEngine e2; e2.set_params(p);
        RealtimeSnapshot rt = rt_basic(); rt.soc = 0.95;
        auto v = e2.evaluate(rt, dev, grid, 0.0, 0.1);
        const auto* c = find_item(v, SafetyEngine::kSocLimit);
        EXPECT(c && c->active);
        EXPECT_NEAR(c ? c->p_lower : -1.0, 0.0, 1e-9);
        EXPECT(std::string(c ? c->reason : "") == "soc_max_reached");
    }
    // --- SOC 预警区 → 折减 50%（属降额，不是硬禁闭） ---
    {
        SafetyEngine e2; e2.set_params(p);
        RealtimeSnapshot rt = rt_basic(); rt.soc = 0.86;
        auto v = e2.evaluate(rt, dev, grid, 0.0, 0.1);
        const auto* c = find_item(v, SafetyEngine::kSocLimit);
        EXPECT(c && c->active);
        EXPECT_NEAR(c ? c->p_lower : 0.0, -100.0, 1e-6);
        EXPECT_NEAR(c ? c->p_upper : 0.0,  100.0, 1e-6);
        EXPECT(c && c->counts_as_derate);
        EXPECT(v.derated);
    }
    // --- 温度故障 58℃ → 紧急 + [0,0] ---
    {
        SafetyEngine e2; e2.set_params(p);
        RealtimeSnapshot rt = rt_basic(); rt.temperature_c = 58.0;
        auto v = e2.evaluate(rt, dev, grid, 0.0, 0.1);
        EXPECT(v.emergency);
        EXPECT_NEAR(v.p_lower, 0.0, 1e-9);
        EXPECT_NEAR(v.p_upper, 0.0, 1e-9);
    }
    // --- 温度预警 47℃ → 折减 ---
    {
        SafetyEngine e2; e2.set_params(p);
        RealtimeSnapshot rt = rt_basic(); rt.temperature_c = 47.0;
        auto v = e2.evaluate(rt, dev, grid, 0.0, 0.1);
        const auto* c = find_item(v, SafetyEngine::kTemp);
        EXPECT(c && c->active);
        EXPECT(c && c->counts_as_derate);
        EXPECT(!v.emergency);
    }
    // --- PCS 降额 0.6 → ±120 ---
    {
        SafetyEngine e2; SafetyParams p2 = p; p2.pcs_derate_ratio = 0.6;
        e2.set_params(p2);
        auto v = e2.evaluate(rt_basic(), dev, grid, 0.0, 0.1);
        const auto* c = find_item(v, SafetyEngine::kPcsLimit);
        EXPECT(c != nullptr);
        EXPECT_NEAR(c ? c->p_lower : 0.0, -120.0, 1e-6);
        EXPECT_NEAR(c ? c->p_upper : 0.0,  120.0, 1e-6);
    }
    // --- BMS 上送限制比 PCS 更小 → 取更紧者 ---
    {
        DeviceLimits d = dev;
        d.bms_chg_limit_kw = 80.0;
        d.bms_dis_limit_kw = 60.0;
        auto v = eng.evaluate(rt_basic(), d, grid, 0.0, 0.1);
        const auto* c = find_item(v, SafetyEngine::kBmsDerate);
        EXPECT(c != nullptr);
        EXPECT_NEAR(c ? c->p_lower : 0.0, -80.0, 1e-6);
        EXPECT_NEAR(c ? c->p_upper : 0.0,  60.0, 1e-6);
    }
    // --- 变压器极端过载 → 禁放 ---
    {
        SafetyEngine e2;
        SafetyParams p2 = p;
        p2.tr_overload_th = 0.95;
        e2.set_params(p2);
        RealtimeSnapshot rt = rt_basic();
        rt.p_load_kw = 1200.0;
        rt.p_grid_kw = 1200.0;      // tr_load = |P_grid| + 0.1*P_load = 1320 > 1.10*1000
        rt.p_pv_kw   = 0.0;
        auto v = e2.evaluate(rt, dev, grid, 0.0, 0.1);
        const auto* c = find_item(v, SafetyEngine::kTransformer);
        EXPECT(c && c->active);
        EXPECT_NEAR(c ? c->p_upper : 1.0, 0.0, 1e-6);
    }
    // --- 并网频率越限 → 紧急 + [0,0] ---
    {
        SafetyEngine e2; e2.set_params(p);
        GridQuality g; g.freq_hz = 51.0;
        auto v = e2.evaluate(rt_basic(), dev, g, 0.0, 0.1);
        EXPECT(v.emergency);
        EXPECT_NEAR(v.p_lower, 0.0, 1e-9);
        EXPECT_NEAR(v.p_upper, 0.0, 1e-9);
    }
    // --- 并网电压越限 → 紧急 ---
    {
        SafetyEngine e2; e2.set_params(p);
        GridQuality g; g.volt_pu = 1.15;
        auto v = e2.evaluate(rt_basic(), dev, g, 0.0, 0.1);
        EXPECT(v.emergency);
    }
    // --- 通信丢失（BMS） → [0,0] ---
    {
        DeviceLimits d = dev;
        d.bms_chg_forbidden = d.bms_dis_forbidden = true;   // 通信丢失的上送表现
        auto v = eng.evaluate(rt_basic(), d, grid, 0.0, 0.1);
        EXPECT_NEAR(v.p_lower, 0.0, 1e-9);
        EXPECT_NEAR(v.p_upper, 0.0, 1e-9);
    }
}

// =====================================================================
// T02: 区间求交 + binding trace
// =====================================================================
static void test_02_interval_intersection_and_binding() {
    std::cerr << "[T02] 区间求交 + binding trace ...\n";
    SafetyEngine eng;
    SafetyParams p;
    p.ramp_kw_per_s = 1e9;
    p.grid_p_min_kw = -1e9;
    eng.set_params(p);

    DeviceLimits dev = dev_basic();
    dev.bms_dis_limit_kw = 150.0;   // 比 PCS 200 更紧
    GridQuality grid;
    auto v = eng.evaluate(rt_basic(), dev, grid, 0.0, 0.1);

    // 收敛后上界 = min(所有上界) = 150（BMS 限制）
    EXPECT_NEAR(v.p_upper, 150.0, 1e-6);
    EXPECT(has_binding(v, SafetyEngine::kBmsDerate));
    // 所有约束都不 active 时不应出现在 binding 里
    EXPECT(!has_binding(v, SafetyEngine::kBmsForbid));
    EXPECT(v.p_lower <= v.p_upper + 1e-9);
}

// =====================================================================
// T03: 区间矛盾 → [0,0] + derated，且**不判为紧急**
// =====================================================================
static void test_03_contradiction_is_derate_not_emergency() {
    std::cerr << "[T03] 区间矛盾 → [0,0] + DERATED（非紧急） ...\n";
    SafetyEngine eng;
    SafetyParams p;
    p.ramp_kw_per_s = 1e9;
    p.grid_p_min_kw = 0.0;      // 不允许倒送
    eng.set_params(p);

    DeviceLimits dev = dev_basic();
    GridQuality grid;
    RealtimeSnapshot rt = rt_basic();
    rt.soc = 0.95;              // 满充 → 禁充（下界 0）
    rt.p_pv_kw = 400.0;         // 光伏大发 → base = 300−400 = −100 < 0
    rt.p_load_kw = 300.0;

    auto v = eng.evaluate(rt, dev, grid, 0.0, 0.1);
    EXPECT(v.contradiction);
    EXPECT(v.derated);          // 语义：系统受限运行
    EXPECT(!v.emergency);       // 关键：不是设备紧急，不得锁存
    EXPECT_NEAR(v.p_lower, 0.0, 1e-9);
    EXPECT_NEAR(v.p_upper, 0.0, 1e-9);

    // 状态机侧确认：矛盾不触发 EMERGENCY
    StateMachineConfig cfg;
    cfg.init_hold_s = 0.0;
    cfg.self_check_cycles = 1;
    EmsStateMachine fsm(cfg);
    fsm.reset_all();
    fsm.request_run(true);
    FaultFlags none;
    for (int i = 0; i < 6; ++i) fsm.update(i * 0.1, none, SafetyVerdict{});
    EXPECT(fsm.state() == EmsState::kNormal);
    fsm.update(1.0, none, v);   // 喂入矛盾裁决
    EXPECT(fsm.state() == EmsState::kDerated);
    EXPECT(fsm.output_enabled());   // DERATED 仍允许输出（由安全层压到 0）
}

// =====================================================================
// T04: 变化率走 slew limiter，不参与求交 → 无假矛盾
// =====================================================================
static void test_04_ramp_is_slew_not_interval() {
    std::cerr << "[T04] 变化率 slew limiter（不产生假矛盾） ...\n";
    SafetyEngine eng;
    SafetyParams p;
    p.ramp_kw_per_s = 50.0;
    p.grid_p_min_kw = 0.0;
    eng.set_params(p);

    DeviceLimits dev = dev_basic();
    GridQuality grid;
    RealtimeSnapshot rt = rt_basic();
    rt.p_load_kw = 300.0;
    rt.p_pv_kw   = 100.0;       // base = 200

    // 上拍指令 +100，本拍限 50kW/s × 0.1s = 5kW → 允许 [-95, +105]
    auto v = eng.evaluate(rt, dev, grid, 100.0, 0.1);
    EXPECT(!v.contradiction);
    EXPECT(v.ramp_active);
    EXPECT_NEAR(v.ramp_limit_kw, 5.0, 1e-9);

    // 变化率约束不得参与区间求交
    const auto* c = find_item(v, SafetyEngine::kRamp);
    EXPECT(c != nullptr);
    EXPECT(c && !c->binds_interval);
    EXPECT(!has_binding(v, SafetyEngine::kRamp));

    // apply() 做后置限速：目标 +200 → 被限到 +105
    PowerCommand cmd;
    cmd.p_bat_cmd_kw = 200.0;
    cmd.p_lower = v.p_lower;
    cmd.p_upper = v.p_upper;
    eng.apply(cmd);
    EXPECT_NEAR(cmd.p_bat_cmd_kw, 105.0, 1e-6);

    // 反向同样受限：上一拍为 +100，本拍允许区间仍是 [95, 105]
    // （变化率是"相对上拍实际下发值"的邻域，不是绝对区间 → 不会产生假矛盾）
    PowerCommand cmd2;
    cmd2.p_bat_cmd_kw = -200.0;
    cmd2.p_lower = v.p_lower;
    cmd2.p_upper = v.p_upper;
    eng.apply(cmd2);
    EXPECT_NEAR(cmd2.p_bat_cmd_kw, 95.0, 1e-6);
}

// =====================================================================
// T05: 并网约束区间推导
// =====================================================================
static void test_05_grid_interval_derivation() {
    std::cerr << "[T05] 并网约束区间推导 ...\n";
    SafetyEngine eng;
    SafetyParams p;
    p.ramp_kw_per_s = 1e9;
    p.grid_p_min_kw = 0.0;      // 不允许倒送
    p.grid_p_max_kw = 150.0;    // 进线容量
    eng.set_params(p);

    DeviceLimits dev = dev_basic();
    GridQuality grid;
    RealtimeSnapshot rt = rt_basic();
    rt.p_load_kw = 300.0;
    rt.p_pv_kw   = 100.0;       // base = 200

    auto v = eng.evaluate(rt, dev, grid, 0.0, 0.1);
    // 不允许倒送：P_grid >= 0 → p_upper <= base − 0 = 200
    // 容量上限：  P_grid <= 150 → p_lower >= base − 150 = 50
    EXPECT_NEAR(v.p_upper, 200.0, 1e-6);
    EXPECT_NEAR(v.p_lower,  50.0, 1e-6);
    EXPECT(has_binding(v, SafetyEngine::kGridConnect));
}

// =====================================================================
// T06: 并网边界量测低通滤波（边界去抖）
// =====================================================================
static void test_06_grid_measurement_filter() {
    std::cerr << "[T06] 并网边界量测滤波 ...\n";
    SafetyParams p;
    p.ramp_kw_per_s = 1e9;
    p.grid_p_min_kw = 0.0;

    DeviceLimits dev = dev_basic();
    GridQuality grid;

    // 关闭滤波：边界随噪声逐拍跳变
    {
        SafetyEngine eng;
        p.grid_filter_alpha = 1.0;
        eng.set_params(p);
        double lo = 1e18, hi = -1e18;
        for (int i = 0; i < 200; ++i) {
            RealtimeSnapshot rt = rt_basic();
            rt.p_load_kw = 250.0 + ((i % 2) ? 40.0 : -40.0);
            rt.p_pv_kw   = 150.0;    // base = 100±40 → 并网边界成为紧约束
            auto v = eng.evaluate(rt, dev, grid, 0.0, 0.1);
            lo = std::min(lo, v.p_upper);
            hi = std::max(hi, v.p_upper);
        }
        EXPECT(hi - lo > 50.0);     // 边界抖动大
    }
    // 开启滤波：边界被显著平滑
    {
        SafetyEngine eng;
        p.grid_filter_alpha = 0.05;
        eng.set_params(p);
        double lo = 1e18, hi = -1e18;
        for (int i = 0; i < 200; ++i) {
            RealtimeSnapshot rt = rt_basic();
            rt.p_load_kw = 250.0 + ((i % 2) ? 40.0 : -40.0);
            rt.p_pv_kw   = 150.0;
            auto v = eng.evaluate(rt, dev, grid, 0.0, 0.1);
            if (i >= 50) {          // 跳过滤波器预置暂态
                lo = std::min(lo, v.p_upper);
                hi = std::max(hi, v.p_upper);
            }
        }
        EXPECT(hi - lo < 20.0);     // 抖动被压掉（40 → ~2 kW）
    }
}


// =====================================================================
int main() {
    std::cerr << "=========================================\n"
              << " 05/ 周期 5 安全约束引擎 单元测试\n"
              << "=========================================\n";

    test_01_per_constraint_intervals();
    test_02_interval_intersection_and_binding();
    test_03_contradiction_is_derate_not_emergency();
    test_04_ramp_is_slew_not_interval();
    test_05_grid_interval_derivation();
    test_06_grid_measurement_filter();

    std::cerr << "=========================================\n"
              << " PASS=" << g_pass << "  FAIL=" << g_fail << "\n"
              << "=========================================\n";
    return (g_fail == 0) ? 0 : 1;
}
