// =====================================================================
// 06/ — 周期 6：EMS 状态机 EmsStateMachine
//
// 依据：工商业储能EMS调控策略设计方案.md §7 周期 6
//   「搭建统一EMS状态流转体系：INIT → SELF_CHECK → READY → NORMAL →
//     DERATED → FAULT → EMERGENCY。定义正常运行、降功率运行、BMS禁止、
//     PCS故障、通信异常、数据异常、紧急停机、故障恢复等全场景状态逻辑。」
//
// 设计要点：
//   1. 状态机只做"状态判定 + 输出门控"，不参与功率计算（功率由仲裁器算）。
//   2. 所有判定都带滞环/计数去抖，避免状态在边界抖动（工程必备）。
//   3. EMERGENCY 是**锁存**态：必须显式 reset() 且故障已清除才能回到 INIT。
//   4. 每次迁移写一条 SOE（事件顺序记录），与 02/ 的 SOE 风格一致。
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "data_models.h"
#include "safety_engine.h"

#include <string>
#include <vector>

namespace ems {

// =====================================================================
// 状态枚举
// =====================================================================
enum class EmsState : int {
    kInit      = 0,   // 上电初始化
    kSelfCheck = 1,   // 自检
    kReady     = 2,   // 就绪待机（允许评估，不允许输出）
    kNormal    = 3,   // 正常运行
    kDerated   = 4,   // 降功率运行（L1 安全约束动作）
    kFault     = 5,   // 故障（可恢复）
    kEmergency = 6,   // 紧急停机（锁存，需手动复位）
};

inline const char* state_name(EmsState s) {
    switch (s) {
        case EmsState::kInit:      return "INIT";
        case EmsState::kSelfCheck: return "SELF_CHECK";
        case EmsState::kReady:     return "READY";
        case EmsState::kNormal:    return "NORMAL";
        case EmsState::kDerated:   return "DERATED";
        case EmsState::kFault:     return "FAULT";
        case EmsState::kEmergency: return "EMERGENCY";
        default:                   return "UNKNOWN";
    }
}

// =====================================================================
// 故障源（位域，便于日志与测试断言）
// =====================================================================
struct FaultFlags {
    bool bms_comm_lost  = false;  // BMS 通信异常
    bool pcs_comm_lost  = false;  // PCS 通信异常
    bool meter_comm_lost = false; // 关口电表通信异常（→ 采集层超时，走 HOLD_LAST）
    bool pcs_fault      = false;  // PCS 故障
    bool data_invalid   = false;  // 数据异常（越界/校验失败）
    bool device_offline = false;  // 设备离线
    bool temp_fault     = false;  // 电池温度故障
    bool emergency_stop = false;  // 外部急停

    bool any() const {
        return bms_comm_lost || pcs_comm_lost || meter_comm_lost || pcs_fault ||
               data_invalid || device_offline || temp_fault || emergency_stop;
    }

    // 需要立刻进入 EMERGENCY 的条件
    bool emergency() const { return emergency_stop || temp_fault; }

    // 需要进入 FAULT（可恢复）的条件
    // 注：电表通信异常不进 FAULT —— 按接口规范 §6 走 HOLD_LAST 降级路径
    bool fault() const {
        return bms_comm_lost || pcs_comm_lost || pcs_fault ||
               data_invalid || device_offline;
    }

    int bits() const {
        return (bms_comm_lost   ? 1 << 0 : 0) |
               (pcs_comm_lost   ? 1 << 1 : 0) |
               (meter_comm_lost ? 1 << 2 : 0) |
               (pcs_fault       ? 1 << 3 : 0) |
               (data_invalid    ? 1 << 4 : 0) |
               (device_offline  ? 1 << 5 : 0) |
               (temp_fault      ? 1 << 6 : 0) |
               (emergency_stop  ? 1 << 7 : 0);
    }

