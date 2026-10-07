// =====================================================================
// 17/ — 关口电表高保真模拟器（B4）
//
// 缺口原文（§10.2 B4）：
//   > **电表模拟器 = 两个 `sin()`**
//   > `11/src/integration_runner.h:134-138`：`380 + 120*sin(t/30)` /
//   > `150 + 60*sin(t/5)`。没有三相、没有电能累积、没有需量
//
// 本文件替换掉那两个 sin()。它要给的是**表计口径的完整性**，不是波形好看：
//
//   类别            本模拟器给什么                              为什么必须给
//   ─────────────────────────────────────────────────────────────────────────
//   三相电压/电流   相电压 + 线电压 + 三相电流 + 零序电流      防逆流/不平衡保护
//                   带**可配不平衡度**与**谐波含量**          的判据都吃这些量
//   有功/无功/视在  总 + 分相，PF = P/S                       需量策略吃 P，无功补偿吃 Q
//   电能累积        正/反向有功电能 + 四象限无功 + 日电能     经济性指标（15/ 的报表要它）
//                   **按功率积分**，不是"再写一个 sin()"
//   需量            当前需量 + 滑窗峰值 + **峰值发生时刻**    契约需量策略的命门
//                    真实表计的需量是"滑窗分格平均"，不是瞬时值
//
// ---------------------------------------------------------------------
// ★ A2 遗留的两条**已在这里落地**（§10.1 A2「遗留（本次未做）」）：
//   · **一阶时延**（`lag_tau_s`）：真机读数是滞后于真值的。原来只有常量偏差，
//     于是"先动作后读数"与"同时"在数据上不可区分。
//   · **慢漂移**（`drift_kw` + `drift_period_s`）：真机电表误差是慢漂移的。
//     只给常量偏差时，任何"用两次读数之差做校验"的判据都会**恒等**。
//   默认值刻意取（lag=0 + drift=0 + bias=0）= 理想电表 → 与 11/ 的旧行为逐位一致，
//   不引入新风险；要判据有区分度就显式打开（见 tests/test_sim_meter.cpp）。
//
// 编译：纯头文件（inline）。
// =====================================================================

#pragma once

#include "device_point_table.h"
#include "point_sink.h"

#include <cmath>
#include <string>

namespace ems {
namespace devsim {

// 点名用枚举取，**不写字面量** —— 打错一个字母就是"静默发不出去"，
// 而 DEV_POINT_NAME(kDpBmsSoc) 是编译期查表，错了编不过。
using devpt::dev_point_name;

inline std::string dp(const int index) { return std::string(dev_point_name(index)); }

struct MeterSimConfig {
    double u_phase_v       = 230.0;   // 相电压额定
    double freq_hz         = 50.0;    // 额定频率
    double imbalance       = 0.02;    // 三相不平衡度（各相幅度调制）
    double thd_u_pct       = 1.5;     // 电压总谐波畸变率基准（%）
    double thd_i_pct       = 3.0;     // 电流总谐波畸变率基准（%）
    double pf_nominal      = 0.98;    // 额定功率因数（用于由 P 反算三相电流）
    double lag_tau_s       = 0.0;     // 一阶时延；0 = 理想（与 11/ 旧行为一致）
    double bias_kw         = 0.0;     // 常量系统偏差（A2 注入点）
    double drift_kw        = 0.0;     // 慢漂移幅度；0 = 不漂
    double drift_period_s  = 90.0;    // 漂移周期
    double demand_window_s = 60.0;    // 需量滑窗长度（真机通常 900 s；演示压缩）
    int    demand_slots    = 10;      // 滑窗分格数
    double p_rated_kw      = 500.0;   // 用于拉电流（谐波随负载上升）
    bool   comm_ok         = true;
    bool   data_valid      = true;
    bool   fault           = false;
};

// 电表输入：真值（来自负荷/光伏/电池的功率平衡，或直接给关口真值）
struct MeterInputs {
    double p_kw      = 0.0;   // 关口总有功真值（进口为正）
    double q_kvar    = 0.0;
    double p_load_kw = 0.0;   // 分路：负荷
    double p_pv_kw   = 0.0;   // 分路：光伏
};

class MeterSim {
public:
    explicit MeterSim(const MeterSimConfig& c = MeterSimConfig()) : cfg_(c) {
        // ★ 先夹紧再算格长：如果先按未夹紧的 slots 算 slot_len_，再把 slots
        //   夹到上限，格长与格数就会**互相矛盾**（滑窗长度悄悄不等于 demand_window_s），
        //   需量峰值随之偏小 —— 而这类偏差在断言里表现得像"策略没跟上"，
        //   查起来会先怀疑策略而不是模拟器。
        if (cfg_.demand_slots < 1)         cfg_.demand_slots = 1;
        if (cfg_.demand_slots > kMaxSlots) cfg_.demand_slots = kMaxSlots;
        if (!(cfg_.demand_window_s > 0.0)) cfg_.demand_window_s = 60.0;
        slot_len_ = cfg_.demand_window_s / static_cast<double>(cfg_.demand_slots);
        ring_.assign(static_cast<std::size_t>(cfg_.demand_slots), 0.0);
    }

