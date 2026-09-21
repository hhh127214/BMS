// =====================================================================
// 07/ — 被控对象仿真模型 PlantModel（周期 7 的"真实设备"替身）
//
// 作用：在闭环中扮演 BMS + PCS + 电表 + 电池，把 EMS 下发的功率指令变成
//       **带物理惯性的实际功率**，并把实际值回馈给下一拍的采集层。
//
// 建模要点（都是为了复现真实闭环的"延迟"与"振荡"来源）：
//   1. 执行死区时间  : PCS 收到指令到开始动作之间的纯延迟
//   2. 一阶惯性      : PCS 功率环的响应滞后（时间常数 tau）
//   3. 物理变化率限幅: PCS 自身的 kW/s 能力上限
//   4. 充放电效率    : 影响 SOC 演化（充电效率 <1，放电效率 <1）
//   5. 热模型        : 温升 ∝ 功率平方，散热 ∝ 温差
//   6. 量测噪声      : 电表读数噪声（默认 0，保证测试确定性）
//   7. 通信开关      : 可注入 BMS / 电表 / PCS 通信与故障
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "data_models.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <random>
#include <string>
#include <vector>

namespace ems {

struct PlantConfig {
    // ---- 电池 ----
    double battery_capacity_kwh = 1000.0;
    double soc_init             = 0.50;
    double soc_phys_min         = 0.05;   // 物理下限（BMS 保护动作点）
    double soc_phys_max         = 0.95;   // 物理上限
    double eta_chg              = 0.95;
    double eta_dis              = 0.95;
    double soh                  = 1.00;

    // ---- PCS ----
    double pcs_max_chg_kw   = 200.0;
    double pcs_max_dis_kw   = 200.0;
    double pcs_ramp_kw_per_s = 300.0;   // 物理变化率上限
    double pcs_tau_s         = 0.50;    // 一阶惯性时间常数
    double pcs_deadtime_s    = 0.20;    // 执行死区时间
    double pcs_standby_kw    = 2.0;     // 空载损耗

    // ---- 热模型 ----
    double temp_ambient_c   = 25.0;
    double temp_heat_coef   = 0.00060;  // 温升系数（归一化功率平方）
    double temp_cool_coef   = 0.00200;  // 散热系数

    // ---- 量测 ----
    double noise_kw  = 0.0;             // 功率量测噪声标准差
    double noise_soc = 0.0;             // SOC 量测噪声标准差
    unsigned int seed = 20260912;
    // 关口电表的**系统偏差**（kW，进口为正方向）。
    //
    // 为什么需要这个字段（2026-09-19，缺口 A2 的断言前提）：
    //   真实系统里关口电表是**唯一权威计量点**，而负荷/光伏/电池三路各有自己的
    //   误差与不同时延 —— 用这三路相减去"推算"关口功率，误差是**叠加**而不是
    //   抵消的。所以 EMS 必须读电表，不能自己算。
    //   但在这之前，设备侧发布的 `MEAS.P_GRID` **恰好等于**那个相减式，
    //   于是"改成读电表"这件事在夹具上**逐位恒等** —— 断言杀不死它。
    //   加了这个偏差项，才能造出「电表读数 ≠ 功率平衡」的夹具：
    //   只有真去读电表，算出来的关口功率才跟着变；继续推算，就会差整整 bias。
    // 默认 0.0 → 既有行为**逐位不变**（07/T21~T24、11/T41~T49 全部不受影响）。
    double meter_bias_kw = 0.0;

    // ---- 故障注入 ----
    bool comm_ok_bms   = true;          // BMS 通信
    bool comm_ok_meter = true;          // 关口电表通信
    bool comm_ok_pcs   = true;          // PCS 通信
    bool pcs_fault     = false;         // PCS 故障（停止出力）
    bool device_offline = false;        // 设备离线
    bool data_valid    = true;          // 数据有效性
};

class PlantModel {
public:
    PlantModel() { reset(); }
    explicit PlantModel(const PlantConfig& cfg) : cfg_(cfg) { reset(); }

    void set_config(const PlantConfig& cfg) { cfg_ = cfg; reset(); }
    const PlantConfig& config() const { return cfg_; }

    void reset() {
        soc_ = cfg_.soc_init;
        p_bat_actual_ = 0.0;
        p_bat_target_ = 0.0;
        p_load_kw_ = 0.0;
        p_pv_kw_ = 0.0;
        temperature_c_ = cfg_.temp_ambient_c;
        deadtime_line_.clear();
        rng_.seed(cfg_.seed);
        total_energy_charge_kwh_ = 0.0;
        total_energy_discharge_kwh_ = 0.0;
    }

