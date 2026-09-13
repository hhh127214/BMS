// =====================================================================
// P1/ — 产品化 P1 配置化 · 加载 / 保存 / 校验 / 装配
//
// 本文件是 P1 的核心：它把「**装配顺序知识**」从各调用点收拢到一处。
//
// ---------------------------------------------------------------------
// 为什么"顺序"必须由 P1 拥有
// ---------------------------------------------------------------------
// P0 之后，一个部署点的装配是这样写的（10/ 里的真实代码）：
//
//     EmsRuntime rt;
//     rt.config()             = cfg.loop;
//     rt.safety_params()      = cfg.safety;
//     rt.coordinator_config() = cfg.coord;
//     rt.fsm_config()         = cfg.fsm;
//     rt.configure_plant(cfg.plant);   // ← 内部 refresh_device_limits() 重置 dev_
//     rt.device_limits()      = cfg.limits;   // ← 必须在上一行**之后**
//     rt.apply_configs();              // ← 必须在 device_limits 之后
//     rt.shaper().set_deadband(cfg.loop.output_deadband_kw);  // ← 必须在 config() 之后
//     ...
//
// 这里面有 **3 处隐式顺序依赖**，而且都不会在编译期报错：
//   ① configure_plant() 会 refresh_device_limits()（io_->read_limits(dev_)），
//      把 dev_ 覆盖成被控对象的额定值 —— 所以 limits 必须**后**赋值。
//   ② apply_configs() 用 dev_.pcs_rated_* 算协同层设备参数 —— 必须**最后**调。
//   ③ OutputShaper 在 init() 里缓存了 deadband/switch_delay 的**副本** ——
//      改了 LoopConfig 后必须重推，否则配置"生效了但没生效"。
//
// 这三条属于"知道的人不会错、不知道的人必错"的知识。P1 把它写进
// `apply_config()` 一个函数，调用方只需 `apply_config(rt, cfg)`。
//
// ---------------------------------------------------------------------
// 分层
// ---------------------------------------------------------------------
//   load_config_file()  : 文件 → EmsConfig（含语法/类型错误，带 行:列）
//   validate()          : EmsConfig → 语义诊断（纯函数，不碰运行时）
//   apply_config()      : EmsConfig → EmsRuntime（**唯一**拥有装配顺序的地方）
//   capture_config()    : EmsRuntime → EmsConfig（导出运行中的现场参数）
//   save_config_file()  : EmsConfig → 文件
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "ems_config.h"

#include <algorithm>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

namespace ems {

// =====================================================================
// 诊断
// =====================================================================
struct ConfigIssue {
    enum class Level { kError, kWarning };
    Level       level = Level::kError;
    std::string path;                 // 形如 "safety.soc_min"
    std::string message;

    std::string to_string() const {
        return std::string(level == Level::kError ? "[错误] " : "[警告] ")
             + path + " —— " + message;
    }
};

struct ConfigDiagnostics {
    std::vector<ConfigIssue> issues;

    void error(const std::string& path, const std::string& msg) {
        issues.push_back({ConfigIssue::Level::kError, path, msg});
    }
    void warning(const std::string& path, const std::string& msg) {
        issues.push_back({ConfigIssue::Level::kWarning, path, msg});
    }

    size_t error_count() const {
        size_t n = 0;
        for (const auto& i : issues) if (i.level == ConfigIssue::Level::kError) ++n;
        return n;
    }
    size_t warning_count() const { return issues.size() - error_count(); }
    bool ok() const { return error_count() == 0; }

    void merge(const ConfigDiagnostics& o) {
        issues.insert(issues.end(), o.issues.begin(), o.issues.end());
    }

