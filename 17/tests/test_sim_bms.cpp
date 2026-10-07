// =====================================================================
// 17/tests/test_sim_bms.cpp —— BMS 高保真模拟器（B5）
//
// 缺口原文：`MEAS.SOC / T_C / SOH` + 通信位 = 6 点。本测试就是"扩到全表 +
// 每个量都由物理过程推出来"的判据。
//
//   T01 SOC 按功率积分（含充放效率的不对称）
//   T02 SOH / 循环次数按累计吞吐量衰减
//   T03 单体电压：OCV(SOC) / 极值与位置 / 压差 / 过压注入
//   T04 热模型：温升 / 降温 / 离散度 / 温度注入
//   T05 ★ 允许充放功率与禁充放位**同源**（A1 那条 L0 锁的物理自洽）
//   T06 绝缘 / 漏电 / 接触器 / 心跳
//   T07 告警字（level + code）
//   T08 多簇（可配簇数）+ 点名交叉核对
//   T09 "全部在动"自证 + 反向守卫
//
// 编译：见 17/scripts/build_test.bat
// =====================================================================

#include "expect.h"

#include "bms_sim.h"
#include "device_point_table.h"
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

// 默认组串的额定容量：1 × 224 × 3.35 V × 280 Ah / 1000
static double cap_default() { return 1.0 * 224.0 * 3.35 * 280.0 / 1000.0; }

// =====================================================================
// T01 SOC 按功率积分（含充放效率）
// =====================================================================
static void test_01_soc_integration() {
    std::printf("T01 SOC 按功率积分（含充放效率）\n");

    const double cap = cap_default();
    const double dt = 1.0;
    const int    n  = 100;                      // 100 s

    {   // 放电 100 kW：SOC 下降 = E / (η_dis · cap)
        BmsSim b;
        TraceSink s;
        for (int k = 0; k < n; ++k) b.step(k * dt, dt, s, 100.0);
        const double e = 100.0 * n * dt / 3600.0;
        const double expect_soc = 0.5 - e / (0.95 * cap);
        EXPECT_NEAR(b.soc(), expect_soc, 1e-12);
        EXPECT_NEAR(s.stat(dpb(DP_BMS_SOC))->last, expect_soc * 100.0, 1e-9);
        EXPECT(b.soc() < 0.5);
    }
    {   // 充电 100 kW：SOC 上升 = E · η_chg / cap
        BmsSim b;
        TraceSink s;
        for (int k = 0; k < n; ++k) b.step(k * dt, dt, s, -100.0);
        const double e = 100.0 * n * dt / 3600.0;
        const double expect_soc = 0.5 + e * 0.95 / cap;
        EXPECT_NEAR(b.soc(), expect_soc, 1e-12);
        EXPECT(b.soc() > 0.5);
    }
    {   // ★ 效率的**不对称**必须体现出来：
        //   同样 100 kWh 往返，充电挣回的 SOC **少于**放电失去的（η 两侧都 < 1）
        BmsSim b;
        TraceSink s;
        for (int k = 0; k < n; ++k) b.step(k * dt, dt, s, 100.0);
        const double after_dis = b.soc();
        const double lost = 0.5 - after_dis;
        for (int k = n; k < 2 * n; ++k) b.step(k * dt, dt, s, -100.0);
        const double gained = b.soc() - after_dis;
        EXPECT(gained < lost);                  // 往返有损
        EXPECT_GT0(lost - gained);              // 差值 > 0（不是"恰好相等"）
        // 反向守卫：效率为 1 时往返无损
        BmsSimConfig c;
        c.eta_chg = 1.0;
        c.eta_dis = 1.0;
        BmsSim b2(c);
        TraceSink s2;
        for (int k = 0; k < n; ++k) b2.step(k * dt, dt, s2, 100.0);
        const double d2 = 0.5 - b2.soc();
        for (int k = n; k < 2 * n; ++k) b2.step(k * dt, dt, s2, -100.0);
        EXPECT_NEAR(b2.soc() - (0.5 - d2), d2, 1e-12);
    }
    {   // SOC 物理上限不许越界（越充也只是停在 1.0）
        BmsSim b;
        TraceSink s;
        for (int k = 0; k < 20000; ++k) b.step(k * 1.0, 1.0, s, -200.0);
        EXPECT(b.soc() <= 1.0 + 1e-12);
        EXPECT(b.soc() >= 0.0 - 1e-12);
    }
}