    // ---- 环境（负荷 / 光伏）----
    void set_environment(double p_load_kw, double p_pv_kw) {
        p_load_kw_ = p_load_kw;
        p_pv_kw_   = p_pv_kw;
    }

    // ---- 执行一拍 ----
    // p_cmd_kw : EMS 下发的 P_bat 指令（放电为正）
    // dt_s     : 控制周期
    // 返回      : 本拍实际 P_bat
    double step(double p_cmd_kw, double dt_s) {
        if (dt_s <= 0.0) dt_s = 0.1;
        last_dt_ = dt_s;

        // PCS 故障 / 离线 / 通信丢失 → 立即零出力（真实 PCS 的 fail-safe）
        if (cfg_.pcs_fault || cfg_.device_offline || !cfg_.comm_ok_pcs) {
            p_bat_target_ = 0.0;
            p_bat_actual_ = 0.0;
            update_thermal(0.0, dt_s);
            return 0.0;
        }

        // 1) 指令限幅到 PCS 能力
        double cmd = clamp(p_cmd_kw, -cfg_.pcs_max_chg_kw, cfg_.pcs_max_dis_kw);

        // 2) 执行死区：指令先入延迟队列，队满后才开始有输出
        int delay_steps = static_cast<int>(std::ceil(cfg_.pcs_deadtime_s / dt_s));
        if (delay_steps <= 0) {
            p_bat_target_ = cmd;
        } else {
            deadtime_line_.push_back(cmd);
            if (static_cast<int>(deadtime_line_.size()) > delay_steps) {
                p_bat_target_ = deadtime_line_.front();
                deadtime_line_.pop_front();
            } else {
                p_bat_target_ = 0.0;   // 纯延迟阶段，尚无输出
            }
        }

        // 3) 物理变化率限幅
        double dmax = cfg_.pcs_ramp_kw_per_s * dt_s;
        double ramped = clamp(p_bat_target_, p_bat_actual_ - dmax, p_bat_actual_ + dmax);

        // 5) 一阶惯性（离散化：alpha = 1 - exp(-dt/tau)）
        double tau = std::max(1e-6, cfg_.pcs_tau_s);
        double alpha = 1.0 - std::exp(-dt_s / tau);
        p_bat_actual_ += (ramped - p_bat_actual_) * alpha;

        // 6) 物理能力再限一次
        p_bat_actual_ = clamp(p_bat_actual_, -cfg_.pcs_max_chg_kw, cfg_.pcs_max_dis_kw);

        // 7) SOC 演化
        update_soc(p_bat_actual_, dt_s);

        // 8) 热模型
        update_thermal(p_bat_actual_, dt_s);

        return p_bat_actual_;
    }

    // ---- 采集层：产出实时快照（评估期间冻结）----
    // 注：PCS 自耗/站用电 pcs_standby_kw 计入**负荷侧**（p_load），
    //     不从 P_bat 里扣 —— 否则会引入一个 EMS 无法补偿的稳态跟踪偏差。
    RealtimeSnapshot sample(Timestamp now_s) const {
        RealtimeSnapshot rt;
        rt.timestamp    = now_s;
        rt.p_bat_actual_kw = p_bat_actual_;
        rt.p_pv_kw      = p_pv_kw_ + noise(cfg_.noise_kw);
        rt.p_load_kw    = std::max(0.0, p_load_kw_ + cfg_.pcs_standby_kw + noise(cfg_.noise_kw));
        // 关口功率 = **电表读数**（不是"自己拿三路量测相减算出来的数"）。
        //
        // 2026-09-19（缺口 A2）：以前这里直接写 `rt.p_load_kw - rt.p_pv_kw - p_bat_actual_`，
        // 而 read_actuals() 走的是 p_grid_actual() —— 同一路数据两个口径。
        // 现在两个入口共用**同一个电表模型**（见下方 meter_p_grid()）：
        // 量测路径带本拍噪声（电表测的就是这个带噪声的物理量），真值路径用无噪声值，
        // 但**计量口径只有一处定义**，且都含电表系统偏差。
        rt.p_grid_kw    = rt.p_load_kw - rt.p_pv_kw - p_bat_actual_ + cfg_.meter_bias_kw;
        rt.soc          = clamp(soc_ + noise(cfg_.noise_soc), 0.0, 1.0);
        rt.temperature_c = temperature_c_;
        rt.soh          = cfg_.soh;
        rt.meters_alive["BMS"]   = cfg_.comm_ok_bms   && !cfg_.device_offline;
        rt.meters_alive["METER"] = cfg_.comm_ok_meter && !cfg_.device_offline;
        rt.meters_alive["PCS"]   = cfg_.comm_ok_pcs   && !cfg_.device_offline;
        return rt;
    }

