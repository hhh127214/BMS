// =====================================================================
// 9 个策略的"参考实现"（设计方案 §3.1-§3.9 + 接口规范 §4）
//
// 说明：
//   - 本文件给出 9 个策略的标准实现形态（基于接口规范 §4 策略接口卡）
//   - 各策略核心算法已在 01/02/03 中实现（C/C++/C++），本文件为复用参考
//   - 演示模式（MOCK）：每个策略在 evaluate() 内给出**理想**输出区间+期望
//     实际部署时把这些类替换为调用 01/02/03 真实算法的适配器
//
// 编译：实现都是 inline 在头文件里，便于单文件 demo。
// =====================================================================

#pragma once

#include "strategy_base.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace ems {

// =====================================================================
// 策略一：S01_BMS_FORBID（BMS 禁止充放）— P0/L0
// =====================================================================
class BmsForbidStrategy : public IStrategy {
public:
    std::string id()   const override { return strategy_id::kBmsForbid; }
    std::string name() const override { return "BMS 禁止充放"; }
    Priority    priority() const override { return Priority::kL0_Safety; }

    StrategyResult evaluate(const RealtimeSnapshot& rt,
                            const DeviceLimits& dev) override {
        StrategyResult r;
        r.strategy_id = id();
        r.priority    = priority();
        r.weight      = 1.0;
        r.p_lower     = -1e18;
        r.p_upper     =  1e18;

        if (dev.bms_chg_forbidden && dev.bms_dis_forbidden) {
            r.active   = true;
            r.p_lower = r.p_upper = 0.0;
            r.reason  = "bms_forbid_both";
        } else if (dev.bms_chg_forbidden) {
            // 禁充 → 充电方向（P_bat < 0）禁止 → p_lower = 0
            r.active   = true;
            r.p_lower  = 0.0;
            r.reason   = "bms_forbid_charge";
        } else if (dev.bms_dis_forbidden) {
            // 禁放 → 放电方向（P_bat > 0）禁止 → p_upper = 0
            r.active   = true;
            r.p_upper  = 0.0;
            r.reason   = "bms_forbid_discharge";
        } else {
            r.reason = "bms_normal";
        }
        r.p_desired = 0.0;  // L0 不参与 desired 收敛
        return r;
    }
};

// =====================================================================
// 策略二：S02_BMS_DERATE（BMS 请求降功率）— L1
// =====================================================================
class BmsDerateStrategy : public IStrategy {
public:
    std::string id()   const override { return strategy_id::kBmsDerate; }
    std::string name() const override { return "BMS 请求降功率"; }
    Priority    priority() const override { return Priority::kL1_SafeOp; }

    StrategyResult evaluate(const RealtimeSnapshot& rt,
                            const DeviceLimits& dev) override {
        StrategyResult r;
        r.strategy_id = id();
        r.priority    = priority();
        r.weight      = 1.0;

        // 把 BMS 上送的限制裁剪到 PCS 额定之内
        double p_chg = std::min({dev.bms_chg_limit_kw,
                                 dev.pcs_rated_chg_kw});
        double p_dis = std::min({dev.bms_dis_limit_kw,
                                 dev.pcs_rated_dis_kw});

        r.p_lower  = -p_chg;
        r.p_upper  =  p_dis;
        r.p_desired = 0.0;  // L1 不参与 desired 收敛

        if (p_chg < dev.pcs_rated_chg_kw || p_dis < dev.pcs_rated_dis_kw) {
            r.active = true;
            r.reason = "bms_derating";
        } else {
            r.reason = "bms_normal";
        }
        return r;
    }
};

// =====================================================================
// 策略三：S03_TRANSFORMER_LIMIT（变压器过载限功率）— L1
// =====================================================================
class TransformerLimitStrategy : public IStrategy {
public:
    std::string id()   const override { return strategy_id::kTransformerLim; }
    std::string name() const override { return "变压器过载限功率"; }
    Priority    priority() const override { return Priority::kL1_SafeOp; }

