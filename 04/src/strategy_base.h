// =====================================================================
// IStrategy — 所有 EMS 策略的统一抽象基类
//
// 依据：docs/接口规范/EMS策略接口规范.md v1.1 §3 + §4
//
// 设计原则：
//   - evaluate() 必须是纯虚函数（每个策略核心逻辑不同）
//   - 其余钩子（on_register/on_unregister/on_config_change/on_tick）提供默认实现
//   - 状态查询用 const getter，不暴露内部 mutable
//   - 错误/异常通过返回值（StrategyResult.active=false, reason="error:xxx"）传达，
//     不抛 C++ 异常（嵌入式实时循环禁用异常）
//
// 编译：本文件是纯头文件。
// =====================================================================

#pragma once

#include "data_models.h"

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace ems {

// 策略参数通用 map（key=字符串名，value=double）
using ParamMap = std::unordered_map<std::string, double>;

// 策略运行模式（v1.1 接口规范 §2.5 — L3 独占模式）
enum class RunMode {
    kTimed,     // 峰谷套利（peak_valley）
    kMPC,       // 模型预测（dispatch_optimizer）
    kCustom,    // 自定义脚本（custom_script）
    kIdle,      // 空闲
};

// =====================================================================
// IStrategy 抽象基类
// =====================================================================
class IStrategy {
public:
    virtual ~IStrategy() = default;

    // ---- 元信息 ----
    virtual std::string id()   const = 0;   // 唯一 id
    virtual std::string name() const = 0;   // 人类可读名
    virtual Priority    priority() const = 0;
    virtual RunMode     mode() const { return RunMode::kIdle; }   // 默认空闲

    // ---- 生命周期钩子 ----
    virtual void on_register()    {}   // 注册时被调一次
    virtual void on_unregister()  {}   // 注销前调一次
    virtual void on_enable()      {}   // enable(true) 后
    virtual void on_disable()     {}   // enable(false) 后
    virtual void on_config_change() {} // 参数变更后

    // ---- 状态 ----
    bool enabled() const { return enabled_; }
    void set_enabled(bool e) {
        bool prev = enabled_;
        enabled_ = e;
        if (e && !prev) on_enable();
        else if (!e && prev) on_disable();
    }

    // ---- 参数热更新 ----
    // 基类只暴露 setter 接口，派生类按需 override 响应变更
    virtual void set_param(const std::string& key, double value) {
        params_[key] = value;
        on_config_change();
    }
    double get_param(const std::string& key, double default_v = 0.0) const {
        auto it = params_.find(key);
        return (it == params_.end()) ? default_v : it->second;
    }

    // ---- 核心：每拍计算 ----
    // 输入：实时数据 + 设备限制
    // 输出：策略结果（区间 + 期望 + 状态）
    // 约束：必须无副作用（除更新内部状态如一阶滤波），同输入同输出（幂等）
    virtual StrategyResult evaluate(const RealtimeSnapshot& rt,
                                    const DeviceLimits& dev) = 0;

protected:
    bool     enabled_ = true;
    ParamMap params_;
};

// 智能指针别名
using StrategyPtr = std::shared_ptr<IStrategy>;

} // namespace ems