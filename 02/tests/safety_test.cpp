// =====================================================================
// Unit tests for safety_constraint_manager.cpp (A 组 安全约束模块).
//
// The .cpp is a single-file demo, so we use the #define-main hack to
// include it and drive its classes directly. Covers all three processors
// + the constraint merge path.
//
// Run via: scripts\build_test.bat   (produces ..\build\safety_test.exe)
// =====================================================================

#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>

#define main safety_constraint_manager_main
#include "../src/safety_constraint_manager.cpp"
#undef main

namespace {

constexpr float kEps = 1e-4f;
int failures = 0;

#define EXPECT(cond) do {                                                  \
    if (!(cond)) {                                                         \
        std::fprintf(stderr, "[FAIL] %s:%d  %s\n",                         \
                     __FILE__, __LINE__, #cond);                           \
        ++failures;                                                        \
    }                                                                      \
} while (0)

#define EXPECT_NEAR(a, b, eps) do {                                        \
    float _a = (a), _b = (b);                                              \
    if (std::fabs(_a - _b) > (eps)) {                                      \
        std::fprintf(stderr,                                               \
            "[FAIL] %s:%d  EXPECT_NEAR(%f, %f, %f)\n",                     \
            __FILE__, __LINE__, _a, _b, float(eps));                       \
        ++failures;                                                        \
    }                                                                      \
} while (0)

// ----------------- 策略一：BMS 禁止充放 -----------------

// 注意：BmsEnableProcessor 是"两次连续相同才更新"的去抖逻辑
// 所以翻转使能位后需要立刻再调用一次 Process 才能生效

void test_strategy1_normal() {
    BmsEnableProcessor p;
    BmsStatus bms;
    bms.charge_enable = true;
    bms.discharge_enable = true;
    bms.last_update_ms = 0;

    p.Process(bms, 100);            // prev 已是 true, 当前 true, 不阻塞
    p.Process(bms, 200);            // 同上, 稳定
    EXPECT(p.charge_blocked() == false);
    EXPECT(p.discharge_blocked() == false);

    // 翻转需要两次确认
    bms.charge_enable = false;
    p.Process(bms, 300);            // prev=true, 当前=false, 不等 -> 不更新
    EXPECT(p.charge_blocked() == false);
    bms.charge_enable = false;
    p.Process(bms, 400);            // prev=false, 当前=false, 相等 -> charge_blocked=true
    EXPECT(p.charge_blocked() == true);
    EXPECT(p.discharge_blocked() == false);
}

void test_strategy1_comm_loss() {
    BmsEnableProcessor p;
    BmsStatus bms;
    bms.charge_enable = true;
    bms.discharge_enable = true;
    bms.last_update_ms = 0;

    p.Process(bms, 100);
    p.Process(bms, 200);

    // BMS 时间戳停在 200, 当前 2400 -> age > 1500ms, 强制禁止
    p.Process(bms, 2400);
    EXPECT(p.charge_blocked() == true);
    EXPECT(p.discharge_blocked() == true);
}

// ----------------- 策略二：BMS 请求降功率 -----------------

void test_strategy2_lowpass_init() {
    BmsPowerLimitProcessor p;
    BmsStatus bms;
    bms.last_update_ms = 0;
    bms.max_charge_power = 100.0f;
    bms.max_discharge_power = 100.0f;

    p.Process(bms, 100);
    EXPECT_NEAR(p.charge_limit(), 100.0f, kEps);
}

void test_strategy2_lowpass_step() {
    BmsPowerLimitProcessor p;
    BmsStatus bms;
    bms.last_update_ms = 0;
    bms.max_charge_power = 100.0f;
    bms.max_discharge_power = 100.0f;

    p.Process(bms, 100);  // 初始化: y = 100

    // 阶跃到 50kW, 一阶低通 alpha=0.2
    // y = 0.2 * 50 + 0.8 * 100 = 90
    bms.max_charge_power = 50.0f;
    p.Process(bms, 200);
    EXPECT_NEAR(p.charge_limit(), 90.0f, kEps);

    // 再一次 50kW
    bms.max_charge_power = 50.0f;
    p.Process(bms, 300);
    // y = 0.2*50 + 0.8*90 = 82
    EXPECT_NEAR(p.charge_limit(), 82.0f, kEps);
}

void test_strategy2_comm_loss() {
    BmsPowerLimitProcessor p;
    BmsStatus bms;
    bms.max_charge_power = 100.0f;
    bms.max_discharge_power = 100.0f;
    bms.last_update_ms = 0;

    p.Process(bms, 100);
    p.Process(bms, 200);

    // 时间戳停在 200, now=2400 -> age > 1500, 上限置 0
    p.Process(bms, 2400);
    EXPECT_NEAR(p.charge_limit(), 0.0f, kEps);
    EXPECT_NEAR(p.discharge_limit(), 0.0f, kEps);
}

// ----------------- 策略三：变压器过载 -----------------

void test_strategy3_threshold() {
    TransformerOverloadProcessor p;
    Config cfg;  // 默认值: 250kVA × 0.9 = 225kW, threshold=0.95, hysteresis=0.02
    p.Init(cfg);

    // 96% (216kW) 严格大于阈值 -> 启动限功率
    p.Process(216.0f);
    EXPECT(p.limiting() == true);

    // 仍在滞回状态（97%）
    p.Process(218.25f);
    EXPECT(p.limiting() == true);

    // 负载回落至 92% (< 95% - 2% = 93%) -> 解除
    p.Process(207.0f);
    EXPECT(p.limiting() == false);
}

