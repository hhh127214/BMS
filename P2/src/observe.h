// =====================================================================
// P2/ — 产品化 P2 可观测性 · 运行时观察者
//
// ---------------------------------------------------------------------
// 这是 P2 的接入点：把 soe.h / metrics.h / trace.h 挂到 EmsRuntime 上。
// ---------------------------------------------------------------------
// 关键约束：**不修改 07/**。观察者从外部喂 StepRecord，用法：
//
//     EmsRuntime rt;  rt.init();
//     RuntimeObserver obs;
//     obs.start(0.0);
//     for (...) {
//         rt.set_environment(load, pv);
//         StepRecord rec = rt.step(dt);
//         obs.on_step(rt, rec);          // ← 唯一的接入点
//     }
//     obs.stop(t);
//
// 这样做的理由：可观测性不该侵入被测对象。07/ 的 `step()` 已经是
// "一次闭环"的完整语义，观察者只读它的输出 + 只读访问 rt 的公开状态。
//
// ---------------------------------------------------------------------
// 两个核心机制
// ---------------------------------------------------------------------
// **① 边沿检测（edge detection）→ SOE**
//   事件不是"每拍的状态"，而是"状态的变化"。一次持续 838 拍的安全限幅
//   应该产生 2 条事件（Start / End），不是 838 条。所以观察者保存上一拍的
//   布尔量，只在跳变时记事件。
//   这一点 P2 之前散落在 10/ 的 collect_alarms() 里手工实现，
//   每个指标都要手写一遍 —— 现在收敛成一张 `edge_state_` 表。
//
//   例外：**状态迁移**与**硬不变量违例**不做抑制（suppressible=false）——
//   合并它们会丢掉迁移链（A→B→C 变成 A 重复 2 次），而迁移链正是要留的。
//
// **② 逐拍累积（incremental）→ 指标**
//   所有指标 O(1) 更新，不保留历史。特别是 `cmd_travel`（指令总行程）与
//   `sign_flips`：这两个必须**逐拍**算才准 —— 用降采样后的日志算会漏掉
//   中间抖动，而"抖动"恰恰是它们要度量的东西。
//
// 硬不变量（指令逃逸 / 功率超限 / 门控失效）逐拍判定并累计 —— 与周期 10
// 的口径一致：违反即架构级问题，任何降采样都不能掩盖。
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "metrics.h"
#include "soe.h"
#include "trace.h"

#include "realtime_loop.h"     // EmsRuntime / StepRecord
#include "device_io.h"         // DeviceLimits

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <sstream>
#include <string>

namespace ems {

// =====================================================================
// 观察者配置
// =====================================================================
struct ObserverConfig {
    SoeConfig   soe;
    TraceConfig trace;

    // SOC 接近限值的判定容差（量测噪声 / 执行机构滑移）
    double soc_tol = 0.005;
    // 关口 / 需量越限判定容差（kW）
    double grid_tol_kw = 1.0;
    // 硬不变量判定容差（kW）
    double inv_tol_kw = 1e-6;
    // 需量窗口（0 = 用 EmsRuntime 的 LoopConfig::demand_window_s）
    double demand_window_s = 0.0;
    // 符号翻转判定死区（kW）：|p_cmd| 超过它才参与符号判定，避免 0 附近误判。
    // 与 07/LoopMetrics 的 sign_eps 同口径。
    double sign_eps_kw = 0.5;
    // 方向反转判定的最小幅度（kW）：滤掉数值噪声。与 07/ 的 dir_eps 同口径。
    double dir_eps_kw = 2.0;
    // 是否统计硬不变量
    bool watch_invariants = true;
};

// =====================================================================
// 逐拍累积量（与 log_every 无关）
// =====================================================================
struct ObserverTotals {
    int    steps = 0;
    double duration_s = 0.0;

    // ---- 能量（kWh）----
    double e_import_kwh = 0.0;
    double e_export_kwh = 0.0;
    double e_chg_kwh    = 0.0;
    double e_dis_kwh    = 0.0;

    // ---- 跟踪质量（逐拍精确）----
    double sum_sq_err_kw2 = 0.0;
    double sum_abs_err_kw = 0.0;
    double max_abs_err_kw = 0.0;
    int    track_samples  = 0;

