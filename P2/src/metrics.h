// =====================================================================
// P2/ — 产品化 P2 可观测性 · 增量指标注册表
//
// ---------------------------------------------------------------------
// 为什么需要这个文件
// ---------------------------------------------------------------------
// P2 之前，指标是这样算的（07/src/realtime_loop.h）：
//
//     LoopMetrics metrics() const {
//         const int stride = cfg_.log_every < 1 ? 1 : cfg_.log_every;
//         LoopMetrics m = LoopMetrics::compute(log_, cfg_.dt_s * stride);  // O(N)！
//         ...
//     }
//
// 两个生产环境会出事的性质：
//
// ① **必须保留全部 StepRecord 才能算指标**。24 h @ dt=1 s = 86400 条 × 约 100 B
//    ≈ 8.6 MB，而且**只增不减**。现场连续跑一年就是 3 GB —— 内存直接爆。
//    而且 `metrics()` 每次都 O(N) 重算，不可能每秒调一次给监控系统。
//
// ② **`log_every` 降采样会让指标失真**。周期 10 已经吃过一次亏：
//    倒送缺陷在 `log_every=10` 下显示 0 违规，`log_every=1` 才暴露。
//    同样的道理：`cmd_travel_kw = Σ|Δcmd|` 在降采样后**漏掉中间的抖动**，
//    `sign_flips` 也会少计 —— 而这些恰恰是"输出抖动"的核心指标。
//
// ---------------------------------------------------------------------
// 本文件的设计
// ---------------------------------------------------------------------
//   · **O(1) 增量累积**：每拍更新固定几个数，不保留历史，内存有界
//   · **与 log_every 无关**：指标走自己的累积路径，不经过日志降采样
//   · 三类指标：counter（只增）/ gauge（可增可减，记 min/max）/ histogram（固定桶）
//   · 直方图给分位数（桶上界近似），能回答"p99 单拍耗时是多少"这类问题
//   · 导出 Prometheus 文本格式 / JSON
//
// 指标按**注册顺序**存储在 vector 中（不是 map）—— 导出结果稳定，diff 友好；
// 名称到下标用 unordered_map，更新是 O(1)。
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace ems {

enum class MetricType : int { kCounter = 0, kGauge = 1, kHistogram = 2 };

inline const char* metric_type_name(MetricType t) {
    switch (t) {
        case MetricType::kCounter:   return "counter";
        case MetricType::kGauge:     return "gauge";
        case MetricType::kHistogram: return "histogram";
    }
    return "?";
}

// 常用桶预设
namespace metric_buckets {
// 单拍耗时（微秒）
inline std::vector<double> cycle_us() {
    return {10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000};
}
// 功率绝对值（kW）
inline std::vector<double> power_kw() {
    return {1, 5, 10, 25, 50, 100, 150, 200, 250, 300, 400, 500, 630};
}
// 跟踪误差绝对值（kW）
inline std::vector<double> error_kw() {
    return {0.5, 1, 2, 5, 10, 20, 50, 100, 250};
}
} // namespace metric_buckets

// =====================================================================
// 单个指标
// =====================================================================
struct Metric {
    std::string name;      // 导出名（已净化）
    std::string help;
    std::string unit;
    MetricType  type = MetricType::kGauge;

    double   value = 0.0;      // counter / gauge
    double   min_v = 0.0;      // gauge 记录极值
    double   max_v = 0.0;
    bool     has_extremes = false;

    std::vector<double>   bounds;   // histogram 桶上界（升序，不含 +Inf）
    std::vector<uint64_t> counts;   // size = bounds.size() + 1（最后一桶为 +Inf）
    uint64_t count = 0;
    double   sum   = 0.0;
    double   h_min = 0.0, h_max = 0.0;

    double quantile(double q) const {
        if (type != MetricType::kHistogram || count == 0) return 0.0;
        const double target = q * static_cast<double>(count);
        uint64_t acc = 0;
        for (size_t i = 0; i < counts.size(); ++i) {
            acc += counts[i];
            if (static_cast<double>(acc) >= target) {
                if (i < bounds.size()) return bounds[i];
                return h_max;      // 落在 +Inf 桶 → 用观测到的最大值近似
            }
        }
        return h_max;
    }
};

// =====================================================================
// 指标注册表
// =====================================================================
class MetricRegistry {
public:
    void reset() {
        metrics_.clear();
        index_.clear();
    }

    size_t size() const { return metrics_.size(); }

    // ---- 注册（幂等：同名重复注册只更新 help/unit）----
    void counter(const std::string& name, const std::string& help = "",
                 const std::string& unit = "") {
        ensure(name, MetricType::kCounter, help, unit);
    }
    void gauge(const std::string& name, const std::string& help = "",
               const std::string& unit = "") {
        ensure(name, MetricType::kGauge, help, unit);
    }
    void histogram(const std::string& name, const std::vector<double>& bounds,
                   const std::string& help = "", const std::string& unit = "") {
        Metric& m = ensure(name, MetricType::kHistogram, help, unit);
        if (m.bounds.empty()) {
            m.bounds = bounds;
            std::sort(m.bounds.begin(), m.bounds.end());
            m.counts.assign(m.bounds.size() + 1, 0);
        }
    }

    // ---- 更新（O(1)）----
    void inc(const std::string& name, double delta = 1.0) {
        Metric& m = ensure(name, MetricType::kCounter);
        m.value += delta;
    }
    void set(const std::string& name, double v) {
        Metric& m = ensure(name, MetricType::kGauge);
        m.value = v;
        if (!m.has_extremes) { m.min_v = m.max_v = v; m.has_extremes = true; }
        else { m.min_v = std::min(m.min_v, v); m.max_v = std::max(m.max_v, v); }
    }
    void add(const std::string& name, double delta) {
        Metric& m = ensure(name, MetricType::kGauge);
        set(name, m.value + delta);
    }
    void observe(const std::string& name, double v,
                 const std::vector<double>& default_bounds = metric_buckets::cycle_us()) {
        Metric& m = ensure(name, MetricType::kHistogram);
        if (m.bounds.empty()) {
            m.bounds = default_bounds;
            std::sort(m.bounds.begin(), m.bounds.end());
            m.counts.assign(m.bounds.size() + 1, 0);
        }
        size_t i = 0;
        while (i < m.bounds.size() && v > m.bounds[i]) ++i;
        m.counts[i] += 1;
        m.count  += 1;
        m.sum    += v;
        if (m.count == 1) { m.h_min = m.h_max = v; }
        else { m.h_min = std::min(m.h_min, v); m.h_max = std::max(m.h_max, v); }
    }

    // ---- 读取 ----
    bool has(const std::string& name) const { return index_.count(name) > 0; }

    double value(const std::string& name, double def = 0.0) const {
        auto it = index_.find(name);
        return (it == index_.end()) ? def : metrics_[it->second].value;
    }
    double min_of(const std::string& name, double def = 0.0) const {
        auto it = index_.find(name);
        if (it == index_.end()) return def;
        const Metric& m = metrics_[it->second];
        return m.type == MetricType::kHistogram ? m.h_min : m.min_v;
    }
    double max_of(const std::string& name, double def = 0.0) const {
        auto it = index_.find(name);
        if (it == index_.end()) return def;
        const Metric& m = metrics_[it->second];
        return m.type == MetricType::kHistogram ? m.h_max : m.max_v;
    }
    double quantile(const std::string& name, double q, double def = 0.0) const {
        auto it = index_.find(name);
        if (it == index_.end()) return def;
        const Metric& m = metrics_[it->second];
        if (m.type != MetricType::kHistogram || m.count == 0) return def;
        return m.quantile(q);
    }
    uint64_t count_of(const std::string& name) const {
        auto it = index_.find(name);
        if (it == index_.end()) return 0;
        return metrics_[it->second].count;
    }
    double mean_of(const std::string& name, double def = 0.0) const {
        auto it = index_.find(name);
        if (it == index_.end()) return def;
        const Metric& m = metrics_[it->second];
        return m.count ? m.sum / static_cast<double>(m.count) : def;
    }

    const std::vector<Metric>& all() const { return metrics_; }
    const Metric* find(const std::string& name) const {
        auto it = index_.find(name);
        return (it == index_.end()) ? nullptr : &metrics_[it->second];
    }

    // ---- 导出：Prometheus 文本格式 ----
    std::string to_prometheus() const {
        std::ostringstream os;
        for (const auto& m : metrics_) {
            const std::string n = sanitize(m.name);
            if (!m.help.empty()) os << "# HELP " << n << " " << m.help << "\n";
            os << "# TYPE " << n << " " << metric_type_name(m.type) << "\n";

            switch (m.type) {
                case MetricType::kCounter:
                case MetricType::kGauge:
                    os << n << " " << num(m.value) << "\n";
                    break;
                case MetricType::kHistogram: {
                    uint64_t acc = 0;
                    for (size_t i = 0; i < m.bounds.size(); ++i) {
                        acc += m.counts[i];
                        os << n << "_bucket{le=\"" << num(m.bounds[i]) << "\"} " << acc << "\n";
                    }
                    acc += m.counts.empty() ? 0 : m.counts.back();
                    os << n << "_bucket{le=\"+Inf\"} " << acc << "\n";
                    os << n << "_sum " << num(m.sum) << "\n";
                    os << n << "_count " << m.count << "\n";
                    break;
                }
            }
        }
        return os.str();
    }

    std::string to_json() const {
        std::ostringstream os;
        os << "{\n";
        for (size_t i = 0; i < metrics_.size(); ++i) {
            const Metric& m = metrics_[i];
            os << "  \"" << esc(m.name) << "\": {"
               << "\"type\": \"" << metric_type_name(m.type) << "\"";
            if (!m.unit.empty()) os << ", \"unit\": \"" << esc(m.unit) << "\"";
            if (m.type == MetricType::kHistogram) {
                os << ", \"count\": " << m.count
                   << ", \"sum\": " << m.sum
                   << ", \"min\": " << m.h_min
                   << ", \"max\": " << m.h_max
                   << ", \"p50\": " << m.quantile(0.50)
                   << ", \"p95\": " << m.quantile(0.95)
                   << ", \"p99\": " << m.quantile(0.99)
                   << ", \"mean\": " << (m.count ? m.sum / static_cast<double>(m.count) : 0.0);
            } else {
                os << ", \"value\": " << m.value;
                if (m.has_extremes)
                    os << ", \"min\": " << m.min_v << ", \"max\": " << m.max_v;
            }
            os << "}" << (i + 1 < metrics_.size() ? "," : "") << "\n";
        }
        os << "}\n";
        return os.str();
    }

private:
    Metric& ensure(const std::string& name, MetricType t,
                   const std::string& help = "", const std::string& unit = "") {
        auto it = index_.find(name);
        if (it != index_.end()) {
            Metric& m = metrics_[it->second];
            if (!help.empty()) m.help = help;
            if (!unit.empty()) m.unit = unit;
            return m;
        }
        Metric m;
        m.name = name;
        m.help = help;
        m.unit = unit;
        m.type = t;
        metrics_.push_back(std::move(m));
        index_[name] = metrics_.size() - 1;
        return metrics_.back();
    }

    // Prometheus 指标名只允许 [a-zA-Z_:][a-zA-Z0-9_:]*
    static std::string sanitize(const std::string& s) {
        std::string o;
        o.reserve(s.size() + 4);
        if (s.empty() || (!isalpha_(s[0]) && s[0] != '_')) o += "m_";
        for (char c : s) {
            o.push_back(isalnum_(c) ? c : '_');
        }
        return o;
    }
    static bool isalpha_(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }
    static bool isalnum_(char c) {
        return isalpha_(c) || (c >= '0' && c <= '9');
    }

    // 数字格式化：整数不带小数点，其余用 %g（与 P1 的 JSON 输出风格一致）
    static std::string num(double v) {
        if (!std::isfinite(v)) return "0";
        char buf[40];
        if (v == std::floor(v) && std::fabs(v) < 1e15) {
            std::snprintf(buf, sizeof(buf), "%.0f", v);
        } else {
            std::snprintf(buf, sizeof(buf), "%.10g", v);
        }
        return buf;
    }

    static std::string esc(const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
            else o.push_back(c);
        }
        return o;
    }

    std::vector<Metric>                     metrics_;
    std::unordered_map<std::string, size_t> index_;
};

} // namespace ems
