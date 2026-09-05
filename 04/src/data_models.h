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