// =====================================================================
// 04/ 单元测试（assert 风格，单文件可编译）
//
// 基础层（设计 §2.5 接口规范）：
//   T01: StrategyManager 注册 / 启停 / 启停切换
//   T02: L0 覆盖 L1/L2/L3（区间交集）
//   T03: L1 覆盖 L2/L3（区间交集）
//   T04: L3 同层 desired 加权
//   T05: 跨层绝不平均（L3 desired 被压扁到 L0/L1 区间）
//   T06: desired_clip（desired 在区间外）
//   T07: 典型场景（5 策略）：峰谷500/需量400/需求响应300/BMS250/变压器200 → 200
//        （a）上界来自 PCS 额定 200；（b）上界来自 transformer_capacity_kw=200 过载
//   T08: 死区 + 滞环
//   T09: L3 独占模式（RunMode）
//   T10: 优先级分桶（tick 输出顺序）
//
// 周期 9 多策略组合（设计方案 §7）：
//   T11: 峰谷套利 + BMS降功率
//   T12: 峰谷套利 + 变压器过载
//   T13: 需量管理 + 防逆流
//   T14: 光伏平抑 + 防逆流
//   T15: 动态优化 + 需量管理
//   T16: 需求响应 + 峰谷套利 + BMS限制
//   T17: 9 策略全量同时启用（设计方案 §7 场景 7）
//   T18: 设备维度视图 BatteryState / GridState（设计方案 §7 周期 2）
//
// 编译：g++ -std=c++17 -I src tests/test_arbiter.cpp -o build/test_arbiter.exe
// =====================================================================

#include "data_models.h"
#include "strategy_base.h"
#include "strategy_manager.h"
#include "strategy_arbiter.h"
#include "strategies_9.h"

#include <cassert>
#include <cstdio>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>

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
                      << " : " << #a << " (" << va << ") != " << #b      \
                      << " (" << vb << "), |diff|=" << std::fabs(va-vb)  \
                      << " > " << (eps) << std::endl;                     \
        }                                                                 \
    } while (0)

#define EXPECT_EQ(a, b)                                                   \
    do {                                                                  \
        if ((a) == (b)) {                                                 \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << " (" << (a) << ") != " << #b      \
                      << " (" << (b) << ")\n";                            \
        }                                                                 \
    } while (0)

// ---------------------------------------------------------------------
// 测试 fixture
// ---------------------------------------------------------------------
struct Fixture {
    StrategyManager mgr;
    StrategyArbiter arb;
    DeviceLimits    dev;

    Fixture() {
        dev.pcs_rated_chg_kw   = 100.0;
        dev.pcs_rated_dis_kw   = 100.0;
        dev.bms_chg_limit_kw   = 100.0;
        dev.bms_dis_limit_kw   = 100.0;
        dev.bms_chg_forbidden  = false;
        dev.bms_dis_forbidden  = false;
        dev.transformer_capacity_kw = 250.0;
        dev.d_target_kw        = 250.0;
    }

    RealtimeSnapshot rt_with(double p_grid = 50.0,
                             double p_pv   = 0.0,
                             double p_load = 0.0,
                             double soc    = 0.5,
                             TouType tou   = TouType::kFlat) {
        RealtimeSnapshot rt;
        rt.timestamp = 1000.0;
        rt.p_grid_kw = p_grid;
        rt.p_pv_kw   = p_pv;
        rt.p_load_kw = p_load;
        rt.soc       = soc;
        rt.pricing.cur_tou_type = tou;
        rt.demand_window.window_s     = 900.0;
        rt.demand_window.t_elapsed_s  = 450.0;
        rt.demand_window.p_avg_past_kw = p_grid;
        return rt;
    }
};

// 周期 9 配套：注册除 DR 外的 8 个策略（让调用方单独持有 DR 共享指针以便注入事件）
inline void register_all_8_other_strategies(StrategyManager& mgr) {
    mgr.register_strategy(std::make_shared<BmsForbidStrategy>());
    mgr.register_strategy(std::make_shared<BmsDerateStrategy>());
    mgr.register_strategy(std::make_shared<TransformerLimitStrategy>());
    mgr.register_strategy(std::make_shared<DemandMgmtStrategy>());
    mgr.register_strategy(std::make_shared<AntiReverseStrategy>());
    mgr.register_strategy(std::make_shared<PvSmoothingStrategy>());
    mgr.register_strategy(std::make_shared<PeakValleyStrategy>());
    mgr.register_strategy(std::make_shared<ForecastOptStrategy>());
}

// ---------------------------------------------------------------------
// T01: StrategyManager 注册/启停
// ---------------------------------------------------------------------
static void test_01_manager_lifecycle(Fixture& f) {
    std::cerr << "[T01] StrategyManager 注册/启停 ..." << std::endl;

    auto s = std::make_shared<BmsForbidStrategy>();
    f.mgr.register_strategy(s);

    EXPECT(f.mgr.has(strategy_id::kBmsForbid));
    EXPECT_EQ(f.mgr.size(), 1u);  // 用 EXPECT macro，size_t 比较见下

    // start 前 evaluate 不被调用
    f.mgr.stop(strategy_id::kBmsForbid);
    auto results = f.mgr.tick(f.rt_with(), f.dev);
    EXPECT(results.empty());

    // start 后 evaluate 被调用
    f.mgr.start(strategy_id::kBmsForbid);
    results = f.mgr.tick(f.rt_with(), f.dev);
    EXPECT_EQ(results.size(), 1u);
    EXPECT(results[0].strategy_id == strategy_id::kBmsForbid);

    // 重复注册抛异常
    bool caught = false;
    try { f.mgr.register_strategy(std::make_shared<BmsForbidStrategy>()); }
    catch (const std::invalid_argument&) { caught = true; }
    EXPECT(caught);

    // 注销
    f.mgr.unregister_strategy(strategy_id::kBmsForbid);
    EXPECT(!f.mgr.has(strategy_id::kBmsForbid));

    std::cerr << "  PASS  T01\n";
}

