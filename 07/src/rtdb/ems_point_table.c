// =====================================================================
// EMS 点表定义（实现）—— 与 07/src/memory_device_io.h 的 mem_point:: 一一对应
//
// 默认值取自 DeviceLimits / PlantConfig 的默认，保证 RtDbDeviceIO 与
// MemoryDeviceIO 在"未做任何注入"时行为一致。
// =====================================================================

#include "ems_point_table.h"

const char* const EMS_POINT_NAMES[EMS_POINT_COUNT] = {
    "MEAS.P_LOAD", "MEAS.P_PV", "MEAS.P_BAT", "MEAS.P_GRID",
    "MEAS.SOC", "MEAS.T_C", "MEAS.SOH",
    "CMD.P_BAT", "CMD.P_UPPER", "CMD.P_LOWER",
    "CFG.BAT_CAP_KWH", "CFG.PCS_MAX_CHG", "CFG.PCS_MAX_DIS",
    "CFG.BMS_CHG_LIM", "CFG.BMS_DIS_LIM", "CFG.TRANSFORMER_KVA",
    "CFG.D_TARGET", "CFG.TAU_S", "CFG.PCS_RAMP_KW_PER_S", "CFG.PCS_STANDBY",
    "CFG.ETA_CHG", "CFG.ETA_DIS", "CFG.SOC_PHYS_MIN", "CFG.SOC_PHYS_MAX",
    "STA.BMS_COMM_OK", "STA.PCS_COMM_OK", "STA.METER_COMM_OK",
    "STA.PCS_FAULT", "STA.OFFLINE", "STA.DATA_VALID"
};

const char* const EMS_POINT_UNITS[EMS_POINT_COUNT] = {
    "kW", "kW", "kW", "kW", "%", "degC", "%",
    "kW", "kW", "kW",
    "kWh", "kW", "kW", "kW", "kW", "kVA",
    "kW", "s", "kW/s", "kW", "-", "-", "-", "-",
    "bool", "bool", "bool", "bool", "bool", "bool"
};

const double EMS_POINT_DEFAULTS[EMS_POINT_COUNT] = {
    // 量测：默认 0（SOC / SOH 例外，由设备侧刷新）
    0.0, 0.0, 0.0, 0.0,
    0.50, 25.0, 1.0,
    // 指令
    0.0, 0.0, 0.0,
    // 配置（与 PlantConfig 默认对齐：容量 1000kWh、PCS 200kW、站用电 2kW）
    1000.0, 200.0, 200.0, 200.0, 200.0, 250.0,
    250.0, 0.0, 1e9, 2.0,
    1.0, 1.0, 0.05, 0.95,
    // 状态（全正常）
    1.0, 1.0, 1.0, 0.0, 0.0, 1.0
};
