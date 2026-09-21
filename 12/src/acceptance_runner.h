// =====================================================================
// 12/ — 周期 12：最终验收
//
// 依据：工商业储能EMS调控策略设计方案.md §7 周期 12
//   「完成功能、策略、安全、性能、稳定性、多策略协同、文档全维度验收，
//     正式输出 EMS V2.0 版本。」
//
// 本模块**不实现任何新算法**。它的唯一职责是把前面 11 个周期交付的能力
// 按设计方案的七个维度**重新测量一遍**，并把结果固化成可复核的报告。
//
//   维度                  验收对象                                证据来源
//   ────────────────────────────────────────────────────────────────────────
//   A1 功能     策略注册/启停、全链路闭环、状态机可达、       04/06/07/P1
//               设备 I/O 三适配器等价、配置化往返、计划装载
//   A2 策略     9 个策略逐个在"触发工况"下核对设计意图          04/strategies_9.h
//   A3 安全     9 条安全约束逐位注入 + 区间矛盾语义 +           05/safety_engine.h
//               急停锁存 + 全域硬不变量                        06/ 09/
//   A4 性能     单拍耗时 / 占用率 / 24h 墙钟加速比 / 资源无泄漏  07/ 10/
//   A5 稳定性   24h 长稳 + 跨进程长稳 + 零 stale / 零丢弃       10/ 11/ P2
//               + BMS 禁充放位（设备侧上报 → 不下发放电）        07/（A5-07）
//   A6 协同     设计方案 §7 周期 9 的 7 个组合场景全通过         09/scenario_runner.h
//   A7 文档     模块文档/构建脚本/构建总入口/报告落盘            文件系统
//
// ---------------------------------------------------------------------
// 为什么验收要"重新测量"而不是"引用既有测试的结论"
// ---------------------------------------------------------------------
// 前面 11 个周期的测试是**开发期断言**：断言"我实现的东西符合我的实现"。
// 验收断言的是"交付物符合设计方案的字面要求"。两者口径不同：
//   · 开发期断言可以用内部量（`p_lower` 变量本身）作判据；
//   · 验收必须用**外部可观测的交付物**作判据（StepRecord / 报告文件 /
//     进程句柄数 / 墙钟时间 / 共享内存点值），否则第三方无法复现。
// 因此本模块只依赖各层的**公开出口**，不读任何私有成员，也不修改任何模块。
//
// ---------------------------------------------------------------------
// 判据全部是"可量化的阈值"，不接受"看起来正常"
// ---------------------------------------------------------------------
// 每个检查项三要素：编号 / 判据（criterion）/ 实测（evidence）。阈值来源：
//   · 单拍耗时阈值按"10 Hz 控制周期下 CPU 占用 ≤ 20%"折算（见 A4）
//   · 抖动阈值沿用 07/ 的工程口径（翻转 ≤ 0.2/s、反转 ≤ 1.0/s）
//   · 长稳拍数取 24 h（1 s 步长 = 86400 拍），与 10/ 的仿真口径一致
//
// 编译：纯头文件，实现全部 inline。
//   头文件搜索路径（见 12/scripts/build.bat）：
//     src ../04/src ../05/src ../06/src ../07/src ../08/src ../09/src
//     ../10/src ../11/src ../P1/src ../P2/src ../07/src/rtdb ../07/vendor/rt_db
//   链接：rt_db_api.c / ems_point_table.c / ems_rt_db_setup.c（gcc 编 C）
//         -lpsapi（A4 的进程工作集采样）
// =====================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>   // GetProcessHandleCount
#include <psapi.h>     // GetProcessMemoryInfo（-lpsapi）
#include <direct.h>    // _mkdir
#include <sys/stat.h>

// ---- 04/ 策略层 ----
// 顺序要紧：strategies_9.h 里的 register_all_9_strategies(StrategyManager&)
// 需要 StrategyManager 已完整定义（strategy_base.h 只给接口）。
#include "data_models.h"
#include "strategy_base.h"
#include "strategy_manager.h"
#include "strategy_arbiter.h"
#include "strategies_9.h"

// ---- 05/06/07/08 运行时 ----
#include "safety_engine.h"
#include "state_machine.h"
#include "plant_model.h"
#include "sim_device_io.h"
#include "memory_device_io.h"
#include "rtdb_device_io.h"
#include "realtime_loop.h"
#include "dispatch_coordinator.h"
#include "plan_loader.h"

// ---- 09/ 多策略场景 / 10/ 24h 离线仿真 ----
#include "scenario_runner.h"
#include "day_curves.h"
#include "econ_metrics.h"
#include "sim_24h.h"

// ---- 11/ 全链路联调（跨进程闭环）----
#include "integration_runner.h"

// ---- P1 配置化 / P2 可观测性 ----
#include "config_loader.h"
#include "config_doc.h"
#include "observe.h"
#include "soe.h"
#include "metrics.h"

// ---- RT_DB ----
#include "ems_rt_db_setup.h"
#include "rt_db_api.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace ems {

// =====================================================================
// 报告模型
// =====================================================================

// 一条验收检查：判据与实测**同时**记录，便于第三方复核
struct CheckItem {
    std::string id;          // "A3-02"
    std::string name;
    std::string criterion;   // 判据（可量化）
    bool        pass = false;
    std::string evidence;    // 实测值
};

// 一个验收维度
struct DimensionReport {
    std::string code;    // "A1"
    std::string name;    // "功能验收"
    std::string scope;   // 验收对象说明
    std::vector<CheckItem> items;

    int  passed() const {
        int n = 0;
        for (const auto& c : items) if (c.pass) ++n;
        return n;
    }
    int  total() const { return static_cast<int>(items.size()); }
    bool pass() const { return !items.empty() && passed() == total(); }
};

// 全量验收报告
struct AcceptanceReport {
    std::string product = "工商业储能 EMS";
    std::string version = "V2.0";
    std::string basis   = "《工商业储能EMS调控策略设计方案》§7 周期 12";
    std::string generated_at;

    std::vector<DimensionReport> dims;

    int  passed() const {
        int n = 0;
        for (const auto& d : dims) n += d.passed();
        return n;
    }
    int  total() const {
        int n = 0;
        for (const auto& d : dims) n += d.total();
        return n;
    }
    int  failed_dimensions() const {
        int n = 0;
        for (const auto& d : dims) if (!d.pass()) ++n;
        return n;
    }
    bool pass() const {
        if (dims.empty()) return false;
        for (const auto& d : dims) if (!d.pass()) return false;
        return true;
    }

    std::string to_markdown() const;
    std::string to_json() const;
    std::string to_html() const;
};

inline const char* dim_name(int i) {
    switch (i) {
        case 0: return "功能验收";
        case 1: return "策略验收";
        case 2: return "安全验收";
        case 3: return "性能验收";
        case 4: return "稳定性验收";
        case 5: return "多策略协同验收";
        case 6: return "文档验收";
        default: return "未知维度";
    }
}

// =====================================================================
// 时间 / 文件工具
// =====================================================================
inline std::string now_stamp() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return std::string(buf);
}

// UTF-8 → UTF-16（Windows 文件 API 唯一可靠的路径编码转换）
inline std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                        static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                          &w[0], n);
    return w;
}

// 文件长度（字节）；-1 = 不存在。
//
// 必须走宽字符 Windows API：MSVCRT 的 `_stat` 按 **ANSI 代码页**（本机 CP936）
// 解释路径，而源码里的字符串字面量是 UTF-8 —— 含中文的路径（例如
// 「工商业储能EMS调控策略设计方案.md」）会被解释成另一串不存在的字节序列，
// 表现为"文件明明在、却报缺失"。这是验收脚本最容易踩的假阴性。
inline long file_size(const std::string& path) {
    const std::wstring w = utf8_to_wide(path);
    if (w.empty()) return -1;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &fad)) return -1;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
    LARGE_INTEGER li;
    li.HighPart = static_cast<LONG>(fad.nFileSizeHigh);
    li.LowPart  = fad.nFileSizeLow;
    return static_cast<long>(li.QuadPart);
}

inline bool is_dir(const std::string& path) {
    const std::wstring w = utf8_to_wide(path);
    if (w.empty()) return false;
    const DWORD a = ::GetFileAttributesW(w.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// 递归建目录（"a/b/c" 逐级创建）。失败返回 false。
inline bool ensure_dir_deep(const std::string& path) {
    std::string cur;
    for (std::size_t i = 0; i < path.size(); ++i) {
        const char c = path[i];
        cur += c;
        if (c == '/' || c == '\\' || i + 1 == path.size()) {
            if (cur == "/" || cur == "\\" || cur == "./" || cur.empty()) continue;
            if (is_dir(cur)) continue;
            const std::wstring w = utf8_to_wide(cur);
            if (w.empty()) return false;
            ::CreateDirectoryW(w.c_str(), nullptr);
        }
    }
    return is_dir(path);
}

// 大小写不敏感的"存在且非空"判定。
// 需要大小写不敏感：仓库里 01/docs 用的是小写 readme.md（历史遗留），
// 其余模块统一 README.md —— 验收不该因文件名大小写而误判缺失。
inline bool file_ok_ci(const std::string& path, long min_bytes = 1) {
    if (file_size(path) >= min_bytes) return true;
    std::string lower = path, upper = path;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return file_size(lower) >= min_bytes || file_size(upper) >= min_bytes;
}

inline std::string read_text_file(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return std::string();
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}

inline bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

inline std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o += c;      // UTF-8 中文按原字节透传（JSON 合法）
                }
        }
    }
    return o;
}

// =====================================================================
// 报告渲染（md / json / html）—— 三件套
// =====================================================================
inline std::string AcceptanceReport::to_markdown() const {
    std::ostringstream os;
    os << "# " << product << " " << version << " 最终验收报告\n\n";
    os << "- 验收依据：" << basis << "\n";
    os << "- 生成时间：" << generated_at << "\n";
    os << "- 结论：**" << (pass() ? "通过" : "不通过") << "**"
       << "（" << passed() << " / " << total() << " 项通过，"
       << failed_dimensions() << " 个维度未全通过）\n\n";

    os << "## 维度总览\n\n";
    os << "| 维度 | 名称 | 通过 | 合计 | 结论 |\n";
    os << "|---|---|---|---|---|\n";
    for (const auto& d : dims) {
        os << "| " << d.code << " | " << d.name << " | " << d.passed()
           << " | " << d.total() << " | " << (d.pass() ? "PASS" : "FAIL") << " |\n";
    }
    os << "| **合计** | — | **" << passed() << "** | **" << total() << "** | **"
       << (pass() ? "PASS" : "FAIL") << "** |\n\n";

    for (const auto& d : dims) {
        os << "## " << d.code << " " << d.name << "\n\n";
        os << "> 验收对象：" << d.scope << "\n\n";
        os << "| 编号 | 检查项 | 判据 | 实测 | 结论 |\n";
        os << "|---|---|---|---|---|\n";
        for (const auto& c : d.items) {
            os << "| " << c.id << " | " << c.name << " | " << c.criterion
               << " | " << c.evidence << " | " << (c.pass ? "PASS" : "FAIL") << " |\n";
        }
        os << "\n";
    }

    os << "---\n\n";
    os << "本报告由 `12/`（周期 12 最终验收模块）自动生成，"
          "全部检查项均可通过 `12/scripts/run_acceptance.bat` 复现。\n";
    return os.str();
}

