// =====================================================================
// 16/ 单元测试 —— 断线重连退避（T01~T10）+ 主备双链路仲裁（T11~T20）
//
//   T01 退避基础序列与"单调不减直到封顶"
//   T02 抖动倍数落在 [1-r, 1+r]，且抖动确实在起作用（反向守卫）
//   T03 确定性：同种子同序列 / 异种子异序列
//   T04 reset() 回到 initial（与全新实例逐位相等）
//   T05 ★ 固定间隔复现：factor=1 & jitter=0 时逐次恒等于 initial_ms
//   T06 ★ 非法 policy 被显式归一（factor<1 / initial>max / 越界抖动）
//   T07 大 attempt 不溢出、不长成 0 或负数
//   T08 seed=0 不退化成"全 0 序列"
//   T09 合法 policy **不**被归一（归一只对非法输入生效）
//   T10 总等待时间与 attempt 计数自洽
//
//   T11 主链持续正常 → 一次都不切
//   T12 ★ 主链连续失败 N-1 次不切、第 N 次才切（反向守卫）
//   T13 ★ 切备后主链恢复 M-1 次不切回、M 次才切回
//   T14 ★ 滞环有效：阈值附近抖动不产生切换风暴（并与"无滞环对照实现"对比）
//   T15 ★ 两条都断：is_up()==false 且不刷屏切换
//   T16 ★ 绝不切到不可用的链路
//   T17 非法 ArbiterPolicy 被显式归一（M<=N 没有滞环）
//   T18 计数器自洽：switches == failovers + failbacks
//   T19 链路契约在**编译期**被检查（检测器惯用法）
//   T20 切换时旧链路被 close（两条同时 up 会让对端看到两个会话）
//
// 编译：见 16/scripts/build_test.bat
// =====================================================================

#include "backoff.h"
#include "link_arbiter.h"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace ems::comm;

static int g_pass = 0;
static int g_fail = 0;

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

#define EXPECT_NEAR(a, b, eps)                                            \
    do {                                                                  \
        double va_ = (a), vb_ = (b);                                      \
        if (std::fabs(va_ - vb_) <= (eps)) {                              \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va_ << " vs " << #b << "=" \
                      << vb_ << " (eps " << (eps) << ")" << std::endl;    \
        }                                                                 \
    } while (0)

// ★ 专门给 std::uint64_t 用。EXPECT_EQ 内部转 long long，
//   对超过 2^63 的无符号值会显示成负数（比较仍正确，但日志会误导）。
#define EXPECT_U64_EQ(a, b)                                               \
    do {                                                                  \
        unsigned long long va_ = (unsigned long long)(a);                 \
        unsigned long long vb_ = (unsigned long long)(b);                 \
        if (va_ == vb_) {                                                 \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va_ << " vs " << #b << "=" \
                      << vb_ << std::endl;                                \
        }                                                                 \
    } while (0)

// =====================================================================
// 测试替身：可编程假链路
//
// 能按脚本返回 connect / is_up 结果 —— 这是"主备仲裁"唯一的可测方式：
// 真链路的重连要等超时、要占端口，无法在单元测试里精确驱动。
// =====================================================================
struct FakeLink {
    std::string ep           = "fake";
    bool        up           = false;   // 当前是否 up
    bool        connect_ok   = true;    // connect() 的返回值
    int         connects     = 0;
    int         closes       = 0;

    bool connect() {
        ++connects;
        if (connect_ok) up = true;
        return connect_ok;
    }
    void close() { ++closes; up = false; }
    bool is_up() const { return up; }
    std::string endpoint() const { return ep; }

    // --- 测试脚本 ---
    void set_healthy(bool on) { up = on; }
    void set_connect_ok(bool ok) { connect_ok = ok; if (!ok) up = false; }
};

// 故意**不满足**契约的类型（用来证明检测器真的在检查）
struct BadLink {
    void close() {}
    bool is_up() const { return false; }
    std::string endpoint() const { return "bad"; }
    // 少了 connect()
};

// =====================================================================
// T01 退避基础序列与封顶
// =====================================================================
static void test_01_basic_sequence() {
    std::printf("T01 退避基础序列与封顶\n");

    BackoffPolicy p;
    p.initial_ms = 1000; p.max_ms = 30000; p.factor = 2.0; p.jitter_ratio = 0.0;
    EXPECT(p.is_valid());
    EXPECT(p.validate().empty());

    Backoff b(p, 12345);
    EXPECT_EQ(b.attempt(), 0);
    EXPECT_EQ(b.current_base_ms(), 1000);

    const int expect_seq[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000};
    int prev = -1;
    for (int i = 0; i < 8; ++i) {
        const int d = b.next_delay_ms();
        EXPECT_EQ(d, expect_seq[i]);
        EXPECT(d >= prev);                 // ★ 单调不减
        prev = d;
    }
    EXPECT_EQ(b.attempt(), 8);

    // 封顶后再走 500 次，必须恒等于 max_ms（不溢出、不越界）
    for (int i = 0; i < 500; ++i) {
        const int d = b.next_delay_ms();
        EXPECT_EQ(d, 30000);
    }
    EXPECT_EQ(b.attempt(), 508);
    // 反向守卫：确实走过了封顶点之后才有意义
    EXPECT(b.attempt() > 8);

    // 总等待时间单调增长且量级正确（前 8 次理论值之和 = 1000+2000+4000+8000+16000+30000*3)
    Backoff b2(p, 12345);
    int sum = 0;
    for (int i = 0; i < 8; ++i) sum += b2.next_delay_ms();
    EXPECT_EQ(b2.total_wait_ms(), sum);
    EXPECT_EQ(sum, 1000 + 2000 + 4000 + 8000 + 16000 + 30000 * 3);
}