    StrategyResult evaluate(const RealtimeSnapshot& rt,
                            const DeviceLimits& dev) override {
        StrategyResult r;
        r.strategy_id = id();
        r.priority    = priority();
        r.weight      = 1.0;
        r.p_desired   = 0.0;  // L1 不参与 desired 收敛

        // 估算变压器负载率
        // P_transformer = P_load - P_pv + P_chg (P_chg < 0)
        // 近似：负载率 ≈ |P_grid + P_bat|/capacity
        double tr_load_kw = std::abs(rt.p_grid_kw) +
                             rt.p_load_kw * 0.1; // 简化估算
        double capacity = dev.transformer_capacity_kw;
        double overload_th  = get_param("overload_threshold", 0.95);
        double extreme_th   = get_param("extreme_threshold", 1.10);

        if (capacity <= 0) {
            r.p_lower = -1e18; r.p_upper = 1e18;
            r.reason  = "tr_no_capacity";
            return r;
        }
        double ratio = tr_load_kw / capacity;

        // 默认按 PCS 额定幅度给区间
        double p_chg = dev.pcs_rated_chg_kw;
        double p_dis = dev.pcs_rated_dis_kw;

        if (ratio > extreme_th) {
            // 极端过载：强制禁放
            r.active   = true;
            r.p_upper  = 0.0;  // 禁放
            r.p_lower  = -p_chg;
            r.reason   = "tr_extreme_overload";
        } else if (ratio > overload_th) {
            // 轻度过载：按余量限功率
            double headroom_kw = std::max(0.0,
                capacity * (1.0 - ratio) / std::max(overload_th, 1e-6));
            double limit = std::min({p_dis, headroom_kw,
                                     dev.bms_dis_limit_kw});
            r.active   = true;
            r.p_upper  = limit;
            r.p_lower  = -p_chg;
            r.reason   = "tr_overload";
        } else {
            r.p_lower = -p_chg;
            r.p_upper =  p_dis;
            r.reason  = "tr_normal";
        }
        return r;
    }
};

// =====================================================================
// 策略四：S04_DEMAND_MGMT（需量管理）— L2
// =====================================================================
class DemandMgmtStrategy : public IStrategy {
public:
    std::string id()   const override { return strategy_id::kDemandMgmt; }
    std::string name() const override { return "需量管理"; }
    Priority    priority() const override { return Priority::kL2_LocalEcon; }
    RunMode     mode() const override { return RunMode::kCustom; }

    StrategyResult evaluate(const RealtimeSnapshot& rt,
                            const DeviceLimits& dev) override {
        StrategyResult r;
        r.strategy_id = id();
        r.priority    = priority();
        r.weight      = 1.0;

        // 简化版 PI：根据窗口已用功率预测本窗口最终均值
        double t_elapsed = rt.demand_window.t_elapsed_s;
        double t_window  = rt.demand_window.window_s;
        double p_avg_past = rt.demand_window.p_avg_past_kw;
        double D_target   = dev.d_target_kw;
        double Kp         = get_param("Kp_avg", 2.0);

        r.p_lower = -dev.pcs_rated_chg_kw;
        r.p_upper =  dev.pcs_rated_dis_kw;

        if (t_elapsed < 1.0 || t_window < 1.0) {
            r.p_desired = 0.0;
            r.reason    = "demand_window_invalid";
            return r;
        }

        // 预测最终窗口均值（外推 t_elapsed/t_window 的比例）
        double remain_s = std::max(0.0, t_window - t_elapsed);
        double ratio_remain = remain_s / t_window;
        double p_pred_avg = p_avg_past * (t_elapsed / t_window);
        // 假设剩余时间维持当前关口功率
        double p_grid_now = rt.p_grid_kw;
        p_pred_avg += p_grid_now * ratio_remain;

        double err = D_target - p_pred_avg;
        // 期望放电 = err * Kp（正 = 需放电）
        double p_des = Kp * err;
        // 限幅
        p_des = std::max(r.p_lower, std::min(r.p_upper, p_des));

        r.p_desired = p_des;
        r.active    = std::abs(err) > get_param("deadband", 1.0);
        r.reason    = r.active ? "demand_active" : "demand_within";
        return r;
    }
};

// =====================================================================
// 策略五：S05_ANTI_REVERSE（防逆流）— L2
// =====================================================================
class AntiReverseStrategy : public IStrategy {
public:
    std::string id()   const override { return strategy_id::kAntiReverse; }
    std::string name() const override { return "防逆流"; }
    Priority    priority() const override { return Priority::kL2_LocalEcon; }

