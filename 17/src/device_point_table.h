// =====================================================================
// 17/ — 设备全点表（**第 1 层**，C4 / B2）
//
// 在整个工程里的位置（见 docs/规划/模拟器与设备接入梳理.md §5.3）：
//
//   第 1 层  设备全点表（本文件）   107 点，忠于厂家/协议点表
//      ↑ 协议适配层（Modbus/IEC104）—— 13/ 已打通主干
//   第 2 层  EMS 抽象视图（07/src/rtdb/ems_point_table.h，40 点）
//      ↑ 算法只认这一层（P0 纪律：点名不许出现在算法里）
//
// 两层之间由 17/src/point_mapping.h 的**代码化映射表**连接 —— 不是文档。
//
// ---------------------------------------------------------------------
// 为什么第 1 层要独立成表，而不是把 40 点表"扩点"：
//
//   ① 40 点表是**算法契约**（RealtimeSnapshot / DeviceLimits 的口径），
//      它的点数是 05/06/08/09/P1/P2/P3 共同依赖的常量。往里塞簇电压、
//      三相电流这种设备细节，等于让算法层直接看见厂家点表 ——
//      换一个 BMS 厂家就要动算法。
//   ② 现场点表是**最容易变**的那一样（C5 原话）。它必须能配置化加载，
//      而算法契约不能跟着站变。两层分开，才能"点表换站、算法不动"。
//   ③ 07/ 的 `ems_point_table.h` 有硬约束："追加在末尾"（索引是运行时坐标）。
//      第 1 层没有这个包袱 —— 它由加载器在运行期构建，索引由点表自己定义。
//
// ---------------------------------------------------------------------
// ★ 本文件**不改** 07/ 的任何东西：40 点表仍是唯一真相源，此处只读引用它。
//
// ---------------------------------------------------------------------
// 点区（zone）的划分与 40 点表**刻意同构**，这样 mapped 时口径一致：
//   MEAS  设备 → 上位机（模拟量量测）
//   STA   设备 → 上位机（数字量状态 / 运行限值）
//   CFG   静态配置（初始化写一次）
//   ALM   告警（level + code 成对）
//   CMD   上位机 → 设备（指令；本表的 PCS 指令点，是 40 点表 CMD 区的**落点**）
//
// 编译：纯头文件（inline）。C++17 / g++ 8.1。
// =====================================================================

#pragma once

#include <cstddef>
#include <string>

