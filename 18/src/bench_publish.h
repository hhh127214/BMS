// =====================================================================
// 18/bench_publish.h — 基准 B5：全量重发 vs 优化后（C1/C2/C3 的代价）
//
// 被测对象：17/src/publish_pipeline.h 的 PublishPipeline（分级节拍 C3 +
//   死区 C2 + 批量写 C1）与它的对照基线 baseline_tick_with（= 修复前的
//   publish_all：每拍写全部点、每个点一次单点调用）。
//
// ---------------------------------------------------------------------
// 负载模型**照抄 17/ 自己的口径**（不是本模块编的）
// ---------------------------------------------------------------------
//   17/docs/README.md §4：全点表 102 点，档位点数 快 32 / 中 38 / 慢 32，
//   快档周期 0.1 s、中 1.0 s、慢 5.0 s，dt=0.5 s，600 拍；
//   17 个安全点全部落在快档且"死区 0 = 永远发布"（见 17/docs §2.7）。
//   动态工况下 17/ 实测：API 调用 61200 → 600（102×），
//   点值写入 61200 → 13184（4.6×），死区抑制率 0.5946。
//
//   本基准按同样的结构（102 点、32/38/32、17 安全点、dt=0.5、600 拍）
//   复现，并将档位比例按点表规模等比缩放（用于 CPU 口径的放大场景）。
//
// ---------------------------------------------------------------------
// ★ 为什么 CPU 判据不设成"≥5×"——踩过 17/ 已经警告过的坑
// ---------------------------------------------------------------------
//   17/docs/README.md「坑 6」原文：
//     "17 个安全点必须永远发布，且它们全在快档 —— 17×600=10200 次写入是
//      **不可压缩的地板**。全量重发 61200 ÷ 10200 ≈ 6.0× 才是点值写入降低
//      倍数的**理论上界**。原来那个'5 倍'既接近上界又没把这层结构说出来。"
//   本模块实测进一步发现：**CPU 时间**的降低倍数比"写入次数"更低 ——
//   优化路径对每个**到期**点要做两次字符串键 map 查找（死区表 + 取值表），
//   而到期点数 = 快档点数 ≈ 31%·N，与死区是否抑制无关。所以
//   CPU 的**地板** ≈ 2×0.31·N / (2·N) ≈ 0.31~0.45·基线，
//   而不是写入次数比 0.23。
//   → 故 CPU 判据取"必须有可测改善（≤0.90）"，并把"2× 预期未达成"
//     作为**诚实记录的发现**写进报告，而不是把阈值调到刚好能过的位置。
//
// ---------------------------------------------------------------------
// 值变化模型（按现场量纲，写清是为了让"死区是否该生效"可审计）
// ---------------------------------------------------------------------
//   快档（保护/状态 + 少量功率）：近静态 + 慢漂移，每拍 Δ≈0.008
//   中档（功率类）：             每拍 Δ≈3 kW
//   慢档（单体/电量/SOC）：      每拍 Δ≈0.0002
//   安全点（17 个）：            死区 0 → 每拍必发（地板，不可压缩）
//   若把**所有**点都设成"每拍大幅变化"，死区按定义就不起作用 ——
//   那是**制造**了 C2 无效的负载，不是现场负载。
// =====================================================================

#pragma once

#include "perf_clock.h"
#include "perf_stats.h"

#include "publish_pipeline.h"   // 17/

