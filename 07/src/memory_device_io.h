// =====================================================================
// 07/ — P0.5：进程内点表适配器 MemoryDeviceIO
//
// 目的：**证明 P0 的接口抽象真的成立**。
//   同一套 EmsRuntime 算法（一行不改），从 SimDeviceIO（PlantModel 物理模型）
//   换到本适配器（纯内存点表）后，指令序列必须完全一致 —— 这就是
//   "换数据源不改算法" 的可执行证明，而不是一句设计口号。
//
// 与 SimDeviceIO 的本质区别：
//   · 不依赖 PlantModel：没有死区队列、没有热模型、没有随机噪声发生器
//   · 内部是**点表**（std::unordered_map<点名, 值>）—— 这正是 RT_DB
//     "共享内存 + 数据点表" 的进程内等价物，为 P3 之后接 RT_DB 铺路
//   · 点名 ↔ 业务结构体的映射**只发生在本文件内部**。算法（05/06/07/08）
//     永远看不到 "MEAS.SOC" 这种点名 —— 这是 P0 的核心纪律。若哪天有人在
//     算法里写了点名，说明分层已经破了。
//
// 建模（刻意简化，够用即可；它是"设备替身"，不是"物理仿真"）：
//   · 执行：一阶惯性（CFG.TAU_S）+ 变化率限幅（CFG.PCS_RAMP_KW_PER_S）
//   · SOC ：按实际功率积分（含充放电效率），限幅到物理上下限
//   · 温度：恒定（CFG.TEMP_C）
//   · 环境：由外部脚本/表注入（set_environment）—— 真实系统里这些来自电表
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "device_io.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace ems {

// ---------------------------------------------------------------------
// 点名约定（仅本文件内部使用；真实项目里这里会换成 IEC 61850 引用名或
// 厂家点表 ID，例如 "BMS_01.SOC"、"PCS01.P_SET"）
// ---------------------------------------------------------------------
namespace mem_point {
    // 量测
    constexpr const char* kPLoad   = "MEAS.P_LOAD";    // kW
    constexpr const char* kPPv     = "MEAS.P_PV";      // kW
    constexpr const char* kPBat    = "MEAS.P_BAT";     // kW（放电为正）
    constexpr const char* kPGrid   = "MEAS.P_GRID";    // kW（进口为正）
    constexpr const char* kSoc     = "MEAS.SOC";       // 0..1
    constexpr const char* kTC      = "MEAS.T_C";       // °C
    constexpr const char* kSoh     = "MEAS.SOH";       // 0..1
    // 指令
    constexpr const char* kCmdPBat = "CMD.P_BAT";      // kW
    constexpr const char* kCmdUp   = "CMD.P_UPPER";    // kW
    constexpr const char* kCmdLo   = "CMD.P_LOWER";    // kW
    // 配置
    constexpr const char* kCapKwh    = "CFG.BAT_CAP_KWH";
    constexpr const char* kMaxChg    = "CFG.PCS_MAX_CHG";
    constexpr const char* kMaxDis    = "CFG.PCS_MAX_DIS";
    constexpr const char* kBmsChgLim = "CFG.BMS_CHG_LIM";
    constexpr const char* kBmsDisLim = "CFG.BMS_DIS_LIM";
    constexpr const char* kTrKva     = "CFG.TRANSFORMER_KVA";
    constexpr const char* kDTarget   = "CFG.D_TARGET";
    constexpr const char* kTauS      = "CFG.TAU_S";
    constexpr const char* kRampKwS   = "CFG.PCS_RAMP_KW_PER_S";
    constexpr const char* kStandby   = "CFG.PCS_STANDBY";
    constexpr const char* kEtaChg    = "CFG.ETA_CHG";
    constexpr const char* kEtaDis    = "CFG.ETA_DIS";
    constexpr const char* kSocMin    = "CFG.SOC_PHYS_MIN";
    constexpr const char* kSocMax    = "CFG.SOC_PHYS_MAX";
    // 状态
    constexpr const char* kStaBms    = "STA.BMS_COMM_OK";
    constexpr const char* kStaPcs    = "STA.PCS_COMM_OK";
    constexpr const char* kStaMeter  = "STA.METER_COMM_OK";
    constexpr const char* kStaFault  = "STA.PCS_FAULT";
    constexpr const char* kStaOff    = "STA.OFFLINE";
    constexpr const char* kStaValid  = "STA.DATA_VALID";
} // namespace mem_point

class MemoryDeviceIO : public IDeviceIO {
public:
    MemoryDeviceIO() { init_defaults(); }

    // =================================================================
    // 点表读写（RT_DB 的进程内等价物）
    // =================================================================
    bool has_point(const std::string& name) const {
        return points_.find(name) != points_.end();
    }
    // 读点：缺失点返回 0.0 并计一次 miss（便于发现点名拼写错误）
    double get(const std::string& name) const {
        auto it = points_.find(name);
        if (it == points_.end()) { ++miss_count_; return 0.0; }
        return it->second;
    }
    void set(const std::string& name, double v) { points_[name] = v; }