// =====================================================================
// T02 抖动
// =====================================================================
static void test_02_jitter() {
    std::printf("T02 抖动落在 [1-r, 1+r]\n");

    const double r = 0.2;
    BackoffPolicy p;
    p.initial_ms = 1000; p.max_ms = 30000; p.factor = 2.0; p.jitter_ratio = r;
    Backoff b(p, 0xC0FFEEULL);

    double jmin = 10.0, jmax = -10.0;
    int    n_off_grid = 0;
    const int    n_distinct_sample = 60;
    std::vector<int> seen;
    for (int i = 0; i < 200; ++i) {
        const int d = b.next_delay_ms();
        const double j = b.last_jitter();
        const int    base = b.last_base_ms();
        // ★ 抖动倍率必须落在 [1-r, 1+r]
        EXPECT(j >= 1.0 - r - 1e-12);
        EXPECT(j <= 1.0 + r + 1e-12);
        // 延迟永远是正的
        EXPECT(d >= 1);
        EXPECT(d <= p.max_ms);
        if (j < jmin) jmin = j;
        if (j > jmax) jmax = j;
        // 未触到封顶时，延迟应当就是 base*jitter 的四舍五入
        if (base < p.max_ms) {
            const int expect = (int)((double)base * j + 0.5);
            if (expect >= 1 && expect <= p.max_ms) EXPECT_EQ(d, expect);
        }
        if (i < n_distinct_sample) seen.push_back(d);
    }
    // 反向守卫：抖动**真的在起作用** —— 若有人把 jitter 写成恒等于 1.0，
    // 下面这两条会立刻红。
    EXPECT(jmax - jmin > 0.05);
    EXPECT(jmin < 1.0);
    EXPECT(jmax > 1.0);
    int distinct = 0;
    for (std::size_t i = 0; i < seen.size(); ++i) {
        bool dup = false;
        for (std::size_t k = 0; k < i; ++k) if (seen[k] == seen[i]) dup = true;
        if (!dup) ++distinct;
    }
    EXPECT(distinct > 5);
    (void)n_off_grid;

    // 对照组：r=0 时必须恒等于 base（抖动关掉就是关掉）
    BackoffPolicy p0 = p; p0.jitter_ratio = 0.0;
    Backoff b0(p0, 0xC0FFEEULL);
    for (int i = 0; i < 40; ++i) {
        const int d = b0.next_delay_ms();
        EXPECT_EQ(d, b0.last_base_ms());
        EXPECT_NEAR(b0.last_jitter(), 1.0, 1e-15);
    }
}

// =====================================================================
// T03 确定性
// =====================================================================
static void test_03_determinism() {
    std::printf("T03 确定性（同种子同序列）\n");

    BackoffPolicy p;
    p.initial_ms = 500; p.max_ms = 20000; p.factor = 1.7; p.jitter_ratio = 0.35;

    Backoff a(p, 42), b(p, 42), c(p, 43);
    bool all_equal = true, any_diff = false;
    for (int i = 0; i < 80; ++i) {
        const int da = a.next_delay_ms();
        const int db = b.next_delay_ms();
        const int dc = c.next_delay_ms();
        if (da != db) all_equal = false;
        if (da != dc) any_diff = true;
    }
    EXPECT(all_equal);      // ★ 同种子逐位相同 —— 测试可复现的前提
    EXPECT(any_diff);       // 反向守卫：不同种子必须给出不同序列（否则 PRNG 是死的）

    // peek 不改变状态，且与随后的 next 一致
    Backoff d(p, 7);
    const int peek1 = d.peek_next_delay_ms();
    const int peek2 = d.peek_next_delay_ms();
    EXPECT_EQ(peek1, peek2);
    EXPECT_EQ(d.attempt(), 0);
    EXPECT_EQ(d.next_delay_ms(), peek1);
    EXPECT_EQ(d.next_delay_ms(), d.last_delay_ms());

    // PRNG 状态在推进后必须改变
    Backoff e(p, 7);
    const std::uint64_t s0 = e.rng_state();
    e.next_delay_ms();
    EXPECT(e.rng_state() != s0);
}

// =====================================================================
// T04 reset()
// =====================================================================
static void test_04_reset() {
    std::printf("T04 reset() 回到 initial\n");

    BackoffPolicy p;
    p.initial_ms = 750; p.max_ms = 12000; p.factor = 2.0; p.jitter_ratio = 0.25;

    Backoff fresh(p, 2024);
    Backoff used(p, 2024);
    for (int i = 0; i < 12; ++i) used.next_delay_ms();
    EXPECT_EQ(used.attempt(), 12);
    EXPECT(used.total_wait_ms() > 0);

    used.reset();
    EXPECT_EQ(used.attempt(), 0);
    EXPECT_EQ(used.total_wait_ms(), 0);
    EXPECT_EQ(used.current_base_ms(), 750);   // 回到 initial

    // ★ 与全新实例的完整序列逐位相等 —— 这才是"reset 干净了"的硬证据
    //   （只比第一次的话，PRNG 状态没复位也能过）
    Backoff fresh2(p, 2024);
    bool all_equal = true;
    for (int i = 0; i < 12; ++i) {
        if (used.next_delay_ms() != fresh2.next_delay_ms()) all_equal = false;
    }
    EXPECT(all_equal);
    (void)fresh;
}

