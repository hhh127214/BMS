// =====================================================================
// 08/ — 周期 8：优化调度与实时控制协同 DispatchCoordinator
//
// 依据：工商业储能EMS调控策略设计方案.md §7 周期 8
//   「重点联调峰谷套利、动态预测优化、需求响应策略，搭建分层协同机制：
//     预测分析 → 优化层生成日间运行计划 → 实时控制层动态纠偏 →
//     安全层兜底约束 → PCS执行」
//   周期目标：「实现优化层规划、实时层纠偏、安全层兜底的分层管控体系」
//
// 分层职责（本文件负责前三层的"计划 + 纠偏"部分，安全层由 SafetyEngine 兜底）：
//
//   ┌── 预测分析 ──────────────────────────────────────────────┐
//   │ ForecastSeries：负荷 / 光伏 / 电价 96 点（15min）时序      │
//   └───────────────────────┬──────────────────────────────────┘
//                           ▼
//   ┌── 优化层（15min 滚动）───────────────────────────────────┐
//   │ IOptimizer.solve() → DayPlan                             │
//   │   生产环境 = 01/ 的 MILP 服务（HTTP /api/v1/optimize）    │
//   │   演示/降级 = HeuristicOptimizer（贪心，非最优）           │
//   └───────────────────────┬──────────────────────────────────┘
//                           ▼
//   ┌── 实时层纠偏（100ms）────────────────────────────────────┐
//   │ ① SOC 偏差反馈  ② 负荷偏差前馈  ③ 窗口电量预算补偿        │
//   │   → coordinated_target = plan_target + Σ corrections      │
//   └───────────────────────┬──────────────────────────────────┘
//                           ▼
//   ┌── 安全层兜底 ──► SafetyEngine（周期 5）──► PCS            │
//   └──────────────────────────────────────────────────────────┘
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "data_models.h"
#include "plan_loader.h"
#include "strategy_base.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace ems {

// =====================================================================
// 1. 预测序列
// =====================================================================
struct ForecastSeries {
    double step_s = 900.0;
    std::vector<double> p_load_kw;
    std::vector<double> p_pv_kw;
    std::vector<double> price;
    bool loaded = false;

    size_t size() const {
        return std::min(p_load_kw.size(),
               std::min(p_pv_kw.size(), price.size()));
    }
    double duration_s() const { return step_s * static_cast<double>(size()); }

    // 阶梯保持采样
    bool sample(double t_s, double* load, double* pv, double* pr) const {
        if (size() == 0) return false;
        double tt = t_s;
        double total = duration_s();
        if (total > 0.0) {
            tt = std::fmod(t_s, total);
            if (tt < 0.0) tt += total;
        }
        size_t i = static_cast<size_t>(tt / std::max(1e-6, step_s));
        if (i >= size()) i = size() - 1;
        if (load) *load = p_load_kw[i];
        if (pv)   *pv   = p_pv_kw[i];
        if (pr)   *pr   = price[i];
        return true;
    }
    double price_at(double t_s) const {
        double pr = 0.0;
        sample(t_s, nullptr, nullptr, &pr);
        return pr;
    }

    // 电价分位阈值（用于 TOU 分类）
    void price_quantiles(double* p_low, double* p_high) const {
        if (price.empty()) { if (p_low) *p_low = 0; if (p_high) *p_high = 0; return; }
        std::vector<double> s = price;
        std::sort(s.begin(), s.end());
        size_t n = s.size();
        if (p_low)  *p_low  = s[n / 3];
        if (p_high) *p_high = s[(n * 2) / 3];
    }

    // 按电价给出 TOU 类型（谷 / 平 / 峰 / 尖）
    TouType classify_tou(double price_now, double p_low, double p_high) const {
        if (p_high <= 0.0) return TouType::kFlat;
        if (price_now <= p_low)  return TouType::kValley;
        if (price_now >= p_high * 1.15) return TouType::kSharp;
        if (price_now >= p_high) return TouType::kPeak;
        return TouType::kFlat;
    }
};

