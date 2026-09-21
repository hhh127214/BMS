#ifndef LVRT_CONTROLLER_H
#define LVRT_CONTROLLER_H

#include <cstdint>
#include <chrono>
#include <functional>
#include <vector>

class LvrtController; // 前向声明

enum class ControlMode {
    MODE_NORMAL,
    MODE_REACTIVE_CURRENT
};

// 设备接口
class ISVGController {
public:
    virtual ~ISVGController() = default;
    virtual void setControlMode(ControlMode mode) = 0;
    virtual void setReactiveCurrent(double iq_pu) = 0;
    virtual void exitLVRT() = 0;
};

class IInverterGroup {
public:
    virtual ~IInverterGroup() = default;
    virtual void setReactiveCurrent(double total_iq_pu) = 0;
    virtual void setActivePowerLimit(double limit_pu) = 0;
    virtual void exitLVRT() = 0;
};

class IOLTCController {
public:
    virtual ~IOLTCController() = default;
    virtual void lock(bool enable) = 0;
};

// 配置结构
struct LvrtConfig {
    double k_factor = 2.0;
    double max_current_pu = 1.1;
    double fault_voltage_threshold = 0.9;
    double fault_delta_threshold = 0.05;
    int    fault_debounce_ms = 10;
    double recovery_voltage_threshold = 0.9;
    int    recovery_delay_ms = 100;
    double svg_current_ratio = 0.7;

    struct RecoveryStep {
        int time_ms;
        std::function<void(LvrtController*)> action;
    };
    std::vector<RecoveryStep> recovery_steps;

    LvrtConfig(); // 在 cpp 中实现
};

// LVRT 控制器
class LvrtController {
public:
    explicit LvrtController(ISVGController* svg,
                            IInverterGroup* inverters,
                            IOLTCController* oltc,
                            const LvrtConfig& config = LvrtConfig());

    void run(std::chrono::steady_clock::time_point now, double voltage_pu);
    void setConfig(const LvrtConfig& config);

    enum class State { IDLE, ACTIVE, RECOVERY };
    State getState() const { return state_; }
    const char* stateString() const;
    void setState(State s) { state_ = s; }   // 只有一个，确保没有重复

    using LogCallback = std::function<void(const std::string&)>;
    void setLogCallback(LogCallback cb) { log_cb_ = std::move(cb); }
    void log(const std::string& msg) const;

    // 设备指针（公有，供 lambda 访问）
    ISVGController* svg_;
    IInverterGroup* inverters_;
    IOLTCController* oltc_;

private:
    // 私有方法
    void onIdle(std::chrono::steady_clock::time_point now, double voltage);
    void onActive(std::chrono::steady_clock::time_point now, double voltage);
    void onRecovery(std::chrono::steady_clock::time_point now);
    bool detectFault(double voltage);
    void enterLVRT(double iq_ref);
    void updateReactiveCurrent(double iq_ref);

    // 私有成员
    State state_;
    LvrtConfig config_;
    double required_iq_ = 0.0;
    double voltage_last_ = 1.0;
    int fault_debounce_counter_ = 0;
    std::chrono::steady_clock::time_point fault_start_time_;
    std::chrono::steady_clock::time_point recovery_start_time_;
    int recovery_step_index_ = 0;
    LogCallback log_cb_;
};

#endif