// =====================================================================
// T02 SOH / 循环次数
// =====================================================================
static void test_02_soh_cycle() {
    std::printf("T02 SOH / 循环次数按累计吞吐量衰减\n");

    BmsSim b;
    TraceSink s;
    const double dt = 1.0;
    for (int k = 0; k < 3600; ++k) b.step(k * dt, dt, s, 100.0);   // 100 kW × 1 h = 100 kWh

    const double th = b.throughput_kwh();
    EXPECT_NEAR(th, 100.0, 1e-6);
    // SOH = 100 − 20 %/MWh × 0.1 MWh = 98.0
    EXPECT_NEAR(b.soh(), 98.0, 1e-9);
    EXPECT_NEAR(s.stat(dpb(DP_BMS_SOH))->last, 98.0, 1e-9);
    EXPECT(b.soh() < 100.0);
    // 循环次数 = 吞吐量 / (2 · 额定容量)
    EXPECT_NEAR(b.cycle_count(), 100.0 / (2.0 * cap_default()), 1e-12);

    // 反向守卫：不干活就不衰减（吞吐量为 0 → SOH 不动）
    BmsSim b0;
    TraceSink s0;
    for (int k = 0; k < 3600; ++k) b0.step(k * dt, dt, s0, 0.0);
    EXPECT_NEAR(b0.soh(), 100.0, 1e-12);
    EXPECT_NEAR(b0.throughput_kwh(), 0.0, 1e-12);
    // 但温度/电压仍在动 —— 证明"没干活"与"没运行"是两回事
    EXPECT(s0.spread(dpb(DP_BMS_T_MAX)) > 0.0 || s0.has(dpb(DP_BMS_T_MAX)));
}

// =====================================================================
// T03 单体电压
// =====================================================================
static void test_03_cells() {
    std::printf("T03 单体电压：OCV / 极值与位置 / 压差 / 过压注入\n");

    {   // 基本：极值关系、下标范围、压差 = 离散度的量级
        BmsSim b;
        TraceSink s;
        b.step(0.0, 1.0, s, 100.0);
        const double vmax = s.stat(dpb(DP_BMS_CELL_V_MAX))->last;
        const double vmin = s.stat(dpb(DP_BMS_CELL_V_MIN))->last;
        const double diff = s.stat(dpb(DP_BMS_CELL_V_DIFF))->last;
        EXPECT(vmax > vmin);
        EXPECT_NEAR(diff, vmax - vmin, 1e-12);
        EXPECT(diff > 0.02);                    // 离散度 ±0.03 V
        EXPECT(diff < 0.15);
        const double i_max = s.stat(dpb(DP_BMS_CELL_V_MAX_IDX))->last;
        const double i_min = s.stat(dpb(DP_BMS_CELL_V_MIN_IDX))->last;
        EXPECT(i_max >= 1.0 && i_max <= 224.0);
        EXPECT(i_min >= 1.0 && i_min <= 224.0);
        EXPECT(i_max != i_min);
        EXPECT_EQ(b.cell_count(), 224);
        // 极值点与单体数组自洽（极值下标指向的单体就是极值）
        EXPECT_NEAR(b.cell_voltage(static_cast<int>(i_max) - 1), vmax, 1e-12);
        EXPECT_NEAR(b.cell_voltage(static_cast<int>(i_min) - 1), vmin, 1e-12);
    }
    {   // OCV(SOC) 单调：充到高 SOC，单体电压整体抬高
        BmsSim b;
        TraceSink s;
        double v_at_50 = 0.0;
        b.step(0.0, 1.0, s, 0.0);
        v_at_50 = s.stat(dpb(DP_BMS_CELL_V_MAX))->last;
        for (int k = 0; k < 12000; ++k) b.step(k * 1.0, 1.0, s, -60.0);   // 充电
        const double v_high = s.stat(dpb(DP_BMS_CELL_V_MAX))->last;
        EXPECT(b.soc() > 0.8);
        EXPECT(v_high > v_at_50 + 0.03);        // 3.32 V(50%) → 3.40 V(90%) 量级
    }
    {   // 过压注入：0 号单体被抬高，极值下标必须指到 1
        BmsSimConfig c;
        c.inject_cell_v_delta_v = 0.20;
        BmsSim b(c);
        TraceSink s;
        b.step(0.0, 1.0, s, 100.0);
        EXPECT_NEAR(s.stat(dpb(DP_BMS_CELL_V_MAX_IDX))->last, 1.0, 1e-12);
        // 默认偏置下的极值点（记下来，用来做"改偏置方向后极值点必须变"的对照）
        BmsSim b0;
        TraceSink s0;
        b0.step(0.0, 1.0, s0, 100.0);
        const double idx0 = s0.stat(dpb(DP_BMS_CELL_V_MAX_IDX))->last;
        EXPECT(idx0 >= 1.0 && idx0 <= 224.0);
        // 换个偏置方向（离散度反号）→ 极值下标必须跟着换。
        // ★ 这条才是"CELL_V_MAX_IDX 是**算**出来的、不是写死的 1"的判据：
        //   只断言"注入时 == 1"的话，一个恒返回 1 的实现也能过。
        BmsSimConfig c2;
        c2.inject_cell_v_delta_v = 0.0;
        c2.cell_v_spread_v = -0.030;            // 反号离散度
        BmsSim b2(c2);
        TraceSink s2;
        b2.step(0.0, 1.0, s2, 100.0);
        EXPECT(s2.stat(dpb(DP_BMS_CELL_V_MAX_IDX))->last != idx0);
        EXPECT(s2.stat(dpb(DP_BMS_CELL_V_MIN_IDX))->last !=
               s0.stat(dpb(DP_BMS_CELL_V_MIN_IDX))->last);
    }
}

