// =====================================================================
// 18/perf_stats.h — 分位数 / 预热 / 离群值 / 多次重复取最差
//
// ---------------------------------------------------------------------
// 为什么"排序后取下标"不够
// ---------------------------------------------------------------------
// 「p99」不是一个数，是**一个族**。同一组样本，两种常见定义能差出可观的值：
//
//   · 最近秩 nearest-rank：rank = ceil(q·n)，取第 rank 个**实际样本**
//       p99(n=100) = sorted[98]     ← 一定是真实观测到的值
//   · 线性插值 linear-interp（R-7 / numpy 默认 / Excel PERCENTILE）：
//       h = (n-1)·q，在 sorted[⌊h⌋] 与 sorted[⌊h⌋+1] 之间插值
//       它可能落在两个样本**之间**，于是给出一个从未被观测到的数
//
// 对**性能门禁**而言选哪个不是风格问题：
//   · 门禁要回答"到底有没有超过 1% 的拍越界"。最近秩返回真实样本，
//     语义与"1% 分位"的直白解释一致，且**对 skew 分布更保守**（值更高）。
//   · 线性插值会平滑掉尾部，在小样本上系统性偏低 → 更容易"过"。
//
// 所以：**门禁用最近秩（SLA 默认 kNearestRank）**；线性插值一并计算并
// 上报，用于与外部工具（numpy/Excel）对口径 —— 两者的差额本身是一个
// 值得看的量（见 test_perf_stats.cpp 的 T05：小样本上差额可达 max−p95）。
//
// ---------------------------------------------------------------------
// 预热（warm-up）：丢多少、为什么
// ---------------------------------------------------------------------
// 首拍含**一次性的、不代表稳态的**代价：策略对象的惰性分配、页错误、
// 分支预测器冷启、std::string/vector 容量首次增长。12/ A4-02 实测首拍
// 可达数百 µs（远高于稳态 9.5 µs 均值两个数量级）。
//
// 本模块的策略（显式、可配）：
//   · **丢弃前 warmup 个样本**（闭环默认 1000；采集默认 200；编解码默认 500）
//     理由：1000 拍 @10 Hz = 100 s，足够覆盖惰性分配与页错误；
//     且 1000/20000 = 5% 的丢弃量不会动摇 p99（p99 需 ≥100 个尾部样本，
//     20000 拍有 200 个）。
//   · **冷启动峰值单独上报**（Summary::cold_max），不混进稳态分位数。
//     这样"稳态 p99"与"含冷启动 max"两个口径都能看到，与 12/ A4-02 可对照。
//
// ---------------------------------------------------------------------
// 离群值（outlier）：检测，但**默认不剔除**
// ---------------------------------------------------------------------
// 这是本模块最容易被做错的一处。很多基准框架默认"剔除离群值后取均值"，
// 在本项目里这是**危险的**：一次 50 ms 的尖峰（GC 式停顿、页换出、
// OS 调度抢占）就足以让 10 Hz 闭环错过一拍 —— 而剔除它会把这件事故
// 从统计里抹掉，让报告显示"一切正常"。
//
// 本模块的策略：
//   · 用 **median + 1.4826·MAD** 做稳健 z 检验，**标记**离群点并计数、
//     上报其量级；
//   · **默认不剔除**（Policy::drop_outliers = false）—— 分位数与 max
//     在全部 post-warmup 样本上计算；
//   · 若调用方显式 drop_outliers=true，Summary 会如实记录
//     n_outliers_dropped，报告里也必须出现该字段（不允许静默丢弃）。
//   · 均值额外给一个**稳健均值**（截尾 10%），用于"这组数的主体在哪"，
//     但它**不参与任何 SLA 门禁**。
//
// 编译：纯头文件。
// =====================================================================

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace ems {
namespace perf {

// =====================================================================
// 分位数定义
// =====================================================================
enum class QuantileMethod {
    kNearestRank = 0,   // rank = ceil(q·n) → 实际样本（门禁默认）
    kLinearInterp = 1   // R-7 / numpy / Excel 默认 → 可能非观测值
};

inline const char* quantile_method_name(QuantileMethod m) {
    return (m == QuantileMethod::kNearestRank) ? "nearest-rank"
                                               : "linear-interp(R-7)";
}

// s 必须已升序。q ∈ [0,1]。
inline double quantile_sorted(const std::vector<double>& s, double q,
                              QuantileMethod m) {
    const std::size_t n = s.size();
    if (n == 0) return 0.0;
    if (n == 1) return s[0];
    if (q <= 0.0) return s.front();
    if (q >= 1.0) return s.back();

    if (m == QuantileMethod::kNearestRank) {
        long long rank = (long long)std::ceil(q * (double)n);
        if (rank < 1) rank = 1;
        if (rank > (long long)n) rank = (long long)n;
        return s[(std::size_t)(rank - 1)];
    }
    const double h = (double)(n - 1) * q;
    long long lo = (long long)std::floor(h);
    long long hi = lo + 1;
    if (hi >= (long long)n) return s.back();
    const double frac = h - (double)lo;
    return s[(std::size_t)lo] + frac * (s[(std::size_t)hi] - s[(std::size_t)lo]);
}

// =====================================================================
// 统计口径与策略
// =====================================================================
struct Policy {
    std::size_t    warmup         = 0;      // 丢弃前 N 个样本
    QuantileMethod method         = QuantileMethod::kNearestRank;
    bool           drop_outliers  = false;  // 默认 false（见文件头）
    double         outlier_k      = 6.0;    // MAD 稳健 z 阈值（仅用于标记）
    bool           sample_stddev  = true;   // n-1（样本）而非 n（总体）
};

struct Summary {
    std::size_t    n_raw              = 0;   // 原始样本数
    std::size_t    n_warmup_dropped   = 0;
    std::size_t    n_outliers_flagged = 0;   // 被标记（未必剔除）
    std::size_t    n_outliers_dropped = 0;   // 实际剔除数（默认 0）
    std::size_t    n_used             = 0;   // 参与分位数的样本数

    QuantileMethod method             = QuantileMethod::kNearestRank;

    double min     = 0.0;
    double p50     = 0.0;
    double p90     = 0.0;
    double p95     = 0.0;
    double p99     = 0.0;
    double p999    = 0.0;
    double max     = 0.0;
    double mean    = 0.0;
    double stddev  = 0.0;
    double cv_pct  = 0.0;
    double mean_robust_trunc10 = 0.0;   // 截尾 10% 稳健均值（不参与门禁）

    // 对照：另一套分位数定义下的 p99（用于报告差额）
    double p99_alt = 0.0;

    // 冷启动（预热段）峰值：单独口径，与 12/ A4-02 对照
    double cold_max = 0.0;
    bool   has_cold = false;

    double of(const std::string& key) const {
        if (key == "min")  return min;
        if (key == "p50")  return p50;
        if (key == "p90")  return p90;
        if (key == "p95")  return p95;
        if (key == "p99")  return p99;
        if (key == "p99.9")return p999;
        if (key == "p999") return p999;
        if (key == "max")  return max;
        if (key == "mean") return mean;
        if (key == "stddev") return stddev;
        if (key == "cv_pct") return cv_pct;
        return 0.0;
    }
};

// 稳健 z 标记：|x - median| > k · 1.4826·MAD
inline void flag_outliers(const std::vector<double>& s, double k,
                          std::vector<char>& is_out) {
    const std::size_t n = s.size();
    is_out.assign(n, 0);
    if (n < 8) return;   // 样本太少，MAD 不稳，不标
    std::vector<double> tmp = s;
    std::sort(tmp.begin(), tmp.end());
    const double med = quantile_sorted(tmp, 0.5, QuantileMethod::kNearestRank);
    std::vector<double> dev(n);
    for (std::size_t i = 0; i < n; ++i) dev[i] = std::fabs(s[i] - med);
    std::sort(dev.begin(), dev.end());
    const double mad = quantile_sorted(dev, 0.5, QuantileMethod::kNearestRank);
    if (mad <= 0.0) return;      // 半数样本同值 → MAD=0，不做标记
    const double sigma = 1.4826 * mad;
    const double thr   = k * sigma;
    for (std::size_t i = 0; i < n; ++i)
        if (std::fabs(s[i] - med) > thr) is_out[i] = 1;
}

// 汇总。samples 为**逐次测量**（µs），顺序即时间顺序（预热取前 N）。
inline Summary summarize(const std::vector<double>& samples,
                         const Policy& pol = Policy()) {
    Summary r;
    r.n_raw  = samples.size();
    r.method = pol.method;
    if (samples.empty()) return r;

    const std::size_t w = std::min(pol.warmup, samples.size());

    // 冷启动峰值：预热段的最大值（若预热为 0 则无此口径）
    if (w > 0) {
        r.has_cold = true;
        for (std::size_t i = 0; i < w; ++i)
            r.cold_max = std::max(r.cold_max, samples[i]);
    }
    r.n_warmup_dropped = w;

    std::vector<double> used(samples.begin() + w, samples.end());
    if (used.empty()) used.assign(1, samples.back());

    // 离群标记（在 post-warmup 样本上）
    std::vector<char> is_out;
    flag_outliers(used, pol.outlier_k, is_out);
    for (std::size_t i = 0; i < is_out.size(); ++i)
        if (is_out[i]) ++r.n_outliers_flagged;

    if (pol.drop_outliers && r.n_outliers_flagged > 0) {
        std::vector<double> kept;
        kept.reserve(used.size());
        for (std::size_t i = 0; i < used.size(); ++i)
            if (!is_out[i]) kept.push_back(used[i]);
        r.n_outliers_dropped = used.size() - kept.size();
        if (!kept.empty()) used.swap(kept);
    }

    std::sort(used.begin(), used.end());
    r.n_used = used.size();

    r.min  = used.front();
    r.max  = used.back();
    r.p50  = quantile_sorted(used, 0.50,  pol.method);
    r.p90  = quantile_sorted(used, 0.90,  pol.method);
    r.p95  = quantile_sorted(used, 0.95,  pol.method);
    r.p99  = quantile_sorted(used, 0.99,  pol.method);
    r.p999 = quantile_sorted(used, 0.999, pol.method);

    const QuantileMethod other =
        (pol.method == QuantileMethod::kNearestRank)
            ? QuantileMethod::kLinearInterp
            : QuantileMethod::kNearestRank;
    r.p99_alt = quantile_sorted(used, 0.99, other);

    double sum = 0.0;
    for (std::size_t i = 0; i < used.size(); ++i) sum += used[i];
    r.mean = sum / (double)used.size();

    double ss = 0.0;
    for (std::size_t i = 0; i < used.size(); ++i) {
        const double d = used[i] - r.mean;
        ss += d * d;
    }
    const double denom = pol.sample_stddev
                             ? (double)(used.size() > 1 ? used.size() - 1 : 1)
                             : (double)used.size();
    r.stddev = std::sqrt(ss / denom);
    r.cv_pct = (r.mean > 1e-12) ? 100.0 * r.stddev / r.mean : 0.0;

    // 截尾 10% 稳健均值（两端各去 10%，至少保留 1 个）
    std::size_t cut = (std::size_t)(0.10 * (double)used.size());
    if (used.size() - 2 * cut < 1) cut = 0;
    double tsum = 0.0;
    std::size_t tn = 0;
    for (std::size_t i = cut; i + cut < used.size(); ++i) { tsum += used[i]; ++tn; }
    r.mean_robust_trunc10 = tn ? tsum / (double)tn : r.mean;

    return r;
}

// =====================================================================
// 多次重复 + 取最差（"性能测试本身不稳定"的对策）
//
// 口径：同一条 SLA 在 R 次独立重复里各自测量 → **取最差的那次**参与门禁。
// 为什么取最差而不是取平均：门禁要保证的是"任何一次跑都过"，
// 平均会掩盖"三次里有一次红"；而 CI 只跑一次，那一次就可能是红的那次。
// 取最差 = 用 R 次里的悲观值近似"一次跑的置信上界"。
// R 的建议值见每类基准（闭环 R=3，编解码 R=5 —— 越便宜越多次）。
// =====================================================================
struct RepeatResult {
    int    repeats     = 0;
    double worst_value = 0.0;   // 最差（最大）观测量
    double best_value  = 0.0;
    double median_value = 0.0;
    int    worst_run   = -1;    // 第几次最差（1-based）
    std::vector<double> per_run;
};

inline RepeatResult worst_of(const std::vector<double>& per_run) {
    RepeatResult r;
    r.repeats = (int)per_run.size();
    r.per_run = per_run;
    if (per_run.empty()) return r;
    r.worst_value = per_run[0];
    r.best_value  = per_run[0];
    r.worst_run   = 1;
    for (std::size_t i = 1; i < per_run.size(); ++i) {
        if (per_run[i] > r.worst_value) { r.worst_value = per_run[i]; r.worst_run = (int)i + 1; }
        if (per_run[i] < r.best_value)  r.best_value = per_run[i];
    }
    std::vector<double> c = per_run;
    std::sort(c.begin(), c.end());
    r.median_value = quantile_sorted(c, 0.5, QuantileMethod::kNearestRank);
    return r;
}

} // namespace perf
} // namespace ems
