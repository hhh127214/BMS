// =====================================================================
// 10/ — 周期 10：24h 离线仿真测试 · 经济性核算
//
// 依据：设计方案 §7 周期 10「输出功率曲线、SOC 曲线、策略状态、
//       **经济收益**、告警日志」
//
// 工商业两部制电价的核算口径（与现场结算单一致）：
//   总电费 = 电度电费 + 需量电费 − 上网收益
//   电度电费 = Σ 购电功率 × 分时电价 × Δt
//   需量电费 = 计量窗口内**最大平均购电功率** × 需量单价
//   上网收益 = Σ 上网电量 × 上网电价
//
// 储能的价值 = 有储能总费用 vs 无储能（基准）总费用的差额，再扣掉
// **电池衰减成本**（按度电摊销）—— 只报"省了多少"而不算衰减，会高估收益。
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "realtime_loop.h"         // StepRecord
#include "dispatch_coordinator.h"  // ForecastSeries

#include <algorithm>
#include <cmath>
#include <vector>

namespace ems {

// =====================================================================
// 经济性参数
// =====================================================================
struct EconParams {
    // 需量电费：元/(kW·天)。月度需量电价 36 元/kW/月 ≈ 1.20 元/(kW·天)
    double demand_charge_cny_per_kw_day = 1.20;
    // 上网电价（元/kWh）。0 = 不允许倒送（防逆流场景）
    double feed_in_price_cny_per_kwh = 0.0;
    // 电池投资与寿命（用于度电衰减成本）
    double battery_capex_cny_per_kwh = 1200.0;
    double battery_cycle_life        = 6000.0;
    double battery_round_trip_eff    = 0.90;
    // 需量计量窗口（s），国内一般为 15 min
    double demand_window_s = 900.0;

    // 度电衰减成本（元 / kWh 放电量）
    //   全生命周期放电量 = 循环寿命 × 容量 × 往返效率
    //   度电成本 = (单位投资 × 容量) / 全生命周期放电量 = 单位投资 / (寿命 × 效率)
    double degradation_cny_per_kwh_dis() const {
        const double denom = std::max(1e-9, battery_cycle_life * battery_round_trip_eff);
        return battery_capex_cny_per_kwh / denom;
    }
};

// =====================================================================
// 经济性结果
// =====================================================================
struct EconResult {
    // ---- 电量 ----
    double e_import_kwh    = 0.0;   // 购电量
    double e_export_kwh    = 0.0;   // 上网电量
    double e_charge_kwh    = 0.0;   // 储能充电电量
    double e_discharge_kwh = 0.0;   // 储能放电电量
    double throughput_kwh  = 0.0;   // 充放总量
    double equiv_cycles    = 0.0;   // 等效循环次数（放电量 / 容量）
    double pv_self_use_kwh = 0.0;   // 光伏自用电量

    // ---- 需量 ----
    double peak_grid_kw      = 0.0;  // 实际最大需量（15 min 平均）
    double peak_grid_base_kw = 0.0;  // 基准（无储能）最大需量

    // ---- 费用（元）----
    double cost_energy_cny      = 0.0;  // 实际电度电费
    double cost_demand_cny      = 0.0;  // 实际需量电费
    double revenue_feed_in_cny  = 0.0;  // 实际上网收益
    double cost_total_cny       = 0.0;  // 实际总费用

    double cost_energy_base_cny = 0.0;  // 基准电度电费
    double cost_demand_base_cny = 0.0;  // 基准需量电费
    double revenue_feed_in_base_cny = 0.0;
    double cost_total_base_cny  = 0.0;  // 基准总费用

    double saving_energy_cny    = 0.0;  // 电度电费节省
    double saving_demand_cny    = 0.0;  // 需量电费节省
    double saving_total_cny     = 0.0;  // 总节省（未扣衰减）
    double cost_degradation_cny = 0.0;  // 电池衰减成本
    double net_benefit_cny      = 0.0;  // 净收益 = 总节省 − 衰减成本

