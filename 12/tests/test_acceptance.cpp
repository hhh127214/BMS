// =====================================================================
// 12/ 单元测试 —— 周期 12：最终验收（T51~T59）
//
//   T51  A1 功能维度：9 项全通过（策略注册/启停、闭环、状态机七态、
//        三适配器等价、点表自检、配置化、计划装载、关口功率口径）
//   T52  A2 策略维度：9 项全通过（9 个策略逐一的触发工况核对）
//   T53  A3 安全维度：11 项全通过（9 条约束 + 区间矛盾语义 + 锁存）
//   T54  A4 性能维度：5 项全通过 + 阈值真的可配置（防"阈值写了不生效"）
//   T55  A5 稳定性维度：7 项全通过（24h + 跨进程 + stale/SOE/抖动 + BMS 禁充放）
//   T56  A6 协同维度：8 项全通过 + 场景 id 与设计 §7 周期 9 一一对应
//   T57  A7 文档维度：6 项全通过
//   T58  报告渲染：md/json/html 三件套自洽 —— JSON 用 P1 的 json_lite
//        反解回结构体，证明报告是**机器可读**的（不只是给人看的文本）
//   T59  汇总一致性：passed/total/failed_dimensions 与逐维度累加一致
//
// 与 11/test_system_integration.cpp（T41~T49）的差别：
//   11/ 验的是"链路通不通"（单点、可归因）；
//   12/ 验的是"交付物达不达标"（七维度、阈值化）。
//   12/ 的检查项**全部**来自 acceptance_runner.h —— 测试只做两件事：
//     ① 断言每个维度的每一项都通过；
//     ② 断言报告本身的性质（机器可读、汇总自洽、阈值可配）。
//
// 编译（见 12/scripts/build_test.bat）：
//   gcc: rt_db_api.c / ems_point_table.c / ems_rt_db_setup.c
//   g++: -I src -I ../04/src ... -I ../P2/src + RT_DB 头文件路径，-lpsapi
// =====================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "acceptance_runner.h"

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

// =====================================================================
// 公共装置：跑一次验收，后续 7 个用例共享同一份报告
//
// 为什么只跑一次：A4（20000 拍 + 24h 仿真）与 A5（86400 拍 + 跨进程）
// 是**不可重入且昂贵**的测量。逐维度各跑一遍会让测试时间翻数倍，而且
// 每次跑出来的耗时/句柄数都不同 —— 断言会变成"赌运气"。
// 一次测量、七项断言，是验收测试唯一合理的结构。
// =====================================================================
struct ReportFixture {
    AcceptanceReport rep;
    bool ok = false;

    ReportFixture() {
        AcceptanceRunner::Options opt;
        opt.root            = "..";
        opt.perf_steps      = 3000;   // 性能采样压缩（判据是阈值，与拍数无关）
        opt.rtdb_soak_steps = 2000;   // 跨进程长稳压缩
        opt.scenario_scale  = 1;      // A6 必须跑设计原设定（12000 拍/场景）
        AcceptanceRunner runner(opt);
        rep = runner.run();
        ok  = !rep.dims.empty();
    }

    const DimensionReport* dim(const std::string& code) const {
        for (const auto& d : rep.dims) if (d.code == code) return &d;
        return nullptr;
    }
};

static const ReportFixture& fixture() {
    static ReportFixture f;   // 只构造一次
    return f;
}

// 打印某维度里未通过的项（失败时定位用）
static void dump_failures(const DimensionReport& d) {
    for (const auto& c : d.items) {
        if (!c.pass) {
            std::cerr << "    [" << c.id << "] " << c.name
                      << "\n      判据: " << c.criterion
                      << "\n      实测: " << c.evidence << std::endl;
        }
    }
}

// =====================================================================
// T51: A1 功能验收
// =====================================================================
static void test_51_functional_dimension(const ReportFixture& fx) {
    std::cerr << "[T51] A1 功能验收维度（策略注册/启停、闭环、状态机七态、"
                 "三适配器等价、点表自检、配置化、计划装载、关口功率口径）...\n";
    const auto* d = fx.dim("A1");
    EXPECT(d != nullptr);
    if (!d) return;
    EXPECT(d->items.size() == 9);
    EXPECT(d->passed() == 9);
    EXPECT(d->pass());
    if (!d->pass()) dump_failures(*d);

    // 逐项 id 定锚：防止后人删项或改名而"通过率不变"
    const char* ids[] = {"A1-01", "A1-02", "A1-03", "A1-04", "A1-05",
                         "A1-06", "A1-07", "A1-08", "A1-09"};
    for (const char* want : ids) {
        bool found = false;
        for (const auto& c : d->items) if (c.id == want) found = true;
        EXPECT(found);
    }
}

