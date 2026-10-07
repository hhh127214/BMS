// =====================================================================
// 10/ — 实时仿真源（sim_live）
//
// 定位：**「运行模式」的默认数据源**。
//
//   在此之前，本项目只有一个时间纪律：离线批量仿真（run_sim_24h）——
//   一口气把 86400 拍算完，越快越好，产物是一次性的 CSV 文件。
//   那套东西适合"跑一遍、看结论"，不适合"挂在那里一直跑、界面跟着动"。
//
//   本文件补上第二条时间纪律：**按墙钟节拍逐拍推进**。每一拍
//     · 取当前真实时刻作为曲线相位（19:30 就按 19:30 的负荷/电价）
//     · 注入环境 → 跑一拍完整的 EMS 闭环（安全 → 仲裁 → 状态机 → 输出整形）
//     · 把这一拍**立刻写进实录 CSV 并 flush**
//   于是 14/ 平台可以像对待 07/ 现场进程一样尾随这个文件、增量入库，
//   界面就能实时看到数据在长。
//
// ★ 与离线仿真的关系（这是本文件最要紧的一条纪律）：
//   两者**共用 assemble_runtime() 的同一套装配**，差别只有两处 ——
//     · 时间来源：i*dt（批量）  vs  墙钟 + 睡眠到绝对期限（实时）
//     · 产出方式：跑完写文件（批量） vs  逐拍 flush 追加（实时）
//   "两者是同一个模型"由 scripts/build_test.bat 的断言保证：同起点、同拍数、
//   同 dt 下，实时源写出的实录与批量仿真的 timeseries.csv **逐列一致**。
//   不是靠"抄的时候小心"。
//
// ★ 实录 CSV 的列契约：**直接复用 07/src/record_csv.h**（同一份序列化器）。
//   意义在于：接真机时只要把数据源换成 `07/ main_field.exe --device modbus`，
//   14/ 的入库、界面、曲线全都不用改 —— 因为两边写的是同一种文件。
//
// ★ 已知边界（诚实记录）：
//   · 曲线是**典型日**：负荷/光伏/电价按 96 点日曲线回绕，不是真实天气序列。
//     所以"运行模式"此刻是"实时跑起来的模型"，不是真实电站。
//   · 每拍耗时若超过 dt（默认 1 s），进程会落后于墙钟。本文件会统计并把
//     lag 暴露出来（SimLiveResult::lag_s / max_lag_s），不假装它不存在。
// =====================================================================

#pragma once

#include "sim_24h.h"
#include "record_csv.h"          // 07/ —— 实录 CSV 的唯一序列化契约

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>
#include <thread>

