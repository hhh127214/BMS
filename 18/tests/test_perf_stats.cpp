// =====================================================================
// 18/tests/test_perf_stats.cpp — 计时与统计基础设施的单元测试（确定性）
//
// 为什么这些用例必须存在（而不只是"跑一遍基准看看数"）：
//   性能基准最怕的不是"数不准"，而是**统计口径悄悄变了**：
//   分位数定义换了、预热窗口没了、离群值被静默剔除 —— 这些都不会
//   让基准报错，只会让数字变得"更好看"。本文件把口径钉死成断言。
//
// 用例：
//   T01 分位数两定义在 skew 小样本上确有差异（差额可量化）
//   T02 最近秩 p99 一定是**实际观测样本**（插值不是）
//   T03 预热丢弃前 N 个（且冷启动峰值单独口径）
//   T04 离群值：默认只标记不剔除；显式开启才剔除
//   T05 均值 / 样本标准差 / 截尾稳健均值正确
//   T06 分位数单调性 p50<=p90<=p95<=p99<=p999<=max
//   T07 worst_of 取最差（多次重复口径）
//   T08 门禁求值逻辑：绝对 / 比值 / **分母为 0 必须判失败**
//   T09 时钟自证：实测粒度 >0 且 is_steady
//   T10 busy_wait 确实等到了指定量级（反向验证的注入手段本身可信）
//   T11 计时器噪声地板可测（读一对时钟的开销上界）
// =====================================================================

#include "perf_clock.h"
#include "perf_stats.h"
#include "sla.h"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <vector>

using namespace ems::perf;

static int g_pass = 0;
static int g_fail = 0;

// 落盘**纯 ASCII** 状态行，供 .bat 汇总用（照 13/tests 的做法）。
// 为什么不让 .bat 去 grep stdout：本模块输出里中英文混排，cmd 用 CP936 抓
// UTF-8 管道会乱码/截断，一旦 "PASS=" 那行没抓到，脚本会**静默**把断言数
// 当 0 上报 —— 那正是本项目最忌讳的失败（看起来跑过、其实是空的）。
// 每行一个 key=value、分隔符只有 '='，避免 for /f 的 delims 里空格歧义。
static bool write_status(const char* path, int p, int f, int s) {
    std::FILE* fp = std::fopen(path, "wb");
    if (!fp) return false;
    std::fprintf(fp, "PASS=%d\nFAIL=%d\nSKIPPED=%d\n", p, f, s);
    std::fclose(fp);
    return true;
}

static void dump_status(int p, int f, int s) {
    // 三级回退：从 18/ 启动 → build/；从别处启动 → ../build/ → 当前目录
    if (write_status("build/status_perf_stats.txt", p, f, s)) return;
    if (write_status("../build/status_perf_stats.txt", p, f, s)) return;
    write_status("status_perf_stats.txt", p, f, s);
}

#define EXPECT(cond)                                                      \
    do {                                                                  \
        if (cond) {                                                       \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #cond << std::endl;                     \
        }                                                                 \
    } while (0)

#define EXPECT_NEAR(a, b, eps)                                            \
    do {                                                                  \
        double va = (a), vb = (b);                                        \
        if (std::fabs(va - vb) <= (eps)) {                                \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va << " vs " << #b << "="  \
                      << vb << " (eps " << (eps) << ")" << std::endl;     \
        }                                                                 \
    } while (0)

#define EXPECT_EQ(a, b)                                                   \
    do {                                                                  \
        long long va = (long long)(a), vb = (long long)(b);               \
        if (va == vb) {                                                   \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va << " vs " << #b << "="  \
                      << vb << std::endl;                                 \
        }                                                                 \
    } while (0)

// =====================================================================
// T01 分位数两定义的差异（skew 小样本）
// =====================================================================
static void test_01_quantile_definitions() {
    // 99 个 0 + 1 个 1000，n=100。
    //   最近秩 p99 = ceil(0.99*100)=99 → sorted[98] = 0
    //   线性插值  h=0.99*99=98.01 → 0 + 0.01*(1000-0) = 10
    std::vector<double> s(100, 0.0);
    s[99] = 1000.0;
    const double nr = quantile_sorted(s, 0.99, QuantileMethod::kNearestRank);
    const double li = quantile_sorted(s, 0.99, QuantileMethod::kLinearInterp);
    EXPECT_NEAR(nr, 0.0, 1e-9);
    EXPECT_NEAR(li, 10.0, 1e-6);
    // 两者之差 = 10，说明"排序后取下标"与"插值"不是一回事
    EXPECT(std::fabs(li - nr) > 9.9);
    // 单侧：插值结果不是任何观测样本
    bool li_is_observed = false;
    for (double v : s) if (std::fabs(v - li) < 1e-9) li_is_observed = true;
    EXPECT(!li_is_observed);
}

