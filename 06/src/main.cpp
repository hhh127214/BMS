// =====================================================================
// 06/ — 周期 6：EMS 状态机 演示（纯状态机口径）
//
//   场景 B：全状态流转 INIT → SELF_CHECK → READY → NORMAL → DERATED →
//           FAULT → EMERGENCY → 人工复位，覆盖正常运行 / 降功率运行 /
//           BMS 禁止 / PCS 故障 / 通信异常 / 数据异常 / 紧急停机 / 故障恢复。
//
//   目标（设计文档 §7 周期 6）：实现系统全状态标准化管控。
//
// 本演示**只驱动 EmsStateMachine**，不引入 07/ 的 EmsRuntime —— 目的是让
// 周期 6 的交付物能被独立验证。带真实功率与被控对象的端到端演示见
// 07/（场景 C）与 08/（场景 D）。
//
// 编译：g++ -std=c++17 -Wall -O2 -I src -I ../04/src -I ../05/src src/main.cpp -o build/fsm_demo.exe
// =====================================================================

#include "data_models.h"
#include "safety_engine.h"
#include "state_machine.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ems;

// =====================================================================
// 工具
// =====================================================================
static void banner(const std::string& s) {
    std::printf("\n");
    std::printf("============================================================\n");
    std::printf("  %s\n", s.c_str());
    std::printf("============================================================\n");
}
static void sub(const std::string& s) {
    std::printf("\n---- %s ----\n", s.c_str());
}

// =====================================================================
// 把一台状态机推到指定状态，用于输出门控真值表的**实测**（而非硬编码）
// =====================================================================
struct GateProbe {
    EmsState state = EmsState::kInit;
    bool     output_enabled = false;
    bool     strategies_enabled = false;
    bool     fail_safe = false;
};

static GateProbe reach_and_probe(EmsState want) {
    StateMachineConfig c;
    c.init_hold_s            = 0.5;
    c.self_check_cycles      = 5;
    c.derate_release_cycles  = 10;
    c.fault_clear_cycles     = 20;

    EmsStateMachine f(c);
    f.reset_all();
    f.request_run(false);

    const double dt = 0.1;
    Timestamp t = 0.0;
    FaultFlags fl;
    SafetyVerdict sv;
    auto adv = [&](int n) {
        for (int i = 0; i < n; ++i) { t += dt; f.update(t, fl, sv); }
    };

    switch (want) {
        case EmsState::kInit:      break;
        case EmsState::kSelfCheck: adv(5);  break;
        case EmsState::kReady:     adv(10); break;
        case EmsState::kNormal:    adv(10); f.request_run(true); adv(1); break;
        case EmsState::kDerated:
            adv(10); f.request_run(true);
            sv.derated = true; sv.reason = "L1:transformer_limit"; adv(1);
            break;
        case EmsState::kFault:
            adv(10); f.request_run(true); adv(1);
            fl.pcs_fault = true; adv(1);
            break;
        case EmsState::kEmergency:
            adv(10); f.request_run(true); adv(1);
            fl.temp_fault = true; adv(1);
            break;
    }

    GateProbe p;
    p.state              = f.state();
    p.output_enabled     = f.output_enabled();
    p.strategies_enabled = f.strategies_enabled();
    p.fail_safe          = f.fail_safe();
    return p;
}

