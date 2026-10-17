// =====================================================================
// 18/bench_acquisition.h — 基准 B2：采集路径（含 seqlock 碰撞与重试）
//
// 被测对象：RtDbDeviceIO::read_snapshot()（07/src/rtdb/rtdb_device_io.h）
//
// 两种负载条件，必须分开报（这是本基准的关键）：
//   · **无竞争**：单线程读。RT_DB 的 seqlock 只在"读窗口叠在写窗口里"
//     时返回 false —— 顺序执行时永远撞不上，collisions()==0。
//     此时 stale==0 不能证明重试策略正确（条件没被触发）。
//   · **有竞争**：另起一个"设备进程"线程持续写全点表，制造真实碰撞。
//     这才是有区分度的条件：验证 64 次重试确实把碰撞吸收掉了。
//
// 反向守卫（约定 §3.2）：先断言 contended 下 collisions>0，
// 否则"重试吸收率"这条相对判据两边都是 0，骗得过任何断言。
// ★ 2026-10-17 补：光有守卫不够 —— 守卫本身会因"写者还没被调度"而假红。
//   故 run_acquisition_bench 在 start() 后**握手**等写者完成 ≥1 次全表写
//   （`PointWriterThread::batches`），把"夹具空转"从"随机假红"变成"不可能"。
//
// 注意：本基准是**有意**让一个写者线程占满一个核 —— 这不是"并发编译"，
// 而是被测对象的负载条件本身。报告里必须写清 p99 是在何种竞争下取得的。
// =====================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "perf_clock.h"
#include "perf_stats.h"

#include "device_io.h"          // 04/
#include "ems_point_table.h"    // 07/src/rtdb
#include "ems_rt_db_setup.h"    // 07/src/rtdb
#include "rt_db_api.h"          // 07/vendor/rt_db
#include "rtdb_device_io.h"     // 07/src/rtdb

#include <atomic>
#include <cmath>
#include <cstddef>
#include <thread>
#include <vector>

namespace ems {
namespace perf {

// ---------------------------------------------------------------
// 共享内存环境（建立段 + 点表，并写入一份合理的设备侧初值）
// ---------------------------------------------------------------
struct RtdbEnv {
    rt_db_handle_t handle{};
    bool           ok = false;
    bool           created = false;

    bool open() {
        ok = ems_rt_db_setup(/*reset=*/true, &created) && rt_db_init(&handle, nullptr);
        return ok;
    }

    // 设备侧初值：让 read_snapshot() 拿到物理上合理的一拍
    void seed_device_points() const {
        RtDbPointWriter w(const_cast<rt_db_handle_t*>(&handle));
        w.write(EMS_P_BAT,   50.0);
        w.write(EMS_P_PV,   140.0);
        w.write(EMS_P_LOAD, 320.0);
        w.write(EMS_P_GRID, 130.0);
        w.write(EMS_SOC,      0.55);
        w.write(EMS_T_C,      28.0);
        w.write(EMS_SOH,      1.0);
        w.write(EMS_CFG_STANDBY, 5.0);
        w.write(EMS_CFG_CAP_KWH, 1000.0);
        w.write(EMS_CFG_MAX_CHG, 250.0);
        w.write(EMS_CFG_MAX_DIS, 250.0);
        w.write(EMS_CFG_BMS_CHG_LIM, 250.0);
        w.write(EMS_CFG_BMS_DIS_LIM, 250.0);
        w.write(EMS_CFG_TR_KVA, 500.0);
        w.write(EMS_CFG_D_TARGET, 400.0);
        w.write(EMS_STA_BMS,    1.0);
        w.write(EMS_STA_PCS,    1.0);
        w.write(EMS_STA_METER,  1.0);
        w.write(EMS_STA_VALID,  1.0);
        w.write(EMS_STA_FAULT,  0.0);
        w.write(EMS_STA_OFFLINE,0.0);
        w.write(EMS_STA_BMS_CHG_FORBID, 0.0);
        w.write(EMS_STA_BMS_DIS_FORBID, 0.0);
    }
};

// 写者线程：模拟设备/SCADA 进程每拍写全点表（含时间戳更新 → 拉长临界区）
struct PointWriterThread {
    std::atomic<bool> stop{false};
    // 已完成的「全表写」批次数 —— **活进度**，供 run_acquisition_bench 握手用
    // （下面的 `writes` 只在退出时写一次，外部读不到中间态）
    std::atomic<long long> batches{0};
    std::thread       th;
    long long         writes = 0;