// =====================================================================
// T52: A2 策略验收
// =====================================================================
static void test_52_strategy_dimension(const ReportFixture& fx) {
    std::cerr << "[T52] A2 策略验收维度（9 个策略的设计意图单点核对）...\n";
    const auto* d = fx.dim("A2");
    EXPECT(d != nullptr);
    if (!d) return;
    EXPECT(d->items.size() == 9);
    EXPECT(d->passed() == 9);
    EXPECT(d->pass());
    if (!d->pass()) dump_failures(*d);

    // 每个检查项的名称里必须出现对应的 strategy_id 前缀 S01..S09
    for (int i = 1; i <= 9; ++i) {
        char tag[8];
        std::snprintf(tag, sizeof(tag), "S%02d ", i);
        bool hit = false;
        for (const auto& c : d->items) if (c.name.find(tag) != std::string::npos) hit = true;
        EXPECT(hit);
    }
}

// =====================================================================
// T53: A3 安全验收
// =====================================================================
static void test_53_safety_dimension(const ReportFixture& fx) {
    std::cerr << "[T53] A3 安全验收维度（9 条约束 + 区间矛盾语义 + 急停锁存 + "
                 "全域硬不变量）...\n";
    const auto* d = fx.dim("A3");
    EXPECT(d != nullptr);
    if (!d) return;
    EXPECT(d->items.size() == 11);
    EXPECT(d->passed() == 11);
    EXPECT(d->pass());
    if (!d->pass()) dump_failures(*d);

    // 关键三项必须存在（它们承载本项目的三条核心安全决策）
    const char* key[] = {"A3-07", "A3-09", "A3-11"};
    for (const char* k : key) {
        bool found = false;
        for (const auto& c : d->items) if (c.id == k) found = true;
        EXPECT(found);
    }
}

// =====================================================================
// T54: A4 性能验收 + 阈值可配置
// =====================================================================
static void test_54_performance_dimension(const ReportFixture& fx) {
    std::cerr << "[T54] A4 性能验收维度 + 判据阈值确实生效...\n";
    const auto* d = fx.dim("A4");
    EXPECT(d != nullptr);
    if (!d) return;
    EXPECT(d->items.size() == 5);
    EXPECT(d->passed() == 5);
    EXPECT(d->pass());
    if (!d->pass()) dump_failures(*d);

    // ---- 阈值必须**真的**参与判定，而不是装饰 ----
    // 把均值上限压到 1e-9 us（任何真实实现都不可能满足）→ A4-01 必须变红。
    // 这一条防的是"判据写了但判的时候没用"这一类静默失效。
    {
        AcceptanceRunner::Options strict;
        strict.root            = "..";
        strict.perf_steps      = 200;
        strict.perf_mean_us_max = 1e-9;
        strict.rtdb_soak_steps = 200;
        AcceptanceRunner r(strict);
        const AcceptanceReport rep = r.run();
        bool a4_01_failed = false;
        for (const auto& dim_ : rep.dims) {
            if (dim_.code != "A4") continue;
            for (const auto& c : dim_.items) {
                if (c.id == "A4-01") a4_01_failed = !c.pass;
            }
        }
        EXPECT(a4_01_failed);
    }

    // ---- 反向：把阈值放到极宽 → A4 必须全绿（说明不是无脑判红）----
    {
        AcceptanceRunner::Options loose;
        loose.root             = "..";
        loose.perf_steps       = 200;
        loose.perf_mean_us_max = 1e9;
        loose.perf_max_us_max  = 1e9;
        loose.perf_duty_pct_max = 100.0;
        loose.perf_speedup_min = 1.0;
        loose.rtdb_soak_steps  = 200;
        AcceptanceRunner r(loose);
        const AcceptanceReport rep = r.run();
        const DimensionReport* p = nullptr;
        for (const auto& dim_ : rep.dims) if (dim_.code == "A4") p = &dim_;
        EXPECT(p != nullptr);
        if (p) EXPECT(p->passed() == 5);
    }
}

