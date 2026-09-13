// =====================================================================
// 10/ — 周期 10：EMS 24h 离线仿真（全场景）
//
// 依据：工商业储能EMS调控策略设计方案.md §7 周期 10
//   「搭建离线仿真环境，导入负荷曲线、光伏曲线、电价时序数据、
//     SOC 初始参数、BMS/PCS/变压器设备参数，完成 24 小时全场景仿真测试，
//     输出功率曲线、SOC 曲线、策略状态、经济收益、告警日志」
//
// 本模块是**装配层**：把 04~09 的全栈（策略/仲裁/安全/状态机/闭环/优化协同）
// 装进一个可配置的 24h 场景里跑一遍，并把结果落成可交付的产物。
// 它不引入任何新的控制逻辑 —— 控制逻辑的验收在 05~09 已完成。
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "data_models.h"
#include "day_curves.h"
#include "econ_metrics.h"
#include "realtime_loop.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

namespace ems {

// =====================================================================
// 告警条目
// =====================================================================
struct AlarmEntry {
    Timestamp   t = 0.0;
    std::string level;    // INFO / WARN / ALARM / FAULT
    std::string source;   // FSM / SAFETY / SOC / GRID / TRANSFORMER / COMM / TEMP
    std::string message;

    std::string to_csv_row() const {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%.1f,%s,%s,\"%s\"",
                      t, level.c_str(), source.c_str(), message.c_str());
        return std::string(buf);
    }
};

// =====================================================================
// 故障注入时间窗
//
// 离线仿真的价值一半在"正常日经济性"，另一半在"异常日安全性"。
// 故障用**时间窗**表达（而不是在配置里一次性置位），这样一条 24h 曲线
// 里可以编排"发生 → 持续 → 恢复"的完整过程，能验证：
//   · 故障期间是否正确门控（指令归零）
//   · 故障恢复后是否能重新并网（而不是锁死）
//   · 状态机是否按 SOE 记录迁移
// =====================================================================
struct FaultWindow {
    double t_begin_s = 0.0;
    double t_end_s   = 0.0;
    // 1=comm_bms_off 2=comm_meter_off 3=comm_pcs_off
    // 4=pcs_fault    5=device_offline  6=data_invalid
    int    kind      = 0;
    std::string note;

    bool active(double t) const { return t >= t_begin_s && t < t_end_s; }
};

inline void apply_fault(EmsRuntime& rt, int kind, bool on) {
    switch (kind) {
        case 1: rt.plant().set_comm_bms(!on);      break;
        case 2: rt.plant().set_comm_meter(!on);    break;
        case 3: rt.plant().set_comm_pcs(!on);      break;
        case 4: rt.plant().set_pcs_fault(on);      break;
        case 5: rt.plant().set_device_offline(on); break;
        case 6: rt.plant().set_data_valid(!on);    break;
        default: break;
    }
}

// =====================================================================
// 仿真配置
// =====================================================================
struct Sim24hConfig {
    // ---- 时序 ----
    double dt_s       = 1.0;        // 控制周期（离线仿真 1 s 足够，10 Hz 亦可）
    int    log_every  = 10;         // 日志降采样：每 10 拍记一条（10 s）
    double duration_s = 24.0 * 3600.0;

    // ---- 设备参数（BMS / PCS / 变压器 / 电池）----
    PlantConfig  plant;
    DeviceLimits limits;
    SafetyParams safety;
    CoordinatorConfig  coord;
    StateMachineConfig fsm;

    // ---- 经济性 ----
    EconParams econ;

    // ---- 曲线来源 ----
    bool          use_csv = false;  // true = 从 curves_path 导入；false = 内置典型日
    std::string   curves_path;
    TypicalDayParams typical;

    // ---- 输出 ----
    std::string out_dir;            // 非空则落盘 timeseries.csv / alarms.csv / summary.json / report.html
    std::string title = "EMS 24h 离线仿真";

    // ---- 故障注入（可空）----
    std::vector<FaultWindow> fault_windows;

    // 故障恢复后是否自动重新下发启动许可（模拟"上层运行许可保持"）。
    //   false（默认）= 严格按安全规程：故障恢复后停在 READY 待机，需人工确认
    //                  （这是 06/ 状态机的设计意图，见 transit() 注释）
    //   true          = 上层持续保持运行许可，用于验证"恢复后能否重新带载"
    bool auto_restart_after_fault = false;