    std::string to_string() const {
        if (!any()) return "none";
        std::string s;
        auto add = [&s](const char* n) {
            if (!s.empty()) s += "|";
            s += n;
        };
        if (bms_comm_lost)   add("BMS_COMM_LOST");
        if (pcs_comm_lost)   add("PCS_COMM_LOST");
        if (meter_comm_lost) add("METER_COMM_LOST");
        if (pcs_fault)       add("PCS_FAULT");
        if (data_invalid)    add("DATA_INVALID");
        if (device_offline)  add("DEVICE_OFFLINE");
        if (temp_fault)      add("TEMP_FAULT");
        if (emergency_stop)  add("EMERGENCY_STOP");
        return s;
    }
};

// =====================================================================
// 配置
// =====================================================================
struct StateMachineConfig {
    double init_hold_s        = 0.5;  // INIT 停留时长
    int    self_check_cycles  = 5;    // 自检需连续通过的拍数
    int    derate_release_cycles = 10; // 降功率解除需连续正常拍数
    int    fault_clear_cycles = 20;   // 故障恢复需连续正常拍数
    bool   allow_ready_output = false; // READY 态是否允许输出（默认不允许）
};

// =====================================================================
// SOE 事件
// =====================================================================
struct StateEvent {
    Timestamp   ts = 0.0;
    EmsState    from = EmsState::kInit;
    EmsState    to   = EmsState::kInit;
    std::string reason;
};

// =====================================================================
// EmsStateMachine
// =====================================================================
class EmsStateMachine {
public:
    EmsStateMachine() = default;
    explicit EmsStateMachine(const StateMachineConfig& cfg) : cfg_(cfg) {}

    void set_config(const StateMachineConfig& cfg) { cfg_ = cfg; }
    const StateMachineConfig& config() const { return cfg_; }

    void reset_all() {
        state_ = EmsState::kInit;
        t_enter_ = 0.0;
        ok_cycles_ = normal_cycles_ = clear_cycles_ = 0;
        latched_emergency_ = false;
        emergency_reason_.clear();
        history_.clear();
        last_reason_ = "power_on";
    }

    EmsState state() const { return state_; }
    const char* state_str() const { return state_name(state_); }
    bool derated() const { return state_ == EmsState::kDerated; }
    const std::string& last_reason() const { return last_reason_; }
    const std::vector<StateEvent>& history() const { return history_; }

    // 上层运行许可（启动/停机命令）
    void request_run(bool on) { run_request_ = on; }
    bool run_requested() const { return run_request_; }

    // 外部急停（锁存）
    void trigger_emergency_stop(const std::string& why) {
        latched_emergency_ = true;
        emergency_reason_ = why;
    }
    bool emergency_latched() const { return latched_emergency_; }

    // 输出门控
    bool output_enabled() const {
        if (state_ == EmsState::kNormal || state_ == EmsState::kDerated) return true;
        if (state_ == EmsState::kReady) return cfg_.allow_ready_output;
        return false;   // INIT / SELF_CHECK / FAULT / EMERGENCY
    }
    // 策略评估门控（READY 起允许评估，便于监控；FAULT 以上停评）
    bool strategies_enabled() const {
        return state_ == EmsState::kReady || state_ == EmsState::kNormal ||
               state_ == EmsState::kDerated;
    }
    // 是否处于"拒绝一切功率流动"的保守态
    bool fail_safe() const {
        return state_ == EmsState::kFault || state_ == EmsState::kEmergency ||
               state_ == EmsState::kInit   || state_ == EmsState::kSelfCheck;
    }