void test_strategy3_extreme_overload() {
    TransformerOverloadProcessor p;
    Config cfg;
    p.Init(cfg);

    // 248kW -> 110.2% 负载率 -> 极端过载
    p.Process(248.0f);
    EXPECT(p.limiting() == true);
    EXPECT(p.extreme_overload() == true);
}

// ----------------- 约束合并：完整 Manager 流程 -----------------

// 自定义最小 provider, 让测试可控
struct TestBms : public IBmsDataProvider {
    BmsStatus data;
    int64_t last_now_ms_ = 0;  // 用于模拟 BMS 数据时间戳与 now 同步

    BmsStatus GetData() const override {
        BmsStatus out = data;
        // 让 last_update_ms 紧跟调用时刻, 避免误触发通信丢失判断
        out.last_update_ms = last_now_ms_;
        return out;
    }
};
struct TestMeter : public IGridMeterProvider {
    float load_kw = 0.0f;
    float GetTransformerLoadKw() const override { return load_kw; }
};

// 包装: 把 now_ms 同步到 TestBms 内部时钟
static SafetyConstraints Step(SafetyConstraintManager& mgr,
                              TestBms& bms, [[maybe_unused]] TestMeter& meter,
                              int64_t now_ms) {
    bms.last_now_ms_ = now_ms;
    return mgr.RunOnce(now_ms);
}

void test_merge_basic() {
    Config cfg;
    TestBms bms;
    TestMeter meter;
    SafetyConstraintManager mgr;
    mgr.Init(cfg, &bms, &meter);

    bms.data.charge_enable = true;
    bms.data.discharge_enable = true;
    bms.data.max_charge_power = 80.0f;
    bms.data.max_discharge_power = 80.0f;

    meter.load_kw = 100.0f;

    // 多次迭代让一阶低通从初始值 100kW 收敛到 80kW
    SafetyConstraints sc;
    for (int i = 0; i < 30; ++i) {
        sc = Step(mgr, bms, meter, 100 + i * 100);
    }
    EXPECT(sc.charge_blocked == false);
    EXPECT(sc.discharge_blocked == false);
    EXPECT_NEAR(sc.charge_power_limit_kw, 80.0f, 0.5f);
    EXPECT_NEAR(sc.discharge_power_limit_kw, 80.0f, 0.5f);
}

void test_merge_transformer_overload() {
    Config cfg;
    TestBms bms;
    TestMeter meter;
    SafetyConstraintManager mgr;
    mgr.Init(cfg, &bms, &meter);

    bms.data.charge_enable = true;
    bms.data.discharge_enable = true;
    bms.data.max_charge_power = 80.0f;
    bms.data.max_discharge_power = 80.0f;

    meter.load_kw = 220.0f;  // 97.8% 负载率, 触发策略三

    // 先多次迭代让一阶低通收敛到 80kW, 同时让过载钳位生效
    SafetyConstraints sc;
    for (int i = 0; i < 30; ++i) {
        sc = Step(mgr, bms, meter, 100 + i * 100);
    }
    EXPECT(sc.discharge_blocked == false);             // 还不到极端过载
    EXPECT_NEAR(sc.discharge_power_limit_kw, 0.0f, 0.5f);   // 过载钳到 ~0
    EXPECT_NEAR(sc.charge_power_limit_kw, 80.0f, 0.5f);     // 充电不受影响, 收敛到 80
}

void test_merge_extreme_overload_blocks_discharge() {
    Config cfg;
    TestBms bms;
    TestMeter meter;
    SafetyConstraintManager mgr;
    mgr.Init(cfg, &bms, &meter);

    bms.data.charge_enable = true;
    bms.data.discharge_enable = true;
    bms.data.max_charge_power = 80.0f;
    bms.data.max_discharge_power = 80.0f;

    meter.load_kw = 250.0f;  // 111% 极端过载

    SafetyConstraints sc = Step(mgr, bms, meter, 1500);
    EXPECT(sc.discharge_blocked == true);
    EXPECT_NEAR(sc.discharge_power_limit_kw, 0.0f, kEps);
}

void test_merge_bms_block_overrides_limits() {
    Config cfg;
    TestBms bms;
    TestMeter meter;
    SafetyConstraintManager mgr;
    mgr.Init(cfg, &bms, &meter);

    bms.data.charge_enable = true;
    bms.data.discharge_enable = true;
    bms.data.max_charge_power = 80.0f;
    bms.data.max_discharge_power = 80.0f;

    meter.load_kw = 100.0f;

    // 1. 先建立稳定状态（用 stable prev）
    Step(mgr, bms, meter, 50);
    Step(mgr, bms, meter, 100);

    // 2. 翻转使能位 -> 需要两次确认, 中间一帧不阻塞
    bms.data.charge_enable = false;
    SafetyConstraints sc1 = Step(mgr, bms, meter, 200);  // 去抖: 不更新
    EXPECT(sc1.charge_blocked == false);
    sc1 = Step(mgr, bms, meter, 300);  // 去抖: 确认
    EXPECT(sc1.charge_blocked == true);
    EXPECT_NEAR(sc1.charge_power_limit_kw, 0.0f, kEps);
    EXPECT(sc1.discharge_blocked == false);  // 放电未禁止
}

} // namespace

int main() {
    std::printf("Running safety_constraint_manager unit tests ...\n");

    test_strategy1_normal();
    test_strategy1_comm_loss();
    test_strategy2_lowpass_init();
    test_strategy2_lowpass_step();
    test_strategy2_comm_loss();
    test_strategy3_threshold();
    test_strategy3_extreme_overload();
    test_merge_basic();
    test_merge_transformer_overload();
    test_merge_extreme_overload_blocks_discharge();
    test_merge_bms_block_overrides_limits();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d ASSERTION(S) FAILED\n", failures);
    return 1;
}