    std::string to_text() const {
        std::ostringstream os;
        if (issues.empty()) { os << "配置检查通过（0 错误 / 0 警告）\n"; return os.str(); }
        for (const auto& i : issues) os << i.to_string() << "\n";
        os << "共 " << error_count() << " 错误 / " << warning_count() << " 警告\n";
        return os.str();
    }
};

// =====================================================================
// 节 ↔ 结构体（Binder 驱动，load / save 共用同一张字段表）
// =====================================================================

// 结构体 → JSON 对象
template <class T>
inline json::Value section_to_json(T& obj) {
    Binder b;
    bind_fields(b, obj);
    json::Value sec = json::Value::make_object();
    for (const auto& f : b.fields()) sec.set(f.key, f.get());
    return sec;
}

// JSON 对象 → 结构体（缺省字段保留结构体默认值；未知键警告）
template <class T>
inline void section_from_json(const json::Value& root, const char* key, T& obj,
                              ConfigDiagnostics* diag) {
    const json::Value* sec = root.find(key);
    if (sec == nullptr || sec->is_null()) return;      // 整节缺省 → 全用默认值
    if (!sec->is_object()) {
        diag->error(key, "该节应为对象（{...}）");
        return;
    }
    Binder b;
    bind_fields(b, obj);

    for (const auto& f : b.fields()) {
        const json::Value* v = sec->find(f.key);
        if (v == nullptr) {
            if (f.required) {
                const json::Value d = f.get();
                diag->warning(std::string(key) + "." + f.key,
                              "关键配置项缺省，将使用默认值 "
                              + (d.is_number() ? json::detail::num_to_string(d.num)
                                               : std::string("<非数值>")));
            }
            continue;
        }
        std::string why;
        if (!f.set(*v, &why)) {
            diag->error(std::string(key) + "." + f.key, why);
        }
    }

    // 未知键 → 警告（typo 是最常见的配置事故）
    for (const auto& kv : sec->obj) {
        bool known = false;
        for (const auto& f : b.fields()) {
            if (f.key == kv.first) { known = true; break; }
        }
        if (!known) {
            diag->warning(std::string(key) + "." + kv.first, "未知配置项，已忽略");
        }
    }
}

// =====================================================================
// EmsConfig ↔ JSON
// =====================================================================
inline json::Value to_json(const EmsConfig& cfg_in) {
    // Binder 需要非 const 指针（get 闭包读地址）；这里做一次拷贝，
    // 保证 to_json 对调用方是 const 语义。
    EmsConfig cfg = cfg_in;
    json::Value root = json::Value::make_object();

    // 元信息
    root.set("name",        json::Value(cfg.name));
    if (!cfg.description.empty()) root.set("description", json::Value(cfg.description));
    if (!cfg.site.empty())        root.set("site",        json::Value(cfg.site));
    root.set("version",     json::Value(cfg.version));

    // 各层参数
    root.set(config_keys::kLoop,   section_to_json(cfg.loop));
    root.set(config_keys::kPlant,  section_to_json(cfg.plant));
    root.set(config_keys::kLimits, section_to_json(cfg.limits));
    root.set(config_keys::kSafety, section_to_json(cfg.safety));
    root.set(config_keys::kCoord,  section_to_json(cfg.coord));
    root.set(config_keys::kFsm,    section_to_json(cfg.fsm));

    // 策略（std::map 已保证 id 有序；参数表是 unordered_map，需排序）
    json::Value ss = json::Value::make_object();
    for (const auto& kv : cfg.strategies) {
        json::Value sv = json::Value::make_object();
        sv.set("enabled", json::Value(kv.second.enabled));
        if (!kv.second.note.empty()) sv.set("note", json::Value(kv.second.note));

        std::vector<std::string> keys;
        keys.reserve(kv.second.params.size());
        for (const auto& p : kv.second.params) keys.push_back(p.first);
        std::sort(keys.begin(), keys.end());

        json::Value pv = json::Value::make_object();
        for (const auto& k : keys) {
            auto it = kv.second.params.find(k);
            pv.set(k, json::Value(it->second));
        }
        sv.set("params", std::move(pv));
        ss.set(kv.first, std::move(sv));
    }
    root.set(config_keys::kStrategies, std::move(ss));
    return root;
}

inline EmsConfig from_json(const json::Value& root, ConfigDiagnostics* diag) {
    EmsConfig cfg;   // 从"结构体默认值"出发，配置只做覆盖

    if (!root.is_object()) {
        diag->error("(根)", "配置根节点应为对象（{...}）");
        return cfg;
    }

    // 元信息
    if (const json::Value* v = root.find("name"))        cfg.name        = v->string(cfg.name);
    if (const json::Value* v = root.find("description")) cfg.description = v->string();
    if (const json::Value* v = root.find("site"))        cfg.site        = v->string();
    if (const json::Value* v = root.find("version"))     cfg.version     = v->string(cfg.version);

    // 各层参数
    section_from_json(root, config_keys::kLoop,   cfg.loop,   diag);
    section_from_json(root, config_keys::kPlant,  cfg.plant,  diag);
    section_from_json(root, config_keys::kLimits, cfg.limits, diag);
    section_from_json(root, config_keys::kSafety, cfg.safety, diag);
    section_from_json(root, config_keys::kCoord,  cfg.coord,  diag);
    section_from_json(root, config_keys::kFsm,    cfg.fsm,    diag);

    // 策略
    if (const json::Value* sec = root.find(config_keys::kStrategies)) {
        if (!sec->is_object()) {
            diag->error(config_keys::kStrategies, "该节应为对象");
        } else {
            for (const auto& kv : sec->obj) {
                const std::string base = std::string(config_keys::kStrategies) + "." + kv.first;
                StrategyConfig sc;
                if (!kv.second.is_object()) {
                    diag->error(base, "策略配置应为对象（{enabled, note, params}）");
                    continue;
                }
                if (const json::Value* e = kv.second.find("enabled")) sc.enabled = e->boolean(true);
                if (const json::Value* n = kv.second.find("note"))    sc.note    = n->string();
                if (const json::Value* p = kv.second.find("params")) {
                    if (!p->is_object()) {
                        diag->error(base + ".params", "params 应为对象");
                    } else {
                        for (const auto& pk : p->obj) {
                            if (!pk.second.is_number()) {
                                diag->error(base + ".params." + pk.first,
                                            "参数值必须是数字（策略参数统一为 double）");
                                continue;
                            }
                            sc.params[pk.first] = pk.second.num;
                        }
                    }
                }
                // 未知键警告
                for (const auto& uk : kv.second.obj) {
                    if (uk.first != "enabled" && uk.first != "note" && uk.first != "params") {
                        diag->warning(base + "." + uk.first, "未知配置项，已忽略");
                    }
                }
                cfg.strategies[kv.first] = std::move(sc);
            }
        }
    }

    // 未知顶层节 → 警告
    for (const auto& kv : root.obj) {
        const std::string& k = kv.first;
        if (k == "name" || k == "description" || k == "site" || k == "version"
            || k == config_keys::kLoop || k == config_keys::kPlant
            || k == config_keys::kLimits || k == config_keys::kSafety
            || k == config_keys::kCoord || k == config_keys::kFsm
            || k == config_keys::kStrategies) continue;
        diag->warning(k, "未知顶层配置节，已忽略");
    }

    return cfg;
}

// =====================================================================
// 文件 I/O
// =====================================================================
inline bool load_config_file(const std::string& path, EmsConfig* out,
                            ConfigDiagnostics* diag) {
    json::Value root;
    json::ParseError perr;
    if (!json::parse_file(path, &root, &perr)) {
        diag->error("(文件)", perr.to_string() + "  [" + path + "]");
        return false;
    }
    EmsConfig cfg = from_json(root, diag);
    if (diag->ok() && out) *out = std::move(cfg);
    return diag->ok();
}

inline bool parse_config_text(const std::string& text, EmsConfig* out,
                              ConfigDiagnostics* diag) {
    json::ParseError perr;
    json::Value root = json::parse(text, &perr);
    if (!perr.ok) {
        diag->error("(文本)", perr.to_string());
        return false;
    }
    EmsConfig cfg = from_json(root, diag);
    if (diag->ok() && out) *out = std::move(cfg);
    return diag->ok();
}

inline bool save_config_file(const std::string& path, const EmsConfig& cfg,
                             int indent = 2) {
    return json::write_file(path, to_json(cfg), indent);
}

// =====================================================================
// 语义校验（纯函数：只看配置本身，不碰运行时）
//
// 只把「一定会跑坏」的判成 error；「可疑/不合常理」判成 warning。
// 现场调试时一个 warning 不该拦住启动。
// =====================================================================
inline ConfigDiagnostics validate(const EmsConfig& cfg) {
    ConfigDiagnostics d;

    auto err = [&d](const char* p, const std::string& m) { d.error(p, m); };
    auto wrn = [&d](const char* p, const std::string& m) { d.warning(p, m); };

    // ---------- loop ----------
    if (!(cfg.loop.dt_s > 0.0))                 err("loop.dt_s", "必须 > 0");
    if (!(cfg.loop.demand_window_s > 0.0))      err("loop.demand_window_s", "必须 > 0");
    else if (cfg.loop.demand_window_s < cfg.loop.dt_s)
                                                err("loop.demand_window_s", "不能小于控制周期 dt_s");
    if (cfg.loop.log_every < 1)                 err("loop.log_every", "必须 >= 1（每 N 拍记一条）");
    if (!(cfg.loop.comm_stale_threshold_s >= 0.0))
                                                err("loop.comm_stale_threshold_s", "不能为负");
    if (cfg.loop.l2_correction_max_kw < 0.0)    err("loop.l2_correction_max_kw", "不能为负（0=不纠偏）");
    if (!cfg.loop.enable_safety_engine)         wrn("loop.enable_safety_engine", "现场部署建议开启安全约束引擎");
    if (!cfg.loop.enable_state_machine)         wrn("loop.enable_state_machine", "现场部署建议开启状态机门控");

    // ---------- plant ----------
    if (!(cfg.plant.battery_capacity_kwh > 0.0)) err("plant.battery_capacity_kwh", "必须 > 0");
    if (!(cfg.plant.eta_chg > 0.0 && cfg.plant.eta_chg <= 1.0))
                                                err("plant.eta_chg", "应在 (0, 1] 区间");
    if (!(cfg.plant.eta_dis > 0.0 && cfg.plant.eta_dis <= 1.0))
                                                err("plant.eta_dis", "应在 (0, 1] 区间");
    if (!(cfg.plant.soh > 0.0 && cfg.plant.soh <= 1.0))
                                                err("plant.soh", "应在 (0, 1] 区间");
    if (!(cfg.plant.pcs_max_chg_kw >= 0.0))     err("plant.pcs_max_chg_kw", "不能为负");
    if (!(cfg.plant.pcs_max_dis_kw >= 0.0))     err("plant.pcs_max_dis_kw", "不能为负");
    if (!(cfg.plant.pcs_ramp_kw_per_s > 0.0))   err("plant.pcs_ramp_kw_per_s", "必须 > 0");
    if (!(cfg.plant.pcs_tau_s > 0.0))           err("plant.pcs_tau_s", "必须 > 0");
    if (cfg.plant.pcs_deadtime_s < 0.0)         err("plant.pcs_deadtime_s", "不能为负");
    if (!(cfg.plant.soc_phys_min < cfg.plant.soc_phys_max))
                                                err("plant.soc_phys_min", "必须小于 soc_phys_max");
    if (cfg.plant.soc_init < cfg.plant.soc_phys_min || cfg.plant.soc_init > cfg.plant.soc_phys_max)
                                                err("plant.soc_init", "必须落在 [soc_phys_min, soc_phys_max] 内");
    if (cfg.plant.pcs_tau_s < cfg.plant.pcs_deadtime_s)
                                                wrn("plant.pcs_tau_s", "小于死区时间，惯性来不及体现");

    // ---------- limits ----------
    if (!(cfg.limits.transformer_capacity_kw > 0.0))
                                                err("limits.transformer_capacity_kw", "必须 > 0");
    if (cfg.limits.d_target_kw < 0.0)           err("limits.d_target_kw", "不能为负");
    if (cfg.limits.d_target_kw > cfg.limits.transformer_capacity_kw)
                                                wrn("limits.d_target_kw", "超过变压器容量，需量约束实际不起作用");
    if (cfg.limits.pcs_rated_chg_kw > cfg.plant.pcs_max_chg_kw)
                                                wrn("limits.pcs_rated_chg_kw", "超过 PCS 物理充电能力");
    if (cfg.limits.pcs_rated_dis_kw > cfg.plant.pcs_max_dis_kw)
                                                wrn("limits.pcs_rated_dis_kw", "超过 PCS 物理放电能力");

    // ---------- safety ----------
    if (!(cfg.safety.soc_min < cfg.safety.soc_max))
                                                err("safety.soc_min", "必须小于 soc_max");
    if (cfg.safety.soc_warn_low < cfg.safety.soc_min)
                                                err("safety.soc_warn_low", "不能低于 soc_min");
    if (cfg.safety.soc_warn_high > cfg.safety.soc_max)
                                                err("safety.soc_warn_high", "不能高于 soc_max");
    if (!(cfg.safety.soc_warn_low < cfg.safety.soc_warn_high))
                                                err("safety.soc_warn_low", "必须小于 soc_warn_high");
    if (cfg.safety.soc_min < cfg.plant.soc_phys_min)
                                                err("safety.soc_min", "低于被控对象物理下限 soc_phys_min（安全层会失效）");
    if (cfg.safety.soc_max > cfg.plant.soc_phys_max)
                                                err("safety.soc_max", "高于被控对象物理上限 soc_phys_max");
    if (!(cfg.safety.soc_warn_derate >= 0.0 && cfg.safety.soc_warn_derate <= 1.0))
                                                err("safety.soc_warn_derate", "应在 [0, 1] 区间");
    if (!(cfg.safety.temp_warn_derate >= 0.0 && cfg.safety.temp_warn_derate <= 1.0))
                                                err("safety.temp_warn_derate", "应在 [0, 1] 区间");
    if (!(cfg.safety.temp_warn_c < cfg.safety.temp_fault_c))
                                                err("safety.temp_warn_c", "必须小于 temp_fault_c");
    if (!(cfg.safety.pcs_derate_ratio > 0.0 && cfg.safety.pcs_derate_ratio <= 1.0))
                                                err("safety.pcs_derate_ratio", "应在 (0, 1] 区间");
    if (!(cfg.safety.grid_p_min_kw <= cfg.safety.grid_p_max_kw))
                                                err("safety.grid_p_min_kw", "不能大于 grid_p_max_kw");
    if (cfg.safety.grid_p_min_kw > 0.0)
                                                wrn("safety.grid_p_min_kw", "下限为正意味着强制买电，确认是否符合预期");
    if (cfg.safety.grid_p_max_kw > cfg.limits.transformer_capacity_kw)
                                                wrn("safety.grid_p_max_kw", "超过变压器容量");
    if (!(cfg.safety.ramp_kw_per_s > 0.0))      err("safety.ramp_kw_per_s", "必须 > 0");
    if (!(cfg.safety.grid_filter_alpha > 0.0 && cfg.safety.grid_filter_alpha <= 1.0))
                                                err("safety.grid_filter_alpha", "应在 (0, 1] 区间");
    if (cfg.safety.grid_lookahead_max_drop_kw < 0.0)
                                                err("safety.grid_lookahead_max_drop_kw", "不能为负（0=关闭前瞻）");
    if (cfg.safety.grid_lookahead_max_drop_kw > 0.0 && !cfg.loop.enable_safety_engine)
                                                wrn("safety.grid_lookahead_max_drop_kw", "安全引擎未启用，前瞻不会生效");
    if (!(cfg.safety.tr_overload_th > 0.0))     err("safety.tr_overload_th", "必须 > 0");
    if (!(cfg.safety.tr_extreme_th >= cfg.safety.tr_overload_th))
                                                err("safety.tr_extreme_th", "不能低于 tr_overload_th");
    if (cfg.safety.grid_freq_min_hz > cfg.safety.grid_freq_max_hz)
                                                err("safety.grid_freq_min_hz", "不能大于 grid_freq_max_hz");
    if (cfg.safety.grid_volt_min_pu > cfg.safety.grid_volt_max_pu)
                                                err("safety.grid_volt_min_pu", "不能大于 grid_volt_max_pu");

    // ---------- coordinator ----------
    if (!(cfg.coord.reopt_period_s > 0.0))      err("coordinator.reopt_period_s", "必须 > 0");
    else if (cfg.coord.reopt_period_s < cfg.loop.dt_s)
                                                err("coordinator.reopt_period_s", "不能小于控制周期 dt_s");
    if (cfg.coord.soc_correction_max_kw < 0.0)  err("coordinator.soc_correction_max_kw", "不能为负");
    if (cfg.coord.dev_correction_max_kw < 0.0)  err("coordinator.dev_correction_max_kw", "不能为负");
    if (cfg.coord.energy_correction_max_kw < 0.0)
                                                err("coordinator.energy_correction_max_kw", "不能为负");
    if (cfg.coord.total_correction_max_kw < 0.0)
                                                err("coordinator.total_correction_max_kw", "不能为负");
    if (cfg.coord.total_correction_max_kw > cfg.loop.l2_correction_max_kw
        && cfg.loop.l2_correction_max_kw > 0.0)
                                                wrn("coordinator.total_correction_max_kw", "大于 L2 纠偏权限上限，会被二次限幅");

    // ---------- state machine ----------
    if (cfg.fsm.init_hold_s < 0.0)              err("state_machine.init_hold_s", "不能为负");
    if (cfg.fsm.self_check_cycles < 0)          err("state_machine.self_check_cycles", "不能为负");
    if (cfg.fsm.derate_release_cycles < 0)      err("state_machine.derate_release_cycles", "不能为负");
    if (cfg.fsm.fault_clear_cycles < 0)         err("state_machine.fault_clear_cycles", "不能为负");
    if (cfg.fsm.allow_ready_output)
                                                wrn("state_machine.allow_ready_output", "READY 态允许输出，现场部署建议 false");

    // ---------- 交叉一致性 ----------
    if (cfg.safety.soc_min > cfg.plant.soc_init)
                                                wrn("plant.soc_init", "初始 SOC 低于安全下限，开局即禁放");
    if (cfg.safety.soc_max < cfg.plant.soc_init)
                                                wrn("plant.soc_init", "初始 SOC 高于安全上限，开局即禁充");

    return d;
}

// =====================================================================
// 装配（P1 存在的理由：这里是**唯一**知道装配顺序的地方）
// =====================================================================
struct ApplyOptions {
    // 仿真装配：把配置里的 plant / limits 注入内置仿真适配器。
    // 现场部署（attach_device() 注入了 RT_DB / Modbus 适配器）应传 false ——
    // 真实设备的额定与限制由设备每拍给出，配置文件里的是**参考值**，不得覆盖。
    bool inject_plant = true;
    // 是否复位运行时（清日志/时间轴）。装配期 true；热更新可传 false。
    bool reset_runtime = true;
};

inline ConfigDiagnostics apply_config(EmsRuntime& rt, const EmsConfig& cfg,
                                      const ApplyOptions& opt = ApplyOptions()) {
    ConfigDiagnostics d;

    // ---------- ① 算法侧参数：纯结构体赋值，顺序无关 ----------
    rt.config()             = cfg.loop;
    rt.safety_params()      = cfg.safety;
    rt.coordinator_config() = cfg.coord;
    rt.fsm_config()         = cfg.fsm;

    // ---------- ② 被控对象 ----------
    // 顺序陷阱 ①：configure_plant() 内部 refresh_device_limits() → io_->read_limits(dev_)，
    // 会把 dev_ 覆盖成被控对象的额定值。因此 device_limits() 必须**后**赋值。
    if (opt.inject_plant) {
        rt.configure_plant(cfg.plant);
        rt.device_limits() = cfg.limits;
    } else {
        // 现场：dev_ 由真实设备每拍刷新，此处不覆盖。
        // 但仍把参考值登记进日志，便于核对（不写入 dev_）。
        if (cfg.limits.transformer_capacity_kw > 0.0) { /* 参考值，仅文档意义 */ }
    }

    // ---------- ③ 统一生效 ----------
    // 顺序陷阱 ②：apply_configs() 用 dev_.pcs_rated_* 计算协同层设备参数，
    // 所以必须在 device_limits 落定之后调用。
    rt.apply_configs();

    // ---------- ④ 输出整形器 ----------
    // 顺序陷阱 ③：OutputShaper 在 init() 里缓存了 deadband / switch_delay 的**副本**。
    // 只改 LoopConfig 不会影响它 —— 必须重推，否则"配置生效了但行为没变"。
    rt.shaper().set_deadband(cfg.loop.output_deadband_kw);
    rt.shaper().set_switch_delay(cfg.loop.output_switch_delay);
    rt.shaper().reset();

    // ---------- ⑤ 计划跟踪边界跟随设备额定 ----------
    // configure_plant() 只在仿真路径上刷新它；现场路径（attach_device）不刷，
    // 这里统一补上，保证两条路径行为一致。
    if (rt.plan_tracker()) {
        rt.plan_tracker()->set_bounds(rt.device_limits().pcs_rated_chg_kw,
                                      rt.device_limits().pcs_rated_dis_kw);
    }

    // ---------- ⑥ 策略：enable + 参数热更新 ----------
    // 策略 id 打错必须报错（静默忽略会让人以为"配了但没生效"）
    for (const auto& kv : cfg.strategies) {
        const std::string& id = kv.first;
        const std::string  base = std::string(config_keys::kStrategies) + "." + id;
        if (!rt.manager().has(id)) {
            std::string known;
            for (const auto& k : rt.manager().list_ids()) { known += " " + k; }
            d.error(base, "策略 id 不存在（已注册:" + known + " ）");
            continue;
        }
        try {
            rt.manager().enable(id, kv.second.enabled);
            for (const auto& p : kv.second.params) {
                // 跳过内部键（__weight__ 由 set_run_mode 管理，不该被配置覆盖）
                if (p.first.rfind("__", 0) == 0) continue;
                rt.manager().set_param(id, p.first, p.second);
            }
        } catch (const std::exception& e) {
            d.error(base, std::string("参数应用失败: ") + e.what());
        }
    }

    return d;
}

// =====================================================================
// 导出运行中的配置
//
// 用途：现场调参之后把"当前实际生效的参数"落盘归档 / 回滚。
// 注意：plant / limits 在 attach_device() 之后来自真实设备，导出的是
//       设备当前上报值（有参考意义，但不一定能原样写回）。
// =====================================================================
inline EmsConfig capture_config(EmsRuntime& rt) {
    EmsConfig cfg;
    cfg.name        = "captured";
    cfg.description = "从运行中的 EmsRuntime 导出（含策略当前参数）";

    cfg.loop   = rt.config();
    cfg.safety = rt.safety_params();
    cfg.coord  = rt.coordinator_config();
    cfg.fsm    = rt.fsm_config();
    cfg.limits = rt.device_limits();
    cfg.plant  = rt.plant().config();

    for (const auto& id : rt.manager().list_ids()) {
        StrategyConfig sc;
        try {
            sc.enabled = rt.manager().get_status(id).enabled;
        } catch (...) {
            sc.enabled = true;
        }
        StrategyPtr s = rt.manager().get_strategy(id);
        if (s) {
            for (const auto& p : s->params()) {
                if (p.first.rfind("__", 0) == 0) continue;   // 过滤内部键
                sc.params[p.first] = p.second;
            }
        }
        cfg.strategies[id] = std::move(sc);
    }
    return cfg;
}

} // namespace ems