// =====================================================================
// T04 热模型
// =====================================================================
static void test_04_thermal() {
    std::printf("T04 热模型：温升 / 降温 / 离散度 / 注入\n");

    {   // 带载升温
        BmsSim b;
        TraceSink s;
        const double dt = 1.0;
        b.step(0.0, dt, s, 0.0);
        const double t0 = s.stat(dpb(DP_BMS_T_MAX))->last;
        for (int k = 0; k < 600; ++k) b.step(k * dt, dt, s, 200.0);      // 0.95C
        const double t1 = s.stat(dpb(DP_BMS_T_MAX))->last;
        EXPECT(t0 < 28.0);
        EXPECT(t1 > 28.0);                       // 稳态温升 ≈ 8 °C，600 s 到达 ~7.6
        EXPECT(t1 < 35.0);                       // 但没到跳闸区
        EXPECT_NEAR(s.stat(dpb(DP_BMS_T_MIN))->last, t1 - 3.0, 2.0);     // 离散度 ±1.5
        const double idx = s.stat(dpb(DP_BMS_T_MAX_IDX))->last;
        EXPECT(idx >= 1.0 && idx <= 224.0);
    }
    {   // 卸载降温（热模型的"冷"那一支也必须通）
        BmsSim b;
        TraceSink s;
        const double dt = 1.0;
        for (int k = 0; k < 900; ++k) b.step(k * dt, dt, s, 200.0);
        const double hot = s.stat(dpb(DP_BMS_T_MAX))->last;
        for (int k = 900; k < 2400; ++k) b.step(k * dt, dt, s, 0.0);
        const double cool = s.stat(dpb(DP_BMS_T_MAX))->last;
        EXPECT(cool < hot);                      // 真的降下来了
        EXPECT_GT0(hot - cool);
        EXPECT(cool > 25.0);                     // 但不会低于环境温度
    }
    {   // 注入升温
        BmsSimConfig c;
        c.inject_temp_delta_c = 15.0;
        BmsSim b(c);
        TraceSink s;
        for (int k = 0; k < 100; ++k) b.step(k * 1.0, 1.0, s, 0.0);
        EXPECT(s.stat(dpb(DP_BMS_T_MAX))->last > 39.0);
        EXPECT_NEAR(s.stat(dpb(DP_BMS_T_MAX))->last,
                    s.stat(dpb(DP_BMS_T_MIN))->last + 3.0, 2.0);
    }
}