    const std::unordered_map<std::string, double>& points() const { return points_; }
    // 点表被"读到不存在的点名"的次数。非 0 说明有拼写错误，应在自检中报错。
    int miss_count() const { return miss_count_; }
    std::size_t point_count() const { return points_.size(); }

    // =================================================================
    // 环境 / 故障注入（仿真专属，非 IDeviceIO 契约）
    // =================================================================
    void set_environment(double p_load_kw, double p_pv_kw) {
        set(mem_point::kPLoad, p_load_kw);
        set(mem_point::kPPv,   p_pv_kw);
    }
    void set_pcs_fault(bool f)       { set(mem_point::kStaFault, f ? 1.0 : 0.0); }
    void set_comm_bms(bool ok)       { set(mem_point::kStaBms,   ok ? 1.0 : 0.0); }
    void set_comm_pcs(bool ok)       { set(mem_point::kStaPcs,   ok ? 1.0 : 0.0); }
    void set_comm_meter(bool ok)     { set(mem_point::kStaMeter, ok ? 1.0 : 0.0); }
    void set_device_offline(bool f)  { set(mem_point::kStaOff,   f ? 1.0 : 0.0); }
    void set_data_valid(bool v)      { set(mem_point::kStaValid, v ? 1.0 : 0.0); }
    void force_soc(double s)         { set(mem_point::kSoc, clamp(s, 0.0, 1.0)); }
    void force_temperature(double c) { set(mem_point::kTC, c); }

    // =================================================================
    // IDeviceIO 实现
    // =================================================================
    bool read_snapshot(Timestamp now, RealtimeSnapshot& out) override {
        // 语义与 PlantModel::sample() 对齐：站用电计入负荷侧，不改 P_bat
        const double p_bat  = get(mem_point::kPBat);
        const double p_pv   = get(mem_point::kPPv);
        const double p_load = std::max(0.0, get(mem_point::kPLoad) + get(mem_point::kStandby));

        out.timestamp       = now;
        out.p_bat_actual_kw = p_bat;
        out.p_pv_kw         = p_pv;
        out.p_load_kw       = p_load;
        out.p_grid_kw       = p_load - p_pv - p_bat;
        out.soc             = clamp(get(mem_point::kSoc), 0.0, 1.0);
        out.temperature_c   = get(mem_point::kTC);
        out.soh             = get(mem_point::kSoh);

        const bool offline = get(mem_point::kStaOff) > 0.5;
        out.meters_alive["BMS"]   = get(mem_point::kStaBms)   > 0.5 && !offline;
        out.meters_alive["METER"] = get(mem_point::kStaMeter) > 0.5 && !offline;
        out.meters_alive["PCS"]   = get(mem_point::kStaPcs)   > 0.5 && !offline;

        // 电价窗口：给独立使用时的保守默认（EmsRuntime 会用 forecast 覆盖）
        const double h = std::fmod(now, 86400.0) / 3600.0;
        const bool valley = (h < 8.0 || h >= 22.0);
        out.pricing.cur_tou_type  = valley ? TouType::kValley : TouType::kPeak;
        out.pricing.cur_tou_price = valley ? 0.30 : 0.90;

        return get(mem_point::kStaValid) > 0.5;
    }

    bool read_limits(DeviceLimits& out) override {
        out = DeviceLimits{};
        out.pcs_rated_chg_kw       = get(mem_point::kMaxChg);
        out.pcs_rated_dis_kw       = get(mem_point::kMaxDis);
        out.bms_chg_limit_kw       = get(mem_point::kBmsChgLim);
        out.bms_dis_limit_kw       = get(mem_point::kBmsDisLim);
        out.transformer_capacity_kw = get(mem_point::kTrKva);
        out.d_target_kw            = get(mem_point::kDTarget);
        out.bms_chg_forbidden      = false;
        out.bms_dis_forbidden      = false;
        return true;
    }

    DeviceStatus read_status() const override {
        DeviceStatus s;
        s.bms_comm_ok    = get(mem_point::kStaBms)   > 0.5;
        s.pcs_comm_ok    = get(mem_point::kStaPcs)   > 0.5;
        s.meter_comm_ok  = get(mem_point::kStaMeter) > 0.5;
        s.pcs_fault      = get(mem_point::kStaFault) > 0.5;
        s.device_offline = get(mem_point::kStaOff)   > 0.5;
        s.data_valid     = get(mem_point::kStaValid) > 0.5;
        return s;
    }

    DeviceActuals read_actuals() const override {
        DeviceActuals a;
        a.p_bat_kw      = get(mem_point::kPBat);
        a.p_grid_kw     = get(mem_point::kPGrid);
        a.p_load_kw     = get(mem_point::kPLoad);
        a.p_pv_kw       = get(mem_point::kPPv);
        a.soc           = get(mem_point::kSoc);
        a.temperature_c = get(mem_point::kTC);
        return a;
    }

