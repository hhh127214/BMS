// =====================================================================
// 17/tests/test_sim_meter.cpp —— 关口电表模拟器（B4）
//
// 缺口原文：`11/src/integration_runner.h:134-138` 的电表 = 两个 sin()，
// 没有三相、没有电能累积、没有需量。本测试就是这三条的判据。
//
//   T01 三相 / 线电压 / 电流 / 频率 / PF 基本关系
//   T02 电能累积 == ∫P dt（恒定正向 / 反向 / 净口径）
//   T03 需量：滑窗平均 + 峰值 + 峰值发生时刻
//   T04 一阶时延（A2 遗留）：τ=0 理想 vs τ>0 滞后
//   T05 系统偏差 + 慢漂移（A2 遗留）让"电表读数 ≠ 功率平衡"可表达
//   T06 状态位 + 静态配置点
//   T07 交叉核对：发出的每个点名都必须在全点表里存在
//   T08 "全部在动"自证（不许有恒为常量的**关键量**）+ 反向守卫
//
// 编译：见 17/scripts/build_test.bat
// =====================================================================

#include "expect.h"

#include "device_point_table.h"
#include "meter_sim.h"
#include "point_sink.h"

#include <cmath>
#include <string>
#include <vector>

using namespace ems;
using namespace ems::devsim;
using namespace ems::devpt;

static int g_pass = 0;
static int g_fail = 0;

static const double kPi = 3.14159265358979323846;

// 演示工况 A：关口功率 300 ± 100 kW（恒正 —— 只走正向电能）
static double p_of(double t) { return 300.0 + 100.0 * std::sin(2.0 * kPi * t / 60.0); }

// 演示工况 B：**双极性** 60 ± 250 kW（净进口，但半个周期在倒送）
//   —— 只有它能同时让"正向电能"和"反向电能"都真的动起来。
//   工况 A 下 EP_REV 恒为 0，若把它写进"在动"清单就会红 —— 那是**判据写错**，
//   不是模拟器错。这正是"必须按物理量纲给阈值"的原因（见 point_sink.h 说明）。
static double p_bi(double t) { return 60.0 + 250.0 * std::sin(2.0 * kPi * t / 60.0); }

// =====================================================================
// T01 三相 / 线电压 / PF
// =====================================================================
static void test_01_three_phase() {
    std::printf("T01 三相 / 线电压 / 电流 / 频率 / PF\n");

    MeterSimConfig c;
    c.imbalance = 0.03;
    MeterSim m(c);
    TraceSink s;
    MeterInputs in;
    in.p_kw = 300.0;
    in.q_kvar = 60.0;
    in.p_load_kw = 420.0;
    in.p_pv_kw = 120.0;
    m.step(0.5, 0.5, s, in);

    // 三相电压量级对（230 V 相电压，带不平衡与纹波）
    EXPECT_NEAR(s.stat(dp(DP_METER_U_A))->last, 230.0, 12.0);
    EXPECT_NEAR(s.stat(dp(DP_METER_U_B))->last, 230.0, 12.0);
    EXPECT_NEAR(s.stat(dp(DP_METER_U_C))->last, 230.0, 12.0);
    // 三相不平衡确实存在（不是三相一模一样 —— 那等于没有不平衡）
    const double u_a = s.stat(dp(DP_METER_U_A))->last;
    const double u_b = s.stat(dp(DP_METER_U_B))->last;
    const double u_c = s.stat(dp(DP_METER_U_C))->last;
    EXPECT(std::fabs(u_a - u_b) > 1e-6);
    EXPECT(std::fabs(u_b - u_c) > 1e-6);

    // 线电压 ≈ √3 × 相电压
    EXPECT_NEAR(s.stat(dp(DP_METER_U_AB))->last, std::sqrt(3.0) * u_a, 1e-6);
    EXPECT_NEAR(s.stat(dp(DP_METER_U_BC))->last, std::sqrt(3.0) * u_b, 1e-6);
    EXPECT_NEAR(s.stat(dp(DP_METER_U_CA))->last, std::sqrt(3.0) * u_c, 1e-6);

    // 三相电流 > 0，且零序 < 相电流（不平衡量级合理）
    EXPECT(s.stat(dp(DP_METER_I_A))->last > 10.0);
    EXPECT(s.stat(dp(DP_METER_I_B))->last > 10.0);
    EXPECT(s.stat(dp(DP_METER_I_C))->last > 10.0);
    EXPECT(s.stat(dp(DP_METER_I_N))->last < s.stat(dp(DP_METER_I_A))->last);

    // 频率 50 Hz 附近
    EXPECT_NEAR(s.stat(dp(DP_METER_FREQ))->last, 50.0, 0.2);

    // PF == P / S
    const double p = s.stat(dp(DP_METER_P_TOTAL))->last;
    const double q = s.stat(dp(DP_METER_Q_TOTAL))->last;
    const double ss = std::sqrt(p * p + q * q);
    EXPECT_NEAR(s.stat(dp(DP_METER_S_TOTAL))->last, ss, 1e-6);
    EXPECT_NEAR(s.stat(dp(DP_METER_PF))->last, p / ss, 1e-9);

    // 分相有功之和 == 总有功
    const double ps = s.stat(dp(DP_METER_P_A))->last + s.stat(dp(DP_METER_P_B))->last +
                      s.stat(dp(DP_METER_P_C))->last;
    EXPECT_NEAR(ps, p, 1e-9);

    // 分路（负荷 / 光伏）原样透传
    EXPECT_NEAR(s.stat(dp(DP_METER_P_LOAD))->last, 420.0, 1e-12);
    EXPECT_NEAR(s.stat(dp(DP_METER_P_PV))->last, 120.0, 1e-12);
}