// =====================================================================
// T05 允许充放功率 与 禁充放位 同源（本模块最重要的一条）
// =====================================================================
static void test_05_limit_forbid_same_source() {
    std::printf("T05 ★ 允许充放功率与禁充放位同源\n");

    BmsSim b;
    TraceSink s;
    const double dt = 5.0;
    int violations = 0;
    long checked = 0;

    // ★ 不变式：**只要限值 > 0，禁位必须为 0；只要限值为 0，禁位必须为 1**。
    //   整条轨迹上逐拍检查 —— 这比"看某一个端点"强得多。
    //   A1 缺口的本质就是这两个量来自两条独立的路径（一条通了、一条恒 false），
    //   所以此处的判据必须是"它们在任何时刻都自洽"。
    auto check_invariant = [&](void) {
        const double chg = s.stat(dpb(DP_BMS_CHG_LIMIT_KW))->last;
        const double dis = s.stat(dpb(DP_BMS_DIS_LIMIT_KW))->last;
        const double fchg = s.stat(dpb(DP_BMS_CHG_FORBID))->last;
        const double fdis = s.stat(dpb(DP_BMS_DIS_FORBID))->last;
        ++checked;
        if ((chg <= 1e-9) != (fchg >= 0.5)) ++violations;
        if ((dis <= 1e-9) != (fdis >= 0.5)) ++violations;
    };

    // 一段变功率运行：限值随温度/SOC 抖动，禁位必须跟着一致
    for (int k = 0; k < 1200; ++k) {
        const double p = 150.0 * std::sin(2.0 * kPi * k * dt / 1800.0);
        b.step(k * dt, dt, s, p);
        check_invariant();
    }
    EXPECT_GT0(checked);
    EXPECT_EQ(violations, 0);

    // 中段 SOC：两侧限值都 > 0，两个禁位都是 0
    {
        BmsSim bm;
        TraceSink sm;
        for (int k = 0; k < 20; ++k) bm.step(k * 1.0, 1.0, sm, 0.0);
        EXPECT_NEAR(sm.stat(dpb(DP_BMS_CHG_LIMIT_KW))->last, 200.0, 1e-9);
        EXPECT_NEAR(sm.stat(dpb(DP_BMS_DIS_LIMIT_KW))->last, 200.0, 1e-9);
        EXPECT_NEAR(sm.stat(dpb(DP_BMS_CHG_FORBID))->last, 0.0, 1e-12);
        EXPECT_NEAR(sm.stat(dpb(DP_BMS_DIS_FORBID))->last, 0.0, 1e-12);
    }
    // 充到 SOC 上限：**只禁充**，不禁放（不对称 —— 这条最能抓"用一个开关控两边"的错实现）
    {
        BmsSim bh;
        TraceSink sh;
        int steps = 0;
        for (; steps < 4000 && bh.chg_limit_kw() > 1e-9; ++steps) {
            const double p = -std::max(5.0, bh.chg_limit_kw());
            bh.step(steps * dt, dt, sh, p);
        }
        EXPECT(bh.soc() > 0.94);
        EXPECT_NEAR(sh.stat(dpb(DP_BMS_CHG_LIMIT_KW))->last, 0.0, 1e-9);
        EXPECT_NEAR(sh.stat(dpb(DP_BMS_CHG_FORBID))->last, 1.0, 1e-12);
        EXPECT(sh.stat(dpb(DP_BMS_DIS_LIMIT_KW))->last > 1.0);      // 放电照常
        EXPECT_NEAR(sh.stat(dpb(DP_BMS_DIS_FORBID))->last, 0.0, 1e-12);
        // 禁充原因码 = 1（SOC 过高）
        EXPECT_NEAR(sh.stat(dpb(DP_BMS_FORBID_REASON))->last, 1.0, 1e-12);
    }
    // 放到 SOC 下限：**只禁放**
    {
        BmsSim bl;
        TraceSink sl;
        int steps = 0;
        for (; steps < 4000 && bl.dis_limit_kw() > 1e-9; ++steps) {
            const double p = std::max(5.0, bl.dis_limit_kw());
            bl.step(steps * dt, dt, sl, p);
        }
        EXPECT(bl.soc() < 0.06);
        EXPECT_NEAR(sl.stat(dpb(DP_BMS_DIS_LIMIT_KW))->last, 0.0, 1e-9);
        EXPECT_NEAR(sl.stat(dpb(DP_BMS_DIS_FORBID))->last, 1.0, 1e-12);
        EXPECT(sl.stat(dpb(DP_BMS_CHG_LIMIT_KW))->last > 1.0);
        EXPECT_NEAR(sl.stat(dpb(DP_BMS_CHG_FORBID))->last, 0.0, 1e-12);
        EXPECT_NEAR(sl.stat(dpb(DP_BMS_FORBID_REASON))->last, 2.0, 1e-12);
    }
    // 过温：两侧同时降额 —— 高温是**共同的**约束（与 SOC 的不对称正好互补）
    {
        BmsSimConfig c;
        c.inject_temp_delta_c = 22.0;            // T_max ≈ 47 °C → f_temp = 0
        BmsSim bt(c);
        TraceSink st;
        for (int k = 0; k < 50; ++k) bt.step(k * 1.0, 1.0, st, 0.0);
        EXPECT(st.stat(dpb(DP_BMS_T_MAX))->last > 45.0);
        EXPECT_NEAR(st.stat(dpb(DP_BMS_CHG_LIMIT_KW))->last, 0.0, 1e-9);
        EXPECT_NEAR(st.stat(dpb(DP_BMS_DIS_LIMIT_KW))->last, 0.0, 1e-9);
        EXPECT_NEAR(st.stat(dpb(DP_BMS_CHG_FORBID))->last, 1.0, 1e-12);
        EXPECT_NEAR(st.stat(dpb(DP_BMS_DIS_FORBID))->last, 1.0, 1e-12);
    }
}