// =====================================================================
// T05 ★ 固定间隔复现（能复现 13/ 现有行为的根据）
// =====================================================================
static void test_05_fixed_interval_reproduces_13() {
    std::printf("T05 固定间隔复现（接入 13/ 的根据）\n");

    // 13/src/modbus_device_io.h 的 Config::reconnect_interval_ms 默认 1000，
    // 其 try_reconnect() 的行为是"每 1000 ms 试一次"。
    const BackoffPolicy p = BackoffPolicy::fixed_interval(1000);
    EXPECT(p.is_valid());
    EXPECT_EQ(p.initial_ms, 1000);
    EXPECT_EQ(p.max_ms, 1000);
    EXPECT_NEAR(p.factor, 1.0, 1e-15);
    EXPECT_NEAR(p.jitter_ratio, 0.0, 1e-15);

    Backoff b(p, 1);
    for (int i = 0; i < 500; ++i) {
        // ★ 逐次恒等于 1000 —— 这就是"默认值逐位复现当前固定间隔行为"
        EXPECT_EQ(b.next_delay_ms(), 1000);
    }
    EXPECT_EQ(b.total_wait_ms(), 500 * 1000);

    // 反向守卫：换一个 interval，输出必须随之改变（证明上面不是把 1000 写死了）
    Backoff b2(BackoffPolicy::fixed_interval(250), 1);
    for (int i = 0; i < 20; ++i) EXPECT_EQ(b2.next_delay_ms(), 250);

    // 退化路径：initial > max 被归一后 = 常量间隔（不会产生 0/负）
    BackoffPolicy bad;
    bad.initial_ms = 5000; bad.max_ms = 1000; bad.factor = 2.0; bad.jitter_ratio = 0.0;
    EXPECT(!bad.is_valid());
    Backoff b3(bad, 1);
    EXPECT(b3.normalized());
    EXPECT_EQ(b3.policy().max_ms, b3.policy().initial_ms);
    for (int i = 0; i < 30; ++i) EXPECT_EQ(b3.next_delay_ms(), 5000);
}

// =====================================================================
// T06 ★ 非法 policy 显式归一（反向守卫）
// =====================================================================
static void test_06_illegal_policy() {
    std::printf("T06 非法 policy 被显式归一\n");

    // --- ① factor < 1：越试越密，极限是忙等 ---
    {
        BackoffPolicy p;
        p.factor = 0.5; p.initial_ms = 1000; p.max_ms = 30000; p.jitter_ratio = 0.0;
        EXPECT(!p.is_valid());
        EXPECT(!p.validate().empty());
        Backoff b(p, 5);
        EXPECT(b.normalized());                                  // ★ 必须**显式**标记
        EXPECT(!b.normalize_reason().empty());
        EXPECT(b.policy().factor >= 1.0);
        int prev = 0;
        for (int i = 0; i < 60; ++i) {
            const int d = b.next_delay_ms();
            EXPECT(d >= 1);
            EXPECT(d >= prev);                                   // 不递减
            prev = d;
        }
    }
    // --- ② initial > max ---
    {
        BackoffPolicy p;
        p.initial_ms = 60000; p.max_ms = 1000; p.factor = 2.0; p.jitter_ratio = 0.3;
        EXPECT(!p.is_valid());
        Backoff b(p, 5);
        EXPECT(b.normalized());
        EXPECT(b.policy().max_ms >= b.policy().initial_ms);
        for (int i = 0; i < 40; ++i) {
            const int d = b.next_delay_ms();
            EXPECT(d >= 1);
            EXPECT(d <= b.policy().max_ms);
        }
    }
    // --- ③ initial_ms = 0 / 负数：会产生 0 延迟（忙等） ---
    {
        BackoffPolicy p;
        p.initial_ms = 0; p.max_ms = 1000; p.factor = 2.0; p.jitter_ratio = 0.0;
        EXPECT(!p.is_valid());
        Backoff b(p, 5);
        EXPECT(b.normalized());
        EXPECT(b.policy().initial_ms >= 1);
        for (int i = 0; i < 20; ++i) EXPECT(b.next_delay_ms() >= 1);
    }
    {
        BackoffPolicy p;
        p.initial_ms = -100; p.max_ms = -100; p.factor = 3.0; p.jitter_ratio = 0.0;
        EXPECT(!p.is_valid());
        Backoff b(p, 5);
        EXPECT(b.normalized());
        for (int i = 0; i < 20; ++i) EXPECT(b.next_delay_ms() >= 1);
    }
    // --- ④ 抖动越界 / 负数 ---
    {
        BackoffPolicy p;
        p.jitter_ratio = 5.0;
        EXPECT(!p.is_valid());
        Backoff b(p, 5);
        EXPECT(b.normalized());
        EXPECT(b.policy().jitter_ratio <= BackoffPolicy::kMaxJitterRatio);
        // ★ 即便 r 被钳到 1.0（下界会到 0），延迟也**永远** >= 1
        for (int i = 0; i < 300; ++i) EXPECT(b.next_delay_ms() >= 1);
    }
    {
        BackoffPolicy p;
        p.jitter_ratio = -0.5;
        EXPECT(!p.is_valid());
        Backoff b(p, 5);
        EXPECT(b.normalized());
        EXPECT_NEAR(b.policy().jitter_ratio, 0.0, 1e-15);
        for (int i = 0; i < 20; ++i) EXPECT(b.next_delay_ms() >= 1);
    }
    // --- ⑤ NaN / inf ---
    {
        BackoffPolicy p;
        p.factor = std::nan("");
        EXPECT(!p.is_valid());
        Backoff b(p, 5);
        EXPECT(b.normalized());
        EXPECT(b.policy().factor >= 1.0);
    }
    {
        BackoffPolicy p;
        p.jitter_ratio = std::nan("");
        EXPECT(!p.is_valid());
        Backoff b(p, 5);
        EXPECT(b.normalized());
        for (int i = 0; i < 20; ++i) EXPECT(b.next_delay_ms() >= 1);
    }
    {
        BackoffPolicy p;
        p.factor = INFINITY;
        EXPECT(!p.is_valid());
        Backoff b(p, 5);
        EXPECT(b.normalized());
        for (int i = 0; i < 20; ++i) {
            const int d = b.next_delay_ms();
            EXPECT(d >= 1);
            EXPECT(d <= b.policy().max_ms);
        }
    }
}