    StrategyResult evaluate(const RealtimeSnapshot& rt,
                            const DeviceLimits& dev) override {
        StrategyResult r;
        r.strategy_id = id();
        r.priority    = priority();
        r.weight      = 1.0;

        r.p_lower = -dev.pcs_rated_chg_kw;
        r.p_upper =  dev.pcs_rated_dis_kw;

        // P_grid = P_load - P_pv - P_bat, P_bat > 0 = 放电
        // 防逆流要求 P_grid >= P_grid_min（默认 0）
        double p_grid_min = get_param("P_grid_min", 0.0);
        double surplus = p_grid_min - rt.p_grid_kw;
        // surplus > 0 → 关口功率偏低（即将倒送或已经倒送）→ 需要储能多充电
        double Kp = get_param("Kp", 1.0);

        if (surplus > 0) {
            // 需要充电（负方向 P_bat）
            double p_chg = Kp * surplus;
            // 限幅
            p_chg = std::min(p_chg, dev.pcs_rated_chg_kw);
            r.p_desired = -p_chg;
            r.active    = true;
            r.reason    = "anti_reverse_active";
        } else {
            r.p_desired = 0.0;
            r.active    = false;
            r.reason    = "anti_reverse_idle";
        }
        return r;
    }
};

// =====================================================================
// 策略六：S06_PV_SMOOTHING（光伏出力平抑）— L2
// =====================================================================
class PvSmoothingStrategy : public IStrategy {
public:
    std::string id()   const override { return strategy_id::kPvSmoothing; }
    std::string name() const override { return "光伏出力平抑"; }
    Priority    priority() const override { return Priority::kL2_LocalEcon; }

    StrategyResult evaluate(const RealtimeSnapshot& rt,
                            const DeviceLimits& dev) override {
        StrategyResult r;
        r.strategy_id = id();
        r.priority    = priority();
        r.weight      = 1.0;

        r.p_lower = -dev.pcs_rated_chg_kw;
        r.p_upper =  dev.pcs_rated_dis_kw;

        // 平滑逻辑：delta_pv = p_pv_now - p_pv_smoothed (内部状态)
        // 期望储能吸收 delta_pv（充电增加 = P_bat 更负）
        double Kp = get_param("Kp", 0.5);
        double delta = rt.p_pv_kw - p_pv_smoothed_;
        p_pv_smoothed_ += delta * 0.1; // 一阶滤波
        double desired_chg = -Kp * delta;  // 负值表示充电
        desired_chg = std::max(-dev.pcs_rated_chg_kw,
                       std::min(dev.pcs_rated_dis_kw, desired_chg));

        r.p_desired = desired_chg;
        r.active    = std::abs(delta) > get_param("deadband", 0.5);
        r.reason    = r.active ? "smoothing_active" : "smoothing_idle";
        return r;
    }
private:
    double p_pv_smoothed_ = 0.0;
};

// =====================================================================
// 策略七：S07_PEAK_VALLEY（峰谷套利）— L3（Timed 模式）
// =====================================================================
class PeakValleyStrategy : public IStrategy {
public:
    std::string id()   const override { return strategy_id::kPeakValley; }
    std::string name() const override { return "峰谷套利"; }
    Priority    priority() const override { return Priority::kL3_GlobalEcon; }
    RunMode     mode() const override { return RunMode::kTimed; }

    StrategyResult evaluate(const RealtimeSnapshot& rt,
                            const DeviceLimits& dev) override {
        StrategyResult r;
        r.strategy_id = id();
        r.priority    = priority();
        r.weight      = 1.0;
        r.p_lower     = -dev.pcs_rated_chg_kw;
        r.p_upper     =  dev.pcs_rated_dis_kw;

        // 简化：谷时充电（-P_bat）、峰时放电（+P_bat）
        switch (rt.pricing.cur_tou_type) {
            case TouType::kValley:
                r.p_desired = -get_param("P_charge", 80.0);
                r.active    = true;
                r.reason    = "peak_valley_charge";
                break;
            case TouType::kPeak:
            case TouType::kSharp:
                r.p_desired = +get_param("P_discharge", 80.0);
                r.active    = true;
                r.reason    = "peak_valley_discharge";
                break;
            case TouType::kFlat:
            default:
                r.p_desired = 0.0;
                r.active    = false;
                r.reason    = "peak_valley_flat";
                break;
        }
        // SOC 保护
        if (rt.soc < get_param("soc_low", 0.15) && r.p_desired > 0) {
            r.p_desired = 0.0;
            r.reason    = "peak_valley_soc_low";
        }
        if (rt.soc > get_param("soc_high", 0.85) && r.p_desired < 0) {
            r.p_desired = 0.0;
            r.reason    = "peak_valley_soc_high";
        }
        return r;
    }
};

