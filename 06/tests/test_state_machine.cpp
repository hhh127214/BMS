// =====================================================================
// 06/ 单元测试（assert 风格，单文件可编译）—— 周期 6 EMS 状态机
//
//   T07: INIT→SELF_CHECK→READY→NORMAL 标准启动
//   T08: DERATED 进入/解除（滞环）
//   T09: FAULT 进入/恢复 + 运行许可撤销
//   T10: EMERGENCY 锁存 + 人工复位
//
// 本文件**只依赖 06/src/state_machine.h**（及其上游 05/ 的 SafetyVerdict），
// 不引入 07/ 的实时闭环 —— 保证周期 6 的交付物可独立验证。
// 带真实功率/被控对象的门控不变量验证见 07/tests（T11）。
//
// 编译：g++ -std=c++17 -Wall -O2 -I src -I ../04/src -I ../05/src
//           tests/test_state_machine.cpp -o build/test_state_machine.exe
// =====================================================================

#include "data_models.h"
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

// =====================================================================
// T07: 标准启动路径
// =====================================================================
static void test_07_state_machine_startup() {
    std::cerr << "[T07] 状态机标准启动 ...\n";
    StateMachineConfig cfg;
    cfg.init_hold_s = 0.0;
    cfg.self_check_cycles = 1;
    EmsStateMachine fsm(cfg);
    fsm.reset_all();
    EXPECT(fsm.state() == EmsState::kInit);
    EXPECT(!fsm.output_enabled());
    EXPECT(!fsm.strategies_enabled());

    FaultFlags none;
    fsm.update(0.0, none, SafetyVerdict{});
    EXPECT(fsm.state() == EmsState::kSelfCheck);
    fsm.update(0.1, none, SafetyVerdict{});
    EXPECT(fsm.state() == EmsState::kReady);
    EXPECT(!fsm.output_enabled());      // READY 默认不允许输出
    EXPECT(fsm.strategies_enabled());   // 但允许评估

    fsm.request_run(true);
    fsm.update(0.2, none, SafetyVerdict{});
    EXPECT(fsm.state() == EmsState::kNormal);
    EXPECT(fsm.output_enabled());
    EXPECT(fsm.history().size() >= 3);
}

// =====================================================================
// T08: DERATED 进入/解除滞环
// =====================================================================
static void test_08_derate_enter_release() {
    std::cerr << "[T08] DERATED 进入/解除滞环 ...\n";
    StateMachineConfig cfg;
    cfg.init_hold_s = 0.0;
    cfg.self_check_cycles = 1;
    cfg.derate_release_cycles = 4;
    EmsStateMachine fsm(cfg);
    fsm.reset_all();
    fsm.request_run(true);
    FaultFlags none;
    for (int i = 0; i < 3; ++i) fsm.update(i * 0.1, none, SafetyVerdict{});
    EXPECT(fsm.state() == EmsState::kNormal);

    SafetyVerdict derated;
    derated.derated = true;
    derated.reason  = "soc_warn_high";
    fsm.update(1.0, none, derated);
    EXPECT(fsm.state() == EmsState::kDerated);
    EXPECT(fsm.output_enabled());       // 降额仍可输出

    // 解除需连续 N 拍
    SafetyVerdict ok;
    for (int i = 0; i < cfg.derate_release_cycles - 1; ++i) {
        fsm.update(1.1 + i * 0.1, none, ok);
        EXPECT(fsm.state() == EmsState::kDerated);
    }
    fsm.update(2.0, none, ok);
    EXPECT(fsm.state() == EmsState::kNormal);
}