// =====================================================================
// T02 电能累积 == ∫P dt
// =====================================================================
static void test_02_energy_integration() {
    std::printf("T02 电能累积 == ∫P dt\n");

    {   // 恒定正向 360 kW × 10 s = 1.000 kWh（整数好核对）
        MeterSimConfig c;
        c.lag_tau_s = 0.0;
        MeterSim m(c);
        TraceSink s;
        MeterInputs in;
        in.p_kw = 360.0;
        const double dt = 0.5;
        for (int k = 0; k < 20; ++k) m.step((k + 1) * dt, dt, s, in);
        EXPECT_NEAR(m.ep_fwd_kwh(), 1.000, 1e-9);
        EXPECT_NEAR(m.ep_rev_kwh(), 0.0, 1e-12);
        EXPECT_NEAR(s.stat(dp(DP_METER_EP_FWD))->last, 1.000, 1e-9);
    }
    {   // 恒定反向 -360 kW × 5 s = 0.500 kWh（走反向电能）
        MeterSimConfig c;
        c.lag_tau_s = 0.0;
        MeterSim m(c);
        TraceSink s;
        MeterInputs in;
        in.p_kw = -360.0;
        const double dt = 0.5;
        for (int k = 0; k < 10; ++k) m.step((k + 1) * dt, dt, s, in);
        EXPECT_NEAR(m.ep_rev_kwh(), 0.500, 1e-9);
        EXPECT_NEAR(m.ep_fwd_kwh(), 0.0, 1e-12);
    }
    {   // 交变功率：正向/反向电能都累加，且**日电能 = 两者之和**
        MeterSimConfig c;
        c.lag_tau_s = 0.0;
        MeterSim m(c);
        TraceSink s;
        const double dt = 0.25;
        for (int k = 0; k < 480; ++k) {          // 120 s
            MeterInputs in;
            in.p_kw = p_bi((k + 1) * dt);
            m.step((k + 1) * dt, dt, s, in);
        }
        EXPECT_GT0(m.ep_fwd_kwh());
        // ★ 反向守卫：|最小功率| 只有半个周期为负 → 反向电能必须**严格小于**正向
        //   （"两个 sin()"那种写法根本没有正反向之分）
        EXPECT(m.ep_rev_kwh() > 0.0);
        EXPECT(m.ep_rev_kwh() < m.ep_fwd_kwh());
        const double day = s.stat(dp(DP_METER_EP_DAY))->last;
        EXPECT_NEAR(day, m.ep_fwd_kwh() + m.ep_rev_kwh(), 1e-9);
    }
}