// ---------------------------------------------------------------------
// T02: L0 覆盖 L1/L2/L3（区间交集）
// ---------------------------------------------------------------------
static void test_02_l0_overrides(Fixture& f) {
    std::cerr << "[T02] L0 覆盖 L1/L2/L3 ..." << std::endl;
    register_all_9_strategies(f.mgr);
    f.mgr.start_all();

    auto rt = f.rt_with();
    rt.pricing.cur_tou_type = TouType::kPeak;  // 让峰谷套利输出 +80

    // BMS 禁止放电 → L0 收紧上界为 0
    f.dev.bms_dis_forbidden = true;

    auto results = f.mgr.tick(rt, f.dev);
    auto cmd = f.arb.arbitrate(results, rt.timestamp);

    // 区间上界应为 0（被 L0 收紧）
    EXPECT_NEAR(cmd.p_upper, 0.0, 1e-6);
    // 下发指令 ≤ 0
    EXPECT(cmd.p_bat_cmd_kw <= 0.0 + 1e-6);

    std::cerr << "  PASS  T02\n";
}

// ---------------------------------------------------------------------
// T03: L1 覆盖 L2/L3
// ---------------------------------------------------------------------
static void test_03_l1_overrides(Fixture& f) {
    std::cerr << "[T03] L1 覆盖 L2/L3 ..." << std::endl;
    register_all_9_strategies(f.mgr);
    f.mgr.start_all();

    auto rt = f.rt_with();
    rt.pricing.cur_tou_type = TouType::kPeak;

    // 把 BMS 限制调到 30kW → L1 把上界压成 30
    f.dev.bms_dis_limit_kw = 30.0;

    auto results = f.mgr.tick(rt, f.dev);
    auto cmd = f.arb.arbitrate(results, rt.timestamp);

    // 区间上界应 ≤ 30
    EXPECT(cmd.p_upper <= 30.0 + 1e-6);
    // 下发指令 ≤ 30
    EXPECT(cmd.p_bat_cmd_kw <= 30.0 + 1e-6);

    std::cerr << "  PASS  T03\n";
}

// ---------------------------------------------------------------------
// T04: L3 同层 desired 加权
// ---------------------------------------------------------------------
static void test_04_l3_weighted(Fixture& f) {
    std::cerr << "[T04] L3 同层 desired 加权 ..." << std::endl;
    register_all_9_strategies(f.mgr);
    f.mgr.start_all();

    auto rt = f.rt_with();
    rt.pricing.cur_tou_type = TouType::kPeak;

    // 设 L3 权重：peak_valley = 1.0, forecast = 0.0（强制独占）
    f.mgr.set_param(strategy_id::kPeakValley,     "__weight__", 1.0);
    f.mgr.set_param(strategy_id::kForecastOpt,    "__weight__", 0.0);
    f.mgr.set_param(strategy_id::kDemandResponse, "__weight__", 0.0);

    // 同时修改 PeakValley 的 P_discharge
    f.mgr.set_param(strategy_id::kPeakValley, "P_discharge", 60.0);

    auto results = f.mgr.tick(rt, f.dev);
    auto cmd = f.arb.arbitrate(results, rt.timestamp);

    // 因为只 PeakValley 一个 L3 活跃，desired ≈ +60
    EXPECT_NEAR(cmd.p_bat_cmd_kw, 60.0, 0.5);

    std::cerr << "  PASS  T04\n";
}

// ---------------------------------------------------------------------
// T05: 跨层绝不平均（L0/L1/L2 与 L3 之间只取区间交集）
// ---------------------------------------------------------------------
static void test_05_cross_layer_no_avg(Fixture& f) {
    std::cerr << "[T05] 跨层绝不平均 ..." << std::endl;
    register_all_9_strategies(f.mgr);
    f.mgr.start_all();

    auto rt = f.rt_with();
    rt.pricing.cur_tou_type = TouType::kPeak;
    rt.p_load_kw = 0.0;  // 让 ForecastOpt 输出 desired=0 (idle)
    rt.p_grid_kw = 0.0;  // 让 AntiReverse idle

    // 让 PeakValley 输出 +90（区间 [−100, +100]，desired +90）
    f.mgr.set_param(strategy_id::kPeakValley, "P_discharge", 90.0);
    // 独占模式：仅 PeakValley 参与 desired 收敛
    f.mgr.set_param(strategy_id::kPeakValley,     "__weight__", 1.0);
    f.mgr.set_param(strategy_id::kForecastOpt,    "__weight__", 0.0);
    f.mgr.set_param(strategy_id::kDemandResponse, "__weight__", 0.0);

    // L1 把上界收紧到 50
    f.dev.bms_dis_limit_kw = 50.0;

    auto results = f.mgr.tick(rt, f.dev);
    auto cmd = f.arb.arbitrate(results, rt.timestamp);

    // 上界 ≤ 50
    EXPECT(cmd.p_upper <= 50.0 + 1e-6);
    // 下发指令 ≤ 50（不应该是 90 或其他 L3 平均值）
    EXPECT(cmd.p_bat_cmd_kw <= 50.0 + 1e-6);
    EXPECT(cmd.clamped);  // 期望被裁剪

    std::cerr << "  PASS  T05\n";
}

