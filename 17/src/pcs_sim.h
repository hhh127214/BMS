// =====================================================================
// 17/ — PCS 高保真模拟器（B3 的设备侧执行体）
//
// 缺口原文（§10.2 B3：Python 模拟器不存在；§6.1 模拟器矩阵）：
//   > **PCS** | 现在有什么：3 点（P_BAT / 通信 / 故障）+ 完整的物理积分
//   > 目标：全点表（有功无功/模式/状态字/故障码/三相电量）
//
// 本文件保留 11/ 已有的物理口径（**一阶惯性 τ**、变化率上限），并把点位扩到全表：
//
//   有功/无功     一阶惯性跟踪指令 + 变化率限幅；无功另受视在功率容量约束
//   运行模式      STANDBY / GRID_TIED / OFF_GRID / FAULT 四态状态机
//   直流侧        Udc 随出力下垂（负载越重母线越低）+ 纹波；Idc = P/Udc
//   交流侧        三相电压/电流/频率（含纹波）
//   故障码        可注入、**可恢复**（保护动作与恢复都要能测）
//   日/累计发电量 按功率积分；运行小时按非待机时长累计
//
// ---------------------------------------------------------------------
// ★ "一阶惯性 τ" 的口径必须与 11/ 一致，否则两处模拟器给出的
//   跟踪曲线形状不同，而"哪一条才是被测系统的真实行为"就说不清了。
//   这里用与 `MemoryDeviceIO::execute()` 同一式：
//       p ← p + (ref − p) · min(1, dt/τ)
//   并保留 `ramp_kw_per_s` 的变化率限幅（两者串联，先惯性后限幅）。
//
// ★ 为什么 mode 用 0..4 的**模拟量**而不是四个数字量位：
//   真机的"运行状态字"往往是位图，但最常被 EMS 直接消费的是**模式码**
//   （待机/并网/离网/故障）。用模拟量表达模式码，与 40 点表把
//   STA.PCS_FAULT 单独拎出来的做法互补：一个给"现在是什么态"，
//   一个给"有没有故障"。
//
// 编译：纯头文件（inline）。
// =====================================================================

#pragma once

#include "device_point_table.h"
#include "point_sink.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace ems {
namespace devsim {

inline std::string dpp(const int index) { return std::string(devpt::dev_point_name(index)); }

struct PcsSimConfig {
    double rated_p_kw      = 200.0;   // 额定有功
    double rated_q_kvar    = 200.0;   // 额定无功
    double tau_s           = 0.5;     // 一阶惯性（与 11/ 同口径）
    double ramp_kw_per_s   = 400.0;   // 变化率上限
    double standby_kw      = 2.0;     // 空载损耗（计入负荷侧，不改变 P_ACT）
    double eta             = 0.97;    // 交直流效率
    double dc_bus_v        = 750.0;   // 直流母线额定
    double dc_v_sag_per_kw = 0.30;    // 出力越大母线越低（V/kW）
    double ac_u_nominal    = 230.0;   // 相电压额定
    double pf_min          = 0.90;    // 最小功率因数（限制无功）
    double freq_hz         = 50.0;
    bool   grid_present    = true;    // 电网可用
    bool   comm_ok         = true;
    int    fault_code      = 0;       // 注入故障码（0 = 无故障）
};

class PcsSim {
public:
    enum Mode { kStandby = 0, kGridTied = 1, kOffGrid = 2, kFault = 3 };

    explicit PcsSim(const PcsSimConfig& c = PcsSimConfig()) : cfg_(c) {}

    // ---- 静态配置 ----
    void publish_static(PointSink& sink) const {
        sink.set(dpp(devpt::DP_PCS_TAU_S),      cfg_.tau_s);
        sink.set(dpp(devpt::DP_PCS_RAMP),       cfg_.ramp_kw_per_s);
        sink.set(dpp(devpt::DP_PCS_STANDBY),    cfg_.standby_kw);
        sink.set(dpp(devpt::DP_PCS_RATED_Q_KVAR), cfg_.rated_q_kvar);
        sink.set(dpp(devpt::DP_PCS_MAX_CHG_KW), cfg_.rated_p_kw);
        sink.set(dpp(devpt::DP_PCS_MAX_DIS_KW), cfg_.rated_p_kw);
    }

