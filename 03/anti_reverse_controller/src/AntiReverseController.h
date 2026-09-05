#ifndef ANTI_REVERSE_CONTROLLER_H
#define ANTI_REVERSE_CONTROLLER_H

#include <cmath>
#include "clamper.h"   // resolved via -I 03/shared in build scripts

// 控制器配置结构体
struct AntiReverseConfig
{
    double Kp;              // 比例系数
    double Ki;              // 积分系数
    double Kff;             // 光伏前馈增益, 单位 s
    double Ts;              // 控制周期 s
    double deadband_enter;  // 进入死区阈值 kW
    double deadband_exit;   // 退出死区阈值 kW(迟滞,应大于deadband_enter)
    double P_grid_min;      // 允许最小并网功率 kW
    double integral_max;    // 积分项硬限幅上限
    double integral_min;    // 积分项硬限幅下限
};

// 简易clamp实现已迁移至 ../shared/clamper.h 的 bms::clamp。

class AntiReverseController
{
public:
    explicit AntiReverseController(const AntiReverseConfig& cfg);

    /// 校验配置合法性（Ts>0、增益非负、0<=deadband_enter<deadband_exit、积分上下限有序）
    static bool isValidConfig(const AntiReverseConfig& cfg);

    /**
     * @brief 防逆流控制器更新函数
     * @param P_grid 关口并网功率 kW，正：电网向用户供电；负：向电网倒送
     * @param P_pv 光伏输出功率 kW
     * @param P_chg_max BMS允许最大充电功率 kW(>=0)
     * @param charge_enabled 充电使能，false强制输出0并重置内部状态
     * @return 储能充电功率需求(正值) kW
     */
    double update(double P_grid, double P_pv, double P_chg_max, bool charge_enabled);

    /// 重置控制器全部内部状态
    void reset();

private:
    AntiReverseConfig cfg_;

    double integral_;
    double prev_P_pv_;
    double last_output_;

    bool in_deadband_;
    bool first_update_;

    unsigned int safe_overcharge_cnt_;  // 安全侧持续过充拍数（陈旧积分清零用）
};

#endif //ANTI_REVERSE_CONTROLLER_H
