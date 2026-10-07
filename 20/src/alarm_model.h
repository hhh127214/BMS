// =====================================================================
// 20/ — 告警能力与持久化 · 告警模型（与仿真无关）
//
// ---------------------------------------------------------------------
// 为什么要有这个文件（本模块要清的三条债之一，交接文档 §3.6「①⑤」）
// ---------------------------------------------------------------------
// 现状：告警条目与告警组装**寄生在仿真装配层** ——
//
//     10/src/sim_24h.h
//         struct AlarmEntry { t; std::string level; std::string source; message; }
//         inline void collect_alarms(EmsRuntime&, const Sim24hConfig&, ...)
//                                                          ^^^^^^^^^^^^^^^
//
// 入参是 `Sim24hConfig`（仿真专属结构体）。后果：
//   · 换个入口（11/ 的 main_ems.cpp、14/ 的平台后端）**就没有告警**；
//   · `level` / `source` 是**字符串**：无法排序、无法按等级过滤、无法统计；
//   · `06/StateEvent` 只有状态迁移，**没有等级 / 事件码**，没法按严重度过滤、
//     没法对外报码；
//   · message 用 `to_string((long long)v)` **丢掉了小数**（P2/soe.h 已记录）。
//
// 本文件把告警模型抽出来，做到：
//   1. 等级用 **enum**（可比较 / 可统计 / 编译期检查）—— 且**不另造一套**：
//      取值与 `14/schema.sql` 的 `alarm.level` 注释逐字对齐
//      （INFO / WARNING / DERATED / FAULT / EMERGENCY）。
//   2. 事件码 / 来源**直接复用 P2 的 `SoeCode` / `SoeSource`** —— 它们已经是
//      全项目唯一的结构化事件码表（P2/src/soe.h）。本文件不复制、不改写。
//   3. 每条告警带**处置动作**（谁该动）—— 这张表来自
//      `11/docs/README.md` §4「六类故障源 × 谁该动」，那是联调固化下来的，
//      不得自己编。
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "soe.h"            // P2：复用 SoeCode / SoeSource / SoeLevel / 名称函数
#include "state_machine.h"  // 06：EmsState（状态迁移类的等级由目标态决定）

#include <string>

