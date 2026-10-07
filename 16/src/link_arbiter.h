// =====================================================================
// 16/ — 主备双链路仲裁（LinkArbiter）
//
// 现场形态：一台 EMS 常常对同一个对端有**两条物理通路** ——
//   例：主用光纤以太网 + 备用 4G/无线；或 主用 A 网 + 备用 B 网；
//       或 Modbus TCP 主链路 + RTU 串口备链路。
// 两条链路用**同一个抽象**（connect/close/is_up/endpoint）暴露给仲裁器，
// 于是仲裁逻辑与"链路到底是什么"彻底解耦 —— 这也是它可测的前提。
//
// ★ 为什么必须"概念式"（C++17 无 concept）：本模块不引第三方、也不接受
//   虚函数开销与堆分配。用检测器惯用法（void_t）在**编译期**断言链路满足
//   契约 —— 比运行时 dynamic_cast 早一步失败，且零开销。
//
// ★ 滞环（hysteresis）是**这个文件存在的全部理由**：
//   不要滞环的写法长这样：
//     if (主链断) 切备;  if (主链通) 切回主;
//   主链路"时通时断"（现场最常见的劣化形态：接口 CRC 错、光衰临界）
//   会让它在阈值附近**来回抖** —— 每次切换都重连、重握手、丢一帧数据。
//   量化：链路劣化到 50% 可用率时，上面的写法每次采样都可能切；
//   带滞环（N=3, M=5）后，同一个序列只剩个位数次切换（测试 T13 用
//   固定序列把这个上界**断言**下来）。
//
//   滞环的数学形式：**向下（切备）的阈值 N  <  向上（切回）的阈值 M**。
//   中间 [N, M) 这一段是"保持现状"的死区 —— 死区越宽越稳、越窄越灵敏。
//
// ★ 第三条纪律：**绝不切换到一条不可用的链路上**。
//   否则"两条都断"时会变成每拍一次 switches++，日志被刷屏、
//   而且 current() 变成随机值。这里用 `target.is_up()` 做门禁，
//   并把它写成测试 T14 的硬断言。
//
// 编译：纯头文件，无外部依赖。
// =====================================================================

#pragma once

#include <string>
#include <type_traits>
#include <utility>

namespace ems {
namespace comm {

// =====================================================================
// 链路契约检测（C++17 检测器惯用法，等价于 C++20 的 concept）
//
// 要求：bool connect() / void close() / bool is_up() const / std::string endpoint() const
// 注意这里用 `declval<L&>()`（可变的）来测 connect/close，
// 用 `declval<const L&>()` 来测 is_up/endpoint —— 正好把 const 正确性也测进去。
// =====================================================================
template <class L, class = void>
struct is_link_contract : std::false_type {};

template <class L>
struct is_link_contract<
    L,
    decltype(std::declval<L&>().connect(),
             std::declval<L&>().close(),
             std::declval<const L&>().is_up(),
             std::declval<const L&>().endpoint(),
             (void)0)>
    : std::true_type {};

// =====================================================================
// 仲裁策略
// =====================================================================
struct ArbiterPolicy {
    // 主链**连续**失败 N 次 → 切到备链
    int fail_threshold = 3;
    // 主链**连续**成功 M 次 → 切回主链。★ 必须 > fail_threshold 才有滞环。
    int recover_threshold = 5;

    std::string validate() const {
        if (fail_threshold < 1) {
            return "fail_threshold 必须 >= 1（0 表示一次失败都不允许，等于不停切换）";
        }
        if (recover_threshold < 1) {
            return "recover_threshold 必须 >= 1";
        }
        if (recover_threshold <= fail_threshold) {
            return "recover_threshold 必须 > fail_threshold，否则没有滞环（阈值附近会来回抖）";
        }
        return std::string();
    }
    bool is_valid() const { return validate().empty(); }

    // 归一：M <= N 时取 M = N + 1（最小可用的滞环）。
    // ★ 不静默接受 M <= N —— 那是"没有滞环"，正是本模块要防的那件事；
    //   归一后 `normalized()` 为真，调用方/测试能看见"你的配置被改过"。
    ArbiterPolicy normalized() const {
        ArbiterPolicy p = *this;
        if (p.fail_threshold < 1) p.fail_threshold = 1;
        if (p.recover_threshold < 1) p.recover_threshold = p.fail_threshold + 1;
        if (p.recover_threshold <= p.fail_threshold) {
            p.recover_threshold = p.fail_threshold + 1;
        }
        return p;
    }
};

// =====================================================================
// 主备仲裁器
//
// 单拍流程（step()）：
//   ① 对两条链路各做一次"拉起尝试"：**只有当前未 up 的链路**才调 connect()，
//      已经 up 的完全不碰（现场不容许对在用的链路反复重连）。
//   ② 读两条链路的 is_up() 作为本拍的可用性样本。
//   ③ 用**主链**的样本驱动连续成功/失败计数，按滞环决定是否切换。
//
// ★ 为什么每拍都探主链（即使当前在备链上）：
//   不探就永远不知道主链恢复了 —— 被动看 is_up() 只会一直读到 false
//   （我们上次把它 close 掉了）。"切回来"这件事本身要求对外发起一次
//   主动验证。代价是每拍一次 connect 尝试；探的频率由调用方用 Backoff
//   控制（见 16/src/backoff.h），仲裁器不关心节奏。
// =====================================================================
template <class PrimaryLink, class BackupLink>
class LinkArbiter {
    static_assert(is_link_contract<PrimaryLink>::value,
                  "主链类型必须提供 bool connect() / void close() / "
                  "bool is_up() const / std::string endpoint() const");
    static_assert(is_link_contract<BackupLink>::value,
                  "备链类型必须提供 bool connect() / void close() / "
                  "bool is_up() const / std::string endpoint() const");

public:
    enum { kPrimary = 0, kBackup = 1 };