    // ---- 场景不变量（用于验收）----
    double grid_min_required = -1e18;
    double grid_max_required =  1e18;
    double tr_check_cap_kw   = 0.0;
    double tr_check_tol      = 1.05;
    // ---- 不变量（与 09/ 同口径：硬不变量逐拍、安全不变量稳态）----
    // 稳态判定窗口，单位是**控制拍**（dt_s），不是日志行 —— 日志按 log_every
    // 降采样，若按行数比较，同一物理过程在 log_every=1 与 10 下会得到不同的
    // 稳态起点，不变量结论就不一致了。run_sim_24h() 内部会换算成日志行数。
    int    settle_ticks      = 50;
    // SOC 越界容差（见 econ_metrics.h compute_economics 的说明）
    double soc_tol           = 0.002;
};

// =====================================================================
// 仿真结果
// =====================================================================
struct Sim24hResult {
    bool        ok = false;
    std::string error;

    int    steps      = 0;
    int    log_rows   = 0;
    double wall_s     = 0.0;
    double dt_s       = 1.0;
    double rec_dt_s   = 10.0;

    CurveStats curves;
    ForecastSeries forecast;   // 本次仿真实际使用的曲线（报告绘图用）
    EconResult econ;
    LoopMetrics metrics;

    std::vector<StepRecord> log;
    std::vector<AlarmEntry> alarms;

    // ---- 不变量（与 09/ 同口径：硬不变量逐拍、安全不变量稳态）----
    int out_of_interval = 0;   // p_cmd 逃出 [p_lower, p_upper]
    int over_limit      = 0;   // |p_cmd| 超 PCS/BMS
    int gated_nonzero   = 0;   // 非运行态非零指令
    int grid_breach     = 0;   // 关口越界（稳态窗口）
    int tr_breach       = 0;   // 变压器越限（稳态窗口）
    // 关口越界中，有多少拍发生在 SOC 已触底/触顶（储能无可用容量）。
    // 这是**能量预算**结果，不是控制失效 —— 报告必须把两者分开，
    // 否则"设备故障一天 → 傍晚无容量削峰"会被误读成控制缺陷。
    int grid_breach_soc_limited = 0;
    double max_grid_kw  = 0.0;
    double min_grid_kw  = 0.0;
    int    alarm_count  = 0;
    int    fault_count  = 0;
    int    fault_ticks  = 0;   // 处于故障时间窗内的拍数

    // 分时电价下的运行分布（供报告展示）
    double e_valley_kwh = 0.0, e_flat_kwh = 0.0, e_peak_kwh = 0.0, e_sharp_kwh = 0.0;
    double e_chg_valley_kwh = 0.0, e_dis_peak_kwh = 0.0;

    bool invariants_ok() const {
        return out_of_interval == 0 && over_limit == 0 &&
               gated_nonzero == 0 && grid_breach == 0 && tr_breach == 0;
    }
    // 硬不变量：EMS 自己声明的权限区间与设备能力 —— 任何场景、任何时刻都不得违反
    bool hard_invariants_ok() const {
        return out_of_interval == 0 && over_limit == 0 && gated_nonzero == 0;
    }
    // 安全不变量：物理量。受**能量预算**影响（SOC 用尽即无削峰能力），
    // 越限本身是有效结论而非缺陷；其中 grid_breach_soc_limited 是可归因部分。
    bool safety_invariants_ok() const {
        return grid_breach == 0 && tr_breach == 0;
    }
};

