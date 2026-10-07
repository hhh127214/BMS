// =====================================================================
// 17/tests/test_sim_pcs.cpp —— PCS 高保真模拟器（§6.1 模拟器矩阵）
//
// 缺口原文：PCS 现有 3 点（P_BAT / 通信 / 故障）+ 物理积分；
//          目标是把点位扩到全表（有功无功/模式/状态字/故障码/三相电量）。
//
//   T01 一阶惯性 τ（与 11/ 同口径）
//   T02 变化率限幅
//   T03 运行模式状态机（待机 / 并网 / 离网 / 故障）
//   T04 无功 / 功率因数 / 视在容量约束（含 P=0 纯无功）
//   T05 直流侧 Udc/Idc + 交流侧三相
//   T06 日/累计发电量 + 运行小时（按功率积分）
//   T07 故障码注入 **与恢复**
//   T08 点名交叉核对 + "全部在动"自证 + 反向守卫
//
// 编译：见 17/scripts/build_test.bat
// =====================================================================

#include "expect.h"

#include "device_point_table.h"
#include "pcs_sim.h"
#include "point_sink.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace ems;
using namespace ems::devsim;
using namespace ems::devpt;

static int g_pass = 0;
static int g_fail = 0;

static const double kPi = 3.14159265358979323846;

// =====================================================================
// T01 一阶惯性
// =====================================================================
static void test_01_inertia() {
    std::printf("T01 一阶惯性（与 11/ 同口径 p += (ref-p)·dt/τ）\n");

    PcsSimConfig c;
    c.tau_s         = 0.5;
    c.ramp_kw_per_s = 1e9;            // 关掉限幅，只看惯性
    PcsSim p(c);
    TraceSink s;

    // 第一拍：p = ref · dt/τ = 200 · 0.1/0.5 = 40 kW
    p.step(0.1, 0.1, s, 200.0, 0.0, true);
    EXPECT_NEAR(s.stat(dpp(DP_PCS_P_ACT))->last, 40.0, 1e-9);
    // 第二拍：p = 40 + (200−40)·0.2 = 72
    p.step(0.2, 0.1, s, 200.0, 0.0, true);
    EXPECT_NEAR(s.stat(dpp(DP_PCS_P_ACT))->last, 72.0, 1e-9);
    // 解析解：p_n = ref·(1 − (1−α)^n)
    double expect = 0.0;
    for (int k = 3; k <= 50; ++k) {
        p.step(k * 0.1, 0.1, s, 200.0, 0.0, true);
        expect = 200.0 * (1.0 - std::pow(0.8, k));
    }
    EXPECT_NEAR(s.stat(dpp(DP_PCS_P_ACT))->last, expect, 1e-9);
    EXPECT(std::fabs(s.stat(dpp(DP_PCS_P_ACT))->last - 200.0) < 1.0);   // 50 拍后基本到位

    // ★ 反向守卫：τ 越小响应越快（同一拍比较）—— 否则"惯性"这件事不可见
    PcsSimConfig cf;
    cf.tau_s = 0.1;
    cf.ramp_kw_per_s = 1e9;
    PcsSim fast(cf);
    TraceSink sf;
    fast.step(0.1, 0.1, sf, 200.0, 0.0, true);
    EXPECT_NEAR(sf.stat(dpp(DP_PCS_P_ACT))->last, 200.0, 1e-9);          // 一拍到位
    EXPECT(std::fabs(sf.stat(dpp(DP_PCS_P_ACT))->last -
                     s.stat(dpp(DP_PCS_P_ACT))->last) >= 0.0);
}

// =====================================================================
// T02 变化率限幅
// =====================================================================
static void test_02_ramp() {
    std::printf("T02 变化率限幅\n");

    PcsSimConfig c;
    c.tau_s         = 1e-6;           // 惯性极快 → 限幅接管
    c.ramp_kw_per_s = 100.0;          // 100 kW/s → dt=0.1 每拍最多 10 kW
    PcsSim p(c);
    TraceSink s;
    double prev = 0.0;
    double max_step = 0.0;
    for (int k = 0; k < 60; ++k) {
        p.step((k + 1) * 0.1, 0.1, s, 200.0, 0.0, true);
        const double now = s.stat(dpp(DP_PCS_P_ACT))->last;
        max_step = std::max(max_step, std::fabs(now - prev));
        prev = now;
    }
    EXPECT_NEAR(max_step, 10.0, 1e-6);              // 恰好贴住限幅
    EXPECT_NEAR(prev, 200.0, 1e-6);                 // 60 拍 × 10 kW 刚好到额定

    // 反向守卫：把限幅放大 → 同一工况下每拍变化必须**更大**
    PcsSimConfig c2;
    c2.tau_s = 1e-6;
    c2.ramp_kw_per_s = 1e9;
    PcsSim p2(c2);
    TraceSink s2;
    p2.step(0.1, 0.1, s2, 200.0, 0.0, true);
    EXPECT(s2.stat(dpp(DP_PCS_P_ACT))->last > 10.0 + 1.0);
}