// =====================================================================
// T55: A5 稳定性验收
// =====================================================================
static void test_55_stability_dimension(const ReportFixture& fx) {
    std::cerr << "[T55] A5 稳定性验收维度（24h 长稳 + 跨进程 + BMS 禁充放 + stale/SOE/抖动）...\n";
    const auto* d = fx.dim("A5");
    EXPECT(d != nullptr);
    if (!d) return;
    // 7 项 = A5-01~03（24h 长稳）+ A5-04（跨进程）+ A5-05（SOE）
    //        + A5-06（可观测性）+ A5-07（BMS 禁放位，2026-09-19 补）
    EXPECT(d->items.size() == 7);
    EXPECT(d->passed() == 7);
    EXPECT(d->pass());
    if (!d->pass()) dump_failures(*d);
}

// =====================================================================
// T56: A6 多策略协同验收
// =====================================================================
static void test_56_coordination_dimension(const ReportFixture& fx) {
    std::cerr << "[T56] A6 多策略协同维度（设计 §7 周期 9 的 7 个场景）...\n";
    const auto* d = fx.dim("A6");
    EXPECT(d != nullptr);
    if (!d) return;
    EXPECT(d->items.size() == 8);
    EXPECT(d->passed() == 8);
    EXPECT(d->pass());
    if (!d->pass()) dump_failures(*d);

    // 7 个场景 id 必须与 09/ 一致，且一个不少
    const char* scen[] = {"S1", "S2", "S3", "S4", "S5", "S6", "S7"};
    for (const char* s : scen) {
        bool hit = false;
        for (const auto& c : d->items) {
            if (c.name.compare(0, std::string(s).size(), s) == 0 &&
                c.name.size() > std::string(s).size() &&
                c.name[std::string(s).size()] == ' ') {
                hit = true;
            }
        }
        EXPECT(hit);
    }
    // 汇总项
    bool has_summary = false;
    for (const auto& c : d->items) if (c.id == "A6-08") has_summary = true;
    EXPECT(has_summary);
}

// =====================================================================
// T57: A7 文档验收
// =====================================================================
static void test_57_documentation_dimension(const ReportFixture& fx) {
    std::cerr << "[T57] A7 文档验收维度（模块文档/构建脚本/构建入口/报告落盘）...\n";
    const auto* d = fx.dim("A7");
    EXPECT(d != nullptr);
    if (!d) return;
    EXPECT(d->items.size() == 6);
    EXPECT(d->passed() == 6);
    EXPECT(d->pass());
    if (!d->pass()) dump_failures(*d);
}

// =====================================================================
// T58: 报告渲染 —— 三件套自洽，且 JSON 必须机器可读
//
// 验收报告的价值一半在"人能看"，另一半在"机器能判"。后者要求 JSON 是
// 真的 JSON。这里用 P1 的 json_lite 反解我们自己渲染的 JSON —— 用**另一个
// 模块**的解析器验证输出，比自证更有说服力（也顺带证明 P1 的解析器可用）。
// =====================================================================
static void test_58_report_rendering(const ReportFixture& fx) {
    std::cerr << "[T58] 报告渲染（md / json / html 三件套 + JSON 机器可读）...\n";
    const AcceptanceReport& rep = fx.rep;

    // ---- Markdown ----
    const std::string md = rep.to_markdown();
    EXPECT(md.find("# 工商业储能 EMS V2.0 最终验收报告") != std::string::npos);
    EXPECT(md.find("## A1 功能验收") != std::string::npos);
    EXPECT(md.find("## A7 文档验收") != std::string::npos);
    EXPECT(md.find("| **合计** |") != std::string::npos);
    EXPECT(md.size() > 2000);

    // ---- HTML ----
    const std::string html = rep.to_html();
    EXPECT(html.find("<!DOCTYPE html>") == 0);
    EXPECT(html.find("A7 文档验收") != std::string::npos);
    EXPECT(html.find("</html>") != std::string::npos);
    // 表格行数：每个检查项一行
    {
        std::size_t rows = 0;
        for (std::size_t p = html.find("<tr class="); p != std::string::npos;
             p = html.find("<tr class=", p + 1)) {
            ++rows;
        }
        EXPECT(rows >= static_cast<std::size_t>(rep.total()));
    }

    // ---- JSON：必须能被独立解析器反解 ----
    const std::string js = rep.to_json();
    json::ParseError perr;
    const json::Value v = json::parse(js, &perr);
    EXPECT(perr.ok);            // ParseError::ok 是成员变量，不是函数
    EXPECT(v.is_object());

    const json::Value* summary = v.find("summary");
    EXPECT(summary != nullptr);
    if (summary) {
        EXPECT(summary->find("passed") != nullptr);
        EXPECT(summary->find("total") != nullptr);
        const json::Value* pv = summary->find("passed");
        const json::Value* tv = summary->find("total");
        if (pv && tv) {
            EXPECT(static_cast<int>(pv->number(-1.0)) == rep.passed());
            EXPECT(static_cast<int>(tv->number(-1.0)) == rep.total());
        }
    }
    const json::Value* dims = v.find("dimensions");
    EXPECT(dims != nullptr);
    if (dims && dims->is_array()) {
        EXPECT(dims->arr.size() == rep.dims.size());
        // 每个维度必须有 items 数组，且长度与内存里的一致
        std::size_t total_items = 0;
        for (const auto& dv : dims->arr) {
            const json::Value* items = dv.find("items");
            EXPECT(items != nullptr);
            if (items && items->is_array()) total_items += items->arr.size();
        }
        EXPECT(total_items == static_cast<std::size_t>(rep.total()));
    }
}