// ---------------------------------------------------------------------
// T06: desired_clip
// ---------------------------------------------------------------------
static void test_06_desired_clip(Fixture& f) {
    std::cerr << "[T06] desired_clip 缩放 ..." << std::endl;
    register_all_9_strategies(f.mgr);
    f.mgr.start_all();

    auto rt = f.rt_with();
    rt.pricing.cur_tou_type = TouType::kPeak;

    f.mgr.set_param(strategy_id::kPeakValley,     "__weight__", 1.0);
    f.mgr.set_param(strategy_id::kForecastOpt,    "__weight__", 0.0);
    f.mgr.set_param(strategy_id::kDemandResponse, "__weight__", 0.0);
    f.mgr.set_param(strategy_id::kPeakValley, "P_discharge", 500.0);

    // 上界压到 200（典型场景的最终值）
    f.dev.bms_dis_limit_kw = 250.0;
    f.dev.transformer_capacity_kw = 200.0;  // 变压器过载（P_bat 可行带下界被抬高）
    // 但 BMS 上限 250 已经覆盖，这里直接构造更窄的上界
    // 用一个更直接的方式：把 L1 的上界收紧到 200
    // 已通过 BmsDerate 把上界设为 min(250, pcs_dis)=100.0
    // 这里改成 pcs 80
    f.dev.pcs_rated_dis_kw = 80.0;

    auto results = f.mgr.tick(rt, f.dev);
    auto cmd = f.arb.arbitrate(results, rt.timestamp);

    EXPECT(cmd.clamped);
    EXPECT(cmd.reason.find("desired_clip") != std::string::npos);
    EXPECT_NEAR(cmd.p_bat_cmd_kw, 80.0, 1e-6);  // desired +500 → 压到 +80

    std::cerr << "  PASS  T06\n";
}

// ---------------------------------------------------------------------
// T07: 典型场景 — 峰谷500/需量400/需求响应300/BMS250/变压器200 → 200
//
// 分两个子场景，分别覆盖"上界 200 从哪来"的两种路径（design §7 经典例）：
//   (a) PCS 额定上界：pcs_rated_dis_kw = 200 → 变压器不动作，上界来自 PCS
//   (b) 变压器过载上界：transformer_capacity_kw = 200 且变压器确实过载
//                       → 上界由 TransformerLimitStrategy 收紧到 200
// 只测 (a) 会漏掉"变压器这道防线到底有没有接进仲裁"。
// ---------------------------------------------------------------------
static void test_07_typical_scenario(Fixture& f) {
    std::cerr << "[T07] 典型多策略场景 ..." << std::endl;
    register_all_9_strategies(f.mgr);
    f.mgr.start_all();

    // =================================================================
    // (a) 上界 200 来自 PCS 额定（变压器不动作）
    // =================================================================
    {
        auto rt = f.rt_with();
        rt.pricing.cur_tou_type = TouType::kPeak;

        // PeakValley +500
        f.mgr.set_param(strategy_id::kPeakValley, "P_discharge", 500.0);
        // 让所有 L3 都活跃，验证加权平均 → 然后被压扁到 200
        f.mgr.set_param(strategy_id::kPeakValley,     "__weight__", 1.0);
        f.mgr.set_param(strategy_id::kForecastOpt,    "__weight__", 1.0);
        f.mgr.set_param(strategy_id::kDemandResponse, "__weight__", 1.0);

        // DemandResponse 期望 +300（直接通过 set_event 不便，改用 rt 数据驱动）
        // 这里简化：让 ForecastOpt 输出 ~+450（load=650, target=250）
        rt.p_load_kw = 650.0;

        // BMS 上限 250
        f.dev.bms_dis_limit_kw = 250.0;
        // PCS 上限 = 200
        f.dev.pcs_rated_dis_kw = 200.0;

        auto results = f.mgr.tick(rt, f.dev);
        auto cmd = f.arb.arbitrate(results, rt.timestamp);

        // 期望：上界 200（min of BMS 250, PCS 200），下发 ≤ 200
        EXPECT_NEAR(cmd.p_upper, 200.0, 1e-6);
        EXPECT(cmd.p_bat_cmd_kw <= 200.0 + 1e-6);
        EXPECT(cmd.p_bat_cmd_kw >  0.0);  // 还在放电方向
        EXPECT(cmd.clamped);
    }

    // =================================================================
    // (b) 上界 200 来自变压器过载（design §7 经典例的那条路径）
    //
    // 让 PCS/BMS 都放到 200 以上，逼出"只有变压器能压到 200"的现场：
    //   容量 200 kVA；负载估算 tr_load = |P_grid| + 0.1·P_load（与 05/ 同口径）
    //   P_grid = 105, P_load = 950 → tr_load = 105 + 95 = 200 → ratio = 1.00
    //   判据 0.95 < ratio ≤ 1.10 → 轻度过载，reason = "tr_overload"
    //   可行带 half = 0.95×200 − 0.1×950 = 95
    //   base = P_grid + P_bat = 105  →  [lo, hi] = [10, 200]
    //   于是 p_upper = min(PCS 300, 200) = 200，p_lower = max(−100, 10) = 10
    // =================================================================
    {
        auto rt = f.rt_with(/*p_grid=*/105.0, /*p_pv=*/0.0, /*p_load=*/950.0);
        rt.p_bat_actual_kw = 0.0;
        rt.pricing.cur_tou_type = TouType::kPeak;
        rt.demand_window.p_avg_past_kw = 105.0;

        f.mgr.set_param(strategy_id::kPeakValley, "P_discharge", 500.0);
        f.mgr.set_param(strategy_id::kPeakValley,     "__weight__", 1.0);
        f.mgr.set_param(strategy_id::kForecastOpt,    "__weight__", 1.0);
        f.mgr.set_param(strategy_id::kDemandResponse, "__weight__", 1.0);

        // PCS / BMS 都开到 250 以上 —— 上界不再可能由它们给出
        f.dev.transformer_capacity_kw = 200.0;
        f.dev.pcs_rated_dis_kw        = 300.0;
        f.dev.bms_dis_limit_kw        = 260.0;

        auto results = f.mgr.tick(rt, f.dev);

        // 定位变压器策略自己的输出：证明"是它把上界压到 200 的"
        const StrategyResult* tr = nullptr;
        for (const StrategyResult& r : results) {
            if (r.strategy_id == strategy_id::kTransformerLim) tr = &r;
        }
        EXPECT(tr != nullptr);
        if (tr != nullptr) {
            EXPECT_EQ(tr->reason, std::string("tr_overload"));
            EXPECT_NEAR(tr->p_upper, 200.0, 1e-6);
            EXPECT_NEAR(tr->p_lower,  10.0, 1e-6);
            EXPECT(tr->active);
        }

        auto cmd = f.arb.arbitrate(results, rt.timestamp);
        EXPECT_NEAR(cmd.p_upper, 200.0, 1e-6);          // 变压器给出的上界
        EXPECT_NEAR(cmd.p_lower,  10.0, 1e-6);          // 变压器给出的下界
        EXPECT(cmd.p_bat_cmd_kw <= 200.0 + 1e-6);
        EXPECT(cmd.clamped);                            // desired 500 被压到 200
    }

    std::cerr << "  PASS  T07\n";
}