// =====================================================================
// 2. 优化器接口
//    生产环境：01/ 的 MILP HTTP 服务（POST /api/v1/optimize）
//    本目录  ：HeuristicOptimizer 作为可运行兜底
// =====================================================================
class IOptimizer {
public:
    virtual ~IOptimizer() = default;
    virtual std::string name() const = 0;
    // start_slot：滚动重优化时的起始时段序号（points[].t_s 为绝对时刻）
    virtual DayPlan solve(const ForecastSeries& fc,
                          size_t start_slot,
                          double soc_now,
                          double soc_min,
                          double soc_max,
                          double capacity_kwh,
                          double p_chg_max_kw,
                          double p_dis_max_kw) = 0;
};

// 贪心启发式（降级兜底）：保证 01/ 服务不可用时系统仍在线运行
class HeuristicOptimizer : public IOptimizer {
public:
    std::string name() const override { return "heuristic_greedy"; }

    DayPlan solve(const ForecastSeries& fc,
                  size_t start_slot,
                  double soc_now,
                  double soc_min,
                  double soc_max,
                  double capacity_kwh,
                  double p_chg_max_kw,
                  double p_dis_max_kw) override {
        DayPlan p = build_plan_greedy(fc.price, fc.p_load_kw, fc.p_pv_kw,
                                      soc_now, soc_min, soc_max,
                                      capacity_kwh, p_chg_max_kw, p_dis_max_kw,
                                      fc.step_s / 3600.0, start_slot);
        p.source = "heuristic_greedy";
        return p;
    }
};

// =====================================================================
// 3. 协同配置
// =====================================================================
struct CoordinatorConfig {
    // 优化层
    double reopt_period_s = 900.0;      // 滚动优化周期（15min）
    bool   enable_reopt   = true;

    // 实时层纠偏
    double soc_kp = 600.0;              // SOC 偏差比例增益（kW / 单位SOC）
    double soc_ki = 3.0;                // SOC 偏差积分增益（kW / (单位SOC·s)）
    double soc_correction_max_kw = 60.0;

    double dev_kp = 0.5;                // 负荷偏差前馈增益
    double dev_correction_max_kw = 50.0;

    double energy_kp = 1.0;             // 窗口电量预算补偿增益
    double energy_correction_max_kw = 40.0;

    double total_correction_max_kw = 80.0;
    double on_plan_tol_kw = 10.0;       // "在计划上"的判定容差
    bool   enable_correction = true;
};

// =====================================================================
// 4. 协同器
// =====================================================================
class DispatchCoordinator {
public:
    struct Stats {
        int    reopt_count = 0;
        int    samples = 0;
        int    on_plan_samples = 0;
        double sum_abs_track_err = 0.0;
        double max_abs_track_err = 0.0;
        double sum_abs_soc_err = 0.0;
        double max_abs_soc_err = 0.0;
        double sum_abs_correction = 0.0;

        double mean_abs_track_err() const { return samples ? sum_abs_track_err / samples : 0.0; }
        double mean_abs_soc_err()   const { return samples ? sum_abs_soc_err / samples : 0.0; }
        double on_plan_ratio()      const { return samples ? static_cast<double>(on_plan_samples) / samples : 0.0; }
        double mean_abs_correction() const { return samples ? sum_abs_correction / samples : 0.0; }
    };

    DispatchCoordinator() = default;
    explicit DispatchCoordinator(const CoordinatorConfig& c) : cfg_(c) {}

    void set_config(const CoordinatorConfig& c) { cfg_ = c; }
    const CoordinatorConfig& config() const { return cfg_; }

    void set_optimizer(IOptimizer* opt) { opt_ = opt; }   // 非拥有
    void set_forecast(const ForecastSeries& fc) { fc_ = fc; }

    void set_plan(const DayPlan& p) {
        plan_ = p;
        plan_valid_ = p.loaded;
        last_reopt_ts_ = -1e18;
        reset_window();
    }
    const DayPlan& plan() const { return plan_; }
    bool plan_valid() const { return plan_valid_; }
    const ForecastSeries& forecast() const { return fc_; }

    void reset_stats() { stats_ = Stats{}; }