// =====================================================================
// T09: FAULT 进入/恢复 + 运行许可撤销
// =====================================================================
static void test_09_fault_enter_recover() {
    std::cerr << "[T09] FAULT 进入/恢复 + 许可撤销 ...\n";
    StateMachineConfig cfg;
    cfg.init_hold_s = 0.0;
    cfg.self_check_cycles = 1;
    cfg.fault_clear_cycles = 3;
    EmsStateMachine fsm(cfg);
    fsm.reset_all();
    fsm.request_run(true);
    FaultFlags none;
    for (int i = 0; i < 3; ++i) fsm.update(i * 0.1, none, SafetyVerdict{});
    EXPECT(fsm.state() == EmsState::kNormal);
    EXPECT(fsm.run_requested());

    FaultFlags pcs_fault;
    pcs_fault.pcs_fault = true;
    fsm.update(1.0, pcs_fault, SafetyVerdict{});
    EXPECT(fsm.state() == EmsState::kFault);
    EXPECT(!fsm.output_enabled());
    EXPECT(!fsm.run_requested());       // 安全要求：撤销运行许可

    // 故障消失后需连续 N 拍 → 回 READY（需重新下发启动命令）
    for (int i = 0; i < cfg.fault_clear_cycles; ++i)
        fsm.update(2.0 + i * 0.1, none, SafetyVerdict{});
    EXPECT(fsm.state() == EmsState::kReady);
    fsm.update(3.0, none, SafetyVerdict{});
    EXPECT(fsm.state() == EmsState::kReady);   // 未重新下发 → 停在 READY
    fsm.request_run(true);
    fsm.update(3.1, none, SafetyVerdict{});
    EXPECT(fsm.state() == EmsState::kNormal);
}

// =====================================================================
// T10: EMERGENCY 锁存 + 人工复位
// =====================================================================
static void test_10_emergency_latch_and_reset() {
    std::cerr << "[T10] EMERGENCY 锁存 + 人工复位 ...\n";
    StateMachineConfig cfg;
    cfg.init_hold_s = 0.0;
    cfg.self_check_cycles = 1;
    EmsStateMachine fsm(cfg);
    fsm.reset_all();
    fsm.request_run(true);
    FaultFlags none;
    for (int i = 0; i < 3; ++i) fsm.update(i * 0.1, none, SafetyVerdict{});
    EXPECT(fsm.state() == EmsState::kNormal);

    SafetyVerdict emg;
    emg.emergency = true;
    fsm.update(1.0, none, emg);
    EXPECT(fsm.state() == EmsState::kEmergency);
    EXPECT(!fsm.output_enabled());

    // 故障源消失也不会自动退出（锁存）
    for (int i = 0; i < 50; ++i) fsm.update(2.0 + i * 0.1, none, SafetyVerdict{});
    EXPECT(fsm.state() == EmsState::kEmergency);

    // 仍有故障源时拒绝复位
    FaultFlags still_bad;
    still_bad.pcs_fault = true;
    EXPECT(!fsm.reset_emergency(10.0, still_bad));
    EXPECT(fsm.state() == EmsState::kEmergency);

    // 故障源清除后复位 → 回 INIT 重走自检
    EXPECT(fsm.reset_emergency(11.0, none));
    EXPECT(fsm.state() == EmsState::kInit);

    // 显式急停同样锁存
    EmsStateMachine f2(cfg);
    f2.reset_all();
    f2.request_run(true);
    for (int i = 0; i < 3; ++i) f2.update(i * 0.1, none, SafetyVerdict{});
    f2.trigger_emergency_stop("manual_estop");
    f2.update(1.0, none, SafetyVerdict{});
    EXPECT(f2.state() == EmsState::kEmergency);
}


// =====================================================================
int main() {
    std::cerr << "=========================================\n"
              << " 06/ 周期 6 EMS 状态机 单元测试\n"
              << "=========================================\n";

    test_07_state_machine_startup();
    test_08_derate_enter_release();
    test_09_fault_enter_recover();
    test_10_emergency_latch_and_reset();

    std::cerr << "=========================================\n"
              << " PASS=" << g_pass << "  FAIL=" << g_fail << "\n"
              << "=========================================\n";
    return (g_fail == 0) ? 0 : 1;
}
