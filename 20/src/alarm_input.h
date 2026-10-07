// =====================================================================
// 20/ — 告警输入适配器（两个入口，同一个装配器）
//
// ---------------------------------------------------------------------
// 本文件要回答的问题
// ---------------------------------------------------------------------
// `AlarmAssembler` 只认 `AlarmInput`。那么"换个入口还有没有告警"就等价于
// "新入口能不能把它的数据映射成 AlarmInput"。本文件提供**两个**适配器，
// 覆盖项目里仅有的两类入口：
//
//   ① 仿真入口：`alarm_input_from_step_record()`
//        数据来源 10/ 的 `StepRecord`（`run_sim_24h()` 的产物）。
//        这是**过渡期**入口，用于回归对比旧 `collect_alarms()`。
//
//   ② 生产入口：`alarm_input_from_production()`
//        数据来源是 P0 的三个公共契约 + 运行期标志：
//          `RealtimeSnapshot`（量测）+ `DeviceStatus`（通信/故障）
//          + `DeviceLimits`（阈值相关的设备限值）+ `EmsState`（状态）。
//        **没有任何 `Sim24hConfig`** —— 这正是交接文档 §2.2 要求的。
//        11/ 的 `main_ems.cpp`、14/ 的平台后端照这个签名取数即可。
//
// ★ 两个适配器都把"通信状态"折算成 **状态机口径**（见 fault_view_from_status()），
//   而不是照抄 `DeviceStatus` 的原始字段 —— 否则同一台设备在
//   "设备离线"时，仿真路径（`FaultFlags::bits()` 里 bms_comm_lost 被
//   `|| device_offline` 置位）与生产路径（`DeviceStatus::bms_comm_ok` 仍为 true）
//   会得到**两套不同的告警**，等价性测试就是假的。
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "alarm_assembler.h"   // AlarmInput / AlarmContext
#include "data_models.h"       // RealtimeSnapshot / DeviceLimits
#include "device_io.h"         // DeviceStatus
#include "realtime_loop.h"     // StepRecord
#include "safety_engine.h"     // SafetyParams（构造 AlarmContext 用）
#include "state_machine.h"     // EmsState

#include <cmath>
#include <string>

namespace ems {

// =====================================================================
// 通信/故障状态的状态机口径
//
// 与 `EmsRuntime::detect_faults()` 逐行一致 —— 那里才是全项目对
// "什么算故障"的权威判定。适配器**不自己发明**。
// =====================================================================
struct FaultView {
    bool bms_comm_ok    = true;
    bool pcs_comm_ok    = true;
    bool meter_comm_ok  = true;
    bool pcs_fault      = false;
    bool device_offline = false;
    bool data_valid     = true;
};

inline FaultView fault_view_from_status(const DeviceStatus& st) {
    FaultView f;
    f.bms_comm_ok    = st.bms_comm_ok && !st.device_offline;  // 同 detect_faults()
    f.pcs_comm_ok    = st.pcs_comm_ok;
    f.meter_comm_ok  = st.meter_comm_ok;
    f.pcs_fault      = st.pcs_fault;
    f.device_offline = st.device_offline;
    f.data_valid     = st.data_valid;
    return f;
}

// fault_bits（StepRecord）解码 —— 位序取自 06/FaultFlags::bits()
inline FaultView fault_view_from_bits(int bits) {
    FaultView f;
    f.bms_comm_ok    = (bits & (1 << 0)) == 0;
    f.pcs_comm_ok    = (bits & (1 << 1)) == 0;
    f.meter_comm_ok  = (bits & (1 << 2)) == 0;
    f.pcs_fault      = (bits & (1 << 3)) != 0;
    f.data_valid     = (bits & (1 << 4)) == 0;
    f.device_offline = (bits & (1 << 5)) != 0;
    return f;
}

inline bool fault_bits_emergency_stop(int bits) { return (bits & (1 << 7)) != 0; }

// =====================================================================
// 生产快照 ProductionSnapshot —— 与仿真无关的取数包
//
//   现场侧装配：rt.device()->read_snapshot() / read_status() / read_limits()
//               + rt.fsm().state() + 运行期标志（是否 HOLD_LAST / 限幅）
//   这里把"一次取数"打包，避免调用方漏项。
// =====================================================================
struct ProductionSnapshot {
    RealtimeSnapshot rt;          // 量测
    DeviceStatus     st;          // 通信 / 故障 / 品质
    DeviceLimits     dev;         // 设备限值（阈值随设备走）
    EmsState         state = EmsState::kInit;

