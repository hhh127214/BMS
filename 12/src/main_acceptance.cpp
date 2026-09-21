// =====================================================================
// 12/ — 周期 12：最终验收 · 命令行入口
//
//   build\acceptance.exe [--root DIR] [--perf-steps N] [--scenario-scale K]
//                        [--rtdb-ticks N] [--no-perf] [--no-write] [--quiet]
//
// 行为：
//   1. 跑七维度验收（A1..A7）
//   2. 标准输出打印逐项结论（判据 + 实测），供人工复核 / CI 抓取
//   3. 把完整报告写到 <root>/12/docs/ACCEPTANCE-REPORT.{md,json,html}
//   4. 退出码：0 = 全部通过；1 = 存在未通过项；2 = 参数错误
//
// 退出码是给 CI 用的 —— 验收报告可以给人看，但**门禁必须机器可判**。
// =====================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "acceptance_runner.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

using namespace ems;

static void usage() {
    std::cout <<
        "用法: acceptance.exe [选项]\n"
        "  --root DIR          工程根目录（默认 ..，即模块的上一级）\n"
        "  --perf-steps N      A4 性能采样拍数（默认 20000）\n"
        "  --rtdb-ticks N      A5 跨进程长稳拍数（默认 6000）\n"
        "  --scenario-scale K  A6 场景倍数（默认 1，即 09/ 原设定）\n"
        "  --no-write         不落盘报告（只打印）\n"
        "  --quiet            只打印维度汇总，不逐项展开\n"
        "  -h, --help         显示本帮助\n";
}

int main(int argc, char** argv) {
    AcceptanceRunner::Options opt;
    bool do_write = true;
    bool quiet    = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "缺少参数: " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--root")           opt.root = next("--root");
        else if (a == "--perf-steps")     opt.perf_steps = std::atoi(next("--perf-steps"));
        else if (a == "--rtdb-ticks")     opt.rtdb_soak_steps = std::atoi(next("--rtdb-ticks"));
        else if (a == "--scenario-scale") opt.scenario_scale = std::atoi(next("--scenario-scale"));
        else if (a == "--no-write")       do_write = false;
        else if (a == "--quiet")          quiet = true;
        else {
            std::cerr << "未知参数: " << a << "\n";
            usage();
            return 2;
        }
    }
    if (opt.perf_steps <= 0 || opt.rtdb_soak_steps <= 0 || opt.scenario_scale <= 0) {
        std::cerr << "数值参数必须为正整数\n";
        return 2;
    }

    std::cout << "=== 工商业储能 EMS V2.0 最终验收（周期 12）===\n";
    std::cout << "工程根目录 : " << opt.root << "\n";
    std::cout << "性能采样   : " << opt.perf_steps << " 拍 @ 10 Hz\n";
    std::cout << "长稳         : 24 h（86400 拍 @ 1 s）+ 跨进程 "
              << opt.rtdb_soak_steps << " 拍\n";
    std::cout << "协同场景   : 7 场景 × " << opt.scenario_scale << "\n\n";

    AcceptanceRunner runner(opt);
    const AcceptanceReport rep = runner.run();

    for (const auto& d : rep.dims) {
        std::cout << "--------------------------------------------------------\n";
        std::cout << d.code << " " << d.name << "   "
                  << d.passed() << "/" << d.total() << "  "
                  << (d.pass() ? "PASS" : "FAIL") << "\n";
        std::cout << "  验收对象: " << d.scope << "\n";
        if (!quiet) {
            std::cout << "--------------------------------------------------------\n";
            for (const auto& c : d.items) {
                std::cout << "  [" << (c.pass ? "PASS" : "FAIL") << "] "
                          << c.id << "  " << c.name << "\n";
                std::cout << "        判据: " << c.criterion << "\n";
                std::cout << "        实测: " << c.evidence << "\n";
            }
        } else {
            for (const auto& c : d.items) {
                if (!c.pass) {
                    std::cout << "  [FAIL] " << c.id << "  " << c.name
                              << "  —  实测: " << c.evidence << "\n";
                }
            }
        }
    }

    const auto& raw = runner.raw();
    std::cout << "--------------------------------------------------------\n";
    std::cout << "关键测量（不参与判据，供复核）\n";
    std::cout << "  单拍耗时均值/峰值/占用率 : " << raw.perf_mean_us << " / "
              << raw.perf_max_us << " us / " << raw.perf_duty_pct << " %\n";
    std::cout << "  24h 墙钟 / 加速比        : " << raw.soak_wall_s << " s / "
              << raw.soak_speedup << "×\n";
    std::cout << "  24h 经济净收益           : " << raw.econ_saving_cny
              << " 元（等效循环 " << raw.econ_cycles << " 次）\n";
    std::cout << "  跨进程 stale / 越区间    : " << raw.rtdb_stale << " / "
              << raw.rtdb_out_ival << "\n";
    std::cout << "  SOE 事件 / 丢弃          : " << raw.soe_events << " / "
              << raw.soe_dropped << "\n";
    std::cout << "--------------------------------------------------------\n";

    std::cout << "\n结论: " << (rep.pass() ? "通过" : "不通过")
              << "  ——  " << rep.passed() << " / " << rep.total() << " 项通过，"
              << rep.failed_dimensions() << " 个维度未全通过\n";

    if (do_write) {
        const std::string dir = opt.root + "/12/docs";
        std::string err;
        if (write_reports(dir, rep, &err)) {
            std::cout << "报告已写入: " << dir
                      << "/ACCEPTANCE-REPORT.{md,json,html}\n";
        } else {
            std::cerr << "报告写盘失败: " << err << "\n";
        }
    }

    return rep.pass() ? 0 : 1;
}