// =====================================================================
// T07 大 attempt 不溢出
// =====================================================================
static void test_07_large_attempt() {
    std::printf("T07 大 attempt 不溢出、不变 0\n");

    BackoffPolicy p;
    p.initial_ms = 100; p.max_ms = 30000; p.factor = 2.0; p.jitter_ratio = 0.1;
    Backoff b(p, 99);
    for (int i = 0; i < 3000; ++i) {
        const int d = b.next_delay_ms();
        if (!(d >= 1 && d <= p.max_ms)) { EXPECT(false); break; }
    }
    EXPECT(b.attempt() == 3000);
    EXPECT(b.next_delay_ms() >= 1);        // 3001 次后依然正常
}

// =====================================================================
// T08 seed = 0
// =====================================================================
static void test_08_seed_zero() {
    std::printf("T08 seed=0 不退化成全 0 序列\n");

    BackoffPolicy p;
    p.initial_ms = 1000; p.max_ms = 100000; p.factor = 2.0; p.jitter_ratio = 0.5;
    Backoff b(p, 0);
    int distinct = 0;
    std::vector<int> v;
    for (int i = 0; i < 20; ++i) v.push_back(b.next_delay_ms());
    for (std::size_t i = 0; i < v.size(); ++i) {
        bool dup = false;
        for (std::size_t k = 0; k < i; ++k) if (v[k] == v[i]) dup = true;
        if (!dup) ++distinct;
    }
    EXPECT(distinct > 3);                       // ★ 序列必须有变化
    // 原始 PRNG：seed=0 会被换成非零守卫，且仍然确定性
    Xorshift64 g(0), h(0);
    const std::uint64_t v1 = g.next_u64();
    const std::uint64_t v2 = g.next_u64();
    EXPECT(v1 != 0);                            // ★ 不退化成全 0 序列
    EXPECT(v2 != 0);
    EXPECT(v1 != v2);
    EXPECT_U64_EQ(h.next_u64(), v1);            // 同种子 → 同序列
    EXPECT_U64_EQ(h.next_u64(), v2);
    // 反向守卫：非零守卫确实改变了状态（否则 seed=0 与 seed=kSeedGuard 会退化成同一序列）
    Xorshift64 k(Xorshift64::kSeedGuard);
    EXPECT_U64_EQ(k.next_u64(), v1);
}

// =====================================================================
// T09 合法 policy 不被归一
// =====================================================================
static void test_09_valid_policy_not_normalized() {
    std::printf("T09 合法 policy 不被归一（归一只对非法生效）\n");

    BackoffPolicy p;
    EXPECT(p.is_valid());
    Backoff b(p, 1);
    EXPECT(!b.normalized());                        // ★ 反向守卫
    EXPECT(b.normalize_reason().empty());
    EXPECT_EQ(b.policy().initial_ms, p.initial_ms);
    EXPECT_EQ(b.policy().max_ms, p.max_ms);
    EXPECT_NEAR(b.policy().factor, p.factor, 1e-15);
    EXPECT_NEAR(b.policy().jitter_ratio, p.jitter_ratio, 1e-15);

    // requested_policy() 保留原始输入，policy() 是生效值 —— 两者都看得见
    BackoffPolicy bad;
    bad.factor = 0.1;
    Backoff b2(bad, 1);
    EXPECT_NEAR(b2.requested_policy().factor, 0.1, 1e-15);
    EXPECT(b2.policy().factor >= 1.0);
}

// =====================================================================
// T10 计数自洽
// =====================================================================
static void test_10_counters() {
    std::printf("T10 总等待时间与 attempt 自洽\n");

    BackoffPolicy p;
    p.initial_ms = 200; p.max_ms = 5000; p.factor = 1.5; p.jitter_ratio = 0.15;
    Backoff b(p, 314159);
    int sum = 0;
    for (int i = 0; i < 100; ++i) {
        const int d = b.next_delay_ms();
        sum += d;
        EXPECT_EQ(b.total_wait_ms(), sum);
    }
    EXPECT_EQ(b.attempt(), 100);
    EXPECT_EQ(b.total_wait_ms(), sum);

    // 封顶后每次都是 max_ms
    Backoff c(p, 1);
    for (int i = 0; i < 200; ++i) c.next_delay_ms();
    EXPECT_EQ(c.current_base_ms(), 5000);
}

