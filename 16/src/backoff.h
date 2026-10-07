// =====================================================================
// 16/ — 断线重连退避策略（Backoff）
//
// 为什么需要它：
//   13/ 的 ModbusDeviceIO 重连是**固定间隔**（Config::reconnect_interval_ms，
//   默认 1000 ms）。固定间隔在现场有两个对称的坏处：
//     · 设备重启要 30 s 才能起来时，1000 ms 的固定节奏会在前 30 s 里
//       白白打 30 次无用连接（每次都等到 connect 超时）；一个串口/网关
//       后面挂 20 台设备时，这个代价乘 20，把控制周期拖垮。
//     · 反过来，若为了省事把间隔调大到 30 s，那么"闪断 1 s"的现场故障
//       也要等 30 s 才恢复 —— 恢复慢比多试几次贵得多。
//   退避（指数 + 抖动）就是这两者之间的折中：前期密、后期疏、且**永不封顶到无穷**。
//
// ★ 本文件最重要的一条设计约束：**确定性**。
//   退避带"抖动"，而抖动的实现几乎总是 rand()/random_device —— 那样
//   **测试就不可复现**：同一个 bug 只在某次随机序列下出现，构建却全绿。
//   所以这里自带一个 xorshift64* PRNG，同种子同序列，逐位可断言。
//   生产上仍可用时间戳当种子（现场不同实例错开），但测试里种子是常量。
//
// ★ 第二条：**非法策略不允许静默产生 0 或负数延迟**。
//   `factor < 1` 会让重试越试越密（极限等于忙等，把 CPU 和总线打满）；
//   `initial_ms > max_ms` 语义自相矛盾。两者都不报错的话，现场表现是
//   "重连风暴"或"延迟为 0 的死循环"，而日志上什么都看不出来。
//   本实现把非法输入**归一**成安全值，并留下 `normalized()` / `normalize_reason()`
//   两个可观测点 —— 测试用它们做反向守卫。
//
// 编译：纯头文件，无外部依赖。
// =====================================================================

#pragma once

#include <cmath>
#include <cstdint>
#include <string>

namespace ems {
namespace comm {

// =====================================================================
// 退避策略参数
// =====================================================================
struct BackoffPolicy {
    // 第 0 次（首次重试）的等待时间
    int    initial_ms   = 1000;
    // 等待时间的上限（★ 必须有：没有上限的指数退避迟早等于"永不重连"）
    int    max_ms       = 30000;
    // 指数因子。★ 必须 >= 1.0：< 1.0 表示"越试越密"，极限是忙等。
    double factor       = 2.0;
    // 抖动比例 r。第 n 次的等待 = base * (1 + r*(2u-1))，u∈[0,1)
    // → 乘数落在 [1-r, 1+r)。r=0 表示不抖动（逐次严格等于 base）。
    // 为什么要抖动：多台设备/多个进程若同相位重连，会在网关处形成
    // 周期性尖峰（"惊群"）。抖动把它们的相位打散。
    double jitter_ratio = 0.2;

    // 判据用常量：测试与文档都引用它，避免"两处各写一个 1.0"
    static constexpr double kMaxJitterRatio = 1.0;
    static constexpr int    kMinDelayMs     = 1;

    // 合法 = 逐字段无歧义。空串表示合法，否则返回中文原因（给日志/断言用）
    std::string validate() const {
        if (initial_ms < kMinDelayMs) {
            return "initial_ms 必须 >= 1（0 或负数会退化成忙等）";
        }
        if (max_ms < initial_ms) {
            return "max_ms 必须 >= initial_ms（上限低于起点时语义自相矛盾）";
        }
        if (!(factor >= 1.0) || !std::isfinite(factor)) {
            return "factor 必须是 >= 1.0 的有限值（< 1.0 表示越试越密，极限是忙等）";
        }
        if (!(jitter_ratio >= 0.0) || !std::isfinite(jitter_ratio)) {
            return "jitter_ratio 必须 >= 0";
        }
        if (jitter_ratio > kMaxJitterRatio) {
            return "jitter_ratio 必须 <= 1.0（再大会让下界越过 0，产生负延迟）";
        }
        return std::string();
    }

    bool is_valid() const { return validate().empty(); }

