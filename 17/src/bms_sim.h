// =====================================================================
// 17/ — BMS 高保真模拟器（B5）
//
// 缺口原文（§10.2 B5）：
//   > **BMS 模拟器只有 6 点** —— `MEAS.SOC / T_C / SOH` + 通信位
//
// 本文件把它扩到全点表口径，并且**每个量都由物理过程推出来**，不是各自一个 sin()：
//
//   簇级      簇电压 = Σ单体电压；簇电流 = 簇功率 / 簇电压；簇温度 = 单体温度均值
//   单体级    单体电压 = OCV(SOC) + 单体偏置 + 内阻压降(I·R) + 微纹波
//             单体温度由**热模型**积分：dT/dt = k_heat·I² − k_cool·(T − T_amb)
//   能量      SOC 按**功率积分**（含充放效率），SOH 按**累计吞吐量**衰减
//   允许/禁止 允许充放功率随 **温度与 SOC** 降额；禁充放位是降额到 0 的**结果**，
//             不是独立的开关 —— 这样"限值"和"禁位"在物理上必然自洽
//   绝缘/漏电 绝缘电阻随温度与注入因子变化；漏电流随温度上升
//   告警      level + code 由温度/压差/绝缘的阈值共同决定
//
// ---------------------------------------------------------------------
// ★ 为什么"禁充放位"必须与"允许功率"同源：
//   A1 的教训是这两位是 L0 最底层那道锁（05/ 折进 p_lower/p_upper）。
//   如果模拟器把它们做成两个独立的布尔开关，那么"T=50°C 时功率限值掉到 0"
//   与"T=50°C 时禁位还是 0"可以同时成立 —— 于是 05/ 的保护逻辑**永远不会
//   在温度维度上被触发**，测试全绿而现场会出事。同源以后，
//   "温度越限 → 限值 0 → 禁位置 1"是一条因果链，可以被单条断言钉住。
//
// ★ 单位口径：CFG.BMS.SOC_MIN/MAX 用 **比例**（"-"，0..1），
//   MEAS.BMS.SOC/SOH 用 **百分数**（"%"）。这是刻意的 ——
//   它与 40 点表抽象契约逐字对齐（CFG.SOC_PHYS_MIN 单位是 "-"），
//   self_check 的"单位一致"判据正是靠这条抓"比例/百分数混填"。
//
// 编译：纯头文件（inline）。
// =====================================================================

#pragma once

#include "device_point_table.h"
#include "point_sink.h"

#include <cmath>
#include <string>
#include <vector>

namespace ems {
namespace devsim {

inline std::string dpb(const int index) { return std::string(devpt::dev_point_name(index)); }

struct BmsSimConfig {
    // ---- 组串结构（真机：多簇并联，每簇若干单体串联）----
    // 默认 = 一条 224 串 × 280 Ah 的 750 V 簇（≈210 kWh），配 200 kW PCS ≈ 0.95C。
    // "多簇"由 clusters 配置项表达（见 tests/test_sim_bms.cpp T08）。
    int    clusters          = 1;
    int    cells_per_cluster = 224;    // 224 × 3.35 V ≈ 750 V 簇电压
    double cell_capacity_ah  = 280.0;
    // ★ 额定容量：**0 = 由组串结构推导**
    //   （clusters × cells × 3.35 V × 280 Ah / 1000 = 210.1 kWh）
    //   为什么默认推导而不是写死：写死会让"能量口径"与"单体口径"可以互相矛盾
    //   （比如 SOC 按 1000 kWh 积分，而单体电压按 210 kWh 的组串算），
    //   这种矛盾在数据上表现为"SOC 掉得比电压曲线该有的慢" —— 极难查。
    double cap_kwh           = 0.0;

    // ---- 初始状态 ----
    double soc0 = 0.50;
    double soh0 = 100.0;
    double cycle0 = 0.0;

    // ---- 效率 / 门限 ----
    double eta_chg = 0.95;
    double eta_dis = 0.95;
    double soc_min = 0.05;      // 比例
    double soc_max = 0.95;      // 比例

    // ---- 单体电气 ----
    double cell_v_nominal  = 3.35;
    double cell_r_int_ohm  = 0.00025;   // 0.25 mΩ
    double cell_v_spread_v = 0.030;     // 单体电压离散度（±）

    // ---- 热模型 ----
    // 标定：0.95C（267 A/簇）下稳态温升 ≈ 8 °C（k_heat·I² = k_cool·ΔT），
    //       时间常数 1/k_cool = 200 s。这是储能集装箱的典型量级。
    double temp_ambient_c  = 25.0;
    double heat_coef       = 5.6e-7;    // degC/s / A²
    double cool_coef       = 0.005;     // 1/s
    double temp_spread_c   = 1.5;       // 单体温度离散度（±）