#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace ems {
namespace perf {

namespace dsim = ::ems::devsim;

struct PublishBenchArgs {
    int    points        = 102;    // 全点表点数（17/ 口径）
    int    ticks         = 600;    // 采样拍数（17/ 口径）
    double dt_s          = 0.5;    // 17/ 口径
    int    warmup        = 20;     // 丢弃前 N 拍（首拍冷启动全档齐发）
    double fast_s        = 0.1;    // 档位周期（17/ 口径）
    double medium_s      = 1.0;
    double slow_s        = 5.0;
    // 反向验证（SLA-P1）：把"优化路径"退化——死区全 0、全部点进快档，
    // 于是每拍全发，写入比 → 1.0，`writes×4 < base` 必红。
    bool   force_full_optimized = false;
    // 反向验证（SLA-P4）：在优化路径**计时区间内**注入忙等，模拟实现变慢。
    double opt_delay_us = 0.0;
};

struct PublishBenchResult {
    std::vector<double> base_tick_us;   // 基线逐拍墙钟（仅发布步）
    std::vector<double> opt_tick_us;    // 优化逐拍墙钟（仅发布步）
    double base_cpu_us_total = 0.0;     // 整循环（含夹具）进程 CPU 时间
    double opt_cpu_us_total  = 0.0;
    double base_loop_wall_us = 0.0;     // 整循环墙钟（含夹具）
    double opt_loop_wall_us  = 0.0;
    long long base_api_calls = 0;
    long long opt_api_calls  = 0;
    long long base_point_writes = 0;
    long long opt_point_writes  = 0;
    long long safety_point_writes = 0;      // 安全点地板（算出来的，非写死）
    long long opt_data_point_writes = 0;    // 扣除地板后的数据点写入
    long long base_data_point_writes = 0;
    double opt_suppress_rate = 0.0;
    double base_change_ratio = 0.0;         // opt_point_writes / base
    double data_change_ratio = 0.0;         // opt_data / base_data
    int    n_safety = 0, n_fast = 0, n_medium = 0, n_slow = 0;
    int    ticks_measured = 0;
    Summary base_sum;
    Summary opt_sum;
};

inline PublishBenchResult run_publish_bench(const PublishBenchArgs& a) {
    PublishBenchResult r;
    const int N = a.points;

    // ---- 档位划分：按 17/ 文档的 32/38/32 与 17 安全点**等比缩放** ----
    const int n_safety = std::max(1, (int)std::lround((double)N * 17.0 / 102.0));
    const int n_fast   = std::max(n_safety + 1,
                                  (int)std::lround((double)N * 32.0 / 102.0));
    const int n_medium = (int)std::lround((double)N * 38.0 / 102.0);
    // ★ 口径说明：17/docs §4 的「快 32」**含** 17 个安全点（安全点本身也是
    //   快档，只是死区为 0）。所以这里 n_fast 记**快档总数**，与文档同口径，
    //   且 n_fast + n_medium + n_slow == N 可自校验。一开始误记为 n_fast-n_safety，
    //   报告里出现「15/38/32」三档相加 = 85 ≠ 102，是本模块自己踩的坑。
    r.n_safety = n_safety;
    r.n_fast   = n_fast;
    r.n_medium = n_medium;
    r.n_slow   = N - n_fast - n_medium;
    if (r.n_slow < 0) r.n_slow = 0;

    std::vector<std::string> names;
    std::vector<int>         tier(N, dsim::TieredScheduler::kSlow);
    names.reserve((size_t)N);
    char nb[32];
    for (int i = 0; i < N; ++i) {
        std::snprintf(nb, sizeof(nb), "PTS.%05d", i);
        names.push_back(nb);
        if (i < n_fast)             tier[i] = dsim::TieredScheduler::kFast;
        else if (i < n_fast + n_medium) tier[i] = dsim::TieredScheduler::kMedium;
        else                        tier[i] = dsim::TieredScheduler::kSlow;
    }

    dsim::PublishPipeline pipe;
    dsim::CountingWriter  opt_writer;
    dsim::CountingWriter  base_writer;
    pipe.set_writer(&opt_writer);
    pipe.scheduler().set_period(dsim::TieredScheduler::kFast, a.fast_s);
    pipe.scheduler().set_period(dsim::TieredScheduler::kMedium, a.medium_s);
    pipe.scheduler().set_period(dsim::TieredScheduler::kSlow, a.slow_s);

    for (int i = 0; i < N; ++i) {
        if (a.force_full_optimized) {
            pipe.scheduler().assign(names[i], dsim::TieredScheduler::kFast);
            pipe.deadband().configure(names[i], 0.0, false);
            continue;
        }
        if (i < n_safety) {
            // 安全点：快档 + 死区 0 = 永远发布（不可压缩的地板）
            pipe.scheduler().assign(names[i], dsim::TieredScheduler::kFast);
            pipe.deadband().configure(names[i], 0.0, true);
        } else if (tier[i] == dsim::TieredScheduler::kFast) {
            pipe.scheduler().assign(names[i], dsim::TieredScheduler::kFast);
            pipe.deadband().configure(names[i], 0.05, false);
        } else if (tier[i] == dsim::TieredScheduler::kMedium) {
            pipe.scheduler().assign(names[i], dsim::TieredScheduler::kMedium);
            pipe.deadband().configure(names[i], 0.50, false);
        } else {
            pipe.scheduler().assign(names[i], dsim::TieredScheduler::kSlow);
            pipe.deadband().configure(names[i], 2.00, false);
        }
    }

    // ---- 值变化模型（按档位量纲；见文件头）----
    auto value_at = [&](int i, int k) -> double {
        if (i < n_safety) {
            // 安全点：值本身不重要（死区 0 → 必发），取 0/1 类位值
            return ((k / 7 + i) % 3 == 0) ? 1.0 : 0.0;
        }
        if (a.force_full_optimized) {
            return 100.0 + 40.0 * std::sin(i * 0.01 + k * 0.02);   // 全量扰动
        }
        if (tier[i] == dsim::TieredScheduler::kFast) {
            // 近静态 + 慢漂移：每拍 Δ ≈ 0.008
            return 1.0 + 0.4 * std::sin(0.01 * i + 0.02 * k);
        }
        if (tier[i] == dsim::TieredScheduler::kMedium) {
            // 功率类：每拍 Δ ≈ 3 kW
            return 100.0 + 20.0 * std::sin(0.05 * i + 0.15 * k);
        }
        // 慢档：单体电压/电量：每拍 Δ ≈ 0.0002
        return 3.2 + 0.02 * std::sin(0.01 * i + 0.01 * k);
    };

    std::map<std::string, double> base_values;
    r.base_tick_us.reserve((size_t)a.ticks);
    r.opt_tick_us.reserve((size_t)a.ticks);
    r.ticks_measured = a.ticks;

    // ---- 基线：全量单点写 ----
    // CPU 时间口径：逐拍墙钟只包住**发布步**（夹具不计入）；
    // process_cpu_us() 差值包住**整循环**（含夹具）。为了证明
    // "发布步墙钟 ≈ CPU"，同时记录整循环墙钟并算 cpu/wall 比值（应 ≈1.0）。
    const double base_loop_t0 = now_us();
    const double base_cpu0 = process_cpu_us();
    for (int k = 0; k < a.ticks; ++k) {
        for (int i = 0; i < N; ++i) base_values[names[i]] = value_at(i, k);
        const double t0 = now_us();
        dsim::PublishPipeline::baseline_tick_with(base_writer, base_values);
        const double t1 = now_us();
        r.base_tick_us.push_back(t1 - t0);
    }
    r.base_cpu_us_total = process_cpu_us() - base_cpu0;
    r.base_loop_wall_us = now_us() - base_loop_t0;
    r.base_api_calls    = base_writer.stats().calls;
    r.base_point_writes = base_writer.stats().point_writes;

    // ---- 优化：分级节拍 → 死区 → 批量写 ----
    const double opt_loop_t0 = now_us();
    const double opt_cpu0 = process_cpu_us();
    for (int k = 0; k < a.ticks; ++k) {
        for (int i = 0; i < N; ++i) pipe.set_value(names[i], value_at(i, k));
        const double t0 = now_us();
        pipe.tick(a.dt_s);
        if (a.opt_delay_us > 0.0) busy_wait_us(a.opt_delay_us);   // 计时区间内注入
        const double t1 = now_us();
        r.opt_tick_us.push_back(t1 - t0);
    }
    r.opt_cpu_us_total  = process_cpu_us() - opt_cpu0;
    r.opt_loop_wall_us  = now_us() - opt_loop_t0;
    r.opt_api_calls     = opt_writer.stats().calls;
    r.opt_point_writes  = opt_writer.stats().point_writes;

    // ---- 17/ 文档的两段式口径：地板与真实压缩力度分开算 ----
    // 地板 = 安全点每拍必发 → n_safety × ticks（**算出来**的，不是写死的常量）
    r.safety_point_writes    = (long long)n_safety * (long long)a.ticks;
    r.base_point_writes      = (long long)N * (long long)a.ticks;
    r.base_data_point_writes = r.base_point_writes - r.safety_point_writes;
    r.opt_data_point_writes  = r.opt_point_writes - r.safety_point_writes;
    if (r.opt_data_point_writes < 0) r.opt_data_point_writes = 0;
    r.base_change_ratio = (r.base_point_writes > 0)
        ? (double)r.opt_point_writes / (double)r.base_point_writes : 0.0;
    r.data_change_ratio = (r.base_data_point_writes > 0)
        ? (double)r.opt_data_point_writes / (double)r.base_data_point_writes : 0.0;
    {
        const long long due_est = (long long)n_fast * (long long)a.ticks + 0; // 快档必到期
        const long long pub_data =
            r.opt_point_writes - r.safety_point_writes;
        (void)due_est;
        r.opt_suppress_rate = (due_est > 0 && pub_data >= 0)
            ? 1.0 - (double)pub_data / (double)due_est : 0.0;
    }

    Policy p;
    p.warmup = (size_t)a.warmup;
    p.drop_outliers = false;
    r.base_sum = summarize(r.base_tick_us, p);
    r.opt_sum  = summarize(r.opt_tick_us, p);
    return r;
}

} // namespace perf
} // namespace ems