    // 把非法输入**归一**成安全值（不是拒绝，因为现场配置多半来自文本文件，
    // 一个错别字不该让整个服务起不来）。归一侧重于"绝不产生 0/负延迟"：
    //   initial_ms < 1        → 1
    //   max_ms < initial_ms   → max_ms = initial_ms（退化成常量间隔，安全）
    //   factor < 1 或非有限    → 1.0（退化成常量间隔，**不会**越试越密）
    //   jitter_ratio 越界/NaN  → 钳到 [0, 1]
    BackoffPolicy normalized() const {
        BackoffPolicy p = *this;
        if (p.initial_ms < kMinDelayMs) p.initial_ms = kMinDelayMs;
        if (std::isnan(p.factor) || !std::isfinite(p.factor) || p.factor < 1.0) {
            p.factor = 1.0;
        }
        if (std::isnan(p.jitter_ratio) || !std::isfinite(p.jitter_ratio)) {
            p.jitter_ratio = 0.0;
        }
        if (p.jitter_ratio < 0.0) p.jitter_ratio = 0.0;
        if (p.jitter_ratio > kMaxJitterRatio) p.jitter_ratio = kMaxJitterRatio;
        if (p.max_ms < p.initial_ms) p.max_ms = p.initial_ms;
        return p;
    }

    // ★ 接入既有固定间隔行为的桥梁：
    //   factor=1, jitter=0 时 Backoff 的每一次输出**逐位等于** interval_ms。
    //   这就是"能复现 13/ 现有固定间隔行为"的根据（见 16/docs/README.md §6）。
    static BackoffPolicy fixed_interval(int interval_ms) {
        BackoffPolicy p;
        p.initial_ms   = interval_ms;
        p.max_ms       = interval_ms;
        p.factor       = 1.0;
        p.jitter_ratio = 0.0;
        return p;
    }
};

// =====================================================================
// 确定性 PRNG —— xorshift64*（Marsaglia / Vigna）
//
// 为什么不用 <random>：
//   ① std::mt19937_64 的**输出分布**在标准库实现之间不保证一致
//      （uniform_real_distribution 的实现自由度很大）→ 跨编译器不可复现。
//   ② 这里只需要"同种子同序列、看起来无规律、够快"，xorshift64* 三个异或
//      一个乘法就够，而且序列**逐位写死**在标准里（就是下面这几行）。
// =====================================================================
class Xorshift64 {
public:
    static constexpr std::uint64_t kSeedGuard = 0x9E3779B97F4A7C15ULL;

    explicit Xorshift64(std::uint64_t seed) {
        // ★ 0 是 xorshift 的不动点（0 异或 0 恒为 0）—— 种子为 0 会退化成
        //   全 0 序列，抖动全部失效且**不报错**。这里直接换成一个非零常量。
        s_ = (seed == 0) ? kSeedGuard : seed;
    }

    std::uint64_t next_u64() {
        std::uint64_t x = s_;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        s_ = x;
        return x * 0x2545F4914F6CDD1DULL;
    }

    // [0, 1) —— 取高 53 位构造成 double，避开低位质量差与浮点舍入
    double next_unit() {
        return (double)(next_u64() >> 11) * (1.0 / 9007199254740992.0);
    }

    std::uint64_t state() const { return s_; }

private:
    std::uint64_t s_;
};

// =====================================================================
// 退避器
// =====================================================================
class Backoff {
public:
    // 默认种子取黄金比例常数（Knuth）—— 只是个"看起来没有规律"的常量，
    // 测试里会显式传种子，生产上建议传时间戳/链路名哈希。
    static constexpr std::uint64_t kDefaultSeed = Xorshift64::kSeedGuard;

    explicit Backoff(BackoffPolicy policy = BackoffPolicy(),
                     std::uint64_t  seed   = kDefaultSeed)
        : requested_(policy),
          policy_(policy.normalized()),
          rng_(seed),
          seed_(seed) {
        normalize_reason_ = requested_.validate();
        normalized_       = !normalize_reason_.empty();
    }

    // -----------------------------------------------------------------
    // 主接口
    // -----------------------------------------------------------------