// =====================================================================
// 告警采集
// =====================================================================
inline void collect_alarms(EmsRuntime& rt,
                           const Sim24hConfig& cfg,
                           const std::vector<StepRecord>& log,
                           std::vector<AlarmEntry>& out) {
    // ---- ① 状态机 SOE（状态迁移）----
    for (const auto& e : rt.fsm().history()) {
        AlarmEntry a;
        a.t = e.ts;
        a.source = "FSM";
        a.level = (e.to == EmsState::kFault || e.to == EmsState::kEmergency) ? "FAULT"
                : (e.to == EmsState::kDerated) ? "WARN" : "INFO";
        a.message = std::string("状态迁移 ") + state_name(e.from) + " → " + state_name(e.to)
                  + (e.reason.empty() ? "" : (" (" + e.reason + ")"));
        out.push_back(a);
    }

    // ---- ② 采集层/安全层异常（按"事件"去抖，连续拍只记首次与恢复）----
    bool in_reverse = false, in_tr_over = false, in_soc_hi = false, in_soc_lo = false;
    bool in_clip = false;
    const double eps = 1e-6;

    for (size_t i = 0; i < log.size(); ++i) {
        const auto& s = log[i];
        const double t = s.t;

        // 倒送（仅在"不允许倒送"的配置下才算告警）
        if (cfg.grid_min_required > -1e17 && cfg.grid_min_required > -1e8) {
            const bool bad = s.p_grid < cfg.grid_min_required - 1.0;
            if (bad && !in_reverse) {
                out.push_back({t, "ALARM", "GRID",
                    "关口倒送/越下限 P_grid=" + std::to_string((long long)s.p_grid) + " kW"});
                in_reverse = true;
            } else if (!bad && in_reverse) {
                out.push_back({t, "INFO", "GRID", "关口回到限值内"});
                in_reverse = false;
            }
        }

        // 变压器过载
        if (cfg.tr_check_cap_kw > 0.0) {
            const double tr = std::fabs(s.p_grid) + 0.1 * s.p_load;
            const double lim = cfg.safety.tr_overload_th * cfg.tr_check_cap_kw;
            const bool bad = tr > lim * cfg.tr_check_tol;
            if (bad && !in_tr_over) {
                out.push_back({t, "ALARM", "TRANSFORMER",
                    "变压器过载 tr_load=" + std::to_string((long long)tr) + " kW > "
                    + std::to_string((long long)lim) + " kW"});
                in_tr_over = true;
            } else if (!bad && in_tr_over) {
                out.push_back({t, "INFO", "TRANSFORMER", "变压器负载回到阈值内"});
                in_tr_over = false;
            }
        }

        // SOC 越限（预留 1% 量测容差）
        if (s.soc >= cfg.safety.soc_max - 0.005) {
            if (!in_soc_hi) {
                out.push_back({t, "WARN", "SOC", "SOC 接近/触及上限 " + std::to_string(s.soc)});
                in_soc_hi = true;
            }
        } else if (s.soc < cfg.safety.soc_max - 0.02) {
            in_soc_hi = false;
        }
        if (s.soc <= cfg.safety.soc_min + 0.005) {
            if (!in_soc_lo) {
                out.push_back({t, "WARN", "SOC", "SOC 接近/触及下限 " + std::to_string(s.soc)});
                in_soc_lo = true;
            }
        } else if (s.soc > cfg.safety.soc_min + 0.02) {
            in_soc_lo = false;
        }

        // 安全兜底频繁动作（说明策略与安全边界长期冲突，值得关注）
        if (s.safety_clip) {
            if (!in_clip) {
                out.push_back({t, "WARN", "SAFETY",
                    "安全层限幅生效 reason=" + s.reason});
                in_clip = true;
            }
        } else {
            in_clip = false;
        }

        // 高温
        if (s.temp >= cfg.safety.temp_warn_c) {
            out.push_back({t, "WARN", "TEMP",
                "电池温度偏高 " + std::to_string(s.temp) + " °C"});
        }
        // 故障位
        if (s.fault_bits != 0) {
            out.push_back({t, "FAULT", "COMM",
                "故障位 bits=" + std::to_string(s.fault_bits)});
        }
    }
    (void)eps;
}