// =====================================================================
// T03 需量：滑窗平均 + 峰值 + 峰值发生时刻
// =====================================================================
static void test_03_demand() {
    std::printf("T03 需量：滑窗平均 / 峰值 / 峰值发生时刻\n");

    MeterSimConfig c;
    c.lag_tau_s        = 0.0;
    c.demand_window_s  = 60.0;
    c.demand_slots     = 10;         // 每格 6 s
    MeterSim m(c);
    TraceSink s;
    const double dt = 0.5;

    // 前 30 s 为 0，后 30 s 为 120 kW（阶跃）
    for (int k = 0; k < 60; ++k) {
        MeterInputs in;
        in.p_kw = 0.0;
        m.step((k + 1) * dt, dt, s, in);
    }
    const double demand_before = s.stat(dp(DP_METER_DEMAND_NOW))->last;
    EXPECT_NEAR(demand_before, 0.0, 1e-9);

    for (int k = 60; k < 120; ++k) {
        MeterInputs in;
        in.p_kw = 120.0;
        m.step((k + 1) * dt, dt, s, in);
    }
    const double demand_after = s.stat(dp(DP_METER_DEMAND_NOW))->last;
    // ★ 滑窗平均：窗内一半时间是 0 → 需量应显著小于瞬时 120
    //   如果实现是"取瞬时值当需量"，这条会红。
    EXPECT(demand_after > 10.0);
    EXPECT(demand_after < 90.0);
    // 峰值 = 过程中出现过的最大需量
    EXPECT(s.stat(dp(DP_METER_DEMAND_PEAK))->last >= demand_after - 1e-9);
    EXPECT_NEAR(s.stat(dp(DP_METER_DEMAND_PEAK))->last, m.demand_peak_kw(), 1e-12);
    // 峰值发生时刻必须落在观测窗内，且**不是** 0（真的记了时刻）
    EXPECT(m.demand_peak_ts() > 30.0);
    EXPECT(m.demand_peak_ts() <= 60.0);
    EXPECT_NEAR(s.stat(dp(DP_METER_DEMAND_PEAK_TS))->last, m.demand_peak_ts(), 1e-12);

    // 交变工况下需量必须持续在动（不是一条直线）
    MeterSim m2(c);
    TraceSink s2;
    for (int k = 0; k < 400; ++k) {
        MeterInputs in;
        in.p_kw = p_of((k + 1) * dt);
        m2.step((k + 1) * dt, dt, s2, in);
    }
    EXPECT(s2.spread(dp(DP_METER_DEMAND_NOW)) > 1.0);
    EXPECT(s2.spread(dp(DP_METER_DEMAND_PEAK)) > 1.0);
}

// =====================================================================
// T04 一阶时延
// =====================================================================
static void test_04_lag() {
    std::printf("T04 一阶时延（A2 遗留）：τ=0 理想 vs τ>0 滞后\n");

    // τ = 0：读数立即等于真值
    {
        MeterSimConfig c;
        c.lag_tau_s = 0.0;
        MeterSim m(c);
        TraceSink s;
        MeterInputs in;
        in.p_kw = 100.0;
        m.step(0.1, 0.1, s, in);
        EXPECT_NEAR(s.stat(dp(DP_METER_P_TOTAL))->last, 100.0, 1e-12);
    }
    // τ = 10 s：第一拍只跟上 dt/τ = 1%
    {
        MeterSimConfig c;
        c.lag_tau_s = 10.0;
        MeterSim m(c);
        TraceSink s;
        MeterInputs in;
        in.p_kw = 100.0;
        m.step(0.1, 0.1, s, in);
        EXPECT_NEAR(s.stat(dp(DP_METER_P_TOTAL))->last, 1.0, 1e-9);   // 100 × 0.1/10

        // 给足时间后收敛到真值附近
        for (int k = 1; k < 2000; ++k) m.step(0.1 * (k + 1), 0.1, s, in);
        EXPECT_NEAR(s.stat(dp(DP_METER_P_TOTAL))->last, 100.0, 0.5);
    }
    // ★ 反向守卫：同一时刻 τ=0 与 τ=10 的读数必须**明显不同** ——
    //   否则"加了时延"这件事在数据上不可见（A2 那条缺口正是这么漏过去的）。
    {
        MeterSimConfig c0;
        c0.lag_tau_s = 0.0;
        MeterSimConfig c1;
        c1.lag_tau_s = 10.0;
        MeterSim m0(c0), m1(c1);
        TraceSink s0, s1;
        MeterInputs in;
        in.p_kw = 100.0;
        m0.step(0.1, 0.1, s0, in);
        m1.step(0.1, 0.1, s1, in);
        EXPECT(std::fabs(s0.stat(dp(DP_METER_P_TOTAL))->last -
                         s1.stat(dp(DP_METER_P_TOTAL))->last) > 50.0);
    }
}

