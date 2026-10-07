// =====================================================================
// 18/bench_closed_loop.h — 基准 B1：单拍闭环（10 Hz 的命门）
//
// 被测对象：EmsRuntime::step(0.1) 的端到端时延（07/src/realtime_loop.h）
//   11 步编排：采集 → 故障 → 安全预判 → 状态机 → 协同 → 策略 ×9 →
//   仲裁 → L2 纠偏 → 整形 → 安全兜底 → 门控 → 执行 → 记录
//
// 为什么在**外部**计时而不是复用 step() 内部的 cycle_us_sum_：
//   · 内部计时把两次 steady_clock 读也算进"单拍"，但那两次读本身是
//     被测代码的一部分（真实闭环里也是它自己读的）—— 外部计时得到的是
//     "调用方观测到的单拍时延"，正是 SLA 要约束的量；
//   · 外部计时同时给出逐拍样本，才能算 p50/p99 与头尾对比（内部只有
//     sum 与 max，算不出分位数）。
//   两者在 SLA-C3 上可交叉校验（差值即内部两次读的开销）。
//
// 环境注入（set_environment）放在**计时区间之外** —— 它是夹具，不是
// 被测运行时的一部分。
// =====================================================================

#pragma once

#include "perf_clock.h"
#include "perf_stats.h"

#include "realtime_loop.h"   // 07/  EmsRuntime

#include <cmath>
#include <functional>
#include <vector>

namespace ems {
namespace perf {

struct LoopBenchArgs {
    int  steps          = 20000;   // 采样拍数
    double dt_s         = 0.1;
    bool log_every_1    = true;    // 与 12/ A4 同口径（每拍记一条）
    bool with_environment = true;  // 注入负荷/光伏曲线（与 A4 同口径）
    // 反向验证钩子：第 i 拍 step() 之后额外忙等 us_delay(i) 微秒。
    std::function<double(int)> delay_after_step_us;
};

struct LoopBenchResult {
    std::vector<double> step_us;      // 逐拍样本（µs），含预热段
    double first_step_us  = 0.0;      // 首拍（冷启动）
    double max_all_us     = 0.0;      // 含预热段的峰值（SLA-C5 口径）
    int    steps          = 0;
    double injected_us_total = 0.0;   // 注入的延迟总量（便于审计）
};

inline LoopBenchResult run_closed_loop_bench(const LoopBenchArgs& a) {
    LoopBenchResult r;
    r.steps = a.steps;
    r.step_us.reserve((size_t)a.steps);

    EmsRuntime rt;
    rt.config().dt_s = a.dt_s;
    rt.config().log_every = a.log_every_1 ? 1 : 10;
    rt.apply_configs();
    rt.fsm().request_run(true);

    for (int i = 0; i < a.steps; ++i) {
        if (a.with_environment) {
            rt.set_environment(320.0 + 110.0 * std::sin(i * 0.003),
                               140.0 + 70.0 * std::sin(i * 0.017));
        }
        const double t0 = now_us();
        rt.step(a.dt_s);
        // ★ 注入延迟必须在**计时区间内** —— 否则测到的是"调用方变慢"，
        //   而不是"step() 变慢"，反向验证就失去意义（BUG 记录见 README）。
        if (a.delay_after_step_us) {
            const double d = a.delay_after_step_us(i);
            if (d > 0.0) { busy_wait_us(d); r.injected_us_total += d; }
        }
        const double t1 = now_us();
        const double us = t1 - t0;
        if (i == 0) {
            r.first_step_us = us;
            r.max_all_us = us;
        }
        r.step_us.push_back(us);
    }
    for (size_t i = 0; i < r.step_us.size(); ++i)
        r.max_all_us = std::max(r.max_all_us, r.step_us[i]);
    return r;
}

// 便捷：按策略汇总
inline Summary summarize_loop(const LoopBenchResult& r, int warmup) {
    Policy p;
    p.warmup = (size_t)warmup;
    p.method = QuantileMethod::kNearestRank;
    p.drop_outliers = false;      // 性能门禁不剔除尖峰（见 perf_stats.h）
    return summarize(r.step_us, p);
}

} // namespace perf
} // namespace ems