// =====================================================================
// 运行 24h 仿真
// =====================================================================
inline Sim24hResult run_sim_24h(const Sim24hConfig& cfg) {
    Sim24hResult r;
    r.dt_s     = cfg.dt_s;
    r.rec_dt_s = cfg.dt_s * std::max(1, cfg.log_every);

    // ---- 曲线 ----
    ForecastSeries fc;
    if (cfg.use_csv) {
        std::string err;
        if (!load_day_curves_csv(cfg.curves_path, fc, cfg.typical.step_s, &err)) {
            r.ok = false;
            r.error = "load_day_curves_csv failed: " + err;
            return r;
        }
    } else {
        fc = make_typical_day_curves(cfg.typical);
    }
    r.curves = analyze_curves(fc);
    r.forecast = fc;

    // ---- 装配运行时 ----
    EmsRuntime rt;
    rt.config().dt_s      = cfg.dt_s;
    rt.config().log_every = cfg.log_every;
    rt.config().enable_log = true;
    rt.config().demand_window_s = cfg.econ.demand_window_s;

    rt.configure_plant(cfg.plant);
    // 顺序要紧：configure_plant() 内部会 refresh_device_limits() 重置 dev_
    rt.device_limits()      = cfg.limits;
    rt.safety_params()      = cfg.safety;
    rt.coordinator_config() = cfg.coord;
    rt.fsm_config()         = cfg.fsm;
    rt.set_forecast(fc);
    rt.apply_configs();

    const int steps = static_cast<int>(std::lround(cfg.duration_s / cfg.dt_s));
    r.steps = steps;

    auto t0 = std::chrono::steady_clock::now();
    // 每拍**之前**注入当拍环境（负荷 / 光伏），曲线按阶梯保持采样；
    // 同时按时间窗注入故障（若有）。
    const double dt = cfg.dt_s;
    const Sim24hConfig& ccfg = cfg;
    int fault_ticks = 0;
    rt.run(steps, cfg.dt_s, [&fc, &ccfg, &fault_ticks, dt](EmsRuntime& rr, int i) {
        const double t = static_cast<double>(i) * dt;
        double ld = 0.0, pv = 0.0;
        fc.sample(t, &ld, &pv, nullptr);
        rr.set_environment(ld, pv);
        bool any_fault = false;
        if (!ccfg.fault_windows.empty()) {
            for (const auto& w : ccfg.fault_windows) {
                const bool on = w.active(t);
                apply_fault(rr, w.kind, on);
                if (on) any_fault = true;
            }
        }
        if (any_fault) ++fault_ticks;

        // 上层运行许可：故障恢复后（无故障源且已回到 READY）重新下发启动命令。
        // 这是"人工确认后重启"的仿真等价物 —— 用于验证恢复路径可用，
        // 而不是让系统自动带载（后者是安全上明确禁止的）。
        if (ccfg.auto_restart_after_fault && !any_fault &&
            rr.fsm().state() == EmsState::kReady) {
            rr.fsm().request_run(true);
        }
    });
    auto t1 = std::chrono::steady_clock::now();
    r.wall_s = std::chrono::duration<double>(t1 - t0).count();

    r.log    = rt.log();
    r.log_rows = static_cast<int>(r.log.size());
    r.metrics = rt.metrics();
    r.fault_ticks = fault_ticks;

    // ---- 经济性 ----
    r.econ = compute_economics(r.log, fc, cfg.econ, r.rec_dt_s,
                               cfg.plant.battery_capacity_kwh,
                               cfg.safety.soc_min, cfg.safety.soc_max,
                               cfg.soc_tol);

    // ---- 告警 ----
    collect_alarms(rt, cfg, r.log, r.alarms);
    r.alarm_count = static_cast<int>(r.alarms.size());
    for (const auto& a : r.alarms) if (a.level == "FAULT") ++r.fault_count;

    // ---- 不变量 ----
    const double eps = 1e-6;
    const double allow_chg = std::min(cfg.limits.pcs_rated_chg_kw, cfg.limits.bms_chg_limit_kw);
    const double allow_dis = std::min(cfg.limits.pcs_rated_dis_kw, cfg.limits.bms_dis_limit_kw);
    r.max_grid_kw = -1e18;
    r.min_grid_kw =  1e18;
    // 稳态窗口按**时间**换算成日志行数（见 Sim24hConfig::settle_ticks 注释）
    const int settle_rows = std::max(
        1, static_cast<int>(std::lround(cfg.settle_ticks * cfg.dt_s / r.rec_dt_s)));
    int ungated_run = 0;
    for (const auto& s : r.log) {
        r.max_grid_kw = std::max(r.max_grid_kw, s.p_grid);
        r.min_grid_kw = std::min(r.min_grid_kw, s.p_grid);

        if (s.p_cmd >  allow_dis + eps) ++r.over_limit;
        if (s.p_cmd < -allow_chg - eps) ++r.over_limit;
        if (s.p_cmd > s.p_upper + eps || s.p_cmd < s.p_lower - eps) ++r.out_of_interval;
        if (s.state_gated && std::fabs(s.p_cmd) > eps) ++r.gated_nonzero;

        if (s.state_gated) ungated_run = 0; else ++ungated_run;
        if (ungated_run > settle_rows) {
            bool breached = false;
            if (s.p_grid < cfg.grid_min_required - eps) breached = true;
            if (s.p_grid > cfg.grid_max_required + eps) breached = true;
            if (breached) {
                ++r.grid_breach;
                // 归因：储能此刻是否已无可用容量（SOC 触底禁放 / 触顶禁充）
                const bool soc_at_floor = (s.soc <= cfg.safety.soc_min + cfg.soc_tol);
                const bool soc_at_ceil  = (s.soc >= cfg.safety.soc_max - cfg.soc_tol);
                if (soc_at_floor || soc_at_ceil) ++r.grid_breach_soc_limited;
            }
            if (cfg.tr_check_cap_kw > 0.0) {
                const double tr = std::fabs(s.p_grid) + 0.1 * s.p_load;
                if (tr > cfg.safety.tr_overload_th * cfg.tr_check_cap_kw * cfg.tr_check_tol)
                    ++r.tr_breach;
            }
        }

        // 分时电价分布
        double pr = 0.0;
        if (fc.loaded) fc.sample(s.t, nullptr, nullptr, &pr);
        const double dt_h = r.rec_dt_s / 3600.0;
        const double imp = std::max(0.0, s.p_grid) * dt_h;
        const double chg = std::max(0.0, -s.p_actual) * dt_h;
        const double dis = std::max(0.0,  s.p_actual) * dt_h;
        if (pr <= 0.35)      { r.e_valley_kwh += imp; r.e_chg_valley_kwh += chg; }
        else if (pr <= 0.80) { r.e_flat_kwh   += imp; }
        else if (pr <= 1.10) { r.e_peak_kwh   += imp; r.e_dis_peak_kwh += dis; }
        else                 { r.e_sharp_kwh  += imp; r.e_dis_peak_kwh += dis; }
    }
    if (r.log.empty()) { r.max_grid_kw = 0.0; r.min_grid_kw = 0.0; }

    r.ok = true;
    return r;
}