    // ---- 状态统计 ----
    int    samples         = 0;
    double soc_min = 1.0, soc_max = 0.0, soc_avg = 0.0, soc_end = 0.0;
    int    soc_violation   = 0;   // SOC 越限拍数
    int    grid_reverse_ticks = 0;
    int    safety_clip_ticks  = 0;
    int    gated_ticks        = 0;
    double max_abs_cmd_kw  = 0.0;
    double max_temp_c      = 0.0;

    double saving_pct() const {
        return (cost_total_base_cny > 1e-9)
             ? 100.0 * saving_total_cny / cost_total_base_cny : 0.0;
    }
    // 简单静态回收期（年）= 投资 / 年净收益（按当日净收益线性外推 365 天）
    double payback_years(double capex_cny) const {
        const double annual = net_benefit_cny * 365.0;
        return (annual > 1e-9) ? capex_cny / annual : 0.0;
    }
};

// =====================================================================
// 需量统计：计量窗口内的最大平均购电功率
//
// 为什么不用瞬时最大值：国内需量电费按**计量窗口平均功率**的最大值计费，
// 用瞬时峰值会把一个尖刺当成整月需量，严重高估。窗口默认 15 min。
// =====================================================================
inline double peak_demand_kw(const std::vector<StepRecord>& recs,
                             double rec_dt_s,
                             double window_s,
                             bool use_pv_baseline) {
    if (recs.empty() || rec_dt_s <= 0.0) return 0.0;
    const int win = std::max(1, static_cast<int>(std::lround(window_s / rec_dt_s)));

    auto pg_at = [use_pv_baseline](const StepRecord& s) {
        return use_pv_baseline ? (s.p_load - s.p_pv) : s.p_grid;
    };

    double best = 0.0, sum = 0.0;
    for (size_t i = 0; i < recs.size(); ++i) {
        sum += std::max(0.0, pg_at(recs[i]));
        if (static_cast<int>(i) >= win) sum -= std::max(0.0, pg_at(recs[i - win]));
        const int n = std::min(static_cast<int>(i) + 1, win);
        best = std::max(best, sum / n);
    }
    return best;
}

// =====================================================================
// 主入口：核算经济性
//
//   recs        仿真日志（可能已按 log_every 降采样）
//   fc          日曲线（取当时电价；未 loaded 时按 0 电价计，仅统计电量）
//   rec_dt_s    相邻日志记录的时间间隔（= dt_s × log_every）
//   capacity_kwh 电池容量（等效循环用）
//   soc_tol     SOC 越界容差。**不能取 0**：EMS 的 soc_min 是控制限值，
//               被控对象的物理下限是 plant.soc_phys_min（默认 0.05）。
//               PCS 带死区 + 惯性，指令停止时 SOC 会再滑过限值一点点
//               （实测 ~5e-5），用 1e-9 判据会把这种数值滑移记成"越界"。
//               默认 0.002（0.2%）足以覆盖执行机构动态，又远小于真实越限。
// =====================================================================
inline EconResult compute_economics(const std::vector<StepRecord>& recs,
                                    const ForecastSeries& fc,
                                    const EconParams& ep,
                                    double rec_dt_s,
                                    double capacity_kwh,
                                    double soc_min = 0.10,
                                    double soc_max = 0.90,
                                    double soc_tol = 0.002) {
    EconResult r;
    r.samples = static_cast<int>(recs.size());
    if (recs.empty() || rec_dt_s <= 0.0) return r;

    const double dt_h = rec_dt_s / 3600.0;
    double sum_soc = 0.0;
    double cost_energy = 0.0, cost_energy_base = 0.0;
    double feed_in = 0.0, feed_in_base = 0.0;

    for (const auto& s : recs) {
        double pr = 0.0;
        if (fc.loaded && fc.size() > 0) {
            double ld = 0.0, pv = 0.0;
            fc.sample(s.t, &ld, &pv, &pr);
        }

        const double pg      = s.p_grid;
        const double pg_base = s.p_load - s.p_pv;   // 基准：储能不动作
        const double pb      = s.p_actual;

        // ---- 电量 ----
        r.e_import_kwh    += std::max(0.0,  pg) * dt_h;
        r.e_export_kwh    += std::max(0.0, -pg) * dt_h;
        r.e_charge_kwh    += std::max(0.0, -pb) * dt_h;
        r.e_discharge_kwh += std::max(0.0,  pb) * dt_h;

        // ---- 费用 ----
        cost_energy      += std::max(0.0,  pg)      * pr * dt_h;
        cost_energy_base += std::max(0.0,  pg_base) * pr * dt_h;
        feed_in      += std::max(0.0, -pg)      * ep.feed_in_price_cny_per_kwh * dt_h;
        feed_in_base += std::max(0.0, -pg_base) * ep.feed_in_price_cny_per_kwh * dt_h;

        // ---- 状态 ----
        r.soc_min = std::min(r.soc_min, s.soc);
        r.soc_max = std::max(r.soc_max, s.soc);
        sum_soc  += s.soc;
        r.soc_end = s.soc;
        r.max_abs_cmd_kw = std::max(r.max_abs_cmd_kw, std::fabs(s.p_cmd));
        r.max_temp_c     = std::max(r.max_temp_c, s.temp);
        if (s.soc < soc_min - soc_tol || s.soc > soc_max + soc_tol) ++r.soc_violation;
        if (pg < -1.0)      ++r.grid_reverse_ticks;
        if (s.safety_clip)  ++r.safety_clip_ticks;
        if (s.state_gated)  ++r.gated_ticks;
    }
    r.soc_avg = sum_soc / recs.size();

    // ---- 需量 ----
    r.peak_grid_kw      = peak_demand_kw(recs, rec_dt_s, ep.demand_window_s, false);
    r.peak_grid_base_kw = peak_demand_kw(recs, rec_dt_s, ep.demand_window_s, true);

    // ---- 汇总费用 ----
    r.cost_energy_cny      = cost_energy;
    r.cost_energy_base_cny = cost_energy_base;
    r.revenue_feed_in_cny      = feed_in;
    r.revenue_feed_in_base_cny = feed_in_base;
    r.cost_demand_cny      = r.peak_grid_kw      * ep.demand_charge_cny_per_kw_day;
    r.cost_demand_base_cny = r.peak_grid_base_kw * ep.demand_charge_cny_per_kw_day;
    r.cost_total_cny       = r.cost_energy_cny + r.cost_demand_cny - r.revenue_feed_in_cny;
    r.cost_total_base_cny  = r.cost_energy_base_cny + r.cost_demand_base_cny
                           - r.revenue_feed_in_base_cny;

    r.saving_energy_cny = r.cost_energy_base_cny - r.cost_energy_cny;
    r.saving_demand_cny = r.cost_demand_base_cny - r.cost_demand_cny;
    r.saving_total_cny  = r.cost_total_base_cny  - r.cost_total_cny;

    r.throughput_kwh    = r.e_charge_kwh + r.e_discharge_kwh;
    r.equiv_cycles      = (capacity_kwh > 1e-9) ? r.e_discharge_kwh / capacity_kwh : 0.0;
    r.cost_degradation_cny = r.e_discharge_kwh * ep.degradation_cny_per_kwh_dis();
    r.net_benefit_cny   = r.saving_total_cny - r.cost_degradation_cny;

    // 光伏自用 = 发电量 − 上网量
    double e_pv = 0.0;
    for (int i = 0; i < static_cast<int>(fc.size()); ++i)
        e_pv += fc.p_pv_kw[i] * (fc.step_s / 3600.0);
    r.pv_self_use_kwh = std::max(0.0, e_pv - r.e_export_kwh);
    return r;
}

} // namespace ems