// =====================================================================
// T11 主链持续正常 → 一次都不切
// =====================================================================
static void test_11_healthy_primary_no_switch() {
    std::printf("T11 主链持续正常 → 不切换\n");

    FakeLink prim; prim.ep = "primary"; prim.up = true;
    FakeLink back; back.ep = "backup";
    ArbiterPolicy pol; pol.fail_threshold = 3; pol.recover_threshold = 5;
    LinkArbiter<FakeLink, FakeLink> arb(prim, back, pol);

    for (int i = 0; i < 50; ++i) {
        EXPECT(arb.step());
        EXPECT(arb.current() == 0);   // 0 = 主链
    }
    EXPECT_EQ(arb.switches(), 0);
    EXPECT_EQ(arb.failovers(), 0);
    EXPECT_EQ(arb.failbacks(), 0);
    EXPECT_EQ(arb.consecutive_failures(), 0);
    EXPECT_EQ(arb.consecutive_successes(), 50);
    EXPECT(arb.current_endpoint() == "primary");
    EXPECT(!arb.normalized());
}

// =====================================================================
// T12 ★ 连续失败 N-1 不切、第 N 次才切
// =====================================================================
static void test_12_failover_threshold() {
    std::printf("T12 连续失败 N-1 不切、N 才切\n");

    const int N = 3;
    {
        FakeLink prim; prim.ep = "primary"; prim.up = true;
        FakeLink back; back.ep = "backup";
        ArbiterPolicy pol; pol.fail_threshold = N; pol.recover_threshold = 5;
        LinkArbiter<FakeLink, FakeLink> arb(prim, back, pol);

        arb.primary().set_connect_ok(false);       // 主链开始失败
        arb.step();  EXPECT_EQ(arb.consecutive_failures(), 1);
        EXPECT_EQ(arb.current(), 0);
        EXPECT_EQ(arb.switches(), 0);
        arb.step();  EXPECT_EQ(arb.consecutive_failures(), 2);
        EXPECT_EQ(arb.current(), 0);               // ★ N-1 次**不**切
        EXPECT_EQ(arb.switches(), 0);

        arb.step();  EXPECT_EQ(arb.consecutive_failures(), 3);
        EXPECT_EQ(arb.current(), 1);               // ★ 第 N 次才切
        EXPECT_EQ(arb.switches(), 1);
        EXPECT_EQ(arb.failovers(), 1);
        EXPECT_EQ(arb.failbacks(), 0);
        EXPECT(arb.is_up());                       // 备链已 up
        EXPECT(arb.current_endpoint() == "backup");
    }
    // 反向守卫：N-1 次之后 `switches()==0` 只有在"确有条链路真的失败过"时才有意义。
    // 用 consecutive_failures 的推进把这件事钉住（上面已断言 1 → 2 → 3）。
    {
        // 中途恢复一次 → 计数归零，再失败 N-1 次仍不切
        FakeLink prim; prim.ep = "primary"; prim.up = true;
        FakeLink back; back.ep = "backup";
        ArbiterPolicy pol; pol.fail_threshold = N; pol.recover_threshold = 5;
        LinkArbiter<FakeLink, FakeLink> arb(prim, back, pol);

        arb.primary().set_connect_ok(false);
        arb.step(); arb.step();                    // 失败 2 次
        EXPECT_EQ(arb.consecutive_failures(), 2);
        arb.primary().set_connect_ok(true);        // 恢复 1 拍
        arb.step();
        EXPECT_EQ(arb.consecutive_failures(), 0);  // ★ "连续"的含义：被成功打断就归零
        EXPECT_EQ(arb.consecutive_successes(), 1);
        arb.primary().set_connect_ok(false);
        arb.step(); arb.step();
        EXPECT_EQ(arb.current(), 0);               // 又只有 2 次 → 仍不切
        EXPECT_EQ(arb.switches(), 0);
    }
}

// =====================================================================
// T13 ★ 恢复 M-1 不切回、M 才切回
// =====================================================================
static void test_13_failback_threshold() {
    std::printf("T13 主链恢复 M-1 不切回、M 才切回\n");

    const int N = 3, M = 5;
    FakeLink prim; prim.ep = "primary"; prim.up = true;
    FakeLink back; back.ep = "backup";
    ArbiterPolicy pol; pol.fail_threshold = N; pol.recover_threshold = M;
    LinkArbiter<FakeLink, FakeLink> arb(prim, back, pol);

    // 先切到备链
    arb.primary().set_connect_ok(false);
    for (int i = 0; i < N; ++i) arb.step();
    EXPECT_EQ(arb.current(), 1);
    EXPECT_EQ(arb.switches(), 1);
    EXPECT(arb.is_up());                          // 备链在役且可用

    // 主链恢复：连续 M-1 次不切回
    arb.primary().set_connect_ok(true);
    for (int i = 0; i < M - 1; ++i) {
        arb.step();
        EXPECT_EQ(arb.consecutive_successes(), i + 1);
        EXPECT_EQ(arb.current(), 1);              // ★ 仍在备链
    }
    EXPECT_EQ(arb.switches(), 1);                 // 一次都没多切

    // 第 M 次
    arb.step();
    EXPECT_EQ(arb.consecutive_successes(), M);
    EXPECT_EQ(arb.current(), 0);                  // ★ 切回主链
    EXPECT_EQ(arb.switches(), 2);
    EXPECT_EQ(arb.failovers(), 1);
    EXPECT_EQ(arb.failbacks(), 1);
    EXPECT(arb.current_endpoint() == "primary");

    // − 反例：M-1 次后主链又断一次 → 计数归零，"还没到 M" 是真的被重置过
    {
        FakeLink p2; p2.ep = "p2"; p2.up = true;
        FakeLink b2; b2.ep = "b2";
        LinkArbiter<FakeLink, FakeLink> a2(p2, b2, pol);
        a2.primary().set_connect_ok(false);
        for (int i = 0; i < N; ++i) a2.step();
        EXPECT_EQ(a2.current(), 1);
        a2.primary().set_connect_ok(true);
        for (int i = 0; i < M - 1; ++i) a2.step();
        EXPECT_EQ(a2.consecutive_successes(), M - 1);
        a2.primary().set_connect_ok(false);       // 打断
        a2.step();
        EXPECT_EQ(a2.consecutive_successes(), 0);
        a2.primary().set_connect_ok(true);
        for (int i = 0; i < M - 1; ++i) a2.step();
        EXPECT_EQ(a2.current(), 1);               // ★ 依然没切回
        EXPECT_EQ(a2.switches(), 1);
    }
}