    // ---- 输出质量（必须逐拍算，降采样会漏）----
    double cmd_travel_kw  = 0.0;   // Σ|Δcmd|：指令总行程
    int    sign_flips     = 0;     // p_cmd 过零次数（符号翻转）
    int    cmd_reversals  = 0;     // Δcmd 符号翻转次数（方向反转，振荡的直接度量）
    double max_abs_cmd_kw = 0.0;

    // ---- 关口 / 需量 ----
    double min_grid_kw = 0.0;
    double max_grid_kw = 0.0;
    double demand_peak_kw = 0.0;   // 需量窗口均值的最大值

    // ---- SOC / 温度 ----
    double soc_min = 1.0, soc_max = 0.0, soc_end = 0.0;
    double temp_max_c = -1e18;

    // ---- 计数（拍）----
    int safety_clip_ticks   = 0;
    int state_gate_ticks    = 0;
    int hold_last_ticks     = 0;
    int grid_reverse_ticks  = 0;
    int tr_overload_ticks   = 0;
    int soc_violation_ticks = 0;
    int fault_ticks         = 0;
    int state_changes       = 0;

    // ---- 硬不变量（必须为 0）----
    int out_of_interval_ticks = 0;
    int over_limit_ticks      = 0;
    int gated_nonzero_ticks   = 0;

    bool hard_invariants_ok() const {
        return out_of_interval_ticks == 0 && over_limit_ticks == 0
            && gated_nonzero_ticks == 0;
    }

    double rmse_track_kw() const {
        return track_samples ? std::sqrt(sum_sq_err_kw2 / track_samples) : 0.0;
    }
    double mean_abs_track_err_kw() const {
        return track_samples ? sum_abs_err_kw / track_samples : 0.0;
    }
};

// =====================================================================
// 运行时观察者
// =====================================================================
class RuntimeObserver {
public:
    explicit RuntimeObserver(const ObserverConfig& c = ObserverConfig()) : cfg_(c) {
        soe_.set_config(cfg_.soe);
        trace_.set_config(cfg_.trace);
        edge_state_.fill(false);
        fault_bit_.fill(false);
        register_metrics();
    }

    // ---- 接入点 ----
    void start(double t) {
        running_ = true;
        soe_.add(t, SoeLevel::kInfo, SoeSource::kSystem, SoeCode::kObserverStart,
                 "观察者启动");
    }

    void stop(double t) {
        if (!running_) return;
        running_ = false;
        soe_.add(t, SoeLevel::kInfo, SoeSource::kSystem, SoeCode::kObserverStop,
                 "观察者停止");
    }