// =====================================================================
// 策略八：S08_FORECAST_OPT（动态预测优化）— L3（MPC 模式）
// =====================================================================
class ForecastOptStrategy : public IStrategy {
public:
    std::string id()   const override { return strategy_id::kForecastOpt; }
    std::string name() const override { return "动态预测优化"; }
    Priority    priority() const override { return Priority::kL3_GlobalEcon; }
    RunMode     mode() const override { return RunMode::kMPC; }

    StrategyResult evaluate(const RealtimeSnapshot& rt,
                            const DeviceLimits& dev) override {
        StrategyResult r;
        r.strategy_id = id();
        r.priority    = priority();
        r.weight      = 1.0;
        r.p_lower     = -dev.pcs_rated_chg_kw;
        r.p_upper     =  dev.pcs_rated_dis_kw;

        // 简化：负荷越高越倾向放电
        double load = rt.p_load_kw;
        double d_target = dev.d_target_kw;
        double err = load - d_target;
        double p_des = err > 0
            ? +std::min(get_param("P_max", 60.0), err * 0.3)
            : -std::min(get_param("P_max", 60.0), -err * 0.3);

        r.p_desired = p_des;
        r.active    = std::abs(err) > get_param("deadband", 1.0);
        r.reason    = r.active ? "forecast_active" : "forecast_within";
        return r;
    }
};

// =====================================================================
// 策略九：S09_DEMAND_RESPONSE（需求响应）— L3（Custom 模式）
// =====================================================================
class DemandResponseStrategy : public IStrategy {
public:
    std::string id()   const override { return strategy_id::kDemandResponse; }
    std::string name() const override { return "需求响应"; }
    Priority    priority() const override { return Priority::kL3_GlobalEcon; }
    RunMode     mode() const override { return RunMode::kCustom; }

    // 外部需求响应事件
    struct DrEvent {
        bool   active     = false;
        double target_kw  = 0.0;   // 期望放电正 / 充电负
        Timestamp end_ts  = 0.0;
    };
    void set_event(DrEvent ev) { event_ = ev; }

    StrategyResult evaluate(const RealtimeSnapshot& rt,
                            const DeviceLimits& dev) override {
        StrategyResult r;
        r.strategy_id = id();
        r.priority    = priority();
        r.weight      = 1.0;
        r.p_lower     = -dev.pcs_rated_chg_kw;
        r.p_upper     =  dev.pcs_rated_dis_kw;

        if (!event_.active || rt.timestamp > event_.end_ts) {
            r.p_desired = 0.0;
            r.active    = false;
            r.reason    = "dr_idle";
            return r;
        }

        double p_des = event_.target_kw;
        p_des = std::max(r.p_lower, std::min(r.p_upper, p_des));
        r.p_desired = p_des;
        r.active    = true;
        r.reason    = "dr_active";
        return r;
    }
private:
    DrEvent event_{};
};

// 工厂：注册全部 9 策略到管理器
inline void register_all_9_strategies(StrategyManager& mgr) {
    mgr.register_strategy(std::make_shared<BmsForbidStrategy>());
    mgr.register_strategy(std::make_shared<BmsDerateStrategy>());
    mgr.register_strategy(std::make_shared<TransformerLimitStrategy>());
    mgr.register_strategy(std::make_shared<DemandMgmtStrategy>());
    mgr.register_strategy(std::make_shared<AntiReverseStrategy>());
    mgr.register_strategy(std::make_shared<PvSmoothingStrategy>());
    mgr.register_strategy(std::make_shared<PeakValleyStrategy>());
    mgr.register_strategy(std::make_shared<ForecastOptStrategy>());
    mgr.register_strategy(std::make_shared<DemandResponseStrategy>());
}

} // namespace ems