// =====================================================================
// T05 系统偏差 + 慢漂移
// =====================================================================
static void test_05_bias_drift() {
    std::printf("T05 系统偏差 + 慢漂移（A2 遗留）\n");

    // 偏差：电表读数 = 真值 + 40 kW（"电表读数 ≠ 功率平衡"可表达）
    {
        MeterSimConfig c;
        c.lag_tau_s = 0.0;
        c.bias_kw   = 40.0;
        MeterSim m(c);
        TraceSink s;
        MeterInputs in;
        in.p_kw = 100.0;
        m.step(0.1, 0.1, s, in);
        EXPECT_NEAR(s.stat(dp(DP_METER_P_TOTAL))->last, 140.0, 1e-9);
    }
    // 漂移：偏差本身在**变**（常量偏差无法表达"慢漂移"）
    {
        MeterSimConfig c;
        c.lag_tau_s      = 0.0;
        c.drift_kw       = 20.0;
        c.drift_period_s = 60.0;
        MeterSim m(c);
        TraceSink s;
        const double dt = 0.5;
        for (int k = 0; k < 240; ++k) {           // 120 s = 2 个漂移周期
            MeterInputs in;
            in.p_kw = 100.0;
            m.step((k + 1) * dt, dt, s, in);
        }
        // 读数在 100 ± 20 之间摆动 → spread ≈ 40
        EXPECT(s.spread(dp(DP_METER_P_TOTAL)) > 30.0);
        EXPECT_NEAR(s.stat(dp(DP_METER_P_TOTAL))->last, 100.0, 25.0);
        // ★ 反向守卫：关掉漂移，同一工况下读数**必须**是常量
        //   （证明前一条的 spread 来自漂移，不是别的东西）
        MeterSimConfig c0 = c;
        c0.drift_kw = 0.0;
        MeterSim m0(c0);
        TraceSink s0;
        for (int k = 0; k < 240; ++k) {
            MeterInputs in;
            in.p_kw = 100.0;
            m0.step((k + 1) * dt, dt, s0, in);
        }
        EXPECT_NEAR(s0.spread(dp(DP_METER_P_TOTAL)), 0.0, 1e-12);
    }
}

// =====================================================================
// T06 状态位 + 静态配置点
// =====================================================================
static void test_06_status_and_static() {
    std::printf("T06 状态位 + 静态配置点\n");

    MeterSimConfig c;
    c.comm_ok    = false;
    c.data_valid = false;
    c.fault      = true;
    MeterSim m(c);
    TraceSink s;
    MeterInputs in;
    in.p_kw = 100.0;
    m.step(0.1, 0.1, s, in);
    EXPECT_NEAR(s.stat(dp(DP_METER_COMM_OK))->last, 0.0, 1e-12);
    EXPECT_NEAR(s.stat(dp(DP_METER_DATA_VALID))->last, 0.0, 1e-12);
    EXPECT_NEAR(s.stat(dp(DP_METER_FAULT))->last, 1.0, 1e-12);

    // 静态配置点（初始化写一次）
    MeterSimConfig c2;
    MeterSim m2(c2);
    m2.set_site_config(630.0, 400.0);
    TraceSink st;
    m2.publish_static(st);
    EXPECT_NEAR(st.stat(dp(DP_METER_TR_KVA))->last, 630.0, 1e-12);
    EXPECT_NEAR(st.stat(dp(DP_METER_D_TARGET))->last, 400.0, 1e-12);
    EXPECT_EQ(st.point_count(), 2);
}

// =====================================================================
// T07 交叉核对：点名的每个点都必须在全点表里
// =====================================================================
static void test_07_names_exist() {
    std::printf("T07 交叉核对：发出的点名都必须在全点表里\n");

    MeterSimConfig c;
    MeterSim m(c);
    TraceSink s;
    const double dt = 0.5;
    for (int k = 0; k < 200; ++k) {
        MeterInputs in;
        in.p_kw = p_of((k + 1) * dt);
        in.q_kvar = 50.0;
        in.p_load_kw = 400.0;
        in.p_pv_kw = 100.0;
        m.step((k + 1) * dt, dt, s, in);
    }
    m.publish_static(s);

    int unknown = 0;
    for (const auto& n : s.names()) {
        if (dev_point_index_of(n) < 0) {
            ++unknown;
            std::printf("    点表里没有: %s\n", n.c_str());
        }
    }
    EXPECT_EQ(unknown, 0);
    // 电表侧动态点 33 个 + 静态配置点 2 个 = 35
    EXPECT_EQ(static_cast<int>(s.point_count()), 35);

    // 每个点都必须是 DK_METER（防止模拟器误发别的设备的点）
    int wrong_dev = 0;
    for (const auto& n : s.names()) {
        const int i = dev_point_index_of(n);
        if (i >= 0 && kBuiltinPoints[i].device != DK_METER) ++wrong_dev;
    }
    EXPECT_EQ(wrong_dev, 0);
}

