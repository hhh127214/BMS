// =====================================================================
// 17/ — 点值输出口 + 观测簿（"六类数据源全部在动"的自证装置）
//
// 为什么模拟器不直接写点表/共享内存，而写一个 **sink 接口**：
//   ① 模拟器的物理逻辑与"值怎么送出去"解耦。同一份 MeterSim 既能喂
//      TraceSink（测试/自证），也能喂 DeadbandPublisher（C2），
//      将来还能喂 Modbus 从站（B3 的 pymodbus 路线）。
//   ② 才谈得上"自证"。11/docs/联调报告 §3 抓过一类**假联调**：
//      某个量在观测窗里恒为常量，而所有断言照样绿（因为没人断言它在动）。
//      本文件的 TraceSink 把"每个量动了多少"变成可查询的数据，
//      才使 "变化量 > 阈值" 成为一条**会红的**断言。
//
// 编译：纯头文件（inline）。
// =====================================================================

#pragma once

#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ems {
namespace devsim {

// =====================================================================
// 输出口
// =====================================================================
struct PointSink {
    virtual ~PointSink() = default;
    // quality：0 = good（与 rtdb_quality::kGood 对齐；本模块不引 RT_DB 头）
    virtual void set(const std::string& name, double value, int quality = 0) = 0;
};

// =====================================================================
// 观测簿：记录每个点名的 首值/末值/极值/写入次数/变化次数
//
// 它同时承担两件事：
//   · 内存观测（测试断言用）
//   · "常量检测"（range == 0 的点 = 恒为常量 = 疑似假联调）
// =====================================================================
class TraceSink : public PointSink {
public:
    struct Stat {
        long   count   = 0;       // 被写入次数
        long   changes = 0;       // 值与上次不同的次数
        double first   = 0.0;
        double last    = 0.0;
        double min_v   = 0.0;
        double max_v   = 0.0;
        double sum     = 0.0;
        bool   touched = false;
    };

    void set(const std::string& name, double value, int quality = 0) override {
        (void)quality;
        auto it = idx_.find(name);
        Stat* s = nullptr;
        if (it == idx_.end()) {
            const std::size_t i = order_.size();
            order_.push_back(name);
            idx_.emplace(name, i);
            stats_.push_back(Stat());
            s = &stats_.back();
            s->first = value;
            s->min_v = value;
            s->max_v = value;
        } else {
            s = &stats_[it->second];
            if (value != s->last) ++s->changes;
            if (value < s->min_v) s->min_v = value;
            if (value > s->max_v) s->max_v = value;
        }
        ++s->count;
        s->last    = value;
        s->sum    += value;
        s->touched = true;
        ++total_writes_;
    }

    const Stat* stat(const std::string& name) const {
        auto it = idx_.find(name);
        if (it == idx_.end()) return nullptr;
        return &stats_[it->second];
    }
    bool has(const std::string& name) const { return idx_.find(name) != idx_.end(); }

    std::size_t point_count() const { return order_.size(); }
    const std::vector<std::string>& names() const { return order_; }

    // 观测窗内的变化量（max - min）。未出现过的点返回 0。
    double spread(const std::string& name) const {
        const Stat* s = stat(name);
        if (s == nullptr) return 0.0;
        return s->max_v - s->min_v;
    }
    double mean(const std::string& name) const {
        const Stat* s = stat(name);
        if (s == nullptr || s->count == 0) return 0.0;
        return s->sum / static_cast<double>(s->count);
    }
    long changes(const std::string& name) const {
        const Stat* s = stat(name);
        return s == nullptr ? 0 : s->changes;
    }

    // 未登记但被写入的点名计数：本模块用**"点名是否在点表里"**这条更强的
    // 判据代替（见各测试的交叉核对），所以这里不自己维护一份"已知点名"。
    std::size_t point_writes() const { return total_writes_; }

    void clear() {
        order_.clear(); idx_.clear(); stats_.clear(); total_writes_ = 0;
    }

private:
    std::unordered_map<std::string, std::size_t> idx_;
    std::vector<std::string> order_;
    std::vector<Stat>        stats_;
    std::size_t              total_writes_ = 0;
};

// =====================================================================
// "全部在动"检测
//
// required：点名 → 观测窗内**至少**要有的变化量阈值
// 返回"没动"的点名清单（空 = 全部达标）
//
// ★ 为什么必须显式给阈值，而不是自动检测"range == 0"：
//   有些点**本来就该是常量**（配置点、通信位 1、模式 0）。自动检测会把它们
//   当成缺陷。真正的判据是"**关键量**的物理过程有没有在跑"，
//   所以阈值必须由人按物理量纲给出。
// =====================================================================
struct MovementReport {
    int checked = 0;
    std::vector<std::string> frozen;     // 变化量 < 阈值（含从未出现）
    std::vector<std::string> missing;    // 从未被写入过

    bool ok() const { return frozen.empty() && missing.empty(); }

    std::string text() const {
        std::string s = "在动判据：检查 " + std::to_string(checked) + " 个点";
        if (ok()) return s + "，全部达标";
        s += "，";
        if (!missing.empty()) s += "未出现 " + std::to_string(missing.size()) + " 个 ";
        if (!frozen.empty())  s += "未达标 " + std::to_string(frozen.size()) + " 个 ";
        for (const auto& n : missing) s += "\n  · 未出现: " + n;
        for (const auto& n : frozen)  s += "\n  · 变化不足: " + n;
        return s;
    }
};

inline MovementReport check_moving(
    const TraceSink& sink,
    const std::vector<std::pair<std::string, double>>& required) {
    MovementReport rep;
    for (const auto& req : required) {
        ++rep.checked;
        if (!sink.has(req.first)) {
            rep.missing.push_back(req.first);
            rep.frozen.push_back(req.first);
            continue;
        }
        if (sink.spread(req.first) < req.second) rep.frozen.push_back(req.first);
    }
    return rep;
}

// =====================================================================
// 期望点名清单是否都出现过（打错字/漏发）
// =====================================================================
inline std::vector<std::string> missing_names(const TraceSink& sink,
                                             const std::vector<std::string>& expect) {
    std::vector<std::string> miss;
    for (const auto& n : expect) if (!sink.has(n)) miss.push_back(n);
    return miss;
}

} // namespace devsim
} // namespace ems