    // ---- 绝缘 ----
    double insul_base_kohm = 2000.0;
    double leak_ma_base    = 2.0;

    // ---- 衰减 ----
    double soh_degrade_per_mwh = 20.0;  // %/MWh（吞吐量口径）

    // ---- 额定（用于允许功率的基准）----
    double p_rated_kw = 200.0;

    // ---- 注入（测试用；默认全部"不注入"）----
    double inject_temp_delta_c  = 0.0;  // 全体单体升温
    double inject_cell_v_delta_v = 0.0; // 0 号单体过压
    double inject_insul_scale   = 1.0;  // 绝缘下降（<1）
    bool   comm_ok = true;
    bool   bms_fault = false;           // BMS 自身故障（**不是**禁充放）
};

class BmsSim {
public:
    explicit BmsSim(const BmsSimConfig& c = BmsSimConfig()) : cfg_(c) {
        if (cfg_.clusters < 1) cfg_.clusters = 1;
        if (cfg_.cells_per_cluster < 1) cfg_.cells_per_cluster = 1;
        const int n_cells = cfg_.clusters * cfg_.cells_per_cluster;
        cell_v_.assign(static_cast<std::size_t>(n_cells), cfg_.cell_v_nominal);
        cell_t_.assign(static_cast<std::size_t>(n_cells), cfg_.temp_ambient_c);
        bias_v_.assign(static_cast<std::size_t>(n_cells), 0.0);
        bias_t_.assign(static_cast<std::size_t>(n_cells), 0.0);
        phase_.assign(static_cast<std::size_t>(n_cells), 0.0);
        // 单体离散度与纹波相位：**初始化一次**，逐拍复用 ——
        // 避免在热循环里调 sin()（224×4 单体 × 上万拍）。
        for (int i = 0; i < n_cells; ++i) {
            const double fi = static_cast<double>(i);
            bias_v_[static_cast<std::size_t>(i)] =
                cfg_.cell_v_spread_v * std::sin(fi * 1.7 + 0.3);
            bias_t_[static_cast<std::size_t>(i)] =
                cfg_.temp_spread_c * std::sin(fi * 2.3 + 1.1);
            phase_[static_cast<std::size_t>(i)] = fi * 0.37;
        }
        soc_ = cfg_.soc0;
        soh_ = cfg_.soh0;
        cycle_ = cfg_.cycle0;
        // ★ 允许功率的初值必须取"额定"，**不能**留 0：
        //   "还没算过"与"算出来是 0"在数值上不可区分，而 0 在语义上是
        //   **禁充 + 禁放** —— 与 A1 把禁充放位默认成 1 是同一个坑
        //   （一上电就锁死 [0,0]，冷启动不可用）。
        chg_limit_ = cfg_.p_rated_kw;
        dis_limit_ = cfg_.p_rated_kw;
    }

    // ---- 静态配置点 ----
    void publish_static(PointSink& sink) const {
        sink.set(dpb(devpt::DP_BMS_CAP_KWH),           cap_kwh_());
        sink.set(dpb(devpt::DP_BMS_SOC_MIN),           cfg_.soc_min);
        sink.set(dpb(devpt::DP_BMS_SOC_MAX),           cfg_.soc_max);
        sink.set(dpb(devpt::DP_BMS_ETA_CHG),           cfg_.eta_chg);
        sink.set(dpb(devpt::DP_BMS_ETA_DIS),           cfg_.eta_dis);
        sink.set(dpb(devpt::DP_BMS_CLUSTER_N),         static_cast<double>(cfg_.clusters));
        sink.set(dpb(devpt::DP_BMS_CELLS_PER_CLUSTER), static_cast<double>(cfg_.cells_per_cluster));
    }