    // 每拍调用一次。dt 由 rec.t 与上一拍之差推断（支持变步长）。
    void on_step(EmsRuntime& rt, const StepRecord& rec) {
        double dt = have_prev_ ? (rec.t - t_prev_) : 0.0;
        if (dt <= 0.0) dt = rt.config().dt_s;
        t_prev_ = rec.t;

        if (!have_prev_) {
            t_start_ = rec.t;
            tot_.min_grid_kw = tot_.max_grid_kw = rec.p_grid;
            tot_.soc_min = tot_.soc_max = rec.soc;
            tot_.temp_max_c = rec.temp;
        }
        have_prev_ = true;

        ++tot_.steps;
        tot_.duration_s = rec.t - t_start_;

        const SafetyParams& sp  = rt.safety_params();
        const DeviceLimits& dev = rt.device_limits();
        const double h = dt / 3600.0;

        // ---------- ① 能量累积 ----------
        if (rec.p_grid > 0.0) tot_.e_import_kwh += rec.p_grid * h;
        else                  tot_.e_export_kwh += -rec.p_grid * h;
        if (rec.p_actual < 0.0) tot_.e_chg_kwh += -rec.p_actual * h;
        else                    tot_.e_dis_kwh +=  rec.p_actual * h;

        // ---------- ② 跟踪质量 ----------
        const double err = rec.p_cmd - rec.p_actual;
        const double ae  = std::fabs(err);
        tot_.sum_sq_err_kw2 += err * err;
        tot_.sum_abs_err_kw += ae;
        if (ae > tot_.max_abs_err_kw) tot_.max_abs_err_kw = ae;
        ++tot_.track_samples;

        // ---------- ③ 输出质量（逐拍精确）----------
        // 注意两个量的语义**不同**，不能合并：
        //   · sign_flips    —— 指令 p_cmd 自身过零（符号翻转）
        //   · cmd_reversals —— 增量 Δcmd 的符号翻转（方向反转，振荡的直接度量）
        // 阈值与 07/LoopMetrics::compute 同口径（sign_eps=0.5 / dir_eps=2.0），
        // 但这里是**逐拍**判定 —— 07/ 是在（可能被降采样的）日志上判定。
        const bool had_cmd = have_cmd_;
        const double d_step = had_cmd ? (rec.p_cmd - last_cmd_) : 0.0;
        if (had_cmd) {
            tot_.cmd_travel_kw += std::fabs(d_step);
            if (std::fabs(d_step) > cfg_.dir_eps_kw &&
                std::fabs(prev_d_step_) > cfg_.dir_eps_kw) {
                if ((d_step > 0.0) != (prev_d_step_ > 0.0)) ++tot_.cmd_reversals;
            }
            if (std::fabs(d_step) > cfg_.dir_eps_kw) prev_d_step_ = d_step;
        }
        if (std::fabs(rec.p_cmd) > cfg_.sign_eps_kw) {
            const int s = (rec.p_cmd > 0.0) ? 1 : -1;
            if (have_cmd_sign_ && s != last_cmd_sign_) ++tot_.sign_flips;
            last_cmd_sign_ = s;
            have_cmd_sign_ = true;
        }
        last_cmd_  = rec.p_cmd;
        have_cmd_  = true;
        if (std::fabs(rec.p_cmd) > tot_.max_abs_cmd_kw)
            tot_.max_abs_cmd_kw = std::fabs(rec.p_cmd);

        // ---------- ④ 关口 / 需量 ----------
        if (rec.p_grid < tot_.min_grid_kw) tot_.min_grid_kw = rec.p_grid;
        if (rec.p_grid > tot_.max_grid_kw) tot_.max_grid_kw = rec.p_grid;
        const double dem_avg = push_demand(rec.p_grid, dt, rt);
        if (dem_avg > tot_.demand_peak_kw) tot_.demand_peak_kw = dem_avg;

        // ---------- ⑤ SOC / 温度 ----------
        if (rec.soc < tot_.soc_min) tot_.soc_min = rec.soc;
        if (rec.soc > tot_.soc_max) tot_.soc_max = rec.soc;
        tot_.soc_end = rec.soc;
        if (rec.temp > tot_.temp_max_c) tot_.temp_max_c = rec.temp;

        // ---------- ⑥ 硬不变量（逐拍）----------
        if (cfg_.watch_invariants) {
            const double e = cfg_.inv_tol_kw;
            if (rec.p_cmd < rec.p_lower - e || rec.p_cmd > rec.p_upper + e) {
                ++tot_.out_of_interval_ticks;
                if (out_of_interval_ticks_++ == 0) {
                    SoeEvent ev;
                    ev.t = rec.t; ev.level = SoeLevel::kFatal;
                    ev.source = SoeSource::kSystem; ev.code = SoeCode::kInvariantBroken;
                    ev.suppressible = false;
                    ev.message = "指令逃逸 p_cmd ∉ [p_lower, p_upper]";
                    ev.data = {{"p_cmd", rec.p_cmd}, {"p_lower", rec.p_lower},
                               {"p_upper", rec.p_upper}};
                    soe_.push(ev);
                }
            }
            const double lim_dis = std::min(dev.pcs_rated_dis_kw, dev.bms_dis_limit_kw);
            const double lim_chg = std::min(dev.pcs_rated_chg_kw, dev.bms_chg_limit_kw);
            const bool over = (rec.p_cmd > 0.0 && rec.p_cmd > lim_dis + e)
                           || (rec.p_cmd < 0.0 && -rec.p_cmd > lim_chg + e);
            if (over) ++tot_.over_limit_ticks;

            if (rec.state_gated && std::fabs(rec.p_cmd) > e)
                ++tot_.gated_nonzero_ticks;
        }

        // ---------- ⑦ 条件量 ----------
        if (rec.safety_clip) ++tot_.safety_clip_ticks;
        if (rec.state_gated) ++tot_.state_gate_ticks;
        if (rec.hold_last)   ++tot_.hold_last_ticks;
        if (rec.fault_bits != 0) ++tot_.fault_ticks;

        const bool grid_rev = (rec.p_grid < sp.grid_p_min_kw - cfg_.grid_tol_kw);
        const bool tr_over  = (dev.transformer_capacity_kw > 0.0) &&
                              (std::fabs(rec.p_grid) >
                               sp.tr_overload_th * dev.transformer_capacity_kw);
        const bool soc_lo   = (rec.soc <= sp.soc_min + cfg_.soc_tol);
        const bool soc_hi   = (rec.soc >= sp.soc_max - cfg_.soc_tol);
        const bool soc_bad  = (rec.soc < sp.soc_min - cfg_.soc_tol) ||
                              (rec.soc > sp.soc_max + cfg_.soc_tol);
        const bool temp_hi  = (rec.temp >= sp.temp_warn_c);
        const bool dem_bad  = (dem_avg > dev.d_target_kw + cfg_.grid_tol_kw);

        if (grid_rev) ++tot_.grid_reverse_ticks;
        if (tr_over)  ++tot_.tr_overload_ticks;
        if (soc_bad)  ++tot_.soc_violation_ticks;

        // ---------- ⑧ 边沿检测 → SOE ----------
        // 状态迁移不自己推导，直接读状态机的**权威记录** rt.fsm().history()：
        //   · 迁移带 reason（现场要的是"为什么迁"，不是"迁到哪"）
        //   · 不会漏掉第一拍迁移（若自己用 prev_state_ 边沿检测，第一拍的
        //     INIT→SELF_CHECK 会被当成"初始状态"吃掉）
        //   · 迁移链必须完整 → suppressible=false，不允许被时间窗合并
        const auto& hist = rt.fsm().history();
        while (hist_seen_ < hist.size()) {
            const auto& h = hist[hist_seen_++];
            SoeEvent ev;
            ev.t = h.ts;
            ev.level = (h.to == EmsState::kFault || h.to == EmsState::kEmergency)
                     ? SoeLevel::kError
                     : (h.to == EmsState::kDerated ? SoeLevel::kWarn : SoeLevel::kInfo);
            ev.source = SoeSource::kFsm;
            ev.code = SoeCode::kFsmTransition;
            ev.suppressible = false;
            ev.message = std::string("状态迁移 ") + state_name(h.from)
                       + " → " + state_name(h.to)
                       + (h.reason.empty() ? "" : (" (" + h.reason + ")"));
            ev.data = {{"from", static_cast<double>(static_cast<int>(h.from))},
                       {"to",   static_cast<double>(static_cast<int>(h.to))}};
            soe_.push(ev);
            ++tot_.state_changes;
        }

        edge(rec.t, rec.safety_clip, SoeSource::kSafety,
             SoeCode::kSafetyClipStart, SoeCode::kSafetyClipEnd, SoeLevel::kWarn,
             "安全限幅开始", "安全限幅结束", {});

        edge(rec.t, grid_rev, SoeSource::kSafety,
             SoeCode::kGridReverseStart, SoeCode::kGridReverseEnd, SoeLevel::kError,
             "关口越下限/倒送", "关口回到限值内",
             {{"p_grid", rec.p_grid}, {"limit", sp.grid_p_min_kw}});

        edge(rec.t, tr_over, SoeSource::kSafety,
             SoeCode::kTrOverloadStart, SoeCode::kTrOverloadEnd, SoeLevel::kWarn,
             "变压器过载", "变压器负载回到阈值内",
             {{"p_grid", rec.p_grid}});

        edge(rec.t, soc_lo, SoeSource::kSafety,
             SoeCode::kSocLowStart, SoeCode::kSocLowEnd, SoeLevel::kWarn,
             "SOC 触及下限", "SOC 离开下限",
             {{"soc", rec.soc}, {"limit", sp.soc_min}});

        edge(rec.t, soc_hi, SoeSource::kSafety,
             SoeCode::kSocHighStart, SoeCode::kSocHighEnd, SoeLevel::kWarn,
             "SOC 触及上限", "SOC 离开上限",
             {{"soc", rec.soc}, {"limit", sp.soc_max}});

        edge(rec.t, temp_hi, SoeSource::kSafety,
             SoeCode::kTempHighStart, SoeCode::kTempHighEnd, SoeLevel::kWarn,
             "温度进入预警区", "温度离开预警区",
             {{"temp_c", rec.temp}});

        edge(rec.t, rec.hold_last, SoeSource::kComm,
             SoeCode::kHoldLastStart, SoeCode::kHoldLastEnd, SoeLevel::kError,
             "采集超时 → 保持上一拍指令", "采集恢复", {});

        edge(rec.t, dem_bad, SoeSource::kEcon,
             SoeCode::kDemandBreachStart, SoeCode::kDemandBreachEnd, SoeLevel::kWarn,
             "需量窗口均值超过契约需量", "需量回到契约内",
             {{"demand_kw", dem_avg}, {"target_kw", dev.d_target_kw}});

        // 故障位：按位边沿
        for (int bit = 0; bit < 8; ++bit) {
            const bool now = ((rec.fault_bits >> bit) & 1u) != 0u;
            if (now == fault_bit_[bit]) continue;
            fault_bit_[bit] = now;
            if (now) {
                soe_.add(rec.t, SoeLevel::kError, fault_source(bit), fault_code(bit),
                         std::string("故障位置位: ") + fault_name(bit),
                         {{"fault_bits", static_cast<double>(rec.fault_bits)}});
            } else {
                soe_.add(rec.t, SoeLevel::kInfo, fault_source(bit), fault_clear_code(bit),
                         std::string("故障位清除: ") + fault_name(bit));
            }
        }

        // ---------- ⑨ 指标（O(1)）----------
        metrics_.set("ems_soc", rec.soc);
        metrics_.set("ems_p_grid_kw", rec.p_grid);
        metrics_.set("ems_p_cmd_kw", rec.p_cmd);
        metrics_.set("ems_p_actual_kw", rec.p_actual);
        metrics_.set("ems_temp_c", rec.temp);
        metrics_.set("ems_demand_window_kw", dem_avg);
        metrics_.set("ems_p_upper_kw", rec.p_upper);
        metrics_.set("ems_p_lower_kw", rec.p_lower);

        metrics_.inc("ems_steps_total");
        if (rec.safety_clip)  metrics_.inc("ems_safety_clip_ticks_total");
        if (rec.state_gated)  metrics_.inc("ems_state_gate_ticks_total");
        if (rec.hold_last)    metrics_.inc("ems_hold_last_ticks_total");
        if (grid_rev)         metrics_.inc("ems_grid_reverse_ticks_total");
        if (tr_over)          metrics_.inc("ems_tr_overload_ticks_total");
        if (soc_bad)          metrics_.inc("ems_soc_violation_ticks_total");
        if (rec.fault_bits)   metrics_.inc("ems_fault_ticks_total");

        metrics_.observe("ems_track_error_kw", ae, metric_buckets::error_kw());
        if (had_cmd) metrics_.observe("ems_cmd_step_kw", std::fabs(d_step),
                                      metric_buckets::power_kw());

        metrics_.set("ems_duration_seconds", tot_.duration_s);
        metrics_.set("ems_energy_import_kwh", tot_.e_import_kwh);
        metrics_.set("ems_energy_export_kwh", tot_.e_export_kwh);
        metrics_.set("ems_energy_charge_kwh", tot_.e_chg_kwh);
        metrics_.set("ems_energy_discharge_kwh", tot_.e_dis_kwh);
        metrics_.set("ems_cmd_travel_kw", tot_.cmd_travel_kw);
        metrics_.set("ems_demand_peak_kw", tot_.demand_peak_kw);
    }

