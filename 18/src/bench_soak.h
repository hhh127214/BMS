// =====================================================================
// 18/bench_soak.h — 基准 B6：24h 长跑（加速比 / 内存有界 / 不随运行时长退化）
//
// 口径说明（必须写清，否则数字会被误读）：
//   本基准按任务要求跑 **86400 拍 @ dt=0.1 s**，即仿真 8640 s（2.4 h）。
//   这与 12/ A4-05 的「86400 拍 @ dt=1 s（= 24 h）」**不是同一口径**：
//     · A4-05 关心"24 h 离线仿真多久跑完" → 步长 1 s，每步算得粗
//     · 本基准关心"10 Hz 真实节拍下跑满 86400 拍" → 步长 0.1 s，每拍
//       都是完整闭环，是**更重**的负载，更适合暴露"随时间退化"。
//   报告里两个数都给出加速比，但分别标注仿真时长，避免混口径。
//
// 最后一条判据（SLA-L2）才是这类系统的真问题：
//   内存泄漏 / 容器膨胀 / 缓存膨胀都不会让 p50 变差，只会让**尾部**变差。
//   所以判据是「末 1000 拍 p99 / 头 1000 拍 p99」，而不是看均值。
// =====================================================================

#pragma once

#include "perf_clock.h"
#include "perf_stats.h"

#include "realtime_loop.h"   // 07/

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <utility>
#include <vector>

namespace ems {
namespace perf {

struct SoakArgs {
    int    steps            = 86400;
    double dt_s             = 0.1;
    int    warmup           = 1000;   // 预热段（不计入头/尾窗口，但计入样本向量）
    int    window           = 1000;   // 头/尾窗口长度
    int    mem_sample_every = 5000;   // 内存采样间隔（拍）
    bool   log_every_1      = true;
    std::function<double(int)> delay_after_step_us;   // 反向验证注入
};

struct SoakResult {
    int    steps        = 0;
    double dt_s         = 0.0;
    double sim_s        = 0.0;   // 仿真时长 = steps × dt_s
    double wall_s       = 0.0;   // 墙钟
    double speedup      = 0.0;   // sim_s / wall_s

    double rss_start_mb = 0.0;
    double rss_end_mb   = 0.0;
    double rss_max_mb   = 0.0;
    double rss_growth_mb = 0.0;  // end - start
    long long handle_delta = 0;

    std::vector<double> step_us;              // 逐拍样本（µs）
    std::vector<std::pair<int,double>> rss_series;   // (拍号, RSS MB)

    Summary head;      // 预热后第 1..window 拍
    Summary tail;      // 最后 window 拍
    Summary all;       // 全部 post-warmup
    double tail_over_head_p99 = 0.0;   // 原始单窗比值（**报告用**，见下）