// =====================================================================
// T59: 汇总一致性 —— 报告的"结论"必须能从明细重算出来
//
// 这是防"汇总与明细脱节"的最后一道闸：任何一处数字对不上就说明报告不可信。
// =====================================================================
static void test_59_report_consistency(const ReportFixture& fx) {
    std::cerr << "[T59] 汇总一致性（passed/total/失败维度数 可由明细重算）...\n";
    const AcceptanceReport& rep = fx.rep;

    int passed = 0, total = 0, failed_dims = 0;
    for (const auto& d : rep.dims) {
        EXPECT(d.total() == static_cast<int>(d.items.size()));
        int p = 0;
        for (const auto& c : d.items) if (c.pass) ++p;
        EXPECT(p == d.passed());
        passed += p;
        total  += d.total();
        if (d.items.empty() || p != d.total()) ++failed_dims;
    }
    EXPECT(rep.passed() == passed);
    EXPECT(rep.total() == total);
    EXPECT(rep.failed_dimensions() == failed_dims);

    // 维度数量与顺序：A1..A7
    EXPECT(rep.dims.size() == 7);
    if (rep.dims.size() == 7) {
        const char* codes[] = {"A1", "A2", "A3", "A4", "A5", "A6", "A7"};
        for (int i = 0; i < 7; ++i) EXPECT(rep.dims[i].code == codes[i]);
    }

    // 每个检查项三要素必须齐全（判据/实测非空）—— 验收项不允许"空口通过"
    int incomplete = 0;
    for (const auto& d : rep.dims) {
        for (const auto& c : d.items) {
            if (c.id.empty() || c.name.empty() ||
                c.criterion.empty() || c.evidence.empty()) ++incomplete;
        }
    }
    EXPECT(incomplete == 0);

    // 生成时间戳格式：YYYY-mm-dd HH:MM:SS（19 字符）
    EXPECT(rep.generated_at.size() == 19);
    EXPECT(rep.generated_at[4] == '-' && rep.generated_at[13] == ':');

    std::cerr << "  汇总: " << rep.passed() << "/" << rep.total()
              << " 项通过，" << rep.failed_dimensions() << " 个维度未全通过\n";
    if (!rep.pass()) {
        for (const auto& d : rep.dims) if (!d.pass()) dump_failures(d);
    }
}

// =====================================================================
int main() {
    std::cerr << "=== 12/ 周期 12 最终验收 · 模块自测（T51~T59）===\n";

    const ReportFixture& fx = fixture();
    std::cerr << "（验收已执行：共 " << fx.rep.dims.size() << " 个维度）\n\n";

    test_51_functional_dimension(fx);
    test_52_strategy_dimension(fx);
    test_53_safety_dimension(fx);
    test_54_performance_dimension(fx);
    test_55_stability_dimension(fx);
    test_56_coordination_dimension(fx);
    test_57_documentation_dimension(fx);
    test_58_report_rendering(fx);
    test_59_report_consistency(fx);

    std::cerr << "\n=== PASS=" << g_pass << " FAIL=" << g_fail << " ===\n";
    if (fx.rep.pass()) {
        std::cerr << "验收结论: 通过（" << fx.rep.passed() << "/"
                  << fx.rep.total() << "）\n";
    } else {
        std::cerr << "验收结论: 不通过（" << fx.rep.passed() << "/"
                  << fx.rep.total() << "）\n";
    }
    return (g_fail == 0) ? 0 : 1;
}