// =====================================================================
// T02 最近秩 p99 一定是实际观测样本
// =====================================================================
static void test_02_nearest_rank_is_observed() {
    std::vector<double> s;
    for (int i = 0; i < 137; ++i) s.push_back(1.0 + (double)(i % 13) * 3.5);
    // quantile_sorted 要求输入已升序（见 perf_stats.h 的注释）
    std::vector<double> c = s;
    std::sort(c.begin(), c.end());
    const double q = quantile_sorted(c, 0.99, QuantileMethod::kNearestRank);
    bool found = false;
    for (double v : s) if (v == q) found = true;
    EXPECT(found);
    // 137 样本的 p99：rank = ceil(0.99*137) = ceil(135.63) = 136 → 第 136 小
    EXPECT_NEAR(q, c[135], 1e-12);
}

// =====================================================================
// T03 预热丢弃 + 冷启动峰值单独口径
// =====================================================================
static void test_03_warmup() {
    std::vector<double> s;
    for (int i = 0; i < 10; ++i) s.push_back(1000.0);   // 冷启动区
    for (int i = 0; i < 100; ++i) s.push_back(10.0);    // 稳态
    Policy p; p.warmup = 10;
    Summary r = summarize(s, p);
    EXPECT_EQ(r.n_raw, 10 + 100);
    EXPECT_EQ(r.n_warmup_dropped, 10);
    EXPECT_EQ(r.n_used, 100);
    EXPECT_NEAR(r.mean, 10.0, 1e-9);
    EXPECT_NEAR(r.max, 10.0, 1e-9);
    EXPECT(r.has_cold);
    EXPECT_NEAR(r.cold_max, 1000.0, 1e-9);   // 冷启动峰值单独上报
}

// =====================================================================
// T04 离群值：默认只标记不剔除
// =====================================================================
static void test_04_outliers() {
    std::vector<double> s;
    for (int i = 0; i < 200; ++i) s.push_back(100.0 + (double)(i % 7) * 0.1);
    s.push_back(1e6);      // 3 个极端离群
    s.push_back(2e6);
    s.push_back(3e6);

    Policy p; p.warmup = 0; p.drop_outliers = false;
    Summary a = summarize(s, p);
    EXPECT(a.n_outliers_flagged >= 3);
    EXPECT_EQ(a.n_outliers_dropped, 0);           // 默认不剔
    EXPECT_NEAR(a.max, 3e6, 1.0);                  // 尖峰保留在 max 里

    Policy q; q.warmup = 0; q.drop_outliers = true;
    Summary b = summarize(s, q);
    EXPECT(b.n_outliers_dropped >= 3);
    EXPECT(b.max < 1e5);                           // 剔除后 max 回落到主体
    EXPECT(b.n_outliers_flagged == a.n_outliers_flagged);
}

// =====================================================================
// T05 均值 / 样本标准差 / 截尾稳健均值
// =====================================================================
static void test_05_mean_stddev() {
    std::vector<double> s;
    for (int i = 1; i <= 5; ++i) s.push_back((double)i);   // 1..5
    Policy p; p.warmup = 0; p.sample_stddev = true;
    Summary r = summarize(s, p);
    EXPECT_NEAR(r.mean, 3.0, 1e-12);
    EXPECT_NEAR(r.stddev, std::sqrt(2.5), 1e-12);   // 样本 sd = sqrt(10/4)
    EXPECT_NEAR(r.mean_robust_trunc10, 3.0, 1e-12);
    // 总体标准差对照
    Policy q; q.warmup = 0; q.sample_stddev = false;
    Summary r2 = summarize(s, q);
    EXPECT_NEAR(r2.stddev, std::sqrt(2.0), 1e-12);  // sqrt(10/5)
}

// =====================================================================
// T06 分位数单调性
// =====================================================================
static void test_06_monotonicity() {
    std::vector<double> s;
    unsigned seed = 12345;
    for (int i = 0; i < 5000; ++i) {
        seed = seed * 1103515245u + 12345u;
        s.push_back((double)(seed % 1000));
    }
    Policy p; p.warmup = 0;
    Summary r = summarize(s, p);
    EXPECT(r.min <= r.p50);
    EXPECT(r.p50 <= r.p90);
    EXPECT(r.p90 <= r.p95);
    EXPECT(r.p95 <= r.p99);
    EXPECT(r.p99 <= r.p999);
    EXPECT(r.p999 <= r.max);
}

