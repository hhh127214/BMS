// =====================================================================
// 04/ — 策略管理层 统一数据模型
//
// 依据：docs/接口规范/EMS策略接口规范.md v1.1 §3
// 本文件定义 4 个核心数据模型：Device / Realtime / StrategyResult / Command
//
// 功率符号约定（全系统强制，源自接口规范 §2.1）：
//   P_bat  : 电池功率，放电为正(+)，充电为负(-)
//   P_grid : 关口并网点，进口为正(+)，馈网为负(-)
//   P_load : 本地负荷，正值
//   P_pv   : 光伏出力，正值
//   功率平衡: P_grid = P_load - P_pv - P_bat
//
// 编译：本文件是纯头文件，无需单独编译。
// =====================================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace ems {

// 时间戳（Unix 秒，double 精度 1ms）
using Timestamp = double;

// 控制周期（Cycle）由各策略决定，最快 100 ms
constexpr double kFastCycleS = 0.1;   // 实时控制周期
constexpr double kSlowCycleS = 900.0; // 优化调度周期（默认 15min）

// 默认窗口长度（需量管理）
constexpr double kDefaultDemandWindowS = 900.0;

// =====================================================================
// 1. DeviceData（设备静态/慢变数据）
// =====================================================================

enum class DeviceType {
    kPCS,            // 储能变流器
    kBMS,            // 电池管理系统
    kMeter,          // 电能表
    kPVInverter,     // 光伏逆变器
    kLoad,           // 负荷
    kTransformer,    // 变压器
    kUnknown
};

inline const char* device_type_name(DeviceType t) {
    switch (t) {
        case DeviceType::kPCS:         return "PCS";
        case DeviceType::kBMS:         return "BMS";
        case DeviceType::kMeter:       return "METER";
        case DeviceType::kPVInverter:  return "PV_INVERTER";
        case DeviceType::kLoad:        return "LOAD";
        case DeviceType::kTransformer: return "TRANSFORMER";
        default:                       return "UNKNOWN";
    }
}

struct Device {
    std::string id;                      // 设备唯一标识 e.g. "PCS-01"
    DeviceType  type = DeviceType::kUnknown;
    double      rated_power_kw = 0.0;    // 设备额定功率（正数幅度）
    int         comm_timeout_ms = 1500;  // 通信超时阈值
    std::string install_date;            // ISO date
};

// 设备运行时限制（每个控制周期刷新）
struct DeviceLimits {
    // PCS
    double pcs_rated_chg_kw   = 100.0;   // PCS 额定充电幅度
    double pcs_rated_dis_kw   = 100.0;   // PCS 额定放电幅度
    // BMS
    double bms_chg_limit_kw   = 100.0;   // BMS 允许最大充电功率（动态降功率值）
    double bms_dis_limit_kw   = 100.0;   // BMS 允许最大放电功率
    bool   bms_chg_forbidden  = false;   // BMS 禁止充电
    bool   bms_dis_forbidden  = false;   // BMS 禁止放电
    // 变压器
    double transformer_capacity_kw = 250.0; // 变压器物理容量（kVA 视在功率近似）
    // 需量
    double d_target_kw        = 250.0;   // 契约需量上限（kW）
    Timestamp updated_at = 0.0;
};

// 设备注册表
struct DeviceRegistry {
    std::unordered_map<std::string, Device> devices;

    const Device* find(const std::string& id) const {
        auto it = devices.find(id);
        return (it == devices.end()) ? nullptr : &it->second;
    }

    bool alive(const std::string& id, Timestamp /*now_s*/) const {
        // 简化：实际工程需记录每设备 last_seen 并比较 comm_timeout
        return devices.count(id) > 0;
    }
};

// =====================================================================
// 2. RealtimeData（实时数据快照）
// =====================================================================

enum class TouType {
    kValley,   // 谷时
    kFlat,     // 平时
    kPeak,     // 峰时
    kSharp,    // 尖峰
    kUnknown
};

inline const char* tou_type_name(TouType t) {
    switch (t) {
        case TouType::kValley: return "VALLEY";
        case TouType::kFlat:   return "FLAT";
        case TouType::kPeak:   return "PEAK";
        case TouType::kSharp:  return "SHARP";
        default:               return "UNKNOWN";
    }
}

struct TouInterval {
    double start_s = 0.0;
    double end_s   = 0.0;
    TouType type   = TouType::kUnknown;
    double price   = 0.0;
};

struct RealtimeSnapshot {
    Timestamp timestamp = 0.0;
    double    p_grid_kw = 0.0;       // 关口并网点实时功率（>0 进口, <0 馈网）
    double    p_pv_kw   = 0.0;       // 光伏出力（>0 发电）
    double    p_load_kw = 0.0;       // 本地负荷（>0 用电）
    // 实际电池功率反馈（P_bat 约定：放电为正）。
    // 周期 7 实时闭环用：PCS 执行后的**实测值**回灌下一拍，形成闭环。
    double    p_bat_actual_kw = 0.0;