// ---------------------------------------------------------------------
// T08: 死区 + 滞环
// ---------------------------------------------------------------------
static void test_08_deadband_hysteresis(Fixture& f) {
    std::cerr << "[T08] 死区 + 滞环 ..." << std::endl;
    f.arb.set_deadband(2.0);
    f.arb.set_switch_delay(2);

    // 构造一个 desired < 死区的结果
    StrategyResult r;
    r.strategy_id = "X";
    r.priority    = Priority::kL3_GlobalEcon;
    r.active      = true;
    r.weight      = 1.0;
    r.p_lower     = -10;
    r.p_upper     = +10;
    r.p_desired   = 0.5;  // 小于死区 2.0

    std::vector<StrategyResult> v = { r };
    auto cmd = f.arb.arbitrate(v, 1.0);
    EXPECT_NEAR(cmd.p_bat_cmd_kw, 0.0, 1e-6);

    // 改为 desired = 5（> 死区）
    r.p_desired = 5.0;
    v = { r };
    cmd = f.arb.arbitrate(v, 2.0);
    EXPECT_NEAR(cmd.p_bat_cmd_kw, 5.0, 1e-6);

    // 再切回 0.5，滞环 1 拍 keep，再 1 拍切
    r.p_desired = 0.5;
    v = { r };
    cmd = f.arb.arbitrate(v, 3.0);  // 第一拍：hysteresis_keep
    cmd = f.arb.arbitrate(v, 4.0);  // 第二拍：deadband → 0
    EXPECT_NEAR(cmd.p_bat_cmd_kw, 0.0, 1e-6);

    std::cerr << "  PASS  T08\n";
}

// ---------------------------------------------------------------------
// T09: L3 独占模式（RunMode）
// ---------------------------------------------------------------------
static void test_09_run_mode_exclusive(Fixture& f) {
    std::cerr << "[T09] L3 独占模式 ..." << std::endl;
    register_all_9_strategies(f.mgr);
    f.mgr.start_all();

    // 设 MPC 模式
    f.mgr.set_run_mode(RunMode::kMPC);
    EXPECT_NEAR(f.mgr.get_status(strategy_id::kForecastOpt).last_p_lower,
                0.0, 1e-6); // 占位判断，状态是空的因为 tick 没跑
    // 直接判断 weight
    // （这里只能通过再次 tick 检查结果来验证）

    auto rt = f.rt_with();
    rt.pricing.cur_tou_type = TouType::kPeak;
    auto results = f.mgr.tick(rt, f.dev);
    auto cmd = f.arb.arbitrate(results, rt.timestamp);

    // 在 MPC 模式下，只有 ForecastOpt 参与 desired 加权
    // 其他 L3 策略 weight=0
    for (const auto& r : results) {
        if (r.priority != Priority::kL3_GlobalEcon) continue;
        if (r.strategy_id == strategy_id::kForecastOpt) {
            EXPECT_NEAR(r.weight, 1.0, 1e-6);
        } else {
            EXPECT_NEAR(r.weight, 0.0, 1e-6);
        }
    }

    std::cerr << "  PASS  T09\n";
}

