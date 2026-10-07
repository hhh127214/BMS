// =====================================================================
// 10/ — 实时仿真源 · 命令行入口
//
// 用法：
//   sim_live.exe --record <实录.csv> [选项]
//
// 「运行模式」的数据源。按墙钟节拍一拍一拍地跑 EMS 闭环，每拍 flush 一行
// 到实录 CSV；14/ 平台尾随该文件增量入库，界面即实时刷新。
//
// 为什么单独一个 main（不往 sim_demo 里塞开关）：
//   两者是**两种时间纪律**，不是同一个程序的两种输出格式。
//   sim_demo   —— 离线批量：跑完就退出，产物是四件套（含报告）
//   sim_live   —— 在线实时：不设终点，产物是**一直长**的实录 CSV
//   混在一个 main 里，"演示"与"在线运行"的语义会互相污染，
//   而 sim_demo 的输出格式被 10/ 的 142 条断言逐字锁定，不能冒这个险。
//
// 用法示例：
//   sim_live.exe --record build/live.csv                    按墙钟跑，相位取当前真实时刻
//   sim_live.exe --record t.csv --duration-s 600 --phase 0 --no-pace
//                                                           全速跑 600 拍（测试用）
// =====================================================================

#include "sim_live.h"
#include "sim_24h.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace ems;

