#include "lvrt_controller.h"
#include <cmath>
#include <sstream>
#include <iostream>

LvrtConfig::LvrtConfig() {
    recovery_steps = {
        { 50,  [](LvrtController* c) {
            if (c->inverters_) {
                c->inverters_->exitLVRT();
                c->inverters_->setActivePowerLimit(1.0);
            }
            c->log("[LVRT] 恢复步骤1：逆变器退出 LVRT");
        }},
        { 150, [](LvrtController* c) {
            if (c->svg_) {
                c->svg_->exitLVRT();
                c->svg_->setControlMode(ControlMode::MODE_NORMAL);
            }
            c->log("[LVRT] 恢复步骤2：SVG 退出 LVRT");
        }},
        { 500, [](LvrtController* c) {
            if (c->oltc_) c->oltc_->lock(false);
            c->log("[LVRT] 恢复步骤3：OLTC 解锁");
        }},
        { 1000, [](LvrtController* c) {
            c->setState(LvrtController::State::IDLE);   // 使用 setter
            c->log("[LVRT] 恢复完成，回到正常模式");
        }}
    };
}

LvrtController::LvrtController(ISVGController* svg,
                               IInverterGroup* inverters,
                               IOLTCController* oltc,
                               const LvrtConfig& config)
    : state_(State::IDLE),
      config_(config),
      svg_(svg),
      inverters_(inverters),
      oltc_(oltc) {
    setLogCallback([](const std::string& msg) { std::cout << msg << std::endl; });
}

void LvrtController::setConfig(const LvrtConfig& config) {
    config_ = config;
}

void LvrtController::run(std::chrono::steady_clock::time_point now, double voltage_pu) {
    // 先处理状态，使用旧的 voltage_last_
    switch (state_) {
        case State::IDLE:     onIdle(now, voltage_pu); break;
        case State::ACTIVE:   onActive(now, voltage_pu); break;
        case State::RECOVERY: onRecovery(now); break;
    }
    // 最后更新历史电压
    voltage_last_ = voltage_pu;
}

void LvrtController::onIdle(std::chrono::steady_clock::time_point now, double voltage) {
    if (detectFault(voltage)) {
        state_ = State::ACTIVE;
        fault_start_time_ = now;
        required_iq_ = config_.k_factor * (config_.fault_voltage_threshold - voltage);
        if (required_iq_ < 0) required_iq_ = 0;
        enterLVRT(required_iq_);
        log("[LVRT] 检测到故障，进入 ACTIVE 状态");
    }
}

void LvrtController::onActive(std::chrono::steady_clock::time_point now, double voltage) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - fault_start_time_).count();
    if (voltage >= config_.recovery_voltage_threshold && elapsed > config_.recovery_delay_ms) {
        state_ = State::RECOVERY;
        recovery_start_time_ = now;
        recovery_step_index_ = 0;
        log("[LVRT] 电压恢复，开始退出流程");
        return;
    }

    if (voltage < config_.fault_voltage_threshold) {
        double new_iq = config_.k_factor * (config_.fault_voltage_threshold - voltage);
        if (new_iq < 0) new_iq = 0;
        if (new_iq > config_.max_current_pu) new_iq = config_.max_current_pu;
        if (std::abs(new_iq - required_iq_) > 0.01) {
            required_iq_ = new_iq;
            updateReactiveCurrent(required_iq_);
        }
    }
}

void LvrtController::onRecovery(std::chrono::steady_clock::time_point now) {
    while (recovery_step_index_ < static_cast<int>(config_.recovery_steps.size())) {
        const auto& step = config_.recovery_steps[recovery_step_index_];
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - recovery_start_time_).count();
        if (elapsed >= step.time_ms) {
            step.action(this);
            recovery_step_index_++;
        } else {
            break;
        }
    }
}

bool LvrtController::detectFault(double voltage) {
    bool low_voltage = voltage < config_.fault_voltage_threshold;
    double delta = voltage_last_ - voltage;
    bool sudden_drop = delta > config_.fault_delta_threshold;

    if (low_voltage && sudden_drop) {
        fault_debounce_counter_++;
    } else {
        fault_debounce_counter_ = 0;
    }

    int needed = config_.fault_debounce_ms / 5;
    return fault_debounce_counter_ >= needed;
}

void LvrtController::enterLVRT(double iq_ref) {
    if (iq_ref > config_.max_current_pu) iq_ref = config_.max_current_pu;

    double svg_iq = iq_ref * config_.svg_current_ratio;
    double inv_iq = iq_ref * (1.0 - config_.svg_current_ratio);

    if (svg_) {
        svg_->setControlMode(ControlMode::MODE_REACTIVE_CURRENT);
        svg_->setReactiveCurrent(svg_iq);
    }
    if (inverters_) {
        inverters_->setReactiveCurrent(inv_iq);
        double max_id = std::sqrt(config_.max_current_pu * config_.max_current_pu - iq_ref * iq_ref);
        if (max_id < 0) max_id = 0;
        inverters_->setActivePowerLimit(max_id);
    }
    if (oltc_) oltc_->lock(true);

    std::ostringstream oss;
    oss << "[LVRT] 进入故障模式，总无功电流需求=" << iq_ref << " pu";
    log(oss.str());
}

void LvrtController::updateReactiveCurrent(double iq_ref) {
    if (iq_ref > config_.max_current_pu) iq_ref = config_.max_current_pu;

    double svg_iq = iq_ref * config_.svg_current_ratio;
    double inv_iq = iq_ref * (1.0 - config_.svg_current_ratio);

    if (svg_) svg_->setReactiveCurrent(svg_iq);
    if (inverters_) {
        inverters_->setReactiveCurrent(inv_iq);
        double max_id = std::sqrt(config_.max_current_pu * config_.max_current_pu - iq_ref * iq_ref);
        if (max_id < 0) max_id = 0;
        inverters_->setActivePowerLimit(max_id);
    }
}

const char* LvrtController::stateString() const {
    switch (state_) {
        case State::IDLE:     return "IDLE";
        case State::ACTIVE:   return "ACTIVE";
        case State::RECOVERY: return "RECOVERY";
        default:              return "UNKNOWN";
    }
}

void LvrtController::log(const std::string& msg) const {
    if (log_cb_) log_cb_(msg);
}