// ---------------------------------------------------------------------
// T10: tick 输出顺序按优先级
// ---------------------------------------------------------------------
static void test_10_tick_order(Fixture& f) {
    std::cerr << "[T10] tick 输出按优先级 ..." << std::endl;
    register_all_9_strategies(f.mgr);
    f.mgr.start_all();

    auto rt = f.rt_with();
    auto results = f.mgr.tick(rt, f.dev);

    // 验证 L0 在前，L3 在后
    for (size_t i = 1; i < results.size(); ++i) {
        EXPECT(static_cast<int>(results[i].priority)
             >= static_cast<int>(results[i-1].priority));
    }

    std::cerr << "  PASS  T10\n";
}

// ---------------------------------------------------------------------
// T11: 峰谷套利 + BMS降功率（设计方案 §7 场景 1）
// 期望：PeakValley 想 +300，被 BMS 限制 80 → 最终 ≤ 80, clamped=true
// ---------------------------------------------------------------------
static void test_11_peakvalley_plus_bms_derate(Fixture& f) {
    std::cerr << "[T11] 峰谷套利 + BMS降功率 ..." << std::endl;
    register_all_9_strategies(f.mgr);
    f.mgr.start_all();

    auto rt = f.rt_with();
    rt.pricing.cur_tou_type = TouType::kPeak;  // 触发 PeakValley 强烈放电

    // PeakValley 想要 +300 kW 放电
    f.mgr.set_param(strategy_id::kPeakValley, "P_discharge", 300.0);
    // BMS 把放电上限压到 80 kW（温控/电芯限功率）
    f.dev.bms_dis_limit_kw = 80.0;
    // 隔离变压器干扰：容量调大
    f.dev.transformer_capacity_kw = 1000.0;

    auto results = f.mgr.tick(rt, f.dev);
    auto cmd = f.arb.arbitrate(results, rt.timestamp);

    // 区间上界 ≤ 80（被 BMS L1 收紧）
    EXPECT(cmd.p_upper <= 80.0 + 1e-6);
    // 下发 ≤ 80（desired 被 clipped）
    EXPECT(cmd.p_bat_cmd_kw <= 80.0 + 1e-6);
    // 必须 clipped（PeakValley 300 被压扁）
    EXPECT(cmd.clamped);
    EXPECT(cmd.reason.find("desired_clip") != std::string::npos);

    std::cerr << "  PASS  T11\n";
}

// ---------------------------------------------------------------------
// T12: 峰谷套利 + 变压器过载（设计方案 §7 场景 2）
// 期望：PeakValley 想 +200，变压器过载把 P_bat 可行带抬到 [15, 145]，
//       与 PCS [−100, 100] 求交 → [15, 100]，最终指令落在该带内（正向放电）
// ---------------------------------------------------------------------
static void test_12_peakvalley_plus_transformer(Fixture& f) {
    std::cerr << "[T12] 峰谷套利 + 变压器过载 ..." << std::endl;
    register_all_9_strategies(f.mgr);
    f.mgr.start_all();

    auto rt = f.rt_with();
    rt.pricing.cur_tou_type = TouType::kPeak;

    f.mgr.set_param(strategy_id::kPeakValley, "P_discharge", 200.0);

    // 变压器过载（**进口方向**）：capacity=100，|P_grid|+0.1·P_load = 80+30 = 110
    f.dev.transformer_capacity_kw = 100.0;
    rt.p_grid_kw = 80.0;
    rt.p_load_kw = 300.0;

    auto results = f.mgr.tick(rt, f.dev);
    auto cmd = f.arb.arbitrate(results, rt.timestamp);

    // 期望可行带 [15, 100]：
    //   base = P_grid + P_bat = 80 + 0 = 80
    //   half = 0.95·cap − 0.1·P_load = 95 − 30 = 65
    //   → P_bat ∈ [15, 145]，与 PCS [−100, 100] 求交 → [15, 100]
    //
    // **语义修正（周期 9）**：原实现把过载一律处理为"禁放"（p_upper = 0）。
    //   但变压器负载看的是关口功率的**绝对值** —— 本例是进口方向过载
    //   （P_grid = +80），放电会降低 P_grid，恰恰是缓解手段；禁放反而让过载
    //   持续。修正后约束表达为上述**区间**：下界被抬到 +15（必须放电 ≥15 kW
    //   才能把负载率压回阈值内），上界保持设备能力。权威实现见
    //   05/src/safety_engine.h::check_transformer。
    EXPECT_NEAR(cmd.p_lower, 15.0, 1e-6);
    EXPECT_NEAR(cmd.p_upper, 100.0, 1e-6);
    EXPECT(cmd.p_bat_cmd_kw >= 15.0 - 1e-6);   // 允许（且要求）放电
    EXPECT(cmd.p_bat_cmd_kw <= 100.0 + 1e-6);

    std::cerr << "  PASS  T12\n";
}