    // ★ 稳健口径（SLA-L2 门禁用）：把 post-warmup 样本切成 K 个等长子窗，逐窗取
    //   p99，再用「首/末各 1/4 子窗 p99 的中位数」作比值。
    //
    //   为什么不能直接用单个 1000 样本窗的 p99 作比值：p99 是第 10 差的样本，
    //   一次 OS 调度抖动落进窗口就能把它抬 3~5 倍。实测**同一二进制连跑 5 次**，
    //   head p99 在 5.5~39 µs、tail p99 在 7.8~35 µs 之间跳，ratio 在 0.20~3.42
    //   之间翻 —— 那测的是"抖动恰好落在头窗还是尾窗"，与"性能有没有随时间长退"
    //   无关（本机 32 核，但后台进程多，抖动不可避免）。取中位数后单窗抖动被压掉，
    //   而**真退化**（内存泄漏 / 容器膨胀会让末段持续变慢）仍会让末段中位数抬高。
    int    win_count          = 0;     // 子窗个数（0 表示样本不足，未算）
    double head_win_med       = 0.0;   // 首 1/4 子窗 p99 的中位数
    double tail_win_med       = 0.0;   // 末 1/4 子窗 p99 的中位数
    double tail_over_head_med = 0.0;   // 门禁用的比值
};

// 中位数（对副本排序后取 nearest-rank 0.5；样本少时即中间那个）
inline double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return quantile_sorted(v, 0.5, QuantileMethod::kNearestRank);
}

inline SoakResult run_soak_bench(const SoakArgs& a) {
    SoakResult r;
    r.steps  = a.steps;
    r.dt_s   = a.dt_s;
    r.sim_s  = (double)a.steps * a.dt_s;

    EmsRuntime rt;
    rt.config().dt_s = a.dt_s;
    rt.config().log_every = a.log_every_1 ? 1 : 10;
    rt.apply_configs();
    rt.fsm().request_run(true);

    r.rss_start_mb = rss_mb();
    r.rss_max_mb   = r.rss_start_mb;
    const long long h0 = handle_count();

    r.step_us.reserve((size_t)a.steps);
    const double w0 = now_us();
    for (int i = 0; i < a.steps; ++i) {
        rt.set_environment(320.0 + 110.0 * std::sin(i * 0.003),
                           140.0 + 70.0 * std::sin(i * 0.017));
        const double t0 = now_us();
        rt.step(a.dt_s);
        // 注入延迟在计时区间内（见 bench_closed_loop.h 的同款说明）
        if (a.delay_after_step_us) {
            const double d = a.delay_after_step_us(i);
            if (d > 0.0) busy_wait_us(d);
        }
        const double t1 = now_us();
        r.step_us.push_back(t1 - t0);
        if (a.mem_sample_every > 0 && (i % a.mem_sample_every) == 0) {
            const double m = rss_mb();
            if (m > r.rss_max_mb) r.rss_max_mb = m;
            r.rss_series.push_back(std::make_pair(i, m));
        }
    }
    const double w1 = now_us();
    r.wall_s = (w1 - w0) / 1e6;
    r.speedup = (r.wall_s > 1e-9) ? (r.sim_s / r.wall_s) : 0.0;

    r.rss_end_mb = rss_mb();
    if (r.rss_end_mb > r.rss_max_mb) r.rss_max_mb = r.rss_end_mb;
    r.rss_growth_mb = r.rss_end_mb - r.rss_start_mb;
    r.handle_delta  = handle_count() - h0;
    r.rss_series.push_back(std::make_pair(a.steps, r.rss_end_mb));

    Policy p;
    p.warmup = 0;
    p.drop_outliers = false;
    r.all = summarize(r.step_us, p);

    // 头/尾窗口（均在预热之后）
    const size_t w = (size_t)a.window;
    const size_t n = r.step_us.size();
    if (n > (size_t)a.warmup + 2 * w) {
        std::vector<double> head(r.step_us.begin() + a.warmup,
                                 r.step_us.begin() + a.warmup + (long)w);
        std::vector<double> tail(r.step_us.end() - (long)w, r.step_us.end());
        Policy q; q.warmup = 0; q.drop_outliers = false;
        r.head = summarize(head, q);
        r.tail = summarize(tail, q);
        r.tail_over_head_p99 =
            (r.head.p99 > 1e-12) ? (r.tail.p99 / r.head.p99) : 0.0;
    }

    // 稳健口径（SLA-L2 门禁）：K 个等长子窗，首/末 1/4 的 p99 取中位数（见 SoakResult 说明）
    const int K = 16;
    const size_t post = (n > (size_t)a.warmup) ? (n - (size_t)a.warmup) : 0;
    if (K >= 4 && post >= (size_t)K * 50) {
        const size_t wlen = post / (size_t)K;
        Policy q; q.warmup = 0; q.drop_outliers = false;
        std::vector<double> wp99;
        wp99.reserve((size_t)K);
        for (int k = 0; k < K; ++k) {
            const size_t b = (size_t)a.warmup + (size_t)k * wlen;
            std::vector<double> wv(r.step_us.begin() + (long)b,
                                   r.step_us.begin() + (long)(b + wlen));
            wp99.push_back(summarize(wv, q).p99);
        }
        const int q4 = K / 4;
        std::vector<double> hw(wp99.begin(), wp99.begin() + q4);
        std::vector<double> tw(wp99.begin() + (K - q4), wp99.end());
        r.win_count    = K;
        r.head_win_med = median_of(hw);
        r.tail_win_med = median_of(tw);
        r.tail_over_head_med =
            (r.head_win_med > 1e-12) ? (r.tail_win_med / r.head_win_med) : 0.0;
    }
    return r;
}

} // namespace perf
} // namespace ems
