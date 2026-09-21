// =====================================================================
// EMS 点表定义（实现）—— 与 07/src/memory_device_io.h 的 mem_point:: 一一对应
//
// 默认值取自 DeviceLimits / PlantConfig 的默认，保证 RtDbDeviceIO 与
// MemoryDeviceIO 在"未做任何注入"时行为一致。
// =====================================================================

#include "ems_point_table.h"

// 编译期守卫（C99 没有 static_assert，用"负长度数组"等价表达）：
//   EMS_EXT_END 必须等于 EMS_POINT_COUNT。
// 为什么要它：这两个数一处是"点区边界"、一处是"数组长度"，
//   加点时只改一处 → 数组少一个元素（C 会把缺的元素补 0，**不报错**）→
//   某个点名字变成 NULL，现场表现为"点名对得上、值全是隔壁点的"。
typedef char ems_ext_end_must_equal_point_count
    [(EMS_EXT_END == EMS_POINT_COUNT) ? 1 : -1];

const char* const EMS_POINT_NAMES[EMS_POINT_COUNT] = {
    "MEAS.P_LOAD", "MEAS.P_PV", "MEAS.P_BAT", "MEAS.P_GRID",
    "MEAS.SOC", "MEAS.T_C", "MEAS.SOH",
    "CMD.P_BAT", "CMD.P_UPPER", "CMD.P_LOWER",
    "CFG.BAT_CAP_KWH", "CFG.PCS_MAX_CHG", "CFG.PCS_MAX_DIS",
    "CFG.BMS_CHG_LIM", "CFG.BMS_DIS_LIM", "CFG.TRANSFORMER_KVA",
    "CFG.D_TARGET", "CFG.TAU_S", "CFG.PCS_RAMP_KW_PER_S", "CFG.PCS_STANDBY",
    "CFG.ETA_CHG", "CFG.ETA_DIS", "CFG.SOC_PHYS_MIN", "CFG.SOC_PHYS_MAX",
    "STA.BMS_COMM_OK", "STA.PCS_COMM_OK", "STA.METER_COMM_OK",
    "STA.PCS_FAULT", "STA.OFFLINE", "STA.DATA_VALID",
    "STA.BMS_CHG_FORBID", "STA.BMS_DIS_FORBID",
    // ---- EXT：外部设定（调度 → EMS）----
    "EXT.P_SETPOINT", "EXT.P_UPPER_SET", "EXT.P_LOWER_SET", "EXT.D_TARGET",
    "EXT.PCS_ONOFF", "EXT.EMS_ENABLE", "EXT.SEQ", "EXT.TS"
};

const char* const EMS_POINT_UNITS[EMS_POINT_COUNT] = {
    "kW", "kW", "kW", "kW", "%", "degC", "%",
    "kW", "kW", "kW",
    "kWh", "kW", "kW", "kW", "kW", "kVA",
    "kW", "s", "kW/s", "kW", "-", "-", "-", "-",
    "bool", "bool", "bool", "bool", "bool", "bool",
    "bool", "bool",
    "kW", "kW", "kW", "kW", "bool", "bool", "-", "s"
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
    1.0, 1.0, 1.0, 0.0, 0.0, 1.0,
    // BMS 禁充放：默认 **0 = 允许**。
    // 刻意不设成 1：默认禁充放会让"设备侧尚未上线"直接锁死 [0,0]（冷启动
    // 不可用），而"允许"与修复前的行为逐位一致，不引入新风险。安全性由
    // 05/ 的 bms_comm_lost → [0,0] 独立承担（那条路径不依赖本点）。
    0.0, 0.0,
    // ---- EXT：外部设定（A3.1）----
    // ★ 取法与 A1 同一条纪律：**默认值必须落在"不引入新约束 / 不改变现状"的那一侧**。
    //   默认落在"施加约束"那一侧 = 冷启动就带上一个没人下过的约束。
    0.0,     // EXT.P_SETPOINT   0 kW 对储能不是有意义的指令（停机另有 5001 遥控）
             //                  → 天然作"无设定"哨兵，不施加任何约束
    1e9,     // EXT.P_UPPER_SET  与 CFG.PCS_RAMP_KW_PER_S 同款"极大数 = 不限制"约定
    -1e9,    // EXT.P_LOWER_SET  同上，反向
    0.0,     // EXT.D_TARGET     0 = 不覆盖 CFG.D_TARGET
    1.0,     // EXT.PCS_ONOFF    默认在线
    1.0,     // EXT.EMS_ENABLE   ★ 默认 **1 = 允许 EMS 接管**。
             //   若默认 0，则"调度还没发过任何命令"就等于"调度闭锁了 EMS"
             //   → 一上线 EMS 只监视不调节，冷启动不可用。
             //   这与 A1 把禁充放位默认成 0 是同一类坑，处置也相同。
    0.0,     // EXT.SEQ          0 = 从未收到过任何外部设定 → 整区视为无效
    0.0      // EXT.TS           0 = 从未
};

// ---------------------------------------------------------------------
// 点区归属
// ---------------------------------------------------------------------
const char* ems_zone_name(ems_point_zone_t z) {
    switch (z) {
        case EMS_ZONE_MEAS: return "MEAS";
        case EMS_ZONE_CMD:  return "CMD";
        case EMS_ZONE_CFG:  return "CFG";
        case EMS_ZONE_STA:  return "STA";
        case EMS_ZONE_EXT:  return "EXT";
        default:            return "INVALID";
    }
}

ems_point_zone_t ems_point_zone_of(int index) {
    // ★ 全部用**枚举值**写边界，不抄数字 —— 点表一动，这里跟着动。
    if (index < 0 || index >= EMS_POINT_COUNT) return EMS_ZONE_INVALID;
    if (index < EMS_CMD_P_BAT)                 return EMS_ZONE_MEAS;
    if (index < EMS_CFG_CAP_KWH)               return EMS_ZONE_CMD;
    if (index < EMS_STA_BMS)                   return EMS_ZONE_CFG;
    if (index < EMS_EXT_BEGIN)                 return EMS_ZONE_STA;
    return EMS_ZONE_EXT;
}
