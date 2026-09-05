#ifndef DEMAND_CONTROLLER_H
#define DEMAND_CONTROLLER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include "clamper.h"   // resolved via -I 03/shared in build scripts

class DemandController {
public:
    struct Config {
        double T_window;      // 滑动窗口时长（秒），典型900
        double D_target;      // 目标需量上限（kW）
        double dP_max;        // 最大功率变化率（kW/s）
        double Kp_avg;        // 窗口平均误差比例增益
        double Ki;            // 窗口平均误差积分增益
        double integral_max;  // 积分项上限（放电单向，下界为0）
        double Ts;            // 控制周期（秒），典型0.1

        Config()
            : T_window(900.0),
              D_target(250.0),
              dP_max(50.0),
              Kp_avg(100.0),
              Ki(20.0),
              integral_max(5000.0),
              Ts(0.1) {}
    };

    explicit DemandController(const Config& cfg);

    /// 校验配置合法性（T_window>0、Ts>0、D_target>0、dP_max>=0、Kp_avg>0、Ki>0、integral_max>=0）
    static bool isValidConfig(const Config& cfg);

    /**
     * @brief 需量管理控制器更新函数（滑动窗口算法）
     * @param P_grid 关口实时功率（kW），正=电网向用户供电（已含上一拍放电指令的效果）
     * @param P_dis_max 最大允许放电功率（kW），<=0 时输出0
     * @param discharge_enabled 放电使能，false 时输出0
     * @param comm_fault 关口电表通信异常，true 时输出0并置告警
     * @return 储能放电功率需求（正值）kW
     */
    double update(double P_grid, double P_dis_max,
                  bool discharge_enabled, bool comm_fault);

    /// 重置控制器全部内部状态（含滑动窗口缓冲）
    void reset();

    double getLastOutput() const { return last_output_; }
    double getWindowAverage() const { return window_avg_; }
    bool isActive() const { return active_; }
    bool hasAlarm() const { return alarm_; }

private:
    Config cfg_;
    size_t window_size_;        // N = round(T_window/Ts)，滑动窗口样本数
    std::deque<double> window_; // 最近N个P_grid样本（滚动缓存）

    double window_avg_;         // 最近一次计算出的窗口平均功率（kW）
    double integral_;           // 平均误差积分项（放电单向，>=0）
    double last_output_;
    bool active_;
    bool alarm_;
    // clamp() 已迁移至 ../shared/clamper.h 的 bms::clamp。
};

#endif // DEMAND_CONTROLLER_H
