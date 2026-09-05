// 防逆流控制器单元测试（覆盖修复项1~7的关键逻辑）
#include <cmath>
#include <iostream>
#include <stdexcept>
#include "AntiReverseController.h"

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

static AntiReverseConfig testCfg()
{
    AntiReverseConfig c = {0.5, 0.05, 1.0, 0.1, 2.0, 4.0, 0.0, 60000.0, -60000.0};
    return c;
}

// ---- 测试1：配置校验（修复项2） ----
static void test_config_validation()
{
    AntiReverseConfig c = testCfg();
    c.deadband_exit = 1.0;                       // exit<=enter → 非法
    bool threw = false;
    try { AntiReverseController ctrl(c); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw, "配置校验：deadband_exit<=deadband_enter 抛异常");

    c = testCfg();
    c.Ts = 0.0;
    threw = false;
    try { AntiReverseController ctrl(c); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw, "配置校验：Ts<=0 抛异常");

    c = testCfg();
    c.integral_max = c.integral_min - 1.0;
    threw = false;
    try { AntiReverseController ctrl(c); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw, "配置校验：integral_max<=integral_min 抛异常");

    AntiReverseController ctrl(testCfg());       // 合法配置不抛
    CHECK(true, "配置校验：合法配置构造成功");
}

// ---- 测试2：死区迟滞 进入/冻结/退出（修复项1） ----
static void test_deadband_hysteresis()
{
    // 首拍 e=0 → |e|<enter → 进入死区，冻结输出0
    AntiReverseController c(testCfg());
    double out = c.update(0.0, 50.0, 200.0, true);
    CHECK(nearly(out, 0.0), "死区：首拍 e=0 进入死区输出0");

    // 死区内 e=±1 → 保持冻结0（不更新积分）
    out = c.update(1.0, 50.0, 200.0, true);
    CHECK(nearly(out, 0.0), "死区：e=-1 保持冻结0");
    out = c.update(-1.0, 50.0, 200.0, true);
    CHECK(nearly(out, 0.0), "死区：e=+1 保持冻结0");

    // 构造有积分状态：e=100 → 输出Kp*e=50, integral=100
    AntiReverseController c2(testCfg());
    out = c2.update(-100.0, 50.0, 200.0, true);
    CHECK(nearly(out, 50.0), "正常调节：e=100 输出Kp*e=50");
    out = c2.update(0.0, 50.0, 200.0, true);     // e=0 → 进入死区冻结50
    CHECK(nearly(out, 50.0), "死区：e=0 进入死区冻结上一拍50");
    out = c2.update(-1.0, 50.0, 200.0, true);    // e=1，|e|<exit → 仍冻结50
    CHECK(nearly(out, 50.0), "死区：e=1 仍冻结50");
    // e=5，|e|>exit → 退出死区恢复PI：out=Kp*5+Ki*Ts*integral=2.5+0.5=3.0
    out = c2.update(-5.0, 50.0, 200.0, true);
    CHECK(nearly(out, 3.0), "死区：|e|>exit 退出后恢复PI(out=Kp*e+Ki*Ts*integral)");

    // 安全侧同样退出死区（锁死修复的关键：|e|>exit 双向退出）
    AntiReverseController c3(testCfg());
    c3.update(-100.0, 50.0, 200.0, true);        // 充电50
    c3.update(0.0, 50.0, 200.0, true);           // 进死区冻结50
    out = c3.update(60.0, 50.0, 200.0, true);    // e=-60,|e|>exit → 退出 → 输出钳位0
    CHECK(nearly(out, 0.0), "死区：安全侧 |e|>exit 同样退出死区并输出0(锁死已修复)");
}

// ---- 测试3：遇限削弱积分（上限饱和不累加，设计文档3.3） ----
static void test_anti_windup_high()
{
    AntiReverseController c(testCfg());
    double out = 0.0;
    for (int i = 0; i < 100; ++i)
        out = c.update(-100.0, 50.0, 20.0, true);   // e=100，P_chg_max=20 → 饱和
    CHECK(nearly(out, 20.0), "抗饱和：上限饱和输出=P_chg_max=20");
    // 若积分未被累加（integral=0），放宽限值后输出=Kp*e=50
    out = c.update(-100.0, 50.0, 200.0, true);
    CHECK(nearly(out, 50.0), "抗饱和：饱和期积分未累加(放宽限值后输出=Kp*e=50)");
}

