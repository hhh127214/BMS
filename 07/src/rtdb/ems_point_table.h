// =====================================================================
// EMS 点表定义（C / C++ 共用）
//
// 为什么点表要单独抽一个文件：
//   1. 初始化器（C）与适配器（C++）必须用**同一份**点表，否则索引会错位；
//   2. RT_DB 本身**没有"注册点表"的公开 API**（其 build_index_map 里
//      `(void)config_path` 把配置参数忽略了），点表必须由调用方写进共享内存。
//      本文件就是那个"点表真相源"。
//
// 点名规范与 07/src/memory_device_io.h 的 mem_point:: 完全一致 ——
// 这样 MemoryDeviceIO 与 RtDbDeviceIO 是**逐点可互换**的，可以互相做对照测试。
// 真实项目里这里换成 IEC 61850 引用名或厂家点表 ID。
//
// 编译：C / C++ 均可 include（用 extern "C" 保护符号）。
// =====================================================================

#ifndef EMS_POINT_TABLE_H
#define EMS_POINT_TABLE_H

#include <stddef.h>

#define EMS_POINT_COUNT 30

// ---------------------------------------------------------------------
// 索引常量：用枚举固定顺序，运行时不做字符串比较
// ---------------------------------------------------------------------
enum {
    // 量测（设备 → EMS）
    EMS_P_LOAD = 0,      // kW   负荷
    EMS_P_PV,            // kW   光伏
    EMS_P_BAT,           // kW   电池实际功率（放电为正）
    EMS_P_GRID,          // kW   关口功率（进口为正）
    EMS_SOC,             // 0..1 SOC
    EMS_T_C,             // °C   电池温度
    EMS_SOH,             // 0..1 SOH

    // 指令（EMS → 设备）
    EMS_CMD_P_BAT,       // kW   下发电池功率
    EMS_CMD_P_UPPER,     // kW   允许上界
    EMS_CMD_P_LOWER,     // kW   允许下界

    // 配置（静态，初始化时写一次）
    EMS_CFG_CAP_KWH,     // kWh  电池额定容量
    EMS_CFG_MAX_CHG,     // kW   PCS 额定充电幅度
    EMS_CFG_MAX_DIS,     // kW   PCS 额定放电幅度
    EMS_CFG_BMS_CHG_LIM, // kW   BMS 充电限值
    EMS_CFG_BMS_DIS_LIM, // kW   BMS 放电限值
    EMS_CFG_TR_KVA,      // kVA  变压器容量
    EMS_CFG_D_TARGET,    // kW   契约需量
    EMS_CFG_TAU_S,       // s    执行惯性时间常数
    EMS_CFG_RAMP_KW_S,   // kW/s 变化率上限
    EMS_CFG_STANDBY,     // kW   PCS 空载损耗
    EMS_CFG_ETA_CHG,     // -    充电效率
    EMS_CFG_ETA_DIS,     // -    放电效率
    EMS_CFG_SOC_MIN,     // -    SOC 物理下限
    EMS_CFG_SOC_MAX,     // -    SOC 物理上限

    // 状态（设备 → EMS）
    EMS_STA_BMS,         // 0/1  BMS 通信正常
    EMS_STA_PCS,         // 0/1  PCS 通信正常
    EMS_STA_METER,       // 0/1  电表通信正常
    EMS_STA_FAULT,       // 0/1  PCS 故障
    EMS_STA_OFFLINE,     // 0/1  设备离线
    EMS_STA_VALID        // 0/1  数据有效
};

#ifdef __cplusplus
extern "C" {
#endif

// 点名数组（下标 = 上面的枚举值）
extern const char* const EMS_POINT_NAMES[EMS_POINT_COUNT];
// 单位数组（与点名一一对应，RT_DB 的 DataPoint.units 用）
extern const char* const EMS_POINT_UNITS[EMS_POINT_COUNT];
// 配置/状态点的默认值（量测点默认 0，由设备侧刷新）
extern const double EMS_POINT_DEFAULTS[EMS_POINT_COUNT];

#ifdef __cplusplus
}
#endif

#endif // EMS_POINT_TABLE_H
