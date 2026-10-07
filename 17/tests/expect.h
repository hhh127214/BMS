// =====================================================================
// 17/ 测试公共断言宏（照 13/tests/test_modbus_tcp.cpp 的骨架，抽成一份）
//
// 为什么抽出来：17/ 有 6 个测试可执行文件。若每个文件各抄一份宏，
// 一旦有人改了 EXPECT_NEAR 的打印格式，六个文件的失败输出就会不一致 ——
// 而失败输出正是排查的第一现场。
//
// ★ 用法（每个测试文件仍必须有**自己的 main 与 PASS=/FAIL= 收尾**）：
//
//     #include "expect.h"
//     static int g_pass = 0;
//     static int g_fail = 0;
//     ...
//     int main() { ... TEST_TAIL(); }
//
// 宏引用 g_pass / g_fail，展开点必须能看到这两个名字。
// =====================================================================

#pragma once

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (cond) {                                                           \
            ++g_pass;                                                         \
        } else {                                                              \
            ++g_fail;                                                         \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__            \
                      << " : " << #cond << std::endl;                         \
        }                                                                     \
    } while (0)

#define EXPECT_EQ(a, b)                                                       \
    do {                                                                      \
        long long va = (long long)(a), vb = (long long)(b);                   \
        if (va == vb) {                                                       \
            ++g_pass;                                                         \
        } else {                                                              \
            ++g_fail;                                                         \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__            \
                      << " : " << #a << "=" << va << " vs " << #b << "=" << vb \
                      << std::endl;                                           \
        }                                                                     \
    } while (0)

#define EXPECT_NEAR(a, b, eps)                                                \
    do {                                                                      \
        double va = (a), vb = (b);                                            \
        if (std::fabs(va - vb) <= (double)(eps)) {                            \
            ++g_pass;                                                         \
        } else {                                                              \
            ++g_fail;                                                         \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__            \
                      << " : " << #a << "=" << va << " vs " << #b << "=" << vb \
                      << " (eps " << (eps) << ")" << std::endl;               \
        }                                                                     \
    } while (0)

// 字符串相等（含"两串都相等"的定位输出）
#define EXPECT_STR_EQ(a, b)                                                   \
    do {                                                                      \
        std::string va = (a), vb = (b);                                       \
        if (va == vb) {                                                       \
            ++g_pass;                                                         \
        } else {                                                              \
            ++g_fail;                                                         \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__            \
                      << " : " << #a << "='" << va << "' vs " << #b << "='"   \
                      << vb << "'" << std::endl;                              \
        }                                                                     \
    } while (0)

// 断言"确实有区分度"的辅助：故意改坏的输入必须让某类计数 > 0
//
// ★ 用 double 比较，**不要** cast 成 long long：一旦被断言的差值小于 1
//   （例如 SOC 往返损耗 0.00136），整型截断会把"确实 > 0"变成 0，
//   于是**正确的实现被判红**。这类"反向守卫自己把自己坑了"的错很难查 ——
//   因为它的现象是"实现越精确越容易红"。
#define EXPECT_GT0(v)                                                         \
    do {                                                                      \
        double vv = (double)(v);                                              \
        if (vv > 0.0) {                                                       \
            ++g_pass;                                                         \
        } else {                                                              \
            ++g_fail;                                                         \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__            \
                      << " : 期待 > 0（反向守卫），实为 " << vv << " : "       \
                      << #v << std::endl;                                     \
        }                                                                     \
    } while (0)

#define TEST_BANNER(name)                                                     \
    std::printf("=== %s ===\n\n", name)

#define TEST_TAIL()                                                           \
    do {                                                                      \
        std::printf("\n");                                                    \
        if (g_fail == 0) std::printf("ALL TESTS PASSED\n");                    \
        std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);                      \
    } while (0)