namespace ems {
namespace devpt {

// =====================================================================
// 点类型 / 点区 / 设备
// =====================================================================
enum PointType {
    PT_ANALOG  = 0,   // 模拟量（含以工程量表达的枚举值，如 PCS 模式 0..4）
    PT_DIGITAL = 1    // 数字量（0/1 位，或小整数位图）
};

enum PointZone {
    PZ_MEAS = 0,
    PZ_STA  = 1,
    PZ_CFG  = 2,
    PZ_ALM  = 3,
    PZ_CMD  = 4
};

enum DeviceKind {
    DK_BMS   = 0,
    DK_METER = 1,
    DK_PCS   = 2
};

inline const char* point_type_name(int t) {
    switch (t) {
        case PT_ANALOG:  return "analog";
        case PT_DIGITAL: return "digital";
        default:         return "invalid";
    }
}

inline const char* point_zone_name(int z) {
    switch (z) {
        case PZ_MEAS: return "meas";
        case PZ_STA:  return "sta";
        case PZ_CFG:  return "cfg";
        case PZ_ALM:  return "alm";
        case PZ_CMD:  return "cmd";
        default:      return "invalid";
    }
}

inline const char* device_kind_name(int d) {
    switch (d) {
        case DK_BMS:   return "bms";
        case DK_METER: return "meter";
        case DK_PCS:   return "pcs";
        default:       return "invalid";
    }
}

inline int point_type_from_name(const std::string& s) {
    if (s == "analog")  return PT_ANALOG;
    if (s == "digital") return PT_DIGITAL;
    return -1;
}
inline int point_zone_from_name(const std::string& s) {
    if (s == "meas") return PZ_MEAS;
    if (s == "sta")  return PZ_STA;
    if (s == "cfg")  return PZ_CFG;
    if (s == "alm")  return PZ_ALM;
    if (s == "cmd")  return PZ_CMD;
    return -1;
}
inline int device_kind_from_name(const std::string& s) {
    if (s == "bms")   return DK_BMS;
    if (s == "meter") return DK_METER;
    if (s == "pcs")   return DK_PCS;
    return -1;
}

// =====================================================================
// 索引常量：用枚举固定顺序，运行时不做字符串比较
//
// ★ 与 40 点表同款纪律：**新点一律追加在末尾**（见本文件末尾的注释）。
//   一旦在中间插入，其后所有点的索引整体偏移 —— 而"已发布的旧段"/已经
//   记进日志的索引不会跟着变，现场表现为"点名对得上、值全是隔壁点的"。
// =====================================================================
enum {
    // ---------------- BMS（device=bms） ----------------
    // · 簇电压/电流/温度
    DP_BMS_CLUSTER_U = 0,
    DP_BMS_CLUSTER_I,
    DP_BMS_CLUSTER_T,
    // · 单体最高/最低电压与其位置（1 基）
    DP_BMS_CELL_V_MAX,
    DP_BMS_CELL_V_MIN,
    DP_BMS_CELL_V_MAX_IDX,
    DP_BMS_CELL_V_MIN_IDX,
    DP_BMS_CELL_V_DIFF,
    // · 最高/最低温度与其位置（1 基）
    DP_BMS_T_MAX,
    DP_BMS_T_MIN,
    DP_BMS_T_MAX_IDX,
    // · SOC / SOH / 剩余可充放
    DP_BMS_SOC,
    DP_BMS_SOH,
    DP_BMS_RM_CHG_KWH,
    DP_BMS_RM_DIS_KWH,
    // · 绝缘 / 漏电
    DP_BMS_INSUL_R,
    DP_BMS_LEAK_I,
    // · 循环次数 + 允许充放功率 + 禁充放位（安全链最关键的一类）
    DP_BMS_CYCLE_COUNT,
    DP_BMS_CHG_LIMIT_KW,
    DP_BMS_DIS_LIMIT_KW,
    DP_BMS_CHG_FORBID,
    DP_BMS_DIS_FORBID,
    DP_BMS_FORBID_REASON,
    // · 接触器 / 运行位
    DP_BMS_MAIN_POS,
    DP_BMS_MAIN_NEG,
    DP_BMS_CHARGING,
    // · 通信
    DP_BMS_COMM_OK,
    DP_BMS_HEARTBEAT,
    DP_BMS_DATA_VALID,
    // · 告警字（level + code 成对）
    DP_BMS_ALM_LEVEL,
    DP_BMS_ALM_CODE,
    // · 配置
    DP_BMS_CAP_KWH,
    DP_BMS_SOC_MIN,
    DP_BMS_SOC_MAX,
    DP_BMS_ETA_CHG,
    DP_BMS_ETA_DIS,
    DP_BMS_CLUSTER_N,
    DP_BMS_CELLS_PER_CLUSTER,

    // ---------------- 关口电表（device=meter） ----------------
    // · 三相电压（相 / 线）
    DP_METER_U_A,
    DP_METER_U_B,
    DP_METER_U_C,
    DP_METER_U_AB,
    DP_METER_U_BC,
    DP_METER_U_CA,
    // · 三相电流 + 零序
    DP_METER_I_A,
    DP_METER_I_B,
    DP_METER_I_C,
    DP_METER_I_N,
    // · 频率
    DP_METER_FREQ,
    // · 有功 / 无功 / 视在 / 功率因数（总 + 分相）
    DP_METER_P_TOTAL,
    DP_METER_Q_TOTAL,
    DP_METER_S_TOTAL,
    DP_METER_PF,
    DP_METER_P_A,
    DP_METER_P_B,
    DP_METER_P_C,
    // · 分路（负荷 / 光伏）—— 40 点表的 P_LOAD / P_PV 的落点
    DP_METER_P_LOAD,
    DP_METER_P_PV,
    // · 电能质量
    DP_METER_THD_U,
    DP_METER_THD_I,
    // · 正反向电能（累计 kWh / kvarh）+ 日电能
    DP_METER_EP_FWD,
    DP_METER_EP_REV,
    DP_METER_EQ_FWD,
    DP_METER_EQ_REV,
    DP_METER_EP_DAY,
    // · 需量（当前 + 峰值 + 发生时刻）
    DP_METER_DEMAND_NOW,
    DP_METER_DEMAND_PEAK,
    DP_METER_DEMAND_PEAK_TS,
    // · 状态
    DP_METER_COMM_OK,
    DP_METER_DATA_VALID,
    DP_METER_FAULT,
    // · 配置（变电站一次侧参数）
    DP_METER_TR_KVA,
    DP_METER_D_TARGET,

