#include "SmoothingController.h"

SmoothingController::SmoothingController(const Config& cfg)
    : cfg_(cfg),
      alpha_(cfg.tau / (cfg.tau + cfg.Ts)),
      P_smooth_prev_(0.0),
      last_output_(0.0),
      first_update_(true) {}

double SmoothingController::update(double P_pv, double SOC,
                                   double P_chg_max, double P_dis_max,
                                   bool charge_enabled, bool discharge_enabled,
                                   bool smoothing_enabled, bool data_valid) {
    // 数据无效或功能关闭：输出0，重新初始化
    if (!data_valid || !smoothing_enabled) {
        P_smooth_prev_ = P_pv;
        last_output_ = 0.0;
        first_update_ = true;
        return 0.0;
    }

    if (first_update_) {
        P_smooth_prev_ = P_pv;
        first_update_ = false;
        last_output_ = 0.0;
        return 0.0;
    }

    // 一阶低通滤波
    double P_smooth = alpha_ * P_smooth_prev_ + (1.0 - alpha_) * P_pv;
    P_smooth_prev_ = P_smooth;

    // 理想补偿功率
    double P_comp = P_smooth - P_pv;

    // SOC方向性衰减
    if (SOC > cfg_.SOC_high && P_comp < 0.0) {
        // 高SOC，只衰减充电
        double decay = std::max(0.0, (cfg_.SOC_max - SOC) / (cfg_.SOC_max - cfg_.SOC_high));
        P_comp *= decay;
    } else if (SOC < cfg_.SOC_low && P_comp > 0.0) {
        // 低SOC，只衰减放电
        double decay = std::max(0.0, (SOC - cfg_.SOC_min) / (cfg_.SOC_low - cfg_.SOC_min));
        P_comp *= decay;
    }
    // 其他情况不衰减

    // 安全限幅与使能
    if (P_comp < 0.0) {  // 充电
        if (!charge_enabled) {
            P_comp = 0.0;
        } else {
            P_comp = std::max(-P_chg_max, P_comp);
        }
    } else {  // 放电
        if (!discharge_enabled) {
            P_comp = 0.0;
        } else {
            P_comp = std::min(P_dis_max, P_comp);
        }
    }

    last_output_ = P_comp;
    return P_comp;
}

void SmoothingController::reset() {
    P_smooth_prev_ = 0.0;
    last_output_ = 0.0;
    first_update_ = true;
}