    // =================================================================
    // 一拍。p_kw：PCS 实际功率（**放电为正**），单位 kW。
    // =================================================================
    void step(double t, double dt, PointSink& sink, double p_kw) {
        // ① SOC 按功率积分（含充放效率）
        const double e_kwh = p_kw * dt / 3600.0;      // 本拍交换的能量（>0 放电）
        const double cap   = cap_kwh_();
        if (e_kwh >= 0.0) {
            soc_ -= (cfg_.eta_dis > 1e-9) ? (e_kwh / cfg_.eta_dis / cap) : 0.0;
        } else {
            soc_ += (cfg_.eta_chg > 1e-9) ? ((-e_kwh) * cfg_.eta_chg / cap) : 0.0;
        }
        soc_ = clamp(soc_, 0.0, 1.0);

        // ② SOH / 循环次数按**累计吞吐量**衰减
        const double throughput_kwh = std::fabs(e_kwh);
        total_throughput_kwh_ += throughput_kwh;
        soh_   = std::max(0.0, cfg_.soh0 - cfg_.soh_degrade_per_mwh * total_throughput_kwh_ / 1000.0);
        cycle_ = total_throughput_kwh_ / (2.0 * std::fmax(cap, 1e-6));

        // ③ 单体电压 / 温度
        const double ocv_base = ocv_of_soc(soc_);
        const double p_per_cluster_kw = p_kw / static_cast<double>(cfg_.clusters);
        const double cluster_u_nominal = cfg_.cells_per_cluster * cfg_.cell_v_nominal;
        const double i_cluster = (cluster_u_nominal > 1e-6)
                                     ? (p_per_cluster_kw * 1000.0 / cluster_u_nominal) : 0.0;

        double v_max = -1e30, v_min = 1e30;
        double t_max = -1e30, t_min = 1e30;
        int v_max_i = 1, v_min_i = 1, t_max_i = 1;

        double cluster_u_sum = 0.0;   // 用于簇电压
        // ★ 每拍必须清空重填：per-cluster 向量是**本拍的快照**。
        //   漏了清空 → push_back 跨拍累积 → cluster_u(i) 越界读不报错，
        //   而是永远读到**第一拍**的旧值（看起来"簇电压恒定"，正是假联调）。
        cluster_u_.clear();
        cluster_i_.clear();
        cluster_t_.clear();
        for (int c = 0; c < cfg_.clusters; ++c) {
            double cu = 0.0, ct = 0.0;
            for (int k = 0; k < cfg_.cells_per_cluster; ++k) {
                const int i = c * cfg_.cells_per_cluster + k;
                const double ocv = ocv_base + bias_v_[static_cast<std::size_t>(i)];
                // 内阻压降：充电（i<0）抬高端电压，放电（i>0）拉低
                const double dv = i_cluster * cfg_.cell_r_int_ohm;
                const double ripple = 0.0015 * std::sin(t * 0.8 + phase_[static_cast<std::size_t>(i)]);
                double v = ocv + dv + ripple;
                if (i == 0) v += cfg_.inject_cell_v_delta_v;
                cell_v_[static_cast<std::size_t>(i)] = v;
                cu += v;

                // 热模型积分：**状态量只有核心温度**，离散度/纹波/注入都在
                // 观测侧叠加。若把纹波或注入直接加回状态，它们会按"每拍一次"
                // 累积（等价于一个随 dt 变化的热源）—— 同一份代码换个 dt
                // 就得到完全不同的温度曲线，这类 bug 在 dt=0.1 的测试里看不出来。
                double& Tcore = cell_t_[static_cast<std::size_t>(i)];
                const double heat = cfg_.heat_coef * i_cluster * i_cluster;
                const double cool = cfg_.cool_coef * (Tcore - cfg_.temp_ambient_c);
                Tcore += (heat - cool) * dt;
                const double T = Tcore + bias_t_[static_cast<std::size_t>(i)] +
                                 0.05 * std::sin(t * 0.5 + phase_[static_cast<std::size_t>(i)]) +
                                 cfg_.inject_temp_delta_c;
                ct += T;

                if (v > v_max) { v_max = v; v_max_i = i + 1; }
                if (v < v_min) { v_min = v; v_min_i = i + 1; }
                if (T > t_max) { t_max = T; t_max_i = i + 1; }
                if (T < t_min) t_min = T;
            }
            cluster_u_sum += cu;
            cluster_t_.push_back(ct / static_cast<double>(cfg_.cells_per_cluster));
            cluster_u_.push_back(cu);
            const double u_cl = std::fmax(cu, 1.0);
            cluster_i_.push_back(p_per_cluster_kw * 1000.0 / u_cl);
        }
        // 上一拍的 appends 会在下一拍累积 —— 每拍清空重填
        const double cluster_u_avg = cluster_u_sum / static_cast<double>(cfg_.clusters);

        const double v_diff = v_max - v_min;

        // ④ 绝缘电阻 / 漏电流
        const double insul = cfg_.insul_base_kohm * cfg_.inject_insul_scale *
                             (1.0 - 0.0004 * (t_max - 25.0)) +
                             20.0 * std::sin(2.0 * kPi * t / 67.0);
        const double leak = cfg_.leak_ma_base + 0.05 * t_max +
                            0.3 * std::sin(2.0 * kPi * t / 13.0);

        // ⑤ 允许充放功率（随温度 / SOC 降额）
        const double f_soc_chg = clamp((cfg_.soc_max - soc_) / 0.10, 0.0, 1.0);
        const double f_soc_dis = clamp((soc_ - cfg_.soc_min) / 0.10, 0.0, 1.0);
        const double f_temp    = clamp(1.0 - (t_max - 35.0) / 10.0, 0.0, 1.0);
        const double chg_limit = cfg_.p_rated_kw * f_soc_chg * f_temp;
        const double dis_limit = cfg_.p_rated_kw * f_soc_dis * f_temp;

        // ⑥ 禁充放位 = 降额到 0 的**结果**（与限值同源，见文件头说明）
        const bool chg_forbid = (chg_limit <= 1e-9);
        const bool dis_forbid = (dis_limit <= 1e-9);

        // ⑦ 告警等级 / 码
        int level = 0, code = 0;
        if (t_max > 55.0 || v_max > 3.65 || insul < 100.0) {
            level = 3;
            code  = (t_max > 55.0) ? 3 : ((v_max > 3.65) ? 4 : 5);
        } else if (t_max > 50.0 || v_diff > 0.5 || insul < 300.0) {
            level = 2;
            code  = (t_max > 50.0) ? 3 : ((v_diff > 0.5) ? 7 : 5);
        } else if (t_max > 40.0 || v_diff > 0.3) {
            level = 1;
            code  = (t_max > 40.0) ? 3 : 7;
        }
        int reason = 0;
        if (chg_forbid && soc_ >= cfg_.soc_max - 1e-9) reason = 1;
        else if (dis_forbid && soc_ <= cfg_.soc_min + 1e-9) reason = 2;
        else if (chg_forbid || dis_forbid) reason = 3;
        else if (v_max > 3.60) reason = 4;
        else if (insul < 300.0) reason = 5;

        // ⑧ 发布
        sink.set(dpb(devpt::DP_BMS_CLUSTER_U), cluster_u_avg);
        sink.set(dpb(devpt::DP_BMS_CLUSTER_I), cluster_i_.empty() ? 0.0 : cluster_i_[0]);
        sink.set(dpb(devpt::DP_BMS_CLUSTER_T),
                 cluster_t_.empty() ? cfg_.temp_ambient_c : cluster_t_[0]);
        sink.set(dpb(devpt::DP_BMS_CELL_V_MAX), v_max);
        sink.set(dpb(devpt::DP_BMS_CELL_V_MIN), v_min);
        sink.set(dpb(devpt::DP_BMS_CELL_V_MAX_IDX), static_cast<double>(v_max_i));
        sink.set(dpb(devpt::DP_BMS_CELL_V_MIN_IDX), static_cast<double>(v_min_i));
        sink.set(dpb(devpt::DP_BMS_CELL_V_DIFF), v_diff);
        sink.set(dpb(devpt::DP_BMS_T_MAX), t_max);
        sink.set(dpb(devpt::DP_BMS_T_MIN), t_min);
        sink.set(dpb(devpt::DP_BMS_T_MAX_IDX), static_cast<double>(t_max_i));
        sink.set(dpb(devpt::DP_BMS_SOC), soc_ * 100.0);
        sink.set(dpb(devpt::DP_BMS_SOH), soh_);
        sink.set(dpb(devpt::DP_BMS_RM_CHG_KWH), std::max(0.0, (cfg_.soc_max - soc_)) * cap);
        sink.set(dpb(devpt::DP_BMS_RM_DIS_KWH), std::max(0.0, (soc_ - cfg_.soc_min)) * cap);
        sink.set(dpb(devpt::DP_BMS_INSUL_R), std::max(0.0, insul));
        sink.set(dpb(devpt::DP_BMS_LEAK_I), std::max(0.0, leak));
        sink.set(dpb(devpt::DP_BMS_CYCLE_COUNT), cycle_);
        sink.set(dpb(devpt::DP_BMS_CHG_LIMIT_KW), chg_limit);
        sink.set(dpb(devpt::DP_BMS_DIS_LIMIT_KW), dis_limit);
        sink.set(dpb(devpt::DP_BMS_CHG_FORBID), chg_forbid ? 1.0 : 0.0);
        sink.set(dpb(devpt::DP_BMS_DIS_FORBID), dis_forbid ? 1.0 : 0.0);
        sink.set(dpb(devpt::DP_BMS_FORBID_REASON), static_cast<double>(reason));
        sink.set(dpb(devpt::DP_BMS_MAIN_POS), cfg_.bms_fault ? 0.0 : 1.0);
        sink.set(dpb(devpt::DP_BMS_MAIN_NEG), cfg_.bms_fault ? 0.0 : 1.0);
        sink.set(dpb(devpt::DP_BMS_CHARGING), (p_kw < -1.0) ? 1.0 : 0.0);
        sink.set(dpb(devpt::DP_BMS_COMM_OK), cfg_.comm_ok ? 1.0 : 0.0);
        sink.set(dpb(devpt::DP_BMS_HEARTBEAT), static_cast<double>(++heartbeat_));
        sink.set(dpb(devpt::DP_BMS_DATA_VALID),
                 (cfg_.comm_ok && !cfg_.bms_fault) ? 1.0 : 0.0);
        sink.set(dpb(devpt::DP_BMS_ALM_LEVEL), static_cast<double>(level));
        sink.set(dpb(devpt::DP_BMS_ALM_CODE), static_cast<double>(code));
        // 中间量（不发布，只留访问器给测试）
        insul_kohm_  = insul;
        t_max_c_     = t_max;
        v_max_v_     = v_max;
        v_diff_v_    = v_diff;
        dis_limit_   = dis_limit;
        chg_limit_   = chg_limit;
        ++ticks_;
    }