    // ---- 下一控制拍的负荷/光伏预测量（来自预报曲线，可为空）----
    // 为什么需要前瞻：并网"无倒送"上界由 base = P_load − P_pv 决定，而现场
    //   预报通常是 15 min 阶梯。阶梯跳变会让 base 在一个控制拍内突降十几 kW，
    //   而此时按旧边界下发的指令仍在 PCS 死区/惯性里执行 → 关口瞬时倒送。
    //   安全层取 min(base_now, base_next) 作上界即可消除该穿越。
    // has_lookahead=false 时安全层行为与历史**完全一致**（向后兼容）。
    bool      has_lookahead  = false;
    double    p_load_next_kw = 0.0;
    double    p_pv_next_kw   = 0.0;
    double    soc       = 0.5;       // 当前 SOC [0.0, 1.0]
    double    temperature_c = 25.0;  // 电池温度
    double    soh       = 1.0;       // 电池健康度 [0.0, 1.0]
    std::unordered_map<std::string, bool> meters_alive;

    // 需量窗口状态
    struct DemandWindowState {
        double window_s     = kDefaultDemandWindowS;
        double t_elapsed_s  = 0.0;
        double p_avg_past_kw = 0.0;
    } demand_window;

    // 当前电价窗口
    struct PricingWindow {
        TouType cur_tou_type   = TouType::kUnknown;
        double  cur_tou_price  = 0.0;
        std::vector<TouInterval> tou_prices;
    } pricing;
};

// =====================================================================
// 2.5 BatteryState / GridState —— 设备类型维度的显式视图
//
// 设计方案（§7 周期 2）的字面契约要求把设备状态按**设备类型**分体建模
// （BatteryState / GridState），而不是把 SOC、并网点功率都摊在同一个
// "实时快照"里。04/ 的做法是：**不替换** RealtimeSnapshot / DeviceLimits
// 的既有字段 —— 它们已是全项目的公共契约，改动会波及 05/06/07/08/09/10
// 与 P1/P2/P3 —— 而是把同源字段**显式成体**为只读视图。
//
// 为什么要成体（而不是继续摊平）：
//   1. **扩展性**：现场一旦出现"多并网点 / 多储能柜"，摊平字段会退化成
//      p_grid1_kw / p_grid2_kw … 的命名灾难；成体后按对象聚合即可。
//   2. **可读性**：`battery_state_of(...).soc` 比 `rt.soc` 更能自证
//      "这是电池的状态"；`p_grid_kw` 归到 GridState 后语义边界也清楚了。
//   3. **可测性**：单测可直接构造一个 BatteryState 做边界测试，
//      不必凑齐整个 RealtimeSnapshot。
//
// 纪律：**这里不产生第二份真相**。视图字段一律由 RealtimeSnapshot /
// DeviceLimits 单向映射而来；写入设备状态仍然只走原来那两个结构。
// 故障 / 离线标志由 `DeviceStatus`（P0 设备抽象，见 device_io.h）承载，
// 此处不重复建模，避免"同一事实两处不同步"。
// =====================================================================
struct BatteryState {
    double soc           = 0.5;    // [0,1]
    double soh           = 1.0;    // [0,1]
    double temperature_c = 25.0;
    double p_bat_kw      = 0.0;    // 实测（放电为正，与 P_bat 约定一致）
    double rated_chg_kw  = 0.0;    // PCS 额定充电幅度
    double rated_dis_kw  = 0.0;    // PCS 额定放电幅度
    double chg_limit_kw  = 0.0;    // BMS 允许最大充电功率（动态降额后的值）
    double dis_limit_kw  = 0.0;    // BMS 允许最大放电功率
    bool   chg_forbidden = false;  // BMS 禁止充电（硬安全）
    bool   dis_forbidden = false;  // BMS 禁止放电（硬安全）
    bool   comm_ok       = false;  // BMS 通信是否正常
};

struct GridState {
    double p_grid_kw               = 0.0;   // 并网点功率（>0 进口, <0 馈网）
    double p_load_kw               = 0.0;   // 本地负荷
    double p_pv_kw                 = 0.0;   // 光伏出力
    double transformer_capacity_kw = 0.0;   // 变压器物理容量
    double d_target_kw             = 0.0;   // 契约需量上限
    bool   meter_ok                = true;  // 关口表通信是否正常
};