    LinkArbiter(PrimaryLink primary, BackupLink backup,
                ArbiterPolicy policy = ArbiterPolicy())
        : primary_(std::move(primary)),
          backup_(std::move(backup)),
          requested_(policy),
          policy_(policy.normalized()) {
        normalize_reason_ = requested_.validate();
        normalized_       = !normalize_reason_.empty();
    }

    // -----------------------------------------------------------------
    // 单拍
    // -----------------------------------------------------------------

    // 返回本拍结束后"当前活动链路是否可用"（与 is_up() 一致）。
    bool step() {
        // ① 拉起：只对没起来的链路尝试 connect()，每拍每条链路至多一次
        if (!primary_.is_up()) primary_.connect();
        if (!backup_.is_up())  backup_.connect();

        // ② 采样
        const bool p_up = primary_.is_up();
        const bool b_up = backup_.is_up();

        // ③ 主链健康计数（与当前在哪条链上无关）
        if (p_up) {
            consecutive_failures_ = 0;
            ++consecutive_successes_;
        } else {
            consecutive_successes_ = 0;
            ++consecutive_failures_;
        }

        if (active_ == kPrimary) {
            // 主链连续失败到 N → 切备（★ 门禁：备链必须真的可用）
            if (consecutive_failures_ >= policy_.fail_threshold && b_up) {
                switch_to(kBackup, /*is_failover=*/true);
            }
        } else {
            // 备链期间主链连续恢复到 M → 切回（★ 门禁：主链必须真的可用）
            if (consecutive_successes_ >= policy_.recover_threshold && p_up) {
                switch_to(kPrimary, /*is_failover=*/false);
            }
        }

        return is_up();
    }

    // 把两条链路都关掉（重连风暴后的"归零"入口）
    void close_all() {
        primary_.close();
        backup_.close();
    }

    // -----------------------------------------------------------------
    // 可观测
    // -----------------------------------------------------------------
    int current() const { return active_; }               // 0=主 / 1=备
    int switches() const { return switches_; }
    int failovers() const { return failovers_; }          // 主 → 备 的次数
    int failbacks() const { return failbacks_; }          // 备 → 主 的次数
    int consecutive_failures() const { return consecutive_failures_; }
    int consecutive_successes() const { return consecutive_successes_; }
    const ArbiterPolicy& policy() const { return policy_; }
    const ArbiterPolicy& requested_policy() const { return requested_; }
    bool normalized() const { return normalized_; }
    const std::string& normalize_reason() const { return normalize_reason_; }

    bool is_up() const {
        return active_ == kPrimary ? primary_.is_up() : backup_.is_up();
    }
    std::string current_endpoint() const {
        return active_ == kPrimary ? primary_.endpoint() : backup_.endpoint();
    }

    PrimaryLink&       primary()       { return primary_; }
    BackupLink&        backup()        { return backup_; }
    const PrimaryLink& primary() const { return primary_; }
    const BackupLink&  backup() const  { return backup_; }

    // 清零计数但**保留当前活动链路**（用于滚动观察窗口）
    void reset_counters() {
        switches_              = 0;
        failovers_             = 0;
        failbacks_             = 0;
        consecutive_failures_  = 0;
        consecutive_successes_ = 0;
    }

private:
    void switch_to(int target, bool is_failover) {
        // ★ 双保险：调用点已经检查过，这里再挡一次。任何"切到不可用链路"
        //   的路径都是缺陷，宁可多一次判断。
        if (target != kPrimary && target != kBackup) return;
        if (active_ == target) return;
        const bool target_up = (target == kPrimary) ? primary_.is_up() : backup_.is_up();
        if (!target_up) return;

        // 关掉旧链路：两条同时 up 会让对端看到两个会话（很多 PLC 只允许 1 个），
        // 而且"到底谁在用"在抓包里就分不清了。
        if (active_ == kPrimary) primary_.close();
        else                     backup_.close();

        active_ = target;
        ++switches_;
        if (is_failover) ++failovers_;
        else             ++failbacks_;
    }

    PrimaryLink   primary_;
    BackupLink    backup_;
    ArbiterPolicy requested_;
    ArbiterPolicy policy_;

    int active_ = kPrimary;
    int switches_              = 0;
    int failovers_             = 0;
    int failbacks_             = 0;
    int consecutive_failures_  = 0;
    int consecutive_successes_ = 0;

    bool        normalized_ = false;
    std::string normalize_reason_;
};

}  // namespace comm
}  // namespace ems