    // ---- 访问器 ----
    SoeLog&         soe()     { return soe_; }
    const SoeLog&   soe() const { return soe_; }
    MetricRegistry& metrics() { return metrics_; }
    const MetricRegistry& metrics() const { return metrics_; }
    TraceControl&   trace()   { return trace_; }
    const TraceControl& trace() const { return trace_; }
    const ObserverTotals& totals() const { return tot_; }
    const ObserverConfig& config() const { return cfg_; }

    void reset() {
        soe_.reset();
        metrics_.reset();
        trace_.reset();
        register_metrics();
        tot_ = ObserverTotals();
        have_prev_ = have_cmd_ = have_cmd_sign_ = running_ = false;
        out_of_interval_ticks_ = 0;
        last_cmd_ = 0.0; prev_d_step_ = 0.0; last_cmd_sign_ = 0;
        hist_seen_ = 0;
        edge_state_.fill(false);
        fault_bit_.fill(false);
        win_.clear(); win_sum_ = 0.0;
    }

    std::string summary_text() const {
        std::ostringstream os;
        os << "===== 观察者汇总（逐拍累积，与 log_every 无关）=====\n";
        os << "  步数 " << tot_.steps << " / 时长 " << tot_.duration_s << " s\n";
        os << "  关口 min/max " << tot_.min_grid_kw << " / " << tot_.max_grid_kw << " kW\n";
        os << "  需量峰值(窗口均值) " << tot_.demand_peak_kw << " kW\n";
        os << "  跟踪 RMSE " << tot_.rmse_track_kw()
           << " kW / 平均绝对误差 " << tot_.mean_abs_track_err_kw()
           << " kW / 最大 " << tot_.max_abs_err_kw << " kW\n";
        os << "  指令总行程 " << tot_.cmd_travel_kw << " kW / 符号翻转 "
           << tot_.sign_flips << " / 方向反转 " << tot_.cmd_reversals << "\n";
        os << "  能量 购 " << tot_.e_import_kwh << " / 上网 " << tot_.e_export_kwh
           << " / 充 " << tot_.e_chg_kwh << " / 放 " << tot_.e_dis_kwh << " kWh\n";
        os << "  SOC min/max/end " << tot_.soc_min << " / " << tot_.soc_max
           << " / " << tot_.soc_end << " / 温度最高 " << tot_.temp_max_c << " C\n";
        os << "  限幅 " << tot_.safety_clip_ticks << " / 门控 " << tot_.state_gate_ticks
           << " / HOLD_LAST " << tot_.hold_last_ticks
           << " / 故障 " << tot_.fault_ticks
           << " / 状态迁移 " << tot_.state_changes << " 拍\n";
        os << "  硬不变量 逃逸 " << tot_.out_of_interval_ticks
           << " / 超限 " << tot_.over_limit_ticks
           << " / 门控失效 " << tot_.gated_nonzero_ticks << "\n";
        os << soe_.summary_text();
        os << trace_.summary_text();
        return os.str();
    }

private:
    // 边沿检测：now != 上一拍 → 记一条事件（on / off 两种码）
    void edge(double t, bool now, SoeSource src, SoeCode on_code, SoeCode off_code,
              SoeLevel lv, const std::string& on_msg, const std::string& off_msg,
              std::initializer_list<SoeField> fields) {
        const int idx = static_cast<int>(on_code);
        if (idx < 0 || idx >= static_cast<int>(edge_state_.size())) return;
        bool& prev = edge_state_[idx];
        if (now == prev) return;
        prev = now;
        if (now) soe_.add(t, lv, src, on_code, on_msg, fields);
        else     soe_.add(t, SoeLevel::kInfo, src, off_code, off_msg);
    }