namespace ems {

// =====================================================================
// 1. 等级 AlarmSeverity
//
// ★ 取值口径**不在这里发明** —— 来自 `14/schema.sql`：
//     alarm.level  TEXT NOT NULL,  -- INFO / WARNING / DERATED / FAULT / EMERGENCY
//   顺序即严重度顺序（数值越大越严重），与 schema 的枚举顺序一致。
//
//   与 P2 `SoeLevel`（DEBUG/INFO/WARN/ERROR/FATAL）的关系：见 soe_level_of()。
//   两套并存的原因：SoeLevel 是**日志通道**口径（多一档 DEBUG、少一档 DERATED），
//   AlarmSeverity 是**告警通道 / 平台库**口径。两者用显式映射函数连接，
//   不允许各自隐式转换。
// =====================================================================
enum class AlarmSeverity : int {
    kInfo      = 0,   // 正常 / 恢复类事件
    kWarning   = 1,   // 预警：接近限值、通信抖动、需量触及
    kDerated   = 2,   // 降功率运行（**可自恢复**；见项目纪律 §7-5）
    kFault     = 3,   // 故障：门控归零，需恢复流程
    kEmergency = 4,   // 紧急：锁存，需人工复位
};

inline const char* alarm_severity_name(AlarmSeverity s) {
    switch (s) {
        case AlarmSeverity::kInfo:      return "INFO";
        case AlarmSeverity::kWarning:   return "WARNING";
        case AlarmSeverity::kDerated:   return "DERATED";
        case AlarmSeverity::kFault:     return "FAULT";
        case AlarmSeverity::kEmergency: return "EMERGENCY";
    }
    return "?";
}

inline bool alarm_severity_from_name(const std::string& s, AlarmSeverity* out) {
    if (!out) return false;
    if (s == "INFO")      { *out = AlarmSeverity::kInfo;      return true; }
    if (s == "WARNING")   { *out = AlarmSeverity::kWarning;   return true; }
    if (s == "DERATED")   { *out = AlarmSeverity::kDerated;   return true; }
    if (s == "FAULT")     { *out = AlarmSeverity::kFault;     return true; }
    if (s == "EMERGENCY") { *out = AlarmSeverity::kEmergency; return true; }
    return false;
}

// 告警通道 → 日志通道（显式映射，集中在一处，便于审计）。
//   DERATED → WARN：日志口径没有"降功率"这一档，但**语义不丢** ——
//   条目本身的 severity 仍是 DERATED，降级只发生在日志侧。
inline SoeLevel soe_level_of(AlarmSeverity s) {
    switch (s) {
        case AlarmSeverity::kInfo:      return SoeLevel::kInfo;
        case AlarmSeverity::kWarning:   return SoeLevel::kWarn;
        case AlarmSeverity::kDerated:   return SoeLevel::kWarn;
        case AlarmSeverity::kFault:     return SoeLevel::kError;
        case AlarmSeverity::kEmergency: return SoeLevel::kFatal;
    }
    return SoeLevel::kInfo;
}

// 状态迁移的默认等级：由**目标态**决定（与 10/collect_alarms 的历史口径一致，
// 但这里是唯一实现，不再散落在装配层）。
inline AlarmSeverity alarm_severity_for_state(EmsState to) {
    switch (to) {
        case EmsState::kEmergency: return AlarmSeverity::kEmergency;
        case EmsState::kFault:     return AlarmSeverity::kFault;
        case EmsState::kDerated:   return AlarmSeverity::kDerated;
        default:                   return AlarmSeverity::kInfo;
    }
}

// =====================================================================
// 2. 事件码 / 来源 —— 复用 P2，不另造一套
// =====================================================================
using AlarmCode   = SoeCode;
using AlarmSource = SoeSource;

inline const char* alarm_code_name(AlarmCode c)   { return soe_code_name(c); }
inline const char* alarm_source_name(AlarmSource s) { return soe_source_name(s); }

// 与 SET 配对的 CLEAR 事件码。
//
//   · Start/End 成对的阈值类 → 对应 End
//   · 通信类 → kCommRestored（三类共用，靠 source/source_id 区分）
//   · PCS 故障 / 离线 → 各自的 Clear / Online
//   · **点事件**（状态迁移）没有配对 → kNone，由装配器按"点事件"处理
//
// 为什么单独做一张表而不是给 SoeCode 加 `End` 字段：SoeCode 是公共契约，
//   波及 P2/P3/11/14，改它要过集成方（见硬约束）。映射表放在这里，
//   只读、单向，不产生第二份真相。
inline AlarmCode alarm_clear_code(AlarmCode set_code) {
    switch (set_code) {
        case SoeCode::kSocLowStart:      return SoeCode::kSocLowEnd;
        case SoeCode::kSocHighStart:     return SoeCode::kSocHighEnd;
        case SoeCode::kTempHighStart:    return SoeCode::kTempHighEnd;
        case SoeCode::kTrOverloadStart:  return SoeCode::kTrOverloadEnd;
        case SoeCode::kDemandBreachStart:return SoeCode::kDemandBreachEnd;
        case SoeCode::kGridReverseStart: return SoeCode::kGridReverseEnd;
        case SoeCode::kGridOverStart:    return SoeCode::kGridOverEnd;
        case SoeCode::kSafetyClipStart:  return SoeCode::kSafetyClipEnd;
        case SoeCode::kRampLimitedStart: return SoeCode::kRampLimitedEnd;
        case SoeCode::kDataStaleStart:   return SoeCode::kDataStaleEnd;
        case SoeCode::kHoldLastStart:    return SoeCode::kHoldLastEnd;
        case SoeCode::kPlanInvalidStart: return SoeCode::kPlanValidEnd;
        case SoeCode::kCommLostBms:      return SoeCode::kCommRestored;
        case SoeCode::kCommLostMeter:    return SoeCode::kCommRestored;
        case SoeCode::kCommLostPcs:      return SoeCode::kCommRestored;
        case SoeCode::kPcsFaultSet:      return SoeCode::kPcsFaultClear;
        case SoeCode::kDeviceOffline:    return SoeCode::kDeviceOnline;
        default:                         return SoeCode::kNone;  // 点事件 / 无配对
    }
}

// =====================================================================
// 3. 处置动作 AlarmAction —— "谁该动"的机器可读形式
//
//   与 `11/docs/README.md` §4 的表格逐行对应（见 alarm_policy()）。
// =====================================================================
enum class AlarmAction : int {
    kNone     = 0,   // 仅记录（信息类）
    kNotify   = 1,   // 提示（不需自动动作）
    kHoldLast = 2,   // 保持上一拍指令（接口规范 §6：电表通信丢失的降级路径）
    kDerate   = 3,   // 降功率运行（可自恢复，**不进 FAULT**）
    kGateZero = 4,   // EMS 门控归零（撤销运行许可 → 指令强制 0）
};

inline const char* alarm_action_name(AlarmAction a) {
    switch (a) {
        case AlarmAction::kNone:     return "NONE";
        case AlarmAction::kNotify:   return "NOTIFY";
        case AlarmAction::kHoldLast: return "HOLD_LAST";
        case AlarmAction::kDerate:   return "DERATE";
        case AlarmAction::kGateZero: return "GATE_ZERO";
    }
    return "?";
}

// 说明：owner 表示**谁该动**。
//   kEms    —— EMS 侧动作（门控 / 降功率 / 保持指令）
//   kDevice —— 设备侧 fail-safe（PCS 自己能判定并跳闸）
//   kBoth   —— 两侧同时
enum class AlarmOwner : int { kEms = 0, kDevice = 1, kBoth = 2 };

inline const char* alarm_owner_name(AlarmOwner o) {
    switch (o) {
        case AlarmOwner::kEms:    return "EMS";
        case AlarmOwner::kDevice: return "DEVICE";
        case AlarmOwner::kBoth:   return "BOTH";
    }
    return "?";
}

struct AlarmPolicy {
    AlarmSeverity severity = AlarmSeverity::kInfo;
    AlarmAction   action   = AlarmAction::kNone;
    AlarmOwner    owner    = AlarmOwner::kEms;
    // 设备侧是否应当观察到 fail-safe（11/§4 判据：kind ∈ {3,4,5} → 是）。
    bool          device_self_protect = false;
};

// =====================================================================
// 4. 策略表 alarm_policy()
//
// ★ 六类故障源 (SoeCode) 的等级/处置**逐条对齐** `11/docs/README.md` §4：
//
//   kind 1  BMS 通信丢失  bit0 → FAULT  + 门控归零  设备侧 fail-safe: 否
//   kind 2  电表通信丢失  bit2 → 不进 FAULT，走 HOLD_LAST  fail-safe: 否
//   kind 3  PCS 通信丢失  bit1 → FAULT  + 门控归零  设备侧 fail-safe: 是
//   kind 4  PCS 故障      bit3 → FAULT  + 门控归零  设备侧 fail-safe: 是
//   kind 5  设备离线      bit5 → FAULT  + 门控归零  设备侧 fail-safe: 是
//   kind 6  数据品质劣化  bit4 → FAULT  + 门控归零  设备侧 fail-safe: 否
//
// §4 判据原文：`dev_self_protect = (kind ∈ {3,4,5})` → 断言 failsafe_ticks > 0。
//   本函数的 device_self_protect 与之逐位一致。
// =====================================================================
inline AlarmPolicy alarm_policy(AlarmCode code) {
    AlarmPolicy p;
    switch (code) {
        // ---- kind 1：BMS 通信丢失（断的是 BMS→EMS，设备自己不知道）----
        case SoeCode::kCommLostBms:
            p = {AlarmSeverity::kFault, AlarmAction::kGateZero, AlarmOwner::kEms, false};
            break;
        // ---- kind 2：电表通信丢失（PCS 对关口表无感知；超阈值 HOLD_LAST）----
        case SoeCode::kCommLostMeter:
            p = {AlarmSeverity::kWarning, AlarmAction::kHoldLast, AlarmOwner::kEms, false};
            break;
        // ---- kind 3：PCS 通信丢失（设备自己能判定）----
        case SoeCode::kCommLostPcs:
            p = {AlarmSeverity::kFault, AlarmAction::kGateZero, AlarmOwner::kBoth, true};
            break;
        // ---- kind 4：PCS 故障（设备自己能判定）----
        case SoeCode::kPcsFaultSet:
            p = {AlarmSeverity::kFault, AlarmAction::kGateZero, AlarmOwner::kBoth, true};
            break;
        // ---- kind 5：设备离线（设备自己能判定）----
        case SoeCode::kDeviceOffline:
            p = {AlarmSeverity::kFault, AlarmAction::kGateZero, AlarmOwner::kBoth, true};
            break;
        // ---- kind 6：数据品质位劣化（值仍是值，只是不可信；设备照常执行）----
        case SoeCode::kDataStaleStart:
            p = {AlarmSeverity::kFault, AlarmAction::kGateZero, AlarmOwner::kEms, false};
            break;

        // ---- 状态机 ----
        case SoeCode::kFsmEmergencyStop:
            p = {AlarmSeverity::kEmergency, AlarmAction::kGateZero, AlarmOwner::kBoth, true};
            break;
        case SoeCode::kFsmTransition:
            // 等级由目标态决定（见 alarm_severity_for_state），动作由状态机门控承担
            p = {AlarmSeverity::kInfo, AlarmAction::kNone, AlarmOwner::kEms, false};
            break;

        // ---- 阈值类（可自恢复，**不得**升级为锁存 EMERGENCY，纪律 §7-5）----
        case SoeCode::kSocLowStart:
        case SoeCode::kSocHighStart:
            p = {AlarmSeverity::kWarning, AlarmAction::kDerate, AlarmOwner::kEms, false};
            break;
        case SoeCode::kTempHighStart:
            p = {AlarmSeverity::kWarning, AlarmAction::kDerate, AlarmOwner::kEms, false};
            break;
        case SoeCode::kTrOverloadStart:
            p = {AlarmSeverity::kDerated, AlarmAction::kDerate, AlarmOwner::kEms, false};
            break;
        case SoeCode::kDemandBreachStart:
            p = {AlarmSeverity::kWarning, AlarmAction::kDerate, AlarmOwner::kEms, false};
            break;
        case SoeCode::kGridReverseStart:
        case SoeCode::kGridOverStart:
            p = {AlarmSeverity::kWarning, AlarmAction::kDerate, AlarmOwner::kEms, false};
            break;
        case SoeCode::kSafetyClipStart:
        case SoeCode::kRampLimitedStart:
            p = {AlarmSeverity::kDerated, AlarmAction::kDerate, AlarmOwner::kEms, false};
            break;

        // ---- 保持上一拍（降级路径本身）----
        case SoeCode::kHoldLastStart:
            p = {AlarmSeverity::kWarning, AlarmAction::kHoldLast, AlarmOwner::kEms, false};
            break;

        // ---- 其余：仅记录 ----
        default:
            p = {AlarmSeverity::kInfo, AlarmAction::kNotify, AlarmOwner::kEms, false};
            break;
    }
    return p;
}

// =====================================================================
// 5. 告警条目 AlarmRecord —— 一条 SET↔CLEAR 的完整生命周期
//
//   与 10/AlarmEntry 的区别：
//     · code / severity / source 是 **enum**（可排序、可过滤、可统计）
//     · 带 t_clear / active，即 **SET-CLEAR 配对**
//     · 带 value（数值）+ repeat（持续拍数，不产生重复条目）
// =====================================================================
struct AlarmRecord {
    long long     id         = 0;                 // 单调序号（从 1 开始）
    AlarmCode     code       = AlarmCode::kNone;  // SET 事件码
    AlarmCode     clear_code = AlarmCode::kNone;  // 配对 CLEAR 事件码（无配对 = kNone）
    AlarmSeverity severity   = AlarmSeverity::kInfo;
    AlarmAction   action     = AlarmAction::kNone;
    AlarmOwner    owner      = AlarmOwner::kEms;
    AlarmSource   source     = AlarmSource::kSystem;
    std::string   source_id;                      // BMS / PCS / METER / TRANSFORMER / ...
    double        t_set      = 0.0;
    double        t_clear    = -1.0;              // <0 表示尚未恢复
    std::string   message;
    double        value      = 0.0;
    bool          active     = true;
    int           repeat     = 1;                 // 持续期间被观测到的拍数

    bool   cleared()    const { return !active; }
    double duration_s() const { return active ? 0.0 : (t_clear - t_set); }
    // 配对/去重键：同一 (source, source_id, code) 视为同一条告警
    std::string key() const {
        return std::string(alarm_source_name(source)) + ":" + source_id + ":" +
               alarm_code_name(code);
    }
    // 点事件（无 SET-CLEAR 生命周期，如状态迁移）
    bool is_point() const { return clear_code == AlarmCode::kNone && !active && t_clear == t_set; }
};

// 离散事件（装配器每拍返回的就是这个）：
//   SET  = 上升沿（active 由 false→true）产生的**首次**条目
//   CLEAR = 下降沿产生的恢复事件
struct AlarmEvent {
    double        t          = 0.0;
    AlarmCode     code       = AlarmCode::kNone;
    AlarmSeverity severity   = AlarmSeverity::kInfo;
    AlarmSource   source     = AlarmSource::kSystem;
    std::string   source_id;
    bool          is_set     = true;    // true=SET, false=CLEAR
    long long     pair_id    = 0;       // 关联的 AlarmRecord::id
    double        value      = 0.0;
    std::string   message;
};

} // namespace ems
