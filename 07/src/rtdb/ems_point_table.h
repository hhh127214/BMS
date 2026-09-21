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

#define EMS_POINT_COUNT 40

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
    EMS_STA_VALID,       // 0/1  数据有效

    // ---- BMS 保护（设备 → EMS，安全输入）----
    // 为什么追加在**末尾**而不是插进 STA 段中间：
    //   索引是运行时坐标，一旦在中间插入，其后所有点的索引整体偏移，
    //   而共享内存里已有段的索引不会跟着变 —— 现场表现为"点名对得上、
    //   值全是隔壁点的"。追加在末尾则 0..29 的索引**逐位不变**，
    //   老段（30 点）与新段（32 点）在前 30 点上继续兼容。
    //
    // 为什么这是**安全输入**（区别于普通 STA 点）：
    //   它们经 IDeviceIO::read_limits() → DeviceLimits → S01(kBmsForbid, L0)
    //   → 05/ 折进 (p_lower, p_upper)。恒 0 = "BMS 永远允许充放"，
    //   等于把 L0 最底层那道锁摘掉。
    //
    // ★ 现场接入硬要求：BMS 网关必须**每拍显式写**这两个点。
    //   RT_DB 里"没被写过"与"写过 0"在值上不可区分（都是 0.0），
    //   所以禁止"不支持就不写"——不支持时应写 0，并在自己的通信位
    //   STA.BMS_COMM_OK 上体现；BMS 通信丢失由 05/ 独立收紧到 [0,0]，
    //   不依赖本点。
    EMS_STA_BMS_CHG_FORBID,  // 0/1  BMS 禁止充电（1=禁充）
    EMS_STA_BMS_DIS_FORBID,  // 0/1  BMS 禁止放电（1=禁放）

    // ---- EXT 点区：外部设定（调度 → EMS，A3.1，纯追加）----
    //
    // 谁写：**只有** `P3/src/rtdb_ext_sink.h` 的 `RtDbExtWriter`，
    //   由 IEC104 网关进程持有。于是本项目有**三个**写入者，点区互不重叠：
    //     RtDbPointWriter（设备侧）→ MEAS / STA / CFG
    //     RtDbDeviceIO   （EMS 侧）→ CMD
    //     RtDbExtWriter  （网关）  → EXT
    //   ★ 这条边界由 `ems_point_zone_of()` + 每个写入者的 `self_check()` 守着，
    //     不靠注释 —— 跨区写入必须能被测试判红。
    //
    // 为什么叫 EXT 而不是塞进 CMD：
    //   语义完全不同。`CMD.*` 是 **EMS 自己的决定**（算法输出，可每拍变）；
    //   `EXT.*` 是**外部操作者的请求**。混在一区会让"这条指令是谁下的"
    //   在数据上不可分辨 —— 事故复盘时这是致命的。
    //
    // ★ 为什么是"电平语义"而不是"事件语义"：
    //   本区保存的是**当前外部设定的状态**（最新值持续生效），不是命令队列。
    //   所以不需要逐点序号去重 —— 一个 `EXT.SEQ` 表示"本区被改写过多少次"
    //   （发布信号），一个 `EXT.TS` 表示"最后一次改写的时间"（陈旧判定）。
    //   逐点 TS/SEQ 唯一多出来的能力是"分辨哪一条命令变新了"，那对应事件语义，
    //   与设定语义不匹配，而且是 3 倍的点数与出错面。
    //
    // ★ 陈旧的处置由**消费侧**判定（`now - EXT.TS > stale_s` → 整区视为无效），
    //   不能只靠网关"断开时清空" —— 网关进程自己死掉时没人清。
    //
    // ★ 默认值的取法（同 A1 的纪律：默认取"不引入新约束"的那一侧）：
    //   P_SETPOINT=0   0 kW 对储能不是有意义的指令（停机另有 5001 遥控）
    //                  → 天然作"无设定"哨兵，不施加任何约束
    //   P_UPPER_SET=+1e9 / P_LOWER_SET=-1e9   与 CFG.PCS_RAMP_KW_PER_S 同款
    //                  "极大数 = 不限制"约定，求交后无效果
    //   EMS_ENABLE=1   语义是"允许 EMS 接管"。★ 若默认 0，则**一上线 EMS 就被
    //                  调度闭锁**（冷启动不可用）—— 与 A1 把禁充放位默认成 0
    //                  是同一个坑：默认必须落在"不改变现状"的那一侧
    //   PCS_ONOFF=1    默认在线（同上）
    EMS_EXT_BEGIN = EMS_STA_BMS_DIS_FORBID + 1,
    EMS_EXT_P_SETPOINT = EMS_EXT_BEGIN,  // kW    有功功率设定（IOA 6001）
    EMS_EXT_P_UPPER_SET,                 // kW    允许上界设定（IOA 6003）
    EMS_EXT_P_LOWER_SET,                 // kW    允许下界设定（IOA 6004）
    EMS_EXT_D_TARGET,                    // kW    契约需量设定（IOA 6002）
    EMS_EXT_PCS_ONOFF,                   // 0/1   PCS 远程启停（IOA 5001）
    EMS_EXT_EMS_ENABLE,                  // 0/1   EMS 投入退出（IOA 5002）
    EMS_EXT_SEQ,                         // -     本区写入序号（发布信号，0 = 从未收到）
    EMS_EXT_TS,                          // s     最后一次写入的时标（陈旧判定）
    EMS_EXT_END                          // 哨兵：== EMS_POINT_COUNT
};

// ---------------------------------------------------------------------
// 点区归属
//
// 用途：把"某个写入者只能写自己那一区"从**注释**变成**可被测试检出**的性质。
//       三个写入者各自在 self_check() 里对自己要写的每个索引调一次即可。
// ---------------------------------------------------------------------
typedef enum {
    EMS_ZONE_INVALID = -1,
    EMS_ZONE_MEAS    = 0,   // 设备 → EMS
    EMS_ZONE_CMD     = 1,   // EMS → 设备
    EMS_ZONE_CFG     = 2,   // 静态配置
    EMS_ZONE_STA     = 3,   // 设备 → EMS（状态/安全输入）
    EMS_ZONE_EXT     = 4    // 调度 → EMS（外部设定）
} ems_point_zone_t;

// 便捷判据：读侧用它区分"这个点是设备量的"还是"外部设定的"
static inline int ems_is_ext_point(int index) {
    return index >= EMS_EXT_BEGIN && index < EMS_EXT_END;
}
static inline int ems_is_cmd_point(int index) {
    return index >= EMS_CMD_P_BAT && index <= EMS_CMD_P_LOWER;
}

#ifdef __cplusplus
extern "C" {
#endif

// 点名数组（下标 = 上面的枚举值）
extern const char* const EMS_POINT_NAMES[EMS_POINT_COUNT];
// 单位数组（与点名一一对应，RT_DB 的 DataPoint.units 用）
extern const char* const EMS_POINT_UNITS[EMS_POINT_COUNT];
// 配置/状态点的默认值（量测点默认 0，由设备侧刷新）
extern const double EMS_POINT_DEFAULTS[EMS_POINT_COUNT];

// ★ 这两个**必须**在 extern "C" 里面。
//   踩过的坑：C 里编出的是未修饰符号，C++ 里若按 C++ 链接（名字修饰）声明，
//   链接期才会报 undefined reference —— 编译期一个字都不说。
const char* ems_zone_name(ems_point_zone_t z);
ems_point_zone_t ems_point_zone_of(int index);

#ifdef __cplusplus
}
#endif

#endif // EMS_POINT_TABLE_H
