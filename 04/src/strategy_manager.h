// =====================================================================
// StrategyManager — 策略管理器（设计方案周期3）
//
// 职责（对应 §7 周期 3 主要任务）：
//   - 策略注册 / 注销
//   - 启动 / 停止 / 启停切换
//   - 状态监控（每策略的 enabled、active、上次 reason、上次 result 时间戳）
//   - 参数配置（set_param 热更新）
//   - 全生命周期管理
//   - 全局运行模式（RunMode）：保证 L3 策略独占（同接口规范 §2.5）
//
// 线程安全：单线程实时循环使用，不加锁。
//   若需要多线程访问，由调用方在外层加锁。
//
// 编译：本文件是纯头文件，实现都是 inline。
// =====================================================================

#pragma once

#include "strategy_base.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace ems {

// 单策略状态快照（监控用）
struct StrategyStatus {
    std::string strategy_id;
    bool        enabled = true;
    bool        started = false;     // 是否被 start() 激活
    bool        last_active = false; // 最后一次 evaluate 是否 active
    Timestamp   last_eval_ts = 0.0;  // 上次 evaluate 时间戳
    std::string last_reason;         // 上次 reason
    double      last_p_lower  = 0.0;
    double      last_p_upper  = 0.0;
    double      last_p_desired = 0.0;
};

class StrategyManager {
public:
    // ---- 注册 / 注销 ----
    // 注册同名 strategy 时抛 invalid_argument（id 必须唯一）
    void register_strategy(StrategyPtr s) {
        if (!s) throw std::invalid_argument("register_strategy: null");
        const std::string& id = s->id();
        if (id.empty()) throw std::invalid_argument("register_strategy: empty id");
        std::lock_guard<std::mutex> lk(mu_);
        if (registry_.count(id)) {
            throw std::invalid_argument(
                "register_strategy: duplicate id '" + id + "'");
        }
        registry_[id] = s;
        StrategyStatus st;
        st.strategy_id = id;
        st.enabled = s->enabled();
        st.started = false;
        status_[id] = st;
        s->on_register();
    }

    void unregister_strategy(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = registry_.find(id);
        if (it == registry_.end()) return;
        it->second->on_unregister();
        registry_.erase(it);
        status_.erase(id);
    }

    // 清空全部注册（幂等重初始化用；会先回调每个策略的 on_unregister）
    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : registry_) kv.second->on_unregister();
        registry_.clear();
        status_.clear();
        run_mode_ = RunMode::kIdle;
    }

    // ---- 启动 / 停止 ----
    // start() 把策略置为 started=true（参与 tick）
    // stop() 把策略置为 started=false（不参与 tick 但保留注册）
    void start(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu_);
        auto sit = status_.find(id);
        if (sit == status_.end()) {
            throw std::invalid_argument("start: unknown id '" + id + "'");
        }
        sit->second.started = true;
    }

    void stop(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu_);
        auto sit = status_.find(id);
        if (sit == status_.end()) return;
        sit->second.started = false;
    }

    void start_all() {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : status_) kv.second.started = true;
    }

    void stop_all() {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : status_) kv.second.started = false;
    }

    // ---- 启用 / 禁用（与 started 独立：enabled=false 时 evaluate 返回非 active）----
    void enable(const std::string& id, bool on = true) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = registry_.find(id);
        if (it == registry_.end()) {
            throw std::invalid_argument("enable: unknown id '" + id + "'");
        }
        it->second->set_enabled(on);
        status_[id].enabled = on;
    }

    // ---- 参数热更新 ----
    void set_param(const std::string& id,
                   const std::string& key, double value) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = registry_.find(id);
        if (it == registry_.end()) {
            throw std::invalid_argument("set_param: unknown id '" + id + "'");
        }
        it->second->set_param(key, value);
    }

    // ---- 全局运行模式（影响 L3 策略的 weight） ----
    void set_run_mode(RunMode m) {
        std::lock_guard<std::mutex> lk(mu_);
        run_mode_ = m;
        // L3 独占：除匹配 mode 的策略外，其他 L3 策略 weight 设为 0
        for (auto& kv : registry_) {
            auto& s = kv.second;
            if (s->priority() != Priority::kL3_GlobalEcon) continue;
            bool match = (s->mode() == m) || (m == RunMode::kIdle);
            s->set_param("__weight__", match ? 1.0 : 0.0);
        }
    }
    RunMode run_mode() const { return run_mode_; }

    // ---- tick：调用一次所有 started=true 且 enabled=true 的策略 evaluate() ----
    // 输出按 priority 升序返回（L0 在前），便于仲裁器逐层收敛。
    std::vector<StrategyResult> tick(const RealtimeSnapshot& rt,
                                     const DeviceLimits& dev) {
        std::vector<StrategyResult> out;
        std::lock_guard<std::mutex> lk(mu_);

        // 第一遍：按 priority 分桶
        std::vector<std::vector<StrategyPtr>> bucket(4);
        for (auto& kv : registry_) {
            const auto& st = status_[kv.first];
            if (!st.started || !kv.second->enabled()) continue;
            int idx = static_cast<int>(kv.second->priority());
            if (idx < 0 || idx >= 4) continue;
            bucket[idx].push_back(kv.second);
        }

        // 第二遍：调用 evaluate，收集结果
        for (int lvl = 0; lvl < 4; ++lvl) {
            for (auto& sp : bucket[lvl]) {
                StrategyResult r;
                try {
                    r = sp->evaluate(rt, dev);
                } catch (...) {
                    // 兜底：策略抛异常时记录为错误，不影响其他策略
                    r = StrategyResult{};
                    r.strategy_id = sp->id();
                    r.priority    = sp->priority();
                    r.active      = false;
                    r.reason      = "exception_caught";
                }

                // 读取 __weight__ 作为 weight（用于 L3 独占模式）
                double w = sp->get_param("__weight__", 1.0);
                if (w < 0.0) w = 0.0;
                if (w > 1.0) w = 1.0;
                r.weight = w;

                // 更新状态
                auto& st = status_[sp->id()];
                st.last_active   = r.active;
                st.last_reason   = r.reason;
                st.last_eval_ts  = rt.timestamp;
                st.last_p_lower  = r.p_lower;
                st.last_p_upper  = r.p_upper;
                st.last_p_desired = r.p_desired;

                out.push_back(r);
            }
        }
        return out;
    }

    // ---- 状态查询 ----
    bool has(const std::string& id) const {
        std::lock_guard<std::mutex> lk(mu_);
        return registry_.count(id) > 0;
    }

    StrategyStatus get_status(const std::string& id) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = status_.find(id);
        if (it == status_.end()) {
            throw std::invalid_argument("get_status: unknown id '" + id + "'");
        }
        return it->second;
    }

    std::vector<StrategyStatus> get_all_status() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<StrategyStatus> v;
        v.reserve(status_.size());
        for (auto& kv : status_) v.push_back(kv.second);
        return v;
    }

    std::vector<std::string> list_ids() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::string> v;
        v.reserve(registry_.size());
        for (auto& kv : registry_) v.push_back(kv.first);
        return v;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return registry_.size();
    }

private:
    // 注：成员按声明顺序构造、析构逆序
    mutable std::mutex                                mu_;
    std::unordered_map<std::string, StrategyPtr>      registry_;
    std::unordered_map<std::string, StrategyStatus>   status_;
    RunMode                                            run_mode_ = RunMode::kIdle;
};

} // namespace ems