    // 静态配置点（初始化写一次）
    void publish_static(PointSink& sink) const {
        sink.set(dp(devpt::DP_METER_TR_KVA),   cfg_tr_kva_);
        sink.set(dp(devpt::DP_METER_D_TARGET), cfg_d_target_kw_);
    }

    // 一拍
    void step(double t, double dt, PointSink& sink, const MeterInputs& in) {
        // ① 一阶时延（真机读数是滞后的）
        const double alpha = (cfg_.lag_tau_s > 0.0)
                                 ? clamp(dt / cfg_.lag_tau_s, 0.0, 1.0) : 1.0;
        p_lag_ += (in.p_kw - p_lag_) * alpha;
        q_lag_ += (in.q_kvar - q_lag_) * alpha;

        // ② 系统偏差 + 慢漂移（A2 遗留）
        const double drift = (cfg_.drift_period_s > 0.0)
                                 ? cfg_.drift_kw * std::sin(2.0 * kPi * t / cfg_.drift_period_s)
                                 : 0.0;
        const double p_meas = p_lag_ + cfg_.bias_kw + drift;
        const double q_meas = q_lag_;

        // ③ 三相：幅度调制制造不平衡；电压带 0.5% 纹波，电流带谐波含量
        const double ripple_u = 0.005 * std::sin(2.0 * kPi * t / 3.0);
        double u[3], i[3];
        const double i_base = std::fabs(p_meas) * 1000.0 /
                              (3.0 * std::fmax(cfg_.u_phase_v, 1.0) * cfg_.pf_nominal);
        for (int k = 0; k < 3; ++k) {
            const double ph = 2.0 * kPi * t / 17.0 + static_cast<double>(k) * (2.0 * kPi / 3.0);
            const double mod = 1.0 + cfg_.imbalance * std::sin(ph);
            u[k] = cfg_.u_phase_v * mod * (1.0 + ripple_u);
            // 谐波畸变随负载上升 → 电流有效值抬高
            const double hx = 1.0 + 0.0001 * cfg_.thd_i_pct *
                                        std::fabs(p_meas) / std::fmax(cfg_.p_rated_kw, 1.0) *
                                        (1.0 + 0.3 * std::sin(2.0 * kPi * t / 5.0 + ph));
            i[k] = i_base / std::fmax(mod, 0.2) * hx;
        }
        const double u_ab = std::sqrt(3.0) * u[0];
        const double u_bc = std::sqrt(3.0) * u[1];
        const double u_ca = std::sqrt(3.0) * u[2];
        // 零序（近似）：相电流失衡的合成
        const double i_n = 0.5 * (std::fabs(i[0] - i[1]) + std::fabs(i[1] - i[2]));

        // ④ 功率 / 功率因数
        const double s_total = std::sqrt(p_meas * p_meas + q_meas * q_meas);
        const double pf = (s_total > 1e-9) ? (p_meas / s_total) : 1.0;
        const double freq = cfg_.freq_hz + 0.015 * std::sin(2.0 * kPi * t / 7.0);
        const double thd_u = cfg_.thd_u_pct * (1.0 + 0.2 * std::sin(2.0 * kPi * t / 11.0));
        const double thd_i = cfg_.thd_i_pct +
                             0.02 * std::fabs(p_meas) / std::fmax(cfg_.p_rated_kw, 1.0) *
                                 cfg_.thd_i_pct * (1.0 + 0.4 * std::sin(2.0 * kPi * t / 4.0));

        // 分相有功（按电压调制分配）
        // ★ 必须**归一化**：直接用 u[k]/U_n 的话，三相调制之和未必恰好等于 3，
        //   于是"分相有功之和 ≠ 总有功"—— 而分相量与总量的自洽是电表最基本的契约。
        double p_ph[3];
        const double wsum = u[0] / std::fmax(cfg_.u_phase_v, 1.0) +
                            u[1] / std::fmax(cfg_.u_phase_v, 1.0) +
                            u[2] / std::fmax(cfg_.u_phase_v, 1.0);
        for (int k = 0; k < 3; ++k) {
            const double w = (u[k] / std::fmax(cfg_.u_phase_v, 1.0)) / std::fmax(wsum, 1e-9);
            p_ph[k] = p_meas * w;
        }
        // 让"和"在浮点意义下也严格成立：末相取补
        p_ph[2] = p_meas - p_ph[0] - p_ph[1];

        // ⑤ 电能累积（**按功率积分** —— 这是"两个 sin()"最缺的东西）
        const double de = dt / 3600.0;                        // s → h
        ep_fwd_ += std::fmax(p_meas, 0.0) * de;
        ep_rev_ += std::fmax(-p_meas, 0.0) * de;
        eq_fwd_ += std::fmax(q_meas, 0.0) * de;
        eq_rev_ += std::fmax(-q_meas, 0.0) * de;
        ep_day_ += std::fabs(p_meas) * de;

        // ⑥ 需量（滑窗分格平均 + 峰值 + 峰值发生时刻）
        slot_sum_  += p_meas * dt;      // 本格的能量（kW·s）
        slot_time_ += dt;
        if (slot_time_ >= slot_len_ - 1e-12 && slot_time_ > 0.0) {
            ring_[static_cast<std::size_t>(ring_pos_)] = slot_sum_ / slot_time_;
            ring_pos_ = (ring_pos_ + 1) % static_cast<int>(ring_.size());
            if (ring_count_ < static_cast<int>(ring_.size())) ++ring_count_;
            slot_sum_  = 0.0;
            slot_time_ = 0.0;
        }
        // 当前需量 = (已归档格能量 + 本格部分能量) / 对应时长
        double ring_e = 0.0;
        for (int k = 0; k < ring_count_; ++k) ring_e += ring_[static_cast<std::size_t>(k)] * slot_len_;
        const double denom = static_cast<double>(ring_count_) * slot_len_ + slot_time_;
        const double demand_now = (denom > 1e-9) ? (ring_e + slot_sum_) / denom : p_meas;
        demand_now_ = demand_now;
        if (demand_now > demand_peak_) {
            demand_peak_    = demand_now;
            demand_peak_ts_ = t;
        }

        // ⑦ 发布
        sink.set(dp(devpt::DP_METER_U_A), u[0]);
        sink.set(dp(devpt::DP_METER_U_B), u[1]);
        sink.set(dp(devpt::DP_METER_U_C), u[2]);
        sink.set(dp(devpt::DP_METER_U_AB), u_ab);
        sink.set(dp(devpt::DP_METER_U_BC), u_bc);
        sink.set(dp(devpt::DP_METER_U_CA), u_ca);
        sink.set(dp(devpt::DP_METER_I_A), i[0]);
        sink.set(dp(devpt::DP_METER_I_B), i[1]);
        sink.set(dp(devpt::DP_METER_I_C), i[2]);
        sink.set(dp(devpt::DP_METER_I_N), i_n);
        sink.set(dp(devpt::DP_METER_FREQ), freq);
        sink.set(dp(devpt::DP_METER_P_TOTAL), p_meas);
        sink.set(dp(devpt::DP_METER_Q_TOTAL), q_meas);
        sink.set(dp(devpt::DP_METER_S_TOTAL), s_total);
        sink.set(dp(devpt::DP_METER_PF), pf);
        sink.set(dp(devpt::DP_METER_P_A), p_ph[0]);
        sink.set(dp(devpt::DP_METER_P_B), p_ph[1]);
        sink.set(dp(devpt::DP_METER_P_C), p_ph[2]);
        sink.set(dp(devpt::DP_METER_P_LOAD), in.p_load_kw);
        sink.set(dp(devpt::DP_METER_P_PV), in.p_pv_kw);
        sink.set(dp(devpt::DP_METER_THD_U), thd_u);
        sink.set(dp(devpt::DP_METER_THD_I), thd_i);
        sink.set(dp(devpt::DP_METER_EP_FWD), ep_fwd_);
        sink.set(dp(devpt::DP_METER_EP_REV), ep_rev_);
        sink.set(dp(devpt::DP_METER_EQ_FWD), eq_fwd_);
        sink.set(dp(devpt::DP_METER_EQ_REV), eq_rev_);
        sink.set(dp(devpt::DP_METER_EP_DAY), ep_day_);
        sink.set(dp(devpt::DP_METER_DEMAND_NOW), demand_now);
        sink.set(dp(devpt::DP_METER_DEMAND_PEAK), demand_peak_);
        sink.set(dp(devpt::DP_METER_DEMAND_PEAK_TS), demand_peak_ts_);
        sink.set(dp(devpt::DP_METER_COMM_OK), cfg_.comm_ok ? 1.0 : 0.0);
        sink.set(dp(devpt::DP_METER_DATA_VALID), cfg_.data_valid ? 1.0 : 0.0);
        sink.set(dp(devpt::DP_METER_FAULT), cfg_.fault ? 1.0 : 0.0);

        // 序号推进（供"上报序号"类判据；本模块不发布它，只留观测点）
        ++ticks_;
    }

