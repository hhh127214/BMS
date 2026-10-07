// =====================================================================
// 18/bench_soe_alarm.h — 基准 B4：SOE 写入 + 告警装配
//
// 被测对象：
//   · P2/src/soe.h        SoeLog::push       （事件记录，有界内存 + 抑制）
//   · 20/src/alarm_assembler.h AlarmAssembler::update（每拍条件评估）
//
// 为什么这两件事要一起基准：
//   它们是**同一条可观测性链路的上下游**，且都挂在每拍路径上：
//     P2 的 SoeLog 负责"记"，20/ 的装配器负责"判定该不该记"。
//   把两者的每拍代价钉住，才能说明"可观测性没有拖垮 10 Hz" ——
//   这正是 P2 立项目标（可观测性是一等公民）必须付的账。
//
// 已知热点（写进报告）：AlarmAssembler::eval_conditions 对**每条**潜在
//   条件都无条件调用 snprintf 构造消息串（即使条件不成立）。所以它的
//   p99 主要由 ~10 次 snprintf 决定，而不是由判定逻辑决定。
//
// 只引用 P2/ 与 20/ 的头文件（-I ../P2/src -I ../20/src），不改它们。
// =====================================================================

#pragma once

#include "perf_clock.h"
#include "perf_stats.h"

#include "alarm_assembler.h"   // 20/
#include "alarm_model.h"       // 20/
#include "soe.h"               // P2/

#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace ems {
namespace perf {

struct SoeBenchArgs {
    int  iters            = 20000;
    int  warmup           = 200;
    bool merge_mode       = false;   // true = 同键重复（抑制路径）；false = 新键（插入路径）
    std::function<double(int)> delay_per_iter_us;
};

struct SoeBenchResult {
    std::vector<double> push_us;
    size_t size    = 0;
    size_t dropped = 0;
    size_t total   = 0;
    size_t suppressed = 0;
    double sink = 0.0;
};

inline SoeBenchResult run_soe_bench(const SoeBenchArgs& a) {
    SoeBenchResult r;
    r.push_us.reserve((size_t)a.iters);
    SoeLog log;

    for (int i = 0; i < a.iters; ++i) {
        SoeEvent ev;
        ev.t      = (double)i * 0.1;
        ev.level  = (i & 3) ? SoeLevel::kWarn : SoeLevel::kError;
        ev.source = SoeSource::kSafety;
        ev.code   = (a.merge_mode) ? SoeCode::kSafetyClipStart
                                   : (SoeCode)(200 + (i % 20));   // 轮换 20 个键
        ev.message = "bench event";
        ev.suppressible = a.merge_mode;   // 新键模式不参与抑制
        const double t0 = now_us();
        const bool created = log.push(ev);
        // 注入延迟在**计时区间内**（否则测的是"调用方变慢"，反向验证失效）
        if (a.delay_per_iter_us) {
            const double d = a.delay_per_iter_us(i);
            if (d > 0.0) busy_wait_us(d);
        }
        const double t1 = now_us();
        r.push_us.push_back(t1 - t0);
        r.sink += created ? 1.0 : 0.0;
    }
    r.size       = log.size();
    r.dropped    = log.dropped();
    r.total      = log.total();
    r.suppressed = log.suppressed();
    return r;
}

// 容量有界性（SLA-S2）：推 3*capacity 个互异键，size 必须封顶在 capacity
inline bool soe_capacity_bounded(size_t capacity, size_t* out_size,
                                 size_t* out_dropped) {
    SoeConfig cfg;
    cfg.capacity = capacity;
    cfg.suppress_enabled = false;      // 互异键本就不会被合并；显式关掉更干净
    SoeLog log(cfg);
    const size_t n = capacity * 3;
    for (size_t i = 0; i < n; ++i) {
        SoeEvent ev;
        ev.t = (double)i;
        ev.level = SoeLevel::kInfo;
        ev.source = SoeSource::kSystem;
        // 用 message 区分键：key() 由 (source, code) 组成，故轮换 code 域
        ev.code = (SoeCode)(1000 + (int)(i % 5000));
        ev.message = "cap test";
        ev.suppressible = false;
        log.push(ev);
    }
    if (out_size)    *out_size    = log.size();
    if (out_dropped) *out_dropped = log.dropped();
    return log.size() <= capacity;
}

// ---------------------------------------------------------------
// 告警装配
// ---------------------------------------------------------------
struct AlarmBenchArgs {
    int  iters   = 20000;
    int  warmup  = 200;
    bool with_alarms = true;    // true → 有若干告警成立（走 SET/CLEAR 路径）
    std::function<double(int)> delay_per_iter_us;
};

struct AlarmBenchResult {
    std::vector<double> update_us;
    size_t records = 0;
    size_t events  = 0;
    double sink = 0.0;
};

inline AlarmBenchResult run_alarm_bench(const AlarmBenchArgs& a) {
    AlarmBenchResult r;
    r.update_us.reserve((size_t)a.iters);

    AlarmContext ctx;
    ctx.transformer_capacity_kw = 500.0;
    ctx.demand_target_kw        = 400.0;
    ctx.forbid_reverse          = true;
    ctx.grid_min_kw             = 0.0;
    AlarmAssembler asm_(ctx);

    AlarmInput cur;
    AlarmInput prev;
    cur.t = 0.0;

    for (int i = 0; i < a.iters; ++i) {
        prev = cur;
        cur.t = (double)i * 0.1;
        // 状态在 NORMAL / DERATED 之间偶尔迁移（点事件）
        const bool trans = (i % 500 == 0) && i > 0;
        cur.has_transition = trans;
        cur.state_from = EmsState::kNormal;
        cur.state_to   = trans ? EmsState::kDerated : EmsState::kNormal;
        cur.state      = cur.state_to;
        cur.transition_reason = trans ? "bench" : "";

        cur.soc           = 0.5 + 0.02 * std::sin(i * 0.01);
        cur.temperature_c = 30.0 + 8.0 * std::sin(i * 0.003);
        cur.p_grid_kw     = 300.0 + 120.0 * std::sin(i * 0.02);
        cur.p_load_kw     = 320.0;
        if (a.with_alarms) {
            // 每 300 拍造一次变压器过载 + 逆流，走 SET/CLEAR
            if ((i / 300) % 2 == 1) {
                cur.p_grid_kw = 520.0;
                cur.temperature_c = 47.0;
            }
        }
        const double t0 = now_us();
        std::vector<AlarmEvent> evs = asm_.update(cur, &prev);
        // 注入延迟在计时区间内
        if (a.delay_per_iter_us) {
            const double d = a.delay_per_iter_us(i);
            if (d > 0.0) busy_wait_us(d);
        }
        const double t1 = now_us();
        r.update_us.push_back(t1 - t0);
        r.events += evs.size();
        r.sink += (double)evs.size();
    }
    r.records = asm_.size();
    return r;
}

} // namespace perf
} // namespace ems
