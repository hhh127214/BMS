#ifndef SMOOTHING_CONTROLLER_H
#define SMOOTHING_CONTROLLER_H

#include <cmath>
#include <algorithm>
#include "clamper.h"   // resolved via -I 03/shared in build scripts

class SmoothingController {
public:
    struct Config {
        double tau;          // 滤波时间常数（秒）
        double Ts;           // 控制周期（秒）
        double SOC_low;      // 正常工作区下限（%）
        double SOC_high;     // 正常工作区上限（%）
        double SOC_min;      // 绝对下限（%）
        double SOC_max;      // 绝对上限（%）

        Config()
            : tau(60.0),
              Ts(0.1),
              SOC_low(20.0),
              SOC_high(80.0),
              SOC_min(10.0),
              SOC_max(90.0) {}
    };

    explicit SmoothingController(const Config& cfg);
    ~SmoothingController() = default;

    /**
     * @brief 更新平抑控制器
     * @param P_pv            光伏实时功率（kW）
     * @param SOC             电池SOC（%）
     * @param P_chg_max       最大允许充电功率（kW）
     * @param P_dis_max       最大允许放电功率（kW）
     * @param charge_enabled   是否允许充电
     * @param discharge_enabled 是否允许放电
     * @param smoothing_enabled 平抑功能是否启用
     * @param data_valid       光伏数据有效性（通信正常为true）
     * @return 补偿功率需求（kW），正为放电，负为充电
     */
    double update(double P_pv, double SOC,
                  double P_chg_max, double P_dis_max,
                  bool charge_enabled, bool discharge_enabled,
                  bool smoothing_enabled, bool data_valid);

    void reset();

    // 诊断接口
    double getLastOutput() const { return last_output_; }
    double getSmoothValue() const { return P_smooth_prev_; }
    double getAlpha() const { return alpha_; }

private:
    Config cfg_;
    double alpha_;
    double P_smooth_prev_;
    double last_output_;
    bool first_update_;
    // clamp() 已迁移至 ../shared/clamper.h 的 bms::clamp。
};

#endif // SMOOTHING_CONTROLLER_H