// ---------------------------------------------------------------------
// T13: 需量管理 + 防逆流（设计方案 §7 场景 3）
// 期望：变压器隔离；高负荷时段需量想放电，PV 已覆盖部分。
//       防逆流没动作（grid 仍正向），最终指令服从需求管理结果（正值）
//       与 PV 平抑（负值）的 L2 区间交集和 desired（L3 加权）配合。
// ---------------------------------------------------------------------
static void test_13_demand_plus_anti_reverse(Fixture& f) {
    std::cerr << "[T13] 需量管理 + 防逆流 ..." << std::endl;
    register_all_9_strategies(f.mgr);
    f.mgr.start_all();

    // 隔离变压器干扰 + 把 BMS 上限提升（不卡住）
    f.dev.transformer_capacity_kw = 1000.0;

    auto rt = f.rt_with();
    rt.p_load_kw = 450.0;
    rt.p_pv_kw   = 200.0;
    rt.p_grid_kw = 250.0;           // 网购（不逆流 → 防逆流 idle）
    rt.soc       = 0.6;
    rt.pricing.cur_tou_type = TouType::kPeak;
    rt.demand_window.t_elapsed_s = 600.0;
    rt.demand_window.p_avg_past_kw = 300.0;

    f.dev.d_target_kw = 350.0;

    auto results = f.mgr.tick(rt, f.dev);
    auto cmd = f.arb.arbitrate(results, rt.timestamp);

    // 需量管理期望放电，但 PV 平抑期望充电 → L3 同层加权后取正值（PeakValley/FRC 都是正）
    // 验证：区间合法（L0/L1/L2 收敛成功）；cmd 在 PCS 范围内
    EXPECT(cmd.p_lower <= cmd.p_upper + 1e-6);
    EXPECT(cmd.p_bat_cmd_kw >= f.dev.pcs_rated_chg_kw * -1.0 - 1e-6);
    EXPECT(cmd.p_bat_cmd_kw <= f.dev.pcs_rated_dis_kw + 1e-6);

    std::cerr << "  PASS  T13\n";
}

// ---------------------------------------------------------------------
// T14: 光伏平抑 + 防逆流（设计方案 §7 场景 4）
// 期望：变压器隔离；PV 远超负荷、关口已倒送，防逆流应推动充电吸收（p_desire < 0），
//       但 PV 平抑也能给出合适充电信号。最终区间下界被收紧（不能更负的过多以避免继续倒送）。
// ---------------------------------------------------------------------
static void test_14_pv_smoothing_plus_anti_reverse(Fixture& f) {
    std::cerr << "[T14] 光伏平抑 + 防逆流 ..." << std::endl;
    register_all_9_strategies(f.mgr);
    f.mgr.start_all();

    // 隔离变压器干扰
    f.dev.transformer_capacity_kw = 1000.0;

    auto rt = f.rt_with();
    rt.p_load_kw = 100.0;
    rt.p_pv_kw   = 400.0;
    rt.p_grid_kw = -300.0;          // 已倒送
    rt.soc       = 0.7;

    auto results = f.mgr.tick(rt, f.dev);
    auto cmd = f.arb.arbitrate(results, rt.timestamp);

    // 验证：区间合法；下界不能小于 pcs 充电上限太多（不应允许更大充电超出 PCS）
    EXPECT(cmd.p_lower <= cmd.p_upper + 1e-6);
    EXPECT(cmd.p_lower >= -f.dev.pcs_rated_chg_kw - 1e-6);
    EXPECT(cmd.p_upper <= f.dev.pcs_rated_dis_kw + 1e-6);

    std::cerr << "  PASS  T14\n";
}

// ---------------------------------------------------------------------
// T15: 动态优化 + 需量管理（设计方案 §7 场景 5）
// 期望：变压器隔离；L3 动态优化（PeakValley 占优）和 L2 需量同向，
//       最终指令在 PCS 区间内、遵循 L3 加权策略。
// ---------------------------------------------------------------------
static void test_15_forecast_plus_demand_mgmt(Fixture& f) {
    std::cerr << "[T15] 动态优化 + 需量管理 ..." << std::endl;
    register_all_9_strategies(f.mgr);
    f.mgr.start_all();

    // 隔离变压器干扰
    f.dev.transformer_capacity_kw = 1000.0;

    auto rt = f.rt_with();
    rt.p_load_kw = 500.0;
    rt.p_grid_kw = 500.0;
    rt.soc       = 0.6;
    rt.pricing.cur_tou_type = TouType::kPeak;
    rt.demand_window.t_elapsed_s = 600.0;
    rt.demand_window.p_avg_past_kw = 400.0;

    f.dev.d_target_kw = 350.0;

    auto results = f.mgr.tick(rt, f.dev);
    auto cmd = f.arb.arbitrate(results, rt.timestamp);

    // 区间合法且在 PCS 范围内
    EXPECT(cmd.p_lower <= cmd.p_upper + 1e-6);
    EXPECT(cmd.p_lower >= -f.dev.pcs_rated_chg_kw - 1e-6);
    EXPECT(cmd.p_upper <= f.dev.pcs_rated_dis_kw + 1e-6);
    // 期望正方向（高峰需量管理 + PeakValley 期望放电）
    EXPECT(cmd.p_bat_cmd_kw > 0.0);

    std::cerr << "  PASS  T15\n";
}