    static SoeSource fault_source(int bit) {
        switch (bit) {
            case 0: return SoeSource::kComm;     // bms_comm_lost
            case 1: return SoeSource::kComm;     // pcs_comm_lost
            case 2: return SoeSource::kComm;     // meter_comm_lost
            case 3: return SoeSource::kDevice;   // pcs_fault
            case 4: return SoeSource::kComm;     // data_invalid
            case 5: return SoeSource::kDevice;   // device_offline
            case 6: return SoeSource::kSafety;   // temp_fault
            case 7: return SoeSource::kFsm;      // emergency_stop
            default: return SoeSource::kSystem;
        }
    }
    static SoeCode fault_code(int bit) {
        switch (bit) {
            case 0: return SoeCode::kCommLostBms;
            case 1: return SoeCode::kCommLostPcs;
            case 2: return SoeCode::kCommLostMeter;
            case 3: return SoeCode::kPcsFaultSet;
            case 4: return SoeCode::kDataStaleStart;
            case 5: return SoeCode::kDeviceOffline;
            case 6: return SoeCode::kTempHighStart;
            case 7: return SoeCode::kFsmEmergencyStop;
            default: return SoeCode::kInvariantBroken;
        }
    }
    // 清除事件码必须与置位事件码**配对**，否则现场看到"PCS 故障置位"却收到
    // "通信恢复"会直接误判根因。
    static SoeCode fault_clear_code(int bit) {
        switch (bit) {
            case 0: case 1: case 2: return SoeCode::kCommRestored;
            case 3: return SoeCode::kPcsFaultClear;
            case 4: return SoeCode::kDataStaleEnd;
            case 5: return SoeCode::kDeviceOnline;
            case 6: return SoeCode::kTempHighEnd;
            case 7: return SoeCode::kCommRestored;
            default: return SoeCode::kCommRestored;
        }
    }
    static const char* fault_name(int bit) {
        switch (bit) {
            case 0: return "BMS 通信丢失";
            case 1: return "PCS 通信丢失";
            case 2: return "关口电表通信丢失";
            case 3: return "PCS 故障";
            case 4: return "数据无效";
            case 5: return "设备离线";
            case 6: return "温度故障";
            case 7: return "外部急停";
            default: return "未知故障位";
        }
    }