// =====================================================================
// T14 ★ 滞环有效：阈值附近抖动不产生切换风暴
// =====================================================================
static void test_14_hysteresis_no_storm() {
    std::printf("T14 滞环有效（与无滞环对照实现对比）\n");

    const int N = 3, M = 5;
    const int steps = 40;

    // 主链劣化形态：通-断交替（现场最常见的"临界光衰/接口 CRC 错"）
    std::vector<bool> prim_up(steps);
    for (int i = 0; i < steps; ++i) prim_up[i] = (i % 2 == 0);

    // --- ① 被测：带滞环 ---
    FakeLink prim; prim.ep = "primary"; prim.up = prim_up[0];
    FakeLink back; back.ep = "backup"; back.up = true;   // 备链始终可用
    ArbiterPolicy pol; pol.fail_threshold = N; pol.recover_threshold = M;
    LinkArbiter<FakeLink, FakeLink> arb(prim, back, pol);

    int failures_seen = 0;
    for (int i = 0; i < steps; ++i) {
        // 按脚本设置主链本拍的健康度：
        //  up=true  → connect_ok=true 且 up=true
        //  up=false → connect_ok=false
        if (prim_up[i]) { arb.primary().set_connect_ok(true); arb.primary().set_healthy(true); }
        else            { arb.primary().set_connect_ok(false); ++failures_seen; }
        arb.step();
    }
    // 反向守卫：确实抖了 20 次，不是"序列全通所以没切"
    EXPECT_EQ(failures_seen, steps / 2);
    EXPECT(failures_seen > 0);
    EXPECT_EQ(arb.consecutive_failures() <= N, true);
    // ★ 单次都不切 —— 这就是滞环的全部价值
    EXPECT_EQ(arb.switches(), 0);
    EXPECT_EQ(arb.current(), 0);

    // --- ② 对照组：同样的驱动序列喂给"无滞环"的朴素实现 ---
    //    朴素实现 = 本模块开头点名要防的那两行：
    //        if (主链断) 切备;   if (主链通) 切回主;
    //    ★ 这个对照组的**存在**就是判据有效性的证据：如果它切得也不多，
    //      那"被测实现 0 次切换"就说明不了任何事。
    int  naive_switches = 0;
    int  naive_active   = 0;                  // 0=主 1=备
    for (int i = 0; i < steps; ++i) {
        const bool p_up = prim_up[i];
        if (naive_active == 0) {
            if (!p_up) { naive_active = 1; ++naive_switches; }   // 主断 → 切备
        } else {
            if (p_up)  { naive_active = 0; ++naive_switches; }   // 主通 → 切回
        }
    }
    EXPECT(naive_switches >= 10);                    // ★ 反向守卫
    EXPECT(arb.switches() * 10 < naive_switches);    // 滞环把切换压到 0

    // --- ③ 用"短脉冲"再打一次：连续失败 2 次就恢复 ---
    {
        FakeLink p3; p3.ep = "p3"; p3.up = true;
        FakeLink b3; b3.ep = "b3"; b3.up = true;
        LinkArbiter<FakeLink, FakeLink> a3(p3, b3, pol);
        int fails = 0;
        for (int round = 0; round < 20; ++round) {
            a3.primary().set_connect_ok(false);
            a3.step(); ++fails;
            a3.primary().set_connect_ok(false);
            a3.step(); ++fails;                 // 连续 2 次（< N=3）
            a3.primary().set_connect_ok(true);
            a3.primary().set_healthy(true);
            a3.step();
        }
        EXPECT_EQ(fails, 40);
        EXPECT_EQ(a3.switches(), 0);            // ★ 40 次失败、0 次切换
    }
}