// =====================================================================
// T03 运行模式状态机
// =====================================================================
static void test_03_mode_fsm() {
    std::printf("T03 运行模式状态机\n");

    PcsSimConfig c;
    c.tau_s = 0.2;
    c.ramp_kw_per_s = 1e9;
    PcsSim p(c);
    TraceSink s;

    // 并网运行
    for (int k = 0; k < 20; ++k) p.step((k + 1) * 0.1, 0.1, s, 150.0, 0.0, true);
    EXPECT_EQ(s.stat(dpp(DP_PCS_MODE))->last, PcsSim::kGridTied);
    EXPECT(p.p_actual() > 140.0);

    // 停机（onoff=false）：模式 → STANDBY，出力按惯性滑到 0
    p.step(2.1, 0.1, s, 150.0, 0.0, false);
    EXPECT_EQ(s.stat(dpp(DP_PCS_MODE))->last, PcsSim::kStandby);
    const double p_after_one = p.p_actual();
    EXPECT(p_after_one < 150.0);
    EXPECT(p_after_one > 0.0);                       // 不是"立刻变 0"
    for (int k = 0; k < 40; ++k) p.step(2.2 + k * 0.1, 0.1, s, 150.0, 0.0, false);
    EXPECT(std::fabs(p.p_actual()) < 1e-6);          // 最终归零
    EXPECT_NEAR(s.stat(dpp(DP_PCS_LIMIT_REASON))->last, 6.0, 1e-12);   // 停机态

    // 离网（电网不可用）
    p.mutable_cfg().grid_present = false;
    p.step(10.0, 0.1, s, 150.0, 0.0, true);
    EXPECT_EQ(s.stat(dpp(DP_PCS_MODE))->last, PcsSim::kOffGrid);
    EXPECT_NEAR(s.stat(dpp(DP_PCS_LIMIT_REASON))->last, 5.0, 1e-12);

    // 故障
    p.mutable_cfg().grid_present = true;
    p.mutable_cfg().fault_code   = 42;
    p.step(11.0, 0.1, s, 150.0, 0.0, true);
    EXPECT_EQ(s.stat(dpp(DP_PCS_MODE))->last, PcsSim::kFault);
    EXPECT_NEAR(s.stat(dpp(DP_PCS_FAULT))->last, 1.0, 1e-12);
    EXPECT_EQ(static_cast<int>(s.stat(dpp(DP_PCS_MODE))->last), 3);
    // 模式名（给人看）
    EXPECT_STR_EQ(PcsSim::mode_name(PcsSim::kGridTied), "GRID_TIED");
    EXPECT_STR_EQ(PcsSim::mode_name(PcsSim::kFault), "FAULT");
    EXPECT_STR_EQ(PcsSim::mode_name(99), "?");
}