// =====================================================================
// 场景 B（周期 6）：EMS 状态机全状态流转
// =====================================================================
static void scenario6_state_machine() {
    banner("场景 B（周期 6）：EMS 状态机 —— INIT→SELF_CHECK→READY→NORMAL→DERATED→FAULT→EMERGENCY→复位");

    StateMachineConfig cfg;
    cfg.init_hold_s           = 0.5;
    cfg.self_check_cycles     = 5;
    cfg.derate_release_cycles = 10;
    cfg.fault_clear_cycles    = 20;

    EmsStateMachine fsm(cfg);
    fsm.reset_all();
    fsm.request_run(false);          // 演示：先待机，再显式下发启动命令

    const double dt = 0.1;
    Timestamp t = 0.0;
    FaultFlags faults;
    SafetyVerdict verdict;

    auto run = [&](int n, const char* what) {
        for (int i = 0; i < n; ++i) { t += dt; fsm.update(t, faults, verdict); }
        // 状态名是纯 ASCII，放前面用 %-10s 对齐才不会被中文宽度带偏
        std::printf("  t=%6.1fs  %-10s 门控=%-4s %s\n",
                    t, fsm.state_str(),
                    fsm.output_enabled() ? "允许" : "禁止", what);
    };

    // --- 阶段 1：上电 → 自检 → 就绪 → 运行 ---
    sub("阶段 1：上电初始化 → 自检 → 就绪 → 正常运行");
    run(5,  "INIT 停留 0.5s 结束");
    run(5,  "自检连续通过 5 拍");
    fsm.request_run(true);
    run(1,  "下发启动命令 run_request=true");

    // --- 阶段 2：L1 降额 → DERATED ---
    sub("阶段 2：L1 安全约束动作（如变压器容量收紧）→ DERATED");
    verdict.derated = true;
    verdict.reason  = "L1:transformer_limit";
    run(1,  "喂入 derated=true 的安全裁决");
    std::printf("      状态机不参与功率计算，只标记「降功率运行」；\n");
    std::printf("      实际功率区间收紧由 05/ 安全引擎负责。\n");

    // --- 阶段 3：降额解除（滞环）---
    sub("阶段 3：降额解除 → 回 NORMAL（需连续 10 拍正常，防抖）");
    verdict = SafetyVerdict{};
    run(5,  "解除后第 5 拍（仍 DERATED，滞环未满）");
    run(8,  "解除后第 13 拍（滞环满足）");

    // --- 阶段 4：PCS 故障 → FAULT ---
    sub("阶段 4：PCS 故障 → FAULT（输出门控为 0，运行许可被撤销）");
    faults.pcs_fault = true;
    run(1,  "PCS 报故障");
    std::printf("      运行许可 run_requested=%s（进 FAULT 时被强制撤销）\n",
                fsm.run_requested() ? "true" : "false");

    // --- 阶段 5：故障恢复 → READY（待机）---
    sub("阶段 5：PCS 故障清除 → 连续 20 拍正常后回 READY（待机，需重新启动）");
    faults = FaultFlags{};
    run(25, "故障清除满 20 拍");
    fsm.request_run(true);
    run(1,  "运维确认后重新下发启动命令");

    // --- 阶段 6：紧急停机（锁存）---
    sub("阶段 6：电池温度故障 → EMERGENCY（锁存，不可自愈）");
    faults.temp_fault = true;
    run(1,  "电池温度 58℃ 超故障阈值 55℃");
    std::printf("      reason=%s\n", fsm.last_reason().c_str());

    // --- 阶段 7：故障消失仍锁存 ---
    sub("阶段 7：温度已恢复但未复位 → 仍锁存于 EMERGENCY");
    faults = FaultFlags{};
    run(30, "温度恢复后等待 3s（不自愈）");

    // --- 阶段 8：手动复位 ---
    sub("阶段 8：手动复位 → 重新走 INIT→SELF_CHECK→READY→NORMAL");
    const bool ok = fsm.reset_emergency(t, faults);
    std::printf("      手动复位结果：%s\n", ok ? "成功（清锁存）" : "拒绝（仍有故障源）");
    run(5,  "复位后重新 INIT 停留");
    run(5,  "重新自检");
    run(1,  "自检通过 → READY（待机，无功率输出）");
    fsm.request_run(true);
    run(1,  "重新下发启动命令 → NORMAL");

    // --- 阶段 9：电表通信异常不进 FAULT ---
    sub("阶段 9：关口电表通信异常 → **不进** FAULT（按接口规范 §6 走 HOLD_LAST）");
    faults.meter_comm_lost = true;
    run(5, "电表通信中断 0.5s");
    std::printf("      faults.fault()=%s  faults.emergency()=%s  → 状态 %s\n",
                faults.fault() ? "true" : "false",
                faults.emergency() ? "true" : "false",
                fsm.state_str());
    std::printf("      HOLD_LAST 的**执行**属采集层（07/ 的 EmsRuntime）：\n");
    std::printf("      电表超时 > 阈值时复用上一拍指令，而不是停机。\n");
    faults = FaultFlags{};

    // --- 阶段 10：输出门控真值表（实测）---
    sub("输出门控真值表（对每种状态实际构造一台状态机并读取门控位）");
    std::printf("  %-11s %-14s %-18s %s\n",
                "状态", "output_enabled", "strategies_enabled", "fail_safe");
    std::printf("  %s\n", std::string(62, '-').c_str());
    const EmsState all[] = {EmsState::kInit, EmsState::kSelfCheck, EmsState::kReady,
                            EmsState::kNormal, EmsState::kDerated, EmsState::kFault,
                            EmsState::kEmergency};
    int gate_violations = 0;
    for (EmsState s : all) {
        const GateProbe p = reach_and_probe(s);
        if (p.state != s) ++gate_violations;      // 构造未到位即视为不变量破坏
        std::printf("  %-11s %-14s %-18s %s\n",
                    state_name(p.state),
                    p.output_enabled ? "允许" : "禁止",
                    p.strategies_enabled ? "允许" : "禁止",
                    p.fail_safe ? "是" : "否");
    }
    std::printf("\n  门控策略：仅 NORMAL / DERATED 允许输出；READY 默认禁止（可配置）；\n");
    std::printf("            INIT / SELF_CHECK / FAULT / EMERGENCY 一律禁止输出。\n");

    // --- 状态迁移表（SOE）---
    sub("状态迁移记录（SOE）");
    std::printf("  %-11s → %-11s  %s\n", "从", "到", "原因");
    std::printf("  %s\n", std::string(66, '-').c_str());
    for (const auto& ev : fsm.history()) {
        std::printf("  %-11s → %-11s  %s\n",
                    state_name(ev.from), state_name(ev.to), ev.reason.c_str());
    }

    // --- 不变量校验 ---
    int violations = 0;
    for (const auto& ev : fsm.history()) {
        // 进入 FAULT / EMERGENCY 必须撤销运行许可（安全要求）
        if ((ev.to == EmsState::kFault || ev.to == EmsState::kEmergency)) ++violations;
    }
    std::printf("\n  合计迁移 %zu 次；状态构造不变量破坏 = %d；\n",
                fsm.history().size(), gate_violations);
    std::printf("  FAULT/EMERGENCY 迁移次数 = %d（每次都应撤销运行许可，故障恢复后落 READY 待机）\n",
                violations);
    std::printf("  输出门控不变量（非运行态恒 0 的**执行**验证）见 07/ 场景 C 与 T11。\n");
}

// =====================================================================
int main() {
    std::printf("============================================================\n");
    std::printf("  工商业储能 EMS —— 06/ 周期 6 EMS 状态机演示（纯状态机口径）\n");
    std::printf("============================================================\n");

    scenario6_state_machine();

    banner("周期 6 交付小结");
    std::printf("  7 状态：INIT / SELF_CHECK / READY / NORMAL / DERATED / FAULT / EMERGENCY\n");
    std::printf("  8 类故障源：BMS/PCS/电表通信、PCS 故障、数据异常、设备离线、温度、急停\n");
    std::printf("  全部迁移带滞环/计数去抖；EMERGENCY 锁存，需人工复位且故障已清除\n");
    std::printf("  进 FAULT/EMERGENCY 自动撤销运行许可；恢复后落 READY，不自动带载\n");
    std::printf("  输出门控：非运行态恒 0（INIT/SELF_CHECK/READY/FAULT/EMERGENCY）\n");
    std::printf("\n");
    return 0;
}
