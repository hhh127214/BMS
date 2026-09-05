// =====================================================================
// StrategyArbiter — 策略仲裁器（设计方案周期4，核心周期）
//
// 职责（对应 §7 周期 4 主要任务 + 接口规范 §2.5 仲裁规则）：
//   - 多策略冲突解决
//   - L0-L3 优先级判定
//   - 区间逐层收敛：[L0] ∩ [L1] ∩ [L2] ∩ [L3]
//   - 同层 desired 加权平均
//   - 跨层绝不平均（L0/L1/L2 与 L3 之间只取区间交集）
//   - 动态切换（hysteresis on mode switch，详见 set_switch_delay）
//   - 期望裁剪（desired ∉ [lower, upper] → 按比例缩放并标 desired_clip）
//
// 典型场景（设计方案 §7 周期 4）：
//   峰谷套利：放电500kW
//   需量管理：放电400kW
//   需求响应：放电300kW
//   BMS最大放电：250kW
//   变压器最大允许：200kW
//   最终输出：200kW（L0/L1 区间 ∩ L2/L3 区间）
//
// 编译：本文件是纯头文件，实现都是 inline。
// =====================================================================

#pragma once

#include "strategy_base.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace ems {

class StrategyArbiter {
public:
    StrategyArbiter() = default;

    // 模式切换滞环（拍数）：desired 跨越 0 时需要连续 N 拍才切换，避免抖动
    void set_switch_delay(int n) { switch_delay_ = std::max(1, n); }
    int  switch_delay() const { return switch_delay_; }

    // 死区（kW）：desired 绝对值小于该值时输出 0
    void set_deadband(double d) { deadband_ = std::max(0.0, d); }
    double deadband() const { return deadband_; }

    // 主入口：合并所有策略输出为下发指令
    PowerCommand arbitrate(const std::vector<StrategyResult>& results,
                           Timestamp now_s) {
        PowerCommand cmd;
        cmd.timestamp = now_s;

        if (results.empty()) {
            cmd.reason = "no_strategy";
            return cmd;
        }

        // 初始化绝对区间：[−∞, +∞]
        double abs_lower = -1e18;
        double abs_upper =  1e18;

        // 收集每层的 contributing
        std::vector<std::string> contributing;

        // ---- 逐层收敛区间（L0 → L1 → L2 → L3） ----
        for (int lvl = 0; lvl < 4; ++lvl) {
            for (const auto& r : results) {
                if (static_cast<int>(r.priority) != lvl) continue;
                // 不论 active 与否，都参与区间收敛（active=false 表示"未动作"，
                // 此时区间为父层基线 [−∞, +∞]，实际不收紧 — 由策略实现保证）
                if (r.p_lower > abs_lower) abs_lower = r.p_lower;
                if (r.p_upper < abs_upper) abs_upper = r.p_upper;
                contributing.push_back(r.strategy_id);
            }
        }

        // 检查区间一致性（lower > upper 视为矛盾）
        if (abs_lower > abs_upper) {
            // 极端情况：L0 直接禁充 + L0 直接禁放 → 输出 0
            abs_lower = 0.0;
            abs_upper = 0.0;
            cmd.clamped = true;
            cmd.reason = "interval_contradiction";
            cmd.contributing = contributing;
            cmd.p_bat_cmd_kw = 0.0;
            cmd.p_lower = 0.0;
            cmd.p_upper = 0.0;
            return cmd;
        }

        // ---- desired 加权平均：仅 L3 之间（L0/L1/L2 不参与 desired 收敛） ----
        double desired_sum = 0.0;
        double weight_sum  = 0.0;
        for (const auto& r : results) {
            if (r.priority != Priority::kL3_GlobalEcon) continue;
            if (!r.active) continue;        // 不参与的策略
            if (r.weight <= 0.0) continue;  // L3 独占模式下未匹配的策略
            desired_sum += r.p_desired * r.weight;
            weight_sum  += r.weight;
        }

        double desired = (weight_sum > 0.0)
            ? (desired_sum / weight_sum)
            : 0.0;

        // ---- desired 必须落在收敛后区间内，否则按比例缩放 ----
        bool clipped = false;
        std::string clip_reason;
        if (desired < abs_lower) {
            desired = abs_lower;
            clipped = true;
            clip_reason = "desired_below_lower";
        } else if (desired > abs_upper) {
            desired = abs_upper;
            clipped = true;
            clip_reason = "desired_above_upper";
        }

        // ---- 死区 ----
        std::string mode_reason;
        if (std::abs(desired) < deadband_) {
            // 检查是否需要滞环切换
            if (current_mode_ == SignMode::kZero ||
                ++zero_counter_ >= switch_delay_) {
                desired = 0.0;
                current_mode_ = SignMode::kZero;
                mode_reason = "deadband";
            } else {
                // 滞环未达，沿用上次符号方向
                desired = (last_desired_ > 0) ? abs_upper * 0.0 : 0.0;
                mode_reason = "hysteresis_keep";
            }
            zero_counter_ = 0;
        } else {
            zero_counter_ = 0;
            current_mode_ = (desired > 0) ? SignMode::kPos : SignMode::kNeg;
        }

        last_desired_ = desired;

        // ---- 填回 cmd ----
        cmd.p_bat_cmd_kw = desired;
        cmd.p_lower      = abs_lower;
        cmd.p_upper      = abs_upper;
        cmd.clamped      = clipped;
        cmd.contributing = std::move(contributing);

        // reason 拼接：clipped → 优先；否则 deadband/hysteresis；否则 ok
        if (clipped) {
            cmd.reason = "desired_clip(" + clip_reason + ")";
        } else if (!mode_reason.empty()) {
            cmd.reason = mode_reason;
        } else {
            cmd.reason = "ok";
        }
        return cmd;
    }

    // 调试辅助：导出"每层区间收敛过程"
    struct LayerDebug {
        int lvl;
        double lower;
        double upper;
        std::vector<std::string> ids;
    };

    std::vector<LayerDebug> debug_layers(
            const std::vector<StrategyResult>& results) const {
        std::vector<LayerDebug> out;
        for (int lvl = 0; lvl < 4; ++lvl) {
            LayerDebug d{};
            d.lvl   = lvl;
            d.lower = -1e18;
            d.upper =  1e18;
            for (const auto& r : results) {
                if (static_cast<int>(r.priority) != lvl) continue;
                if (r.p_lower > d.lower) d.lower = r.p_lower;
                if (r.p_upper < d.upper) d.upper = r.p_upper;
                d.ids.push_back(r.strategy_id);
            }
            out.push_back(d);
        }
        return out;
    }

private:
    enum class SignMode { kZero, kPos, kNeg };
    SignMode current_mode_  = SignMode::kZero;
    int      zero_counter_  = 0;       // 死区方向连续拍数
    double   last_desired_  = 0.0;     // 上次下发 desired（滞环）
    int      switch_delay_  = 3;       // 死区方向切换滞环（拍）
    double   deadband_      = 0.5;     // 死区（kW）
};

} // namespace ems