    // -----------------------------------------------------------------
    // 每拍调用
    // -----------------------------------------------------------------
    void update(const RealtimeSnapshot& rt, Timestamp t_s, double dt_s) {
        now_ = t_s;

        // ---- ① 优化层：到点滚动重优化 ----
        reoptimized_this_step_ = false;
        if (cfg_.enable_reopt && opt_ && fc_.loaded) {
            if (last_reopt_ts_ < -1e17 || (t_s - last_reopt_ts_) >= cfg_.reopt_period_s) {
                do_reoptimize(rt);
                last_reopt_ts_ = t_s;
                reoptimized_this_step_ = true;
            }
        }

        // ---- ② 计划目标采样 ----
        double p_plan = 0.0, soc_ref = rt.soc;
        if (plan_valid_) plan_.sample(t_s, &p_plan, &soc_ref);
        plan_target_kw_ = p_plan;
        soc_ref_ = soc_ref;

        // ---- ③ 窗口电量预算：本窗口已执行电量 vs 计划电量 ----
        update_window(rt, t_s, dt_s, p_plan);

        // ---- ④ 实时层纠偏 ----
        compute_corrections(rt, t_s, dt_s);

        // ---- ⑤ 统计 ----
        double track_err = std::fabs(rt.p_bat_actual_kw - p_plan);
        double soc_err   = std::fabs(rt.soc - soc_ref);
        stats_.samples++;
        stats_.sum_abs_track_err += track_err;
        stats_.max_abs_track_err = std::max(stats_.max_abs_track_err, track_err);
        stats_.sum_abs_soc_err += soc_err;
        stats_.max_abs_soc_err = std::max(stats_.max_abs_soc_err, soc_err);
        stats_.sum_abs_correction += std::fabs(total_correction_kw());
        if (track_err <= cfg_.on_plan_tol_kw) stats_.on_plan_samples++;
    }

    // ---- 输出 ----
    double plan_target_kw() const { return plan_target_kw_; }
    double soc_ref() const { return soc_ref_; }
    double soc_correction_kw() const { return soc_corr_; }
    double dev_correction_kw() const { return dev_corr_; }
    double energy_correction_kw() const { return energy_corr_; }

    double total_correction_kw() const {
        if (!cfg_.enable_correction) return 0.0;
        double c = soc_corr_ + dev_corr_ + energy_corr_;
        return clamp(c, -cfg_.total_correction_max_kw, cfg_.total_correction_max_kw);
    }
    // 实时层纠偏后的最终 L3 目标（供 PlanTrackingStrategy 跟踪）
    double coordinated_target_kw() const {
        return plan_target_kw_ + total_correction_kw();
    }
    bool on_plan(double tol_kw) const {
        return std::fabs(plan_target_kw_ - last_actual_kw_) <= tol_kw;
    }
    bool reoptimized_this_step() const { return reoptimized_this_step_; }
    const Stats& stats() const { return stats_; }
    int reopt_count() const { return stats_.reopt_count; }

