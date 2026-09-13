// =====================================================================
// P1/ — 产品化 P1 配置化 · 统一配置模型
//
// 目标（产品化 P1）：**现场部署不改源码，只改配置文件。**
//
// 在 P1 之前，一个部署点的参数散落在 7 个结构体里，而且装配顺序有坑：
//
//     rt.configure_plant(cfg.plant);   // ← 内部 refresh_device_limits() 会重置 dev_
//     rt.device_limits() = cfg.limits; // ← 必须在 configure_plant **之后**
//     rt.safety_params() = cfg.safety;
//     rt.coordinator_config() = cfg.coord;
//     rt.fsm_config() = cfg.fsm;
//     rt.apply_configs();              // ← 最后统一生效
//
// 10/ 里为此专门写了"顺序要紧"的注释 —— 这说明**顺序知识散落在调用方**，
// 每个新的装配点都要重新踩一遍。P1 把这份知识收进 `apply_config()` 一处。
//
// ---------------------------------------------------------------------
// 为什么用「字段绑定表」而不是手写 to_json / from_json
// ---------------------------------------------------------------------
// 手写两份映射（读一份、写一份）必然漂移：加了字段忘了写其中一份，
// 表现是"配置里改了但没生效"或"导出的配置少了字段"，且**编译期不报错**。
// 本模块用一张绑定表同时驱动 load 与 save，字段只声明一次：
//   · 漏字段 → 该字段无法被配置（但导出的配置里也不会出现，行为一致）
//   · 字段表与结构体**编译期绑定**（取地址），改名即编译失败
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "json_lite.h"