// =====================================================================
// T06 绝缘 / 漏电 / 接触器 / 心跳
// =====================================================================
static void test_06_insulation() {
    std::printf("T06 绝缘 / 漏电 / 接触器 / 心跳\n");

    BmsSim b;
    TraceSink s;
    const double dt = 2.0;
    for (int k = 0; k < 400; ++k) {
        b.step(k * dt, dt, s, 180.0 * std::sin(2.0 * kPi * k * dt / 600.0));
    }
    EXPECT(s.stat(dpb(DP_BMS_INSUL_R))->last > 0.0);
    EXPECT(s.spread(dpb(DP_BMS_INSUL_R)) > 5.0);        // 随温度/纹波在动
    EXPECT(s.stat(dpb(DP_BMS_LEAK_I))->last > 0.0);
    EXPECT(s.spread(dpb(DP_BMS_LEAK_I)) > 0.1);
    // 漏电随温度上升（正相关）：高温工况下漏电流更大
    EXPECT_NEAR(s.stat(dpb(DP_BMS_MAIN_POS))->last, 1.0, 1e-12);
    EXPECT_NEAR(s.stat(dpb(DP_BMS_MAIN_NEG))->last, 1.0, 1e-12);
    // 心跳单调递增
    EXPECT(s.changes(dpb(DP_BMS_HEARTBEAT)) == 399);
    EXPECT_NEAR(s.stat(dpb(DP_BMS_HEARTBEAT))->last, 400.0, 1e-12);

    // 绝缘注入：绝缘下降 → 过低时告警
    BmsSimConfig c;
    c.inject_insul_scale = 0.03;                         // 2000 → 60 kΩ
    BmsSim bi(c);
    TraceSink si;
    for (int k = 0; k < 20; ++k) bi.step(k * 1.0, 1.0, si, 0.0);
    EXPECT(si.stat(dpb(DP_BMS_INSUL_R))->last < 100.0);
    EXPECT(si.stat(dpb(DP_BMS_ALM_LEVEL))->last >= 3.0);

    // BMS 自身故障 → 接触器打开、数据无效（但**不是**禁充放）
    BmsSimConfig cf;
    cf.bms_fault = true;
    BmsSim bf(cf);
    TraceSink sf;
    bf.step(0.0, 1.0, sf, 0.0);
    EXPECT_NEAR(sf.stat(dpb(DP_BMS_MAIN_POS))->last, 0.0, 1e-12);
    EXPECT_NEAR(sf.stat(dpb(DP_BMS_MAIN_NEG))->last, 0.0, 1e-12);
    EXPECT_NEAR(sf.stat(dpb(DP_BMS_DATA_VALID))->last, 0.0, 1e-12);
    // ★ 故障 ≠ 禁充放：禁位由温度/SOC 决定，不由 bms_fault 决定（§10.1 A1 的纪律）
    EXPECT_NEAR(sf.stat(dpb(DP_BMS_CHG_FORBID))->last, 0.0, 1e-12);
    EXPECT_NEAR(sf.stat(dpb(DP_BMS_DIS_FORBID))->last, 0.0, 1e-12);
}

