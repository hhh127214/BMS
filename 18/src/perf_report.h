// =====================================================================
// 18/perf_report.h — 基准报告渲染（Markdown，落盘到 18/build/）
//
// 报告必须包含（缺一不可，否则性能数字不可复核）：
//   · 日期 / 机器 / 编译器 / 构建时间
//   · **背景噪声说明**（空转基线 + 系统 CPU 忙率 + 时钟粒度）
//   · 每类基准的统计量（p50/p95/p99/max/mean/stddev + 样本数 + 丢弃数）
//   · SLA 与实测对照（通过/余量 + **数字来源** + 反向验证方式）
//   · **本次未覆盖的性能风险**（诚实列出）
//
// 参照 10/src/sim_report.h 的"手写 HTML + 内联 SVG"是可选项；
// 本模块优先把**数字与判据**做扎实，图表从简（只用文本条形）。
// =====================================================================

#pragma once

#include "perf_clock.h"
#include "perf_stats.h"
#include "sla.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace ems {
namespace perf {

struct ReportHeader {
    std::string  title;
    MachineInfo  machine;
    ClockProbe   clock;
    double       system_busy_pct = -1.0;   // 空载窗口实测
    NoiseBaseline noise;
    int          repeats        = 1;       // 重复次数（取最差口径）
    std::string  workload_note;            // 负载条件总说明
};

inline std::string render_header_md(const ReportHeader& h) {
    std::string s;
    s += "# " + h.title + "\n\n";
    s += "> 本报告由 `18/scripts/build_test.bat` 自动生成，全部数字可用该脚本复现。\n";
    s += "> 声明口径：本项目尚未全部完成，本报告是**阶段成果**。\n\n";

    s += "## 0. 环境与背景噪声\n\n";
    s += "| 项 | 值 |\n|---|---|\n";
    s += "| 生成时间 | `" + h.machine.stamp + "` |\n";
    s += "| 主机 | `" + h.machine.hostname + "` |\n";
    s += "| CPU 标识 | `" + h.machine.cpu_identifier + "` |\n";
    s += "| 逻辑核数 | " + h.machine.cores + " |\n";
    s += "| OS / 架构 | `" + h.machine.os + "` / `" + h.machine.arch + "` |\n";
    s += "| 编译器 | `" + h.machine.compiler + "` |\n";
    s += "| 可执行文件构建时间 | `" + h.machine.build_date + "` |\n";
    s += "| 计时后端 | `" + std::string(h.clock.backend) + "` |\n";
    s += "| 时钟声称分辨率 | " + fmt_double(h.clock.nominal_resolution_ns, 3) + " ns |\n";
    s += "| **时钟实测粒度（最小正增量）** | **" + fmt_double(h.clock.observed_granularity_ns, 3) + " ns** |\n";
    s += "| 时钟 is_steady | " + std::string(h.clock.is_steady ? "true" : "false") + " |\n";
    if (h.system_busy_pct >= 0.0)
        s += "| **测量前系统 CPU 忙率（空载窗口）** | **" + fmt_double(h.system_busy_pct, 2) + " %** |\n";
    else
        s += "| 测量前系统 CPU 忙率 | (取不到) |\n";
    s += "| 计时器噪声地板 p50 / p99 / max | " +
         fmt_double(h.noise.p50_us, 3) + " / " + fmt_double(h.noise.p99_us, 3) + " / " +
         fmt_double(h.noise.max_us, 3) + " µs |\n";
    s += "| 计时器噪声 CV | " + fmt_double(h.noise.cv_pct, 1) + " % |\n";
    s += "| 重复次数口径 | 每类基准独立重复，SLA 取最差（repeats=" +
         std::to_string(h.repeats) + "） |\n";
    s += "\n";
    s += "**背景噪声说明**：计时器噪声地板（读一对时钟的耗时）p99 = " +
         fmt_double(h.noise.p99_us, 3) +
         " µs，即**测量装置本身**的抖动不超过该量级；任何比它更小的差异都不可信。"
         "时钟的**真实粒度**是 " + fmt_double(h.clock.observed_granularity_ns, 1) +
         " ns（不是它声明的 " + fmt_double(h.clock.nominal_resolution_ns, 1) +
         " ns），故本次所有 µs 级数字的量化误差上限为 " +
         fmt_double(h.clock.observed_granularity_ns / 1000.0, 4) + " µs。\n";
    if (!h.workload_note.empty()) s += "\n" + h.workload_note + "\n";
    s += "\n";
    return s;
}

inline std::string fmt_u(double v, int prec) { return fmt_double(v, prec); }

// 一行统计量（µs 单位）
inline std::string summary_row(const std::string& name, const std::string& unit,
                               const Summary& s, const std::string& extra = "") {
    char b[512];
    std::snprintf(b, sizeof(b),
                  "| %s | %zu | %zu | %zu | %s | %s | %s | %s | %s | %s | %s | %s |",
                  name.c_str(), s.n_raw, s.n_warmup_dropped,
                  s.n_outliers_flagged,
                  fmt_double(s.p50, 4).c_str(), fmt_double(s.p90, 4).c_str(),
                  fmt_double(s.p95, 4).c_str(), fmt_double(s.p99, 4).c_str(),
                  fmt_double(s.p999, 4).c_str(), fmt_double(s.max, 4).c_str(),
                  fmt_double(s.mean, 4).c_str(), fmt_double(s.stddev, 4).c_str());
    std::string out = b;
    out += " " + unit;
    if (!extra.empty()) out += "  " + extra;
    return out;
}

inline std::string summary_table_header() {
    return "| 基准 | 样本 | 预热丢弃 | 离群标记 | p50 | p90 | p95 | p99 | p99.9 | max | mean | stddev | 单位 |\n"
           "|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|---|\n";
}

// SLA 对照表
inline std::string render_sla_md(const SlaTable& t) {
    std::string s;
    s += "## SLA 与实测对照\n\n";
    s += "| 编号 | 判据 | 形式 | 阈值 | 来源 | 实测 | 比值/余量 | 结论 |\n";
    s += "|---|---|---|---:|---|---:|---:|---|\n";
    for (const auto& o : t.outcomes) {
        char line[768];
        std::string meas = fmt_double(o.measured, 4);
        if (o.rule.dir == SlaDir::kRatioUpper || o.rule.dir == SlaDir::kRatioBound)
            meas = fmt_double(o.measured, 3) + " (= " +
                   fmt_double(o.measured * o.measured_extra, 3) + " / " +
                   fmt_double(o.measured_extra, 3) + ")";
        std::snprintf(line, sizeof(line), "| %s | %s | %s %.4g %s | %.4g %s | %s | %s | %s | %s |",
                      o.rule.id.c_str(), o.rule.name.c_str(),
                      sla_dir_name(o.rule.dir), o.rule.limit, o.rule.unit.c_str(),
                      o.rule.limit, o.rule.unit.c_str(),
                      sla_source_name(o.rule.source),
                      meas.c_str(),
                      (fmt_double(o.headroom_x(), 2) + "x").c_str(),
                      o.evaluated ? (o.pass ? "PASS" : "**FAIL**") : "NOT-COVERED");
        s += line;
        s += "\n";
    }
    s += "\n";

    s += "### 每条 SLA 的数字来源与反向验证\n\n";
    for (const auto& o : t.outcomes) {
        s += "**" + o.rule.id + " " + o.rule.name + "**（来源：" +
             sla_source_name(o.rule.source) + "）\n\n";
        s += "- 推导 / 裕度：" + o.rule.derivation + "\n";
        s += "- 成立条件：" + o.rule.condition + "\n";
        s += "- 反向验证：" + o.rule.reverse + "\n";
        s += "- 结论：" + std::string(o.pass ? "PASS" : "FAIL") +
             "，实测 " + fmt_double(o.measured, 4) + " vs 阈值 " +
             fmt_double(o.rule.limit, 4) + " " + o.rule.unit +
             "，余量 " + fmt_double(o.headroom_x(), 2) + "x\n\n";
    }
    return s;
}

inline bool write_text_file(const std::string& path, const std::string& body) {
    std::ofstream f(path.c_str(), std::ios::binary);
    if (!f.is_open()) return false;
    f.write(body.data(), (std::streamsize)body.size());
    return f.good();
}

// 组装完整报告
inline std::string build_report(const ReportHeader& h,
                                const std::vector<std::string>& blocks,
                                const SlaTable& sla,
                                const std::string& reverse_md,
                                const std::string& risks_md,
                                const std::string& caveats_md) {
    std::string s = render_header_md(h);
    s += "---\n\n";
    for (const auto& b : blocks) { s += b; s += "\n"; }
    s += "---\n\n";
    s += render_sla_md(sla);
    s += "---\n\n";
    s += reverse_md;
    s += "\n---\n\n";
    s += caveats_md;
    s += "\n---\n\n";
    s += risks_md;
    s += "\n---\n\n";
    s += "**汇总**：SLA 通过 " + std::to_string(sla.passed()) + " 条，失败 " +
         std::to_string(sla.failed()) + " 条，未覆盖 " +
         std::to_string(sla.not_evaluated()) + " 条。\n";
    return s;
}

} // namespace perf
} // namespace ems