// =====================================================================
// T15 ★ 两条都断
// =====================================================================
static void test_15_both_down() {
    std::printf("T15 两条都断：is_up()==false 且不刷屏切换\n");

    FakeLink prim; prim.ep = "primary"; prim.up = true;
    FakeLink back; back.ep = "backup";
    ArbiterPolicy pol; pol.fail_threshold = 3; pol.recover_threshold = 5;
    LinkArbiter<FakeLink, FakeLink> arb(prim, back, pol);

    // 两条都拉不起来
    arb.primary().set_connect_ok(false);
    arb.backup().set_connect_ok(false);
    for (int i = 0; i < 40; ++i) {
        EXPECT(!arb.step());                 // ★ 始终不可用
        EXPECT_EQ(arb.switches(), 0);        // ★ 一次都不切（切了也没用）
        EXPECT_EQ(arb.current(), 0);
    }
    EXPECT(!arb.is_up());
    EXPECT_EQ(arb.consecutive_failures(), 40);

    // 备链恢复 → 下一拍应当切过去（此时主链已连续失败 40 >> N）
    arb.backup().set_connect_ok(true);
    arb.step();
    EXPECT_EQ(arb.switches(), 1);
    EXPECT_EQ(arb.current(), 1);
    EXPECT(arb.is_up());

    // 主链也恢复 → 连续 5 次后切回；期间备链若也断掉，则在窗口内不可用
    arb.primary().set_connect_ok(true);
    arb.backup().set_connect_ok(false);
    arb.step();                              // 备链本拍已断，主链成功第 1 次
    EXPECT_EQ(arb.consecutive_successes(), 1);
    EXPECT_EQ(arb.current(), 1);             // 滞环窗口内仍算在备链
    EXPECT(!arb.is_up());                    // ★ 这段窗口内不可用 —— 滞环的代价，见文档
    for (int i = 0; i < 10; ++i) arb.step();
    EXPECT_EQ(arb.current(), 0);             // 达到 M 后切回
    EXPECT(arb.is_up());
    EXPECT_EQ(arb.switches(), 2);
}

// =====================================================================
// T16 ★ 绝不切到不可用的链路
// =====================================================================
static void test_16_never_switch_to_dead_link() {
    std::printf("T16 绝不切到不可用的链路\n");

    FakeLink prim; prim.ep = "primary"; prim.up = true;
    FakeLink back; back.ep = "backup";
    ArbiterPolicy pol; pol.fail_threshold = 2; pol.recover_threshold = 4;
    LinkArbiter<FakeLink, FakeLink> arb(prim, back, pol);

    arb.primary().set_connect_ok(false);     // 主断
    arb.backup().set_connect_ok(false);      // 备也断
    for (int i = 0; i < 30; ++i) arb.step();
    EXPECT_EQ(arb.current(), 0);             // ★ 备链不可用 → 不切（否则 current 变随机值）
    EXPECT_EQ(arb.switches(), 0);
    EXPECT(!arb.is_up());

    // 备链一起来，立刻切
    arb.backup().set_connect_ok(true);
    arb.step();
    EXPECT_EQ(arb.current(), 1);
    EXPECT_EQ(arb.switches(), 1);

    // 切过去之后又把备链**拉不起来** + 主链仍断 → 不切回（主链不可用）
    // ★ 注意必须用 set_connect_ok(false)（connect 失败），不能只 set_healthy(false)：
    //   仲裁器每拍都会对未 up 的链路主动 connect()，只把 up 置 false 的话
    //   下一拍就被拉起来了 —— 这正是"主动探测"的设计，不是缺陷。
    arb.primary().set_connect_ok(false);
    arb.backup().set_connect_ok(false);
    arb.step();
    EXPECT_EQ(arb.current(), 1);
    EXPECT_EQ(arb.switches(), 1);
    EXPECT(!arb.is_up());
}

// =====================================================================
// T17 非法 ArbiterPolicy 归一
// =====================================================================
static void test_17_illegal_arbiter_policy() {
    std::printf("T17 非法 ArbiterPolicy 被归一\n");

    // M <= N → 没有滞环，必须被显式归一
    ArbiterPolicy bad; bad.fail_threshold = 5; bad.recover_threshold = 5;
    EXPECT(!bad.is_valid());
    EXPECT(!bad.validate().empty());
    const ArbiterPolicy fixed = bad.normalized();
    EXPECT(fixed.recover_threshold > fixed.fail_threshold);
    EXPECT_EQ(fixed.recover_threshold, 6);

    FakeLink prim; prim.ep = "primary"; prim.up = true;
    FakeLink back; back.ep = "backup";
    LinkArbiter<FakeLink, FakeLink> arb(prim, back, bad);
    EXPECT(arb.normalized());                        // ★ 显式标记
    EXPECT(!arb.normalize_reason().empty());
    EXPECT_EQ(arb.requested_policy().recover_threshold, 5);
    EXPECT_EQ(arb.policy().recover_threshold, 6);

    // 未归一的情形
    ArbiterPolicy ok; ok.fail_threshold = 2; ok.recover_threshold = 3;
    EXPECT(ok.is_valid());
    LinkArbiter<FakeLink, FakeLink> arb2(prim, back, ok);
    EXPECT(!arb2.normalized());                      // 反向守卫

    // fail_threshold = 0：一次失败都不允许 → 也必须归一
    ArbiterPolicy z; z.fail_threshold = 0; z.recover_threshold = 0;
    EXPECT(!z.is_valid());
    const ArbiterPolicy fz = z.normalized();
    EXPECT(fz.fail_threshold >= 1);
    EXPECT(fz.recover_threshold > fz.fail_threshold);
}