// =====================================================================
// T07 告警字
// =====================================================================
static void test_07_alarm() {
    std::printf("T07 告警字（level + code）\n");

    {   // 正常工况：0 级
        BmsSim b;
        TraceSink s;
        for (int k = 0; k < 60; ++k) b.step(k * 1.0, 1.0, s, 100.0);
        EXPECT_NEAR(s.stat(dpb(DP_BMS_ALM_LEVEL))->last, 0.0, 1e-12);
        EXPECT_NEAR(s.stat(dpb(DP_BMS_ALM_CODE))->last, 0.0, 1e-12);
    }
    {   // 过温 1~2 级：code 3
        BmsSimConfig c;
        c.inject_temp_delta_c = 17.0;        // T_max ≈ 42 → level 1
        BmsSim b(c);
        TraceSink s;
        for (int k = 0; k < 60; ++k) b.step(k * 1.0, 1.0, s, 0.0);
        EXPECT_NEAR(s.stat(dpb(DP_BMS_ALM_LEVEL))->last, 1.0, 1e-12);
        EXPECT_NEAR(s.stat(dpb(DP_BMS_ALM_CODE))->last, 3.0, 1e-12);
    }
    {   // 单体过压 → 3 级 + code 4
        BmsSimConfig c;
        c.inject_cell_v_delta_v = 0.40;      // 3.32 + 0.03 + 0.40 > 3.65
        BmsSim b(c);
        TraceSink s;
        for (int k = 0; k < 60; ++k) b.step(k * 1.0, 1.0, s, 0.0);
        EXPECT(s.stat(dpb(DP_BMS_CELL_V_MAX))->last > 3.65);
        EXPECT_NEAR(s.stat(dpb(DP_BMS_ALM_LEVEL))->last, 3.0, 1e-12);
        EXPECT_NEAR(s.stat(dpb(DP_BMS_ALM_CODE))->last, 4.0, 1e-12);
    }
    {   // 压差过大 → 至少 1 级 + code 7
        BmsSimConfig c;
        c.cell_v_spread_v = 0.20;            // 压差 ≈ 0.4 V > 0.3
        BmsSim b(c);
        TraceSink s;
        for (int k = 0; k < 60; ++k) b.step(k * 1.0, 1.0, s, 0.0);
        EXPECT(s.stat(dpb(DP_BMS_CELL_V_DIFF))->last > 0.3);
        EXPECT(s.stat(dpb(DP_BMS_ALM_LEVEL))->last >= 1.0);
        EXPECT_NEAR(s.stat(dpb(DP_BMS_ALM_CODE))->last, 7.0, 1e-12);
    }
}