    // ---- 访问器 ----
    double soc() const { return soc_; }
    double soh() const { return soh_; }
    double cycle_count() const { return cycle_; }
    double insul_kohm() const { return insul_kohm_; }
    double t_max_c() const { return t_max_c_; }
    double v_max_v() const { return v_max_v_; }
    double v_diff_v() const { return v_diff_v_; }
    double chg_limit_kw() const { return chg_limit_; }
    double dis_limit_kw() const { return dis_limit_; }
    double throughput_kwh() const { return total_throughput_kwh_; }
    long   ticks() const { return ticks_; }
    int    cell_count() const { return static_cast<int>(cell_v_.size()); }

    double cell_voltage(int i) const {
        return (i >= 0 && i < static_cast<int>(cell_v_.size())) ? cell_v_[static_cast<std::size_t>(i)] : 0.0;
    }
    double cell_temp(int i) const {
        return (i >= 0 && i < static_cast<int>(cell_t_.size())) ? cell_t_[static_cast<std::size_t>(i)] : 0.0;
    }
    double cluster_u(int i) const {
        return (i >= 0 && i < static_cast<int>(cluster_u_.size())) ? cluster_u_[static_cast<std::size_t>(i)] : 0.0;
    }
    double cluster_i(int i) const {
        return (i >= 0 && i < static_cast<int>(cluster_i_.size())) ? cluster_i_[static_cast<std::size_t>(i)] : 0.0;
    }

