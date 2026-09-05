// 需量管理控制器单元测试（滑动窗口算法，对应设计文档7.1修订版用例）
#include <cmath>
#include <iostream>
#include <stdexcept>
#include "DemandController.h"

static int failures = 0;

#define CHECK(cond, msg)                                                \
    do {                                                                \
        if (cond) { std::cout << "PASS: " << msg << "\n"; }             \
        else     { std::cout << "FAIL: " << msg << "\n"; ++failures; }  \
    } while (0)

static bool nearly(double a, double b, double tol = 1e-6)
{
    return std::fabs(a - b) < tol;
}

static DemandController::Config testCfg()
{
    DemandController::Config c;
    c.T_window = 10.0;   // N = round(10/0.1) = 100
    c.D_target = 250.0;
    c.dP_max = 50.0;
    c.Kp_avg = 100.0;
    c.Ki = 20.0;
    c.integral_max = 5000.0;
    c.Ts = 0.1;
    return c;
}

// 连续喂入同一P_grid，返回最后一次输出
static double feed(DemandController& d, double value, int count,
                   double P_dis_max = 150.0, bool en = true, bool comm = false)
{
    double out = 0.0;
    for (int i = 0; i < count; ++i)
        out = d.update(value, P_dis_max, en, comm);
    return out;
}

// ---- 测试1：配置校验 ----
static void test_config_validation()
{
    DemandController::Config c = testCfg();
    c.T_window = 0.0;
    bool threw = false;
    try { DemandController d(c); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw, "配置校验：T_window=0 抛异常");

    c = testCfg();
    c.D_target = 0.0;
    threw = false;
    try { DemandController d(c); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw, "配置校验：D_target=0 抛异常");

    c = testCfg();
    c.Ts = 0.0;
    threw = false;
    try { DemandController d(c); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw, "配置校验：Ts=0 抛异常");

    c = testCfg();
    c.Kp_avg = 0.0;
    threw = false;
    try { DemandController d(c); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw, "配置校验：Kp_avg=0 抛异常");

    c = testCfg();
    c.Ki = 0.0;
    threw = false;
    try { DemandController d(c); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw, "配置校验：Ki=0 抛异常");

    DemandController d(testCfg());
    CHECK(true, "配置校验：合法配置构造成功");
}

// ---- 测试2：UT-01 无越限风险 ----
static void test_no_risk()
{
    DemandController d(testCfg());
    double out = feed(d, 200.0, 100);   // 窗口=100×200，平均200<250
    CHECK(nearly(out, 0.0), "UT-01：窗口全200（平均200<250）→ 输出0");
    CHECK(nearly(d.getWindowAverage(), 200.0), "UT-01：窗口平均=200");
    CHECK(!d.isActive(), "UT-01：控制器未激活");
    CHECK(!d.hasAlarm(), "UT-01：无告警");
}

// ---- 测试3：平均误差触发——窗口平均超过250时开始放电 ----
static void test_trigger()
{
    DemandController d(testCfg());
    feed(d, 200.0, 100);                        // 窗口=100×200
    double out = feed(d, 300.0, 51);            // 第51个300后A_prev=250，输出0
    CHECK(nearly(out, 0.0), "触发：窗口平均250时输出0(e=0)");
    out = feed(d, 300.0, 1);                    // A_prev=251 → P_req=Kp·1=100 → 限速5
    CHECK(nearly(out, 5.0), "触发：窗口平均>250时开始放电(限速5kW/拍)");
    CHECK(d.isActive(), "触发：控制器激活");
}

// ---- 测试4：UT-03 禁止放电 ----
static void test_discharge_disabled()
{
    DemandController d(testCfg());
    feed(d, 200.0, 100);
    double out = d.update(300.0, 150.0, false, false);
    CHECK(nearly(out, 0.0), "UT-03：禁止放电 → 输出0");
    // 窗口仍记录实际功率（P_grid=300 有效计量）
    CHECK(nearly(d.getWindowAverage(), 201.0), "UT-03：禁止放电时窗口仍记录实际功率");
}