    // ---------------- PCS（device=pcs） ----------------
    // · 有功 / 无功 / 视在 / 功率因数
    DP_PCS_P_ACT,
    DP_PCS_Q_ACT,
    DP_PCS_S_ACT,
    DP_PCS_PF,
    // · 直流侧
    DP_PCS_UDC,
    DP_PCS_IDC,
    // · 交流侧三相
    DP_PCS_U_A,
    DP_PCS_U_B,
    DP_PCS_U_C,
    DP_PCS_I_A,
    DP_PCS_I_B,
    DP_PCS_I_C,
    DP_PCS_FREQ,
    // · 日 / 累计发电量 + 运行小时
    DP_PCS_E_DAY_KWH,
    DP_PCS_E_TOTAL_KWH,
    DP_PCS_RUN_HOURS,
    // · 运行模式状态机 + 状态
    DP_PCS_MODE,
    DP_PCS_FAULT,
    DP_PCS_COMM_OK,
    DP_PCS_DATA_VALID,
    DP_PCS_LIMIT_REASON,
    DP_PCS_MAX_CHG_KW,
    DP_PCS_MAX_DIS_KW,
    // · 告警 / 故障码
    DP_PCS_ALM_LEVEL,
    DP_PCS_FAULT_CODE,
    // · 指令（40 点表 CMD 区的落点）
    DP_PCS_CMD_P_SET,
    DP_PCS_CMD_Q_SET,
    DP_PCS_CMD_P_UPPER,
    DP_PCS_CMD_P_LOWER,
    DP_PCS_CMD_ONOFF,
    // · 配置
    DP_PCS_TAU_S,
    DP_PCS_RAMP,
    DP_PCS_STANDBY,
    DP_PCS_RATED_Q_KVAR,

    // ---- 追加点区（换站新增点位一律从这里往下加）----
    // ★ 为什么留这个显式锚点：与 40 点表"追加在末尾"是同一纪律，
    //   但这里把它做成**可被断言的名字** —— 新点从 DP_APPEND_BEGIN 起，
    //   测试里钉住内置表的点数 == DP_APPEND_BEGIN，
    //   避免后人图省事把新点插进 BMS/METER/PCS 段中间。
    DP_APPEND_BEGIN = DP_PCS_RATED_Q_KVAR + 1,

