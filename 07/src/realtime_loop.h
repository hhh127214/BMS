// =====================================================================
// 07/ — 周期 7：实时控制闭环 RealtimeClosedLoop / EmsRuntime
//
// 依据：工商业储能EMS调控策略设计方案.md §7 周期 7
//   「重点联调需量管理、防逆流、光伏平抑三大实时策略，打通完整闭环链路：
//     电表实时数据 → EMS算法计算 → 策略仲裁 → 安全约束 → PCS执行 →
//     实际功率反馈 → EMS迭代修正」
//   周期目标：「实现毫秒级实时控制闭环，无延迟、无振荡」
//
// 闭环时序（每 100 ms 一拍）：
//
//   ① 采集层   io_->read_snapshot()      ← 电表实时数据（含噪声/通信状态）
//   ② 故障判定 detect_faults()           ← BMS/PCS/电表通信、数据有效性
//   ③ 安全预判 safety.evaluate()          ← 供状态机判定 DERATED / EMERGENCY
//   ④ 状态机   fsm.update()              ← 状态流转 + 输出门控
//   ⑤ 协同层   coord.update()            ← 优化层计划 + 实时层纠偏（周期 8）
//   ⑥ 策略层   mgr.tick()                ← L2/L3 策略并行评估
//   ⑦ 仲裁层   arbiter.arbitrate()        ← L0→L3 区间收敛 + desired 加权 + 死区滞环
//   ⑧ 纠偏叠加 L2 实时控制器修正量叠加到 L3 目标（防逆流 > 需量 > 平抑）
//   ⑨ 安全兜底 safety.apply()             ← 指令压回安全区间（安全层兜底）
//   ⑩ 执行层   io_->write_command()       ← 先把「指令 + 权限区间」发布出去
//              io_->execute()              ← 再驱动 PCS（死区 + 惯性 + 变化率）
//   ⑪ 反馈     实际功率回灌下一拍 ①
//
// 注：⑩ 的两步不能合并。权限区间 (p_lower, p_upper) 是 EMS 对设备/SCADA 的
//   **公开声明**，设备侧要按它做交叉校核（"EMS 下发的指令是否落在它自己声明的
//   区间内"是联调期的硬契约）。IDeviceIO::execute() 只带 p_cmd 一个参数，
//   无法携带区间 —— 区间必须由 write_command() 单独发布，否则落地方（07/ 的
//   RtDbDeviceIO）永远看不到它，设备侧只能读到 (0,0)。
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "data_models.h"
#include "device_io.h"
#include "dispatch_coordinator.h"
#include "plant_model.h"
#include "safety_engine.h"
#include "sim_device_io.h"
#include "state_machine.h"
#include "strategy_base.h"
#include "strategy_manager.h"
#include "strategies_9.h"
#include "strategy_arbiter.h"
#include "ext_setpoints.h"   // A3.2：外部设定（调度遥调）读取 + 收窄

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace ems {

// =====================================================================
// 输出整形器 OutputShaper
//
// 作用：对**最终下发指令**做死区 + 方向滞环去抖。
//
// 为什么必须放在最后一级：仲裁器（04/）的死区只作用于 L3 的 desired，
// 而周期 7 的实时层纠偏（L2 控制器的修正量）是在仲裁之后叠加的。
// 若只依赖仲裁器死区，L2 纠偏的微小抖动会直接透传到 PCS —— 实测会
// 产生每秒数次的指令抖动。因此把死区/滞环放在输出最后一级才真正有效。
// =====================================================================
class OutputShaper {
public:
    void set_deadband(double d) { deadband_ = std::max(0.0, d); }
    double deadband() const { return deadband_; }
    void set_switch_delay(int n) { switch_delay_ = std::max(1, n); }
    int  switch_delay() const { return switch_delay_; }

    void reset() {
        zero_counter_ = 0;
        last_sign_ = 0;
        last_out_ = 0.0;
    }

    // 返回整形后的指令；*held 置位表示本拍因死区/滞环保持
    //
    // 语义：|cmd| 持续落在死区内 → 输出 0；一旦越出死区立即跟随。
    //   v1.x 的实现里多了一条"同方向死区内沿用上一拍输出"的分支，
    //   导致小信号**永远被保持**而不是被清零 —— 死区形同虚设，
    //   并网点附近的极限环完全无法抑制（试验 2 中开关两档结果一模一样）。
    double shape(double cmd, bool* held = nullptr) {
        if (held) *held = false;
        if (deadband_ <= 0.0) { last_out_ = cmd; last_sign_ = sign_of(cmd); return cmd; }

        if (std::fabs(cmd) < deadband_) {
            // 连续 N 拍都落在死区内 → 真正切到 0（N 拍滞环用于抑制边界抖动）
            if (++zero_counter_ >= switch_delay_) {
                zero_counter_ = 0;
                last_sign_ = 0;
                if (last_out_ != 0.0) { last_out_ = 0.0; if (held) *held = true; }
                return 0.0;
            }
            if (held) *held = true;
            return last_out_;      // 滞环确认期内保持上一拍输出
        }
        zero_counter_ = 0;
        last_sign_ = sign_of(cmd);
        last_out_ = cmd;
        return cmd;
    }

private:
    static int sign_of(double v) {
        if (v > 0.0) return 1;
        if (v < 0.0) return -1;
        return 0;
    }
    double deadband_ = 2.0;
    int    switch_delay_ = 3;
    int    zero_counter_ = 0;
    int    last_sign_ = 0;
    double last_out_ = 0.0;
};

// =====================================================================
// 闭环配置
// =====================================================================
struct LoopConfig {
    double dt_s = 0.1;                    // 控制周期（100 ms）
    bool   enable_state_machine = true;
    bool   enable_safety_engine = true;
    bool   enable_realtime_correction = true;
    bool   hold_last_on_comm_loss = true; // 采集层超时 → HOLD_LAST（接口规范 §6）
    double comm_stale_threshold_s = 5.0;  // 快照停滞多久算超时
    double demand_window_s = 900.0;       // 需量窗口
    // 实时层（L2）纠偏权限上限：L2 控制器只对优化层目标做**有界**修正。
    // 硬边界（并网不倒送、变压器容量、BMS 限值）由 L0/L1 安全层保证，
    // 因此这里限制 L2 的修正幅度，避免实时层完全覆盖优化层的调度意图。
    double l2_correction_max_kw = 100.0;
    // 输出整形（死区 + 方向滞环）：作用于最终下发指令，抑制 0 附近抖动
    bool   enable_output_shaper = true;
    double output_deadband_kw = 2.0;
    int    output_switch_delay = 3;
    int    log_every = 1;                 // 每 N 拍记一条日志（长时仿真降采样用）
    bool   enable_log = true;

    // 每拍是否从 IDeviceIO 刷新设备限值（dev_）。
    //
    // 为什么这是个**开关**而不是无条件刷新：dev_ 有两个写入者 ——
    //   ① IDeviceIO::read_limits()（设备侧说了算：BMS 动态降功率、禁充放位）
    //   ② 装配层注入（rt.device_limits() = cfg.limits，仿真/测试的夹具手法）
    // 无条件每拍刷新会让 ② 被 ① 覆盖，现有多处仿真注入（09/10/P1/P2）即刻失效。
    //
    // 缺省策略：**跟随适配器能力** —— attach_device() 时若
    // io->limits_are_live() 为真则自动打开（见 attach_device 的说明）。
    // 真实设备 / 共享内存适配器 = live，仿真适配器 = 不 live（限值来自配置，
    // 不存在"脚下会变"的问题），所以现场装配不需要记得手动打开。
    bool   refresh_limits_each_step = false;
};

// =====================================================================
// 单拍记录（用于指标计算与 CSV 导出）
// =====================================================================
struct StepRecord {
    Timestamp t = 0.0;
    EmsState  state = EmsState::kInit;
    double p_load = 0.0;
    double p_pv = 0.0;
    double p_grid = 0.0;
    double p_cmd = 0.0;        // 最终下发指令
    double p_actual = 0.0;     // 实际电池功率（反馈）
    double soc = 0.0;
    double temp = 0.0;
    double p_lower = 0.0;
    double p_upper = 0.0;
    double plan_target = 0.0;
    double correction = 0.0;
    bool   clamped = false;
    bool   safety_clip = false;
    bool   state_gated = false;
    bool   hold_last = false;
    int    fault_bits = 0;
    std::string reason;
};

// =====================================================================
// 闭环指标（周期 7 目标：无延迟、无振荡 —— 用可量化指标验收）
//
// 指标口径说明（避免歧义）：
//   跟踪误差 = P_actual − P_cmd（被控对象对指令的跟随程度）
//   超发率   = mean(max(0, |P_actual| − |P_cmd|)) / mean(|P_cmd|)
//              —— 实际出力"超出"指令的平均比例，反映惯性/滞后导致的过冲
//   指令抖动 = 相邻两拍指令变化超过阈值的次数（阈值默认 5 kW）
//   符号翻转 = 指令穿越零点（带 0.5 kW 死区）的次数
//   倒送     = P_grid < 0（向电网馈电）；防逆流场景下应尽量为 0
// =====================================================================
struct LoopMetrics {
    int    samples = 0;
    int    track_samples = 0;
    double rmse_track_kw = 0.0;         // 指令 vs 实际 的 RMSE
    double mean_abs_track_err_kw = 0.0; // 平均绝对跟踪误差
    double max_abs_track_err_kw = 0.0;  // 最大跟踪误差
    double steady_state_err_kw = 0.0;   // 末段（后 10%）平均绝对误差
    double over_delivery_pct = 0.0;     // 超发率（%）
    double settling_time_s = 0.0;       // 首次进入 ±5% 并保持 20 拍的用时
    int    sign_flips = 0;              // 指令符号翻转次数
    double flip_rate_per_s = 0.0;       // 符号翻转率（/s）
    int    cmd_reversals = 0;           // 指令方向反转次数（振荡的直接度量）
    double reversal_rate_per_s = 0.0;   // 方向反转率（/s）
    int    cmd_jitter = 0;              // 指令大幅跳变次数（|Δcmd| > 阈值），仅作参考
    double cmd_travel_kw = 0.0;         // 指令总行程 Σ|Δcmd|（输出"抖动总量"的标准度量）
    double cmd_travel_per_s = 0.0;      // 平均行程速率（kW/s）
    double max_reverse_kw = 0.0;        // 最大倒送功率（正数表示倒送幅度）
    double reverse_duration_s = 0.0;    // 有效倒送累计时长（|倒送| > 5 kW 才算）
    double mean_grid_kw = 0.0;
    double max_grid_kw = 0.0;
    int    hold_last_count = 0;
    int    safety_clip_count = 0;
    int    state_gate_count = 0;
    double mean_cycle_us = 0.0;
    double max_cycle_us = 0.0;
    int    state_changes = 0;

    std::string to_string() const {
        std::ostringstream os;
        os.setf(std::ios::fixed);
        os.precision(3);
        os << "samples=" << samples
           << " mean_err=" << mean_abs_track_err_kw << "kW"
           << " rmse=" << rmse_track_kw << "kW"
           << " max_err=" << max_abs_track_err_kw << "kW"
           << " steady_err=" << steady_state_err_kw << "kW"
           << " over_delivery=" << over_delivery_pct << "%"
           << " settle=" << settling_time_s << "s"
           << " flips=" << sign_flips << "(" << flip_rate_per_s << "/s)"
           << " reversals=" << cmd_reversals << "(" << reversal_rate_per_s << "/s)"
           << " jitter=" << cmd_jitter
           << " travel=" << cmd_travel_kw << "kW(" << cmd_travel_per_s << "/s)"
           << " max_reverse=" << max_reverse_kw << "kW"
           << " reverse_t=" << reverse_duration_s << "s"
           << " hold_last=" << hold_last_count
           << " safety_clip=" << safety_clip_count
           << " state_gate=" << state_gate_count
           << " state_changes=" << state_changes
           << " mean_cycle=" << mean_cycle_us << "us";
        return os.str();
    }

    // 无振荡判定（工程口径）：
    //   · 指令换向频率 ≤ 0.2 /s（平均每 5 秒不超过一次换向）
    //   · 指令方向反转率 ≤ 1.0 /s（不应持续来回摆）
    bool no_oscillation(double flip_rate_limit = 0.20,
                        double reversal_rate_limit = 1.00) const {
        return flip_rate_per_s <= flip_rate_limit &&
               reversal_rate_per_s <= reversal_rate_limit;
    }
    // 无延迟判定（工程口径）：
    //   · 稳态跟踪误差 ≤ 15 kW（相对 250kW 级 PCS 约 6%）
    //   · 超发率 ≤ 25%（惯性导致的过冲在可控范围）
    // 整定时间在连续变化的激励下不具判别力，仅作参考输出。
    bool no_lag(double steady_err_limit_kw = 15.0,
                double over_delivery_limit_pct = 25.0) const {
        return steady_state_err_kw <= steady_err_limit_kw &&
               over_delivery_pct <= over_delivery_limit_pct;
    }

    static LoopMetrics compute(const std::vector<StepRecord>& recs, double dt_s,
                               double jitter_threshold_kw = 5.0,
                               double reverse_eps_kw = 5.0) {
        LoopMetrics m;
        m.samples = static_cast<int>(recs.size());
        if (recs.empty()) return m;

        double se = 0.0, sae = 0.0, over = 0.0, abs_cmd = 0.0;
        int flips = 0, reversals = 0, jitter = 0;
        double prev_cmd = 0.0, prev_delta = 0.0;
        bool prev_sign = false;
        bool sign_valid = false;
        int running_cnt = 0;
        bool settled = false;
        double settle_time = 0.0;
        const double sign_eps = 0.5;
        const double dir_eps  = 2.0;   // 方向反转的最小幅度（滤掉数值噪声）

        for (size_t i = 0; i < recs.size(); ++i) {
            const auto& r = recs[i];
            const double err = r.p_actual - r.p_cmd;
            se  += err * err;
            sae += std::fabs(err);
            abs_cmd += std::fabs(r.p_cmd);
            m.track_samples++;
            m.max_abs_track_err_kw = std::max(m.max_abs_track_err_kw, std::fabs(err));

            double excess = std::fabs(r.p_actual) - std::fabs(r.p_cmd);
            if (excess > 0.0) over += excess;

            // 指令符号翻转（带死区，避免 0 附近误判）
            if (std::fabs(r.p_cmd) > sign_eps) {
                bool sign = (r.p_cmd > 0.0);
                if (sign_valid && sign != prev_sign) ++flips;
                prev_sign = sign;
                sign_valid = true;
            }
            // 指令方向反转（振荡的直接度量：增量的符号发生翻转）
            if (i > 0) {
                double delta = r.p_cmd - prev_cmd;
                m.cmd_travel_kw += std::fabs(delta);      // 指令总行程 Σ|Δcmd|
                if (std::fabs(delta) > dir_eps && std::fabs(prev_delta) > dir_eps) {
                    if ((delta > 0.0) != (prev_delta > 0.0)) ++reversals;
                }
                if (std::fabs(delta) > dir_eps) prev_delta = delta;
                if (std::fabs(delta) > jitter_threshold_kw) ++jitter;
            }
            prev_cmd = r.p_cmd;

            // 倒送统计（时长只统计"有效倒送"，滤掉趋近零的渐近尾）
            if (r.p_grid < 0.0) {
                m.max_reverse_kw = std::max(m.max_reverse_kw, -r.p_grid);
            }
            if (r.p_grid < -reverse_eps_kw) m.reverse_duration_s += dt_s;
            m.mean_grid_kw += r.p_grid;
            m.max_grid_kw = std::max(m.max_grid_kw, r.p_grid);

            if (r.hold_last)   m.hold_last_count++;
            if (r.safety_clip) m.safety_clip_count++;
            if (r.state_gated) m.state_gate_count++;

            // 整定时间：连续 20 拍误差在 ±5% 或 ±2kW 内
            double tol = std::max(2.0, 0.05 * std::fabs(r.p_cmd));
            if (!settled) {
                if (std::fabs(err) <= tol) {
                    if (++running_cnt >= 20) { settled = true; settle_time = r.t; }
                } else {
                    running_cnt = 0;
                }
            }
        }
        const int n = std::max(1, m.track_samples);
        m.rmse_track_kw         = std::sqrt(se / n);
        m.mean_abs_track_err_kw = sae / n;
        m.over_delivery_pct     = 100.0 * over / std::max(1e-6, abs_cmd);
        m.sign_flips            = flips;
        m.cmd_reversals         = reversals;
        m.cmd_jitter            = jitter;
        m.mean_grid_kw          = m.mean_grid_kw / n;
        m.settling_time_s       = settle_time;

        double dur = recs.back().t - recs.front().t;
        if (dur > 0.0) {
            m.flip_rate_per_s     = flips     / dur;
            m.reversal_rate_per_s = reversals / dur;
            m.cmd_travel_per_s    = m.cmd_travel_kw / dur;
        }

        // 稳态误差：末段 10%
        size_t tail = recs.size() / 10;
        if (tail < 1) tail = 1;
        double tse = 0.0;
        for (size_t i = recs.size() - tail; i < recs.size(); ++i) {
            tse += std::fabs(recs[i].p_actual - recs[i].p_cmd);
        }
        m.steady_state_err_kw = tse / tail;

        int changes = 0;
        for (size_t i = 1; i < recs.size(); ++i)
            if (recs[i].state != recs[i - 1].state) ++changes;
        m.state_changes = changes;
        return m;
    }
};

// =====================================================================
// EmsRuntime —— 周期 5/6/7/8 的总编排（由 07/ 提供，装配 05/06/08 的能力）
//
// 产品化 P0（架构分层）：本类**不再直接持有 PlantModel**，而是通过
// IDeviceIO* 读写设备（见 04/src/device_io.h）。默认挂 SimDeviceIO（仿真），
// 现场部署用 attach_device() 换成 RT_DB / Modbus 适配器，**算法代码零改动**。
// 这是"能交付"与"只是实验室 demo"的分界线。
// =====================================================================
class EmsRuntime {
public:
    EmsRuntime() { init(); }

    // -----------------------------------------------------------------
    // 初始化：注册策略、接好优化器、复位状态机
    // -----------------------------------------------------------------
    void init() {
        // --- P0 设备 I/O 抽象：默认挂仿真适配器 ---
        // 若装配层已通过 attach_device() 注入真实适配器（RT_DB / Modbus），
        // 则保留注入的那个 —— init() 幂等，不得覆盖外部注入。
        if (io_ == nullptr) io_ = static_cast<IDeviceIO*>(&sim_io_);

        // --- 策略注册：L2 三个实时控制器 + L3 三个经济策略 ---
        // L0/L1 由 SafetyEngine 统一承担（见 safety_engine.h），不再注册
        // strategies_9.h 里的 BmsForbid / BmsDerate / TransformerLimit 三个 MOCK。
        // 注：StrategyManager 内含 mutex，不可整体赋值；clear() 保证 init() 幂等。
        mgr_.clear();
        mgr_.register_strategy(std::make_shared<DemandMgmtStrategy>());
        mgr_.register_strategy(std::make_shared<AntiReverseStrategy>());
        mgr_.register_strategy(std::make_shared<PvSmoothingStrategy>());
        mgr_.register_strategy(std::make_shared<PeakValleyStrategy>());
        mgr_.register_strategy(std::make_shared<DemandResponseStrategy>());

        plan_tracker_ = std::make_shared<PlanTrackingStrategy>();
        mgr_.register_strategy(plan_tracker_);

        mgr_.start_all();
        // 默认 MPC 模式：计划跟踪策略独占 L3（接口规范 §2.5 运行模式独占）
        mgr_.set_run_mode(RunMode::kMPC);

        refresh_device_limits();

        // --- 周期 8：协同器接优化器 ---
        coord_ = DispatchCoordinator(coord_cfg_);
        coord_.set_optimizer(&optimizer_);
        coord_.set_device_params(safety_params_.soc_min, safety_params_.soc_max,
                                 io_->battery_capacity_kwh(),
                                 dev_.pcs_rated_chg_kw, dev_.pcs_rated_dis_kw);

        arbiter_.set_deadband(0.0);      // 死区/滞环统一放到输出最后一级（OutputShaper）
        arbiter_.set_switch_delay(1);
        shaper_.set_deadband(cfg_.output_deadband_kw);
        shaper_.set_switch_delay(cfg_.output_switch_delay);
        shaper_.reset();

        fsm_ = EmsStateMachine(fsm_cfg_);
        fsm_.reset_all();
        fsm_.request_run(true);

        t_ = 0.0;
        log_.clear();
        win_buf_.clear();
        win_sum_ = 0.0;
        step_index_ = 0;
        cycle_us_sum_ = 0.0;
        cycle_us_max_ = 0.0;
        stale_elapsed_s_ = 0.0;
        last_cmd_ = PowerCommand{};
        last_verdict_ = SafetyVerdict{};
        safety_.reset();
    }

    // 更换被控对象配置（会同步刷新设备限制），可在 init() 之后调用。
    // 注：本方法只对**仿真适配器**有意义（真实设备不接受配置注入）。
    //     现场部署时不调用它，改由 attach_device() 注入真实适配器。
    void configure_plant(const PlantConfig& pc) {
        sim_io_.set_config(pc);
        refresh_device_limits();
        if (plan_tracker_) {
            plan_tracker_->set_bounds(dev_.pcs_rated_chg_kw, dev_.pcs_rated_dis_kw);
        }
        coord_.set_device_params(safety_params_.soc_min, safety_params_.soc_max,
                                 io_->battery_capacity_kwh(),
                                 dev_.pcs_rated_chg_kw, dev_.pcs_rated_dis_kw);
    }

    // -----------------------------------------------------------------
    // 注入设备适配器（产品化 P0 的入口）
    //
    // 装配层（进程入口）在 init() 之前或之后调用：
    //     EmsRuntime rt;
    //     RtDbDeviceIO io(shared_mem_name);
    //     rt.attach_device(&io);      // 此后算法只经 io 读写设备
    //     rt.init();                  // 幂等，不会覆盖已注入的 io
    //
    // 传 nullptr 表示恢复默认仿真适配器。适配器生命周期由调用方管理
    // （EmsRuntime 只持有裸指针，不拥有）。
    // -----------------------------------------------------------------
    void attach_device(IDeviceIO* io) {
        io_ = io ? io : static_cast<IDeviceIO*>(&sim_io_);
        // 立刻按新设备的限制刷新一次，并把设备参数同步给优化协同层。
        // 否则优化层仍在用旧设备的额定/容量做 96 点规划，与实时层不一致。
        refresh_device_limits();
        // 限值的**运行期刷新**跟随适配器能力（见 LoopConfig 的说明）。
        // 只打开、不关闭：调用方若已显式设 false（仿真夹具要固定注入），
        // 这里不能悄悄改回去。
        if (io_->limits_are_live()) cfg_.refresh_limits_each_step = true;
        coord_.set_device_params(safety_params_.soc_min, safety_params_.soc_max,
                                 io_->battery_capacity_kwh(),
                                 dev_.pcs_rated_chg_kw, dev_.pcs_rated_dis_kw);
    }
    IDeviceIO* device() { return io_; }
    const IDeviceIO* device() const { return io_; }

    // -----------------------------------------------------------------
    // 外部设定源（A3.2）：调度经网关写 EXT 区，这里只注入"怎么读"。
    //
    // read      按点索引读一个 double，返回该点当前是否可读（与 ext_setpoints.h
    //           的 ReadFn 同形：bool(int, double&)）。由装配层用 RT_DB 句柄构造。
    // stale_s   超时秒；<=0 表示"禁用外部设定"（见 ext_setpoints.h）。
    // now_fn    墙钟秒（判定陈旧用）。默认 system_clock；测试可注入假时钟。
    //
    // 不设置（read 为空）→ 本拍无外部设定，行为与历史逐位一致（仿真默认）。
    // -----------------------------------------------------------------
    void set_ext_source(std::function<bool(int, double&)> read, double stale_s,
                        std::function<double()> now_fn = nullptr) {
        ext_read_    = std::move(read);
        ext_stale_s_ = stale_s;
        ext_now_fn_  = now_fn ? std::move(now_fn)
                              : std::function<double()>([]() {
                                    using namespace std::chrono;
                                    return duration<double>(
                                        system_clock::now().time_since_epoch()).count();
                                });
    }
    bool has_ext_source() const { return static_cast<bool>(ext_read_); }

    // -----------------------------------------------------------------
    // 一拍闭环
    // -----------------------------------------------------------------
    StepRecord step(double dt_s) {
        if (dt_s <= 0.0) dt_s = cfg_.dt_s;
        auto t0 = std::chrono::steady_clock::now();

        t_ += dt_s;

        // ---------- ⓪ 设备限值刷新 ----------
        // 必须在①之前：本拍的仲裁/安全用的是**本拍的**限值。
        // 关闭时（仿真/测试的夹具注入场景）行为与历史逐位一致。
        if (cfg_.refresh_limits_each_step) refresh_device_limits();

        // ---------- ① 采集层：冻结快照 ----------
        // 经 IDeviceIO 读量测 —— 算法不知道底层是 PlantModel / RT_DB / Modbus。
        // 采集品质（通信中断、数据无效）由 ② 的 read_status() 反映，不在此处分支。
        RealtimeSnapshot rt;
        io_->read_snapshot(t_, rt);
        fill_context(rt, dt_s);

        // ---------- ② 故障判定 ----------
        FaultFlags faults = detect_faults();

        // 采集层超时（电表通信丢失累计 > 阈值）
        if (faults.meter_comm_lost) stale_elapsed_s_ += dt_s;
        else                        stale_elapsed_s_ = 0.0;
        const bool data_stale = stale_elapsed_s_ > cfg_.comm_stale_threshold_s;

        // ---------- ③ 安全预判 ----------
        SafetyVerdict verdict;
        if (cfg_.enable_safety_engine) {
            verdict = safety_.evaluate(rt, dev_, grid_,
                                       last_cmd_.p_bat_cmd_kw, dt_s,
                                       ext_of_tick());
        } else {
            verdict.p_lower = -dev_.pcs_rated_chg_kw;
            verdict.p_upper =  dev_.pcs_rated_dis_kw;
        }
        last_verdict_ = verdict;

        // ---------- ④ 状态机 ----------
        if (cfg_.enable_state_machine) {
            fsm_.update(t_, faults, verdict);
        } else {
            // 不启用状态机时，直接以"启动"许可运行
            if (fsm_.state() != EmsState::kNormal) {
                fsm_.request_run(true);
                fsm_.update(t_, faults, verdict);
            }
        }

        // ---------- ⑤ 协同层（周期 8）----------
        double plan_target = 0.0, coord_target = 0.0, correction = 0.0;
        bool strategies_on = (!cfg_.enable_state_machine) || fsm_.strategies_enabled();
        if (strategies_on) {
            coord_.update(rt, t_, dt_s);
            plan_target = coord_.plan_target_kw();
            coord_target = coord_.coordinated_target_kw();
            plan_tracker_->set_target(coord_target, coord_.soc_ref(), coord_.plan_valid());
        }

        // ---------- ⑥ 策略层 ----------
        std::vector<StrategyResult> results;
        if (strategies_on) results = mgr_.tick(rt, dev_);

        // 安全引擎的 L0/L1 结论并入同一批结果，交给仲裁器统一收敛
        if (cfg_.enable_safety_engine) {
            std::vector<StrategyResult> safe = verdict.as_results();
            results.insert(results.end(), safe.begin(), safe.end());
        }

        // ---------- ⑦ 仲裁层 ----------
        PowerCommand cmd = arbiter_.arbitrate(results, t_);

        // ---------- ⑧ 实时层纠偏叠加 ----------
        // L2 实时控制器（防逆流/需量/平抑）对 L3 优化目标做二次修正
        if (strategies_on && cfg_.enable_realtime_correction) {
            correction = merge_realtime_correction(results, cmd.p_bat_cmd_kw);
            // 实时层权限限幅（有界纠偏）
            const double lim = cfg_.l2_correction_max_kw;
            if (lim > 0.0) correction = std::max(-lim, std::min(lim, correction));
            if (std::fabs(correction) > 1e-9) {
                double p = cmd.p_bat_cmd_kw + correction;
                p = std::max(cmd.p_lower, std::min(cmd.p_upper, p));
                cmd.p_bat_cmd_kw = p;
                cmd.reason += "+l2_correction";
            }
        }

        // ---------- ⑨ 输出整形 + 安全兜底 + 状态门控 ----------
        // 顺序很重要：
        //   整形（死区/滞环去抖） → 安全层（区间限幅 + 变化率限速）
        //   → 状态机（FAULT/EMERGENCY 强制 0，覆盖一切） → HOLD_LAST
        bool shaped_held = false;
        if (cfg_.enable_output_shaper) {
            cmd.p_bat_cmd_kw = shaper_.shape(cmd.p_bat_cmd_kw, &shaped_held);
            if (shaped_held) cmd.reason += "+shaper_hold";
        }

        bool safety_clip = false;
        if (cfg_.enable_safety_engine) {
            safety_clip = safety_.apply(cmd);
        }

        bool state_gated = false;
        if (cfg_.enable_state_machine && !fsm_.output_enabled()) {
            cmd.p_bat_cmd_kw = 0.0;
            // 权限区间必须与门控一致收成 [0,0]。否则会出现"指令 0 却声明
            // 允许区间 [42, 250]"的矛盾：设备端按 PermissionRange 限幅执行时
            // 会把 0 抬到 42 —— 恰恰违反门控（非运行态不得动作）。
            cmd.p_lower = 0.0;
            cmd.p_upper = 0.0;
            cmd.reason = std::string("state_gate:") + fsm_.state_str();
            state_gated = true;
            shaper_.reset();          // 非运行态复位整形器，恢复时从 0 起步
        }

        // 采集层超时 → HOLD_LAST（复用上一拍命令，接口规范 §6）
        bool hold_last = false;
        if (data_stale && cfg_.hold_last_on_comm_loss) {
            cmd.p_bat_cmd_kw = last_cmd_.p_bat_cmd_kw;
            // 同理：保持指令的同时把权限区间钉在该指令上（单点），
            // 保证 p_cmd ∈ [p_lower, p_upper] 这条硬不变量在任何路径下都成立。
            cmd.p_lower = cmd.p_bat_cmd_kw;
            cmd.p_upper = cmd.p_bat_cmd_kw;
            cmd.reason = "hold_last";
            hold_last = true;
        }

        last_cmd_ = cmd;

        // ---------- ⑩ 执行层：PCS 执行 ----------
        // 先发布「指令 + 权限区间」（SCADA / 设备进程从这里取；设备侧要按 EMS
        // 声明的区间做交叉校核），再驱动执行机构。
        // 经 IDeviceIO 下发。返回值在仿真下即本拍实际功率；真实适配器下只是
        // "尽力反馈"，**闭环不依赖它** —— 闭环走下一拍的 ① 量测（p_bat_actual_kw）。
        io_->write_command(cmd);
        double p_actual = io_->execute(cmd.p_bat_cmd_kw, dt_s);

        // ---------- ⑪ 反馈 + 记录 ----------
        DeviceActuals act = io_->read_actuals();
        StepRecord rec;
        rec.t           = t_;
        rec.state       = fsm_.state();
        rec.p_load      = act.p_load_kw;
        rec.p_pv        = act.p_pv_kw;
        rec.p_grid      = act.p_grid_kw;
        rec.p_cmd       = cmd.p_bat_cmd_kw;
        rec.p_actual    = p_actual;
        rec.soc         = act.soc;
        rec.temp        = act.temperature_c;
        rec.p_lower     = cmd.p_lower;
        rec.p_upper     = cmd.p_upper;
        rec.plan_target = plan_target;
        rec.correction  = correction;
        rec.clamped     = cmd.clamped;
        rec.safety_clip = safety_clip;
        rec.state_gated = state_gated;
        rec.hold_last   = hold_last;
        rec.fault_bits  = faults.bits();
        rec.reason      = cmd.reason;

        auto t1 = std::chrono::steady_clock::now();
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        cycle_us_sum_ += us;
        cycle_us_max_ = std::max(cycle_us_max_, us);

        ++step_index_;
        if (cfg_.enable_log) {
            int stride = cfg_.log_every < 1 ? 1 : cfg_.log_every;
            if (stride == 1 || (step_index_ % stride) == 0) log_.push_back(rec);
        }
        return rec;
    }
    void run(int n_steps, double dt_s,
             const std::function<void(EmsRuntime&, int)>& hook = nullptr) {
        for (int i = 0; i < n_steps; ++i) {
            if (hook) hook(*this, i);
            step(dt_s);
        }
    }

    // 汇总指标
    LoopMetrics metrics() const {
        // 日志按 log_every 降采样存储，故相邻日志记录的时间间隔是 dt_s×stride，
        // 而不是 dt_s。指标里的"率"（flip/rev/travel per second）依赖该间隔 ——
        // 传 dt_s 会让所有率被放大 stride 倍（长时仿真下 stride 可达 10~100）。
        const int stride = cfg_.log_every < 1 ? 1 : cfg_.log_every;
        LoopMetrics m = LoopMetrics::compute(log_, cfg_.dt_s * stride);
        int n = static_cast<int>(log_.size());
        m.mean_cycle_us = n ? cycle_us_sum_ / n : 0.0;
        m.max_cycle_us  = cycle_us_max_;
        return m;
    }

    // CSV 导出
    bool dump_csv(const std::string& path) const {
        std::ofstream f(path.c_str());
        if (!f.is_open()) return false;
        f << "t_s,state,P_load_kW,P_pv_kW,P_grid_kW,P_cmd_kW,P_actual_kW,SOC,"
             "T_C,P_lower,P_upper,plan_target,correction,clamped,safety_clip,"
             "state_gated,hold_last,reason\n";
        for (const auto& r : log_) {
            f << r.t << "," << state_name(r.state) << ","
              << r.p_load << "," << r.p_pv << "," << r.p_grid << ","
              << r.p_cmd << "," << r.p_actual << "," << r.soc << ","
              << r.temp << "," << r.p_lower << "," << r.p_upper << ","
              << r.plan_target << "," << r.correction << ","
              << (r.clamped ? 1 : 0) << "," << (r.safety_clip ? 1 : 0) << ","
              << (r.state_gated ? 1 : 0) << "," << (r.hold_last ? 1 : 0) << ","
              << r.reason << "\n";
        }
        return true;
    }

    // -----------------------------------------------------------------
    // 配置与访问器
    // -----------------------------------------------------------------
    LoopConfig& config() { return cfg_; }
    const LoopConfig& config() const { return cfg_; }
    SafetyParams& safety_params() { return safety_params_; }
    const SafetyParams& safety_params() const { return safety_params_; }
    CoordinatorConfig& coordinator_config() { return coord_cfg_; }
    StateMachineConfig& fsm_config() { return fsm_cfg_; }

    void apply_configs() {
        safety_.set_params(safety_params_);
        fsm_.set_config(fsm_cfg_);
        coord_.set_config(coord_cfg_);
        coord_.set_device_params(safety_params_.soc_min, safety_params_.soc_max,
                                 io_->battery_capacity_kwh(),
                                 dev_.pcs_rated_chg_kw, dev_.pcs_rated_dis_kw);
    }

    // 默认仿真适配器的被控对象。**仅在未 attach_device() 时有效** ——
    // 注入真实适配器后它返回的是一个未被使用的仿真对象。
    // 保留此访问器是为了兼容既有 6594 条断言（大量使用 plant().set_pcs_fault()
    // 等仿真注入）。新增的生产代码请改用 device()。
    PlantModel& plant() { return sim_io_.plant(); }
    const PlantModel& plant() const { return sim_io_.plant(); }
    DeviceLimits& device_limits() { return dev_; }
    const DeviceLimits& device_limits() const { return dev_; }
    GridQuality& grid() { return grid_; }
    SafetyEngine& safety() { return safety_; }
    const SafetyVerdict& last_verdict() const { return last_verdict_; }
    EmsStateMachine& fsm() { return fsm_; }
    StrategyManager& manager() { return mgr_; }
    StrategyArbiter& arbiter() { return arbiter_; }
    OutputShaper& shaper() { return shaper_; }
    DispatchCoordinator& coordinator() { return coord_; }
    std::shared_ptr<PlanTrackingStrategy> plan_tracker() { return plan_tracker_; }
    const PowerCommand& last_command() const { return last_cmd_; }
    const std::vector<StepRecord>& log() const { return log_; }
    Timestamp now() const { return t_; }

    // 环境注入（负荷 / 光伏）。**仅对仿真适配器有效** —— 真实系统里负荷与
    // 光伏来自电表量测，由采集层提供，算法不得设置。现场部署时请勿调用。
    void set_environment(double p_load_kw, double p_pv_kw) {
        sim_io_.set_environment(p_load_kw, p_pv_kw);
    }
    void set_forecast(const ForecastSeries& fc) {
        fc_ = fc;
        coord_.set_forecast(fc);
    }
    const ForecastSeries& forecast() const { return fc_; }
    bool load_plan_file(const std::string& path) {
        DayPlan p;
        std::string err;
        if (!ems::load_plan_file(path, p, &err)) return false;
        coord_.set_plan(p);
        return true;
    }
    void set_plan(const DayPlan& p) { coord_.set_plan(p); }

    // 手动急停 / 复位
    void emergency_stop(const std::string& why) { fsm_.trigger_emergency_stop(why); }
    bool reset_emergency() {
        return fsm_.reset_emergency(t_, detect_faults());
    }

    FaultFlags detect_faults() const {
        FaultFlags f;
        const DeviceStatus st = io_->read_status();
        f.bms_comm_lost   = !st.bms_comm_ok || st.device_offline;
        f.pcs_comm_lost   = !st.pcs_comm_ok;
        f.meter_comm_lost = !st.meter_comm_ok;
        f.pcs_fault       = st.pcs_fault;
        f.data_invalid    = !st.data_valid;
        f.device_offline  = st.device_offline;
        f.temp_fault      = io_->read_actuals().temperature_c >= safety_.params().temp_fault_c;
        return f;
    }

private:
    // 设备限制刷新：唯一入口是 IDeviceIO::read_limits()。
    //
    // ★ 2026-09-19 修正：此前这里写着"现场 BMS 动态降功率就是通过这个入口
    //   每拍刷进 dev_ 的"，但 step() 里**根本没有调用** —— dev_ 只在
    //   init / reset / configure_plant / attach_device 里刷新，也就是运行期
    //   **冻结**。后果远不止"注释不准"：BMS 动态降功率、BMS 禁充放位、
    //   PCS 额定、变压器容量、契约需量全部退化成**装配期常量**。
    //   修法见 LoopConfig::refresh_limits_each_step（由 attach_device 按
    //   适配器能力自动打开）。教训与本项目前几次完全一致：
    //   **注释声称的行为必须与代码实际的行为一致**，否则它会掩盖真实缺口。
    void refresh_device_limits() {
        if (io_ == nullptr) return;
        io_->read_limits(dev_);
    }

    // ---- 填充快照上下文（TOU 电价 / 需量窗口）----
    void fill_context(RealtimeSnapshot& rt, double dt_s) {
        // 电价窗口
        if (fc_.loaded) {
            double pr = fc_.price_at(t_);
            double lo = 0.0, hi = 0.0;
            fc_.price_quantiles(&lo, &hi);
            rt.pricing.cur_tou_price = pr;
            rt.pricing.cur_tou_type  = fc_.classify_tou(pr, lo, hi);
        } else {
            double h = std::fmod(t_, 86400.0) / 3600.0;
            bool valley = (h < 8.0 || h >= 22.0);
            rt.pricing.cur_tou_type  = valley ? TouType::kValley : TouType::kPeak;
            rt.pricing.cur_tou_price = valley ? 0.30 : 0.90;
        }

        // 下一拍预测（供安全层的并网/变压器边界做前瞻，消除阶梯跳变穿越）
        if (fc_.loaded && fc_.size() > 0) {
            double ln = 0.0, pn = 0.0;
            if (fc_.sample(t_ + dt_s, &ln, &pn, nullptr)) {
                rt.p_load_next_kw = ln;
                rt.p_pv_next_kw   = pn;
                rt.has_lookahead  = true;
            }
        }

        // 需量窗口（滑动平均，O(1) 增量维护，避免长时仿真 O(N) 求和）
        win_buf_.push_back(rt.p_grid_kw);
        win_sum_ += rt.p_grid_kw;
        size_t n = static_cast<size_t>(std::max(1.0, cfg_.demand_window_s / dt_s));
        while (win_buf_.size() > n) {
            win_sum_ -= win_buf_.front();
            win_buf_.pop_front();
        }
        rt.demand_window.window_s      = cfg_.demand_window_s;
        rt.demand_window.t_elapsed_s   = static_cast<double>(win_buf_.size()) * dt_s;
        rt.demand_window.p_avg_past_kw = win_buf_.empty() ? 0.0 : win_sum_ / win_buf_.size();
    }

    // ---- L2 实时控制器纠偏合成 ----
    // 与 03/integration 的合并逻辑保持一致：**覆盖式，不叠加**
    // 优先级：防逆流（并网合规，绝对优先）> 需量管理 > 光伏平抑
    //
    // 关键点：三个 L2 控制器的 p_desired **语义不同**，不能一律当成增量相加：
    //   防逆流 / 需量管理 → p_desired 是"绝对目标功率"（我要求 P_bat 至少/至多到多少）
    //   光伏平抑         → p_desired 是"增量"（相对当前出力要再吸收多少）
    // 若把绝对目标直接加到 L3 优化指令上，等于把"目标"当成"增量"重复计入 ——
    // 周期 8 有日间计划时会出现指令被顶到边界、计划跟踪失效。
    // 故对绝对语义的两个控制器转成"只朝它要求的方向推、最多推到它要求的值"的增量：
    //   需量管理：P_bat 至少要放 p_dem  → 只在 p_cmd < p_dem 时补 (p_dem − p_cmd)
    //   防逆流  ：P_bat 至少要充 p_rev  → 只在 p_cmd > p_rev 时补 (p_rev − p_cmd)
    // 这样在 p_cmd ≈ 0（无计划 / 纯实时控制）时与原实现完全等价。
    double merge_realtime_correction(const std::vector<StrategyResult>& results,
                                     double p_cmd) const {
        const double rev_deadband  = 0.5;   // kW
        const double dem_threshold = 10.0;  // kW
        double p_rev = 0.0, p_dem = 0.0, p_smooth = 0.0;
        bool has_rev = false, has_dem = false, has_smooth = false;
        for (const auto& r : results) {
            if (r.priority != Priority::kL2_LocalEcon) continue;
            if (r.strategy_id == strategy_id::kAntiReverse) {
                p_rev = r.p_desired; has_rev = true;
            } else if (r.strategy_id == strategy_id::kDemandMgmt) {
                p_dem = r.p_desired; has_dem = true;
            } else if (r.strategy_id == strategy_id::kPvSmoothing) {
                p_smooth = r.p_desired; has_smooth = true;
            }
        }
        // 防逆流 p_desired 已是 P_bat 符号（负 = 充电），需要充电时优先执行
        if (has_rev && p_rev < -rev_deadband && p_cmd > p_rev) return p_rev - p_cmd;
        if (has_dem && p_dem > dem_threshold && p_cmd < p_dem) return p_dem - p_cmd;
        if (has_smooth) return p_smooth;
        return 0.0;
    }

    // 本拍外部设定快照。无源 → 默认空（usable()=false → 区间逐位不动）。
    ExtSetpoints ext_of_tick() const {
        if (!ext_read_) return ExtSetpoints{};
        const double now = ext_now_fn_ ? ext_now_fn_() : 0.0;
        return load_ext_setpoints(ext_read_, now, ext_stale_s_);
    }

    LoopConfig        cfg_{};
    SafetyParams      safety_params_{};
    CoordinatorConfig coord_cfg_{};
    StateMachineConfig fsm_cfg_{};

    // --- P0 设备 I/O 抽象 ---
    // sim_io_ 是默认适配器，持有 PlantModel；io_ 是算法唯一可见的设备句柄。
    // 声明顺序有讲究：sim_io_ 必须先于 init() 使用而构造（成员先于构造函数体）。
    SimDeviceIO       sim_io_{};
    IDeviceIO*        io_ = nullptr;

    SafetyEngine      safety_{};
    EmsStateMachine   fsm_{};
    StrategyManager   mgr_{};
    StrategyArbiter   arbiter_{};
    OutputShaper      shaper_{};
    DispatchCoordinator coord_{};
    HeuristicOptimizer  optimizer_{};
    std::shared_ptr<PlanTrackingStrategy> plan_tracker_{};

    // 外部设定源（A3.2）。ext_read_ 为空 = 仿真默认（无外部设定）。
    std::function<bool(int, double&)> ext_read_{};
    double                    ext_stale_s_ = 0.0;
    std::function<double()>   ext_now_fn_{};

    DeviceLimits      dev_{};
    GridQuality       grid_{};
    ForecastSeries    fc_{};
    PowerCommand      last_cmd_{};
    SafetyVerdict     last_verdict_{};

    Timestamp         t_ = 0.0;
    double            stale_elapsed_s_ = 0.0;
    std::deque<double> win_buf_;
    double            win_sum_ = 0.0;
    std::vector<StepRecord> log_;
    long long         step_index_ = 0;

    double cycle_us_sum_ = 0.0;
    double cycle_us_max_ = 0.0;
};

} // namespace ems