    // 返回本次应等待的毫秒数，并把内部状态推进到下一次。
    // 保证返回值落在 [1, policy().max_ms] —— **永远不会是 0 或负数**。
    int next_delay_ms() {
        const double base_d = base_at_raw(attempt_);
        const double j      = jitter();
        double d            = base_d * j;

        // 抖动可能把 base 抬到上限之上（或极端 r=1 时压到 0 附近），
        // 两端都钳住 —— 这是"延迟永不越界"这条不变量的唯一执行点。
        if (!(d >= (double)BackoffPolicy::kMinDelayMs)) d = (double)BackoffPolicy::kMinDelayMs;
        if (d > (double)policy_.max_ms) d = (double)policy_.max_ms;

        int out = (int)(d + 0.5);
        if (out < BackoffPolicy::kMinDelayMs) out = BackoffPolicy::kMinDelayMs;
        if (out > policy_.max_ms) out = policy_.max_ms;

        last_base_ms_  = current_base_ms();
        last_jitter_   = j;
        last_delay_ms_ = out;
        total_wait_ms_ += out;
        ++attempt_;
        return out;
    }

    // 不推进地看一眼"下一次会给多少" —— 全量做一遍再回滚（靠值拷贝）。
    // 有了它，"同种子同序列"和"reset 后回到初始"都能写成**等式**断言，
    // 而不是"看起来差不多"。
    int peek_next_delay_ms() const {
        Backoff tmp = *this;
        return tmp.next_delay_ms();
    }

    // 回到初始状态：下一次的等待重新从 initial_ms 开始。
    // 为什么要有：链路**真的连上过**之后再断，说明对端刚刚还在；
    // 此时从头退避（而不是接着上次的 30 s 上限）才符合现场。
    void reset() {
        attempt_       = 0;
        total_wait_ms_ = 0;
        last_base_ms_  = 0;
        last_delay_ms_ = 0;
        last_jitter_   = 1.0;
        rng_           = Xorshift64(seed_);
    }

    // -----------------------------------------------------------------
    // 可观测（现场诊断 + 测试断言）
    // -----------------------------------------------------------------
    int attempt() const { return attempt_; }
    int total_wait_ms() const { return total_wait_ms_; }
    const BackoffPolicy& policy() const { return policy_; }
    const BackoffPolicy& requested_policy() const { return requested_; }

    // 构造时是否对非法 policy 做过归一
    bool normalized() const { return normalized_; }
    const std::string& normalize_reason() const { return normalize_reason_; }

    // 当前 attempt 对应的**不含抖动**的理论值（已被 max_ms 封顶）
    int current_base_ms() const {
        double b = base_at_raw(attempt_);
        if (b > (double)policy_.max_ms) b = (double)policy_.max_ms;
        return (int)(b + 0.5);
    }

    int last_base_ms() const { return last_base_ms_; }
    int last_delay_ms() const { return last_delay_ms_; }
    double last_jitter() const { return last_jitter_; }
    std::uint64_t rng_state() const { return rng_.state(); }
    std::uint64_t seed() const { return seed_; }

private:
    // 未封顶的理论 base。★ 单调不减的**唯一**来源：attempt 递增 + factor>=1
    // → factor^attempt 不减 → base 不减。抖动是乘上去的，不影响这条。
    double base_at_raw(int attempt) const {
        double b = (double)policy_.initial_ms;
        if (policy_.factor > 1.0 && attempt > 0) {
            // pow 会溢出：与其靠 inf 再钳，不如在指数上就先截断。
            if (attempt >= 64) return (double)policy_.max_ms;
            const double m = std::pow(policy_.factor, (double)attempt);
            if (!std::isfinite(m) || m > 1e18) return (double)policy_.max_ms;
            b *= m;
        }
        if (!std::isfinite(b)) return (double)policy_.max_ms;
        return b;
    }

    // 抖动倍率 ∈ [1-r, 1+r)。★ r==0 时**不消费 PRNG** —— 这样
    // "factor=1, jitter=0" 的固定间隔路径与随机数状态完全无关，
    // 逐次恒等的断言才有意义（否则它只是在测 PRNG 恰好取到了 1.0）。
    double jitter() {
        if (policy_.jitter_ratio <= 0.0) return 1.0;
        const double u = rng_.next_unit();
        return 1.0 + policy_.jitter_ratio * (2.0 * u - 1.0);
    }

    BackoffPolicy requested_;          // 调用方原样传进来的（可能非法）
    BackoffPolicy policy_;             // 归一后的**实际生效**策略
    Xorshift64    rng_;
    std::uint64_t seed_;

    int    attempt_       = 0;
    int    total_wait_ms_ = 0;
    int    last_base_ms_  = 0;
    int    last_delay_ms_ = 0;
    double last_jitter_   = 1.0;

    bool        normalized_ = false;
    std::string normalize_reason_;
};

}  // namespace comm
}  // namespace ems