    // 需量窗口滑动平均（O(1) 增量维护）
    double push_demand(double p_grid, double dt, EmsRuntime& rt) {
        double win = cfg_.demand_window_s;
        if (win <= 0.0) win = rt.config().demand_window_s;
        win_.push_back(p_grid);
        win_sum_ += p_grid;
        const size_t n = static_cast<size_t>(
            std::max(1.0, win / std::max(1e-9, dt)));
        while (win_.size() > n) { win_sum_ -= win_.front(); win_.pop_front(); }
        return win_.empty() ? 0.0 : win_sum_ / static_cast<double>(win_.size());
    }

    void register_metrics() {
        metrics_.counter("ems_steps_total", "闭环步数", "1");
        metrics_.gauge("ems_duration_seconds", "累计时长", "s");
        metrics_.counter("ems_safety_clip_ticks_total", "安全限幅拍数", "1");
        metrics_.counter("ems_state_gate_ticks_total", "状态门控拍数", "1");
        metrics_.counter("ems_hold_last_ticks_total", "HOLD_LAST 拍数", "1");
        metrics_.counter("ems_grid_reverse_ticks_total", "关口越下限拍数", "1");
        metrics_.counter("ems_tr_overload_ticks_total", "变压器过载拍数", "1");
        metrics_.counter("ems_soc_violation_ticks_total", "SOC 越界拍数", "1");
        metrics_.counter("ems_fault_ticks_total", "故障拍数", "1");

        metrics_.gauge("ems_soc", "SOC", "1");
        metrics_.gauge("ems_p_grid_kw", "关口功率", "kW");
        metrics_.gauge("ems_p_cmd_kw", "指令功率", "kW");
        metrics_.gauge("ems_p_actual_kw", "实际功率", "kW");
        metrics_.gauge("ems_temp_c", "温度", "degC");
        metrics_.gauge("ems_demand_window_kw", "需量窗口均值", "kW");
        metrics_.gauge("ems_demand_peak_kw", "需量峰值", "kW");
        metrics_.gauge("ems_p_upper_kw", "允许功率上限", "kW");
        metrics_.gauge("ems_p_lower_kw", "允许功率下限", "kW");

        metrics_.gauge("ems_energy_import_kwh", "购电量", "kWh");
        metrics_.gauge("ems_energy_export_kwh", "上网电量", "kWh");
        metrics_.gauge("ems_energy_charge_kwh", "储能充电量", "kWh");
        metrics_.gauge("ems_energy_discharge_kwh", "储能放电量", "kWh");
        metrics_.gauge("ems_cmd_travel_kw", "指令总行程", "kW");

        metrics_.histogram("ems_track_error_kw", metric_buckets::error_kw(),
                           "跟踪误差绝对值", "kW");
        metrics_.histogram("ems_cmd_step_kw", metric_buckets::power_kw(),
                           "指令单拍变化量", "kW");
    }

    ObserverConfig cfg_;
    SoeLog         soe_;
    MetricRegistry metrics_;
    TraceControl   trace_;
    ObserverTotals tot_;

    bool   running_ = false;
    bool   have_prev_ = false;
    bool   have_cmd_ = false;
    bool   have_cmd_sign_ = false;
    double t_prev_ = 0.0, t_start_ = 0.0;
    double last_cmd_ = 0.0;
    double prev_d_step_ = 0.0;
    int    last_cmd_sign_ = 0;
    int    out_of_interval_ticks_ = 0;
    // 已消费的 rt.fsm().history() 条数（状态迁移的权威来源）
    size_t hist_seen_ = 0;

    std::deque<double> win_;
    double win_sum_ = 0.0;

    std::array<bool, 1024> edge_state_{};
    std::array<bool, 8>    fault_bit_{};
};

} // namespace ems