// =====================================================================
// T18 计数器自洽
// =====================================================================
static void test_18_counters_consistent() {
    std::printf("T18 switches == failovers + failbacks\n");

    FakeLink prim; prim.ep = "primary"; prim.up = true;
    FakeLink back; back.ep = "backup";
    ArbiterPolicy pol; pol.fail_threshold = 2; pol.recover_threshold = 4;
    LinkArbiter<FakeLink, FakeLink> arb(prim, back, pol);

    // 主断 → 切备；主恢复 → 切回。来回 4 轮。
    for (int round = 0; round < 4; ++round) {
        arb.primary().set_connect_ok(false);
        for (int i = 0; i < 2; ++i) arb.step();
        EXPECT_EQ(arb.current(), 1);
        arb.primary().set_connect_ok(true);
        for (int i = 0; i < 4; ++i) arb.step();
        EXPECT_EQ(arb.current(), 0);
    }
    EXPECT_EQ(arb.switches(), 8);
    EXPECT_EQ(arb.failovers(), 4);
    EXPECT_EQ(arb.failbacks(), 4);
    EXPECT_EQ(arb.switches(), arb.failovers() + arb.failbacks());   // ★ 自洽

    // 反向守卫：确实来回过，而不是"从没切过所以 0==0+0"
    EXPECT(arb.failovers() > 0);
    EXPECT(arb.failbacks() > 0);

    // reset_counters 保留当前链路
    const int cur = arb.current();
    arb.reset_counters();
    EXPECT_EQ(arb.switches(), 0);
    EXPECT_EQ(arb.failovers(), 0);
    EXPECT_EQ(arb.failbacks(), 0);
    EXPECT_EQ(arb.current(), cur);
}

// =====================================================================
// T19 编译期契约检查
// =====================================================================
static void test_19_contract_detection() {
    std::printf("T19 链路契约在编译期被检查\n");

    static_assert(is_link_contract<FakeLink>::value,
                  "FakeLink 应当满足链路契约");
    static_assert(!is_link_contract<BadLink>::value,
                  "BadLink 缺 connect()，必须被检测器拒掉");
    static_assert(!is_link_contract<int>::value,
                  "int 显然不满足链路契约");

    EXPECT(is_link_contract<FakeLink>::value);
    EXPECT(!is_link_contract<BadLink>::value);      // ★ 反向守卫（否则检测器形同虚设）
    EXPECT(!is_link_contract<int>::value);

    // 两条链路类型不同也能工作（接口与实现解耦的证据）
    struct OtherLink {
        std::string tag = "other";
        bool        up  = true;
        bool connect() { up = true; return true; }
        void close() { up = false; }
        bool is_up() const { return up; }
        std::string endpoint() const { return tag; }
    };
    static_assert(is_link_contract<OtherLink>::value, "OtherLink 满足契约");
    FakeLink a; a.ep = "A"; a.up = true;
    LinkArbiter<FakeLink, OtherLink> mixed(a, OtherLink(), ArbiterPolicy());
    EXPECT(mixed.step());
    EXPECT_EQ(mixed.current(), 0);
    EXPECT(mixed.current_endpoint() == "A");
}

// =====================================================================
// T20 切换时旧链路被 close
// =====================================================================
static void test_20_close_on_switch() {
    std::printf("T20 切换时旧链路被 close\n");

    FakeLink prim; prim.ep = "primary"; prim.up = true;
    FakeLink back; back.ep = "backup";
    ArbiterPolicy pol; pol.fail_threshold = 2; pol.recover_threshold = 4;
    LinkArbiter<FakeLink, FakeLink> arb(prim, back, pol);

    arb.step();
    EXPECT_EQ(arb.primary().closes, 0);          // 没切就不该 close

    arb.primary().set_connect_ok(false);
    arb.step(); arb.step();
    EXPECT_EQ(arb.current(), 1);
    EXPECT_EQ(arb.primary().closes, 1);          // ★ 切走时把主链关掉
    EXPECT(!arb.primary().is_up());

    arb.primary().set_connect_ok(true);
    for (int i = 0; i < 4; ++i) arb.step();
    EXPECT_EQ(arb.current(), 0);
    EXPECT_EQ(arb.backup().closes, 1);           // ★ 切回时把备链关掉
    EXPECT(!arb.backup().is_up());

    // 反向守卫：切换次数与 close 次数自洽（每次切换恰好 close 一次旧链路）
    EXPECT_EQ(arb.primary().closes + arb.backup().closes, arb.switches());
}

// =====================================================================
int main() {
    std::printf("=== 16/ 退避策略 + 主备链路仲裁 单元测试 ===\n\n");

    test_01_basic_sequence();
    test_02_jitter();
    test_03_determinism();
    test_04_reset();
    test_05_fixed_interval_reproduces_13();
    test_06_illegal_policy();
    test_07_large_attempt();
    test_08_seed_zero();
    test_09_valid_policy_not_normalized();
    test_10_counters();

    test_11_healthy_primary_no_switch();
    test_12_failover_threshold();
    test_13_failback_threshold();
    test_14_hysteresis_no_storm();
    test_15_both_down();
    test_16_never_switch_to_dead_link();
    test_17_illegal_arbiter_policy();
    test_18_counters_consistent();
    test_19_contract_detection();
    test_20_close_on_switch();

    std::printf("\n");
    if (g_fail == 0) std::printf("ALL TESTS PASSED\n");
    // 本用例没有"跳过"路径，恒为 0；仍按约定打出来，
    // 让每层的状态行格式一致（见 新模块开发约定.md §4.5）
    std::printf("PASS=%d FAIL=%d SKIPPED=0\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