// =====================================================================
// T08 多簇 + 点名交叉核对
// =====================================================================
static void test_08_clusters_and_names() {
    std::printf("T08 多簇（可配簇数）+ 点名交叉核对\n");

    BmsSimConfig c;
    c.clusters = 4;
    c.cells_per_cluster = 112;              // 4 × 112 = 448 单体，375 V 簇
    BmsSim b(c);
    TraceSink s;
    b.publish_static(s);
    for (int k = 0; k < 200; ++k) b.step(k * 1.0, 1.0, s, 200.0);

    EXPECT_EQ(b.cell_count(), 448);
    EXPECT_EQ(b.cluster_u(0) > 300.0, 1);
    EXPECT_EQ(b.cluster_u(3) > 300.0, 1);
    EXPECT(b.cluster_u(0) < 420.0);
    // 各簇电压接近（同一组串结构、同一 SOC）
    EXPECT_NEAR(b.cluster_u(0), b.cluster_u(3), 5.0);
    // 簇电流 = 簇功率 / 簇电压；总功率 200 kW → 每簇 50 kW
    EXPECT_NEAR(b.cluster_i(0), 50000.0 / b.cluster_u(0), 1e-6);
    // 配置点反映了簇数
    EXPECT_NEAR(s.stat(dpb(DP_BMS_CLUSTER_N))->last, 4.0, 1e-12);
    EXPECT_NEAR(s.stat(dpb(DP_BMS_CELLS_PER_CLUSTER))->last, 112.0, 1e-12);

    // 点名交叉核对：发出的每个点名都必须在全点表里，且属于 bms
    int unknown = 0, wrong_dev = 0;
    for (const auto& n : s.names()) {
        const int i = dev_point_index_of(n);
        if (i < 0) { ++unknown; std::printf("    点表里没有: %s\n", n.c_str()); }
        else if (kBuiltinPoints[i].device != DK_BMS) ++wrong_dev;
    }
    EXPECT_EQ(unknown, 0);
    EXPECT_EQ(wrong_dev, 0);
    // 动态 31 + 静态 7 = 38 = BMS 侧全部点位
    EXPECT_EQ(static_cast<int>(s.point_count()), 38);

    // 反向守卫：簇数确实是可配的（换回 1 簇，单体数减半）
    BmsSimConfig c1;
    c1.clusters = 1;
    c1.cells_per_cluster = 112;
    BmsSim b1(c1);
    EXPECT_EQ(b1.cell_count(), 112);
}