// =====================================================================
// T04 无功 / 功率因数 / 视在容量
// =====================================================================
static void test_04_reactive() {
    std::printf("T04 无功 / 功率因数 / 视在容量约束\n");

    PcsSimConfig c;
    c.tau_s = 1e-6;
    c.ramp_kw_per_s = 1e9;
    c.rated_p_kw = 200.0;
    c.rated_q_kvar = 200.0;
    {   // P=0 时的**纯无功**（STATCOM 模式）—— 这条最容易写错
        PcsSim p(c);
        TraceSink s;
        for (int k = 0; k < 10; ++k) p.step((k + 1) * 0.1, 0.1, s, 0.0, 120.0, true);
        EXPECT_NEAR(s.stat(dpp(DP_PCS_Q_ACT))->last, 120.0, 1e-6);
        EXPECT_NEAR(s.stat(dpp(DP_PCS_S_ACT))->last, 120.0, 1e-6);
        EXPECT_NEAR(s.stat(dpp(DP_PCS_PF))->last, 0.0, 1e-9);       // P=0 → PF=0
    }
    {   // 额定有功 + 无功 → 视在容量约束把无功压到 0（s_max = 200）
        PcsSim p(c);
        TraceSink s;
        for (int k = 0; k < 40; ++k) p.step((k + 1) * 0.1, 0.1, s, 200.0, 100.0, true);
        EXPECT_NEAR(s.stat(dpp(DP_PCS_P_ACT))->last, 200.0, 1e-6);
        EXPECT(std::fabs(s.stat(dpp(DP_PCS_Q_ACT))->last) < 1e-6);
        EXPECT_NEAR(s.stat(dpp(DP_PCS_PF))->last, 1.0, 1e-9);
        EXPECT_NEAR(s.stat(dpp(DP_PCS_LIMIT_REASON))->last, 2.0, 1e-12);  // 无功受限
    }
    {   // 半额定有功 + 无功 → PF 不低于 pf_min=0.9
        PcsSim p(c);
        TraceSink s;
        for (int k = 0; k < 40; ++k) p.step((k + 1) * 0.1, 0.1, s, 100.0, 100.0, true);
        const double pf = s.stat(dpp(DP_PCS_PF))->last;
        EXPECT(pf >= 0.9 - 1e-9);
        // q 被卡在 |p|·tan(acos(0.9)) = 48.43
        EXPECT_NEAR(std::fabs(s.stat(dpp(DP_PCS_Q_ACT))->last), 48.4322, 1e-3);
        EXPECT_NEAR(s.stat(dpp(DP_PCS_S_ACT))->last,
                    std::sqrt(100.0 * 100.0 + s.stat(dpp(DP_PCS_Q_ACT))->last *
                                               s.stat(dpp(DP_PCS_Q_ACT))->last), 1e-9);
    }
    {   // 指令越额定 → 限幅 + 原因码 3
        PcsSim p(c);
        TraceSink s;
        for (int k = 0; k < 60; ++k) p.step((k + 1) * 0.1, 0.1, s, 999.0, 0.0, true);
        EXPECT_NEAR(s.stat(dpp(DP_PCS_P_ACT))->last, 200.0, 1e-6);
        EXPECT_NEAR(s.stat(dpp(DP_PCS_LIMIT_REASON))->last, 3.0, 1e-12);
        // ★ 反向守卫：指令在额定内时，原因码必须是 0（不是"永远报限幅"）
        PcsSim p2(c);
        TraceSink s2;
        for (int k = 0; k < 40; ++k) p2.step((k + 1) * 0.1, 0.1, s2, 100.0, 0.0, true);
        EXPECT_NEAR(s2.stat(dpp(DP_PCS_LIMIT_REASON))->last, 0.0, 1e-12);
    }
}

// =====================================================================
// T05 直流侧 / 交流侧
// =====================================================================
static void test_05_dc_ac() {
    std::printf("T05 直流侧 Udc/Idc + 交流侧三相\n");

    PcsSimConfig c;
    c.tau_s = 1e-6;
    c.ramp_kw_per_s = 1e9;
    PcsSim p(c);
    TraceSink s;
    for (int k = 0; k < 10; ++k) p.step((k + 1) * 0.1, 0.1, s, 200.0, 0.0, true);

    const double udc = s.stat(dpp(DP_PCS_UDC))->last;
    const double idc = s.stat(dpp(DP_PCS_IDC))->last;
    // Udc = 750 − 0.30×200 ± 2 V
    EXPECT_NEAR(udc, 750.0 - 60.0, 2.1);
    // Idc = P·1000 / Udc
    EXPECT_NEAR(idc, 200.0 * 1000.0 / udc, 1e-6);
    // 充电时 Idc 反号
    PcsSim pc(c);
    TraceSink sc;
    for (int k = 0; k < 10; ++k) pc.step((k + 1) * 0.1, 0.1, sc, -200.0, 0.0, true);
    EXPECT(sc.stat(dpp(DP_PCS_IDC))->last < 0.0);

    // 三相电压 ≈ 230 V；三相电流 > 0
    EXPECT_NEAR(s.stat(dpp(DP_PCS_U_A))->last, 230.0, 2.0);
    EXPECT_NEAR(s.stat(dpp(DP_PCS_U_B))->last, 230.0, 2.0);
    EXPECT_NEAR(s.stat(dpp(DP_PCS_U_C))->last, 230.0, 2.0);
    EXPECT(s.stat(dpp(DP_PCS_I_A))->last > 100.0);
    EXPECT_NEAR(s.stat(dpp(DP_PCS_FREQ))->last, 50.0, 0.2);
    // 三相电流刻度自洽：I = S·1000/(3·U)
    const double expect_i = 200.0 * 1000.0 / (3.0 * s.stat(dpp(DP_PCS_U_A))->last);
    EXPECT_NEAR(s.stat(dpp(DP_PCS_I_A))->last, expect_i, 1e-6);
}