    // -----------------------------------------------------------------
    // 每拍推进
    //   now          当前时间戳
    //   faults       本拍故障源
    //   safety       本拍安全裁决（用于 DERATED 判定与紧急判定）
    // -----------------------------------------------------------------
    EmsState update(Timestamp now,
                    const FaultFlags& faults,
                    const SafetyVerdict& safety) {
        // 紧急条件合并：显式急停 / 温度故障 / 电网越限（频率电压）
        //
        // 注意：**区间矛盾不进紧急**。矛盾表示"本拍无可行非零功率"，
        //   典型来源：电池满充（禁充，下界 0）叠加光伏大发（不许倒送，上界 < 0）。
        //   此时唯一安全的动作是输出 0（由 SafetyEngine 的 strict_l0 保证），
        //   属于"降功率运行"而非设备紧急 —— 若升级为 EMERGENCY 会**锁存**，
        //   把储能冻结到人工复位，反而造成长时间失去调节能力。
        //   故矛盾 → DERATED（安全裁决里置 derated），运行许可保留。
        const bool emergency = latched_emergency_ || faults.emergency() ||
                               safety.emergency;

        switch (state_) {
            // ---------------- INIT ----------------
            case EmsState::kInit: {
                if (emergency) { transit(now, EmsState::kEmergency, "emergency_during_init"); break; }
                if (now - t_enter_ >= cfg_.init_hold_s) {
                    transit(now, EmsState::kSelfCheck, "init_done");
                }
                break;
            }

            // ---------------- SELF_CHECK ----------------
            case EmsState::kSelfCheck: {
                if (emergency) { transit(now, EmsState::kEmergency, "emergency_during_selfcheck"); break; }
                if (faults.fault()) {
                    transit(now, EmsState::kFault, "selfcheck_fail:" + faults.to_string());
                    break;
                }
                if (++ok_cycles_ >= cfg_.self_check_cycles) {
                    ok_cycles_ = 0;
                    transit(now, EmsState::kReady, "selfcheck_pass");
                }
                break;
            }

            // ---------------- READY ----------------
            case EmsState::kReady: {
                if (emergency) { transit(now, EmsState::kEmergency, "emergency_in_ready"); break; }
                if (faults.fault()) {
                    transit(now, EmsState::kFault, "fault_in_ready:" + faults.to_string());
                    break;
                }
                if (run_request_) {
                    // 启动即若已有 L1 约束动作，直接进 DERATED，避免先冲再降
                    if (safety.derated) transit(now, EmsState::kDerated, "start_derated");
                    else                transit(now, EmsState::kNormal, "start_command");
                }
                break;
            }

            // ---------------- NORMAL ----------------
            case EmsState::kNormal: {
                if (emergency) { transit(now, EmsState::kEmergency, "emergency_in_normal"); break; }
                if (faults.fault()) {
                    transit(now, EmsState::kFault, "fault_in_normal:" + faults.to_string());
                    break;
                }
                if (safety.derated) {
                    transit(now, EmsState::kDerated, "derate_enter:" + safety.reason);
                }
                break;
            }

            // ---------------- DERATED ----------------
            case EmsState::kDerated: {
                if (emergency) { transit(now, EmsState::kEmergency, "emergency_in_derated"); break; }
                if (faults.fault()) {
                    transit(now, EmsState::kFault, "fault_in_derated:" + faults.to_string());
                    break;
                }
                if (!safety.derated) {
                    if (++normal_cycles_ >= cfg_.derate_release_cycles) {
                        normal_cycles_ = 0;
                        transit(now, EmsState::kNormal, "derate_release");
                    }
                } else {
                    normal_cycles_ = 0;
                }
                break;
            }

            // ---------------- FAULT ----------------
            case EmsState::kFault: {
                if (emergency) { transit(now, EmsState::kEmergency, "escalate_to_emergency"); break; }
                if (!faults.fault()) {
                    if (++clear_cycles_ >= cfg_.fault_clear_cycles) {
                        clear_cycles_ = 0;
                        transit(now, EmsState::kReady, "fault_recovered");
                    }
                } else {
                    clear_cycles_ = 0;
                }
                break;
            }

            // ---------------- EMERGENCY ----------------
            case EmsState::kEmergency: {
                // 锁存：只有显式 reset_emergency() 才能离开
                break;
            }
        }
        return state_;
    }

    // -----------------------------------------------------------------
    // 紧急复位：清锁存 → 回 INIT 重新走一遍自检
    // 返回是否复位成功（仍有故障源时拒绝复位）
    // -----------------------------------------------------------------
    bool reset_emergency(Timestamp now, const FaultFlags& faults) {
        if (state_ != EmsState::kEmergency) return false;
        if (faults.any()) return false;
        latched_emergency_ = false;
        emergency_reason_.clear();
        transit(now, EmsState::kInit, "manual_reset");
        return true;
    }

private:
    void transit(Timestamp now, EmsState to, const std::string& reason) {
        if (to == state_) return;
        StateEvent ev;
        ev.ts     = now;
        ev.from   = state_;
        ev.to     = to;
        ev.reason = reason;
        history_.push_back(ev);

        state_       = to;
        t_enter_     = now;
        last_reason_ = reason;
        ok_cycles_ = normal_cycles_ = clear_cycles_ = 0;

        // 安全要求：进入 FAULT / EMERGENCY 时撤销运行许可。
        // 故障恢复后落到 READY（待机）而不是直接回到 NORMAL，
        // 必须由上层显式重新下发启动命令 —— 避免故障未彻底排查就自动带载。
        if (to == EmsState::kFault || to == EmsState::kEmergency) {
            run_request_ = false;
        }
    }

    StateMachineConfig cfg_{};
    EmsState    state_ = EmsState::kInit;
    Timestamp   t_enter_ = 0.0;
    int         ok_cycles_ = 0;
    int         normal_cycles_ = 0;
    int         clear_cycles_ = 0;
    bool        run_request_ = false;
    bool        latched_emergency_ = false;
    std::string emergency_reason_;
    std::string last_reason_ = "power_on";
    std::vector<StateEvent> history_;
};

} // namespace ems
