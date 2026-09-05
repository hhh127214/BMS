// =====================================================================
// A组（安全与保护）约束管理模块 —— C++ 骨架实现
// 实现三个策略：BMS禁止充放 / BMS请求降功率 / 变压器过载限功率
// 以及约束合并逻辑。单文件，可直接编译运行演示。
//
// 编译: g++ -std=c++17 safety_constraint_manager.cpp -o safety_constraint_manager
// =====================================================================

#include <iostream>
#include <string>
#include <algorithm>   // std::min
#include <limits>      // std::numeric_limits
#include <cmath>       // std::fabs, std::isinf
#include <cstdint>     // int64_t
#include <cstdio>      // snprintf

// =====================================================================
// 日志（SOE 事件记录）
// =====================================================================
static void LogSoe(const std::string& msg) {
    std::cout << "  [SOE] " << msg << std::endl;
}

// =====================================================================
// 数据结构
// =====================================================================

// BMS 上送数据
struct BmsStatus {
    bool  charge_enable = true;          // 充电使能（1允许/0禁止）
    bool  discharge_enable = true;       // 放电使能（1允许/0禁止）
    float max_charge_power = 100.0f;     // 最大允许充电功率(kW)
    float max_discharge_power = 100.0f;  // 最大允许放电功率(kW)
    int64_t last_update_ms = 0;          // 数据时间戳(ms)，用于判断通信是否中断
};

// 合并后的全局安全约束（对外输出给仲裁模块）
struct SafetyConstraints {
    bool  charge_blocked = false;            // 是否禁止充电
    bool  discharge_blocked = false;         // 是否禁止放电
    float charge_power_limit_kw = 100.0f;    // 充电功率上限(kW)
    float discharge_power_limit_kw = 100.0f; // 放电功率上限(kW)
    int64_t timestamp_ms = 0;                // 时间戳
};

// 配置参数（此处仅保留变压器相关配置；BMS 超时/滤波系数用处理器内部常量）
struct Config {
    float rated_kva = 250.0f;           // 变压器额定容量(kVA)
    float power_factor = 0.9f;          // 功率因数
    float overload_threshold = 0.95f;   // 过载启动阈值（负载率）
    float hysteresis = 0.02f;           // 回差
    float max_overload_ratio = 1.1f;    // 极端过载倍数（强制禁止放电）
};

// =====================================================================
// 策略一：BMS禁止充放
// 逻辑：通信超时保守禁止 + 使能信号去抖 + 输出全局禁止标志
// =====================================================================
class BmsEnableProcessor {
public:
    static constexpr int64_t kTimeoutMs = 1500; // 通信超时判定

    void Process(const BmsStatus& bms, int64_t now_ms) {
        // 通信有效性检查：超过 1.5s 未更新 -> 保守同时禁止充放电
        if (now_ms - bms.last_update_ms > kTimeoutMs) {
            if (!charge_blocked_ || !discharge_blocked_) {
                charge_blocked_ = true;
                discharge_blocked_ = true;
                LogSoe("BMS通信丢失，禁止充放电");
            }
            return;
        }

        // 去抖：连续两次采样一致才更新
        if (bms.charge_enable == prev_charge_enable_) {
            bool new_block = !bms.charge_enable;
            if (new_block != charge_blocked_) {
                charge_blocked_ = new_block;
                LogSoe(charge_blocked_ ? "BMS禁止充电" : "BMS允许充电");
            }
        }
        if (bms.discharge_enable == prev_discharge_enable_) {
            bool new_block = !bms.discharge_enable;
            if (new_block != discharge_blocked_) {
                discharge_blocked_ = new_block;
                LogSoe(discharge_blocked_ ? "BMS禁止放电" : "BMS允许放电");
            }
        }
        prev_charge_enable_ = bms.charge_enable;
        prev_discharge_enable_ = bms.discharge_enable;
    }

    bool charge_blocked() const { return charge_blocked_; }
    bool discharge_blocked() const { return discharge_blocked_; }

private:
    bool charge_blocked_ = false;
    bool discharge_blocked_ = false;
    bool prev_charge_enable_ = true;
    bool prev_discharge_enable_ = true;
};

// =====================================================================
// 策略二：BMS请求降功率
// 逻辑：通信超时保守置0 + 一阶低通滤波平滑 + 输出功率上限
// =====================================================================
class BmsPowerLimitProcessor {
public:
    static constexpr int64_t kTimeoutMs = 1500; // 通信超时判定
    static constexpr float kAlpha = 0.2f;       // 一阶低通滤波系数(约500ms时间常数)

    void Process(const BmsStatus& bms, int64_t now_ms) {
        // 通信有效性检查：中断 -> 保守限制为0（禁止功率流动）
        if (now_ms - bms.last_update_ms > kTimeoutMs) {
            charge_limit_ = 0.0f;
            discharge_limit_ = 0.0f;
            return;
        }
        // 一阶低通滤波：y = alpha*x + (1-alpha)*y_prev
        charge_limit_ = kAlpha * bms.max_charge_power + (1.0f - kAlpha) * charge_limit_;
        discharge_limit_ = kAlpha * bms.max_discharge_power + (1.0f - kAlpha) * discharge_limit_;
    }