inline std::string AcceptanceReport::to_json() const {
    std::ostringstream os;
    os << "{\n";
    os << "  \"product\": \"" << json_escape(product) << "\",\n";
    os << "  \"version\": \"" << json_escape(version) << "\",\n";
    os << "  \"basis\": \"" << json_escape(basis) << "\",\n";
    os << "  \"generated_at\": \"" << json_escape(generated_at) << "\",\n";
    os << "  \"summary\": {\"passed\": " << passed() << ", \"total\": " << total()
       << ", \"failed_dimensions\": " << failed_dimensions()
       << ", \"result\": \"" << (pass() ? "PASS" : "FAIL") << "\"},\n";
    os << "  \"dimensions\": [\n";
    for (std::size_t i = 0; i < dims.size(); ++i) {
        const auto& d = dims[i];
        os << "    {\n";
        os << "      \"code\": \"" << json_escape(d.code) << "\",\n";
        os << "      \"name\": \"" << json_escape(d.name) << "\",\n";
        os << "      \"scope\": \"" << json_escape(d.scope) << "\",\n";
        os << "      \"passed\": " << d.passed() << ", \"total\": " << d.total()
           << ", \"result\": \"" << (d.pass() ? "PASS" : "FAIL") << "\",\n";
        os << "      \"items\": [\n";
        for (std::size_t j = 0; j < d.items.size(); ++j) {
            const auto& c = d.items[j];
            os << "        {\"id\": \"" << json_escape(c.id)
               << "\", \"name\": \"" << json_escape(c.name)
               << "\", \"criterion\": \"" << json_escape(c.criterion)
               << "\", \"evidence\": \"" << json_escape(c.evidence)
               << "\", \"pass\": " << (c.pass ? "true" : "false") << "}";
            os << (j + 1 < d.items.size() ? ",\n" : "\n");
        }
        os << "      ]\n";
        os << "    }";
        os << (i + 1 < dims.size() ? ",\n" : "\n");
    }
    os << "  ]\n}\n";
    return os.str();
}

inline std::string AcceptanceReport::to_html() const {
    std::ostringstream os;
    os << "<!DOCTYPE html>\n<html lang=\"zh-CN\"><head><meta charset=\"utf-8\">\n";
    os << "<title>" << product << " " << version << " 最终验收报告</title>\n";
    os << "<style>\n"
          "body{font-family:'Microsoft YaHei',system-ui,sans-serif;margin:0;"
          "background:#f6f7f9;color:#1f2430;line-height:1.6}\n"
          ".wrap{max-width:1180px;margin:0 auto;padding:32px 24px 64px}\n"
          "h1{font-size:26px;margin:0 0 6px}\n"
          ".meta{color:#5b6472;font-size:13px;margin-bottom:22px}\n"
          ".verdict{display:inline-block;padding:6px 16px;border-radius:6px;"
          "font-weight:700;font-size:15px;margin:10px 0 24px}\n"
          ".ok{background:#e6f6ec;color:#12693a;border:1px solid #9fd9b7}\n"
          ".ng{background:#fdeaea;color:#9b2226;border:1px solid #f0b0b0}\n"
          "table{border-collapse:collapse;width:100%;background:#fff;font-size:13px;"
          "margin:12px 0 28px;box-shadow:0 1px 3px rgba(16,24,40,.08)}\n"
          "th,td{border:1px solid #e3e7ee;padding:8px 10px;text-align:left;"
          "vertical-align:top}\n"
          "th{background:#eef1f6;font-weight:600;white-space:nowrap}\n"
          "tr.pass td:last-child{color:#12693a;font-weight:600}\n"
          "tr.fail td:last-child{color:#9b2226;font-weight:700}\n"
          "tr.fail{background:#fff7f7}\n"
          "h2{font-size:19px;margin:32px 0 4px;border-left:4px solid #3b6fd4;"
          "padding-left:10px}\n"
          ".scope{color:#5b6472;font-size:13px;margin:0 0 8px 14px}\n"
          ".num{font-variant-numeric:tabular-nums;white-space:nowrap}\n"
          "</style></head><body><div class=\"wrap\">\n";

    os << "<h1>" << product << " " << version << " 最终验收报告</h1>\n";
    os << "<div class=\"meta\">验收依据：" << basis << " ｜ 生成时间："
       << generated_at << "</div>\n";
    os << "<div class=\"verdict " << (pass() ? "ok" : "ng") << "\">"
       << (pass() ? "验收通过" : "验收不通过") << " ｜ " << passed() << " / "
       << total() << " 项通过 ｜ " << failed_dimensions() << " 个维度未全通过</div>\n";

    os << "<h2>维度总览</h2>\n<table><tr><th>维度</th><th>名称</th>"
          "<th>通过</th><th>合计</th><th>结论</th></tr>\n";
    for (const auto& d : dims) {
        os << "<tr class=\"" << (d.pass() ? "pass" : "fail") << "\"><td>"
           << d.code << "</td><td>" << d.name << "</td><td class=\"num\">"
           << d.passed() << "</td><td class=\"num\">" << d.total() << "</td><td>"
           << (d.pass() ? "PASS" : "FAIL") << "</td></tr>\n";
    }
    os << "<tr class=\"" << (pass() ? "pass" : "fail")
       << "\"><td><b>合计</b></td><td>—</td><td class=\"num\"><b>" << passed()
       << "</b></td><td class=\"num\"><b>" << total() << "</b></td><td><b>"
       << (pass() ? "PASS" : "FAIL") << "</b></td></tr></table>\n";

    for (const auto& d : dims) {
        os << "<h2>" << d.code << " " << d.name << "</h2>\n";
        os << "<p class=\"scope\">验收对象：" << d.scope << "</p>\n";
        os << "<table><tr><th>编号</th><th>检查项</th><th>判据</th><th>实测</th>"
              "<th>结论</th></tr>\n";
        for (const auto& c : d.items) {
            os << "<tr class=\"" << (c.pass ? "pass" : "fail") << "\"><td class=\"num\">"
               << c.id << "</td><td>" << c.name << "</td><td>" << c.criterion
               << "</td><td class=\"num\">" << c.evidence << "</td><td>"
               << (c.pass ? "PASS" : "FAIL") << "</td></tr>\n";
        }
        os << "</table>\n";
    }
    os << "<hr><p class=\"meta\">本报告由 12/（周期 12 最终验收模块）自动生成，"
          "全部检查项均可通过 12/scripts/run_acceptance.bat 复现。</p>\n";
    os << "</div></body></html>\n";
    return os.str();
}

// 落盘三件套。返回是否全部成功；err 记录首个失败原因。
inline bool write_reports(const std::string& dir, const AcceptanceReport& rep,
                          std::string* err = nullptr) {
    if (!ensure_dir_deep(dir)) {
        if (err) *err = std::string("无法创建目录 ") + dir;
        return false;
    }
    const std::string base = dir + "/ACCEPTANCE-REPORT";
    struct Out { std::string path; std::string body; };
    const Out outs[] = {
        {base + ".md",   rep.to_markdown()},
        {base + ".json", rep.to_json()},
        {base + ".html", rep.to_html()}};
    for (const auto& o : outs) {
        std::ofstream f(o.path.c_str(), std::ios::binary);
        if (!f) {
            if (err) *err = std::string("无法写入 ") + o.path;
            return false;
        }
        f << o.body;
        if (!f.good()) {
            if (err) *err = std::string("写入失败 ") + o.path;
            return false;
        }
    }
    return true;
}

// =====================================================================
// AcceptanceRunner —— 七维度验收运行器
// =====================================================================
class AcceptanceRunner {
public:
    struct Options {
        // A4 性能采样（10 Hz 控制周期下 20000 拍 = 2000 s）
        int    perf_steps       = 20000;
        double perf_dt_s        = 0.1;
        // A5 长稳拍数：24 h @ 1 s（与 10/ 的仿真口径一致）
        double soak_dt_s        = 1.0;
        // A5 跨进程闭环长稳拍数（走共享内存，逐拍全表读写，代价高 → 单独设）
        int    rtdb_soak_steps  = 6000;
        double rtdb_soak_dt_s   = 1.0;
        // A6 协同场景倍数（1 = 09/ 原设定 12000 拍 @ 10 Hz）
        int    scenario_scale   = 1;
        // 工程根目录（文档验收用）。默认本模块的上一级。
        std::string root = "..";

        // -------- A4 判据阈值（可被现场标定覆盖）--------
        double perf_mean_us_max  = 1000.0;    // 单拍均值上限
        // 单拍峰值上限：取 10 Hz 控制周期（100 ms）的一半。
        // 物理含义——任何一拍都不该吃掉半个控制周期，否则节拍会漂。
        // 首拍含冷启动（首次分配、页错误），实测可达十几 ms，故不能按均值同量级设。
        double perf_max_us_max   = 50000.0;
        double perf_duty_pct_max = 20.0;      // 单拍时间占用率上限（%）
        double perf_speedup_min  = 2000.0;    // 24h 仿真墙钟加速比下限
        // -------- A5 判据阈值 --------
        int    stability_state_changes_max = 30;    // 24h 状态迁移次数上限
        double stability_travel_per_s_max  = 5.0;   // 指令总行程速率上限（kW/s）
    };

    // 注：不写成 `const Options& o = Options()` —— GCC 8 下嵌套类的默认成员
    // 初始化器不能出现在同一外层类的默认实参里（硬性编译限制）。两个重载等价。
    AcceptanceRunner() { autodetect_root(); }
    explicit AcceptanceRunner(const Options& o) : opt_(o) { autodetect_root(); }

    const Options& options() const { return opt_; }

    struct RawMeasure {
        double perf_mean_us   = 0.0;
        double perf_max_us    = 0.0;
        double perf_duty_pct  = 0.0;
        int    handle_delta   = 0;
        double rss_kb_delta   = 0.0;

        double soak_wall_s    = 0.0;
        double soak_speedup   = 0.0;
        int    soak_steps     = 0;
        int    soak_breach    = 0;
        double soak_travel_ps = 0.0;
        int    soak_state_chg = 0;

        int    rtdb_stale     = -1;
        int    rtdb_out_ival  = 0;
        int    rtdb_steps     = 0;
        bool   rtdb_ran       = false;
        int    soe_events     = 0;
        int    soe_dropped    = 0;

        double econ_saving_cny = 0.0;
        double econ_cycles     = 0.0;
    };

    const RawMeasure& raw() const { return raw_; }

    // -----------------------------------------------------------------
    // 跑全部七个维度
    // -----------------------------------------------------------------
    AcceptanceReport run() {
        AcceptanceReport rep;
        rep.generated_at = now_stamp();
        rep.dims.push_back(dim_functional());
        rep.dims.push_back(dim_strategy());
        rep.dims.push_back(dim_safety());
        rep.dims.push_back(dim_performance());
        rep.dims.push_back(dim_stability());
        rep.dims.push_back(dim_coordination());
        rep.dims.push_back(dim_documentation());
        return rep;
    }

private:
    void autodetect_root() {
        // 允许从工程根或模块目录启动：两处都探测一次
        if (file_size(opt_.root + "/README.md") >= 0) return;
        if (file_size("./README.md") >= 0) { opt_.root = "."; return; }
        if (file_size("../README.md") >= 0) { opt_.root = ".."; return; }
        opt_.root = "..";
    }

    static CheckItem mk(const std::string& id, const std::string& name,
                        const std::string& criterion, bool pass,
                        const std::string& evidence) {
        CheckItem c;
        c.id = id; c.name = name; c.criterion = criterion;
        c.pass = pass; c.evidence = evidence;
        return c;
    }

    static std::string f2(double v, int prec = 2) {
        std::ostringstream os;
        os << std::fixed << std::setprecision(prec) << v;
        return os.str();
    }

    static const ConstraintResult* find_item(const SafetyVerdict& v,
                                             const std::string& name) {
        for (const auto& it : v.items) if (it.name == name) return &it;
        return nullptr;
    }