    void start() {
        stop.store(false);
        batches.store(0);
        th = std::thread([this]() {
            long long n = 0;
            double phase = 0.0;
            while (!stop.load(std::memory_order_relaxed)) {
                phase += 0.01;
                // 全表写（值在动，逼真）；RT_DB 每次写都更新 sequence + 时间戳
                for (std::size_t i = 0; i < EMS_POINT_COUNT; ++i) {
                    const double v = 100.0 + 50.0 * (double)((i % 7)) +
                                     std::sin(phase + (double)i);
                    rt_db_set_value(handle, i, v, ems::rtdb_quality::kGood);
                }
                ++n;
                batches.store(n, std::memory_order_release);
            }
            writes = n;
        });
    }
    void join() {
        if (th.joinable()) th.join();
    }

    rt_db_handle_t* handle = nullptr;
};

struct AcqBenchArgs {
    int  reads       = 20000;
    int  warmup      = 200;
    bool contended   = false;   // true → 起写者线程
    double dt_s      = 0.1;
};

struct AcqBenchResult {
    std::vector<double> read_us;
    int collisions = 0;
    int stale      = 0;
    int absorbed   = 0;
    bool ran       = false;
};

inline AcqBenchResult run_acquisition_bench(const RtdbEnv& env,
                                            const AcqBenchArgs& a) {
    AcqBenchResult r;
    if (!env.ok) return r;

    RtDbDeviceIO io(const_cast<rt_db_handle_t*>(&env.handle));
    RealtimeSnapshot snap;

    PointWriterThread writer;
    if (a.contended) {
        writer.handle = const_cast<rt_db_handle_t*>(&env.handle);
        writer.start();
        // ★ 握手：等写者线程**真的开始写**再进计时循环（2026-10-17）。
        //   原先 start() 后立刻开读 —— 机器被限频/繁忙时写者可能还没被调度，
        //   20000 次读跑完它一次都没写 ⇒ collisions==0 ⇒ SLA-A2 分母为 0
        //   （`[denom<=0 -> INVALID]`）、SLA-A3 的「竞争下确有碰撞」守卫失守。
        //   实测 8 次里出过 1 次（假红）。等到至少完成 1 次全表写，就把
        //   "竞争条件根本没被触发"这种**空转夹具**变成不可能。
        const double w0 = now_us();
        while (writer.batches.load(std::memory_order_acquire) < 1 &&
               now_us() - w0 < 2.0e6) {
            std::this_thread::yield();
        }
    }

    r.read_us.reserve((size_t)a.reads);
    const int c0 = io.collisions();
    const int s0 = io.stale_reads();
    double t = 0.0;
    for (int i = 0; i < a.reads; ++i) {
        t += a.dt_s;
        const double t0 = now_us();
        io.read_snapshot(t, snap);
        const double t1 = now_us();
        r.read_us.push_back(t1 - t0);
    }
    r.collisions = io.collisions() - c0;
    r.stale      = io.stale_reads() - s0;
    r.absorbed   = r.collisions - r.stale;

    if (a.contended) {
        writer.stop.store(true);
        writer.join();
    }
    r.ran = true;
    return r;
}

inline Summary summarize_acq(const AcqBenchResult& r, int warmup) {
    Policy p;
    p.warmup = (size_t)warmup;
    p.drop_outliers = false;
    return summarize(r.read_us, p);
}

} // namespace perf
} // namespace ems