// =====================================================================
// 默认 24h 场景（630 kVA 变压器 / 250 kW PCS / 1000 kWh 电池）
// =====================================================================
inline Sim24hConfig make_default_24h_config() {
    Sim24hConfig c;

    // ---- 电池 / PCS ----
    c.plant.battery_capacity_kwh = 1000.0;
    c.plant.soc_init             = 0.50;
    c.plant.pcs_max_chg_kw       = 250.0;
    c.plant.pcs_max_dis_kw       = 250.0;
    c.plant.pcs_ramp_kw_per_s    = 400.0;
    c.plant.pcs_tau_s            = 0.30;
    c.plant.pcs_deadtime_s       = 0.10;
    c.plant.eta_chg = c.plant.eta_dis = 0.95;

    // ---- 设备限值 ----
    c.limits.pcs_rated_chg_kw = c.limits.pcs_rated_dis_kw = 250.0;
    c.limits.bms_chg_limit_kw = c.limits.bms_dis_limit_kw = 250.0;
    c.limits.transformer_capacity_kw = 500.0;
    c.limits.d_target_kw             = 400.0;

    // ---- 安全 ----
    c.safety.soc_min = 0.10;
    c.safety.soc_max = 0.90;
    c.safety.grid_p_min_kw = 0.0;      // 不允许倒送
    c.safety.grid_p_max_kw = 400.0;    // 契约需量硬兜底
    // 并网上界前瞻：本场景的预报就是环境的预测（同一组曲线），故开启。
    // 上限 50 kW 足以覆盖 15 min 阶梯的实测最大跳变（本典型日实测 12.5 kW），
    // 同时防止"预报失真"被当成真实跳变而错误收紧并网上界。
    c.safety.grid_lookahead_max_drop_kw = 50.0;

    // ---- 协同 ----
    c.coord.reopt_period_s = 900.0;    // 15 min 滚动重优化

    // ---- 经济 ----
    c.econ.demand_charge_cny_per_kw_day = 1.20;
    c.econ.feed_in_price_cny_per_kwh    = 0.0;

    // ---- 不变量 ----
    c.grid_min_required = -5.0;        // 允许 5 kW 滤波暂态
    c.grid_max_required = 400.0 * 1.02;
    c.tr_check_cap_kw   = 500.0;
    c.tr_check_tol      = 1.05;

    return c;
}

} // namespace ems