    bool        has_transition = false;
    EmsState    state_from = EmsState::kInit;
    EmsState    state_to   = EmsState::kInit;
    std::string transition_reason;

    bool emergency_stop = false;  // 外部急停 / 温度故障等紧急源（设备侧另有通道）
    bool data_stale     = false;  // 采集超时 → HOLD_LAST 生效
    bool safety_clip    = false;  // 本拍安全层限幅真正动作
};

// =====================================================================
// ① 仿真入口：StepRecord → AlarmInput
// =====================================================================
inline void alarm_input_from_step_record(const StepRecord& rec,
                                        const StepRecord* prev,
                                        AlarmInput& out) {
    out.t = rec.t;

    out.state = rec.state;
    out.has_transition = false;
    out.state_from = rec.state;
    out.state_to   = rec.state;
    out.transition_reason.clear();
    if (prev && prev->state != rec.state) {
        out.has_transition = true;
        out.state_from = prev->state;
        out.state_to   = rec.state;
        // 注意：`StepRecord::reason` 是**指令**的理由（cmd.reason），不是状态机
        // 的迁移理由 —— 不能拿来当迁移原因。迁移原因在 `fsm().last_reason()`
        // 或 `fsm().history()` 里，生产入口（alarm_input_from_production）能拿到，
        // 仿真入口这里拿不到，故留空（见 docs/README.md 已知边界）。
        out.transition_reason.clear();
    }

    out.soc           = rec.soc;
    out.temperature_c = rec.temp;
    out.p_grid_kw     = rec.p_grid;
    out.p_load_kw     = rec.p_load;

    const FaultView f = fault_view_from_bits(rec.fault_bits);
    out.bms_comm_ok    = f.bms_comm_ok;
    out.pcs_comm_ok    = f.pcs_comm_ok;
    out.meter_comm_ok  = f.meter_comm_ok;
    out.pcs_fault      = f.pcs_fault;
    out.data_valid     = f.data_valid;
    out.device_offline = f.device_offline;
    out.emergency_stop = fault_bits_emergency_stop(rec.fault_bits);

    out.data_stale  = rec.hold_last;
    out.safety_clip = rec.safety_clip;
}

// =====================================================================
// ② 生产入口：ProductionSnapshot → AlarmInput
// =====================================================================
inline void alarm_input_from_production(const ProductionSnapshot& p, AlarmInput& out) {
    out.t = p.rt.timestamp;

    out.state = p.state;
    out.has_transition = p.has_transition;
    out.state_from = p.has_transition ? p.state_from : p.state;
    out.state_to   = p.has_transition ? p.state_to   : p.state;
    out.transition_reason = p.transition_reason;

    out.soc           = p.rt.soc;
    out.temperature_c = p.rt.temperature_c;
    out.p_grid_kw     = p.rt.p_grid_kw;
    out.p_load_kw     = p.rt.p_load_kw;

    const FaultView f = fault_view_from_status(p.st);
    out.bms_comm_ok    = f.bms_comm_ok;
    out.pcs_comm_ok    = f.pcs_comm_ok;
    out.meter_comm_ok  = f.meter_comm_ok;
    out.pcs_fault      = f.pcs_fault;
    out.data_valid     = f.data_valid;
    out.device_offline = f.device_offline;
    out.emergency_stop = p.emergency_stop;

    out.data_stale  = p.data_stale;
    out.safety_clip = p.safety_clip;
}

// =====================================================================
// 桥：StepRecord → ProductionSnapshot
//
//   用途有两个：
//     · 等价性回归：把仿真的**公共产物**改写成 P0 契约，再喂同一个装配器，
//       断言告警流不变 —— 证明装配器不依赖 Sim24hConfig。
//     · 过渡期：老入口还没有 P0 取数通道时，可先这样搭桥。
// =====================================================================
inline void production_from_step_record(const StepRecord& rec,
                                       const StepRecord* prev,
                                       const DeviceLimits& dev,
                                       ProductionSnapshot& out) {
    out.rt = RealtimeSnapshot{};
    out.rt.timestamp     = rec.t;
    out.rt.p_grid_kw     = rec.p_grid;
    out.rt.p_load_kw     = rec.p_load;
    out.rt.p_pv_kw       = rec.p_pv;
    out.rt.p_bat_actual_kw = rec.p_actual;
    out.rt.soc           = rec.soc;
    out.rt.temperature_c = rec.temp;

    out.st = DeviceStatus{};
    out.st.bms_comm_ok    = (rec.fault_bits & (1 << 0)) == 0;
    out.st.pcs_comm_ok    = (rec.fault_bits & (1 << 1)) == 0;
    out.st.meter_comm_ok  = (rec.fault_bits & (1 << 2)) == 0;
    out.st.pcs_fault      = (rec.fault_bits & (1 << 3)) != 0;
    out.st.data_valid     = (rec.fault_bits & (1 << 4)) == 0;
    out.st.device_offline = (rec.fault_bits & (1 << 5)) != 0;

    out.dev = dev;

    out.state = rec.state;
    out.has_transition = prev && prev->state != rec.state;
    out.state_from = out.has_transition ? prev->state : rec.state;
    out.state_to   = rec.state;
    out.transition_reason.clear();   // 见上：StepRecord 不带状态机迁移理由

    out.emergency_stop = fault_bits_emergency_stop(rec.fault_bits);
    out.data_stale     = rec.hold_last;
    out.safety_clip    = rec.safety_clip;
}

// =====================================================================
// 阈值上下文：从生产侧的 SafetyParams / DeviceLimits 构造
//
//   ★ 这里就是"不再需要 Sim24hConfig"的落点 —— 阈值的权威来源是
//     05/SafetyParams（安全参数）与 04/DeviceLimits（设备限值），
//     仿真配置只是它们的一个装配实例。
// =====================================================================
inline AlarmContext alarm_context_from(const SafetyParams& sp,
                                      const DeviceLimits& dev,
                                      bool forbid_reverse) {
    AlarmContext c;
    // SOC 预警阈值：用 SafetyParams 的预警带（不是绝对上下限）——
    //   预警是"接近限值"，绝对上下限由安全层直接禁充放，另有门控。
    c.soc_low  = std::min(sp.soc_min, sp.soc_warn_low);
    c.soc_high = std::max(sp.soc_max, sp.soc_warn_high);
    c.soc_hysteresis = sp.soc_hysteresis;

    c.temp_warn_c       = sp.temp_warn_c;
    c.temp_fault_c      = sp.temp_fault_c;
    c.temp_hysteresis_c = sp.temp_hysteresis_c;

    c.transformer_capacity_kw = dev.transformer_capacity_kw;
    c.tr_overload_th          = sp.tr_overload_th;
    c.tr_load_pv_share        = sp.tr_load_pv_share;
    c.tr_hysteresis           = sp.tr_hysteresis;

    c.demand_target_kw = dev.d_target_kw;

    c.forbid_reverse = forbid_reverse;
    c.grid_min_kw    = sp.grid_p_min_kw;
    return c;
}

} // namespace ems
