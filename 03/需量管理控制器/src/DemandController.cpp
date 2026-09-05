#include "DemandController.h"
#include <cmath>
#include <stdexcept>

bool DemandController::isValidConfig(const Config& c)
{
    return c.T_window > 0.0 && c.Ts > 0.0 &&
           c.D_target > 0.0 && c.dP_max >= 0.0 &&
           c.Kp_avg > 0.0 && c.Ki > 0.0 && c.integral_max >= 0.0;
}

DemandController::DemandController(const Config& cfg)
    : cfg_(cfg),
      window_size_(static_cast<size_t>(std::lround(cfg.T_window / cfg.Ts))),
      window_avg_(0.0),
      integral_(0.0),
      last_output_(0.0),
      active_(false),
      alarm_(false)
{
    if (!isValidConfig(cfg_))
    {
        throw std::invalid_argument("DemandController: invalid config "
                                    "(need T_window>0, Ts>0, D_target>0, "
                                    "dP_max>=0, Kp_avg>0, Ki>0, integral_max>=0)");
    }
    if (window_size_ < 1) window_size_ = 1;
}

double DemandController::update(double P_grid, double P_dis_max,
                                bool discharge_enabled, bool comm_fault)
{
    // ---------- 通信异常 / 输入非法：不更新窗口，输出0并置告警 ----------
    if (comm_fault || !std::isfinite(P_grid) || P_grid < 0.0)
    {
        last_output_ = 0.0;
        active_ = false;
        alarm_ = comm_fault;
        return 0.0;
    }

    // ---------- 滑动窗口统计（本拍样本入窗之前的状态） ----------
    const size_t n = window_.size();
    double A_prev = 0.0;                       // 入窗前窗口平均（空窗为0）
    if (n > 0)
    {
        double sum = 0.0;
        for (size_t i = 0; i < n; ++i) sum += window_[i];
        A_prev = sum / static_cast<double>(n);
    }

    // ---------- 本拍样本入窗 ----------
    // P_grid 是有效计量数据，无论输出是否被安全约束，都记录进窗口（反映实际关口电量）
    double P_left = 0.0;                       // 本次滚出的最老样本（满窗时）
    if (n >= window_size_)
    {
        P_left = window_.front();
    }
    window_.push_back(P_grid);
    if (window_.size() > window_size_)
    {
        window_.pop_front();
    }
    // 新窗口平均 = (旧窗口能量 − 滚出样本 + 新样本) / 新窗口大小
    window_avg_ = (A_prev * static_cast<double>(n) - P_left + P_grid)
                  / static_cast<double>(window_.size());

    // ---------- 已越限告警（设计文档4.1.2/第5章） ----------
    // 窗口平均已超目标：置告警。放电幅值由 PI 连续给出——越限越深放电越大，
    // 显著越限时比例项饱和到 P_dis_max。
    alarm_ = (A_prev > cfg_.D_target);

    // ---------- 安全约束（BMS禁止放电 / 最大放电功率为0）：输出0 ----------
    if (!discharge_enabled || P_dis_max <= 0.0)
    {
        last_output_ = 0.0;
        active_ = false;
        return 0.0;
    }

    // ---------- 放电需求：对窗口平均误差的 PI 控制（稳定、平滑、无通断游猎） ----------
    // 说明：cap 公式与纯比例控制均因滑动窗口历史样本的滚出/滚入，在目标附近产生
    // 慢速通断（实测 P_req 0↔100kW、周期≈窗口时长）。引入积分项后，放电由积分项
    // 维持稳态基值，比例项只做微调，从而消除通断，窗口平均精确收敛到 D_target。
    double e = A_prev - cfg_.D_target;
    double P_prop = cfg_.Kp_avg * e;
    double P_int  = cfg_.Ki * cfg_.Ts * integral_;
    double P_req_raw = P_prop + P_int;

    // 输出限幅 [0, P_dis_max]
    double output = bms::clamp(P_req_raw, 0.0, P_dis_max);

    // ---------- 遇限削弱积分（抗积分饱和） ----------
    // 上限饱和（e>0 还需放电但被钳位）与下限饱和（e<0 且输出为0）时停止累加；
    // 积分项单向（>=0），硬限幅防漂移
    bool saturated_high = (output >= P_dis_max) && (e > 0.0);
    bool saturated_low  = (output <= 0.0) && (e < 0.0);
    if (!saturated_high && !saturated_low)
    {
        integral_ += e;
    }
    integral_ = bms::clamp(integral_, 0.0, cfg_.integral_max);

    // ---------- 功率变化率限制（上坡限速；下坡允许快速退出，避免追降） ----------
    double max_change = cfg_.dP_max * cfg_.Ts;
    if (output > last_output_ + max_change)
    {
        output = last_output_ + max_change;
    }

    last_output_ = output;
    active_ = (output > 0.0);
    return output;
}

void DemandController::reset()
{
    window_.clear();
    window_avg_ = 0.0;
    integral_ = 0.0;
    last_output_ = 0.0;
    active_ = false;
    alarm_ = false;
}