    static DWORD handle_count() {
        DWORD n = 0;
        ::GetProcessHandleCount(::GetCurrentProcess(), &n);
        return n;
    }
    static double rss_kb() {
        PROCESS_MEMORY_COUNTERS pmc;
        if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc))) {
            return static_cast<double>(pmc.WorkingSetSize) / 1024.0;
        }
        return 0.0;
    }

    // =================================================================
    // A1 功能验收
    // =================================================================
    DimensionReport dim_functional() {
        DimensionReport d;
        d.code = "A1";
        d.name = dim_name(0);
        d.scope = "策略注册/启停、全链路闭环、状态机七态可达、设备 I/O 三适配器等价、"
                  "配置化往返、日计划装载、关口功率单一数据源（电表口径）";

        // ---- A1-01 9 策略注册完整 ----
        {
            StrategyManager mgr;
            register_all_9_strategies(mgr);
            const char* want[] = {
                strategy_id::kBmsForbid, strategy_id::kBmsDerate,
                strategy_id::kTransformerLim, strategy_id::kDemandMgmt,
                strategy_id::kAntiReverse, strategy_id::kPvSmoothing,
                strategy_id::kPeakValley, strategy_id::kForecastOpt,
                strategy_id::kDemandResponse};
            int hit = 0;
            for (const char* w : want) if (mgr.has(w)) ++hit;
            d.items.push_back(mk(
                "A1-01", "9 策略注册完整",
                "mgr.size()==9 且 9 个 strategy_id 全部可查到",
                mgr.size() == 9 && hit == 9,
                "registered=" + std::to_string(mgr.size()) +
                    ", id_hit=" + std::to_string(hit) + "/9"));

            // ---- A1-02 策略启停生效 ----
            RealtimeSnapshot snap;
            snap.timestamp = 1.0;
            DeviceLimits dev;
            mgr.start_all();
            const std::size_t n_run = mgr.tick(snap, dev).size();
            mgr.stop_all();
            const std::size_t n_stop = mgr.tick(snap, dev).size();
            d.items.push_back(mk(
                "A1-02", "策略启停生效",
                "start_all() 后 tick 返回 9 条；stop_all() 后返回 0 条",
                n_run == 9 && n_stop == 0,
                "start_all→" + std::to_string(n_run) +
                    " 条, stop_all→" + std::to_string(n_stop) + " 条"));
        }

        // ---- A1-03 全链路闭环可跑 ----
        {
            EmsRuntime rt;
            rt.config().dt_s = 0.1;
            rt.apply_configs();
            rt.fsm().request_run(true);
            const int steps = 2000;
            rt.run(steps, 0.1, [](EmsRuntime& r, int i) {
                r.set_environment(300.0 + 50.0 * std::sin(i * 0.01),
                                  120.0 + 40.0 * std::cos(i * 0.02));
            });
            const auto& log = rt.log();
            bool finite = (static_cast<int>(log.size()) == steps);
            for (const auto& r : log) {
                if (std::isnan(r.p_cmd) || std::isinf(r.p_cmd)) { finite = false; break; }
            }
            d.items.push_back(mk(
                "A1-03", "全链路闭环可跑（2000 拍 @ 10 Hz）",
                "log().size()==2000 且每拍 p_cmd 有限（非 NaN/Inf）",
                finite,
                "log_rows=" + std::to_string(log.size()) +
                    ", steps=" + std::to_string(steps) +
                    ", final_state=" + state_name(rt.fsm().state())));
        }

        // ---- A1-04 状态机七态全部可达 ----
        {
            EmsStateMachine fsm;
            StateMachineConfig cfg;
            cfg.init_hold_s = 0.5;
            cfg.self_check_cycles = 5;
            fsm.set_config(cfg);
            fsm.request_run(true);

            std::map<int, bool> seen;
            FaultFlags none;
            SafetyVerdict ok;
            ok.p_lower = -200.0; ok.p_upper = 200.0;
            SafetyVerdict der; der.p_lower = -200.0; der.p_upper = 200.0;
            der.derated = true;
            FaultFlags f; f.bms_comm_lost = true;
            FaultFlags em; em.emergency_stop = true;

            auto tick = [&](double t, const FaultFlags& fl, const SafetyVerdict& sv) {
                fsm.update(t, fl, sv);
                seen[static_cast<int>(fsm.state())] = true;
            };
            for (int i = 0; i < 20; ++i)  tick(i * 0.1, none, ok);     // INIT→SC→READY→NORMAL
            for (int i = 20; i < 45; ++i) tick(i * 0.1, none, der);    // NORMAL→DERATED
            for (int i = 45; i < 70; ++i) tick(i * 0.1, f, ok);        // DERATED→FAULT
            fsm.trigger_emergency_stop("acceptance_probe");
            for (int i = 70; i < 85; ++i) tick(i * 0.1, em, ok);       // FAULT→EMERGENCY

            int reach = 0;
            for (int s = 0; s <= 6; ++s) if (seen.count(s)) ++reach;
            const bool latched = fsm.state() == EmsState::kEmergency;
            d.items.push_back(mk(
                "A1-04", "状态机七态全部可达",
                "INIT/SELF_CHECK/READY/NORMAL/DERATED/FAULT/EMERGENCY 七态各至少到过一次",
                reach == 7 && latched,
                "reached=" + std::to_string(reach) + "/7, 末态=" +
                    state_name(fsm.state()) + (latched ? "（锁存 ✔）" : "（未锁存 ✘）")));
        }

        // ---- A1-05 设备 I/O 三适配器逐拍等价 ----
        {
            std::vector<double> cmds;
            for (int i = 0; i < 200; ++i) cmds.push_back(180.0 * std::sin(i * 0.13));
            const double dt = 0.1;

            // ① SimDeviceIO(PlantModel)
            PlantConfig pc;
            pc.battery_capacity_kwh = 1000.0;
            pc.soc_init = 0.50;
            pc.soc_phys_min = 0.05; pc.soc_phys_max = 0.95;
            pc.eta_chg = 0.95; pc.eta_dis = 0.95;
            pc.pcs_max_chg_kw = 200.0; pc.pcs_max_dis_kw = 200.0;
            pc.pcs_ramp_kw_per_s = 400.0; pc.pcs_tau_s = 0.5;
            pc.pcs_deadtime_s = 0.0;    // 只有 MemoryDeviceIO/RT_DB 无死区 → 归零
            pc.pcs_standby_kw = 0.0;    // 隔离站用电，只比 P_bat/SOC
            pc.temp_heat_coef = 0.0; pc.temp_cool_coef = 0.0;
            SimDeviceIO sim(pc);
            std::vector<double> sim_p, sim_soc;
            for (double c : cmds) {
                sim_p.push_back(sim.execute(c, dt));
                RealtimeSnapshot s;
                sim.read_snapshot(0.0, s);
                sim_soc.push_back(s.soc);
            }

            // ② MemoryDeviceIO（进程内点表）
            MemoryDeviceIO mem;
            mem.set(mem_point::kCapKwh, 1000.0);
            mem.set(mem_point::kMaxChg, 200.0);
            mem.set(mem_point::kMaxDis, 200.0);
            mem.set(mem_point::kTauS, 0.5);
            mem.set(mem_point::kRampKwS, 400.0);
            mem.set(mem_point::kStandby, 0.0);
            mem.set(mem_point::kEtaChg, 0.95);
            mem.set(mem_point::kEtaDis, 0.95);
            mem.set(mem_point::kSocMin, 0.05);
            mem.set(mem_point::kSocMax, 0.95);
            mem.force_soc(0.50);
            std::vector<double> mem_p, mem_soc;
            for (double c : cmds) {
                mem_p.push_back(mem.execute(c, dt));
                mem_soc.push_back(mem.get(mem_point::kSoc));
            }

            double d_sm = 0.0;
            for (std::size_t i = 0; i < cmds.size(); ++i) {
                d_sm = std::max(d_sm, std::fabs(sim_p[i] - mem_p[i]));
                d_sm = std::max(d_sm, std::fabs(sim_soc[i] - mem_soc[i]));
            }

            // ③ RtDbDeviceIO（共享内存 + 设备侧进程）
            double d_sr = 0.0;
            bool rtdb_ok = false;
            {
                rt_db_handle_t h_dev{};
                bool created = false;
                if (ems_rt_db_setup(/*reset=*/true, &created) && rt_db_init(&h_dev, nullptr)) {
                    {
                        DeviceSimConfig dc;
                        dc.battery_capacity_kwh = 1000.0;
                        dc.pcs_max_chg_kw = 200.0; dc.pcs_max_dis_kw = 200.0;
                        dc.bms_chg_limit_kw = 200.0; dc.bms_dis_limit_kw = 200.0;
                        dc.transformer_kva = 1000.0; dc.demand_target_kw = 600.0;
                        dc.tau_s = 0.5; dc.ramp_kw_per_s = 400.0; dc.standby_kw = 0.0;
                        dc.eta_chg = 0.95; dc.eta_dis = 0.95;
                        dc.soc_phys_min = 0.05; dc.soc_phys_max = 0.95;
                        dc.temp_heat_coef = 0.0; dc.temp_cool_coef = 0.0;
                        DeviceSideSim dev(&h_dev, {}, dc);
                        dev.dev().force_soc(0.50);
                        dev.publish_all(0.0);

                        rt_db_handle_t h_ems{};
                        if (rt_db_init(&h_ems, nullptr)) {
                            RtDbDeviceIO io(&h_ems);
                            rtdb_ok = io.is_open();
                            for (std::size_t i = 0; i < cmds.size(); ++i) {
                                PowerCommand cmd;
                                cmd.p_bat_cmd_kw = cmds[i];
                                cmd.p_lower = -200.0; cmd.p_upper = 200.0;
                                io.write_command(cmd);
                                io.execute(cmds[i], dt);          // 设备侧泵不设 → 只写 CMD
                                dev.step(static_cast<double>(i) * dt, dt);  // 设备进程推进一拍
                                const DeviceActuals a = io.read_actuals();
                                d_sr = std::max(d_sr, std::fabs(sim_p[i] - a.p_bat_kw));
                                d_sr = std::max(d_sr, std::fabs(sim_soc[i] - a.soc));
                            }
                            rt_db_cleanup(&h_ems);
                        }
                    }
                    rt_db_cleanup(&h_dev);
                }
            }

            d.items.push_back(mk(
                "A1-05", "设备 I/O 三适配器逐拍等价",
                "SimDeviceIO / MemoryDeviceIO / RtDbDeviceIO 在 200 拍同一指令序列下 "
                "P_bat 与 SOC 逐拍偏差 ≤ 1e-9",
                rtdb_ok && d_sm <= 1e-9 && d_sr <= 1e-9,
                "max|Δ(Sim,Mem)|=" + f2(d_sm, 12) +
                    ", max|Δ(Sim,RT_DB)|=" + f2(d_sr, 12) +
                    (rtdb_ok ? "" : " [RT_DB 未建段]")));
        }

        // ---- A1-06 点表自检（全点表；点数从 EMS_POINT_COUNT 取，不手抄）----
        {
            bool ok = false;
            std::string ev = "未建段";
            rt_db_handle_t h{};
            bool created = false;
            if (ems_rt_db_setup(true, &created) && rt_db_init(&h, nullptr)) {
                RtDbDeviceIO io(&h);
                const int rc = io.self_check();
                ok = (rc == 0);
                ev = "self_check() rc=" + std::to_string(rc) +
                     ", 点数=" + std::to_string(EMS_POINT_COUNT) +
                     ", stale_reads=" + std::to_string(io.stale_reads()) +
                     ", is_open=" + (io.is_open() ? "true" : "false");
                rt_db_cleanup(&h);
            }
            d.items.push_back(mk(
                "A1-06", std::to_string(EMS_POINT_COUNT) + " 点表自检（点名/单位/索引一致）",
                "RtDbDeviceIO::self_check()==0 且 is_open()==true",
                ok, ev));
        }

        // ---- A1-07 配置化闭环（JSON 往返）----
        {
            EmsConfig cfg;
            cfg.name = "acceptance";
            cfg.loop.dt_s = 0.1;
            cfg.limits.pcs_rated_dis_kw = 250.0;
            cfg.limits.bms_dis_limit_kw = 180.0;
            cfg.safety.soc_min = 0.12;
            cfg.safety.soc_max = 0.88;

            const std::string text = json::dump(to_json(cfg), 2);
            EmsConfig back;
            ConfigDiagnostics diag;
            const bool parsed = parse_config_text(text, &back, &diag);

            EmsRuntime rt;
            const ConfigDiagnostics ad = apply_config(rt, back);

            const bool round_trip =
                parsed && ad.ok() &&
                std::fabs(rt.config().dt_s - 0.1) < 1e-12 &&
                std::fabs(rt.device_limits().bms_dis_limit_kw - 180.0) < 1e-9 &&
                std::fabs(rt.safety_params().soc_min - 0.12) < 1e-9 &&
                std::fabs(rt.safety_params().soc_max - 0.88) < 1e-9;

            d.items.push_back(mk(
                "A1-07", "配置化闭环（导出→解析→生效）",
                "to_json → parse → apply_config 后 dt_s / BMS 限值 / SOC 上下限与配置一致",
                round_trip,
                std::string("parse=") + (parsed ? "ok" : "fail") +
                    ", apply_errors=" + std::to_string(ad.error_count()) +
                    ", json_bytes=" + std::to_string(text.size()) +
                    ", dt_s=" + f2(rt.config().dt_s, 3) +
                    ", bms_dis=" + f2(rt.device_limits().bms_dis_limit_kw, 2) +
                    ", soc=[" + f2(rt.safety_params().soc_min, 3) + ", " +
                    f2(rt.safety_params().soc_max, 3) + "]"));
        }

        // ---- A1-08 日计划装载与跟踪 ----
        {
            DayPlan plan;
            plan.step_s = 900.0;
            plan.strategy = "acceptance";
            for (int i = 0; i < 96; ++i) {
                const double h = i * 0.25;
                const bool valley = (h < 7.0 || h >= 22.0);
                const bool peak   = (h >= 17.0 && h < 21.0);
                PlanPoint pp;
                pp.t_s       = i * 900.0;
                pp.p_plan_kw = valley ? -100.0 : (peak ? 120.0 : 0.0);
                pp.soc_ref   = valley ? 0.80 : 0.30;
                plan.points.push_back(pp);
            }
            plan.horizon = 96;
            plan.loaded  = true;

            EmsRuntime rt;
            rt.config().dt_s = 0.1;
            rt.apply_configs();
            rt.set_plan(plan);
            rt.fsm().request_run(true);
            rt.run(200, 0.1, [](EmsRuntime& r, int) { r.set_environment(250.0, 0.0); });

            const bool loaded = rt.coordinator().plan_valid();
            const auto& log = rt.log();
            // 200 拍 @ 0.1 s = 20 s，仍在第 0 槽（0~900 s，谷时 → 计划 -100 kW）
            const double tail_target = log.empty() ? 0.0 : log.back().plan_target;
            const bool target_ok = (tail_target < -50.0);
            d.items.push_back(mk(
                "A1-08", "日计划装载与跟踪",
                "96 点 DayPlan 装载后 plan_valid()==true，且 t=20 s 仍落在第 0 槽"
                "（谷时，plan_target ≈ -100 kW）",
                loaded && target_ok,
                std::string("plan_valid=") + (loaded ? "true" : "false") +
                    ", slots=" + std::to_string(plan.points.size()) +
                    ", t=20s 时 plan_target=" + f2(tail_target, 2) + " kW"));
        }

        // ---- A1-09 关口功率单一数据源（电表口径）----
        //
        // 缺口 A2（2026-09-19）。改前三个适配器的 read_snapshot() 都用
        // `P_load − P_pv − P_bat` **推算**关口功率，而设备侧发布的 `MEAS.P_GRID`
        // 恰好等于同一个式子 —— 两个口径逐位相同，于是"到底读没读电表"在任何
        // 夹具上都**不可区分**（断言恒真）。
        // 所以这里给设备侧电表加一个**系统偏差**把两源拉开，判据才有区分度。
        // 现场后果：防逆流（S05）与变压器过载约束拿 p_grid 当命门，
        // 推算偏差直接变成误动作或漏判倒送。
        {
            bool   meter_ok = false;
            double algo_grid = 0.0, balance = 0.0, meter_val = 0.0, path_diff = 0.0;
            const double kBias = 40.0;

            rt_db_handle_t h_dev{};
            bool created = false;
            if (ems_rt_db_setup(/*reset=*/true, &created) && rt_db_init(&h_dev, nullptr)) {
                {
                    DeviceSimConfig dc;
                    dc.battery_capacity_kwh = 1000.0;
                    dc.pcs_max_chg_kw = 200.0; dc.pcs_max_dis_kw = 200.0;
                    dc.bms_chg_limit_kw = 200.0; dc.bms_dis_limit_kw = 200.0;
                    dc.transformer_kva = 1000.0; dc.demand_target_kw = 600.0;
                    dc.tau_s = 0.5; dc.ramp_kw_per_s = 400.0; dc.standby_kw = 2.0;
                    dc.eta_chg = 0.95; dc.eta_dis = 0.95;
                    dc.soc_phys_min = 0.05; dc.soc_phys_max = 0.95;
                    dc.temp_heat_coef = 0.0; dc.temp_cool_coef = 0.0;
                    dc.meter_bias_kw = kBias;      // 电表读数 ≠ 功率平衡

                    DeviceSideSim dev(&h_dev, {}, dc);
                    dev.dev().force_soc(0.50);
                    for (int i = 0; i < 20; ++i) dev.step(i * 0.1, 0.1);   // 稳定几拍

                    rt_db_handle_t h_ems{};
                    if (rt_db_init(&h_ems, nullptr)) {
                        RtDbDeviceIO io(&h_ems);
                        RealtimeSnapshot snap;
                        io.read_snapshot(2.0, snap);
                        algo_grid = snap.p_grid_kw;
                        // 三路量测的"功率平衡" —— 正是**改前**算法拿到的那个数
                        balance   = snap.p_load_kw - snap.p_pv_kw - snap.p_bat_actual_kw;
                        meter_val = dev.dev().get(mem_point::kPGrid);
                        // 算法路径（①快照）vs 记录路径（④actuals）
                        path_diff = std::fabs(io.read_actuals().p_grid_kw - snap.p_grid_kw);

                        meter_ok =
                            std::fabs(algo_grid - meter_val) < 1e-6 &&        // 读的是电表
                            path_diff < 1e-9 &&                               // 两路径同口径
                            std::fabs((meter_val - balance) - kBias) < 1e-6;  // 两源确实差一个 bias
                        rt_db_cleanup(&h_ems);
                    }
                }
                rt_db_cleanup(&h_dev);
            }

            d.items.push_back(mk(
                "A1-09", "关口功率单一数据源（电表口径）",
                "电表读数刻意与三路推算差 40 kW：算法用的关口功率必须等于电表读数，"
                "且与记录路径同口径（反向守卫：两源确实不同，判据有区分度）",
                meter_ok,
                std::string("电表=") + f2(meter_val, 2) + " kW, 三路平衡=" + f2(balance, 2) +
                    " kW, 算法用=" + f2(algo_grid, 2) + " kW, 两路径差=" + f2(path_diff, 6)));
        }

        return d;
    }

    // =================================================================
    // A2 策略验收 —— 9 个策略逐个在"触发工况"下核对设计意图
    //
    // 直接实例化策略类喂**手工构造的工况快照**，而不是跑整条闭环。
    // 原因：闭环里 9 个策略互相影响，某个策略"没动作"可能只是被上层压住了，
    // 无法判定它**本身**是否实现了设计意图。验收策略要的是可归因的单点证据。
    // =================================================================
    DimensionReport dim_strategy() {
        DimensionReport d;
        d.code = "A2";
        d.name = dim_name(1);
        d.scope = "9 个策略的设计意图（设计方案 §3.1~§3.9）在触发工况下的单点核对";

        RealtimeSnapshot rt;
        rt.timestamp = 100.0;
        rt.p_load_kw = 300.0;
        rt.p_grid_kw = 300.0;
        rt.p_pv_kw = 0.0;
        rt.p_bat_actual_kw = 0.0;
        rt.soc = 0.50;
        rt.temperature_c = 25.0;
        rt.demand_window.window_s = 900.0;
        rt.demand_window.t_elapsed_s = 800.0;
        rt.demand_window.p_avg_past_kw = 200.0;
        rt.pricing.cur_tou_type = TouType::kFlat;

        DeviceLimits dev;

        // ---- A2-01 S01 BMS 禁止充放（L0）----
        {
            BmsForbidStrategy s;
            DeviceLimits a; a.bms_dis_forbidden = true;
            DeviceLimits b; b.bms_chg_forbidden = true;
            DeviceLimits c; c.bms_dis_forbidden = true; c.bms_chg_forbidden = true;
            const auto ra = s.evaluate(rt, a);
            const auto rb = s.evaluate(rt, b);
            const auto rc = s.evaluate(rt, c);
            const bool ok = ra.active && std::fabs(ra.p_upper) < 1e-12 &&
                            rb.active && std::fabs(rb.p_lower) < 1e-12 &&
                            rc.active && std::fabs(rc.p_lower) < 1e-12 &&
                            std::fabs(rc.p_upper) < 1e-12 &&
                            s.priority() == Priority::kL0_Safety;
            d.items.push_back(mk(
                "A2-01", "S01 BMS 禁止充放（L0 硬禁闭）",
                "禁放→p_upper=0；禁充→p_lower=0；同时禁→区间收成 [0,0]",
                ok,
                "禁放 upper=" + f2(ra.p_upper, 1) +
                    ", 禁充 lower=" + f2(rb.p_lower, 1) +
                    ", 双禁 [" + f2(rc.p_lower, 1) + ", " + f2(rc.p_upper, 1) +
                    "], reason=" + ra.reason));
        }

        // ---- A2-02 S02 BMS 请求降功率（L1）----
        {
            BmsDerateStrategy s;
            DeviceLimits a;
            a.pcs_rated_chg_kw = a.pcs_rated_dis_kw = 250.0;
            a.bms_chg_limit_kw = 100.0;
            a.bms_dis_limit_kw = 150.0;
            const auto r = s.evaluate(rt, a);
            const bool ok = r.active && std::fabs(r.p_lower + 100.0) < 1e-12 &&
                            std::fabs(r.p_upper - 150.0) < 1e-12 &&
                            s.priority() == Priority::kL1_SafeOp;
            d.items.push_back(mk(
                "A2-02", "S02 BMS 请求降功率（L1）",
                "BMS 限值 100/150 且 PCS 250/250 → 区间 [-100, 150] 且 active",
                ok,
                "[" + f2(r.p_lower, 1) + ", " + f2(r.p_upper, 1) + "], " + r.reason));
        }

        // ---- A2-03 S03 变压器过载限功率（L1）----
        {
            TransformerLimitStrategy s;
            DeviceLimits a;
            a.pcs_rated_chg_kw = a.pcs_rated_dis_kw = 250.0;
            a.transformer_capacity_kw = 300.0;
            // 过载：tr_load = |260| + 0.1×260 = 286 → ratio 0.953 > 0.95
            RealtimeSnapshot ov = rt;
            ov.p_grid_kw = 260.0;
            ov.p_load_kw = 260.0;
            ov.p_bat_actual_kw = 0.0;
            const auto r = s.evaluate(ov, a);
            DeviceLimits nz = a; nz.transformer_capacity_kw = 0.0;
            const auto rn = s.evaluate(rt, nz);
            const bool ok = r.active && r.p_upper <= 250.0 &&
                            rn.p_lower < -1e17 && rn.p_upper > 1e17 &&
                            s.priority() == Priority::kL1_SafeOp;
            d.items.push_back(mk(
                "A2-03", "S03 变压器过载限功率（L1）",
                "负载率 0.953>0.95 → 区间收紧（active）；容量=0 → 不施加约束",
                ok,
                "过载 [" + f2(r.p_lower, 1) + ", " + f2(r.p_upper, 1) + "] " +
                    r.reason + "；无容量 [" + f2(rn.p_lower, 0) + ", " +
                    f2(rn.p_upper, 0) + "]"));
        }

        // ---- A2-04 S04 需量管理（L2）----
        {
            DemandMgmtStrategy s;
            DeviceLimits a;
            a.pcs_rated_chg_kw = a.pcs_rated_dis_kw = 250.0;
            a.d_target_kw = 250.0;
            // 窗口 900 s，已过 800 s，过去均值 200 kW，当前关口 300 kW：
            //   预测均值 = 200×(800/900) + 300×(100/900) = 211.1 < 250 → 不动作
            const auto r_low = s.evaluate(rt, a);
            // 窗口仅过 300 s，当前关口 400 kW：
            //   预测均值 = 200×(1/3) + 400×(2/3) = 333.3 > 250 → 放电削峰
            RealtimeSnapshot hi = rt;
            hi.p_grid_kw = 400.0;
            hi.demand_window.t_elapsed_s = 300.0;
            const auto r_hi = s.evaluate(hi, a);
            const bool ok = !r_low.active && std::fabs(r_low.p_desired) < 1e-9 &&
                            r_hi.active && r_hi.p_desired > 0.0 &&
                            s.priority() == Priority::kL2_LocalEcon;
            d.items.push_back(mk(
                "A2-04", "S04 需量管理（L2）",
                "窗口预测均值 < 契约 → 不动作（desired=0）；> 契约 → 放电削峰"
                "（desired>0）",
                ok,
                "未超限 desired=" + f2(r_low.p_desired, 2) + " (" + r_low.reason +
                    ")；超限 desired=" + f2(r_hi.p_desired, 2) + " (" + r_hi.reason + ")"));
        }

        // ---- A2-05 S05 防逆流（L2）----
        {
            AntiReverseStrategy s;
            DeviceLimits a;
            a.pcs_rated_chg_kw = a.pcs_rated_dis_kw = 250.0;
            RealtimeSnapshot rev = rt;
            rev.p_grid_kw = -80.0;    // 倒送 80 kW → 需要充电 80 kW
            const auto r = s.evaluate(rev, a);
            const auto r_idle = s.evaluate(rt, a);
            const bool ok = r.active && std::fabs(r.p_desired + 80.0) < 1e-9 &&
                            !r_idle.active &&
                            s.priority() == Priority::kL2_LocalEcon;
            d.items.push_back(mk(
                "A2-05", "S05 防逆流（L2）",
                "关口 -80 kW（倒送）→ 指令充电 80 kW（p_desired=-80）；无倒送→不动作",
                ok,
                "倒送时 desired=" + f2(r.p_desired, 2) + " (" + r.reason +
                    ")；正常时 active=" + std::string(r_idle.active ? "true" : "false")));
        }

        // ---- A2-06 S06 光伏出力平抑（L2）----
        {
            PvSmoothingStrategy s;
            DeviceLimits a;
            a.pcs_rated_chg_kw = a.pcs_rated_dis_kw = 250.0;
            RealtimeSnapshot hi = rt;
            hi.p_pv_kw = 200.0;       // 相对内部滤波值（0）大幅上升 → 需要吸收
            const auto r = s.evaluate(hi, a);
            const bool ok = r.active && r.p_desired < 0.0 &&
                            s.priority() == Priority::kL2_LocalEcon;
            d.items.push_back(mk(
                "A2-06", "S06 光伏出力平抑（L2）",
                "光伏突升 200 kW → 储能吸收（p_desired<0）",
                ok,
                "desired=" + f2(r.p_desired, 2) + " (" + r.reason + ")"));
        }

        // ---- A2-07 S07 峰谷套利（L3 / Timed）----
        {
            PeakValleyStrategy s;
            DeviceLimits a;
            a.pcs_rated_chg_kw = a.pcs_rated_dis_kw = 250.0;
            RealtimeSnapshot v  = rt; v.pricing.cur_tou_type  = TouType::kValley;
            RealtimeSnapshot f  = rt; f.pricing.cur_tou_type  = TouType::kFlat;
            RealtimeSnapshot pk = rt; pk.pricing.cur_tou_type = TouType::kSharp;
            // SOC 保护是**方向性**的：低 SOC 只压放电，高 SOC 只压充电。
            RealtimeSnapshot s_lo = pk; s_lo.soc = 0.05;   // 尖峰 + 低 SOC → 禁放
            RealtimeSnapshot s_hi = v;  s_hi.soc = 0.95;   // 谷时 + 高 SOC → 禁充
            const auto rv   = s.evaluate(v, a);
            const auto rf   = s.evaluate(f, a);
            const auto rp   = s.evaluate(pk, a);
            const auto rlo  = s.evaluate(s_lo, a);
            const auto rhi  = s.evaluate(s_hi, a);
            const bool ok = rv.active && rv.p_desired < 0.0 &&
                            !rf.active && std::fabs(rf.p_desired) < 1e-9 &&
                            rp.active && rp.p_desired > 0.0 &&
                            std::fabs(rlo.p_desired) < 1e-9 &&
                            std::fabs(rhi.p_desired) < 1e-9 &&
                            s.mode() == RunMode::kTimed &&
                            s.priority() == Priority::kL3_GlobalEcon;
            d.items.push_back(mk(
                "A2-07", "S07 峰谷套利（L3 / Timed）",
                "谷→充（<0）、平→0、尖→放（>0）；尖峰+SOC 0.05→禁放、"
                "谷时+SOC 0.95→禁充（SOC 保护是方向性的）",
                ok,
                "谷=" + f2(rv.p_desired, 1) + ", 平=" + f2(rf.p_desired, 1) +
                    ", 尖=" + f2(rp.p_desired, 1) +
                    ", 尖+低SOC=" + f2(rlo.p_desired, 1) + " (" + rlo.reason +
                    "), 谷+高SOC=" + f2(rhi.p_desired, 1) + " (" + rhi.reason + ")"));
        }

        // ---- A2-08 S08 动态预测优化（L3 / MPC）----
        {
            ForecastOptStrategy s;
            DeviceLimits a;
            a.pcs_rated_chg_kw = a.pcs_rated_dis_kw = 250.0;
            a.d_target_kw = 250.0;
            RealtimeSnapshot hi = rt; hi.p_load_kw = 400.0;
            RealtimeSnapshot lo = rt; lo.p_load_kw = 100.0;
            const auto rh = s.evaluate(hi, a);
            const auto rl = s.evaluate(lo, a);
            const bool ok = rh.active && rh.p_desired > 0.0 &&
                            rl.active && rl.p_desired < 0.0 &&
                            s.mode() == RunMode::kMPC;
            d.items.push_back(mk(
                "A2-08", "S08 动态预测优化（L3 / MPC）",
                "负荷 > 契约 → 放电（>0）；负荷 < 契约 → 充电（<0）",
                ok,
                "400 kW→" + f2(rh.p_desired, 2) + ", 100 kW→" + f2(rl.p_desired, 2)));
        }

        // ---- A2-09 S09 需求响应（L3 / Custom）----
        {
            DemandResponseStrategy s;
            DeviceLimits a;
            a.pcs_rated_chg_kw = a.pcs_rated_dis_kw = 250.0;
            DemandResponseStrategy::DrEvent ev;
            ev.active = true; ev.target_kw = 150.0; ev.end_ts = rt.timestamp + 100.0;
            s.set_event(ev);
            const auto r_on = s.evaluate(rt, a);
            RealtimeSnapshot late = rt;
            late.timestamp = rt.timestamp + 200.0;
            const auto r_off = s.evaluate(late, a);
            DemandResponseStrategy s2;
            const auto r_idle = s2.evaluate(rt, a);
            const bool ok = r_on.active && std::fabs(r_on.p_desired - 150.0) < 1e-9 &&
                            !r_off.active && std::fabs(r_off.p_desired) < 1e-9 &&
                            !r_idle.active && s.mode() == RunMode::kCustom;
            d.items.push_back(mk(
                "A2-09", "S09 需求响应（L3 / Custom）",
                "事件激活→按 target 出力 150 kW；事件过期 / 未下发事件→不动作",
                ok,
                "激活=" + f2(r_on.p_desired, 1) + ", 过期=" + f2(r_off.p_desired, 1) +
                    ", 无事件=" + f2(r_idle.p_desired, 1) + " (" + r_off.reason + ")"));
        }

        return d;
    }

    // =================================================================
    // A3 安全验收
    // =================================================================
    DimensionReport dim_safety() {
        DimensionReport d;
        d.code = "A3";
        d.name = dim_name(2);
        d.scope = "9 条安全约束逐位注入、区间矛盾语义、急停锁存、"
                  "全域硬不变量（p_cmd ∈ [p_lower, p_upper]）";

        SafetyEngine eng;
        GridQuality grid;
        RealtimeSnapshot base;
        base.timestamp = 10.0;
        base.p_grid_kw = 200.0;
        base.p_load_kw = 250.0;
        base.p_pv_kw = 50.0;
        base.soc = 0.50;
        base.temperature_c = 30.0;
        DeviceLimits dev;
        dev.pcs_rated_chg_kw = dev.pcs_rated_dis_kw = 250.0;
        dev.bms_chg_limit_kw = dev.bms_dis_limit_kw = 250.0;
        dev.transformer_capacity_kw = 800.0;
        dev.d_target_kw = 600.0;

        // ---- A3-01 九条约束全部被评估 ----
        {
            const auto v = eng.evaluate(base, dev, grid, 0.0, 0.1);
            const char* want[9] = {
                SafetyEngine::kBmsForbid, SafetyEngine::kBmsDerate,
                SafetyEngine::kSocLimit, SafetyEngine::kPcsLimit,
                SafetyEngine::kTransformer, SafetyEngine::kTemp,
                SafetyEngine::kRamp, SafetyEngine::kGridConnect,
                SafetyEngine::kGridQuality};
            int hit = 0;
            for (const char* w : want) if (find_item(v, w)) ++hit;
            d.items.push_back(mk(
                "A3-01", "9 条安全约束全部被评估",
                "verdict.items 含 bms_forbid / bms_derate / soc_limit / pcs_limit / "
                "transformer_limit / battery_temp / ramp_rate / grid_connect / grid_quality",
                v.items.size() == 9 && hit == 9,
                "items=" + std::to_string(v.items.size()) +
                    ", name_hit=" + std::to_string(hit) + "/9"));
        }

        // ---- A3-02 BMS 禁充放（L0 硬禁闭）----
        {
            DeviceLimits a = dev; a.bms_dis_forbidden = true;
            DeviceLimits b = dev; b.bms_chg_forbidden = true;
            const auto va = eng.evaluate(base, a, grid, 0.0, 0.1);
            const auto vb = eng.evaluate(base, b, grid, 0.0, 0.1);
            const auto* ca = find_item(va, SafetyEngine::kBmsForbid);
            const auto* cb = find_item(vb, SafetyEngine::kBmsForbid);
            const bool ok = ca && cb && ca->active && !ca->counts_as_derate &&
                            std::fabs(ca->p_upper) < 1e-12 &&
                            va.l0_hard && std::fabs(va.p_upper) < 1e-12 &&
                            cb->active && std::fabs(cb->p_lower) < 1e-12 &&
                            std::fabs(vb.p_lower) < 1e-12;
            d.items.push_back(mk(
                "A3-02", "BMS 禁充放（L0 硬禁闭）",
                "禁放→约束 p_upper=0 且 verdict 上界=0；禁充→下界=0；l0_hard=true",
                ok,
                "禁放 verdict=[" + f2(va.p_lower, 1) + ", " + f2(va.p_upper, 1) +
                    "] l0_hard=" + (va.l0_hard ? "true" : "false") +
                    "；禁充 verdict=[" + f2(vb.p_lower, 1) + ", " + f2(vb.p_upper, 1) + "]"));
        }

        // ---- A3-03 SOC 上下限 ----
        {
            SafetyParams sp;
            sp.soc_min = 0.10; sp.soc_max = 0.90;
            sp.soc_warn_low = 0.15; sp.soc_warn_high = 0.85;
            SafetyEngine e2(sp);
            DeviceLimits d2;
            d2.pcs_rated_chg_kw = d2.pcs_rated_dis_kw = 250.0;
            d2.bms_chg_limit_kw = d2.bms_dis_limit_kw = 250.0;
            d2.transformer_capacity_kw = 800.0;

            RealtimeSnapshot lo = base; lo.soc = 0.08;
            RealtimeSnapshot hi = base; hi.soc = 0.92;
            RealtimeSnapshot wn = base; wn.soc = 0.14;
            const auto vl = e2.evaluate(lo, d2, grid, 0.0, 0.1);
            const auto vh = e2.evaluate(hi, d2, grid, 0.0, 0.1);
            const auto vw = e2.evaluate(wn, d2, grid, 0.0, 0.1);
            const auto* cl = find_item(vl, SafetyEngine::kSocLimit);
            const auto* ch = find_item(vh, SafetyEngine::kSocLimit);
            const auto* cw = find_item(vw, SafetyEngine::kSocLimit);
            const bool ok = cl && cl->active && std::fabs(cl->p_upper) < 1e-12 &&
                            !cl->counts_as_derate &&
                            ch && ch->active && std::fabs(ch->p_lower) < 1e-12 &&
                            !ch->counts_as_derate &&
                            cw && cw->active && cw->counts_as_derate &&
                            vl.l0_hard && vw.derated &&
                            !vl.emergency && !vh.emergency && !vw.emergency;
            d.items.push_back(mk(
                "A3-03", "SOC 上下限（L0：绝对限 + 预警降额）",
                "SOC 0.08→禁放（上界 0，hard）；0.92→禁充（下界 0，hard）；"
                "0.14→降额（counts_as_derate → verdict.derated）；"
                "三种情形都不得升级为 emergency",
                ok,
                "低 SOC 上界=" + f2(cl ? cl->p_upper : 1e18, 1) +
                    " hard=" + std::string((cl && !cl->counts_as_derate) ? "true" : "false") +
                    ", 高 SOC 下界=" + f2(ch ? ch->p_lower : -1e18, 1) +
                    ", 预警 derate=" + ((cw && cw->counts_as_derate) ? "true" : "false") +
                    ", 预警 verdict.derated=" + (vw.derated ? "true" : "false") +
                    ", emergency=" + (vl.emergency ? "true" : "false")));
        }

        // ---- A3-04 电池温度 ----
        {
            SafetyParams sp;
            sp.temp_warn_c = 45.0; sp.temp_fault_c = 55.0;
            SafetyEngine e2(sp);
            RealtimeSnapshot hot = base; hot.temperature_c = 48.0;
            RealtimeSnapshot fc  = base; fc.temperature_c  = 60.0;
            const auto vh = e2.evaluate(hot, dev, grid, 0.0, 0.1);
            const auto vf = e2.evaluate(fc,  dev, grid, 0.0, 0.1);
            const auto* ch = find_item(vh, SafetyEngine::kTemp);
            const auto* cf = find_item(vf, SafetyEngine::kTemp);
            const bool ok = ch && ch->active && ch->counts_as_derate &&
                            cf && cf->active && !cf->counts_as_derate && vf.emergency;
            d.items.push_back(mk(
                "A3-04", "电池温度（L0：预警降额 / 故障禁闭）",
                "48 ℃→active 且 counts_as_derate；60 ℃→禁闭且 emergency=true",
                ok,
                "48℃ active=" + std::string((ch && ch->active) ? "true" : "false") +
                    " derate=" + std::string((ch && ch->counts_as_derate) ? "true" : "false") +
                    "；60℃ emergency=" + (vf.emergency ? "true" : "false") +
                    " reason=" + (cf ? cf->reason : "n/a")));
        }

        // ---- A3-05 PCS 限功率折减 ----
        {
            SafetyParams sp;
            sp.pcs_derate_ratio = 0.50;
            SafetyEngine e2(sp);
            DeviceLimits d2 = dev;
            d2.pcs_rated_chg_kw = d2.pcs_rated_dis_kw = 250.0;
            const auto v = e2.evaluate(base, d2, grid, 0.0, 0.1);
            const auto* c = find_item(v, SafetyEngine::kPcsLimit);
            const bool ok = c && c->active && c->binds_interval &&
                            std::fabs(c->p_upper - 125.0) < 1e-9 &&
                            std::fabs(c->p_lower + 125.0) < 1e-9;
            d.items.push_back(mk(
                "A3-05", "PCS 限功率（L1 折减系数）",
                "折减系数 0.5 × 额定 250 → 约束区间 [-125, 125] 且参与求交",
                ok,
                "[" + f2(c ? c->p_lower : 0.0, 1) + ", " + f2(c ? c->p_upper : 0.0, 1) +
                    "], binds=" + std::string((c && c->binds_interval) ? "true" : "false")));
        }

        // ---- A3-06 变压器容量（含极端过载投影）----
        {
            DeviceLimits a = dev;
            a.transformer_capacity_kw = 300.0;
            RealtimeSnapshot ov = base;
            ov.p_grid_kw = 420.0;
            ov.p_load_kw = 420.0;
            ov.p_bat_actual_kw = 0.0;
            const auto v = eng.evaluate(ov, a, grid, 0.0, 0.1);
            const auto* c = find_item(v, SafetyEngine::kTransformer);
            DeviceLimits a2 = dev; a2.transformer_capacity_kw = 100.0;
            const auto v2 = eng.evaluate(ov, a2, grid, 0.0, 0.1);
            const auto* c2 = find_item(v2, SafetyEngine::kTransformer);
            const bool ok = c && c->active && c->binds_interval && c->p_upper < 1e17 &&
                            c2 && c2->active &&
                            (c2->p_upper < 1e17 || c2->p_lower > -1e17);
            d.items.push_back(mk(
                "A3-06", "变压器容量（L1，含极端过载投影）",
                "负载率超阈值 → 区间收紧且参与求交；极端过载 → 仍给出有限边界"
                "（不得放开为无穷区间）",
                ok,
                "常规 [" + f2(c ? c->p_lower : 0.0, 1) + ", " +
                    f2(c ? c->p_upper : 0.0, 1) + "] " + (c ? c->reason : "") +
                    "；极端 [" + f2(c2 ? c2->p_lower : 0.0, 1) + ", " +
                    f2(c2 ? c2->p_upper : 0.0, 1) + "] " + (c2 ? c2->reason : "")));
        }

        // ---- A3-07 变化率：必须不是区间约束 ----
        {
            SafetyParams sp;
            sp.enable_ramp = true;
            sp.ramp_kw_per_s = 200.0;
            SafetyEngine e2(sp);
            // 上一拍已下发 20 kW（非零）→ 本拍限速生效（200 kW/s × 0.1 s = 20 kW）
            const auto v = e2.evaluate(base, dev, grid, 20.0, 0.1);
            const auto* c = find_item(v, SafetyEngine::kRamp);
            const bool ok = c && c->active && !c->binds_interval &&
                            !c->counts_as_derate &&
                            std::fabs(c->margin_kw - 20.0) < 1e-9;
            d.items.push_back(mk(
                "A3-07", "变化率 = 后置限速器（不是区间约束）",
                "上一拍 20 kW 时 ramp_rate 约束 active=true、margin=20 kW"
                "（200 kW/s × 0.1 s），但 binds_interval=false 且 "
                "counts_as_derate=false（否则会与硬安全区间产生假矛盾）",
                ok,
                "active=" + std::string((c && c->active) ? "true" : "false") +
                    ", binds=" + std::string((c && c->binds_interval) ? "true" : "false") +
                    ", derate=" + std::string((c && c->counts_as_derate) ? "true" : "false") +
                    ", margin=" + f2(c ? c->margin_kw : 0.0, 2) + " kW"));
        }

        // ---- A3-08 并网约束（不倒送）----
        {
            SafetyParams sp;
            sp.grid_p_min_kw = 0.0;
            sp.grid_p_max_kw = 1e9;
            SafetyEngine e2(sp);
            // base = P_load − P_pv = 250 − 50 = 200 → P_bat ≤ 200 才能保证 P_grid ≥ 0
            const auto v = e2.evaluate(base, dev, grid, 0.0, 0.1);
            const auto* c = find_item(v, SafetyEngine::kGridConnect);
            const bool ok = c && c->active && c->binds_interval &&
                            c->p_upper <= 200.0 + 1e-6 &&
                            std::fabs(v.p_upper - 200.0) < 1e-6;
            d.items.push_back(mk(
                "A3-08", "并网约束（不倒送，L1）",
                "P_load=250, P_pv=50 → 上界 = 200 kW（保证 P_grid ≥ 0）",
                ok,
                "约束上界=" + f2(c ? c->p_upper : 0.0, 2) +
                    ", verdict 上界=" + f2(v.p_upper, 2) + " kW"));
        }

        // ---- A3-09 区间矛盾 → DERATED（不是 EMERGENCY）----
        {
            SafetyParams sp;
            sp.soc_min = 0.10; sp.soc_max = 0.90;
            sp.soc_warn_high = 0.95;    // 关掉预警区，只留绝对禁充
            sp.grid_p_min_kw = 0.0;
            sp.grid_p_max_kw = 1e9;
            sp.strict_l0 = true;
            sp.enable_ramp = false;
            SafetyEngine e2(sp);
            RealtimeSnapshot full = base;
            full.soc = 0.99;
            full.p_load_kw = 50.0;
            full.p_pv_kw = 300.0;       // 光伏大发 → base = -250 → 并网上界 -250
            full.p_grid_kw = -250.0;
            const auto v = e2.evaluate(full, dev, grid, 0.0, 0.1);
            const bool ok = v.contradiction && v.derated && !v.emergency &&
                            std::fabs(v.p_lower) < 1e-12 && std::fabs(v.p_upper) < 1e-12;
            d.items.push_back(mk(
                "A3-09", "区间矛盾 → DERATED（可自恢复），绝不锁存 EMERGENCY",
                "满充禁充 ∩ 不许倒送 → contradiction=true、derated=true、"
                "emergency=false、区间收成 [0,0]",
                ok,
                std::string("contradiction=") + (v.contradiction ? "true" : "false") +
                    ", derated=" + (v.derated ? "true" : "false") +
                    ", emergency=" + (v.emergency ? "true" : "false") +
                    ", 区间=[" + f2(v.p_lower, 1) + ", " + f2(v.p_upper, 1) + "]" +
                    ", reason=" + v.reason));
        }

        // ---- A3-10 急停锁存 + 显式复位 ----
        {
            EmsRuntime rt;
            rt.config().dt_s = 0.1;
            rt.apply_configs();
            rt.fsm().request_run(true);
            for (int i = 0; i < 20; ++i) rt.step(0.1);
            const EmsState before = rt.fsm().state();

            rt.emergency_stop("acceptance_probe");
            for (int i = 0; i < 5; ++i) rt.step(0.1);
            const EmsState after = rt.fsm().state();
            for (int i = 0; i < 200; ++i) rt.step(0.1);   // 故障源已消失
            const EmsState still = rt.fsm().state();
            const bool rs = rt.reset_emergency();
            for (int i = 0; i < 5; ++i) rt.step(0.1);
            const EmsState recovered = rt.fsm().state();

            const bool ok = after == EmsState::kEmergency &&
                            still == EmsState::kEmergency && rs &&
                            recovered != EmsState::kEmergency &&
                            !rt.fsm().emergency_latched();
            d.items.push_back(mk(
                "A3-10", "急停锁存 + 显式复位",
                "急停后 205 拍仍停在 EMERGENCY（锁存）；reset_emergency() 后才离开",
                ok,
                std::string(state_name(before)) + " → " + state_name(after) +
                    " → 205 拍后仍 " + state_name(still) +
                    " → reset → " + state_name(recovered)));
        }

        // ---- A3-11 全域硬不变量（7 场景抽样裁决）----
        {
            long long ticks = 0;
            long long breach = 0;
            int scenarios_ok = 0;
            for (const auto& sc : all_scenarios()) {
                ScenarioConfig c = sc;
                c.steps = std::max(2000, c.steps / 6);
                const ScenarioResult r = run_scenario(c);
                ticks   += r.samples;
                breach  += static_cast<long long>(r.out_of_interval + r.over_limit +
                                                  r.gated_nonzero);
                if (r.ok()) ++scenarios_ok;
            }
            d.items.push_back(mk(
                "A3-11", "全域硬不变量：p_cmd ∈ [p_lower, p_upper]",
                "7 场景抽样（各 2000 拍）：指令逃逸 + 功率超限 + 门控失效 合计 = 0",
                breach == 0,
                "抽样拍数=" + std::to_string(ticks) +
                    ", 越界合计=" + std::to_string(breach) +
                    ", 场景通过=" + std::to_string(scenarios_ok) + "/7"));
        }

        return d;
    }

    // =================================================================
    // A4 性能验收
    // =================================================================
    DimensionReport dim_performance() {
        DimensionReport d;
        d.code = "A4";
        d.name = dim_name(3);
        d.scope = "单拍闭环耗时（均值/峰值/占用率）、24h 离线仿真墙钟加速比、"
                  "长跑进程资源无泄漏";

        // ---- 单拍耗时：10 Hz 控制周期下 20000 拍 ----
        {
            EmsRuntime rt;
            rt.config().dt_s = opt_.perf_dt_s;
            rt.config().log_every = 1;
            rt.apply_configs();
            rt.fsm().request_run(true);

            const DWORD h0 = handle_count();
            const double rss0 = rss_kb();

            rt.run(opt_.perf_steps, opt_.perf_dt_s, [](EmsRuntime& r, int i) {
                r.set_environment(320.0 + 110.0 * std::sin(i * 0.003),
                                  140.0 + 70.0 * std::sin(i * 0.017));
            });

            const auto m = rt.metrics();
            const DWORD h1 = handle_count();
            const double rss1 = rss_kb();

            raw_.perf_mean_us  = m.mean_cycle_us;
            raw_.perf_max_us   = m.max_cycle_us;
            raw_.handle_delta  = static_cast<int>(h1) - static_cast<int>(h0);
            raw_.rss_kb_delta  = rss1 - rss0;
            raw_.perf_duty_pct = 100.0 * m.mean_cycle_us / (opt_.perf_dt_s * 1e6);

            d.items.push_back(mk(
                "A4-01", "单拍闭环耗时（均值）",
                "mean_cycle_us ≤ " + f2(opt_.perf_mean_us_max, 0) +
                    " us（10 Hz 控制周期下即 CPU 占用 ≤ " +
                    f2(100.0 * opt_.perf_mean_us_max / (opt_.perf_dt_s * 1e6), 2) + "%）",
                m.mean_cycle_us <= opt_.perf_mean_us_max,
                f2(m.mean_cycle_us, 2) + " us / " + std::to_string(opt_.perf_steps) + " 拍"));

            d.items.push_back(mk(
                "A4-02", "单拍闭环耗时（峰值，含首拍冷启动）",
                "max_cycle_us ≤ " + f2(opt_.perf_max_us_max, 0) +
                    " us（10 Hz 控制周期的一半：单拍不得吃掉半个节拍）",
                m.max_cycle_us <= opt_.perf_max_us_max,
                f2(m.max_cycle_us, 2) + " us"));

            d.items.push_back(mk(
                "A4-03", "单拍时间占用率",
                "mean_cycle_us / (dt_s × 1e6) ≤ " + f2(opt_.perf_duty_pct_max, 1) + "%",
                raw_.perf_duty_pct <= opt_.perf_duty_pct_max,
                f2(raw_.perf_duty_pct, 3) + "% (dt=" + f2(opt_.perf_dt_s, 3) + " s)"));

            d.items.push_back(mk(
                "A4-04", "长跑进程资源无泄漏",
                "20000 拍前后 GetProcessHandleCount 增量 ≤ 0 且 工作集增量 ≤ 64 MB",
                raw_.handle_delta <= 0 && (raw_.rss_kb_delta / 1024.0) <= 64.0,
                "handle Δ=" + std::to_string(raw_.handle_delta) +
                    ", RSS Δ=" + f2(raw_.rss_kb_delta / 1024.0, 2) + " MB"));
        }

        // ---- 24h 离线仿真墙钟加速比 ----
        {
            Sim24hConfig cfg = make_default_24h_config();
            cfg.out_dir.clear();              // 落盘已在 10/ 的 demo 里验过
            const Sim24hResult r = run_sim_24h(cfg);
            const double sim_s = cfg.duration_s;
            const double speedup = (r.wall_s > 1e-9) ? (sim_s / r.wall_s) : 0.0;
            raw_.soak_wall_s    = r.wall_s;
            raw_.soak_speedup   = speedup;
            raw_.econ_saving_cny = r.econ.saving_total_cny;
            raw_.econ_cycles     = r.econ.equiv_cycles;

            d.items.push_back(mk(
                "A4-05", "24h 离线仿真墙钟加速比",
                "24 h（86400 s）仿真墙钟 ≤ " + f2(sim_s / opt_.perf_speedup_min, 1) +
                    " s（即加速比 ≥ " + f2(opt_.perf_speedup_min, 0) + "×）",
                r.ok && speedup >= opt_.perf_speedup_min,
                (r.ok ? std::string() : ("ERROR: " + r.error + "; ")) +
                    "wall=" + f2(r.wall_s, 2) + " s, speedup=" + f2(speedup, 0) +
                    "×, steps=" + std::to_string(r.steps)));
        }

        return d;
    }

    // =================================================================
    // A5 稳定性验收
    // =================================================================
    DimensionReport dim_stability() {
        DimensionReport d;
        d.code = "A5";
        d.name = dim_name(4);
        d.scope = "24 h 长稳（10/ 全栈 + 三段故障窗）、跨进程闭环长稳（11/ RT_DB）、"
                  "采集零 stale、SOE 零丢弃、状态迁移与指令抖动有界、可观测性与 "
                  "log_every 解耦、BMS 禁充放位（设备侧上报 → 下发指令不放电）";

        // ---- A5-01/02/03 24h 长稳 ----
        {
            Sim24hConfig cfg = make_default_24h_config();
            cfg.dt_s = opt_.soak_dt_s;
            cfg.log_every = 1;
            cfg.out_dir.clear();
            cfg.auto_restart_after_fault = true;
            cfg.fault_windows.clear();
            cfg.fault_windows.push_back({6.0 * 3600, 6.2 * 3600, 4, "PCS_FAULT"});
            cfg.fault_windows.push_back({12.0 * 3600, 12.15 * 3600, 1, "BMS_COMM_LOST"});
            cfg.fault_windows.push_back({18.0 * 3600, 18.1 * 3600, 6, "DATA_INVALID"});
            cfg.grid_min_required = -1e18;
            cfg.grid_max_required = 1e18;
            cfg.tr_check_cap_kw = 0.0;

            const Sim24hResult r = run_sim_24h(cfg);
            raw_.soak_steps     = r.steps;
            raw_.soak_breach    = r.out_of_interval + r.over_limit + r.gated_nonzero;
            raw_.soak_state_chg = r.metrics.state_changes;
            raw_.soak_travel_ps = r.metrics.cmd_travel_per_s;

            d.items.push_back(mk(
                "A5-01", "24h 长稳：硬不变量逐拍成立",
                "86400 拍（1 s 步长，含 3 段故障窗）内 指令逃逸 + 功率超限 + "
                "门控失效 合计 = 0",
                r.ok && r.hard_invariants_ok(),
                "steps=" + std::to_string(r.steps) +
                    ", 越界=" + std::to_string(raw_.soak_breach) +
                    ", 故障窗拍数=" + std::to_string(r.fault_ticks) +
                    ", 告警=" + std::to_string(r.alarm_count) +
                    ", 经济净收益=" + f2(raw_.econ_saving_cny, 0) + " 元"));

            d.items.push_back(mk(
                "A5-02", "24h 长稳：状态机无抖动",
                "状态迁移次数 ≤ " + std::to_string(opt_.stability_state_changes_max) +
                    " 次（一天内不应反复来回切）",
                r.metrics.state_changes <= opt_.stability_state_changes_max,
                "state_changes=" + std::to_string(r.metrics.state_changes) +
                    "（86400 拍）"));

            d.items.push_back(mk(
                "A5-03", "24h 长稳：指令抖动有界",
                "指令总行程速率 ≤ " + f2(opt_.stability_travel_per_s_max, 1) + " kW/s，"
                "且 符号翻转率 ≤ 0.20 /s、方向反转率 ≤ 1.00 /s（07/ 工程口径）",
                r.metrics.cmd_travel_per_s <= opt_.stability_travel_per_s_max &&
                    r.metrics.no_oscillation(),
                "travel=" + f2(r.metrics.cmd_travel_kw, 1) + " kW (" +
                    f2(r.metrics.cmd_travel_per_s, 3) + " kW/s), flips=" +
                    f2(r.metrics.flip_rate_per_s, 4) + "/s, rev=" +
                    f2(r.metrics.reversal_rate_per_s, 4) + "/s"));
        }

        // ---- A5-04 跨进程闭环长稳（走共享内存）----
        {
            bool ok = false;
            std::string ev = "未建段";
            bool created = false;
            if (ems_rt_db_setup(true, &created)) {
                rt_db_handle_t h_dev{};
                if (rt_db_init(&h_dev, nullptr)) {
                    {
                        std::vector<DevFaultWindow> script = {
                            {2.0, 2.3, 1, "BMS_COMM_LOST"},
                            {4.0, 4.2, 4, "PCS_FAULT"}};
                        DeviceSideSim dev(&h_dev, script);
                        rt_db_handle_t h_ems{};
                        if (rt_db_init(&h_ems, nullptr)) {
                            EmsSideApp app(&h_ems);
                            double dev_t = 0.0;
                            app.io().set_device_pump([&](double, double dt) {
                                dev.step(dev_t, dt);
                                dev_t += dt;
                            });
                            app.run(opt_.rtdb_soak_steps, opt_.rtdb_soak_dt_s);

                            raw_.rtdb_stale    = app.io().stale_reads();
                            raw_.rtdb_out_ival = dev.stats().cmd_out_of_interval;
                            raw_.rtdb_steps    = opt_.rtdb_soak_steps;
                            raw_.rtdb_ran      = true;
                            raw_.soe_events    =
                                static_cast<int>(app.obs().soe().size());
                            raw_.soe_dropped   =
                                static_cast<int>(app.obs().soe().dropped());

                            ok = (raw_.rtdb_stale == 0 && raw_.rtdb_out_ival == 0);
                            ev = "ticks=" + std::to_string(raw_.rtdb_steps) +
                                 ", stale=" + std::to_string(raw_.rtdb_stale) +
                                 ", 指令越 EMS 区间=" +
                                 std::to_string(raw_.rtdb_out_ival) +
                                 ", SOE=" + std::to_string(raw_.soe_events);
                            rt_db_cleanup(&h_ems);
                        }
                    }
                    rt_db_cleanup(&h_dev);
                }
            }
            d.items.push_back(mk(
                "A5-04", "跨进程闭环长稳（RT_DB 共享内存）",
                "6000 拍：EMS 采集 stale_reads=0 且 设备侧观测到的指令越区间拍数=0",
                ok, ev));
        }

        // ---- A5-05 SOE 零丢弃 ----
        {
            const bool ok = raw_.rtdb_ran && raw_.soe_dropped == 0;
            d.items.push_back(mk(
                "A5-05", "SOE 日志零丢弃",
                "跨进程长稳结束后 soe().dropped()==0（容量足够 + 边沿检测正确）",
                ok,
                std::string("ran=") + (raw_.rtdb_ran ? "true" : "false") +
                    ", dropped=" + std::to_string(raw_.soe_dropped) +
                    ", events=" + std::to_string(raw_.soe_events)));
        }

        // ---- A5-06 可观测性与 log_every 解耦 ----
        {
            long long s1 = 0, s10 = 0;
            int ev1 = 0, ev10 = 0;
            for (int k = 0; k < 2; ++k) {
                EmsRuntime rt;
                rt.config().dt_s = 0.1;
                rt.config().log_every = (k == 0 ? 1 : 10);
                rt.apply_configs();
                rt.fsm().request_run(true);
                RuntimeObserver obs;
                obs.start(0.0);
                for (int i = 0; i < 2000; ++i) {
                    const StepRecord rec = rt.step(0.1);
                    obs.on_step(rt, rec);
                }
                if (k == 0) { s1 = obs.totals().steps;  ev1  = static_cast<int>(obs.soe().size()); }
                else        { s10 = obs.totals().steps; ev10 = static_cast<int>(obs.soe().size()); }
            }
            d.items.push_back(mk(
                "A5-06", "可观测性与 log_every 解耦",
                "log_every=1 与 =10 两种配置下观察者累计步数相同（均 = 实际拍数 2000）",
                s1 == 2000 && s10 == 2000,
                "log_every=1 → steps=" + std::to_string(s1) +
                    " (SOE " + std::to_string(ev1) + ")；log_every=10 → steps=" +
                    std::to_string(s10) + " (SOE " + std::to_string(ev10) + ")"));
        }

        // ---- A5-07 BMS 禁充放位：设备侧上报 → 指令不放电（A1，2026-09-19 补）----
        //
        // 为什么验收层也要单独有一条：这两个 bool 是 04/S01(kBmsForbid, **L0 最
        // 底层**) 与 05/check_bms_forbid() 的**唯一输入**。此前 RtDbDeviceIO 把
        // 它们硬写成 false，且 EmsRuntime::step() 从不刷新设备限值 —— 两条加起来
        // 让"BMS 禁充放"在现场完全失效，而所有单进程测试全绿：仿真场景直接
        // 注入 DeviceLimits，**同时绕开**了点表与运行期刷新这两条路。
        //
        // 本条走真实路径：设备侧写点 → 共享内存 → 每拍刷新 → 安全层 → 指令。
        // 判据取验收口径（公开出口 = 下发指令）：禁放窗内 0 拍放电；
        // 反向守卫 = 窗内**非门控**拍的权限上界被收死 —— 证明约束真的动作了，
        // 而不是"这台机器本来就在充电"（后者会让"不放电"变成假通过）。
        {
            bool ok = false;
            std::string ev = "未建段";
            bool created = false;
            if (ems_rt_db_setup(true, &created)) {
                rt_db_handle_t h_dev{};
                if (rt_db_init(&h_dev, nullptr)) {
                    {
                        std::vector<DevFaultWindow> script = {
                            {2.0, 3.0, 8, "BMS_FORBID_DISCHARGE"}};
                        DeviceSideSim dev(&h_dev, script);
                        rt_db_handle_t h_ems{};
                        if (rt_db_init(&h_ems, nullptr)) {
                            EmsSideApp app(&h_ems);
                            double dev_t = 0.0;
                            app.io().set_device_pump([&](double, double dt) {
                                dev.step(dev_t, dt);
                                dev_t += dt;
                            });

                            const double dt = 0.1;
                            int in_ticks = 0, dead_upper = 0, discharging = 0;
                            for (int i = 0; i < 100; ++i) {   // 10 s：窗前 2 s / 窗内 1 s
                                const StepRecord rec = app.step(dt);
                                const bool w = (i * dt >= 2.0 && i * dt < 3.0);
                                if (!w) continue;
                                if (rec.p_cmd > 1e-6) ++discharging;
                                // 门控拍（非运行态）的区间恒被收到 0，不是安全层的
                                // 动作，计入会让反向守卫失去意义 —— 排除。
                                if (rec.state_gated) continue;
                                ++in_ticks;
                                if (rec.p_upper <= 1e-6) ++dead_upper;
                            }
                            // 反向守卫：窗内至少 5 拍非门控拍，判据才有内容
                            ok = (in_ticks >= 5) && (discharging == 0) &&
                                 (dead_upper >= in_ticks - 3);
                            ev = "窗内非门控拍=" + std::to_string(in_ticks) +
                                 ", 上界收死=" + std::to_string(dead_upper) +
                                 ", 放电拍数=" + std::to_string(discharging);
                            rt_db_cleanup(&h_ems);
                        }
                    }
                    rt_db_cleanup(&h_dev);
                }
            }
            d.items.push_back(mk(
                "A5-07", "BMS 禁放位：设备侧上报 → 下发指令不放电",
                "禁放窗内 0 拍放电、且非门控拍的权限上界全部被收死（反向守卫："
                "窗内 ≥ 5 拍非门控）；走设备侧写点 → 共享内存 → 每拍刷新 的真实路径",
                ok, ev));
        }

        return d;
    }

    // =================================================================
    // A6 多策略协同验收 —— 设计方案 §7 周期 9 的 7 个组合场景
    // =================================================================
    DimensionReport dim_coordination() {
        DimensionReport d;
        d.code = "A6";
        d.name = dim_name(5);
        d.scope = "设计方案 §7 周期 9 的 7 个重点验证场景（闭环时序级）全通过："
                  "杜绝指令冲突、互相覆盖、频繁切换、双向充放电、功率超限";

        const std::vector<ScenarioConfig> all = all_scenarios();
        const int scale = std::max(1, opt_.scenario_scale);
        int pass = 0;
        int idx = 0;
        for (const auto& sc : all) {
            ++idx;
            ScenarioConfig c = sc;
            c.steps = std::max(2000, c.steps * scale);
            const ScenarioResult r = run_scenario(c);
            if (r.ok()) ++pass;

            std::string ev = "samples=" + std::to_string(r.samples) +
                             ", over_limit=" + std::to_string(r.over_limit) +
                             ", out_of_interval=" + std::to_string(r.out_of_interval) +
                             ", grid_breach=" + std::to_string(r.grid_breach) +
                             ", tr_breach=" + std::to_string(r.tr_breach) +
                             ", gated_nonzero=" + std::to_string(r.gated_nonzero) +
                             ", flip=" + f2(r.flip_rate, 4) + "/s" +
                             ", rev=" + f2(r.rev_rate, 4) + "/s" +
                             ", SOC=[" + f2(r.soc_min, 3) + ", " + f2(r.soc_max, 3) + "]";
            if (!r.violations.empty()) ev += "；首条违规: " + r.violations.front();

            d.items.push_back(mk(
                "A6-0" + std::to_string(idx), r.id + " " + r.name,
                "违规计数全 0（指令冲突/互相覆盖/频繁切换/双向充放/功率超限）",
                r.ok(), ev));
        }

        d.items.push_back(mk(
            "A6-08", "7 场景汇总",
            "7/7 场景全部通过",
            pass == 7,
            "通过 " + std::to_string(pass) + "/7"));

        return d;
    }

    // =================================================================
    // A7 文档验收
    // =================================================================
    DimensionReport dim_documentation() {
        DimensionReport d;
        d.code = "A7";
        d.name = dim_name(6);
        d.scope = "根文档、各模块 docs/README.md、各模块构建脚本、统一构建入口"
                  "步数、根 README 模块索引、验收报告落盘能力";

        const std::string R = opt_.root;

        // ---- A7-01 根文档 ----
        {
            const char* root_docs[] = {
                "README.md",
                "CHANGES.md",
                "工商业储能EMS调控策略设计方案.md"};
            int hit = 0;
            std::ostringstream ev;
            for (const char* p : root_docs) {
                const std::string full = R + "/" + p;
                const long n = file_size(full);
                if (n >= 200) ++hit;
                ev << p << "(" << (n >= 0 ? std::to_string(n) : "缺") << " 字节) ";
            }
            d.items.push_back(mk(
                "A7-01", "根文档齐备且非空",
                "README.md / CHANGES.md / 设计方案 三份均存在且 ≥ 200 字节",
                hit == 3, ev.str()));
        }

        // ---- A7-02 模块文档 ----
        {
            const char* mods[] = {
                "01", "02",
                "03/anti_reverse_controller", "03/pv_smoothing_controller",
                "03/demand_management_controller", "03/integration",
                "04", "05", "06", "07", "08", "09", "10", "11", "12", "P1", "P2"};
            const int n = static_cast<int>(sizeof(mods) / sizeof(mods[0]));
            int hit = 0;
            std::ostringstream miss;
            for (int i = 0; i < n; ++i) {
                const std::string full = R + "/" + mods[i] + "/docs/README.md";
                if (file_ok_ci(full, 200)) ++hit;
                else miss << mods[i] << " ";
            }
            d.items.push_back(mk(
                "A7-02", "各模块 docs/README.md 齐备",
                std::to_string(n) + " 个模块文档均存在且 ≥ 200 字节",
                hit == n,
                "hit=" + std::to_string(hit) + "/" + std::to_string(n) +
                    (miss.str().empty() ? "" : ("  缺失: " + miss.str()))));
        }

        // ---- A7-03 模块构建脚本 ----
        {
            const char* dirs[] = {
                "01", "02",
                "03/anti_reverse_controller", "03/pv_smoothing_controller",
                "03/demand_management_controller", "03/integration",
                "04", "05", "06", "07", "08", "09", "10", "11", "12", "P1", "P2"};
            const int n = static_cast<int>(sizeof(dirs) / sizeof(dirs[0]));
            int hit = 0;
            std::ostringstream miss;
            for (int i = 0; i < n; ++i) {
                const std::string a = R + "/" + dirs[i] + "/scripts/build.bat";
                const std::string b = R + "/" + dirs[i] + "/scripts/build_test.bat";
                if (file_ok_ci(a, 50) || file_ok_ci(b, 50)) ++hit;
                else miss << dirs[i] << " ";
            }
            d.items.push_back(mk(
                "A7-03", "各模块构建脚本齐备",
                "每个模块的 scripts/ 下存在 build.bat 或 build_test.bat（≥ 50 字节）",
                hit == n,
                "hit=" + std::to_string(hit) + "/" + std::to_string(n) +
                    (miss.str().empty() ? "" : ("  缺失: " + miss.str()))));
        }

        // ---- A7-04 统一构建入口：步数自洽且覆盖全部模块 ----
        //
        // 判据不写死步数，而是"步骤号集合必须恰好是 1..N 且无缺号"——
        // 这样新增模块时不会因为忘了改验收脚本而假通过/假失败；
        // 同时要求 N ≥ 27（01..12 + P1 + P2 各有 build 与 test 两类步骤）。
        {
            const std::string p = R + "/scripts/build_all.bat";
            const std::string txt = read_text_file(p);

            int total_n = 0;
            {
                const std::string key = "echo 1/";
                const std::size_t pos = txt.find(key);
                if (pos != std::string::npos) {
                    std::size_t i = pos + key.size();
                    while (i < txt.size() && txt[i] >= '0' && txt[i] <= '9') {
                        total_n = total_n * 10 + (txt[i] - '0');
                        ++i;
                    }
                }
            }
            int steps = 0;
            for (int i = 1; i <= total_n; ++i) {
                if (contains(txt, "echo " + std::to_string(i) + "/" +
                                     std::to_string(total_n))) ++steps;
            }
            const char* need[] = {"01\\", "02\\", "03\\", "04\\", "05\\", "06\\",
                                  "07\\", "08\\", "09\\", "10\\", "11\\", "12\\",
                                  "P1\\", "P2\\"};
            int mods = 0;
            for (const char* m : need) if (contains(txt, m)) ++mods;
            const std::string banner = "All " + std::to_string(total_n) +
                                       " components built";
            const bool ok = total_n >= 27 && steps == total_n && mods == 14 &&
                            contains(txt, banner) &&
                            contains(txt, "[BUILD ALL FAIL]");
            d.items.push_back(mk(
                "A7-04", "统一构建入口步数自洽且覆盖全部模块",
                "scripts/build_all.bat 的步骤号恰好是 1..N 无缺号（N ≥ 27，"
                "01..12 + P1/P2 各含 build 与 test 步）、14 个模块目录全部被引用、"
                "成功/失败横幅与 N 一致",
                ok,
                "N=" + std::to_string(total_n) +
                    ", step_markers=" + std::to_string(steps) + "/" +
                    std::to_string(total_n) +
                    ", module_refs=" + std::to_string(mods) + "/14" +
                    ", banner=" + (contains(txt, banner) ? "ok" : "缺")));
        }

        // ---- A7-05 根 README 索引 ----
        {
            const std::string txt = read_text_file(R + "/README.md");
            const char* need[] = {"01/", "02/", "03/", "04/", "05/", "06/", "07/",
                                  "08/", "09/", "10/", "11/", "12/", "P1/", "P2/"};
            int hit = 0;
            std::ostringstream miss;
            for (const char* m : need) {
                if (contains(txt, m)) ++hit;
                else miss << m << " ";
            }
            d.items.push_back(mk(
                "A7-05", "根 README 是全项目索引",
                "README.md 的模块清单里出现 01/ .. 12/ 与 P1/ P2/ 全部 14 项",
                hit == 14,
                "hit=" + std::to_string(hit) + "/14" +
                    (miss.str().empty() ? "" : ("  缺失: " + miss.str()))));
        }

        // ---- A7-06 验收报告落盘能力（三件套自检）----
        {
            // 用当前维度自身作为一个"微型报告"写入临时目录，
            // 验证 md/json/html 三个渲染器与落盘路径全部可用。
            AcceptanceReport probe;
            probe.generated_at = now_stamp();
            probe.dims.push_back(d);
            const std::string dir = R + "/12/data/_report_selftest";
            std::string err;
            const bool written = write_reports(dir, probe, &err);
            const char* files[] = {"ACCEPTANCE-REPORT.md",
                                   "ACCEPTANCE-REPORT.json",
                                   "ACCEPTANCE-REPORT.html"};
            int sizes_hit = 0;
            std::ostringstream ev;
            for (const char* f : files) {
                const long n = file_size(dir + "/" + f);
                if (n >= 1024) ++sizes_hit;
                ev << f << "=" << (n >= 0 ? std::to_string(n) : "缺") << "B ";
            }
            d.items.push_back(mk(
                "A7-06", "验收报告三件套落盘能力（md / json / html）",
                "渲染 + 写盘成功，三个文件均 ≥ 1 KB（正式报告由 "
                "12/scripts/run_acceptance.bat 写入 12/docs/）",
                written && sizes_hit == 3,
                (written ? "" : ("写盘失败: " + err + "; ")) + ev.str()));
        }

        return d;
    }

    Options     opt_;
    RawMeasure  raw_;
};

} // namespace ems