// 视图构造：逐字段单向映射，不做任何计算与钳位
inline BatteryState battery_state_of(const RealtimeSnapshot& rt, const DeviceLimits& dev) {
    BatteryState bs;
    bs.soc           = rt.soc;
    bs.soh           = rt.soh;
    bs.temperature_c = rt.temperature_c;
    bs.p_bat_kw      = rt.p_bat_actual_kw;
    bs.rated_chg_kw  = dev.pcs_rated_chg_kw;
    bs.rated_dis_kw  = dev.pcs_rated_dis_kw;
    bs.chg_limit_kw  = dev.bms_chg_limit_kw;
    bs.dis_limit_kw  = dev.bms_dis_limit_kw;
    bs.chg_forbidden = dev.bms_chg_forbidden;
    bs.dis_forbidden = dev.bms_dis_forbidden;
    const auto it = rt.meters_alive.find("BMS");
    bs.comm_ok = (it != rt.meters_alive.end()) && it->second;
    return bs;
}

inline GridState grid_state_of(const RealtimeSnapshot& rt, const DeviceLimits& dev) {
    GridState gs;
    gs.p_grid_kw               = rt.p_grid_kw;
    gs.p_load_kw               = rt.p_load_kw;
    gs.p_pv_kw                 = rt.p_pv_kw;
    gs.transformer_capacity_kw = dev.transformer_capacity_kw;
    gs.d_target_kw             = dev.d_target_kw;
    const auto it = rt.meters_alive.find("METER");
    gs.meter_ok = (it == rt.meters_alive.end()) || it->second;
    return gs;
}

// =====================================================================
// 3. StrategyResult（策略输出 + 状态）
// =====================================================================

// 策略优先级 L0-L3（数值越小越高，源自接口规范 §2.3）
//   L0: 硬安全保护（消防、BMS 禁止、变压器跳闸）
//   L1: 运行安全与并网合规（BMS 降功率、变压器过载、防孤岛）
//   L2: 本地经济性（需量、防逆流、光伏平抑）
//   L3: 全局经济性（峰谷套利、需求响应、现货交易、SOC 规划）
enum class Priority : int {
    kL0_Safety      = 0,   // 硬安全
    kL1_SafeOp      = 1,   // 运行安全与并网合规
    kL2_LocalEcon   = 2,   // 本地经济性
    kL3_GlobalEcon  = 3,   // 全局经济性
};

inline const char* priority_name(Priority p) {
    switch (p) {
        case Priority::kL0_Safety:     return "L0_SAFETY";
        case Priority::kL1_SafeOp:     return "L1_SAFEOP";
        case Priority::kL2_LocalEcon:  return "L2_LOCAL";
        case Priority::kL3_GlobalEcon: return "L3_GLOBAL";
        default:                       return "UNKNOWN";
    }
}

// 单个策略的输出（区间 + 期望 + 状态）
struct StrategyResult {
    std::string strategy_id;          // 策略唯一 id
    Priority    priority = Priority::kL3_GlobalEcon;
    bool        active   = false;     // 本拍是否产生动作（区间被收紧或产生 desired）
    double      weight   = 1.0;       // 同层加权权重 [0.0, 1.0]
    double      p_lower  = 0.0;       // 期望下界（P_bat 约定：放电为正，充电为负）
    double      p_upper  = 0.0;       // 期望上界
    double      p_desired = 0.0;      // 期望功率点
    std::string reason;               // 自由文本/枚举字符串，UI/日志用
};

// =====================================================================
// 4. PowerCommand（仲裁后下发指令）
// =====================================================================

struct PowerCommand {
    Timestamp timestamp = 0.0;
    double    p_bat_cmd_kw = 0.0;     // 下发电池功率（P_bat 约定：放电为正）
    double    p_upper = 0.0;          // 最终允许上界（kW，放电方向）
    double    p_lower = 0.0;          // 最终允许下界（kW，充电方向）
    bool      clamped = false;        // desired 是否被裁剪到区间内
    std::string reason;               // 合并后的 reason
    std::vector<std::string> contributing;  // 贡献的策略 id 列表
};

// 9 个策略 id 统一约定
namespace strategy_id {
    constexpr const char* kBmsForbid        = "S01_BMS_FORBID";
    constexpr const char* kBmsDerate        = "S02_BMS_DERATE";
    constexpr const char* kTransformerLim   = "S03_TRANSFORMER_LIMIT";
    constexpr const char* kDemandMgmt       = "S04_DEMAND_MGMT";
    constexpr const char* kAntiReverse      = "S05_ANTI_REVERSE";
    constexpr const char* kPvSmoothing      = "S06_PV_SMOOTHING";
    constexpr const char* kPeakValley       = "S07_PEAK_VALLEY";
    constexpr const char* kForecastOpt      = "S08_FORECAST_OPT";
    constexpr const char* kDemandResponse   = "S09_DEMAND_RESPONSE";
}

} // namespace ems