namespace {

void usage() {
    std::printf(
        "usage: sim_live.exe --record <csv> [选项]\n"
        "\n"
        "  --record <csv>        实录 CSV 路径（必填，20 列，与 10/timeseries.csv 同构）\n"
        "  --dt <s>              控制周期，默认 1.0（墙钟节拍也用它）\n"
        "  --duration-s <s>      跑多久，默认 0 = 一直跑到被停\n"
        "  --log-every <n>       每 n 拍写一行实录，默认 1（拍拍都写）。\n"
        "                        接 14/ 平台时给 10 —— 全项目图表与导入都按 10 s 粒度。\n"
        "  --phase <auto|秒>     起始相位，默认 auto（取启动那一刻的当天真实时刻）\n"
        "  --no-pace             不按墙钟睡眠，全速跑（测试/预热用；此时落后统计无意义，\n"
        "                        且 t_s 走**按 dt 合成**的时间轴，不是逐行真实墙钟）\n"
        "  --max-wall-s <s>      墙钟上界护栏，默认 0 = 不限\n"
        "  --progress-every <n>  每 n **拍**打印一次进度，默认 60；0 = 不打印\n"
        "  --csv <path>          从 CSV 导入日曲线（默认内置典型日 96 点）\n"
        "  --quiet               不打印进度（被平台 spawn 时用）\n"
        "  -h, --help            本帮助\n");
}

// 解析浮点，失败返回 false
bool parse_double(const char* s, double* out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (end == s || (end && *end != '\0')) return false;
    *out = v;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    // 被重定向到日志文件时 stdout 默认块缓冲 —— 长跑程序必须行缓冲，
    // 否则 `> log` 的人会以为它卡死了（14/ 的 server.py 踩过同一个坑）。
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    Sim24hConfig cfg = make_default_24h_config();
    cfg.title = "EMS 实时仿真源（运行模式）";

    SimLiveOptions opt;
    std::string curves;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](double* out) -> bool {
            if (i + 1 >= argc) return false;
            return parse_double(argv[++i], out);
        };
        if (a == "--record" && i + 1 < argc) {
            opt.record_path = argv[++i];
        } else if (a == "--dt") {
            if (!need(&cfg.dt_s) || cfg.dt_s <= 0.0) {
                std::printf("[FAIL] --dt 需要正数（秒）\n");
                return 2;
            }
        } else if (a == "--duration-s") {
            if (!need(&opt.duration_s) || opt.duration_s < 0.0) {
                std::printf("[FAIL] --duration-s 需要非负数（0 = 不限）\n");
                return 2;
            }
        } else if (a == "--phase" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v == "auto") {
                opt.phase_auto = true;
            } else {
                double s = 0.0;
                if (!parse_double(v.c_str(), &s) || s < 0.0 || s >= 86400.0) {
                    std::printf("[FAIL] --phase 需要 auto 或 [0,86400) 的秒数\n");
                    return 2;
                }
                opt.phase_auto = false;
                opt.phase_s = s;
            }
        } else if (a == "--no-pace") {
            opt.pace = false;
        } else if (a == "--log-every") {
            double n = 0.0;
            if (!need(&n) || n < 1.0) {
                std::printf("[FAIL] --log-every 需要 >= 1 的整数\n");
                return 2;
            }
            opt.log_every = static_cast<int>(n);
        } else if (a == "--max-wall-s") {
            if (!need(&opt.max_wall_s) || opt.max_wall_s < 0.0) {
                std::printf("[FAIL] --max-wall-s 需要非负数\n");
                return 2;
            }
        } else if (a == "--progress-every") {
            double n = 0.0;
            if (!need(&n) || n < 0.0) {
                std::printf("[FAIL] --progress-every 需要非负整数\n");
                return 2;
            }
            opt.progress_every = static_cast<int>(n);
        } else if (a == "--csv" && i + 1 < argc) {
            curves = argv[++i];
        } else if (a == "--quiet") {
            opt.quiet = true;
        } else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else {
            std::printf("[FAIL] 未知参数：%s（-h 看用法）\n", a.c_str());
            return 2;
        }
    }

    if (opt.record_path.empty()) {
        std::printf("[FAIL] 缺 --record：实时源必须写实录 CSV，否则跑完什么也留不下\n\n");
        usage();
        return 2;
    }

    if (!curves.empty()) {
        cfg.use_csv = true;
        cfg.curves_path = curves;
    }

    if (!opt.quiet) {
        std::printf("=== 10/ 实时仿真源（运行模式的数据源）===\n");
        std::printf("  实录文件 : %s\n", opt.record_path.c_str());
        std::printf("  控制周期 : %.3f s（也是墙钟节拍）\n", cfg.dt_s);
        std::printf("  记录粒度 : 每 %d 拍一行（%.1f s）\n",
                    opt.log_every, cfg.dt_s * opt.log_every);
        std::printf("  时长     : %s\n",
                    opt.duration_s > 0.0
                        ? (std::to_string(static_cast<long long>(opt.duration_s)) + " s").c_str()
                        : "不限（直到被停止）");
        std::printf("  起始相位 : %s\n",
                    opt.phase_auto ? "当天真实时刻（auto）"
                                   : (std::to_string(static_cast<long long>(opt.phase_s)) + " s").c_str());
        std::printf("  节拍     : %s\n", opt.pace ? "按墙钟（sleep_until 绝对期限）" : "全速（--no-pace）");
        std::printf("  曲线来源 : %s\n",
                    cfg.use_csv ? cfg.curves_path.c_str() : "内置典型日（96 点 / 15 min）");
        std::printf("  ★ 边界：曲线是典型日回绕，不是真实天气序列；本进程是"
                    "「实时跑起来的模型」，尚非真实电站。\n");
        std::printf("  ★ 接真机后改用 07/ main_field.exe --device modbus + 同一份实录契约，\n"
                    "    14/ 的入库与界面无需改动。\n");
        std::printf("\n");
    }

    SimLiveResult r = run_sim_live(cfg, opt);

    if (!r.ok) {
        std::printf("[LIVE FAIL] %s\n", r.error.c_str());
        return 1;
    }
    if (!opt.quiet) {
        std::printf("\n=== 实时源停止 ===\n");
        std::printf("  拍数 %lld / 实录 %lld 行 / 墙钟 %.2f s\n",
                    r.ticks, r.rows, r.wall_s);
        std::printf("  起始相位 %.1f s（%.2f h）→ 末相位 %.1f s（%.2f h）\n",
                    r.phase0_s, r.phase0_s / 3600.0,
                    r.last_phase_s, r.last_phase_s / 3600.0);
        std::printf("  末状态 %s / SOC %.4f / 状态迁移 %d 次\n",
                    r.last_state.c_str(), r.last_soc, r.state_changes);
        std::printf("  落后墙钟 当前 %+.2f s / 峰值 %+.2f s%s\n",
                    r.lag_s, r.max_lag_s,
                    r.max_lag_s > cfg.dt_s ? "  ← 每拍耗时已超过 dt，持续落后" : "");
        if (r.stopped_by_wall_limit) std::printf("  （由 --max-wall-s 护栏停止）\n");
    }
    // 机器可读的一行：平台/脚本靠它判断本次实时跑是否正常收尾
    std::printf("[LIVE OK] rows=%lld ticks=%lld wall_s=%.2f max_lag_s=%.2f\n",
                r.rows, r.ticks, r.wall_s, r.max_lag_s);
    return 0;
}
