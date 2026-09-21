// =====================================================================
// 11/ 联调进程 2：EMS 侧进程（ems_side.exe）
//
// RtDbDeviceIO（纯下发，无设备泵）→ EmsRuntime 全栈（04~08）→ P2 观察者。
// 闭环数据全部来自共享内存段（设备进程写的 MEAS），指令经 CMD 下行。
//
// 与 07/T26 的本质差别：那边设备泵在**本进程内**（pump 回调）；这里设备
// 是**另一个进程** —— read_snapshot 读到的是设备进程上一拍发布的量测，
// 这才是真实部署的闭环语义（"execute() 返回值不可信，闭环走下一拍"）。
//
// 用法：ems_side.exe [--steps N] [--dt 0.1] [--speed K]
//                    [--report path] [--log-every K]
//
// 节拍：--speed K（默认 1.0 = 实时）。必须与设备进程用同一个 speed，
//   否则两边模拟时间轴会错开（详见 integration_runner.h 的 Pacing 说明）。
// =====================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "integration_runner.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    int    steps  = 3000;
    double dt     = 0.1;
    double speed  = 1.0;
    int    log_every = 1;
    std::string report_path = "build/integration_ems_report.txt";

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--steps" && i + 1 < argc)       steps = std::atoi(argv[++i]);
        else if (a == "--dt" && i + 1 < argc)     dt = std::atof(argv[++i]);
        else if (a == "--speed" && i + 1 < argc)  speed = std::atof(argv[++i]);
        else if (a == "--log-every" && i + 1 < argc) log_every = std::atoi(argv[++i]);
        else if (a == "--report" && i + 1 < argc) report_path = argv[++i];
    }

    // 连接共享内存段（初始化器必须先起；设备侧应已发布全表初值）
    rt_db_handle_t h{};
    bool connected = false;
    for (int retry = 0; retry < 40 && !connected; ++retry) {
        connected = rt_db_init(&h, nullptr);
        if (!connected) {
            std::printf("[EMS] waiting for RT_DB segment (retry %d)...\n", retry);
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    if (!connected) {
        std::fprintf(stderr, "[EMS] FAIL: cannot connect RT_DB segment "
                             "(run rtdb_initializer.exe first)\n");
        return 1;
    }

    ems::EmsSideApp::Config cfg;
    cfg.dt_s      = dt;
    cfg.log_every = log_every;
    ems::EmsSideApp app(&h, cfg);

    // A3.2：外部设定（调度遥调）—— 网关写 EXT 区，EMS 侧每拍从这里读。
    // read 回调与 ext_setpoints.h 的 ReadFn 同形（bool(int, double&)），
    // 直接读 RT_DB 句柄；stale_s 是"陈旧判据"的墙钟阈值（现场部署再调）。
    app.rt().set_ext_source(
        [&h](int idx, double& v) -> bool {
            long q = 0;
            struct timespec ts{};
            return rt_db_get_value(&h, static_cast<std::size_t>(idx), &v, &q, &ts);
        },
        /*stale_s=*/30.0);

    const int stale0 = app.io().stale_reads();
    const int coll0  = app.io().collisions();

    // 装配层顺序约束：等设备侧先把 CFG.*（一次侧参数）发布进段，再刷新 dev_。
    // 不等的话 EMS 拿到的是点表默认值（变压器 250 kVA），安全边界全错。
    if (!app.wait_for_device(15.0)) {
        std::fprintf(stderr, "[EMS] FAIL: device side not publishing "
                             "(run device_side.exe first)\n");
        return 1;
    }

    // 自检：EMS 连接前设备侧必须已把点表与 CFG 发布进段
    if (app.io().self_check() != 0) {
        std::fprintf(stderr, "[EMS] FAIL: point table mismatch in segment "
                             "(device side not ready?)\n");
        return 1;
    }

    std::printf("[EMS] connected, full stack online "
                "(manager+arbiter+safety+fsm+loop+coordinator), running %d steps @ %.3f s "
                "(speed %.2fx, wall %.1f s), tr=%.0f kVA d_target=%.0f kW\n",
                steps, dt, speed, steps * dt / (speed > 0.0 ? speed : 1.0),
                app.rt().device_limits().transformer_capacity_kw,
                app.rt().device_limits().d_target_kw);
    std::fflush(stdout);

    const auto t0 = std::chrono::steady_clock::now();

    // ---- BMS 禁充放位采样（EMS 侧口径）----
    //
    // 为什么必须"边跑边采"而不是最后看一眼末态：这是一条**事件型**通路 ——
    // 设备侧只在窗口内置 1，窗口一过就回到 0。末态恒 0 既可能是"通路断了"、
    // 也可能是"窗口已经过去"，两者在末态上**不可区分**。这正是 A1 这类
    // "写了但不生效"的缺口长期检不出来的原因（恒 0 的量无法用值验证通路）。
    // 所以判据取因果式：设备侧置 1 → EMS 侧必须看到 1，且约束必须落到指令上。
    int    forbid_chg_ticks = 0, forbid_dis_ticks = 0;
    double max_cmd_while_dis_forbidden = -1e18;   // 禁放期间的最大下发指令
    double min_cmd_while_chg_forbidden =  1e18;   // 禁充期间的最小下发指令
    int    ext_active_ticks = 0;                  // EXT 收紧了区间的拍数
    int    ext_zero_ticks   = 0;                  // 其中区间被收成 [0,0] 的拍数（停机/闭锁）
    {
        ems::Pacing pacer(dt, speed);
        for (int i = 0; i < steps; ++i) {
            const ems::StepRecord rec = app.step(dt);
            // EXT 外部设定（A3.2）：停机/闭锁应把区间收成 [0,0]。
            if (app.rt().last_verdict().ext_active) {
                ++ext_active_ticks;
                if (rec.p_lower == 0.0 && rec.p_upper == 0.0) ++ext_zero_ticks;
            }
            const ems::DeviceLimits& dl = app.rt().device_limits();
            if (dl.bms_dis_forbidden) {
                ++forbid_dis_ticks;
                max_cmd_while_dis_forbidden =
                    std::max(max_cmd_while_dis_forbidden, rec.p_cmd);
            }
            if (dl.bms_chg_forbidden) {
                ++forbid_chg_ticks;
                min_cmd_while_chg_forbidden =
                    std::min(min_cmd_while_chg_forbidden, rec.p_cmd);
            }
            pacer.wait();
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double wall_s = std::chrono::duration<double>(t1 - t0).count();

    std::printf("[EMS] done: wall %.2f s (%.0fx accelerated), final state %s, "
                "SOE %d events, stale %d\n",
                wall_s, steps * dt / (wall_s > 0 ? wall_s : 1e-9),
                app.rt().fsm().state_str(),
                (int)app.obs().soe().size(), app.io().stale_reads());

    // 联调检查单（EMS 侧口径）
    // 注：计数值要减掉**装配预热期**那部分 —— EMS 进程先起、设备侧后发布，
    // 预热期的读失败不属于控制窗口，算进去会让检查项永远失败。
    //
    // "零失败"这个口径在真·跨进程下给不出来：读点用 seqlock，撞上写者的临界区
    // 就返回 false（=请重试）。重试 64 次仍失败时走**设计内的降级路径**（保留上次
    // 有效值 + quality 置为不可信），这是可恢复降级、不是故障。写者若恰好被 OS
    // 抢在临界区里（Windows 时间片 15.6 ms），预算必然耗尽 —— 机器越忙越容易。
    // 所以判据取"重试吸收率"：降级必须远少于碰撞（≤1%），硬判据交给"值自洽"
    // 与下游的安全层（quality!=GOOD 时它们本来就拒绝用这个点）。
    const int stale_in_run = app.io().stale_reads() - stale0;
    const int coll_in_run  = app.io().collisions() - coll0;
    std::vector<ems::IntegrationCheck> checks;
    {
        const auto& tot = app.obs().totals();
        const double energy = tot.e_chg_kwh + tot.e_dis_kwh;
        checks.push_back({"通信建立", "一次侧参数来自设备侧 CFG（非点表默认）",
                          app.rt().device_limits().transformer_capacity_kw > 300.0,
                          "tr=" + std::to_string(app.rt().device_limits().transformer_capacity_kw)
                              + " kVA"});
        checks.push_back({"数据采集", "跨进程全表读取：碰撞被重试吸收（降级 ≤ 碰撞的 1%）",
                          stale_in_run * 100 <= coll_in_run,
                          "降级=" + std::to_string(stale_in_run) + " / 碰撞="
                              + std::to_string(coll_in_run) + " / 吸收="
                              + std::to_string(coll_in_run - stale_in_run)
                              + "（预热期 降级" + std::to_string(stale0) + " 碰撞"
                              + std::to_string(coll0) + "）"});
        checks.push_back({"控制指令", "指令逃逸（硬不变量）",
                          tot.out_of_interval_ticks == 0,
                          "out_of_interval=" + std::to_string(tot.out_of_interval_ticks)});
        checks.push_back({"控制指令", "设备侧实际出力非零（闭环真的在动，防开环）",
                          energy > 0.0, "充放能量=" + std::to_string(energy) + " kWh"});
        checks.push_back({"状态同步", "状态机迁移链完整",
                          app.rt().fsm().history().size() >= 3,
                          "transitions=" + std::to_string(app.rt().fsm().history().size())});
        checks.push_back({"状态同步", "末态非 FAULT/EMERGENCY",
                          app.rt().fsm().state() != ems::EmsState::kFault &&
                              app.rt().fsm().state() != ems::EmsState::kEmergency,
                          std::string("final=") + app.rt().fsm().state_str()});
        checks.push_back({"日志记录", "SOE 事件有界、零丢失",
                          app.obs().soe().dropped() == 0,
                          "size=" + std::to_string(app.obs().soe().size())});
        checks.push_back({"日志记录", "观察者步数与 log_every 解耦",
                          tot.steps == steps,
                          "steps=" + std::to_string(tot.steps) + "/" + std::to_string(steps)});

        // ---- BMS 禁充放：跨进程安全输入（A1，2026-09-19 补）----
        // 第 1 项是**反向守卫**：证明设备侧本轮真的发过禁充放位。没有它，
        // 第 2 项在"窗口根本没出现"时会因为 vacuously true 而假通过。
        checks.push_back({"安全约束", "BMS 禁充放位来自设备侧点表（非硬编码 false）",
                          forbid_dis_ticks > 0 || forbid_chg_ticks > 0,
                          "禁放拍数=" + std::to_string(forbid_dis_ticks)
                              + " / 禁充拍数=" + std::to_string(forbid_chg_ticks)});
        // 第 2 项才是真正的安全语义：限值必须一路走到**下发指令**上。
        // 判据 p_cmd ≤ 0：S01(kBmsForbid, L0) 在禁放时把 p_upper 收到 0，
        // 而仲裁只做"clip 到区间"，所以这是由架构保证的硬不变量。
        checks.push_back({"安全约束", "禁放期间下发指令不放电（p_cmd ≤ 0）",
                          forbid_dis_ticks > 0 && max_cmd_while_dis_forbidden <= 1e-6,
                          "禁放期最大指令=" + std::to_string(max_cmd_while_dis_forbidden)
                              + " kW"});

        // ---- EXT 外部设定（A3.2）：停机/闭锁应把区间收成 [0,0] ----
        // 反向守卫 ext_active_ticks > 0：证明本轮 EXT 真的动过（没动过就 vacuously 恒过）。
        checks.push_back({"外部设定", "EXT 停机/闭锁收成 [0,0]（ext_active → 区间 [0,0]）",
                          ext_active_ticks > 0 && ext_zero_ticks == ext_active_ticks,
                          "ext_active=" + std::to_string(ext_active_ticks)
                              + " / [0,0]拍=" + std::to_string(ext_zero_ticks)});
    }

    {
        std::ofstream f(report_path.c_str());
        if (f.is_open()) {
            f << app.report_text();
            f << "\n联调检查单：\n" << ems::checks_text(checks);
        }
    }

    std::printf("%s", ems::checks_text(checks).c_str());
    rt_db_cleanup(&h);
    return 0;
}
