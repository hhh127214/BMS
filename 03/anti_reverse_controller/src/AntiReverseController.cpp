#include "AntiReverseController.h"
#include <stdexcept>

bool AntiReverseController::isValidConfig(const AntiReverseConfig& c)
{
    return c.Ts > 0.0 &&
           c.Kp >= 0.0 && c.Ki >= 0.0 && c.Kff >= 0.0 &&
           c.deadband_enter >= 0.0 && c.deadband_exit > c.deadband_enter &&
           c.integral_max > c.integral_min;
}

AntiReverseController::AntiReverseController(const AntiReverseConfig& cfg)
    : cfg_(cfg),
      integral_(0.0),
      prev_P_pv_(0.0),
      last_output_(0.0),
      in_deadband_(false),
      first_update_(true),
      safe_overcharge_cnt_(0)
{
    if (!isValidConfig(cfg_))
    {
        throw std::invalid_argument("AntiReverseController: invalid config "
                                    "(need Ts>0, Kp/Ki/Kff>=0, "
                                    "0<=deadband_enter<deadband_exit, "
                                    "integral_min<integral_max)");
    }
}

double AntiReverseController::update(double P_grid, double P_pv,
                                     double P_chg_max, bool charge_enabled)
{
    // 确保最大充电功率非负
    if (P_chg_max < 0.0)
    {
        P_chg_max = 0.0;
    }

    // 如果不允许充电：强制输出0，重置积分，标记首次更新，避免恢复时前馈尖峰
    if (!charge_enabled)
    {
        integral_ = 0.0;
        last_output_ = 0.0;
        in_deadband_ = false;
        prev_P_pv_ = P_pv;
        first_update_ = true;   // 恢复充电时重新初始化光伏历史值
        safe_overcharge_cnt_ = 0;
        return 0.0;
    }

    // 首次更新：初始化prev_P_pv_，避免光伏前馈出现巨大阶跃
    if (first_update_)
    {
        prev_P_pv_ = P_pv;
        first_update_ = false;
    }

    // 计算误差 e = P_grid_min - P_grid
    double error = cfg_.P_grid_min - P_grid;

    // ---------- 死区+迟滞处理（设计文档3.5：以e=0为中心的对称双阈值死区） ----------
    if (in_deadband_)
    {
        // 死区内：|e| > deadband_exit 才退出死区恢复调节（施密特迟滞）
        if (std::fabs(error) > cfg_.deadband_exit)
        {
            in_deadband_ = false;
        }
        else
        {
            // 维持上一拍输出、不更新积分；死区内持续跟踪P_pv，防退出瞬间前馈阶跃
            prev_P_pv_ = P_pv;
            return bms::clamp(last_output_, 0.0, P_chg_max);   // 冻结输出，且受当前BMS限值约束
        }
    }
    else
    {
        // 不在死区：|e| < deadband_enter 才进入死区，冻结输出
        if (std::fabs(error) < cfg_.deadband_enter)
        {
            in_deadband_ = true;
            prev_P_pv_ = P_pv;
            return bms::clamp(last_output_, 0.0, P_chg_max);
        }
    }


    // ---------- 正常调节（不在死区） ----------
    double P_prop = cfg_.Kp * error;
    double P_int  = cfg_.Ki * cfg_.Ts * integral_;

    // 光伏功率变化率前馈
    double dP_pv  = (P_pv - prev_P_pv_) / cfg_.Ts;
    double P_ff   = cfg_.Kff * dP_pv;

    // 更新上一拍光伏
    prev_P_pv_ = P_pv;

    // PI+前馈未限幅输出
    double P_unclamped = P_prop + P_int + P_ff;

    // 输出限幅 [0, P_chg_max]
    double output = bms::clamp(P_unclamped, 0.0, P_chg_max);

    // ---------- 遇限削弱积分（抗积分饱和，设计文档3.3） ----------
    bool saturated_high = (output >= P_chg_max) && (error > 0.0);
    bool saturated_low  = (output <= 0.0) && (error < 0.0);

    if (!saturated_high && !saturated_low)
    {
        integral_ += error;   // 正常累加（含安全侧负误差的缓慢衰减）
    }
    // saturated_high：达到上限且误差为正 → 停止积分累加（防积分饱和）
    // saturated_low ：达到下限且误差为负 → 停止积分累加（防积分无限下漂）

    // 陈旧积分保护（修复项7）：电网供给充裕的安全侧(e<-deadband_exit)仍持续请求充电≥3拍
    // → 判定为积分陈旧导致过充（如长时间大盈余后盈余骤降），积分清零，避免安全侧过充/振荡
    if (error < -cfg_.deadband_exit && output > 0.0)
    {
        ++safe_overcharge_cnt_;
        if (safe_overcharge_cnt_ >= 3)
        {
            integral_ = 0.0;
            safe_overcharge_cnt_ = 0;
        }
    }
    else
    {
        safe_overcharge_cnt_ = 0;
    }

    // 积分项硬限幅，防止浮点累积漂移溢出
    integral_ = bms::clamp(integral_, cfg_.integral_min, cfg_.integral_max);

    last_output_ = output;
    return output;
}

void AntiReverseController::reset()
{
    integral_ = 0.0;
    prev_P_pv_ = 0.0;
    last_output_ = 0.0;
    in_deadband_ = false;
    first_update_ = true;
    safe_overcharge_cnt_ = 0;
}