    // =================================================================
    // 一拍。返回**实际**交流侧有功（放电为正）—— 与 11/ 的 execute() 同语义。
    // =================================================================
    double step(double t, double dt, PointSink& sink,
                double cmd_p_kw, double cmd_q_kvar, bool onoff) {
        // ① 模式状态机
        Mode mode;
        if (cfg_.fault_code != 0)        mode = kFault;
        else if (!cfg_.grid_present)     mode = kOffGrid;
        else if (!onoff)                 mode = kStandby;
        else                             mode = kGridTied;

        // ② 指令整形（越额定 → 限幅并记录原因）
        int limit_reason = 0;
        double ref_p = cmd_p_kw;
        double ref_q = cmd_q_kvar;
        if (std::fabs(ref_p) > cfg_.rated_p_kw) {
            ref_p = (ref_p > 0.0 ? cfg_.rated_p_kw : -cfg_.rated_p_kw);
            limit_reason = 3;                       // 指令越额定
        }
        if (std::fabs(ref_q) > cfg_.rated_q_kvar) {
            ref_q = (ref_q > 0.0 ? cfg_.rated_q_kvar : -cfg_.rated_q_kvar);
            limit_reason = 3;
        }
        if (mode == kFault) {
            // 故障：撤出力（不是"立刻变 0" —— 真机也是按惯性滑到 0）
            ref_p = 0.0; ref_q = 0.0;
            limit_reason = 4;                       // 故障闭锁
        } else if (mode == kOffGrid) {
            ref_p = 0.0; ref_q = 0.0;
            limit_reason = 5;                       // 电网不可用
        } else if (mode == kStandby) {
            // ★ 待机也必须撤出力。漏了这两行的话，待机态会**继续跟踪指令**——
            //   现象是"停机命令发了、模式也变成 STANDBY 了，但功率还是满的"，
            //   而模式位看起来完全正常（最能骗过"只看状态字"的测试）。
            ref_p = 0.0; ref_q = 0.0;
            limit_reason = 6;                       // 停机态无出力
        }

        // ③ 一阶惯性（与 11/ 同式）
        const double alpha = (cfg_.tau_s > 1e-9) ? std::min(1.0, dt / cfg_.tau_s) : 1.0;
        p_ += (ref_p - p_) * alpha;
        q_ += (ref_q - q_) * alpha;

        // ④ 变化率限幅（先惯性后限幅）
        const double step_max = std::fabs(cfg_.ramp_kw_per_s) * dt;
        if (p_ - p_prev_ > step_max)       p_ = p_prev_ + step_max;
        else if (p_prev_ - p_ > step_max)  p_ = p_prev_ - step_max;
        if (q_ - q_prev_ > step_max)       q_ = q_prev_ + step_max;
        else if (q_prev_ - q_ > step_max)  q_ = q_prev_ - step_max;

        // ⑤ 视在功率容量约束（无功受功率因数下限限制）
        const double s_max = cfg_.rated_p_kw;                 // 以额定有功为视在容量上限
        const double q_cap = std::sqrt(std::fmax(s_max * s_max - p_ * p_, 0.0));
        // ★ |p| 很小时**不能**用 PF 去卡无功：P=0 时 PF 无定义，而真机在这个
        //   工况下恰恰是"纯无功"（STATCOM 模式，q 可以给到额定无功）。
        //   如果用 |p|·tanφ 去卡，P=0 会把 q 压成 0 —— 无功补偿永远动不了，
        //   而现象是"算法发无功没反应"，查起来会先怀疑算法。
        const double q_pf_cap = (std::fabs(p_) > 1.0)
                                    ? std::fabs(p_) * std::tan(std::acos(clamp(cfg_.pf_min, 0.1, 1.0)))
                                    : cfg_.rated_q_kvar;
        const double q_lim = std::min(q_cap, q_pf_cap);
        if (std::fabs(q_) > q_lim) {
            q_ = (q_ > 0.0 ? q_lim : -q_lim);
            if (limit_reason == 0) limit_reason = 2;
        }

        // ⑥ 电气量
        const double s_act = std::sqrt(p_ * p_ + q_ * q_);
        const double pf = (s_act > 1e-9) ? (p_ / s_act) : 1.0;
        const double udc = cfg_.dc_bus_v - cfg_.dc_v_sag_per_kw * std::fabs(p_) +
                           2.0 * std::sin(2.0 * kPi * t / 3.0);
        const double idc = (udc > 1e-6) ? (p_ * 1000.0 / udc) : 0.0;
        const double ripple_u = 0.004 * std::sin(2.0 * kPi * t / 2.5);
        const double u[3] = {cfg_.ac_u_nominal * (1.0 + ripple_u),
                             cfg_.ac_u_nominal * (1.0 + 0.6 * ripple_u),
                             cfg_.ac_u_nominal * (1.0 - 0.4 * ripple_u)};
        double i[3];
        for (int k = 0; k < 3; ++k) {
            const double uu = std::fmax(u[k], 1.0);
            i[k] = std::fabs(s_act) * 1000.0 / (3.0 * uu);
        }
        const double freq = cfg_.freq_hz + 0.010 * std::sin(2.0 * kPi * t / 5.5);

        // ⑦ 电量 / 运行小时（**按功率积分**）
        const double de = dt / 3600.0;
        e_day_    += std::fmax(p_, 0.0) * de;
        e_total_  += std::fmax(p_, 0.0) * de;
        e_chg_kwh_ += std::fmax(-p_, 0.0) * de;
        if (mode != kStandby) run_hours_ += de;

        // ⑧ 告警
        const int alm_level = (cfg_.fault_code != 0) ? 3 : ((limit_reason == 2) ? 1 : 0);
        const int alm_code  = (cfg_.fault_code != 0) ? cfg_.fault_code : 0;

        // ⑨ 发布
        sink.set(dpp(devpt::DP_PCS_P_ACT), p_);
        sink.set(dpp(devpt::DP_PCS_Q_ACT), q_);
        sink.set(dpp(devpt::DP_PCS_S_ACT), s_act);
        sink.set(dpp(devpt::DP_PCS_PF), pf);
        sink.set(dpp(devpt::DP_PCS_UDC), udc);
        sink.set(dpp(devpt::DP_PCS_IDC), idc);
        sink.set(dpp(devpt::DP_PCS_U_A), u[0]);
        sink.set(dpp(devpt::DP_PCS_U_B), u[1]);
        sink.set(dpp(devpt::DP_PCS_U_C), u[2]);
        sink.set(dpp(devpt::DP_PCS_I_A), i[0]);
        sink.set(dpp(devpt::DP_PCS_I_B), i[1]);
        sink.set(dpp(devpt::DP_PCS_I_C), i[2]);
        sink.set(dpp(devpt::DP_PCS_FREQ), freq);
        sink.set(dpp(devpt::DP_PCS_E_DAY_KWH), e_day_);
        sink.set(dpp(devpt::DP_PCS_E_TOTAL_KWH), e_total_);
        sink.set(dpp(devpt::DP_PCS_RUN_HOURS), run_hours_);
        sink.set(dpp(devpt::DP_PCS_MODE), static_cast<double>(mode));
        sink.set(dpp(devpt::DP_PCS_FAULT), (cfg_.fault_code != 0) ? 1.0 : 0.0);
        sink.set(dpp(devpt::DP_PCS_COMM_OK), cfg_.comm_ok ? 1.0 : 0.0);
        sink.set(dpp(devpt::DP_PCS_DATA_VALID),
                 (cfg_.comm_ok && cfg_.fault_code == 0) ? 1.0 : 0.0);
        sink.set(dpp(devpt::DP_PCS_LIMIT_REASON), static_cast<double>(limit_reason));
        sink.set(dpp(devpt::DP_PCS_ALM_LEVEL), static_cast<double>(alm_level));
        sink.set(dpp(devpt::DP_PCS_FAULT_CODE), static_cast<double>(alm_code));

        p_prev_ = p_;
        q_prev_ = q_;
        mode_   = mode;
        ++ticks_;
        return p_;
    }