    void reset_window() {
        win_elapsed_s_ = 0.0;
        win_energy_actual_kwh_ = 0.0;
        win_energy_plan_kwh_ = 0.0;
        soc_integral_ = 0.0;
    }

private:
    static double clamp(double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    void do_reoptimize(const RealtimeSnapshot& rt) {
        // 优化层：以**当前实测 SOC** 为初值，从当前时段起重算剩余时段计划
        // （滚动时域：预测 → 优化 → 执行 → 反馈 → 再优化）
        size_t start_slot = 0;
        if (fc_.loaded && fc_.step_s > 0.0) {
            size_t n = fc_.size();
            if (n > 0) start_slot = static_cast<size_t>(now_ / fc_.step_s) % n;
        }
        DayPlan np = opt_->solve(fc_, start_slot, rt.soc, dev_soc_min_, dev_soc_max_,
                                 dev_capacity_kwh_, dev_p_chg_max_, dev_p_dis_max_);
        if (!np.loaded || np.points.empty()) return;   // 求解失败 → 沿用旧计划（降级）
        np.source += "+rolling";
        plan_ = np;
        plan_valid_ = true;
        stats_.reopt_count++;
        reset_window();
    }

    void update_window(const RealtimeSnapshot& rt, Timestamp t_s, double dt_s, double p_plan) {
        double step = plan_valid_ ? plan_.step_s : 900.0;
        if (step <= 0.0) step = 900.0;
        double phase = std::fmod(t_s, step);
        if (phase < std::fmod(t_s - dt_s, step) || t_s <= 0.0) {
            // 跨窗口 → 归零重算
            win_elapsed_s_ = 0.0;
            win_energy_actual_kwh_ = 0.0;
            win_energy_plan_kwh_ = 0.0;
        }
        win_elapsed_s_ += dt_s;
        win_energy_actual_kwh_ += rt.p_bat_actual_kw * dt_s / 3600.0;
        win_energy_plan_kwh_   += p_plan * dt_s / 3600.0;
    }

    // 注：计划电量由 update_window() 累计到 win_energy_plan_kwh_，
    //     故此处无需再传 p_plan。
    void compute_corrections(const RealtimeSnapshot& rt, Timestamp t_s,
                             double dt_s) {
        last_actual_kw_ = rt.p_bat_actual_kw;

        // ① SOC 偏差反馈（e > 0 表示 SOC 高于参考 → 应多放少充）
        double e = rt.soc - soc_ref_;
        soc_integral_ += e * dt_s;
        // 积分限幅（防饱和）
        double i_max = cfg_.soc_correction_max_kw / std::max(1e-6, cfg_.soc_ki);
        soc_integral_ = clamp(soc_integral_, -i_max, i_max);
        soc_corr_ = clamp(cfg_.soc_kp * e + cfg_.soc_ki * soc_integral_,
                          -cfg_.soc_correction_max_kw, cfg_.soc_correction_max_kw);

        // ② 负荷偏差前馈（实测负荷高于预测 → 多放）
        double fc_load = rt.p_load_kw, fc_pv = rt.p_pv_kw, fc_price = 0.0;
        if (fc_.loaded) fc_.sample(t_s, &fc_load, &fc_pv, &fc_price);
        double dev = (rt.p_load_kw - rt.p_pv_kw) - (fc_load - fc_pv);
        dev_corr_ = clamp(cfg_.dev_kp * dev,
                          -cfg_.dev_correction_max_kw, cfg_.dev_correction_max_kw);

        // ③ 窗口电量预算补偿（本窗口偏离计划的电量，在剩余时间内追回）
        double step = plan_valid_ ? plan_.step_s : 900.0;
        double remain = step - std::fmod(t_s, step);
        if (remain < 1.0) remain = 1.0;
        double e_kwh = win_energy_actual_kwh_ - win_energy_plan_kwh_;
        double corr = -cfg_.energy_kp * (e_kwh * 3600.0 / remain);
        energy_corr_ = clamp(corr, -cfg_.energy_correction_max_kw,
                             cfg_.energy_correction_max_kw);
    }

public:
    // 设备参数注入（供优化层使用；由 runtime 在初始化时填好）
    void set_device_params(double soc_min, double soc_max,
                           double capacity_kwh,
                           double p_chg_max_kw, double p_dis_max_kw) {
        dev_soc_min_ = soc_min;
        dev_soc_max_ = soc_max;
        dev_capacity_kwh_ = capacity_kwh;
        dev_p_chg_max_ = p_chg_max_kw;
        dev_p_dis_max_ = p_dis_max_kw;
    }

    // 只读访问器（P1 配置化：校验"协同层设备参数与 DeviceLimits 一致"；
    // 也是 P2 可观测性要导出的量）。
    double device_soc_min()     const { return dev_soc_min_; }
    double device_soc_max()     const { return dev_soc_max_; }
    double device_capacity_kwh() const { return dev_capacity_kwh_; }
    double device_p_chg_max_kw() const { return dev_p_chg_max_; }
    double device_p_dis_max_kw() const { return dev_p_dis_max_; }

private:
    CoordinatorConfig cfg_{};
    IOptimizer*  opt_ = nullptr;
    ForecastSeries fc_{};
    DayPlan      plan_{};
    bool         plan_valid_ = false;

    Timestamp now_ = 0.0;
    Timestamp last_reopt_ts_ = -1e18;
    bool      reoptimized_this_step_ = false;

    double plan_target_kw_ = 0.0;
    double soc_ref_ = 0.5;
    double last_actual_kw_ = 0.0;

    double soc_corr_ = 0.0;
    double dev_corr_ = 0.0;
    double energy_corr_ = 0.0;
    double soc_integral_ = 0.0;

    double win_elapsed_s_ = 0.0;
    double win_energy_actual_kwh_ = 0.0;
    double win_energy_plan_kwh_ = 0.0;

    double dev_soc_min_ = 0.10, dev_soc_max_ = 0.90;
    double dev_capacity_kwh_ = 1000.0;
    double dev_p_chg_max_ = 200.0, dev_p_dis_max_ = 200.0;

    Stats stats_{};
};

// =====================================================================
// 5. L3 策略：跟踪"优化层计划 + 实时层纠偏"后的协同目标
//
//    这就是设计方案里的"动态预测优化（dispatch_optimizer）"在
//    运行期的落地形态：计划来自 01/ 的 MILP，纠偏来自实时层。
//    运行模式 = kMPC，因此 StrategyManager::set_run_mode(kMPC) 会让它
//    独占 L3（峰谷套利 Timed / 需求响应 Custom 的 weight 置 0），
//    完全符合接口规范 §2.5 的"运行模式独占"。
// =====================================================================
class PlanTrackingStrategy : public IStrategy {
public:
    std::string id()   const override { return strategy_id::kForecastOpt; }
    std::string name() const override { return "动态预测优化（计划跟踪）"; }
    Priority    priority() const override { return Priority::kL3_GlobalEcon; }
    RunMode     mode() const override { return RunMode::kMPC; }