// ---------------------------------------------------------------------
// T16: 需求响应 + 峰谷套利 + BMS 限制（设计方案 §7 场景 6）
// 期望：DR + PeakValley 都想 +200，但 BMS 限 80 → 最终 ≤ 80
// ---------------------------------------------------------------------
static void test_16_dr_plus_peakvalley_plus_bms(Fixture& f) {
    std::cerr << "[T16] 需求响应 + 峰谷套利 + BMS限制 ..." << std::endl;

    // DR 策略需要本地持有引用，以便注入事件
    auto dr_strat = std::make_shared<DemandResponseStrategy>();
    {
        DemandResponseStrategy::DrEvent ev;
        ev.active    = true;
        ev.target_kw = 200.0;
        ev.end_ts    = 1e9;  // 长有效
        dr_strat->set_event(ev);
    }
    f.mgr.register_strategy(dr_strat);
    register_all_8_other_strategies(f.mgr);  // 其余 8 个
    f.mgr.start_all();

    auto rt = f.rt_with();
    rt.p_load_kw = 500.0;
    rt.pricing.cur_tou_type = TouType::kSharp;  // 尖峰时段 → DR 强烈放电
    rt.soc = 0.7;

    // PeakValley 想要 +200
    f.mgr.set_param(strategy_id::kPeakValley, "P_discharge", 200.0);

    // BMS 限制放电 80 kW
    f.dev.bms_dis_limit_kw = 80.0;
    // 隔离变压器干扰
    f.dev.transformer_capacity_kw = 1000.0;

    auto results = f.mgr.tick(rt, f.dev);
    auto cmd = f.arb.arbitrate(results, rt.timestamp);

    EXPECT(cmd.p_upper <= 80.0 + 1e-6);
    EXPECT(cmd.p_bat_cmd_kw <= 80.0 + 1e-6);
    EXPECT(cmd.clamped);

    std::cerr << "  PASS  T16\n";
}

// ---------------------------------------------------------------------
// T17: 9 策略全量同时启用（设计方案 §7 场景 7）
// 期望：所有 9 个策略同时发声，最终指令在 L0/L1 区间内，
//       由 L3 同层加权后 desired_clip 触发，无指令冲突、互覆盖、频繁切换
// ---------------------------------------------------------------------
static void test_17_all_nine_simultaneously(Fixture& f) {
    std::cerr << "[T17] 9 策略全量同时启用 ..." << std::endl;

    auto dr_strat = std::make_shared<DemandResponseStrategy>();
    {
        DemandResponseStrategy::DrEvent ev;
        ev.active    = true;
        ev.target_kw = 250.0;
        ev.end_ts    = 1e9;
        dr_strat->set_event(ev);
    }
    f.mgr.register_strategy(dr_strat);
    register_all_8_other_strategies(f.mgr);

    f.mgr.start_all();

    auto rt = f.rt_with();
    rt.p_load_kw = 600.0;
    rt.p_pv_kw   = 200.0;
    rt.p_grid_kw = 400.0;
    rt.soc       = 0.6;
    rt.pricing.cur_tou_type = TouType::kSharp;
    rt.demand_window.t_elapsed_s  = 800.0;
    rt.demand_window.p_avg_past_kw = 380.0;

    // 配置：让多个 L3 策略都想 +大功率放电，被 L1 安全约束压住
    f.mgr.set_param(strategy_id::kPeakValley, "P_discharge", 600.0);
    f.mgr.set_param(strategy_id::kPeakValley, "__weight__", 1.0);
    f.mgr.set_param(strategy_id::kForecastOpt, "__weight__", 1.0);
    f.mgr.set_param(strategy_id::kDemandResponse, "__weight__", 1.0);

    // L1 安全：PCS 上限 200
    f.dev.pcs_rated_dis_kw = 200.0;
    f.dev.transformer_capacity_kw = 1000.0;  // 隔离变压器

    auto results = f.mgr.tick(rt, f.dev);
    auto cmd = f.arb.arbitrate(results, rt.timestamp);

    // 9 个策略都应有结果
    EXPECT_EQ(results.size(), 9u);

    // 区间必须 ≤ 200（被 L1 PCS 限制）
    EXPECT(cmd.p_upper <= 200.0 + 1e-6);
    // 仍应在放电方向
    EXPECT(cmd.p_bat_cmd_kw > 0.0);
    EXPECT(cmd.p_bat_cmd_kw <= 200.0 + 1e-6);
    // 必须 clipped
    EXPECT(cmd.clamped);
    EXPECT(cmd.reason.find("desired_clip") != std::string::npos);

    // 二次确认无指令冲突：区间合法（lower ≤ upper）
    EXPECT(cmd.p_lower <= cmd.p_upper + 1e-6);

    std::cerr << "  PASS  T17\n";
}

