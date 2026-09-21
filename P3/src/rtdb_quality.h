// =====================================================================
// P3 / 在 C++ 里安全地取用 RT_DB 的品质编码
//
// ---------------------------------------------------------------------
// 这个头解决的问题：品质编码**不许在 P3 里手抄**
// ---------------------------------------------------------------------
// 曾经在 rtdb_source.h 里手写了一份 `constexpr long kRtdbQualityGood = 0;`，
// 而 rt_db_structs.h 里 QUALITY_GOOD 的真值是 1。后果不是崩溃，是：
//   · 全部好点被判成坏点 —— 调度画面上 24 个点全灰（值却是对的）；
//   · 协议层看不出异常，它只是忠实地把源说的话翻译出去；
//   · 单元测试全绿 ——「往返一致」永远发现不了符号写反。
// 唯一的解法是**引用真相源**，让编译器替我们盯着上游枚举有没有变。
//
// ---------------------------------------------------------------------
// ★ 必须在使用任何 C++ 标准库头之前 include
// ---------------------------------------------------------------------
// rt_db_structs.h 为了在 Windows 上模拟 C11 原子操作，定义了**函数式宏**：
//     #define atomic_store_explicit(ptr, val, order) (*(ptr) = (val))
//     #define atomic_thread_fence(order) MemoryBarrier()
// 函数式宏的匹配**不看命名空间限定**，所以 libstdc++ 里
//     std::atomic_store_explicit(...)  /  std::atomic_thread_fence(...)
// 这些**声明**会被就地展开成一堆 `(*(ptr) = (val))`，编译器报的却是
// 「expected unqualified-id before 'sizeof'」—— 错误全部指向
// rt_db_structs.h 的第 34 行，跟真正出问题的地方隔着十万八千里。
// 连本项目 iec104_server.h 里自己写的 std::atomic_thread_fence 都会中招。
//
// 所以本头在 include 之后**立刻把那些宏擦掉**，并加了一道前置检查：
// 谁把它 include 晚了，就得到一个明确的 #error，而不是一堆莫名其妙的报错。
// =====================================================================

#ifndef EMS_P3_RTDB_QUALITY_H
#define EMS_P3_RTDB_QUALITY_H

// --- 前置检查：必须在 C++ 标准库之前 -----------------------------------
// 这几组保护宏都是 libstdc++ 的经典命名；命中任意一个就说明已经晚了。
#if defined(_GLIBCXX_ATOMIC) || defined(_GLIBCXX_STRING) || defined(_GLIBCXX_VECTOR) || \
    defined(_GLIBCXX_MEMORY) || defined(_GLIBCXX_THREAD) || defined(_GLIBCXX_MUTEX)
#error "rtdb_quality.h 必须在任何 C++ 标准库头之前 include —— 否则 rt_db_structs.h 的 C11 原子宏会破坏 libstdc++ 的声明"
#endif

extern "C" {
#include "rt_db_structs.h"
}

// --- 擦掉 C11 原子宏（Windows 分支是一批宏，Linux 分支来自 <stdatomic.h>）---
// 我们只需要 data_quality_t 这个枚举；原子操作在 C++ 侧一律用 std::atomic。
// 用 #ifdef 包住：两边分支各有一部分，擦不到也无所谓。
#ifdef atomic_init
#undef atomic_init
#endif
#ifdef atomic_thread_fence
#undef atomic_thread_fence
#endif
#ifdef atomic_signal_fence
#undef atomic_signal_fence
#endif
#ifdef atomic_is_lock_free
#undef atomic_is_lock_free
#endif
#ifdef atomic_load
#undef atomic_load
#endif
#ifdef atomic_load_explicit
#undef atomic_load_explicit
#endif
#ifdef atomic_store
#undef atomic_store
#endif
#ifdef atomic_store_explicit
#undef atomic_store_explicit
#endif
#ifdef atomic_exchange
#undef atomic_exchange
#endif
#ifdef atomic_exchange_explicit
#undef atomic_exchange_explicit
#endif
#ifdef atomic_compare_exchange_strong
#undef atomic_compare_exchange_strong
#endif
#ifdef atomic_compare_exchange_strong_explicit
#undef atomic_compare_exchange_strong_explicit
#endif
#ifdef atomic_compare_exchange_weak
#undef atomic_compare_exchange_weak
#endif
#ifdef atomic_compare_exchange_weak_explicit
#undef atomic_compare_exchange_weak_explicit
#endif
#ifdef atomic_fetch_add
#undef atomic_fetch_add
#endif
#ifdef atomic_fetch_add_explicit
#undef atomic_fetch_add_explicit
#endif
#ifdef atomic_fetch_sub
#undef atomic_fetch_sub
#endif
#ifdef atomic_fetch_sub_explicit
#undef atomic_fetch_sub_explicit
#endif
#ifdef atomic_fetch_or
#undef atomic_fetch_or
#endif
#ifdef atomic_fetch_or_explicit
#undef atomic_fetch_or_explicit
#endif
#ifdef atomic_fetch_xor
#undef atomic_fetch_xor
#endif
#ifdef atomic_fetch_xor_explicit
#undef atomic_fetch_xor_explicit
#endif
#ifdef atomic_fetch_and
#undef atomic_fetch_and
#endif
#ifdef atomic_fetch_and_explicit
#undef atomic_fetch_and_explicit
#endif
#ifdef atomic_flag_test_and_set
#undef atomic_flag_test_and_set
#endif
#ifdef atomic_flag_test_and_set_explicit
#undef atomic_flag_test_and_set_explicit
#endif
#ifdef atomic_flag_clear
#undef atomic_flag_clear
#endif
#ifdef atomic_flag_clear_explicit
#undef atomic_flag_clear_explicit
#endif

namespace iec104 {
namespace rtdb_q {

// 直接引用真相源，不抄数字
constexpr long kBad       = static_cast<long>(QUALITY_BAD);
constexpr long kGood      = static_cast<long>(QUALITY_GOOD);
constexpr long kUncertain = static_cast<long>(QUALITY_UNCERTAIN);

// 编译期对账：上游枚举一变，这里立刻红，而不是等到现场看画面全灰
static_assert(kGood == 1,
              "RT_DB 的 QUALITY_GOOD 变了 —— P3/src/rtdb_quality.h 与 rtdb_source.h 的品质映射要跟着改");
static_assert(kBad != kGood && kGood != kUncertain && kBad != kUncertain,
              "RT_DB 品质枚举出现同值，translate_quality() 的 switch 会走错分支");

} // namespace rtdb_q
} // namespace iec104

#endif // EMS_P3_RTDB_QUALITY_H