    // ---- 访问器（测试与自证用）----
    double ep_fwd_kwh() const { return ep_fwd_; }
    double ep_rev_kwh() const { return ep_rev_; }
    double demand_now_kw() const { return demand_now_; }
    double demand_peak_kw() const { return demand_peak_; }
    double demand_peak_ts() const { return demand_peak_ts_; }
    long   ticks() const { return ticks_; }

    void set_cfg(const MeterSimConfig& c) { cfg_ = c; }
    const MeterSimConfig& cfg() const { return cfg_; }
    // 静态配置的外部注入（真机里来自组态，不来自电表本身）
    void set_site_config(double tr_kva, double d_target_kw) {
        cfg_tr_kva_     = tr_kva;
        cfg_d_target_kw_ = d_target_kw;
    }

private:
    static const int    kMaxSlots = 256;
    static constexpr double kPi = 3.14159265358979323846;

    static double clamp(double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    MeterSimConfig cfg_;
    double slot_len_ = 60.0;

    double p_lag_ = 0.0, q_lag_ = 0.0;

    double ep_fwd_ = 0.0, ep_rev_ = 0.0, eq_fwd_ = 0.0, eq_rev_ = 0.0, ep_day_ = 0.0;

    std::vector<double> ring_ = {0.0};
    int    ring_pos_   = 0;
    int    ring_count_ = 0;
    double slot_sum_   = 0.0;
    double slot_time_  = 0.0;

    double demand_peak_    = 0.0;
    double demand_peak_ts_ = 0.0;
    double demand_now_     = 0.0;

    double cfg_tr_kva_     = 250.0;
    double cfg_d_target_kw_ = 250.0;

    long ticks_ = 0;
};

} // namespace devsim
} // namespace ems
