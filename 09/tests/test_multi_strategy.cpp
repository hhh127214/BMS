// =====================================================================
// 09/ 单元测试 —— 周期 9：多策略组合测试（闭环时序级）
//
//   T91 ~ T97：设计方案 §7 的 7 个组合场景，每个跑 1200 s 闭环
//   断言维度（对应周期目标"杜绝策略指令冲突/互相覆盖/频繁切换/
//              双向充放电/功率超限"）：
//     · 指令逃逸     p_cmd ∈ [p_lower, p_upper]        （仲裁不变量）
//     · 功率超限     |p_cmd| ≤ min(PCS, BMS)           （设备不变量）
//     · 门控不变量   非运行态指令恒 0                   （状态机不变量）
//     · 双向充放电   指令符号翻转率 ≤ 0.20 /s
//     · 频繁切换     指令方向反转率 ≤ 1.00 /s
//     · 关口越界     P_grid ∈ [下限, 上限]
//     · 变压器越限   |P_grid| + 0.1·P_load ≤ 0.95·cap
//     · SOC 越界     SOC ∈ [soc_min, soc_max]
//
// 编译：g++ -std=c++17 -Wall -O2 -I src -I ../04/src -I ../05/src
//           -I ../06/src -I ../07/src -I ../08/src
//           tests/test_multi_strategy.cpp -o build/test_multi_strategy.exe
// =====================================================================

#include "scenario_runner.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace ems;

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

// 7 个场景的结果（跑一次，多处断言）
static std::vector<ScenarioResult> g_results;

// =====================================================================
// 场景结果断言（统一口径）
// =====================================================================
static void check_scenario(const ScenarioResult& r, const char* tid) {
    std::cerr << "[" << tid << "] " << r.name << " ...\n";
    std::cerr << r.to_string();

    // 硬不变量：这三条任何一条被破坏都是**架构级**问题，必须为 0
    EXPECT(r.samples > 0);
    EXPECT(r.out_of_interval == 0);   // 仲裁不变量：指令不得逃出区间
    EXPECT(r.over_limit == 0);        // 设备不变量：不得突破 PCS/BMS 限值
    EXPECT(r.gated_nonzero == 0);     // 状态机不变量：非运行态恒 0

    // 时序不变量：频繁切换 / 双向充放电
    EXPECT(r.flip_rate <= 0.20);      // 双向充放电
    EXPECT(r.rev_rate  <= 1.00);      // 频繁切换

    // 安全不变量
    EXPECT(r.soc_min >= 0.09);
    EXPECT(r.soc_max <= 0.91);

    // 场景特定不变量
    EXPECT(r.grid_breach == 0);
    EXPECT(r.tr_breach == 0);

    // 综合判定
    EXPECT(r.ok());
}

// =====================================================================
int main() {
    std::cerr << "=========================================\n"
              << " 09/ 周期 9 多策略组合测试（闭环时序级）\n"
              << "=========================================\n";

    auto cfgs = all_scenarios();
    g_results.reserve(cfgs.size());
    for (const auto& c : cfgs) g_results.push_back(run_scenario(c));

    const char* tids[] = {"T91", "T92", "T93", "T94", "T95", "T96", "T97"};
    for (size_t i = 0; i < g_results.size() && i < 7; ++i) {
        check_scenario(g_results[i], tids[i]);
    }

    std::cerr << "=========================================\n"
              << " PASS=" << g_pass << "  FAIL=" << g_fail << "\n"
              << "=========================================\n";
    return (g_fail == 0) ? 0 : 1;
}