    const BmsSimConfig& cfg() const { return cfg_; }
    BmsSimConfig& mutable_cfg() { return cfg_; }

    double cap_kwh_() const {
        // 由组串结构推出的额定容量；cfg_.cap_kwh 作为兜底（=0 时用推导值）
        const double derived = static_cast<double>(cfg_.clusters) *
                               static_cast<double>(cfg_.cells_per_cluster) *
                               cfg_.cell_v_nominal * cfg_.cell_capacity_ah / 1000.0;
        return (cfg_.cap_kwh > 1e-9) ? cfg_.cap_kwh : derived;
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    static double clamp(double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }
    // OCV(SOC) 分段线性（LFP 典型曲线）
    static double ocv_of_soc(double s) {
        static const double xs[] = {0.00, 0.05, 0.10, 0.30, 0.50, 0.70, 0.90, 0.95, 1.00};
        static const double ys[] = {3.00, 3.20, 3.25, 3.30, 3.32, 3.35, 3.40, 3.48, 3.55};
        const int n = 9;
        if (s <= xs[0]) return ys[0];
        if (s >= xs[n - 1]) return ys[n - 1];
        for (int i = 1; i < n; ++i) {
            if (s <= xs[i]) {
                const double f = (s - xs[i - 1]) / (xs[i] - xs[i - 1]);
                return ys[i - 1] + f * (ys[i] - ys[i - 1]);
            }
        }
        return ys[n - 1];
    }

    BmsSimConfig cfg_;

    std::vector<double> cell_v_, cell_t_, bias_v_, bias_t_, phase_;
    std::vector<double> cluster_u_, cluster_i_, cluster_t_;

    double soc_ = 0.5, soh_ = 100.0, cycle_ = 0.0;
    double total_throughput_kwh_ = 0.0;

    double insul_kohm_ = 0.0, t_max_c_ = 25.0, v_max_v_ = 3.35, v_diff_v_ = 0.0;
    double chg_limit_ = 0.0, dis_limit_ = 0.0;
    long   heartbeat_ = 0, ticks_ = 0;
};

} // namespace devsim
} // namespace ems