// ---------------------------------------------------------------------
// T18: 设备维度视图 BatteryState / GridState（设计方案 §7 周期 2）
//
// 这两个视图是"同一份真相的另一个视角"，所以测试的重点不是算得对不对，
// 而是**映射是否忠实 + 不产生第二份真相**：
//   1. 每个视图字段都必须等于其来源字段（逐一核对，不是抽样）；
//   2. 修改视图不得影响源结构（只读视角）。
// ---------------------------------------------------------------------
static void test_18_device_views(Fixture& f) {
    std::cerr << "[T18] BatteryState / GridState 视图 ..." << std::endl;

    auto rt = f.rt_with(/*p_grid=*/123.0, /*p_pv=*/45.0, /*p_load=*/300.0, /*soc=*/0.42);
    rt.temperature_c    = 31.5;
    rt.soh              = 0.97;
    rt.p_bat_actual_kw  = -18.0;
    rt.meters_alive["BMS"]   = true;
    rt.meters_alive["METER"] = true;

    f.dev.pcs_rated_chg_kw        = 110.0;
    f.dev.pcs_rated_dis_kw        = 120.0;
    f.dev.bms_chg_limit_kw        = 80.0;
    f.dev.bms_dis_limit_kw        = 90.0;
    f.dev.bms_chg_forbidden       = true;
    f.dev.bms_dis_forbidden       = false;
    f.dev.transformer_capacity_kw = 315.0;
    f.dev.d_target_kw             = 275.0;

    const BatteryState bs = battery_state_of(rt, f.dev);
    EXPECT_NEAR(bs.soc,           rt.soc,               1e-12);
    EXPECT_NEAR(bs.soh,           rt.soh,               1e-12);
    EXPECT_NEAR(bs.temperature_c, rt.temperature_c,     1e-12);
    EXPECT_NEAR(bs.p_bat_kw,      rt.p_bat_actual_kw,   1e-12);
    EXPECT_NEAR(bs.rated_chg_kw,  f.dev.pcs_rated_chg_kw, 1e-12);
    EXPECT_NEAR(bs.rated_dis_kw,  f.dev.pcs_rated_dis_kw, 1e-12);
    EXPECT_NEAR(bs.chg_limit_kw,  f.dev.bms_chg_limit_kw, 1e-12);
    EXPECT_NEAR(bs.dis_limit_kw,  f.dev.bms_dis_limit_kw, 1e-12);
    EXPECT_EQ(bs.chg_forbidden, f.dev.bms_chg_forbidden);
    EXPECT_EQ(bs.dis_forbidden, f.dev.bms_dis_forbidden);
    EXPECT(bs.comm_ok);

    const GridState gs = grid_state_of(rt, f.dev);
    EXPECT_NEAR(gs.p_grid_kw,               rt.p_grid_kw,    1e-12);
    EXPECT_NEAR(gs.p_load_kw,               rt.p_load_kw,    1e-12);
    EXPECT_NEAR(gs.p_pv_kw,                 rt.p_pv_kw,      1e-12);
    EXPECT_NEAR(gs.transformer_capacity_kw, f.dev.transformer_capacity_kw, 1e-12);
    EXPECT_NEAR(gs.d_target_kw,             f.dev.d_target_kw, 1e-12);
    EXPECT(gs.meter_ok);

    // 只读视角：改视图不能回写源结构（防止"第二份真相"）
    BatteryState bs2 = bs;
    bs2.soc = 0.99;
    EXPECT_NEAR(bs2.soc, 0.99, 1e-12);   // 副本确实改了
    EXPECT_NEAR(rt.soc,  0.42, 1e-12);   // 源结构没动

    // BMS 通信丢失要能被视图表达出来
    rt.meters_alive["BMS"] = false;
    EXPECT(!battery_state_of(rt, f.dev).comm_ok);

    std::cerr << "  PASS  T18\n";
}

// ---------------------------------------------------------------------
// 主入口
// ---------------------------------------------------------------------
int main() {
    std::cerr << "=========================================\n"
              << " 04/ Strategy Manager & Arbiter Tests\n"
              << "=========================================\n";

    {
        Fixture f;
        test_01_manager_lifecycle(f);
    }
    {
        Fixture f;
        test_02_l0_overrides(f);
    }
    {
        Fixture f;
        test_03_l1_overrides(f);
    }
    {
        Fixture f;
        test_04_l3_weighted(f);
    }
    {
        Fixture f;
        test_05_cross_layer_no_avg(f);
    }
    {
        Fixture f;
        test_06_desired_clip(f);
    }
    {
        Fixture f;
        test_07_typical_scenario(f);
    }
    {
        Fixture f;
        test_08_deadband_hysteresis(f);
    }
    {
        Fixture f;
        test_09_run_mode_exclusive(f);
    }
    {
        Fixture f;
        test_10_tick_order(f);
    }
    {
        Fixture f;
        test_11_peakvalley_plus_bms_derate(f);
    }
    {
        Fixture f;
        test_12_peakvalley_plus_transformer(f);
    }
    {
        Fixture f;
        test_13_demand_plus_anti_reverse(f);
    }
    {
        Fixture f;
        test_14_pv_smoothing_plus_anti_reverse(f);
    }
    {
        Fixture f;
        test_15_forecast_plus_demand_mgmt(f);
    }
    {
        Fixture f;
        test_16_dr_plus_peakvalley_plus_bms(f);
    }
    {
        Fixture f;
        test_17_all_nine_simultaneously(f);
    }
    {
        Fixture f;
        test_18_device_views(f);
    }

    std::cerr << "=========================================\n"
              << " PASS=" << g_pass << "  FAIL=" << g_fail << "\n"
              << "=========================================\n";
    return (g_fail == 0) ? 0 : 1;
}