// =====================================================================
// T07 worst_of 取最差
// =====================================================================
static void test_07_worst_of() {
    std::vector<double> runs = {3.0, 7.0, 5.0, 9.0, 2.0};
    RepeatResult r = worst_of(runs);
    EXPECT_EQ(r.repeats, 5);
    EXPECT_NEAR(r.worst_value, 9.0, 1e-12);
    EXPECT_NEAR(r.best_value, 2.0, 1e-12);
    EXPECT_NEAR(r.median_value, 5.0, 1e-12);
    EXPECT_EQ(r.worst_run, 4);
}

// =====================================================================
// T08 门禁求值逻辑
// =====================================================================
static void test_08_gate_logic() {
    SlaRule up; up.id = "X1"; up.dir = SlaDir::kUpperBound; up.limit = 10.0;
    EXPECT(evaluate_abs(up, 5.0).pass);
    EXPECT(evaluate_abs(up, 10.0).pass);      // 边界 ≤ 通过
    EXPECT(!evaluate_abs(up, 10.001).pass);

    SlaRule lo; lo.id = "X2"; lo.dir = SlaDir::kLowerBound; lo.limit = 1000.0;
    EXPECT(evaluate_abs(lo, 2000.0).pass);
    EXPECT(!evaluate_abs(lo, 999.0).pass);

    SlaRule rt; rt.id = "X3"; rt.dir = SlaDir::kRatioUpper; rt.limit = 0.5;
    EXPECT(evaluate_ratio(rt, 50.0, 100.0).pass);
    EXPECT(evaluate_ratio(rt, 50.0, 100.0).measured == 0.5);
    EXPECT(!evaluate_ratio(rt, 60.0, 100.0).pass);

    // ★ 分母为 0 必须判失败（不许静默通过）—— 本项目踩过的"除零骗过断言"
    SlaOutcome z = evaluate_ratio(rt, 60.0, 0.0);
    EXPECT(!z.pass);
    EXPECT(!z.note.empty());

    // 余量计算
    SlaOutcome m = evaluate_abs(up, 5.0);
    EXPECT_NEAR(m.headroom_x(), 2.0, 1e-12);
    EXPECT_NEAR(m.headroom_abs(), 5.0, 1e-12);
}

// =====================================================================
// T09 时钟自证
// =====================================================================
static void test_09_clock_probe() {
    const ClockProbe& p = cached_clock_probe();
    EXPECT(p.is_steady);
    EXPECT(p.observed_granularity_ns > 0.0);
    EXPECT(p.observed_granularity_ns <= 1e6);   // 不应比 1 ms 还粗
    EXPECT(p.reads > 0);
    EXPECT(p.changes > 0);
    // 单调性：连续两次 now_us() 不倒退
    double a = now_us();
    double b = now_us();
    EXPECT(b >= a);
}

// =====================================================================
// T10 busy_wait 确实等到了量级
// =====================================================================
static void test_10_busy_wait() {
    for (double want : {100.0, 300.0}) {
        const double t0 = now_us();
        busy_wait_us(want);
        const double got = now_us() - t0;
        EXPECT(got >= want * 0.9);
        EXPECT(got <= want + 5000.0);     // 允许被抢占，但不该离谱
    }
    // 0 或负数 = 不等
    const double t0 = now_us();
    busy_wait_us(0.0);
    busy_wait_us(-5.0);
    EXPECT(now_us() - t0 < 1000.0);
}

// =====================================================================
// T11 计时器噪声地板
// =====================================================================
static void test_11_timer_noise() {
    NoiseBaseline b = measure_timer_noise(5000);
    EXPECT_EQ(b.n, 5000);
    EXPECT(b.min_us >= 0.0);
    EXPECT(b.p50_us <= b.p99_us);
    EXPECT(b.p99_us <= b.max_us);
    EXPECT(b.mean_us >= 0.0);
    // 读一对时钟不应慢到微秒级（否则计时装置本身不可用）
    EXPECT(b.p99_us < 5.0);
}

// =====================================================================
int main() {
    std::printf("=== 18/ 计时与统计基础设施 单元测试 ===\n\n");
    test_01_quantile_definitions();
    test_02_nearest_rank_is_observed();
    test_03_warmup();
    test_04_outliers();
    test_05_mean_stddev();
    test_06_monotonicity();
    test_07_worst_of();
    test_08_gate_logic();
    test_09_clock_probe();
    test_10_busy_wait();
    test_11_timer_noise();

    std::printf("\n");
    if (g_fail == 0) std::printf("ALL TESTS PASSED\n");
    // SKIPPED 每次都打印（哪怕是 0）：只在跳过时打印的话，"确实跑过"在日志里
    // 就没有正面证据，只能靠"没有那一行"推断，而缺行与 grep 写错不可区分。
    std::printf("PASS=%d FAIL=%d SKIPPED=0\n", g_pass, g_fail);
    dump_status(g_pass, g_fail, 0);
    return g_fail == 0 ? 0 : 1;
}