    float charge_limit() const { return charge_limit_; }
    float discharge_limit() const { return discharge_limit_; }

private:
    float charge_limit_ = 100.0f;
    float discharge_limit_ = 100.0f;
};

// =====================================================================
// 策略三：变压器过载限功率
// 逻辑：负载率计算 + 阈值/回差(滞环) + 动态计算允许放电功率
// =====================================================================
class TransformerOverloadProcessor {
public:
    void Init(const Config& cfg) {
        cfg_ = cfg;
        rated_kw_ = cfg.rated_kva * cfg.power_factor; // 视在容量换算为有功容量
    }

    void Process(float load_kw) {
        load_ratio_ = load_kw / rated_kw_;

        // 极端过载标志（>110% 时由合并逻辑强制禁止放电）
        extreme_overload_ = load_ratio_ > cfg_.max_overload_ratio;

        // 带回差(滞环)的启停判断
        if (!limiting_ && load_ratio_ > cfg_.overload_threshold) {
            limiting_ = true;
            LogSoe("变压器过载限功率启动，负载率=" + std::to_string((int)(load_ratio_ * 100)) + "%");
        } else if (limiting_ && load_ratio_ < (cfg_.overload_threshold - cfg_.hysteresis)) {
            limiting_ = false;
            LogSoe("变压器过载限功率解除");
        }

        if (limiting_) {
            // 允许的最大放电功率 = 阈值容量 - 当前负载；为负则置0（禁止放电）
            float allowed = cfg_.overload_threshold * rated_kw_ - load_kw;
            if (allowed < 0.0f) allowed = 0.0f;
            discharge_limit_ = allowed;
        } else {
            discharge_limit_ = std::numeric_limits<float>::infinity(); // 不限制
        }
    }

    float discharge_limit() const { return discharge_limit_; }
    bool extreme_overload() const { return extreme_overload_; }
    bool limiting() const { return limiting_; }
    float load_ratio() const { return load_ratio_; }

private:
    Config cfg_;
    float rated_kw_ = 225.0f;
    bool limiting_ = false;
    bool extreme_overload_ = false;
    float load_ratio_ = 0.0f;
    float discharge_limit_ = std::numeric_limits<float>::infinity();
};

// =====================================================================
// 数据提供者接口
// =====================================================================
class IBmsDataProvider {
public:
    virtual ~IBmsDataProvider() = default;
    virtual BmsStatus GetData() const = 0;
};

class IGridMeterProvider {
public:
    virtual ~IGridMeterProvider() = default;
    virtual float GetTransformerLoadKw() const = 0;
};

// =====================================================================
// 演示用 Mock 数据源（按仿真时间返回不同场景数据）
// =====================================================================
class MockBmsProvider : public IBmsDataProvider {
public:
    void SetSimTime(int64_t t) { sim_time_ms_ = t; }

    BmsStatus GetData() const override {
        BmsStatus s;

        // 场景5 (>=7000ms)：BMS通信中断，数据冻结在 6900ms 的最后有效值
        if (sim_time_ms_ >= 7000) {
            s.charge_enable = false;   // 6900ms 时刻为禁止充电，此后冻结
            s.discharge_enable = true;
            s.max_charge_power = 100.0f;
            s.max_discharge_power = 100.0f;
            s.last_update_ms = 6900;   // 时间戳冻结，age 持续增长
            return s;
        }
        // 场景4 (6000~7000ms)：BMS禁止充电
        else if (sim_time_ms_ >= 6000) {
            s.charge_enable = false;
            s.discharge_enable = true;
        }
        // 场景3 (4000~6000ms)：恢复正常（配合变压器过载场景）
        else if (sim_time_ms_ >= 4000) {
            s.charge_enable = true;
            s.discharge_enable = true;
        }
        // 场景2 (2000~4000ms)：BMS请求降功率
        else if (sim_time_ms_ >= 2000) {
            s.charge_enable = true;
            s.discharge_enable = true;
            s.max_charge_power = 50.0f;
            s.max_discharge_power = 60.0f;
        }
        // 场景1 (0~2000ms)：正常
        else {
            s.charge_enable = true;
            s.discharge_enable = true;
        }

        s.last_update_ms = sim_time_ms_;
        return s;
    }

private:
    int64_t sim_time_ms_ = 0;
};

class MockGridMeterProvider : public IGridMeterProvider {
public:
    void SetSimTime(int64_t t) { sim_time_ms_ = t; }

    float GetTransformerLoadKw() const override {
        // 场景3 (4000~6000ms)：变压器过载，负载 220kW（负载率约97.8%）
        if (sim_time_ms_ >= 4000 && sim_time_ms_ < 6000)
            return 220.0f;
        return 180.0f; // 正常，负载率 80%
    }

private:
    int64_t sim_time_ms_ = 0;
};