    // 由 DispatchCoordinator 每拍注入
    void set_target(double p_target_kw, double soc_ref, bool active) {
        target_kw_ = p_target_kw;
        soc_ref_ = soc_ref;
        active_ = active;
    }
    void set_bounds(double p_chg_kw, double p_dis_kw) {
        p_chg_ = std::max(0.0, p_chg_kw);
        p_dis_ = std::max(0.0, p_dis_kw);
    }
    void set_deadband(double d) { deadband_ = std::max(0.0, d); }

    double target_kw() const { return target_kw_; }
    double soc_ref() const { return soc_ref_; }

    StrategyResult evaluate(const RealtimeSnapshot& rt,
                            const DeviceLimits& dev) override {
        StrategyResult r;
        r.strategy_id = id();
        r.priority    = priority();
        r.weight      = get_param("__weight__", 1.0);

        const double p_chg = std::min(p_chg_, std::max(0.0, dev.pcs_rated_chg_kw));
        const double p_dis = std::min(p_dis_, std::max(0.0, dev.pcs_rated_dis_kw));
        r.p_lower = -p_chg;
        r.p_upper =  p_dis;

        double p = target_kw_;
        p = std::max(r.p_lower, std::min(r.p_upper, p));
        r.p_desired = p;

        // SOC 保护：接近边界时不再往错误方向推
        if (rt.soc <= get_param("soc_low", 0.12) && p > 0.0) {
            p = 0.0; r.p_desired = 0.0;
            r.reason = "plan_soc_low_hold";
            r.active = false;
            return r;
        }
        if (rt.soc >= get_param("soc_high", 0.88) && p < 0.0) {
            p = 0.0; r.p_desired = 0.0;
            r.reason = "plan_soc_high_hold";
            r.active = false;
            return r;
        }

        r.p_desired = p;
        r.active    = active_ && (std::fabs(p) > deadband_);
        r.reason    = r.active ? "plan_tracking" : "plan_idle";
        return r;
    }

private:
    double target_kw_ = 0.0;
    double soc_ref_   = 0.5;
    bool   active_    = false;
    double p_chg_     = 200.0;
    double p_dis_     = 200.0;
    double deadband_  = 1.0;
};

} // namespace ems