// =====================================================================
// T08 "全部在动"自证 + 反向守卫
// =====================================================================
static void test_08_all_moving() {
    std::printf("T08 全部在动自证（不许有恒为常量的关键量）\n");

    MeterSimConfig c;
    MeterSim m(c);
    TraceSink s;
    const double dt = 0.5;
    for (int k = 0; k < 600; ++k) {              // 300 s
        MeterInputs in;
        in.p_kw = p_bi((k + 1) * dt);            // 双极性 → 正反电能都在动
        in.q_kvar = 40.0 * std::sin(2.0 * kPi * (k + 1) * dt / 45.0);
        in.p_load_kw = 420.0 + 60.0 * std::sin(2.0 * kPi * (k + 1) * dt / 70.0);
        in.p_pv_kw = std::fmax(0.0, 120.0 * std::sin(2.0 * kPi * (k + 1) * dt / 90.0));
        m.step((k + 1) * dt, dt, s, in);
    }

    const std::vector<std::pair<std::string, double>> req = {
        {dp(DP_METER_U_A),        0.5},
        {dp(DP_METER_U_AB),       0.5},
        {dp(DP_METER_I_A),        1.0},
        {dp(DP_METER_I_N),        0.01},
        {dp(DP_METER_FREQ),       0.001},
        {dp(DP_METER_P_TOTAL),    50.0},
        {dp(DP_METER_Q_TOTAL),    10.0},
        {dp(DP_METER_S_TOTAL),    10.0},
        {dp(DP_METER_PF),         1e-4},
        {dp(DP_METER_THD_U),      0.01},
        {dp(DP_METER_THD_I),      1e-4},
        {dp(DP_METER_P_LOAD),     10.0},
        {dp(DP_METER_P_PV),       10.0},
        {dp(DP_METER_EP_FWD),     1.0},
        {dp(DP_METER_EP_REV),     0.01},
        {dp(DP_METER_EQ_FWD),     0.01},
        {dp(DP_METER_EP_DAY),     1.0},
        {dp(DP_METER_DEMAND_NOW), 1.0},
        {dp(DP_METER_DEMAND_PEAK),1.0},
        {dp(DP_METER_DEMAND_PEAK_TS), 0.0}   // 只要"变过"（> 0 次变化）
    };
    const MovementReport rep = check_moving(s, req);
    std::printf("    %s\n", rep.text().c_str());
    EXPECT_EQ(static_cast<int>(rep.missing.size()), 0);
    EXPECT_EQ(static_cast<int>(rep.frozen.size()), 0);
    EXPECT(rep.ok());

    // 观测簿确实记了东西（防止"没跑起来所以全 0，于是全过"）
    EXPECT_EQ(static_cast<int>(s.point_count()), 33);
    EXPECT(s.point_writes() > 19000);            // 600 拍 × 33 点

    // ★ 反向守卫：喂**恒定**输入 → 关键量必须被判为"不动"。
    //   没有这一条，"check_moving 永远通过"与"模拟器真的在动"不可区分。
    MeterSim m2(c);
    TraceSink s2;
    for (int k = 0; k < 600; ++k) {
        MeterInputs in;
        in.p_kw = 100.0;                          // 恒定
        in.q_kvar = 0.0;
        in.p_load_kw = 0.0;
        in.p_pv_kw = 0.0;
        m2.step((k + 1) * dt, dt, s2, in);
    }
    const std::vector<std::pair<std::string, double>> req2 = {
        {dp(DP_METER_P_TOTAL), 1.0},
        {dp(DP_METER_EP_FWD),  1.0}
    };
    const MovementReport rep2 = check_moving(s2, req2);
    EXPECT_GT0(rep2.frozen.size());               // 恒定 → 必须报"未达标"
    EXPECT(!rep2.ok());
    // 而恒定的输入下，电能仍在累积（它本来就该动）——差别的来源是 P_TOTAL
    EXPECT_NEAR(s2.spread(dp(DP_METER_P_TOTAL)), 0.0, 1e-12);
    EXPECT(s2.spread(dp(DP_METER_EP_FWD)) > 1.0);
}

int main() {
    TEST_BANNER("17/ 关口电表模拟器（B4）—— 替换掉那两个 sin()");

    test_01_three_phase();
    test_02_energy_integration();
    test_03_demand();
    test_04_lag();
    test_05_bias_drift();
    test_06_status_and_static();
    test_07_names_exist();
    test_08_all_moving();

    TEST_TAIL();
    return g_fail == 0 ? 0 : 1;
}