    // 下发：写点表。真实 RT_DB 适配器在这里就是 rt_db_set_value()。
    bool write_command(const PowerCommand& cmd) override {
        set(mem_point::kCmdPBat, cmd.p_bat_cmd_kw);
        set(mem_point::kCmdUp,   cmd.p_upper);
        set(mem_point::kCmdLo,   cmd.p_lower);
        return true;
    }

    double execute(double p_cmd_kw, double dt_s) override {
        if (dt_s <= 0.0) dt_s = 0.1;

        const double max_chg = get(mem_point::kMaxChg);
        const double max_dis = get(mem_point::kMaxDis);

        // 故障 / 离线 / PCS 通信丢失 → fail-safe 零出力
        const bool fail = get(mem_point::kStaFault) > 0.5 ||
                          get(mem_point::kStaOff)   > 0.5 ||
                          get(mem_point::kStaPcs)   < 0.5;
        if (fail) {
            set(mem_point::kPBat, 0.0);
            update_grid(0.0);
            return 0.0;
        }

        double p = get(mem_point::kPBat);

        // ① 指令限幅到设备能力
        double cmd = clamp(p_cmd_kw, -max_chg, max_dis);
        // ② 变化率限幅
        double dmax = std::max(0.0, get(mem_point::kRampKwS)) * dt_s;
        double ramped = clamp(cmd, p - dmax, p + dmax);
        // ③ 一阶惯性
        double tau = std::max(1e-6, get(mem_point::kTauS));
        double alpha = 1.0 - std::exp(-dt_s / tau);
        p += (ramped - p) * alpha;
        // ④ 物理能力再限
        p = clamp(p, -max_chg, max_dis);
        set(mem_point::kPBat, p);

        // ⑤ SOC 演化
        update_soc(p, dt_s);
        // ⑥ 功率平衡
        update_grid(p);

        return p;
    }

    double battery_capacity_kwh() const override { return get(mem_point::kCapKwh); }

    const char* name() const override { return "MemoryDeviceIO(PointTable)"; }

private:
    static double clamp(double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    void update_grid(double p_bat) {
        set(mem_point::kPGrid,
            get(mem_point::kPLoad) + get(mem_point::kStandby)
            - get(mem_point::kPPv) - p_bat);
    }

    void update_soc(double p_bat_kw, double dt_s) {
        const double cap = std::max(1e-6, get(mem_point::kCapKwh));
        double d_energy_kwh = 0.0;
        if (p_bat_kw > 0.0) {
            d_energy_kwh = -p_bat_kw * dt_s / 3600.0 / std::max(1e-6, get(mem_point::kEtaDis));
        } else if (p_bat_kw < 0.0) {
            d_energy_kwh = -p_bat_kw * dt_s / 3600.0 * get(mem_point::kEtaChg);
        }
        set(mem_point::kSoc,
            clamp(get(mem_point::kSoc) + d_energy_kwh / cap,
                  get(mem_point::kSocMin), get(mem_point::kSocMax)));
    }

    // 点表初始化：与 DeviceLimits / PlantConfig 的默认值保持一致
    void init_defaults() {
        set(mem_point::kPLoad, 0.0);
        set(mem_point::kPPv,   0.0);
        set(mem_point::kPBat,  0.0);
        set(mem_point::kPGrid, 0.0);
        set(mem_point::kSoc,   0.5);
        set(mem_point::kTC,    25.0);
        set(mem_point::kSoh,   1.0);

        set(mem_point::kCmdPBat, 0.0);
        set(mem_point::kCmdUp,   0.0);
        set(mem_point::kCmdLo,   0.0);

        set(mem_point::kCapKwh,    1000.0);
        set(mem_point::kMaxChg,    200.0);
        set(mem_point::kMaxDis,    200.0);
        set(mem_point::kBmsChgLim, 200.0);
        set(mem_point::kBmsDisLim, 200.0);
        set(mem_point::kTrKva,     250.0);
        set(mem_point::kDTarget,   250.0);
        set(mem_point::kTauS,      0.0);
        set(mem_point::kRampKwS,   1e9);
        set(mem_point::kStandby,   2.0);
        set(mem_point::kEtaChg,    1.0);
        set(mem_point::kEtaDis,    1.0);
        set(mem_point::kSocMin,    0.05);
        set(mem_point::kSocMax,    0.95);

        set(mem_point::kStaBms,   1.0);
        set(mem_point::kStaPcs,   1.0);
        set(mem_point::kStaMeter, 1.0);
        set(mem_point::kStaFault, 0.0);
        set(mem_point::kStaOff,   0.0);
        set(mem_point::kStaValid, 1.0);
    }

    std::unordered_map<std::string, double> points_{};
    mutable int miss_count_ = 0;   // get() 是 const，缺失点计数需 mutable
};

} // namespace ems