// ---- 运行时各层的参数结构体（P1 只做"搬运 + 校验"，不重新定义它们）----
#include "data_models.h"          // DeviceLimits
#include "plant_model.h"          // PlantConfig
#include "realtime_loop.h"        // LoopConfig
#include "safety_engine.h"        // SafetyParams
#include "dispatch_coordinator.h" // CoordinatorConfig
#include "state_machine.h"        // StateMachineConfig

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ems {

// =====================================================================
// 字段绑定器
//
// 一个 Field = 「JSON 键名」+「写指针」+「读闭包」，load / save 共用。
// unit / desc 用于自动生成配置文档（`--dump-schema`），避免文档与代码漂移。
// =====================================================================
class Binder {
public:
    struct Field {
        std::string key;
        std::string unit;
        std::string desc;
        bool        required = false;
        // 返回 false 表示类型不符；why 填原因
        std::function<bool(const json::Value&, std::string*)> set;
        std::function<json::Value()>                          get;
    };

    // ---- 标量 ----
    void dbl(const std::string& key, double* p,
             const std::string& unit = "", const std::string& desc = "") {
        Field f;
        f.key = key; f.unit = unit; f.desc = desc;
        f.set = [p](const json::Value& v, std::string* why) {
            if (v.is_number()) { *p = v.num; return true; }
            // 容忍 "0.5" 这种带引号的数字：配置文件里常见（Excel 导出/手抄）
            if (v.is_string()) {
                try {
                    size_t used = 0;
                    const double d = std::stod(v.str, &used);
                    if (used == v.str.size()) { *p = d; return true; }
                } catch (...) {}
            }
            if (why) *why = std::string("期待数字，实际是 ") + v.type_name();
            return false;
        };
        f.get = [p]() { return json::Value(*p); };
        fields_.push_back(std::move(f));
    }

    void integer(const std::string& key, int* p,
                 const std::string& unit = "", const std::string& desc = "") {
        Field f;
        f.key = key; f.unit = unit; f.desc = desc;
        f.set = [p](const json::Value& v, std::string* why) {
            if (v.is_number()) { *p = static_cast<int>(std::lround(v.num)); return true; }
            if (v.is_string()) {
                try {
                    size_t used = 0;
                    const int i = std::stoi(v.str, &used);
                    if (used == v.str.size()) { *p = i; return true; }
                } catch (...) {}
            }
            if (why) *why = std::string("期待整数，实际是 ") + v.type_name();
            return false;
        };
        f.get = [p]() { return json::Value(*p); };
        fields_.push_back(std::move(f));
    }

    void u32(const std::string& key, unsigned int* p,
             const std::string& unit = "", const std::string& desc = "") {
        Field f;
        f.key = key; f.unit = unit; f.desc = desc;
        f.set = [p](const json::Value& v, std::string* why) {
            if (v.is_number() && v.num >= 0.0) {
                *p = static_cast<unsigned int>(std::llround(v.num));
                return true;
            }
            if (why) *why = std::string("期待非负整数，实际是 ") + v.type_name();
            return false;
        };
        f.get = [p]() { return json::Value(static_cast<double>(*p)); };
        fields_.push_back(std::move(f));
    }

    void flag(const std::string& key, bool* p, const std::string& desc = "") {
        Field f;
        f.key = key; f.desc = desc;
        f.set = [p](const json::Value& v, std::string* why) {
            if (v.is_bool()) { *p = v.b; return true; }
            // 容忍 "true"/"false"/"on"/"off"/1/0 —— 现场配置文件经常从别处抄
            if (v.is_number()) { *p = (v.num != 0.0); return true; }
            if (v.is_string()) {
                const std::string s = v.str;
                if (s == "true" || s == "on" || s == "yes" || s == "1")  { *p = true;  return true; }
                if (s == "false" || s == "off" || s == "no" || s == "0") { *p = false; return true; }
            }
            if (why) *why = std::string("期待布尔值，实际是 ") + v.type_name();
            return false;
        };
        f.get = [p]() { return json::Value(*p); };
        fields_.push_back(std::move(f));
    }

    void str(const std::string& key, std::string* p, const std::string& desc = "") {
        Field f;
        f.key = key; f.desc = desc;
        f.set = [p](const json::Value& v, std::string* why) {
            if (v.is_string()) { *p = v.str; return true; }
            if (why) *why = std::string("期待字符串，实际是 ") + v.type_name();
            return false;
        };
        f.get = [p]() { return json::Value(*p); };
        fields_.push_back(std::move(f));
    }

    const std::vector<Field>& fields() const { return fields_; }

    // 标记某字段为「关键项」：缺省时不报错，但给出提示（用默认值）。
    // 用在"少配了会静默跑偏"的参数上，例如 dt_s / 容量 / 需量上限。
    void require(const std::string& key) {
        for (auto& f : fields_) {
            if (f.key == key) { f.required = true; return; }
        }
    }

private:
    std::vector<Field> fields_;
};

// =====================================================================
// 各结构体的字段绑定（只声明一次，load/save 共用）
// =====================================================================

// ---- 07/ 实时循环 ----
inline void bind_fields(Binder& b, LoopConfig& c) {
    b.dbl("dt_s", &c.dt_s, "s", "控制周期（现场 0.1 s；离线仿真可放宽）");
    b.flag("enable_state_machine", &c.enable_state_machine, "启用状态机门控（现场必须 true）");
    b.flag("enable_safety_engine", &c.enable_safety_engine, "启用安全约束引擎（现场必须 true）");
    b.flag("enable_realtime_correction", &c.enable_realtime_correction, "启用 L2 实时纠偏");
    b.flag("hold_last_on_comm_loss", &c.hold_last_on_comm_loss, "采集超时 → 保持上一拍指令");
    b.dbl("comm_stale_threshold_s", &c.comm_stale_threshold_s, "s", "快照停滞多久算通信超时");
    b.dbl("demand_window_s", &c.demand_window_s, "s", "需量计量窗口（国内 900 s）");
    b.dbl("l2_correction_max_kw", &c.l2_correction_max_kw, "kW", "L2 纠偏权限上限（有界纠偏）");
    b.flag("enable_output_shaper", &c.enable_output_shaper, "启用输出整形（死区 + 方向滞环）");
    b.dbl("output_deadband_kw", &c.output_deadband_kw, "kW", "输出死区");
    b.integer("output_switch_delay", &c.output_switch_delay, "拍", "方向切换确认拍数");
    b.integer("log_every", &c.log_every, "拍", "日志降采样：每 N 拍记一条");
    b.flag("enable_log", &c.enable_log, "启用日志");
    b.require("dt_s");
    b.require("demand_window_s");
}

// ---- 07/ 被控对象（仿真适配器用；现场由真实设备提供）----
inline void bind_fields(Binder& b, PlantConfig& c) {
    b.dbl("battery_capacity_kwh", &c.battery_capacity_kwh, "kWh", "电池额定容量");
    b.dbl("soc_init", &c.soc_init, "1", "初始 SOC");
    b.dbl("soc_phys_min", &c.soc_phys_min, "1", "物理下限（BMS 保护动作点）");
    b.dbl("soc_phys_max", &c.soc_phys_max, "1", "物理上限");
    b.dbl("eta_chg", &c.eta_chg, "1", "充电效率");
    b.dbl("eta_dis", &c.eta_dis, "1", "放电效率");
    b.dbl("soh", &c.soh, "1", "健康度");
    b.dbl("pcs_max_chg_kw", &c.pcs_max_chg_kw, "kW", "PCS 物理充电能力");
    b.dbl("pcs_max_dis_kw", &c.pcs_max_dis_kw, "kW", "PCS 物理放电能力");
    b.dbl("pcs_ramp_kw_per_s", &c.pcs_ramp_kw_per_s, "kW/s", "PCS 物理变化率上限");
    b.dbl("pcs_tau_s", &c.pcs_tau_s, "s", "PCS 一阶惯性时间常数");
    b.dbl("pcs_deadtime_s", &c.pcs_deadtime_s, "s", "PCS 执行死区时间");
    b.dbl("pcs_standby_kw", &c.pcs_standby_kw, "kW", "PCS 空载损耗（计入负荷侧）");
    b.dbl("temp_ambient_c", &c.temp_ambient_c, "degC", "环境温度");
    b.dbl("temp_heat_coef", &c.temp_heat_coef, "1", "温升系数");
    b.dbl("temp_cool_coef", &c.temp_cool_coef, "1", "散热系数");
    b.dbl("noise_kw", &c.noise_kw, "kW", "功率量测噪声标准差");
    b.dbl("noise_soc", &c.noise_soc, "1", "SOC 量测噪声标准差");
    b.u32("seed", &c.seed, "1", "随机种子（可复现）");
    // 故障注入项：现场不写（默认全正常），测试/演示用
    b.flag("comm_ok_bms", &c.comm_ok_bms, "BMS 通信正常");
    b.flag("comm_ok_meter", &c.comm_ok_meter, "关口电表通信正常");
    b.flag("comm_ok_pcs", &c.comm_ok_pcs, "PCS 通信正常");
    b.flag("pcs_fault", &c.pcs_fault, "PCS 故障（停止出力）");
    b.flag("device_offline", &c.device_offline, "设备离线");
    b.flag("data_valid", &c.data_valid, "数据有效性");
    b.require("battery_capacity_kwh");
    b.require("soc_init");
    b.require("pcs_max_chg_kw");
    b.require("pcs_max_dis_kw");
}

// ---- 04/ 设备限制（现场由 BMS/PCS/电表实时给出）----
inline void bind_fields(Binder& b, DeviceLimits& c) {
    b.dbl("pcs_rated_chg_kw", &c.pcs_rated_chg_kw, "kW", "PCS 额定充电幅度");
    b.dbl("pcs_rated_dis_kw", &c.pcs_rated_dis_kw, "kW", "PCS 额定放电幅度");
    b.dbl("bms_chg_limit_kw", &c.bms_chg_limit_kw, "kW", "BMS 允许最大充电功率（动态降功率值）");
    b.dbl("bms_dis_limit_kw", &c.bms_dis_limit_kw, "kW", "BMS 允许最大放电功率");
    b.flag("bms_chg_forbidden", &c.bms_chg_forbidden, "BMS 禁止充电");
    b.flag("bms_dis_forbidden", &c.bms_dis_forbidden, "BMS 禁止放电");
    b.dbl("transformer_capacity_kw", &c.transformer_capacity_kw, "kVA", "变压器容量");
    b.dbl("d_target_kw", &c.d_target_kw, "kW", "契约需量上限");
    b.require("transformer_capacity_kw");
    b.require("d_target_kw");
    b.require("pcs_rated_dis_kw");
}

// ---- 05/ 安全约束 ----
inline void bind_fields(Binder& b, SafetyParams& c) {
    b.dbl("soc_min", &c.soc_min, "1", "SOC 绝对下限（触及禁放）");
    b.dbl("soc_max", &c.soc_max, "1", "SOC 绝对上限（触及禁充）");
    b.dbl("soc_warn_low", &c.soc_warn_low, "1", "SOC 预警下限（进入后降功率）");
    b.dbl("soc_warn_high", &c.soc_warn_high, "1", "SOC 预警上限");
    b.dbl("soc_hysteresis", &c.soc_hysteresis, "1", "预警解除回差");
    b.dbl("soc_warn_derate", &c.soc_warn_derate, "1", "预警区功率折减系数");
    b.dbl("temp_warn_c", &c.temp_warn_c, "degC", "预警温度");
    b.dbl("temp_fault_c", &c.temp_fault_c, "degC", "故障温度（禁充放）");
    b.dbl("temp_hysteresis_c", &c.temp_hysteresis_c, "degC", "温度解除回差");
    b.dbl("temp_warn_derate", &c.temp_warn_derate, "1", "高温区折减系数");
    b.dbl("pcs_derate_ratio", &c.pcs_derate_ratio, "1", "额外折减系数（检修/老化）");
    b.dbl("tr_overload_th", &c.tr_overload_th, "1", "变压器轻度过载阈值（负载率）");
    b.dbl("tr_extreme_th", &c.tr_extreme_th, "1", "变压器极端过载阈值");
    b.dbl("tr_hysteresis", &c.tr_hysteresis, "1", "过载解除回差");
    b.dbl("tr_load_pv_share", &c.tr_load_pv_share, "1", "变压器负载估算中负荷的折算系数");
    b.dbl("ramp_kw_per_s", &c.ramp_kw_per_s, "kW/s", "每秒允许的功率变化幅度");
    b.dbl("grid_p_min_kw", &c.grid_p_min_kw, "kW", "关口功率下限（0=不允许倒送）");
    b.dbl("grid_p_max_kw", &c.grid_p_max_kw, "kW", "关口功率上限（进线/变压器容量）");
    b.dbl("grid_filter_alpha", &c.grid_filter_alpha, "1", "并网边界量测低通系数（1=不滤波）");
    b.dbl("grid_lookahead_max_drop_kw", &c.grid_lookahead_max_drop_kw, "kW",
          "并网上界前瞻幅度上限（0=关闭）");
    b.dbl("grid_freq_min_hz", &c.grid_freq_min_hz, "Hz", "电网频率下限");
    b.dbl("grid_freq_max_hz", &c.grid_freq_max_hz, "Hz", "电网频率上限");
    b.dbl("grid_volt_min_pu", &c.grid_volt_min_pu, "pu", "电网电压下限");
    b.dbl("grid_volt_max_pu", &c.grid_volt_max_pu, "pu", "电网电压上限");
    b.flag("strict_l0", &c.strict_l0, "L0 冲突时强制 [0,0]");
    b.flag("enable_ramp", &c.enable_ramp, "启用变化率约束");
    b.require("soc_min");
    b.require("soc_max");
    b.require("grid_p_min_kw");
    b.require("grid_p_max_kw");
}

// ---- 08/ 优化调度与实时协同 ----
inline void bind_fields(Binder& b, CoordinatorConfig& c) {
    b.dbl("reopt_period_s", &c.reopt_period_s, "s", "滚动优化周期（15 min）");
    b.flag("enable_reopt", &c.enable_reopt, "启用滚动重优化");
    b.dbl("soc_kp", &c.soc_kp, "kW/1", "SOC 偏差比例增益");
    b.dbl("soc_ki", &c.soc_ki, "kW/(1·s)", "SOC 偏差积分增益");
    b.dbl("soc_correction_max_kw", &c.soc_correction_max_kw, "kW", "SOC 纠偏上限");
    b.dbl("dev_kp", &c.dev_kp, "1", "负荷偏差前馈增益");
    b.dbl("dev_correction_max_kw", &c.dev_correction_max_kw, "kW", "负荷偏差纠偏上限");
    b.dbl("energy_kp", &c.energy_kp, "1", "窗口电量预算补偿增益");
    b.dbl("energy_correction_max_kw", &c.energy_correction_max_kw, "kW", "电量纠偏上限");
    b.dbl("total_correction_max_kw", &c.total_correction_max_kw, "kW", "纠偏总量上限");
    b.dbl("on_plan_tol_kw", &c.on_plan_tol_kw, "kW", "「在计划上」判定容差");
    b.flag("enable_correction", &c.enable_correction, "启用实时纠偏");
}

// ---- 06/ 状态机 ----
inline void bind_fields(Binder& b, StateMachineConfig& c) {
    b.dbl("init_hold_s", &c.init_hold_s, "s", "INIT 停留时长");
    b.integer("self_check_cycles", &c.self_check_cycles, "拍", "自检需连续通过的拍数");
    b.integer("derate_release_cycles", &c.derate_release_cycles, "拍", "降功率解除需连续正常拍数");
    b.integer("fault_clear_cycles", &c.fault_clear_cycles, "拍", "故障恢复需连续正常拍数");
    b.flag("allow_ready_output", &c.allow_ready_output, "READY 态是否允许输出（现场建议 false）");
}

// =====================================================================
// 策略配置
//
// 策略的"参数"是一张 string→double 的表（见 04/strategy_base.h 的 ParamMap），
// 所以这里不能像上面那样做字段级绑定 —— 只能整体搬运。这不是缺陷：
// 策略参数是**策略自己解释**的（Kp / deadband / P_grid_min ...），
// P1 不应该、也没法知道每个策略有哪些参数。P1 负责的是：
//   · 按 id 精确定位到策略（id 打错必须报错，而不是静默忽略）
//   · 参数名交给策略自己校验（未知参数名由策略决定是忽略还是报错）
// =====================================================================
struct StrategyConfig {
    bool        enabled = true;
    std::string note;                    // 说明用途（现场交接时很有用）
    ParamMap    params;                  // 参数名 → 值（key 排序保证往返稳定）

    bool operator==(const StrategyConfig& o) const {
        return enabled == o.enabled && note == o.note && params == o.params;
    }
};

// =====================================================================
// 统一配置
// =====================================================================
struct EmsConfig {
    // ---- 元信息（不参与控制，用于归档/审计/交接）----
    std::string name        = "default";
    std::string description;
    std::string site;                    // 站点名
    std::string version     = "1.0";

    // ---- 各层参数 ----
    LoopConfig         loop;
    PlantConfig        plant;
    DeviceLimits       limits;
    SafetyParams       safety;
    CoordinatorConfig  coord;
    StateMachineConfig fsm;

    // ---- 策略：id → 配置 ----
    // 用 std::map 而不是 unordered_map：导出配置时键有序，diff 友好
    std::map<std::string, StrategyConfig> strategies;

    // 便捷访问
    StrategyConfig* strategy(const std::string& id) {
        auto it = strategies.find(id);
        return (it == strategies.end()) ? nullptr : &it->second;
    }
    const StrategyConfig* strategy(const std::string& id) const {
        auto it = strategies.find(id);
        return (it == strategies.end()) ? nullptr : &it->second;
    }
};

// =====================================================================
// 顶层 section 名（JSON 结构）
// =====================================================================
namespace config_keys {
inline const char* kLoop       = "loop";
inline const char* kPlant      = "plant";
inline const char* kLimits     = "limits";
inline const char* kSafety     = "safety";
inline const char* kCoord      = "coordinator";
inline const char* kFsm        = "state_machine";
inline const char* kStrategies = "strategies";
} // namespace config_keys

} // namespace ems