namespace ems {

// Unix 墙钟秒（与 07/src/main_field.cpp 的 unix_now() 同口径）
inline double live_unix_now() {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// 本地时区的"当天第几秒"（0 .. 86399）。
// 用 localtime 而不是 gmtime：界面上要看到的时间是现场墙上的钟。
inline double local_second_of_day(double unix_s) {
    const std::time_t t = static_cast<std::time_t>(unix_s);
    std::tm tb{};
#if defined(_WIN32)
    localtime_s(&tb, &t);
#else
    localtime_r(&t, &tb);
#endif
    return static_cast<double>(tb.tm_hour) * 3600.0
         + static_cast<double>(tb.tm_min)  * 60.0
         + static_cast<double>(tb.tm_sec);
}

// =====================================================================
// 选项
// =====================================================================
struct SimLiveOptions {
    std::string record_path;          // 实录 CSV 路径（必填）

    // 起始相位（秒）。
    //   phase_auto = true  → 取启动那一刻的"当天真实时刻"（推荐，运行模式默认）
    //   phase_auto = false → 用 phase_s（测试与预热用，保证可复现）
    bool   phase_auto = true;
    double phase_s    = 0.0;

    // 跑多久。**注意这是 SimLiveOptions 自己的字段，与 Sim24hConfig::duration_s
    // 是两回事** —— 那个是离线仿真的时长，这里管的是实时源。
    //   0 = 一直跑到被停（运行模式的生产口径；由 14/ 引擎显式传 --duration-s 0）
    //   >0 = 跑够这么多秒就收工（测试与一次性预热）
    // ★ 踩过的坑：只在 Sim24hConfig 上设了时长、这里忘了设，实时源会按 0 处理
    //   永不停机，一路往实录文件里写 —— 实测写满 1.8 GB 才被发现。
    //   测试里务必同时设 max_wall_s 兜底。
    double duration_s = 0.0;
    bool   pace       = true;         // 按墙钟节拍（false = 全速，供测试）
                                      //   全速时 t_s 走**合成时间轴**（见 run_sim_live）
    double max_wall_s = 0.0;          // 墙钟上界护栏（0 = 不限；测试用）
                                      //   到点即停并把 stopped_by_wall_limit 置真 ——
                                      //   它是"配置写错了"的信号，不是正常结束
    int    log_every  = 1;            // 每 N 拍写一行实录（1 = 拍拍都写）
    int    progress_every = 0;        // 每 N 行打印一次进度（0 = 不打印）
    bool   quiet      = false;        // 不打印任何东西（供被 spawn 时用）
};

struct SimLiveResult {
    bool        ok = false;
    std::string error;

    long long   ticks = 0;            // 实际跑过的控制拍数
    long long   rows  = 0;            // 写出的实录行数
    double      wall_s = 0.0;
    double      phase0_s = 0.0;       // 起始相位（秒，当天时刻）
    double      last_phase_s = 0.0;
    double      last_soc = 0.0;
    std::string last_state;
    int         state_changes = 0;

    // 跟不跟得上墙钟：lag = 墙钟已过 - 模型应推进的时间。
    //   > 0 表示进程落后（每拍耗时超过 dt），持续落后说明该加 dt 或减负载。
    double      lag_s = 0.0;
    double      max_lag_s = 0.0;
    bool        stopped_by_wall_limit = false;
};

// =====================================================================
// 实时跑
//
//   on_row(rows) 每写出一行回调一次（给调用方做进度/健康上报；可为 nullptr）
// =====================================================================
inline SimLiveResult run_sim_live(const Sim24hConfig& cfg,
                                  const SimLiveOptions& opt,
                                  const std::function<void(long long)>& on_row = nullptr) {
    SimLiveResult r;

    ForecastSeries fc;
    EmsRuntime rt;
    if (!assemble_runtime(cfg, rt, fc, &r.error)) {
        r.ok = false;
        return r;
    }

    // 长跑不能攒日志：log_ 是无上界的 vector，跑一周就是几十万行常驻内存。
    // 实时源要的每一行**已经逐拍写进实录 CSV** 了，内存里再留一份没有消费者。
    rt.config().enable_log = false;

    // ---- 起始相位：内部时钟与曲线相位钉在同一个基准上（见 set_clock 注释）----
    const double phase0 = opt.phase_auto ? local_second_of_day(live_unix_now())
                                         : opt.phase_s;
    r.phase0_s = phase0;
    rt.set_clock(phase0);

    record::RecordWriter rec;
    if (opt.record_path.empty()) {
        r.error = "record_path 为空：实时源必须写实录 CSV，否则跑完什么也留不下";
        return r;
    }
    if (!rec.open(opt.record_path)) {
        r.error = "打不开实录文件: " + opt.record_path;
        return r;
    }

    const double dt = cfg.dt_s;
    const long long max_ticks =
        opt.duration_s > 0.0
            ? static_cast<long long>(std::llround(opt.duration_s / dt))
            : -1;

    const auto t_start = std::chrono::steady_clock::now();
    const double unix_start = live_unix_now();
    std::string prev_state;
    long long i = 0;
    double last_unix = 0.0;

    for (;;) {
        if (max_ticks >= 0 && i >= max_ticks) break;

        if (opt.max_wall_s > 0.0) {
            const double el = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t_start).count();
            if (el >= opt.max_wall_s) { r.stopped_by_wall_limit = true; break; }
        }

        // 落后统计：**在睡之前量**，口径是「墙钟已过多少 - 模型本该推进多少」。
        //   放在循环顶部（i*dt）而不是底部（(i+1)*dt）：底部量出来恒等于 -dt，
        //   因为那一拍刚跑完、还没睡。踩过这个坑 —— 界面上会稳定显示
        //   "-1.0 s 落后"，看着像"跑得比墙钟快"，其实只是量错了位置。
        if (opt.pace) {
            const double wall_el = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t_start).count();
            r.lag_s = wall_el - static_cast<double>(i) * dt;
            if (r.lag_s > r.max_lag_s) r.max_lag_s = r.lag_s;
        }

        // 相位 = 内部时钟（step() 里 t_ += dt，所以这里直接用 rt.now()）
        const double phase = rt.now();
        r.last_phase_s = phase;

        // 无故障窗时 apply_step_env 就是"取曲线相位 → 注入环境"。
        // 不传 fault_ticks：实时源默认没有故障窗，计数没有消费者。
        apply_step_env(rt, fc, cfg, phase, nullptr);

        const StepRecord sr = rt.step(dt);
        const std::string st = state_name(sr.state);
        if (!prev_state.empty() && st != prev_state) ++r.state_changes;
        prev_state = st;
        ++r.ticks;

        // 降采样口径与离线仿真**逐字对齐**：离线在 step() 里做的是
        // `++step_index_; if (step_index_ % stride == 0) log_.push_back(rec)`，
        // 即第 stride、2·stride… 拍各留一行（第一行是 t=stride·dt）。
        // 这里用 (i+1) 而不是 i，就是为了让"第几拍"这个计数与离线一致 ——
        // 差一个下标会整体错开一拍，而两条曲线看起来都"挺正常"。
        const int stride = opt.log_every < 1 ? 1 : opt.log_every;
        const bool keep = ((i + 1) % stride) == 0;
        if (keep) {
            // 写出的 t_s 口径（见 commit 说明，这里是最容易埋雷的一处）：
            //   · 按墙钟跑（生产）  → 用**真实 Unix 时刻**，并强制严格递增。
            //     强制递增不是洁癖：入库主键是 (scenario_id, t_s)，两行撞同一
            //     个 t_s 时 INSERT OR REPLACE 会**静默吃掉一行**，而曲线看起来
            //     只是"少了个点"，没人会发现。
            //   · 全速跑（--no-pace）→ 用**按 dt 合成**的时间轴。
            //     全速跑时几十行几乎在同一毫秒写出，若照抄真实墙钟，整段数据会
            //     挤在 0.06 s 内（横轴完全失去意义）；合成轴则严格等差、终点
            //     对齐启动时刻，至少是个良构的时间轴。这一点必须在界面上标注，
            //     所以 14/ 引擎只在测试/预热路径下才用 --no-pace。
            double unix_s;
            if (opt.pace) {
                unix_s = live_unix_now();
                if (unix_s <= last_unix) unix_s = last_unix + 1e-3;
            } else {
                unix_s = unix_start + static_cast<double>(i + 1) * dt;
            }
            last_unix = unix_s;

            rec.write(sr, unix_s);
            ++r.rows;
            if (on_row) on_row(r.rows);
        }

        r.last_state = st;
        r.last_soc   = sr.soc;

        // 进度按**拍**计数，不按行：初值 rows=0 时 `0 % n == 0` 会在
        // log_every>1 的每一拍都成立，启动瞬间刷出一串 "0 拍" 的噪声行。
        if (!opt.quiet && opt.progress_every > 0 &&
            (r.ticks % opt.progress_every) == 0) {
            std::printf("  [live] %lld 拍 / %lld 行  相位 %5.2f h  SOC %.4f  %-10s 落后 %+.2f s\n",
                        r.ticks, r.rows, phase / 3600.0, sr.soc, st.c_str(), r.lag_s);
            std::fflush(stdout);
        }

        // 墙钟节拍：睡到**绝对期限**，避免逐拍误差累积成持续漂移
        if (opt.pace && (max_ticks < 0 || i + 1 < max_ticks)) {
            std::this_thread::sleep_until(
                t_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(
                                  static_cast<double>(i + 1) * dt)));
        }
        ++i;
    }

    rec.close();
    r.wall_s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t_start).count();
    r.ok = r.error.empty();
    return r;
}

} // namespace ems
