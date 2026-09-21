// =====================================================================
// 05/ — 周期 5：统一安全约束引擎 SafetyEngine
//
// 依据：工商业储能EMS调控策略设计方案.md §7 周期 5
//   「搭建 Safety & Constraint Engine，统一处理 BMS禁止充放、BMS降功率、
//     SOC上下限、PCS功率限制、变压器容量限制、电池温度、功率变化率、
//     电网并网约束」
//
// 与既有模块的关系：
//   - 02/ 的 SafetyConstraintManager 是**设备侧**的三策略处理器（BMS禁止充放 /
//     BMS降功率 / 变压器过载），只输出 {charge_blocked, discharge_blocked,
//     charge_limit, discharge_limit} 四个量。
//   - 本引擎是**系统侧**的统一收敛层：把 8 类约束全部折成同一个
//     (p_lower, p_upper) 区间语义，并逐条留下 trace，便于定位"到底是谁在限"。
//   - 输出可直接作为 L0/L1 层的 StrategyResult 喂给 04/ 的 StrategyArbiter，
//     与 L2/L3 经济策略在同一条链路上收敛（安全层兜底）。
//
// 功率符号约定（全系统强制，源自接口规范 §2.1）：
//   P_bat  : 放电为正(+)，充电为负(-)
//   P_grid : 进口为正(+)，馈网为负(-)
//   功率平衡: P_grid = P_load - P_pv - P_bat
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "data_models.h"
#include "strategy_base.h"
#include "ext_setpoints.h"   // A3.2：外部设定（调度遥调）的快照类型 + 收窄函数

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace ems {

// =====================================================================
// 电网质量（并网合规用；不在 RealtimeSnapshot 里，单独传入）
// =====================================================================
struct GridQuality {
    double freq_hz = 50.0;   // 电网频率
    double volt_pu = 1.00;   // 并网点电压标幺值
    bool   valid   = true;   // 量测有效
};

// =====================================================================
// 约束参数
// =====================================================================
struct SafetyParams {
    // ---- SOC 上下限（L0）----
    double soc_min        = 0.10;   // 绝对下限，触及禁放
    double soc_max        = 0.90;   // 绝对上限，触及禁充
    double soc_warn_low   = 0.15;   // 预警下限，进入后降功率
    double soc_warn_high  = 0.85;   // 预警上限，进入后降功率
    double soc_hysteresis = 0.02;   // 预警解除回差
    double soc_warn_derate = 0.50;  // 预警区功率折减系数

    // ---- 电池温度（L0）----
    double temp_warn_c      = 45.0; // 预警温度，超过后降功率
    double temp_fault_c     = 55.0; // 故障温度，超过后禁充放
    double temp_hysteresis_c = 2.0; // 解除回差
    double temp_warn_derate  = 0.50;

    // ---- PCS 功率限制（L1）----
    double pcs_derate_ratio = 1.00; // 额外折减系数（检修/老化场景）

    // ---- 变压器容量限制（L1）----
    double tr_overload_th   = 0.95; // 轻度过载阈值（负载率）
    double tr_extreme_th    = 1.10; // 极端过载阈值（强制禁放）
    double tr_hysteresis    = 0.02; // 过载解除回差
    double tr_load_pv_share = 0.10; // 变压器负载估算中负荷的折算系数（沿用 04/ 简化口径）

    // ---- 功率变化率（L1）----
    double ramp_kw_per_s = 200.0;   // 每秒允许的功率变化幅度

    // ---- 电网并网约束（L1）----
    double grid_p_min_kw = 0.0;     // 关口功率下限（0 = 不允许倒送；<0 允许倒送）
    double grid_p_max_kw = 1e9;     // 关口功率上限（进线/变压器容量）
    // 并网约束的**量测低通滤波**系数（1.0 = 不滤波）。
    // 为什么需要：并网边界的基准量 base = P_load − P_pv 直接来自电表，带量测噪声时
    //   边界 p_upper 会随噪声抖动，而安全兜底 apply() 必须把指令压进该边界 ——
    //   结果储能被迫跟随一个**抖动的安全边界**，在并网点附近形成极限环。
    //   这是下游任何死区/滞环都压不住的（安全层优先级最高），必须从量测侧根治。
    //   0.05 ≈ 2 s 时间常数 @ dt=0.1s（一阶低通）。
    double grid_filter_alpha = 1.0;
    // 并网上界的"下一拍前瞻"幅度上限（kW）。**0 = 关闭（历史行为）**。
    //
    // 为什么需要前瞻：现场预报是 15 min 阶梯曲线，base = P_load − P_pv 会在
    //   阶梯边界**一拍内突降**十几 kW。若只用当拍 base 作并网上界，上一拍按旧
    //   （更松）边界下发的指令此刻仍在 PCS 死区/惯性里执行 → 关口瞬时倒送。
    //   开启后安全层取 base_safe = min(base_now, base_next)，把穿越消除在源头。
    //
    // 为什么必须设上限：前瞻只在"预报确为环境预测"时才有意义。若预报与实测
    //   严重不符（例如测试台刻意用"名义曲线"驱动优化层，而环境是另一条曲线），
    //   无条件相信预报会把并网上界收紧到一个错误的量级，反而破坏需量约束。
    //   因此只在**降幅不超过本值**时才采纳 —— 超出即判定该预报在此分辨率下
    //   不可信，退回历史行为（当拍量测）。
    double grid_lookahead_max_drop_kw = 0.0;
    double grid_freq_min_hz = 49.5;
    double grid_freq_max_hz = 50.5;
    double grid_volt_min_pu = 0.90;
    double grid_volt_max_pu = 1.10;

    // ---- 引擎行为 ----
    bool strict_l0 = true;          // L0 冲突时强制 [0, 0]
    bool enable_ramp = true;        // 是否启用变化率约束
};

// =====================================================================
// 单条约束的评估结果
// =====================================================================
struct ConstraintResult {
    std::string name;             // 约束名（稳定标识，日志/测试用）
    Priority    level = Priority::kL1_SafeOp;
    bool        active = false;   // 本拍是否真正收紧了区间
    // 是否属于"设备降额"信号（用于 EMS 状态机判定 DERATED）。
    // 常态运行边界（变化率、并网静态限值）不构成降额 —— 见文件头说明。
    bool        counts_as_derate = true;
    // 是否参与"区间求交"。变化率限制走 slew limiter 后置处理，
    // 不作为区间约束参与求交 —— 否则会与其他约束产生**假区间矛盾**：
    //   例：光伏大发时并网要求 P_bat ≤ −200（必须满充），而变化率给出
    //       P_bat ∈ [−20, +20]，求交即得下界 20 > 上界 −200 → 误判矛盾并锁死输出。
    bool        binds_interval = true;
    double      p_lower = -1e18;  // 该约束给出的下界
    double      p_upper =  1e18;  // 该约束给出的上界
    double      margin_kw = 0.0;  // 距边界余量（正 = 还有余量）
    std::string reason;           // 触发原因

    // 折算成仲裁器可消费的策略输出
    StrategyResult to_strategy_result() const {
        StrategyResult r;
        r.strategy_id = "SAFE_" + name;
        r.priority    = level;
        r.active      = active;
        r.weight      = 1.0;
        // **不参与区间求交的约束（变化率）必须以"不收紧"的区间导出。**
        // 求交规则在 evaluate() 内按 binds_interval 过滤，但 as_results() 这条
        // 出口是**另一条路径**，必须保持一致，否则会绕过该规则：
        //   变化率约束的区间是"相对上一拍指令的邻域"（例 [−20, +20]），
        //   若把它与硬安全区间（例 并网要求 [42, 250]）求交，即得 42 > 20，
        //   仲裁器误判为区间矛盾 → 塌缩为 [0,0] → 指令被迫为 0，
        //   反而突破并网/需量边界（周期 9 场景 S3/S4 实测逃逸 3724/1016 拍）。
        r.p_lower     = binds_interval ? p_lower : -1e18;
        r.p_upper     = binds_interval ? p_upper :  1e18;
        r.p_desired   = 0.0;   // L0/L1 不参与 desired 收敛（接口规范 §2.5）
        r.reason      = reason;
        return r;
    }
};

// =====================================================================
// 安全裁决（本拍的总输出）
// =====================================================================
struct SafetyVerdict {
    double p_lower = -1e18;        // 收敛后的最终下界
    double p_upper =  1e18;        // 收敛后的最终上界
    bool   contradiction = false;  // 区间矛盾（下界 > 上界）
    bool   l0_active = false;      // 有 L0 硬安全约束动作（含 SOC/温度预警）
    bool   l0_hard   = false;      // 有 L0 **硬禁闭**动作（禁充/禁放/通信丢失）
    bool   l1_active = false;      // 有 L1 约束动作（含并网静态限值、变化率）
    bool   derated   = false;      // 有降额信号 → 应进入 DERATED
    bool   emergency = false;      // 需要紧急停机
    bool   ramp_active = false;    // 本拍变化率限速生效
    double ramp_limit_kw = 0.0;    // 本拍允许的最大变化幅度（kW）
    bool   ext_active = false;     // 本拍外部设定（调度遥调）实际收紧了区间（含停机/闭锁粘住）
    std::vector<ConstraintResult> items;   // 全部 9 条约束的评估结果
    std::vector<std::string> binding;      // 真正收紧区间的约束名
    std::string reason;                    // 汇总原因

    // 折算成 L0/L1 层策略输出（喂给 04/ StrategyArbiter）
    std::vector<StrategyResult> as_results() const {
        std::vector<StrategyResult> out;
        out.reserve(items.size());
        for (const auto& it : items) out.push_back(it.to_strategy_result());
        return out;
    }

    // 调试：把收敛过程打成一行文本
    std::string trace() const {
        std::string s;
        for (const auto& b : binding) {
            if (!s.empty()) s += "+";
            s += b;
        }
        return s.empty() ? std::string("none") : s;
    }
};

// =====================================================================
// SafetyEngine
// =====================================================================
class SafetyEngine {
public:
    SafetyEngine() = default;
    explicit SafetyEngine(const SafetyParams& p) : p_(p) {}

    void set_params(const SafetyParams& p) {
        p_ = p;
        grid_base_init_ = false;   // 参数变更后重新预置滤波器
    }
    const SafetyParams& params() const { return p_; }

    // 约束名常量（测试与日志引用，避免拼写漂移）
    static constexpr const char* kBmsForbid     = "bms_forbid";
    static constexpr const char* kBmsDerate     = "bms_derate";
    static constexpr const char* kSocLimit      = "soc_limit";
    static constexpr const char* kPcsLimit      = "pcs_limit";
    static constexpr const char* kTransformer   = "transformer_limit";
    static constexpr const char* kTemp          = "battery_temp";
    static constexpr const char* kRamp          = "ramp_rate";
    static constexpr const char* kGridConnect   = "grid_connect";
    static constexpr const char* kGridQuality   = "grid_quality";
    static constexpr const char* kExtSetpoint   = "ext_setpoint";

    // -----------------------------------------------------------------
    // 主入口：评估全部约束并收敛区间
    //
    // rt           当前实时快照（评估期间冻结，接口规范 §5 强约束）
    // dev          设备运行时限制
    // grid         电网质量（频率/电压）
    // p_last_cmd   上一拍实际下发的 P_bat（用于变化率约束）
    // dt_s         控制周期
    // ext          外部设定（调度遥调）。默认空 = 无外部设定（仿真口径，行为与历史逐位一致）
    // -----------------------------------------------------------------
    SafetyVerdict evaluate(const RealtimeSnapshot& rt,
                           const DeviceLimits& dev,
                           const GridQuality& grid,
                           double p_last_cmd_kw,
                           double dt_s,
                           const ExtSetpoints& ext = ExtSetpoints{}) {
        SafetyVerdict v;
        v.items.reserve(9);
        last_p_cmd_ = p_last_cmd_kw;

        // 逐条评估（顺序 = 优先级顺序 L0 → L1）
        v.items.push_back(check_bms_forbid(rt, dev));
        v.items.push_back(check_soc(rt, dev));
        v.items.push_back(check_temperature(rt, dev));
        v.items.push_back(check_bms_derate(rt, dev));
        v.items.push_back(check_pcs(rt, dev));
        v.items.push_back(check_transformer(rt, dev));
        v.items.push_back(check_grid_connect(rt, dev));
        v.items.push_back(check_grid_quality(grid));
        v.items.push_back(check_ramp(p_last_cmd_kw, dt_s));

        // ---- 逐层收敛（L0 → L1），与接口规范 §2.5 一致 ----
        // 只让 binds_interval 的约束参与求交（变化率走后置 slew limiter）
        for (int lvl = 0; lvl < 4; ++lvl) {
            for (const auto& it : v.items) {
                if (!it.binds_interval) continue;
                if (static_cast<int>(it.level) != lvl) continue;
                if (it.p_lower > v.p_lower) v.p_lower = it.p_lower;
                if (it.p_upper < v.p_upper) v.p_upper = it.p_upper;
            }
        }

        // ---- 记录"谁在限"：只统计**本拍动作且把自己的边界变成最终边界**的约束 ----
        const double eps = 1e-9;
        for (const auto& it : v.items) {
            if (!it.binds_interval || !it.active) continue;
            bool binds_lower = (it.p_lower > -1e17) && (std::fabs(it.p_lower - v.p_lower) < eps);
            bool binds_upper = (it.p_upper <  1e17) && (std::fabs(it.p_upper - v.p_upper) < eps);
            if (binds_lower || binds_upper) v.binding.push_back(it.name);
        }

        // ---- 第 10 条约束：外部设定（调度遥调），相对已收敛区间**只收紧、不放宽** ----
        // 放在 9 条本地约束收敛之后、矛盾检查之前：
        //   ① 它只能 min/max，收紧后可能制造区间矛盾（如调度限放 50 kW 而本地
        //      防逆流要求放电 ≥100 kW），矛盾必须由下面的矛盾兜底统一处理
        //      （矛盾 = DERATED，不是 EMERGENCY —— 本项目纪律）；
        //   ② 它**不**推进 items / l0 / derate / emergency 标志 —— 调度停机/限功率
        //      是"监督层命令"，不是"设备降额 / 硬安全动作"。trace 里用 binding 标记。
        {
            const double lo0 = v.p_lower, hi0 = v.p_upper;
            narrow_interval_by_ext(ext, &v.p_lower, &v.p_upper);
            if (v.p_lower > lo0 + 1e-12 || v.p_upper < hi0 - 1e-12) {
                v.ext_active = true;
                v.binding.push_back(kExtSetpoint);
            }
        }

        // ---- L0 动作 / 降额 / 紧急 ----
        // 注：L0 硬禁闭 与 降额 是两个正交维度（SOC/温度预警虽属 L0 层，但语义是降额）
        for (const auto& it : v.items) {
            if (!it.active) continue;
            if (it.level == Priority::kL0_Safety) {
                v.l0_active = true;
                if (!it.counts_as_derate) v.l0_hard = true;
            }
            if (it.level == Priority::kL1_SafeOp) v.l1_active = true;
            if (it.counts_as_derate)              v.derated   = true;
            if (it.name == kRamp) {
                v.ramp_active    = true;
                v.ramp_limit_kw  = it.margin_kw;
            }
        }
        // 温度故障、电网越限 → 紧急停机
        for (const auto& it : v.items) {
            if (it.name == kTemp && it.reason == "temp_fault") v.emergency = true;
            if (it.name == kGridQuality && it.reason == "grid_out_of_range") v.emergency = true;
        }

        // ---- 区间矛盾兜底 ----
        // 矛盾 ≠ 紧急：本拍无可行非零功率，最安全动作是输出 0（strict_l0 下区间收成 [0,0]）。
        // 语义上属于"系统受限运行"，故置 derated → 状态机进 DERATED（可自恢复），
        // 而不是 EMERGENCY（锁存，需人工复位）。典型场景：满充 + 光伏大发不许倒送。
        if (v.p_lower > v.p_upper) {
            v.contradiction = true;
            v.l0_active = true;
            v.l0_hard   = true;
            v.derated   = true;
            if (p_.strict_l0) {
                v.p_lower = 0.0;
                v.p_upper = 0.0;
            }
            v.reason = "interval_contradiction";
        } else if (v.emergency) {
            v.reason = "emergency";
        } else if (v.l0_hard) {
            v.reason = "l0_safety";
        } else if (v.derated) {
            v.reason = "derated";
        } else if (v.l1_active) {
            v.reason = "l1_limited";
        } else {
            v.reason = "normal";
        }

        last_verdict_ = v;
        return v;
    }

    // -----------------------------------------------------------------
    // 兜底限幅：把仲裁后的指令压进安全区间，并做变化率限速
    // 返回是否发生裁剪
    // -----------------------------------------------------------------
    bool apply(PowerCommand& cmd) const {
        bool clipped = false;

        // 1) 压回安全区间（L0/L1 区间求交的结果）
        if (cmd.p_bat_cmd_kw > last_verdict_.p_upper) {
            cmd.p_bat_cmd_kw = last_verdict_.p_upper;
            clipped = true;
            cmd.reason += "+safety_clip_upper";
        } else if (cmd.p_bat_cmd_kw < last_verdict_.p_lower) {
            cmd.p_bat_cmd_kw = last_verdict_.p_lower;
            clipped = true;
            cmd.reason += "+safety_clip_lower";
        }

        // 2) 变化率限速（slew limiter）：相对上一拍**实际下发**指令的邻域
        //    做成后置限速而不是区间约束，既保证不越安全边界，也不会与其他
        //    约束产生假矛盾（区间是凸的，从安全点朝安全目标单调移动必然安全）
        if (last_verdict_.ramp_active && last_verdict_.ramp_limit_kw > 0.0) {
            const double d  = last_verdict_.ramp_limit_kw;
            const double lo = last_p_cmd_ - d;
            const double hi = last_p_cmd_ + d;
            if (cmd.p_bat_cmd_kw > hi) {
                cmd.p_bat_cmd_kw = hi;
                clipped = true;
                cmd.reason += "+ramp_clip";
            } else if (cmd.p_bat_cmd_kw < lo) {
                cmd.p_bat_cmd_kw = lo;
                clipped = true;
                cmd.reason += "+ramp_clip";
            }
        }

        // 区间同步透出（设备端按 PermissionRange 限幅执行，接口规范 §5）
        cmd.p_lower = std::max(cmd.p_lower, last_verdict_.p_lower);
        cmd.p_upper = std::min(cmd.p_upper, last_verdict_.p_upper);

        // 2.5) 区间一致性兜底：仲裁区间 ∩ 安全区间 为空时，安全区间权威接管。
        //
        // 为什么会为空：仲裁器只认"逐层求交"，当 L2/L3 策略（或安全层自身导出
        // 的某个约束）给出与安全区间互斥的边界时，它会把区间塌缩成 [0,0]；
        // 而本函数随后又用安全区间把其中一侧"撑"回去 → 得到 p_lower > p_upper
        // 的空区间。空区间下**任何** p_cmd 都会逃逸（设备端按 PermissionRange
        // 限幅时行为也不确定），必须显式消解。
        // 消解原则与全系统一致：**安全边界是硬约束**，故以安全区间为准；
        // 仅当安全区间自身也矛盾（无可行功率）时才退回 [0,0]。
        if (cmd.p_lower > cmd.p_upper) {
            if (last_verdict_.p_lower <= last_verdict_.p_upper) {
                cmd.p_lower = last_verdict_.p_lower;
                cmd.p_upper = last_verdict_.p_upper;
                cmd.reason += "+interval_fallback_safety";
            } else {
                cmd.p_lower = 0.0;
                cmd.p_upper = 0.0;
                cmd.reason += "+interval_fallback_zero";
            }
            clipped = true;
        }

        // 3) 最终一致性夹：保证下发的 p_cmd 一定落在**声明的权限区间**内。
        //
        // 为什么必须放在最后：上面两步都可能把指令推出区间 ——
        //   ① slew limiter 是"相对上一拍指令的邻域"，当区间在**本拍刚收紧**时
        //      （典型：光伏突增，并网边界从 +200 收到 −50），上一拍指令已落在
        //      本拍区间之外，ramp 会把指令拉回那个"已失效的邻域"→ 逃出安全区间；
        //   ② 上游 OutputShaper 的滞环保持返回的是"上一拍整形输出"，同样可能逃逸。
        // 安全边界是**硬约束**，优先级高于平滑；且接口规范 §5 要求设备端按
        // PermissionRange 限幅执行 —— 若 p_cmd 越界，实际执行值会与 EMS 预期不符。
        if (cmd.p_bat_cmd_kw > cmd.p_upper) {
            cmd.p_bat_cmd_kw = cmd.p_upper;
            clipped = true;
            cmd.reason += "+reclip_upper";
        } else if (cmd.p_bat_cmd_kw < cmd.p_lower) {
            cmd.p_bat_cmd_kw = cmd.p_lower;
            clipped = true;
            cmd.reason += "+reclip_lower";
        }

        if (clipped) cmd.clamped = true;
        return clipped;
    }

    const SafetyVerdict& last_verdict() const { return last_verdict_; }
    void reset() { last_verdict_ = SafetyVerdict{}; }

private:
    // -----------------------------------------------------------------
    // 1. BMS 禁止充放（L0）
    // -----------------------------------------------------------------
    ConstraintResult check_bms_forbid(const RealtimeSnapshot& rt,
                                      const DeviceLimits& dev) {
        ConstraintResult c;
        c.name  = kBmsForbid;
        c.level = Priority::kL0_Safety;
        c.counts_as_derate = false;   // 硬禁闭（L0），不属降额

        // 通信丢失 → 最保守 [0, 0]（接口规范 §6：BMS 通信丢失立即收紧到 [0,0]）
        bool bms_alive = true;
        auto it = rt.meters_alive.find("BMS");
        if (it != rt.meters_alive.end()) bms_alive = it->second;

        if (!bms_alive) {
            c.active = true;
            c.p_lower = c.p_upper = 0.0;
            c.reason = "bms_comm_lost";
            c.margin_kw = 0.0;
            return c;
        }

        if (dev.bms_chg_forbidden && dev.bms_dis_forbidden) {
            c.active  = true;
            c.p_lower = c.p_upper = 0.0;
            c.reason  = "bms_forbid_both";
        } else if (dev.bms_chg_forbidden) {
            c.active  = true;
            c.p_lower = 0.0;
            c.reason  = "bms_forbid_charge";
        } else if (dev.bms_dis_forbidden) {
            c.active  = true;
            c.p_upper = 0.0;
            c.reason  = "bms_forbid_discharge";
        } else {
            c.reason = "bms_normal";
        }
        return c;
    }

    // -----------------------------------------------------------------
    // 2. BMS 请求降功率（L1）
    // -----------------------------------------------------------------
    ConstraintResult check_bms_derate(const RealtimeSnapshot& rt,
                                      const DeviceLimits& dev) {
        ConstraintResult c;
        c.name  = kBmsDerate;
        c.level = Priority::kL1_SafeOp;

        double p_chg = std::min(dev.bms_chg_limit_kw, dev.pcs_rated_chg_kw);
        double p_dis = std::min(dev.bms_dis_limit_kw, dev.pcs_rated_dis_kw);
        c.p_lower = -std::max(0.0, p_chg);
        c.p_upper =  std::max(0.0, p_dis);

        if (p_chg < dev.pcs_rated_chg_kw || p_dis < dev.pcs_rated_dis_kw) {
            c.active   = true;
            c.reason   = "bms_derating";
            c.margin_kw = std::min(p_chg, p_dis);
        } else {
            c.reason = "bms_normal";
            c.margin_kw = std::min(p_chg, p_dis);
        }
        (void)rt;
        return c;
    }

    // -----------------------------------------------------------------
    // 3. SOC 上下限（L0 硬边界 + 预警区降功率）
    // -----------------------------------------------------------------
    ConstraintResult check_soc(const RealtimeSnapshot& rt,
                               const DeviceLimits& dev) {
        ConstraintResult c;
        c.name  = kSocLimit;
        c.level = Priority::kL0_Safety;

        const double soc = rt.soc;
        const double pcs_dis = std::max(0.0, dev.pcs_rated_dis_kw);
        const double pcs_chg = std::max(0.0, dev.pcs_rated_chg_kw);

        // --- 绝对上下限（带滞环释放）---
        if (soc <= p_.soc_min) soc_low_latched_ = true;
        else if (soc > p_.soc_min + p_.soc_hysteresis) soc_low_latched_ = false;

        if (soc >= p_.soc_max) soc_high_latched_ = true;
        else if (soc < p_.soc_max - p_.soc_hysteresis) soc_high_latched_ = false;

        if (soc_low_latched_ && soc_high_latched_) {
            c.active  = true;
            c.counts_as_derate = false;
            c.p_lower = c.p_upper = 0.0;
            c.reason  = "soc_both_limits";
            return c;
        }
        if (soc_low_latched_) {
            // 触及下限：禁放（放电方向 P_bat > 0 关闭）
            c.active  = true;
            c.counts_as_derate = false;
            c.p_upper = 0.0;
            c.p_lower = -pcs_chg;
            c.reason  = "soc_min_reached";
            return c;
        }
        if (soc_high_latched_) {
            // 触及上限：禁充（充电方向 P_bat < 0 关闭）
            c.active  = true;
            c.counts_as_derate = false;
            c.p_lower = 0.0;
            c.p_upper = pcs_dis;
            c.reason  = "soc_max_reached";
            return c;
        }

        // --- 预警区：折减功率 ---
        bool warn = false;
        if (soc < p_.soc_warn_low) {
            soc_warn_low_latched_ = true;
            warn = true;
        } else if (soc > p_.soc_warn_low + p_.soc_hysteresis) {
            soc_warn_low_latched_ = false;
        }
        if (soc > p_.soc_warn_high) {
            soc_warn_high_latched_ = true;
            warn = true;
        } else if (soc < p_.soc_warn_high - p_.soc_hysteresis) {
            soc_warn_high_latched_ = false;
        }
        if (soc_warn_low_latched_ || soc_warn_high_latched_) warn = true;

        if (warn) {
            c.active   = true;
            c.p_lower  = -pcs_chg * p_.soc_warn_derate;
            c.p_upper  =  pcs_dis * p_.soc_warn_derate;
            c.reason   = soc_warn_low_latched_ ? "soc_warn_low" : "soc_warn_high";
            c.margin_kw = std::min(std::fabs(soc - p_.soc_min),
                                   std::fabs(p_.soc_max - soc));
            return c;
        }

        c.p_lower = -pcs_chg;
        c.p_upper =  pcs_dis;
        c.reason  = "soc_normal";
        c.margin_kw = std::min(std::fabs(soc - p_.soc_min), std::fabs(p_.soc_max - soc));
        return c;
    }

    // -----------------------------------------------------------------
    // 4. 电池温度（L0）
    // -----------------------------------------------------------------
    ConstraintResult check_temperature(const RealtimeSnapshot& rt,
                                       const DeviceLimits& dev) {
        ConstraintResult c;
        c.name  = kTemp;
        c.level = Priority::kL0_Safety;

        const double T = rt.temperature_c;
        const double pcs_dis = std::max(0.0, dev.pcs_rated_dis_kw);
        const double pcs_chg = std::max(0.0, dev.pcs_rated_chg_kw);

        if (T >= p_.temp_fault_c) temp_fault_latched_ = true;
        else if (T < p_.temp_fault_c - p_.temp_hysteresis_c) temp_fault_latched_ = false;

        if (temp_fault_latched_) {
            c.active  = true;
            c.counts_as_derate = false;
            c.p_lower = c.p_upper = 0.0;
            c.reason  = "temp_fault";
            return c;
        }

        if (T >= p_.temp_warn_c) temp_warn_latched_ = true;
        else if (T < p_.temp_warn_c - p_.temp_hysteresis_c) temp_warn_latched_ = false;

        if (temp_warn_latched_) {
            c.active   = true;
            c.p_lower  = -pcs_chg * p_.temp_warn_derate;
            c.p_upper  =  pcs_dis * p_.temp_warn_derate;
            c.reason   = "temp_high_derate";
            c.margin_kw = p_.temp_fault_c - T;
            return c;
        }

        c.p_lower = -pcs_chg;
        c.p_upper =  pcs_dis;
        c.reason  = "temp_normal";
        c.margin_kw = p_.temp_fault_c - T;
        return c;
    }

    // -----------------------------------------------------------------
    // 5. PCS 功率限制（L1）
    // -----------------------------------------------------------------
    ConstraintResult check_pcs(const RealtimeSnapshot& rt,
                               const DeviceLimits& dev) {
        ConstraintResult c;
        c.name  = kPcsLimit;
        c.level = Priority::kL1_SafeOp;

        double p_chg = std::max(0.0, dev.pcs_rated_chg_kw) * p_.pcs_derate_ratio;
        double p_dis = std::max(0.0, dev.pcs_rated_dis_kw) * p_.pcs_derate_ratio;
        c.p_lower = -p_chg;
        c.p_upper =  p_dis;
        c.margin_kw = std::min(p_chg, p_dis);

        if (p_.pcs_derate_ratio < 1.0) {
            c.active = true;
            c.reason = "pcs_derated";
        } else {
            c.reason = "pcs_normal";
        }
        (void)rt;
        return c;
    }

    // -----------------------------------------------------------------
    // 6. 变压器容量限制（L1）
    //
    // 两道防线：轻度过载 → 按可行区间限功率；极端过载 → 同一套区间逻辑，
    //   但标记为 extreme（供上层决定是否升级告警）。**注意**：不再有
    //   "极端过载一律禁放" —— 变压器负载看的是关口功率的**绝对值**，进口方向
    //   过载时放电恰恰是缓解手段，禁放会南辕北辙（详见下方方向说明）。
    // -----------------------------------------------------------------
    ConstraintResult check_transformer(const RealtimeSnapshot& rt,
                                       const DeviceLimits& dev) {
        ConstraintResult c;
        c.name  = kTransformer;
        c.level = Priority::kL1_SafeOp;

        const double pcs_dis = std::max(0.0, dev.pcs_rated_dis_kw);
        const double pcs_chg = std::max(0.0, dev.pcs_rated_chg_kw);
        const double cap     = dev.transformer_capacity_kw;

        c.p_lower = -pcs_chg;
        c.p_upper =  pcs_dis;

        if (cap <= 0.0) {
            c.reason = "tr_no_capacity";
            return c;
        }

        // ---- 负载估算：取「量测值」与「在途指令预测值」中的较劣者 ----
        // 口径与 04/ 一致：tr_load = |P_grid| + 0.1·P_load，且 P_grid = base − P_bat。
        //
        // **为什么要预测（原实现的缺陷）**：
        //   若只用**电表量测**判定，约束只能在负载率**已经**越过阈值之后才动作。
        //   但 PCS 存在传输延时 + 一阶惯性（现场必然如此），此刻被控对象已
        //   "带着动量"，必然再冲过一段 —— 阈值留多少余量都不够用
        //   （周期 9 场景 S2 实测：阈值 0.95 时冲高到 1.014；即便收到 0.90
        //    仍在一次指令翻转中冲高到 286 kW 而越限）。
        //   改用「上一拍指令落地后」的预测负载参与判定，约束就能在**指令下发
        //   之前**拦住会导致过载的指令 —— 这才是变压器保护应有的前馈特性。
        // **为什么取 max(量测, 预测) 而不是只取预测**：
        //   两者分别代表"现在的负载"与"指令落地后的负载"，任一越限都必须收敛，
        //   取较劣者最安全；且被控对象稳态时两者相等，不影响既有行为。
        const double base0 = rt.p_load_kw - rt.p_pv_kw;
        const double tr_load_meas = std::fabs(rt.p_grid_kw)
                                  + rt.p_load_kw * p_.tr_load_pv_share;
        const double tr_load_pred = std::fabs(base0 - last_p_cmd_)
                                  + rt.p_load_kw * p_.tr_load_pv_share;
        const double tr_load = std::max(tr_load_meas, tr_load_pred);
        const double ratio   = tr_load / cap;
        c.margin_kw = (p_.tr_overload_th - ratio) * cap;

        // 带滞环的过载锁存
        if (ratio > p_.tr_overload_th) tr_overload_latched_ = true;
        else if (ratio < p_.tr_overload_th - p_.tr_hysteresis) tr_overload_latched_ = false;

        const bool extreme = (ratio > p_.tr_extreme_th);
        if (!tr_overload_latched_ && !extreme) {
            c.reason = "tr_normal";
            return c;
        }

        // -----------------------------------------------------------------
        // 过载：推导 P_bat 的可行区间
        //
        // 设 base = P_load − P_pv（储能不动作时的关口功率），则 P_grid = base − P_bat。
        // 要求  |P_grid| + 0.1·P_load ≤ 阈值·cap
        //   ⟺  |base − P_bat| ≤ half,  half = 阈值·cap − 0.1·P_load
        //   ⟺  P_bat ∈ [base − half, base + half]
        //
        // **方向说明（原实现的缺陷）**：
        //   原实现把余量一律当作 p_upper（= 禁止放电）。但变压器负载看的是
        //   **关口功率的绝对值**，而放电（P_bat > 0）会降低 P_grid —— 恰恰是
        //   缓解过载的手段。在"进口方向过载"（最常见）下，禁放会让储能无法削峰，
        //   负载率持续越限（周期 9 场景 S2 实测越限 3577 拍）。
        //   正确做法是把约束表达为上述**区间**：既限制充电（抬高 p_lower，
        //   防止加剧过载），也限制过度放电（压低 p_upper，防止反向馈网同样抬高负载）。
        // -----------------------------------------------------------------
        const double base = rt.p_load_kw - rt.p_pv_kw;
        const double half = p_.tr_overload_th * cap - rt.p_load_kw * p_.tr_load_pv_share;
        c.active = true;

        if (half <= 0.0) {
            // 变压器容量已被固定负荷吃满（仅 0.1·P_load 折算就超阈值）→
            // 储能无论怎么动都无法把负载压回阈值内。此时最安全的是"不加剧"：
            // 禁止充电（p_lower ≥ 0），允许放电（有助于缓解）。
            c.p_lower = std::max(c.p_lower, 0.0);
            c.reason  = "tr_saturated";
            return c;
        }

        // ---- 可行带 [lo, hi] 与设备区间求交；无交时**投影到最近端点** ----
        //
        // 为什么不能直接 `max/min` 了事：若可行带与设备区间**完全错位**
        //   （变压器已被压到 132%，只有放电 ≥370 kW 才能压回阈值，而 PCS 上限
        //    只有 250 kW），直接求交会得到 p_lower(370) > p_upper(250) 的**空区间**，
        //   一路传导成"区间矛盾 → 输出 0" —— 而 0 恰恰是最差的动作（既不缓解
        //   过载，也不比现在好）。正确做法是**尽力而为**：顶到设备能力的边界上，
        //   把负载率压到最低。这也是现场保护装置的行为（饱和输出，而非放弃）。
        const double lo = base - half;
        const double hi = base + half;        if (lo > c.p_upper) {
            // 需要更多放电才能缓解，但设备放不了这么多 → 顶到放电上限
            c.p_lower = c.p_upper;
            c.reason  = extreme ? "tr_extreme_sat_discharge" : "tr_sat_discharge";
        } else if (hi < c.p_lower) {
            // 需要更多充电才能缓解（馈网方向过载）→ 顶到充电上限
            c.p_upper = c.p_lower;
            c.reason  = extreme ? "tr_extreme_sat_charge" : "tr_sat_charge";
        } else {
            c.p_lower = std::max(c.p_lower, lo);
            c.p_upper = std::min(c.p_upper, hi);
            c.reason  = extreme ? "tr_extreme_overload" : "tr_overload";
        }
        return c;
    }

    // -----------------------------------------------------------------
    // 7. 电网并网约束（L1）：不允许倒送 / 不允许超进线容量
    //    P_grid = P_load - P_pv - P_bat
    //    P_grid >= g_min  →  P_bat <= P_load - P_pv - g_min
    //    P_grid <= g_max  →  P_bat >= P_load - P_pv - g_max
    // -----------------------------------------------------------------
    ConstraintResult check_grid_connect(const RealtimeSnapshot& rt,
                                        const DeviceLimits& dev) {
        ConstraintResult c;
        c.name  = kGridConnect;
        c.level = Priority::kL1_SafeOp;
        c.counts_as_derate = false;   // 并网静态限值属常态边界，不构成设备降额

        const double pcs_dis = std::max(0.0, dev.pcs_rated_dis_kw);
        const double pcs_chg = std::max(0.0, dev.pcs_rated_chg_kw);
        c.p_lower = -pcs_chg;
        c.p_upper =  pcs_dis;

        // 无储能时的关口功率（基准量）。带量测低通滤波时用滤波值，
        // 避免安全边界随电表噪声抖动 → 指令被迫跟随抖动边界（边界极限环）。
        const double base_raw = rt.p_load_kw - rt.p_pv_kw;
        if (!grid_base_init_) { grid_base_f_ = base_raw; grid_base_init_ = true; }
        else if (p_.grid_filter_alpha < 1.0) {
            const double a = std::max(0.0, p_.grid_filter_alpha);
            grid_base_f_ += a * (base_raw - grid_base_f_);
        } else {
            grid_base_f_ = base_raw;
        }
        const double base = grid_base_f_;
        // 前瞻：预报为 15 min 阶梯时，base 会在阶梯边界一拍内突降，而上一拍按
        // 旧（更松）上界下发的指令仍在 PCS 里执行 → 关口瞬时倒送。取当拍与
        // 下一拍的**较小** base（更紧的上界）即可消除该穿越。
        // 但只在降幅可信（≤ 上限）时采纳，否则退回历史行为（当拍量测）——
        // 理由见 SafetyParams::grid_lookahead_max_drop_kw 注释。
        double base_safe = base;
        if (rt.has_lookahead && p_.grid_lookahead_max_drop_kw > 0.0) {
            const double base_next = rt.p_load_next_kw - rt.p_pv_next_kw;
            const double drop = base - base_next;
            if (drop > 0.0 && drop <= p_.grid_lookahead_max_drop_kw)
                base_safe = base_next;
        }

        // 不允许倒送（或限制最大倒送）
        if (p_.grid_p_min_kw > -1e8) {
            double upper_by_grid = base_safe - p_.grid_p_min_kw;
            if (upper_by_grid < c.p_upper) {
                c.p_upper = upper_by_grid;
                c.active  = true;
                c.reason  = "grid_no_reverse";
            }
        }
        // 不允许超进线/变压器容量
        if (p_.grid_p_max_kw < 1e8) {
            double lower_by_grid = base - p_.grid_p_max_kw;
            if (lower_by_grid > c.p_lower) {
                c.p_lower = lower_by_grid;
                c.active  = true;
                c.reason  = c.active ? "grid_both_limits" : "grid_over_capacity";
            }
        }
        if (!c.active) c.reason = "grid_normal";
        c.margin_kw = rt.p_grid_kw - p_.grid_p_min_kw;
        return c;
    }

    // -----------------------------------------------------------------
    // 8. 并网合规（L1）：频率 / 电压越限
    // -----------------------------------------------------------------
    ConstraintResult check_grid_quality(const GridQuality& g) {
        ConstraintResult c;
        c.name  = kGridQuality;
        c.level = Priority::kL1_SafeOp;
        c.counts_as_derate = false;   // 并网合规越限 → 直接紧急停机，不按"降额"处理

        if (!g.valid) {
            c.active = true;
            c.p_lower = c.p_upper = 0.0;
            c.reason = "grid_data_invalid";
            return c;
        }
        const bool freq_bad = (g.freq_hz < p_.grid_freq_min_hz) ||
                              (g.freq_hz > p_.grid_freq_max_hz);
        const bool volt_bad = (g.volt_pu < p_.grid_volt_min_pu) ||
                              (g.volt_pu > p_.grid_volt_max_pu);
        if (freq_bad || volt_bad) {
            c.active = true;
            c.p_lower = c.p_upper = 0.0;
            c.reason = "grid_out_of_range";
            c.margin_kw = 0.0;
            return c;
        }
        c.reason = "grid_quality_normal";
        c.margin_kw = std::min(g.freq_hz - p_.grid_freq_min_hz,
                               p_.grid_freq_max_hz - g.freq_hz);
        return c;
    }

    // -----------------------------------------------------------------
    // 9. 功率变化率（L1）：相对上一拍指令的邻域
    // -----------------------------------------------------------------
    ConstraintResult check_ramp(double p_last_cmd_kw, double dt_s) {
        ConstraintResult c;
        c.name  = kRamp;
        c.level = Priority::kL1_SafeOp;
        c.counts_as_derate = false;   // 变化率限制属常态运行约束，不构成设备降额
        c.binds_interval   = false;   // 走 apply() 的 slew limiter，不参与区间求交

        if (!p_.enable_ramp || dt_s <= 0.0) {
            c.reason = "ramp_disabled";
            return c;
        }
        const double dmax = p_.ramp_kw_per_s * dt_s;
        c.p_lower = p_last_cmd_kw - dmax;
        c.p_upper = p_last_cmd_kw + dmax;
        // 只有确实收窄时才标 active（−∞/+∞ 的绝对边界不算收紧）
        if (std::fabs(p_last_cmd_kw) > 1e-9) {
            c.active = true;
            c.reason = "ramp_limited";
        } else {
            c.reason = "ramp_normal";
        }
        c.margin_kw = dmax;
        return c;
    }

    // ---- 内部状态（滞环锁存，仅用于约束自身去抖，不参与策略决策）----
    SafetyParams p_{};
    SafetyVerdict last_verdict_{};
    double last_p_cmd_ = 0.0;     // 上一拍实际下发指令（变化率限速基准）
    bool soc_low_latched_ = false;
    bool soc_high_latched_ = false;
    bool soc_warn_low_latched_ = false;
    bool soc_warn_high_latched_ = false;
    bool temp_warn_latched_ = false;
    bool temp_fault_latched_ = false;
    bool tr_overload_latched_ = false;
    // 并网约束基准量的低通滤波状态（见 SafetyParams::grid_filter_alpha）
    double grid_base_f_    = 0.0;
    bool   grid_base_init_ = false;
};

} // namespace ems