    // 关口电表模型（**关口功率的唯一定义**）。
    //
    // 为什么单独抽一个函数：本项目踩过的"同一路数据两个口径"正是从
    // "同一个式子在两处各写一遍"开始的 —— 改一处忘一处，且没有编译错误。
    // 现在 sample()（EMS 看到的量测）与 p_grid_actual()（真值）都从电表口径出，
    // 差别只在"是否含本拍噪声"，不再有"谁在推算"的分歧。
    double meter_p_grid() const {
        return (p_load_kw_ + cfg_.pcs_standby_kw) - p_pv_kw_ - p_bat_actual_
               + cfg_.meter_bias_kw;
    }

    // ---- 访问器 ----
    double soc() const { return soc_; }
    double temperature_c() const { return temperature_c_; }
    double p_bat_actual() const { return p_bat_actual_; }
    double p_grid_actual() const { return meter_p_grid(); }
    double p_load() const { return p_load_kw_; }
    double p_pv() const { return p_pv_kw_; }
    double energy_charged_kwh() const { return total_energy_charge_kwh_; }
    double energy_discharged_kwh() const { return total_energy_discharge_kwh_; }

    // ---- 故障注入接口 ----
    void set_comm_bms(bool ok)   { cfg_.comm_ok_bms = ok; }
    void set_comm_meter(bool ok) { cfg_.comm_ok_meter = ok; }
    void set_comm_pcs(bool ok)   { cfg_.comm_ok_pcs = ok; }
    void set_pcs_fault(bool f)   { cfg_.pcs_fault = f; }
    void set_device_offline(bool f) { cfg_.device_offline = f; }
    void set_data_valid(bool v)  { cfg_.data_valid = v; }
    void set_temp_ambient(double c) { cfg_.temp_ambient_c = c; }
    void force_soc(double s)     { soc_ = clamp(s, 0.0, 1.0); }
    void force_temperature(double c) { temperature_c_ = c; }

private:
    static double clamp(double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    double noise(double sigma) const {
        if (sigma <= 0.0) return 0.0;
        std::normal_distribution<double> d(0.0, sigma);
        return d(rng_);
    }

    void update_soc(double p_bat_kw, double dt_s) {
        double cap = std::max(1e-6, cfg_.battery_capacity_kwh);
        double d_energy_kwh = 0.0;
        if (p_bat_kw > 0.0) {
            // 放电：SOC 下降，多消耗 P/eta
            d_energy_kwh = -p_bat_kw * dt_s / 3600.0 / std::max(1e-6, cfg_.eta_dis);
            total_energy_discharge_kwh_ += p_bat_kw * dt_s / 3600.0;
        } else if (p_bat_kw < 0.0) {
            // 充电：SOC 上升，实际存入 P*eta
            d_energy_kwh = -p_bat_kw * dt_s / 3600.0 * cfg_.eta_chg;
            total_energy_charge_kwh_ += (-p_bat_kw) * dt_s / 3600.0;
        }
        soc_ = clamp(soc_ + d_energy_kwh / cap,
                     cfg_.soc_phys_min, cfg_.soc_phys_max);
    }

    void update_thermal(double p_bat_kw, double dt_s) {
        double pn = std::fabs(p_bat_kw) / std::max(1.0, cfg_.pcs_max_dis_kw);
        double heat = cfg_.temp_heat_coef * pn * pn;                       // 焦耳热
        double cool = cfg_.temp_cool_coef * (temperature_c_ - cfg_.temp_ambient_c);
        temperature_c_ += (heat - cool) * dt_s * 60.0;                     // 分钟尺度
        temperature_c_ = std::max(cfg_.temp_ambient_c, temperature_c_);
    }

    PlantConfig cfg_{};
    double soc_ = 0.5;
    double p_bat_actual_ = 0.0;
    double p_bat_target_ = 0.0;
    double p_load_kw_ = 0.0;
    double p_pv_kw_ = 0.0;
    double temperature_c_ = 25.0;
    double last_dt_ = 0.1;
    double total_energy_charge_kwh_ = 0.0;
    double total_energy_discharge_kwh_ = 0.0;
    std::deque<double> deadtime_line_;
    mutable std::mt19937 rng_;
};

} // namespace ems