// ---- 测试5：UT-04 通信异常 ----
static void test_comm_fault()
{
    DemandController d(testCfg());
    feed(d, 200.0, 100);
    double out = d.update(999.0, 150.0, true, true);   // 通信异常
    CHECK(nearly(out, 0.0), "UT-04：通信异常 → 输出0");
    CHECK(d.hasAlarm(), "UT-04：通信异常置告警");
    CHECK(nearly(d.getWindowAverage(), 200.0), "UT-04：无效样本不入窗(窗口未被污染)");
}

// ---- 测试6：已越限（窗口平均超目标）→ 告警 + 满功率放电 ----
static void test_over_limit()
{
    DemandController d(testCfg());
    feed(d, 300.0, 100);                        // 窗口=100×300，平均=300
    CHECK(nearly(d.getWindowAverage(), 300.0), "已越限：窗口平均=300");
    CHECK(d.hasAlarm(), "已越限：置告警");
    double out = d.update(300.0, 150.0, true, false);
    CHECK(nearly(out, 150.0), "已越限：满功率放电(公式饱和到P_dis_max)");
}

// ---- 测试7：UT-06 速率限制（每拍上坡≤5kW，下坡快速退出） ----
static void test_rate_limit()
{
    DemandController d(testCfg());
    feed(d, 300.0, 100, 10.0);                  // P_dis_max=10 → 输出爬升并停在10
    double o1 = d.update(300.0, 150.0, true, false);
    CHECK(nearly(o1, 15.0), "速率限制：从10增至15(每拍≤5)");
    double o2 = d.update(300.0, 150.0, true, false);
    CHECK(nearly(o2, 20.0), "速率限制：从15增至20(每拍≤5)");

    // 下坡快速退出：喂低负载直到窗口平均降至250以下
    // 一旦平均误差变负（A<250），输出应一拍内归0（不受下坡限速）
    bool dropped = false;
    double prev = o2;
    double out = o2;
    for (int i = 0; i < 60; ++i)
    {
        out = d.update(200.0, 150.0, true, false);
        if (prev > 0.0 && out == 0.0) dropped = true;
        prev = out;
    }
    CHECK(dropped, "速率限制：下坡一拍内归0(未受下坡限速)");
    CHECK(nearly(out, 0.0), "速率限制：负荷回落后输出0");
    CHECK(d.getWindowAverage() < 250.0, "速率限制：回落后窗口平均<250");
}

// ---- 测试8：reset ----
static void test_reset()
{
    DemandController d(testCfg());
    feed(d, 300.0, 100);
    d.reset();
    CHECK(nearly(d.getWindowAverage(), 0.0), "reset：窗口清空");
    CHECK(!d.hasAlarm() && !d.isActive(), "reset：状态复位");
    double out = d.update(200.0, 150.0, true, false);
    CHECK(nearly(out, 0.0), "reset：重置后正常调节");
}

// ---- 测试9：启动阶段（窗口未满）----
static void test_startup()
{
    DemandController d(testCfg());
    double out = d.update(200.0, 150.0, true, false);   // 空窗A=0<250，e<0
    CHECK(nearly(out, 0.0), "启动：首拍低负载输出0");
    DemandController d2(testCfg());
    out = d2.update(300.0, 150.0, true, false);         // 空窗A=0<250，e<0，输出0
    CHECK(nearly(out, 0.0), "启动：首拍高负载输出0(窗口未满，平均低于目标)");
}

// ---- 测试10：滑动窗口滚动平均精确性 ----
static void test_window_rolling()
{
    DemandController d(testCfg());
    feed(d, 200.0, 100);
    feed(d, 300.0, 100);                        // 窗口全部滚成300
    CHECK(nearly(d.getWindowAverage(), 300.0), "窗口平均：滚动至全300精确");
    feed(d, 200.0, 100);                        // 再全部滚成200
    CHECK(nearly(d.getWindowAverage(), 200.0), "窗口平均：再滚动至全200精确");
}

int main()
{
    std::cout << "=== DemandController unit tests ===\n";
    test_config_validation();
    test_no_risk();
    test_trigger();
    test_discharge_disabled();
    test_comm_fault();
    test_over_limit();
    test_rate_limit();
    test_reset();
    test_startup();
    test_window_rolling();

    if (failures == 0)
    {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cout << failures << " TEST(S) FAILED\n";
    return 1;
}