// ---- 测试4：积分硬限幅（设计文档3.3） ----
static void test_integral_clamp()
{
    AntiReverseConfig cfg = testCfg();
    cfg.integral_max = 100.0;
    cfg.integral_min = -100.0;
    AntiReverseController c(cfg);
    for (int i = 0; i < 10000; ++i)
        c.update(-100.0, 50.0, 200.0, true);        // e=100，输出50不饱和 → 持续累加
    double out = c.update(-100.0, 50.0, 200.0, true);
    // P_int=Ki*Ts*integral_max=0.05*0.1*100=0.5 → out=Kp*100+0.5=50.5
    CHECK(nearly(out, 50.5), "积分硬限幅：integral 被钳位在100(输出=Kp*e+Ki*Ts*100)");
}

// ---- 测试5：charge_enabled 复位与恢复首拍无前馈尖峰（设计文档3.5/6） ----
static void test_charge_enable_reset()
{
    AntiReverseController c(testCfg());
    c.update(-100.0, 200.0, 200.0, true);           // e=100 → out=50, integral=100
    c.update(-100.0, 200.0, 200.0, true);           // integral=200
    double out = c.update(-100.0, 50.0, 200.0, false);  // 禁用充电
    CHECK(nearly(out, 0.0), "使能：charge_enabled=false 输出强制0");
    // 恢复首拍：光伏历史已重新初始化 → P_ff=Kff*(200-200)/Ts=0 → 无前馈尖峰
    out = c.update(-100.0, 200.0, 200.0, true);
    CHECK(nearly(out, 50.0), "使能：恢复首拍无前馈尖峰(输出=Kp*e=50)");
}

// ---- 测试6：输出钳位0时积分保留（波动场景充电跟随不退化） ----
static void test_low_clamp_integral_preserved()
{
    AntiReverseController c(testCfg());
    for (int i = 0; i < 50; ++i)
        c.update(-100.0, 50.0, 200.0, true);        // integral≈5000, P_int≈25
    // 安全侧大误差：e=-60 → out=clamp(-30+25)=0（下限饱和），积分应被冻结而非清零
    for (int i = 0; i < 5; ++i)
        c.update(60.0, 50.0, 200.0, true);
    // 回充相位 e=+2：若积分保留，out=Kp*2+P_int=1.0+25=26（若清零则只有1.0）
    double out = c.update(-2.0, 50.0, 200.0, true);
    CHECK(nearly(out, 26.0), "积分保留：输出钳位0时积分不清零(充电跟随不退化)");
}

// ---- 测试7：陈旧积分清零——安全侧持续过充3拍（修复项7） ----
static void test_safe_overcharge_integral_reset()
{
    AntiReverseController c(testCfg());
    for (int i = 0; i < 50; ++i)
        c.update(-100.0, 50.0, 200.0, true);        // integral≈5000, P_int≈25
    c.update(40.0, 50.0, 200.0, true);              // e=-40 → out=clamp(-20+25)=5>0, cnt=1
    c.update(40.0, 50.0, 200.0, true);              // cnt=2
    c.update(40.0, 50.0, 200.0, true);              // cnt=3 → 积分清零
    double out = c.update(-2.0, 50.0, 200.0, true); // 积分已清零 → out=1.0
    CHECK(nearly(out, 1.0), "陈旧积分：安全侧持续过充3拍后积分清零(输出=Kp*e=1.0)");
}

// ---- 测试8：reset() ----
static void test_reset()
{
    AntiReverseController c(testCfg());
    for (int i = 0; i < 10; ++i)
        c.update(-100.0, 50.0, 200.0, true);
    c.reset();
    double out = c.update(-2.0, 50.0, 200.0, true); // 重置后首拍：e=2 → out=Kp*2=1.0
    CHECK(nearly(out, 1.0), "reset：重置后从零开始(输出=Kp*e=1.0)");
}

int main()
{
    std::cout << "=== AntiReverseController unit tests ===\n";
    test_config_validation();
    test_deadband_hysteresis();
    test_anti_windup_high();
    test_integral_clamp();
    test_charge_enable_reset();
    test_low_clamp_integral_preserved();
    test_safe_overcharge_integral_reset();
    test_reset();

    if (failures == 0)
    {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cout << failures << " TEST(S) FAILED\n";
    return 1;
}