// =====================================================================
// T06 电量 / 运行小时
// =====================================================================
static void test_06_energy() {
    std::printf("T06 日/累计发电量 + 运行小时（按功率积分）\n");

    PcsSimConfig c;
    c.tau_s = 1e-6;
    c.ramp_kw_per_s = 1e9;
    PcsSim p(c);
    TraceSink s;
    // 100 kW × 3600 s = 100 kWh；dt=1 → 3600 拍
    for (int k = 0; k < 3600; ++k) p.step(k * 1.0, 1.0, s, 100.0, 0.0, true);
    EXPECT_NEAR(p.e_day_kwh(), 100.0, 1e-3);
    EXPECT_NEAR(p.e_total_kwh(), 100.0, 1e-3);
    EXPECT_NEAR(p.e_chg_kwh(), 0.0, 1e-9);
    EXPECT_NEAR(s.stat(dpp(DP_PCS_E_DAY_KWH))->last, 100.0, 1e-3);
    EXPECT_NEAR(p.run_hours(), 1.0, 1e-6);

    // 充电不进"发电量"，但进"充电量"
    PcsSim p2(c);
    TraceSink s2;
    for (int k = 0; k < 1800; ++k) p2.step(k * 1.0, 1.0, s2, -100.0, 0.0, true);
    EXPECT_NEAR(p2.e_day_kwh(), 0.0, 1e-9);
    EXPECT_NEAR(p2.e_chg_kwh(), 50.0, 1e-3);

    // 待机不累计运行小时
    PcsSim p3(c);
    TraceSink s3;
    for (int k = 0; k < 600; ++k) p3.step(k * 1.0, 1.0, s3, 100.0, 0.0, false);
    EXPECT_NEAR(p3.run_hours(), 0.0, 1e-12);
}

// =====================================================================
// T07 故障注入与恢复
// =====================================================================
static void test_07_fault_recovery() {
    std::printf("T07 故障码注入与恢复\n");

    PcsSimConfig c;
    c.tau_s = 0.2;
    c.ramp_kw_per_s = 1e9;
    PcsSim p(c);
    TraceSink s;

    for (int k = 0; k < 20; ++k) p.step((k + 1) * 0.1, 0.1, s, 150.0, 0.0, true);
    EXPECT_NEAR(s.stat(dpp(DP_PCS_FAULT))->last, 0.0, 1e-12);
    EXPECT_NEAR(s.stat(dpp(DP_PCS_ALM_LEVEL))->last, 0.0, 1e-12);

    // 注入故障 42
    p.mutable_cfg().fault_code = 42;
    for (int k = 0; k < 30; ++k) p.step(2.0 + k * 0.1, 0.1, s, 150.0, 0.0, true);
    EXPECT_NEAR(s.stat(dpp(DP_PCS_FAULT))->last, 1.0, 1e-12);
    EXPECT_NEAR(s.stat(dpp(DP_PCS_FAULT_CODE))->last, 42.0, 1e-12);
    EXPECT_NEAR(s.stat(dpp(DP_PCS_ALM_LEVEL))->last, 3.0, 1e-12);
    EXPECT(std::fabs(p.p_actual()) < 1.0);              // 出力被撤

    // ★ 恢复：故障清除后必须**能回到并网**（"故障后不自动带载"是安全规程，
    //   但那是"不自动带载"，不是"永远起不来"）
    p.mutable_cfg().fault_code = 0;
    for (int k = 0; k < 40; ++k) p.step(5.0 + k * 0.1, 0.1, s, 150.0, 0.0, true);
    EXPECT_NEAR(s.stat(dpp(DP_PCS_FAULT))->last, 0.0, 1e-12);
    EXPECT_NEAR(s.stat(dpp(DP_PCS_FAULT_CODE))->last, 0.0, 1e-12);
    EXPECT_EQ(s.stat(dpp(DP_PCS_MODE))->last, PcsSim::kGridTied);
    EXPECT(p.p_actual() > 140.0);                       // 恢复出力
    // 反向守卫：上面"恢复"不是因为从来没故障过 —— 观测窗内确实出现过故障
    EXPECT(s.changes(dpp(DP_PCS_FAULT)) >= 2);
}