    // ---- 访问器 ----
    double p_actual() const { return p_; }
    double q_actual() const { return q_; }
    int    mode() const { return static_cast<int>(mode_); }
    double e_day_kwh() const { return e_day_; }
    double e_total_kwh() const { return e_total_; }
    double e_chg_kwh() const { return e_chg_kwh_; }
    double run_hours() const { return run_hours_; }
    long   ticks() const { return ticks_; }
    PcsSimConfig& mutable_cfg() { return cfg_; }
    const PcsSimConfig& cfg() const { return cfg_; }

    static const char* mode_name(int m) {
        switch (m) {
            case kStandby:  return "STANDBY";
            case kGridTied: return "GRID_TIED";
            case kOffGrid:  return "OFF_GRID";
            case kFault:    return "FAULT";
            default:        return "?";
        }
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    static double clamp(double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    PcsSimConfig cfg_;
    double p_ = 0.0, q_ = 0.0;               // 本拍
    double p_prev_ = 0.0, q_prev_ = 0.0;     // 上一拍（变化率限幅用）
    double e_day_ = 0.0, e_total_ = 0.0, e_chg_kwh_ = 0.0, run_hours_ = 0.0;
    Mode   mode_ = kStandby;
    long   ticks_ = 0;
};

} // namespace devsim
} // namespace ems