// =====================================================================
// A组安全约束管理模块（三个处理器 + 约束合并 + 主循环入口）
// =====================================================================
class SafetyConstraintManager {
public:
    void Init(const Config& cfg, IBmsDataProvider* bms, IGridMeterProvider* meter) {
        cfg_ = cfg;
        bms_ = bms;
        meter_ = meter;
        transformer_.Init(cfg);
    }

    // 单次执行（由外部以 100ms 周期驱动）
    SafetyConstraints RunOnce(int64_t now_ms) {
        BmsStatus bms = bms_->GetData();
        float load_kw = meter_->GetTransformerLoadKw();

        bms_enable_.Process(bms, now_ms);
        bms_power_.Process(bms, now_ms);
        transformer_.Process(load_kw);

        return Merge(now_ms);
    }

    // 三个策略处理器的只读访问（供外部分别查看各策略的中间结果）
    const BmsEnableProcessor& enable_proc() const { return bms_enable_; }
    const BmsPowerLimitProcessor& power_proc() const { return bms_power_; }
    const TransformerOverloadProcessor& transformer_proc() const { return transformer_; }

private:
    SafetyConstraints Merge(int64_t now_ms) {
        SafetyConstraints out;

        // 1) 禁止标志：由 BMS 禁止充放策略唯一决定
        out.charge_blocked = bms_enable_.charge_blocked();
        out.discharge_blocked = bms_enable_.discharge_blocked();

        // 变压器极端过载(>110%)：强制禁止放电
        if (transformer_.extreme_overload()) {
            out.discharge_blocked = true;
        }

        // 2) 功率上限合并
        out.charge_power_limit_kw = bms_power_.charge_limit(); // 充电上限仅 BMS 一个来源
        out.discharge_power_limit_kw = std::min(               // 放电上限取最小值
            bms_power_.discharge_limit(),
            transformer_.discharge_limit());

        // 3) 禁止则上限置0（红灯盖过黄灯）
        if (out.charge_blocked) out.charge_power_limit_kw = 0.0f;
        if (out.discharge_blocked) out.discharge_power_limit_kw = 0.0f;

        out.timestamp_ms = now_ms;
        return out;
    }

    Config cfg_;
    IBmsDataProvider* bms_ = nullptr;
    IGridMeterProvider* meter_ = nullptr;

    BmsEnableProcessor bms_enable_;
    BmsPowerLimitProcessor bms_power_;
    TransformerOverloadProcessor transformer_;
};

// =====================================================================
// 工具函数：功率上限格式化打印
// =====================================================================
static std::string FmtKw(float v) {
    if (std::isinf(v)) return "不限";
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1fkW", v);
    return buf;
}

// =====================================================================
// 演示入口
// =====================================================================
int main() {
    Config cfg;
    MockBmsProvider bms;
    MockGridMeterProvider meter;
    SafetyConstraintManager mgr;
    mgr.Init(cfg, &bms, &meter);

    std::cout << "=== A组安全约束管理模块 仿真演示 ===" << std::endl;
    std::cout << "控制周期 100ms，总时长 9s（覆盖5个场景）" << std::endl;
    std::cout << "场景: 0-2s正常 | 2-4s降功率 | 4-6s变压器过载 | 6-7s禁止充电 | 7-9s通信中断" << std::endl << std::endl;

    for (int64_t t = 0; t <= 9000; t += 100) {
        bms.SetSimTime(t);
        meter.SetSimTime(t);
        SafetyConstraints c = mgr.RunOnce(t);

        // 每 1000ms 打印一次：三个策略的分别结果 + 合并后的全局约束
        if (t % 1000 == 0) {
            const auto& e  = mgr.enable_proc();       // 策略一
            const auto& p  = mgr.power_proc();        // 策略二
            const auto& tr = mgr.transformer_proc();  // 策略三

            std::cout << "===== t=" << t << "ms =====" << std::endl;
            std::cout << "[策略一 BMS禁止充放]      禁止充电=" << (e.charge_blocked() ? "是" : "否")
                      << "  禁止放电=" << (e.discharge_blocked() ? "是" : "否") << std::endl;
            std::cout << "[策略二 BMS请求降功率]    充电上限=" << FmtKw(p.charge_limit())
                      << "  放电上限=" << FmtKw(p.discharge_limit()) << std::endl;
            std::cout << "[策略三 变压器过载限功率] 负载率=" << (int)(tr.load_ratio() * 100) << "%"
                      << "  限功率=" << (tr.limiting() ? "是" : "否")
                      << "  放电上限=" << FmtKw(tr.discharge_limit()) << std::endl;
            std::cout << "[合并→全局安全约束]      禁止充电=" << (c.charge_blocked ? "是" : "否")
                      << "  禁止放电=" << (c.discharge_blocked ? "是" : "否")
                      << "  充电上限=" << FmtKw(c.charge_power_limit_kw)
                      << "  放电上限=" << FmtKw(c.discharge_power_limit_kw) << std::endl;
            std::cout << std::endl;
        }
    }

    std::cout << "=== 演示结束 ===" << std::endl;
    return 0;
}