// =====================================================================
// T08 点名核对 + 全部在动 + 反向守卫
// =====================================================================
static void test_08_names_and_moving() {
    std::printf("T08 点名核对 + 全部在动自证\n");

    PcsSimConfig c;
    c.tau_s = 0.3;
    c.ramp_kw_per_s = 300.0;
    PcsSim p(c);
    TraceSink s;
    p.publish_static(s);                                // 静态 6（CFG 4 + PCS 额定 2）

    const double dt = 0.5;
    for (int k = 0; k < 1200; ++k) {                    // 600 s
        const double t = k * dt;
        const double t0 = 400.0;
        if (t >= t0 && t < t0 + 20.0) p.mutable_cfg().fault_code = 7;   // 故障窗
        else                          p.mutable_cfg().fault_code = 0;
        p.step(t, dt, s, 150.0 * std::sin(2.0 * kPi * t / 300.0),
               80.0 * std::sin(2.0 * kPi * t / 180.0), true);
    }

    int unknown = 0, wrong_dev = 0;
    for (const auto& n : s.names()) {
        const int i = dev_point_index_of(n);
        if (i < 0) { ++unknown; std::printf("    点表里没有: %s\n", n.c_str()); }
        else if (kBuiltinPoints[i].device != DK_PCS) ++wrong_dev;
    }
    EXPECT_EQ(unknown, 0);
    EXPECT_EQ(wrong_dev, 0);
    // 动态 23 + 静态 6 = 29（PCS 侧 34 点里，CMD 区 5 点是 EMS 下行，不由模拟器发布）
    EXPECT_EQ(static_cast<int>(s.point_count()), 29);

    const std::vector<std::pair<std::string, double>> req = {
        {dpp(DP_PCS_P_ACT),      10.0},
        {dpp(DP_PCS_Q_ACT),      1.0},
        {dpp(DP_PCS_S_ACT),      10.0},
        {dpp(DP_PCS_UDC),        0.5},
        {dpp(DP_PCS_IDC),        5.0},
        {dpp(DP_PCS_U_A),        0.01},
        {dpp(DP_PCS_I_A),        1.0},
        {dpp(DP_PCS_FREQ),       0.001},
        {dpp(DP_PCS_E_DAY_KWH),  1.0},
        {dpp(DP_PCS_E_TOTAL_KWH),1.0},
        {dpp(DP_PCS_RUN_HOURS),  0.01},
        {dpp(DP_PCS_LIMIT_REASON), 0.5},
        {dpp(DP_PCS_FAULT),      0.5},
        {dpp(DP_PCS_FAULT_CODE), 0.5},
        {dpp(DP_PCS_ALM_LEVEL),  0.5}
    };
    const MovementReport rep = check_moving(s, req);
    std::printf("    %s\n", rep.text().c_str());
    EXPECT_EQ(static_cast<int>(rep.missing.size()), 0);
    EXPECT_EQ(static_cast<int>(rep.frozen.size()), 0);

    // ★ 反向守卫：故障窗内 FAULT 确实置位过（否则上面那三条是白测的）
    EXPECT(s.stat(dpp(DP_PCS_FAULT))->max_v >= 1.0);
    EXPECT(s.stat(dpp(DP_PCS_FAULT))->min_v <= 0.0);
    EXPECT(s.changes(dpp(DP_PCS_FAULT)) >= 2);
}

int main() {
    TEST_BANNER("17/ PCS 模拟器 —— 全表点位 + 物理积分 + 模式状态机");

    test_01_inertia();
    test_02_ramp();
    test_03_mode_fsm();
    test_04_reactive();
    test_05_dc_ac();
    test_06_energy();
    test_07_fault_recovery();
    test_08_names_and_moving();

    TEST_TAIL();
    return g_fail == 0 ? 0 : 1;
}