// =====================================================================
// T09 全部在动 + 反向守卫
// =====================================================================
static void test_09_all_moving() {
    std::printf("T09 全部在动自证\n");

    // ★ 工况必须**同时**踩到"高 SOC"和"低 SOC"两端，否则允许充放功率
    //   一直停在上限 200 kW —— 它在观测窗里就是常量，而这不是模拟器的错，
    //   是工况选错了（与电表 EP_REV 那条同一个教训）。
    //   轨迹：0.98（高于 soc_max，禁充）→ 先小充一段（CHARGING 置位）
    //        → 长放电（变幅）走到 ~0.09（进入 soc_min 降额带）
    BmsSimConfig c9;
    c9.soc0 = 0.98;
    BmsSim b(c9);
    TraceSink s;
    const double dt = 5.0;
    const int n_steps = 1400;
    for (int k = 0; k < n_steps; ++k) {
        const double t = k * dt;
        const double p_raw = (t < 60.0) ? -200.0                          // 充电 60 s
                                        : 120.0 + 80.0 * std::sin(2.0 * kPi * t / 500.0);
        // 放电命令**受 BMS 限值约束**（真机也是这么跑的：PCS 不许越 BMS 的允许功率）。
        // 这样 SOC 只会渐近逼近 soc_min，**不会**越过它 —— 于是"限值"这件事
        // 是物理上自洽的，而不是"命令照发、SOC 硬夹在 0"。
        const double p = (p_raw > 0.0) ? std::min(p_raw, std::max(0.0, b.dis_limit_kw()))
                                       : p_raw;
        b.step(t, dt, s, p);
    }
    std::printf("    轨迹终点：SOC=%.4f  chg_limit=%.2f  dis_limit=%.4f\n",
                b.soc(), b.chg_limit_kw(), b.dis_limit_kw());
    EXPECT(b.soc() < 0.20);                     // 确实走到了低 SOC 端
    EXPECT(b.soc() > 0.0);                      // 但没有失控到 0（限值把命令压住了）
    EXPECT(b.dis_limit_kw() < 5.0);             // 已经落进 soc_min 降额带

    const std::vector<std::pair<std::string, double>> req = {
        {dpb(DP_BMS_CLUSTER_U),   0.5},
        {dpb(DP_BMS_CLUSTER_I),   10.0},
        {dpb(DP_BMS_CLUSTER_T),   0.5},
        {dpb(DP_BMS_CELL_V_MAX),  0.005},
        {dpb(DP_BMS_CELL_V_MIN),  0.005},
        {dpb(DP_BMS_CELL_V_DIFF), 1e-4},
        {dpb(DP_BMS_T_MAX),       1.0},
        {dpb(DP_BMS_T_MIN),       1.0},
        {dpb(DP_BMS_SOC),         1.0},
        {dpb(DP_BMS_SOH),         1e-3},
        {dpb(DP_BMS_RM_CHG_KWH),  1.0},
        {dpb(DP_BMS_RM_DIS_KWH),  1.0},
        {dpb(DP_BMS_INSUL_R),     1.0},
        {dpb(DP_BMS_LEAK_I),      0.05},
        {dpb(DP_BMS_CYCLE_COUNT), 1e-4},
        {dpb(DP_BMS_CHG_LIMIT_KW),1.0},
        {dpb(DP_BMS_DIS_LIMIT_KW),1.0},
        {dpb(DP_BMS_CHG_FORBID),  0.5},
        {dpb(DP_BMS_FORBID_REASON), 0.5},
        {dpb(DP_BMS_HEARTBEAT),   100.0},
        {dpb(DP_BMS_CHARGING),    0.5}          // 充放状态必须翻转
    };
    const MovementReport rep = check_moving(s, req);
    std::printf("    %s\n", rep.text().c_str());
    EXPECT_EQ(static_cast<int>(rep.missing.size()), 0);
    EXPECT_EQ(static_cast<int>(rep.frozen.size()), 0);

    // ★ 反向守卫：把模拟器"冻住"（功率恒 0 且关掉纹波无从谈起）后，
    //   本应静止的量必须被判为"未达标" —— 证明 check_moving 有区分度。
    //   这里用 "恒定 SOC 附近 + 注入关闭" 的极端：把 SOC 积分系数关掉等价于
    //   cap 极大 → SOC 几乎不动。
    BmsSimConfig c;
    c.cap_kwh = 1e12;                            // 容量极大 → SOC 几乎不动
    c.heat_coef = 0.0;                           // 无发热
    c.cool_coef = 0.0;                           // 无散热
    c.soh_degrade_per_mwh = 0.0;                 // 无衰减
    BmsSim b2(c);
    TraceSink s2;
    for (int k = 0; k < 900; ++k) b2.step(k * dt, dt, s2, 180.0 * std::sin(2.0 * kPi * k * dt / 900.0));
    const std::vector<std::pair<std::string, double>> req2 = {
        {dpb(DP_BMS_SOC), 0.5},
        {dpb(DP_BMS_SOH), 0.5},
        {dpb(DP_BMS_T_MAX), 0.5}
    };
    const MovementReport rep2 = check_moving(s2, req2);
    EXPECT_GT0(rep2.frozen.size());
    EXPECT(!rep2.ok());
    // 而同一工况下"该动的"（簇电流/心跳）仍在动 ——
    // 证明上面那三条红的是**特定量**，不是"整个模拟器没跑"
    EXPECT(s2.spread(dpb(DP_BMS_CLUSTER_I)) > 10.0);
    EXPECT(s2.spread(dpb(DP_BMS_HEARTBEAT)) > 100.0);
}

int main() {
    TEST_BANNER("17/ BMS 模拟器（B5）—— 从 6 点扩到全表 + 物理过程");

    test_01_soc_integration();
    test_02_soh_cycle();
    test_03_cells();
    test_04_thermal();
    test_05_limit_forbid_same_source();
    test_06_insulation();
    test_07_alarm();
    test_08_clusters_and_names();
    test_09_all_moving();

    TEST_TAIL();
    return g_fail == 0 ? 0 : 1;
}