    DP_POINT_COUNT = DP_APPEND_BEGIN
};

// =====================================================================
// 点的定义（POD，编译期内置表用）
//
// 每点必备八项（C4 的硬要求）：
//   点名 / 单位 / 类型 / 量程下限 / 量程上限 / 默认值 / 所属设备 / 点区
// =====================================================================
struct DevicePointDef {
    const char* name;
    const char* unit;
    int         type;
    double      range_min;
    double      range_max;
    double      def;
    int         device;
    int         zone;
};

// ---------------------------------------------------------------------
// 内置默认点表（**点表真相源**）
//
// 为什么用"数组 + 枚举"而不是运行时构造：与 40 点表同构 ——
// 编译期就能对齐索引与下标；运行时的可加载表（point_table_loader.h）
// 以内置表为初值构建，导出的 JSON 就是这张表。
//
// 单位的取法（★ 一处容易踩的坑）：
//   与 40 点表**直接映射**的配置点，单位必须与抽象表**逐字相同**，
//   否则 self_check 的 ③ 会红。典型例子：抽象表 CFG.SOC_PHYS_MIN 单位是
//   "-"（比例 0.05），所以本表的 CFG.BMS.SOC_MIN 也必须是 "-" 且量程 [0,1]，
//   **不能**图好看写成 "%" 与 5..95 —— 那会让"单位一致"这条校验形同虚设。
//   量测点则按工程单位（MEAS.BMS.SOC 用 "%"，与抽象表 MEAS.SOC 一致）。
// ---------------------------------------------------------------------
inline const DevicePointDef kBuiltinPoints[DP_POINT_COUNT] = {
    // ============================ BMS ============================
    // MEAS
    {"MEAS.BMS.CLUSTER_U",        "V",    PT_ANALOG,      0.0,   1500.0,    0.00, DK_BMS, PZ_MEAS},
    {"MEAS.BMS.CLUSTER_I",        "A",    PT_ANALOG,  -1000.0,   1000.0,    0.00, DK_BMS, PZ_MEAS},
    {"MEAS.BMS.CLUSTER_T",        "degC", PT_ANALOG,    -40.0,    125.0,   25.00, DK_BMS, PZ_MEAS},
    {"MEAS.BMS.CELL_V_MAX",       "V",    PT_ANALOG,      0.0,      5.0,    3.35, DK_BMS, PZ_MEAS},
    {"MEAS.BMS.CELL_V_MIN",       "V",    PT_ANALOG,      0.0,      5.0,    3.30, DK_BMS, PZ_MEAS},
    {"MEAS.BMS.CELL_V_MAX_IDX",   "-",    PT_ANALOG,      1.0,    512.0,    1.00, DK_BMS, PZ_MEAS},
    {"MEAS.BMS.CELL_V_MIN_IDX",   "-",    PT_ANALOG,      1.0,    512.0,    1.00, DK_BMS, PZ_MEAS},
    {"MEAS.BMS.CELL_V_DIFF",      "V",    PT_ANALOG,      0.0,      5.0,    0.05, DK_BMS, PZ_MEAS},
    {"MEAS.BMS.T_MAX",            "degC", PT_ANALOG,    -40.0,    125.0,   25.00, DK_BMS, PZ_MEAS},
    {"MEAS.BMS.T_MIN",            "degC", PT_ANALOG,    -40.0,    125.0,   25.00, DK_BMS, PZ_MEAS},
    {"MEAS.BMS.T_MAX_IDX",        "-",    PT_ANALOG,      1.0,    512.0,    1.00, DK_BMS, PZ_MEAS},
    {"MEAS.BMS.SOC",              "%",    PT_ANALOG,      0.0,    100.0,   50.00, DK_BMS, PZ_MEAS},
    {"MEAS.BMS.SOH",              "%",    PT_ANALOG,      0.0,    100.0,  100.00, DK_BMS, PZ_MEAS},
    {"MEAS.BMS.RM_CHG_KWH",       "kWh",  PT_ANALOG,      0.0, 100000.0,    0.00, DK_BMS, PZ_MEAS},
    {"MEAS.BMS.RM_DIS_KWH",       "kWh",  PT_ANALOG,      0.0, 100000.0,    0.00, DK_BMS, PZ_MEAS},
    {"MEAS.BMS.INSUL_R",          "kohm", PT_ANALOG,      0.0, 100000.0, 1000.00, DK_BMS, PZ_MEAS},
    {"MEAS.BMS.LEAK_I",           "mA",   PT_ANALOG,      0.0,  10000.0,    0.00, DK_BMS, PZ_MEAS},
    // STA
    {"STA.BMS.CYCLE_COUNT",       "-",    PT_ANALOG,      0.0,  65535.0,    0.00, DK_BMS, PZ_STA},
    {"STA.BMS.CHG_LIMIT_KW",      "kW",   PT_ANALOG,      0.0, 100000.0,    0.00, DK_BMS, PZ_STA},
    {"STA.BMS.DIS_LIMIT_KW",      "kW",   PT_ANALOG,      0.0, 100000.0,    0.00, DK_BMS, PZ_STA},
    {"STA.BMS.CHG_FORBID",        "bool", PT_DIGITAL,     0.0,      1.0,    0.00, DK_BMS, PZ_STA},
    {"STA.BMS.DIS_FORBID",        "bool", PT_DIGITAL,     0.0,      1.0,    0.00, DK_BMS, PZ_STA},
    {"STA.BMS.FORBID_REASON",     "-",    PT_DIGITAL,     0.0,   9999.0,    0.00, DK_BMS, PZ_STA},
    {"STA.BMS.MAIN_POS",          "bool", PT_DIGITAL,     0.0,      1.0,    1.00, DK_BMS, PZ_STA},
    {"STA.BMS.MAIN_NEG",          "bool", PT_DIGITAL,     0.0,      1.0,    1.00, DK_BMS, PZ_STA},
    {"STA.BMS.CHARGING",          "bool", PT_DIGITAL,     0.0,      1.0,    0.00, DK_BMS, PZ_STA},
    {"STA.BMS.COMM_OK",           "bool", PT_DIGITAL,     0.0,      1.0,    1.00, DK_BMS, PZ_STA},
    {"STA.BMS.HEARTBEAT",         "-",    PT_DIGITAL,     0.0, 4000000000.0, 0.00, DK_BMS, PZ_STA},
    {"STA.BMS.DATA_VALID",        "bool", PT_DIGITAL,     0.0,      1.0,    1.00, DK_BMS, PZ_STA},
    // ALM
    {"ALM.BMS.LEVEL",             "-",    PT_DIGITAL,     0.0,      3.0,    0.00, DK_BMS, PZ_ALM},
    {"ALM.BMS.CODE",              "-",    PT_DIGITAL,     0.0,   9999.0,    0.00, DK_BMS, PZ_ALM},
    // CFG
    {"CFG.BMS.CAP_KWH",           "kWh",  PT_ANALOG,      0.0, 100000.0, 1000.00, DK_BMS, PZ_CFG},
    {"CFG.BMS.SOC_MIN",           "-",    PT_ANALOG,      0.0,      1.0,    0.05, DK_BMS, PZ_CFG},
    {"CFG.BMS.SOC_MAX",           "-",    PT_ANALOG,      0.0,      1.0,    0.95, DK_BMS, PZ_CFG},
    {"CFG.BMS.ETA_CHG",           "-",    PT_ANALOG,      0.0,      1.0,    0.95, DK_BMS, PZ_CFG},
    {"CFG.BMS.ETA_DIS",           "-",    PT_ANALOG,      0.0,      1.0,    0.95, DK_BMS, PZ_CFG},
    {"CFG.BMS.CLUSTER_N",         "-",    PT_ANALOG,      1.0,     64.0,    3.00, DK_BMS, PZ_CFG},
    {"CFG.BMS.CELLS_PER_CLUSTER", "-",    PT_ANALOG,      1.0,    512.0,    8.00, DK_BMS, PZ_CFG},

    // ========================== 关口电表 ==========================
    // MEAS
    {"MEAS.METER.U_A",            "V",    PT_ANALOG,      0.0,    600.0,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.U_B",            "V",    PT_ANALOG,      0.0,    600.0,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.U_C",            "V",    PT_ANALOG,      0.0,    600.0,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.U_AB",           "V",    PT_ANALOG,      0.0,    600.0,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.U_BC",           "V",    PT_ANALOG,      0.0,    600.0,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.U_CA",           "V",    PT_ANALOG,      0.0,    600.0,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.I_A",            "A",    PT_ANALOG,      0.0,   2000.0,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.I_B",            "A",    PT_ANALOG,      0.0,   2000.0,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.I_C",            "A",    PT_ANALOG,      0.0,   2000.0,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.I_N",            "A",    PT_ANALOG,      0.0,    200.0,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.FREQ",           "Hz",   PT_ANALOG,     45.0,     65.0,   50.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.P_TOTAL",        "kW",   PT_ANALOG, -1.0e5,    1.0e5,     0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.Q_TOTAL",        "kvar", PT_ANALOG, -1.0e5,    1.0e5,     0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.S_TOTAL",        "kVA",  PT_ANALOG,      0.0,    1.0e5,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.PF",             "-",    PT_ANALOG,     -1.0,      1.0,    1.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.P_A",            "kW",   PT_ANALOG, -1.0e5,    1.0e5,     0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.P_B",            "kW",   PT_ANALOG, -1.0e5,    1.0e5,     0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.P_C",            "kW",   PT_ANALOG, -1.0e5,    1.0e5,     0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.P_LOAD",         "kW",   PT_ANALOG, -1.0e5,    1.0e5,     0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.P_PV",           "kW",   PT_ANALOG, -1.0e5,    1.0e5,     0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.THD_U",          "%",    PT_ANALOG,      0.0,    100.0,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.THD_I",          "%",    PT_ANALOG,      0.0,    100.0,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.EP_FWD",         "kWh",  PT_ANALOG,      0.0,    1.0e9,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.EP_REV",         "kWh",  PT_ANALOG,      0.0,    1.0e9,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.EQ_FWD",         "kvarh",PT_ANALOG,      0.0,    1.0e9,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.EQ_REV",         "kvarh",PT_ANALOG,      0.0,    1.0e9,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.EP_DAY",         "kWh",  PT_ANALOG,      0.0,    1.0e6,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.DEMAND_NOW",     "kW",   PT_ANALOG,      0.0,    1.0e6,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.DEMAND_PEAK",    "kW",   PT_ANALOG,      0.0,    1.0e6,    0.00, DK_METER, PZ_MEAS},
    {"MEAS.METER.DEMAND_PEAK_TS", "s",    PT_ANALOG,      0.0,    1.0e9,    0.00, DK_METER, PZ_MEAS},
    // STA
    {"STA.METER.COMM_OK",         "bool", PT_DIGITAL,     0.0,      1.0,    1.00, DK_METER, PZ_STA},
    {"STA.METER.DATA_VALID",      "bool", PT_DIGITAL,     0.0,      1.0,    1.00, DK_METER, PZ_STA},
    {"STA.METER.FAULT",           "bool", PT_DIGITAL,     0.0,      1.0,    0.00, DK_METER, PZ_STA},
    // CFG
    {"CFG.METER.TR_KVA",          "kVA",  PT_ANALOG,      0.0,    1.0e5,  250.00, DK_METER, PZ_CFG},
    {"CFG.METER.D_TARGET",        "kW",   PT_ANALOG,      0.0,    1.0e6,  250.00, DK_METER, PZ_CFG},

    // ============================= PCS =============================
    // MEAS
    {"MEAS.PCS.P_ACT",            "kW",   PT_ANALOG, -1.0e6,    1.0e6,     0.00, DK_PCS, PZ_MEAS},
    {"MEAS.PCS.Q_ACT",            "kvar", PT_ANALOG, -1.0e6,    1.0e6,     0.00, DK_PCS, PZ_MEAS},
    {"MEAS.PCS.S_ACT",            "kVA",  PT_ANALOG,      0.0,    1.0e6,    0.00, DK_PCS, PZ_MEAS},
    {"MEAS.PCS.PF",               "-",    PT_ANALOG,     -1.0,      1.0,    1.00, DK_PCS, PZ_MEAS},
    {"MEAS.PCS.UDC",              "V",    PT_ANALOG,      0.0,   1500.0,    0.00, DK_PCS, PZ_MEAS},
    {"MEAS.PCS.IDC",              "A",    PT_ANALOG,  -3000.0,   3000.0,    0.00, DK_PCS, PZ_MEAS},
    {"MEAS.PCS.U_A",              "V",    PT_ANALOG,      0.0,    600.0,    0.00, DK_PCS, PZ_MEAS},
    {"MEAS.PCS.U_B",              "V",    PT_ANALOG,      0.0,    600.0,    0.00, DK_PCS, PZ_MEAS},
    {"MEAS.PCS.U_C",              "V",    PT_ANALOG,      0.0,    600.0,    0.00, DK_PCS, PZ_MEAS},
    {"MEAS.PCS.I_A",              "A",    PT_ANALOG,      0.0,   2000.0,    0.00, DK_PCS, PZ_MEAS},
    {"MEAS.PCS.I_B",              "A",    PT_ANALOG,      0.0,   2000.0,    0.00, DK_PCS, PZ_MEAS},
    {"MEAS.PCS.I_C",              "A",    PT_ANALOG,      0.0,   2000.0,    0.00, DK_PCS, PZ_MEAS},
    {"MEAS.PCS.FREQ",             "Hz",   PT_ANALOG,     45.0,     65.0,   50.00, DK_PCS, PZ_MEAS},
    {"MEAS.PCS.E_DAY_KWH",        "kWh",  PT_ANALOG,      0.0,    1.0e6,    0.00, DK_PCS, PZ_MEAS},
    {"MEAS.PCS.E_TOTAL_KWH",      "kWh",  PT_ANALOG,      0.0,    1.0e9,    0.00, DK_PCS, PZ_MEAS},
    {"MEAS.PCS.RUN_HOURS",        "h",    PT_ANALOG,      0.0,    1.0e6,    0.00, DK_PCS, PZ_MEAS},
    // STA
    {"STA.PCS.MODE",              "-",    PT_ANALOG,      0.0,      4.0,    0.00, DK_PCS, PZ_STA},
    {"STA.PCS.FAULT",             "bool", PT_DIGITAL,     0.0,      1.0,    0.00, DK_PCS, PZ_STA},
    {"STA.PCS.COMM_OK",           "bool", PT_DIGITAL,     0.0,      1.0,    1.00, DK_PCS, PZ_STA},
    {"STA.PCS.DATA_VALID",        "bool", PT_DIGITAL,     0.0,      1.0,    1.00, DK_PCS, PZ_STA},
    {"STA.PCS.LIMIT_REASON",      "-",    PT_DIGITAL,     0.0,   9999.0,    0.00, DK_PCS, PZ_STA},
    {"STA.PCS.MAX_CHG_KW",        "kW",   PT_ANALOG,      0.0,    1.0e6,  200.00, DK_PCS, PZ_STA},
    {"STA.PCS.MAX_DIS_KW",        "kW",   PT_ANALOG,      0.0,    1.0e6,  200.00, DK_PCS, PZ_STA},
    // ALM
    {"ALM.PCS.LEVEL",             "-",    PT_DIGITAL,     0.0,      3.0,    0.00, DK_PCS, PZ_ALM},
    {"ALM.PCS.FAULT_CODE",        "-",    PT_DIGITAL,     0.0,   9999.0,    0.00, DK_PCS, PZ_ALM},
    // CMD
    {"CMD.PCS.P_SET",             "kW",   PT_ANALOG, -1.0e6,    1.0e6,     0.00, DK_PCS, PZ_CMD},
    {"CMD.PCS.Q_SET",             "kvar", PT_ANALOG, -1.0e6,    1.0e6,     0.00, DK_PCS, PZ_CMD},
    {"CMD.PCS.P_UPPER",           "kW",   PT_ANALOG, -1.0e6,    1.0e6,     0.00, DK_PCS, PZ_CMD},
    {"CMD.PCS.P_LOWER",           "kW",   PT_ANALOG, -1.0e6,    1.0e6,     0.00, DK_PCS, PZ_CMD},
    {"CMD.PCS.ONOFF",             "bool", PT_DIGITAL,     0.0,      1.0,    1.00, DK_PCS, PZ_CMD},
    // CFG
    {"CFG.PCS.TAU_S",             "s",    PT_ANALOG,      0.0,    600.0,    0.50, DK_PCS, PZ_CFG},
    {"CFG.PCS.RAMP_KW_PER_S",     "kW/s", PT_ANALOG,      0.0,    1.0e10, 400.00, DK_PCS, PZ_CFG},
    {"CFG.PCS.STANDBY_KW",        "kW",   PT_ANALOG,      0.0,   1000.0,    2.00, DK_PCS, PZ_CFG},
    {"CFG.PCS.RATED_Q_KVAR",      "kvar", PT_ANALOG,      0.0,    1.0e6,  200.00, DK_PCS, PZ_CFG}
};

// =====================================================================
// 便捷访问
// =====================================================================
inline const char* dev_point_name(int index) {
    if (index < 0 || index >= DP_POINT_COUNT) return "";
    return kBuiltinPoints[index].name;
}

// 按名查索引（内置表）。找不到返回 -1。
// ★ 找不到**不能**退化成"返回 0 号点" —— 那是采了个假值的经典写法。
inline int dev_point_index_of(const char* name) {
    if (name == nullptr) return -1;
    for (int i = 0; i < DP_POINT_COUNT; ++i) {
        const char* n = kBuiltinPoints[i].name;
        if (n == nullptr) continue;
        const char* a = n;
        const char* b = name;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == '\0' && *b == '\0') return i;
    }
    return -1;
}

inline int dev_point_index_of(const std::string& name) {
    return dev_point_index_of(name.c_str());
}

} // namespace devpt
} // namespace ems
