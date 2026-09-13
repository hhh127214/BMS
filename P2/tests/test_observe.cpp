// =====================================================================
// P2/ 单元测试 —— 产品化 P2：可观测性
//
//   T301 枚举：等级 / 来源 / 事件码 / 名称互转
//   T302 时间窗抑制：838 拍风暴 → 1 条 Start + 1 条 End
//   T303 抑制例外：状态迁移链不被合并（suppressible=false）
//   T304 有界内存：容量淘汰 + dropped 如实上报
//   T305 导出：CSV / JSON 转义与结构
//   T306 指标增量：counter / gauge / histogram（O(1)）
//   T307 指标与 log_every 无关（关键：cmd_travel / sign_flips / reversals）
//   T308 直方图分位（含 +Inf 桶回落）
//   T309 Prometheus 文本格式 + 名称净化
//   T310 跟踪等级过滤：故障级永不受限
//   T311 采样间隔：按子系统独立
//   T312 观察者端到端：24h 默认场景 + 硬不变量
//   T313 与 10/ 结果交叉校验（log_every=1 时逐拍口径一致）
//   T314 故障场景事件：PCS 故障 / 通信中断 → 置位/清除事件对
//   T315 观察者自身可观测：suppressed / dropped / filtered / passed
//
// 编译：g++ -std=c++17 -Wall -O2 -I src -I ../04/src -I ../05/src
//           -I ../06/src -I ../07/src -I ../08/src -I ../10/src
//           tests/test_observe.cpp -o build/test_observe.exe
// =====================================================================

#include "observe.h"
#include "sim_24h.h"          // 10/ 装配层：T313 交叉校验用

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

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

#define EXPECT_NEAR(a, b, tol)                                            \
    do {                                                                  \
        const double _a = (a), _b = (b), _t = (tol);                      \
        if (std::fabs(_a - _b) <= _t) {                                   \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : |" << #a << " - " << #b << "| = "            \
                      << std::fabs(_a - _b) << " > " << _t                \
                      << "  (" << _a << " vs " << _b << ")" << std::endl; \
        }                                                                 \
    } while (0)

#define EXPECT_STREQ(a, b)                                                \
    do {                                                                  \
        const std::string _a = (a), _b = (b);                             \
        if (_a == _b) {                                                   \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : \"" << _a << "\" != \"" << _b << "\""        \
                      << std::endl;                                       \
        }                                                                 \
    } while (0)

#define EXPECT_CONTAINS(hay, needle)                                      \
    do {                                                                  \
        const std::string _h = (hay), _n = (needle);                      \
        if (_h.find(_n) != std::string::npos) {                           \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : 未找到 \"" << _n << "\"" << std::endl;       \
        }                                                                 \
    } while (0)

// =====================================================================
// 共用装配：一个"能跑起来"的运行时
// =====================================================================
static void assemble(EmsRuntime& rt, const LoopConfig& lc,
                     const DeviceLimits& dl, const SafetyParams& sp) {
    rt.config() = lc;
    rt.configure_plant(PlantConfig{});
    rt.device_limits() = dl;      // 顺序要紧：configure_plant 内部会 refresh_device_limits
    rt.safety_params() = sp;
    rt.apply_configs();
}

// 抖动场景：负荷在充/放之间来回切，制造大量指令行程与方向反转。
// 用于 T307 —— 这类"逐拍抖动"正是降采样会吃掉的东西。
static void run_square_wave(int log_every, RuntimeObserver& obs,
                            LoopMetrics* naive, int n = 600) {
    EmsRuntime rt;
    LoopConfig lc;
    lc.dt_s = 1.0;
    lc.log_every = log_every;
    lc.enable_log = true;
    lc.demand_window_s = 300.0;
    lc.output_deadband_kw = 0.0;      // 关死区，让抖动完整地进到指令里
    lc.output_switch_delay = 1;
    DeviceLimits dl;
    SafetyParams sp;
    assemble(rt, lc, dl, sp);

    obs.start(0.0);
    for (int i = 0; i < n; ++i) {
        const bool hi = ((i / 5) % 2) == 0;
        rt.set_environment(hi ? 300.0 : 50.0, hi ? 50.0 : 300.0);
        StepRecord rec = rt.step(1.0);
        obs.on_step(rt, rec);
    }
    obs.stop(static_cast<double>(n));
    if (naive) *naive = rt.metrics();
}

// 按 10/ 的装配顺序建运行时，并把每拍 StepRecord 喂给观察者。
// 刻意与 run_sim_24h() 保持一致，否则 T313 的交叉校验没有意义。
static void run_24h_with_observer(const Sim24hConfig& cfg, RuntimeObserver& obs,
                                  ForecastSeries* fc_out = nullptr,
                                  LoopMetrics* naive_out = nullptr) {
    ForecastSeries fc = make_typical_day_curves(cfg.typical);
    if (fc_out) *fc_out = fc;

    EmsRuntime rt;
    rt.config().dt_s            = cfg.dt_s;
    rt.config().log_every       = cfg.log_every;
    rt.config().enable_log      = true;
    rt.config().demand_window_s = cfg.econ.demand_window_s;
    rt.configure_plant(cfg.plant);
    rt.device_limits()      = cfg.limits;
    rt.safety_params()      = cfg.safety;
    rt.coordinator_config() = cfg.coord;
    rt.fsm_config()         = cfg.fsm;
    rt.set_forecast(fc);
    rt.apply_configs();

    const int steps = static_cast<int>(std::lround(cfg.duration_s / cfg.dt_s));
    const double dt = cfg.dt_s;
    obs.start(0.0);
    for (int i = 0; i < steps; ++i) {
        const double t = static_cast<double>(i) * dt;
        double ld = 0.0, pv = 0.0;
        fc.sample(t, &ld, &pv, nullptr);
        rt.set_environment(ld, pv);
        bool any_fault = false;
        for (const auto& w : cfg.fault_windows) {
            const bool on = w.active(t);
            apply_fault(rt, w.kind, on);
            if (on) any_fault = true;
        }
        if (cfg.auto_restart_after_fault && !any_fault &&
            rt.fsm().state() == EmsState::kReady) {
            rt.fsm().request_run(true);
        }
        StepRecord rec = rt.step(dt);
        obs.on_step(rt, rec);
    }
    obs.stop(steps * dt);
    if (naive_out) *naive_out = rt.metrics();
}

// =====================================================================
// T301 枚举
// =====================================================================
static void t301_enums() {
    std::cout << "[T301] 枚举：等级 / 来源 / 事件码\n";

    EXPECT_STREQ(soe_level_name(SoeLevel::kDebug), "DEBUG");
    EXPECT_STREQ(soe_level_name(SoeLevel::kInfo),  "INFO");
    EXPECT_STREQ(soe_level_name(SoeLevel::kWarn),  "WARN");
    EXPECT_STREQ(soe_level_name(SoeLevel::kError), "ERROR");
    EXPECT_STREQ(soe_level_name(SoeLevel::kFatal), "FATAL");

    // 等级必须可比较（这是"用 enum 而不是 string"的全部意义）
    EXPECT(SoeLevel::kDebug < SoeLevel::kInfo);
    EXPECT(SoeLevel::kInfo  < SoeLevel::kWarn);
    EXPECT(SoeLevel::kWarn  < SoeLevel::kError);
    EXPECT(SoeLevel::kError < SoeLevel::kFatal);
    EXPECT(SoeLevel::kError >= SoeLevel::kError);

    SoeLevel lv = SoeLevel::kDebug;
    EXPECT(soe_level_from_name("ERROR", &lv));
    EXPECT(lv == SoeLevel::kError);
    EXPECT(soe_level_from_name("FATAL", &lv));
    EXPECT(lv == SoeLevel::kFatal);
    EXPECT(!soe_level_from_name("BOGUS", &lv));
    EXPECT(!soe_level_from_name("error", &lv));   // 大小写敏感
    EXPECT(!soe_level_from_name("WARN", nullptr)); // 空指针不崩

    EXPECT_STREQ(soe_source_name(SoeSource::kSystem),      "SYSTEM");
    EXPECT_STREQ(soe_source_name(SoeSource::kFsm),         "FSM");
    EXPECT_STREQ(soe_source_name(SoeSource::kSafety),      "SAFETY");
    EXPECT_STREQ(soe_source_name(SoeSource::kCoordinator), "COORD");
    EXPECT_STREQ(soe_source_name(SoeSource::kStrategy),    "STRATEGY");
    EXPECT_STREQ(soe_source_name(SoeSource::kDevice),      "DEVICE");
    EXPECT_STREQ(soe_source_name(SoeSource::kComm),        "COMM");
    EXPECT_STREQ(soe_source_name(SoeSource::kConfig),      "CONFIG");
    EXPECT_STREQ(soe_source_name(SoeSource::kEcon),        "ECON");
    EXPECT(kSoeSourceCount == 9);
    EXPECT(soe_source_index(SoeSource::kEcon) == 8);
    EXPECT(soe_source_index(SoeSource::kSystem) == 0);

    EXPECT_STREQ(soe_code_name(SoeCode::kFsmTransition),    "FSM_TRANSITION");
    EXPECT_STREQ(soe_code_name(SoeCode::kSafetyClipStart),  "SAFETY_CLIP_START");
    EXPECT_STREQ(soe_code_name(SoeCode::kPcsFaultSet),      "PCS_FAULT_SET");
    EXPECT_STREQ(soe_code_name(SoeCode::kInvariantBroken),  "INVARIANT_BROKEN");
    // 每个码都必须有名字（防止新增枚举忘了补 switch）
    for (int c : {100, 101, 102, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209,
                  210, 211, 212, 213, 214, 215, 300, 301, 302, 303, 304, 305, 306,
                  307, 308, 309, 310, 311, 400, 401, 402, 500, 501, 900, 901, 902, 903}) {
        const std::string n = soe_code_name(static_cast<SoeCode>(c));
        EXPECT(n != "?");
        EXPECT(!n.empty());
    }
    EXPECT_STREQ(soe_code_name(SoeCode::kNone), "NONE");

    SoeEvent ev;
    ev.source = SoeSource::kSafety;
    ev.code   = SoeCode::kSafetyClipStart;
    EXPECT_STREQ(ev.key(), "SAFETY:SAFETY_CLIP_START");
    EXPECT(!ev.suppressed());
    ev.repeat_count = 5;
    EXPECT(ev.suppressed());
    ev.first_t = 10.0; ev.last_t = 30.0;
    EXPECT_NEAR(ev.duration_s(), 20.0, 1e-12);
    ev.data = {{"p_grid", -7.9}, {"limit", 0.0}};
    EXPECT_NEAR(ev.get("p_grid"), -7.9, 1e-12);
    EXPECT_NEAR(ev.get("nope", 42.0), 42.0, 1e-12);
}

// =====================================================================
// T302 时间窗抑制
// =====================================================================
static void t302_suppression() {
    std::cout << "[T302] 时间窗抑制：838 拍风暴 → 1 条 Start + 1 条 End\n";

    SoeConfig c;
    c.suppress_window_s = 5.0;
    c.suppress_enabled  = true;
    SoeLog log(c);

    // 838 拍连续限幅：每拍都"发生"，但物理上是同一件事
    for (int i = 0; i < 838; ++i) {
        SoeEvent ev;
        ev.t      = static_cast<double>(i);
        ev.level  = SoeLevel::kWarn;
        ev.source = SoeSource::kSafety;
        ev.code   = SoeCode::kSafetyClipStart;
        ev.message = "安全限幅开始";
        ev.data = {{"p_grid", -7.0 - 0.001 * i}};   // 幅度逐拍微增
        log.push(ev);
    }
    log.add(838.0, SoeLevel::kInfo, SoeSource::kSafety, SoeCode::kSafetyClipEnd,
            "安全限幅结束");

    EXPECT(log.size() == 2);                 // 838 + 1 次 push → 只有 2 条
    EXPECT(log.total() == 839);
    EXPECT(log.suppressed() == 837);         // 838 条合并掉 837 条
    EXPECT(log.dropped() == 0);

    const SoeEvent* st = log.find(SoeCode::kSafetyClipStart);
    EXPECT(st != nullptr);
    if (st) {
        EXPECT(st->repeat_count == 838);
        EXPECT_NEAR(st->first_t, 0.0, 1e-12);
        EXPECT_NEAR(st->last_t, 837.0, 1e-12);
        EXPECT_NEAR(st->duration_s(), 837.0, 1e-12);
        // 抑制不能丢峰值：合并后 p_grid 应保留幅度最大的那个
        EXPECT_NEAR(st->get("p_grid"), -7.0 - 0.001 * 837, 1e-9);
    }
    EXPECT(log.has(SoeCode::kSafetyClipEnd));
    EXPECT(log.total_occurrences(SoeCode::kSafetyClipStart) == 838);
    EXPECT(log.count(SoeCode::kSafetyClipStart) == 1);

    // ---- 窗口边界：超出窗口必须新开一条 ----
    SoeLog log2(c);
    for (double t : {0.0, 3.0, 6.0, 20.0}) {   // 间隔 3s(合并) / 3s(合并) / 14s(新开)
        SoeEvent ev;
        ev.t = t; ev.level = SoeLevel::kWarn;
        ev.source = SoeSource::kSafety; ev.code = SoeCode::kSafetyClipStart;
        ev.message = "clip";
        log2.push(ev);
    }
    EXPECT(log2.size() == 2);
    EXPECT(log2.total_occurrences(SoeCode::kSafetyClipStart) == 4);

    // ---- 关掉抑制 → 逐条记录 ----
    SoeConfig off = c;
    off.suppress_enabled = false;
    SoeLog log3(off);
    for (int i = 0; i < 10; ++i) {
        log3.add(static_cast<double>(i), SoeLevel::kWarn, SoeSource::kSafety,
                 SoeCode::kSafetyClipStart, "clip");
    }
    EXPECT(log3.size() == 10);
    EXPECT(log3.suppressed() == 0);

    // ---- 不同 (source, code) 互不干扰 ----
    SoeLog log4(c);
    log4.add(0.0, SoeLevel::kWarn, SoeSource::kSafety, SoeCode::kSafetyClipStart, "a");
    log4.add(0.5, SoeLevel::kWarn, SoeSource::kSafety, SoeCode::kTrOverloadStart, "b");
    log4.add(1.0, SoeLevel::kWarn, SoeSource::kEcon,   SoeCode::kSafetyClipStart, "c");
    EXPECT(log4.size() == 3);
}

// =====================================================================
// T303 抑制例外：状态迁移链
// =====================================================================
static void t303_no_merge_transitions() {
    std::cout << "[T303] 状态迁移链不被抑制\n";

    SoeConfig c;
    c.suppress_window_s = 60.0;     // 窗口故意开得很大
    SoeLog log(c);

    // A→B→C：三次迁移在 1 秒内发生。若被合并就丢掉迁移链（只剩 A 重复 3 次）
    const char* chain[3] = {"INIT → READY", "READY → NORMAL", "NORMAL → DERATED"};
    for (int i = 0; i < 3; ++i) {
        SoeEvent ev;
        ev.t = static_cast<double>(i) * 0.5;
        ev.level = SoeLevel::kInfo;
        ev.source = SoeSource::kFsm;
        ev.code = SoeCode::kFsmTransition;
        ev.message = chain[i];
        ev.suppressible = false;         // ★ 关键
        log.push(ev);
    }
    EXPECT(log.size() == 3);
    EXPECT(log.suppressed() == 0);
    EXPECT_STREQ(log.events()[0].message, chain[0]);
    EXPECT_STREQ(log.events()[1].message, chain[1]);
    EXPECT_STREQ(log.events()[2].message, chain[2]);
    for (const auto& e : log.events()) EXPECT(e.repeat_count == 1);

    // 对照：同样的输入，若允许抑制 → 合并成 1 条
    SoeLog log2(c);
    for (int i = 0; i < 3; ++i) {
        SoeEvent ev;
        ev.t = static_cast<double>(i) * 0.5;
        ev.level = SoeLevel::kInfo; ev.source = SoeSource::kFsm;
        ev.code = SoeCode::kFsmTransition;
        ev.message = chain[i];
        ev.suppressible = true;
        log2.push(ev);
    }
    EXPECT(log2.size() == 1);
    EXPECT(log2.suppressed() == 2);
    // 合并后只剩第一条的 message —— 这正是"不能合并迁移"的原因
    EXPECT_STREQ(log2.events()[0].message, chain[0]);
}

// =====================================================================
// T304 有界内存
// =====================================================================
static void t304_bounded_memory() {
    std::cout << "[T304] 有界内存：容量淘汰 + dropped 上报\n";

    SoeConfig c;
    c.capacity = 100;
    c.suppress_enabled = false;      // 关掉抑制，才能造出 150 条独立条目
    SoeLog log(c);

    for (int i = 0; i < 150; ++i) {
        log.add(static_cast<double>(i), SoeLevel::kInfo, SoeSource::kSystem,
                SoeCode::kFsmTransition, "e" + std::to_string(i));
    }
    EXPECT(log.size() == 100);
    EXPECT(log.dropped() == 50);
    EXPECT(log.overflowed());
    EXPECT(log.total() == 150);

    // 淘汰的是**最旧**的：保留 e50..e149
    const auto v = log.events();
    EXPECT_STREQ(v.front().message, "e50");
    EXPECT_STREQ(v.back().message, "e149");

    // 容量足够时不淘汰
    SoeConfig big;
    big.capacity = 4096;
    SoeLog log2(big);
    for (int i = 0; i < 150; ++i) {
        log2.add(static_cast<double>(i), SoeLevel::kInfo, SoeSource::kSystem,
                 SoeCode::kFsmTransition, "x");
    }
    EXPECT(log2.size() == 1);        // 抑制开着 → 全合并
    EXPECT(log2.dropped() == 0);
    EXPECT(!log2.overflowed());

    // reset 清零全部计数
    log.reset();
    EXPECT(log.size() == 0);
    EXPECT(log.dropped() == 0);
    EXPECT(log.total() == 0);
    EXPECT(log.suppressed() == 0);
}

// =====================================================================
// T305 导出
// =====================================================================
static void t305_export() {
    std::cout << "[T305] 导出：CSV / JSON\n";

    SoeLog log;
    log.add(1.5, SoeLevel::kError, SoeSource::kComm, SoeCode::kCommLostPcs,
            "PCS 通信丢失", {{"fault_bits", 2.0}});
    // 带引号与换行的消息，验证转义
    log.add(2.5, SoeLevel::kWarn, SoeSource::kSafety, SoeCode::kSafetyClipStart,
            "含\"引号\"与\n换行", {{"p_grid", -12.5}});
    log.add(3.5, SoeLevel::kFatal, SoeSource::kSystem, SoeCode::kInvariantBroken,
            "指令逃逸");

    const std::string csv = log.to_csv();
    EXPECT_CONTAINS(csv, SoeLog::csv_header());
    EXPECT_CONTAINS(csv, "t_first,t_last,duration_s,repeat,level,source,code,message,data");
    EXPECT_CONTAINS(csv, "ERROR,COMM,COMM_LOST_PCS");
    EXPECT_CONTAINS(csv, "fault_bits=2");
    EXPECT_CONTAINS(csv, "p_grid=-12.5");
    // CSV 里的双引号必须成对转义；换行必须转成字面量 \n，保证"一事件一行"
    EXPECT_CONTAINS(csv, "\"含\"\"引号\"\"与\\n换行\"");
    EXPECT(csv.find("含\"引号\"与\n换行") == std::string::npos);
    // 表头 1 行 + 3 条数据（事件里的换行不得把行数撑开）
    size_t lines = 0;
    for (char ch : csv) if (ch == '\n') ++lines;
    EXPECT(lines == 4);

    const std::string js = log.to_json();
    EXPECT_CONTAINS(js, "\"summary\"");
    EXPECT_CONTAINS(js, "\"events\"");
    EXPECT_CONTAINS(js, "\"code\": \"COMM_LOST_PCS\"");
    EXPECT_CONTAINS(js, "\"level\": \"FATAL\"");
    EXPECT_CONTAINS(js, "\"total_pushed\": 3");
    EXPECT_CONTAINS(js, "\"stored\": 3");
    // JSON 里换行必须转义成 \n
    EXPECT_CONTAINS(js, "含\\\"引号\\\"与\\n换行");
    EXPECT(js.find("含\"引号\"与\n换行") == std::string::npos);

    const std::string sm = log.summary_text();
    EXPECT_CONTAINS(sm, "存储 3 条");
    EXPECT_CONTAINS(sm, "累计 3 次");
    EXPECT_CONTAINS(sm, "FATAL: 1");
    EXPECT_CONTAINS(sm, "ERROR: 1");

    // ---- 过滤 / 查询 ----
    EXPECT(log.count(SoeLevel::kError) == 1);
    EXPECT(log.count_at_least(SoeLevel::kWarn) == 3);
    EXPECT(log.count_at_least(SoeLevel::kFatal) == 1);
    EXPECT(log.count(SoeSource::kComm) == 1);
    EXPECT(log.count(SoeCode::kSafetyClipStart) == 1);
    EXPECT(log.filter_level(SoeLevel::kWarn).size() == 3);
    EXPECT(log.filter_level(SoeLevel::kFatal).size() == 1);
    EXPECT(log.filter_source(SoeSource::kSafety).size() == 1);
    EXPECT(log.filter_range(2.0, 3.0).size() == 1);
    EXPECT(log.filter_range(0.0, 100.0).size() == 3);
    EXPECT(log.last(SoeCode::kInvariantBroken) != nullptr);
    EXPECT(log.find(SoeCode::kNone) == nullptr);
}

// =====================================================================
// T306 指标增量
// =====================================================================
static void t306_metrics() {
    std::cout << "[T306] 指标：counter / gauge / histogram\n";

    MetricRegistry m;
    EXPECT(m.size() == 0);

    m.counter("c1", "计数器", "1");
    m.inc("c1");
    m.inc("c1");
    m.inc("c1", 3.0);
    EXPECT_NEAR(m.value("c1"), 5.0, 1e-12);
    EXPECT(m.has("c1"));

    m.gauge("g1", "表计", "kW");
    m.set("g1", 10.0);
    m.set("g1", 4.0);
    m.set("g1", 25.0);
    EXPECT_NEAR(m.value("g1"), 25.0, 1e-12);
    EXPECT_NEAR(m.min_of("g1"), 4.0, 1e-12);      // gauge 记极值
    EXPECT_NEAR(m.max_of("g1"), 25.0, 1e-12);

    m.histogram("h1", {1, 2, 5, 10}, "直方图", "kW");
    for (double v : {0.5, 1.5, 1.5, 7.0, 100.0}) m.observe("h1", v);
    EXPECT(m.count_of("h1") == 5);
    EXPECT_NEAR(m.mean_of("h1"), (0.5 + 1.5 + 1.5 + 7.0 + 100.0) / 5.0, 1e-9);
    EXPECT_NEAR(m.min_of("h1"), 0.5, 1e-12);
    EXPECT_NEAR(m.max_of("h1"), 100.0, 1e-12);

    // 幂等注册：同名重复注册只更新 help/unit，不新增
    const size_t n_before = m.size();
    m.counter("c1", "新说明", "1");
    EXPECT(m.size() == n_before);
    EXPECT(m.find("c1") != nullptr);
    EXPECT_STREQ(m.find("c1")->help, "新说明");

    // 未注册的名字：读返回默认值，不崩
    EXPECT_NEAR(m.value("nope", -1.0), -1.0, 1e-12);
    EXPECT(m.count_of("nope") == 0);
    EXPECT(m.find("nope") == nullptr);
    EXPECT(!m.has("nope"));

    // reset 清空
    m.reset();
    EXPECT(m.size() == 0);
    EXPECT(!m.has("c1"));
}

// =====================================================================
// T307 指标与 log_every 无关（核心）
// =====================================================================
static void t307_decoupling() {
    std::cout << "[T307] 指标与 log_every 无关（逐拍精确）\n";

    RuntimeObserver o1, o10;
    LoopMetrics n1, n10;
    run_square_wave(1,  o1,  &n1);
    run_square_wave(10, o10, &n10);

    // ① 控制行为本身不受 log_every 影响 → 两拍的样本数一致
    EXPECT(o1.totals().steps == 600);
    EXPECT(o10.totals().steps == 600);

    // ② ★ 观察者的输出质量指标**完全一致**（逐拍累积，不经过日志）
    EXPECT_NEAR(o1.totals().cmd_travel_kw, o10.totals().cmd_travel_kw, 1e-9);
    EXPECT(o1.totals().sign_flips    == o10.totals().sign_flips);
    EXPECT(o1.totals().cmd_reversals == o10.totals().cmd_reversals);
    EXPECT_NEAR(o1.totals().max_abs_cmd_kw, o10.totals().max_abs_cmd_kw, 1e-9);
    EXPECT_NEAR(o1.totals().e_import_kwh, o10.totals().e_import_kwh, 1e-9);
    EXPECT_NEAR(o1.totals().e_chg_kwh,    o10.totals().e_chg_kwh, 1e-9);
    EXPECT_NEAR(o1.totals().e_dis_kwh,    o10.totals().e_dis_kwh, 1e-9);

    // 这个场景确实有抖动（否则测试没意义）
    EXPECT(o1.totals().cmd_travel_kw > 1000.0);
    EXPECT(o1.totals().cmd_reversals > 50);

    // ③ 对照：log_every=1 时，观察者与 07/ 的逐拍口径**一致**
    EXPECT_NEAR(o1.totals().cmd_travel_kw, n1.cmd_travel_kw, 1e-6);
    EXPECT(o1.totals().sign_flips    == n1.sign_flips);
    EXPECT(o1.totals().cmd_reversals == n1.cmd_reversals);

    // ④ ★ 对照：log_every=10 时，07/ 的日志口径**失明**，观察者不受影响
    EXPECT(n10.cmd_travel_kw < 1e-9);            // 降采样后行程被吃干净
    EXPECT(o10.totals().cmd_travel_kw > 1000.0); // 观察者照旧
    EXPECT(n10.cmd_reversals == 0);
    EXPECT(o10.totals().cmd_reversals > 50);
    // 差距有多大：至少三个数量级
    EXPECT(o10.totals().cmd_travel_kw > n10.cmd_travel_kw + 1000.0);
    EXPECT(n10.samples * 10 == n1.samples);      // 日志确实被降采样了

    // ⑤ 硬不变量与 log_every 无关
    EXPECT(o1.totals().out_of_interval_ticks == o10.totals().out_of_interval_ticks);
    EXPECT(o1.totals().over_limit_ticks      == o10.totals().over_limit_ticks);
    EXPECT(o1.totals().gated_nonzero_ticks   == o10.totals().gated_nonzero_ticks);
    EXPECT(o1.totals().hard_invariants_ok());

    // ⑥ 观察者自己注册的指标也要与 log_every 无关
    EXPECT_NEAR(o1.metrics().value("ems_cmd_travel_kw"),
                o10.metrics().value("ems_cmd_travel_kw"), 1e-9);
    EXPECT(o1.metrics().value("ems_steps_total") == 600.0);
    EXPECT(o10.metrics().value("ems_steps_total") == 600.0);
}

// =====================================================================
// T308 直方图分位
// =====================================================================
static void t308_quantile() {
    std::cout << "[T308] 直方图分位（含 +Inf 桶回落）\n";

    MetricRegistry m;
    m.histogram("h", {1, 2, 5, 10});
    for (double v : {1.0, 1.0, 1.0, 10.0, 10.0}) m.observe("h", v);

    EXPECT(m.count_of("h") == 5);
    EXPECT_NEAR(m.mean_of("h"), 23.0 / 5.0, 1e-9);
    // 3 个样本落在 (0,1]，p50 应回落到桶上界 1
    EXPECT_NEAR(m.quantile("h", 0.50), 1.0, 1e-12);
    // p90 需要覆盖 4.5 个样本 → 落在 (5,10]
    EXPECT_NEAR(m.quantile("h", 0.90), 10.0, 1e-12);
    EXPECT_NEAR(m.quantile("h", 1.00), 10.0, 1e-12);

    // 超出最大桶 → 落 +Inf，分位用观测最大值近似（而不是返回 0 或瞎报桶上界）
    m.observe("h", 100.0);
    EXPECT(m.count_of("h") == 6);
    EXPECT_NEAR(m.quantile("h", 1.00), 100.0, 1e-12);
    EXPECT_NEAR(m.max_of("h"), 100.0, 1e-12);

    // 空直方图：不崩，返回默认值
    MetricRegistry m2;
    m2.histogram("e", {1, 2});
    EXPECT(m2.count_of("e") == 0);
    EXPECT_NEAR(m2.quantile("e", 0.5, -7.0), -7.0, 1e-12);

    // 单桶边界：v == bounds[0] 应计入该桶（le 语义）
    MetricRegistry m3;
    m3.histogram("b", {5.0});
    m3.observe("b", 5.0);
    EXPECT_NEAR(m3.quantile("b", 1.0), 5.0, 1e-12);
    m3.observe("b", 5.0001);
    EXPECT_NEAR(m3.quantile("b", 1.0), 5.0001, 1e-9);
}

// =====================================================================
// T309 Prometheus 格式
// =====================================================================
static void t309_prometheus() {
    std::cout << "[T309] Prometheus 文本格式\n";

    MetricRegistry m;
    m.counter("ems_steps_total", "闭环步数", "1");
    m.inc("ems_steps_total", 86400);
    m.gauge("ems_soc", "SOC", "1");
    m.set("ems_soc", 0.5);
    m.histogram("ems_track_error_kw", {1, 10}, "跟踪误差", "kW");
    for (double v : {0.5, 5.0, 50.0}) m.observe("ems_track_error_kw", v);

    const std::string p = m.to_prometheus();
    EXPECT_CONTAINS(p, "# HELP ems_steps_total 闭环步数");
    EXPECT_CONTAINS(p, "# TYPE ems_steps_total counter");
    EXPECT_CONTAINS(p, "ems_steps_total 86400");
    EXPECT_CONTAINS(p, "# TYPE ems_soc gauge");
    EXPECT_CONTAINS(p, "ems_soc 0.5");
    EXPECT_CONTAINS(p, "# TYPE ems_track_error_kw histogram");
    EXPECT_CONTAINS(p, "ems_track_error_kw_bucket{le=\"1\"} 1");
    EXPECT_CONTAINS(p, "ems_track_error_kw_bucket{le=\"10\"} 2");
    EXPECT_CONTAINS(p, "ems_track_error_kw_bucket{le=\"+Inf\"} 3");
    EXPECT_CONTAINS(p, "ems_track_error_kw_sum 55.5");
    EXPECT_CONTAINS(p, "ems_track_error_kw_count 3");

    // 名称净化：非法字符替换成下划线，非法首字符补前缀
    MetricRegistry m2;
    m2.gauge("weird-name.with space");
    m2.set("weird-name.with space", 1.0);
    const std::string p2 = m2.to_prometheus();
    EXPECT_CONTAINS(p2, "weird_name_with_space 1");
    EXPECT(p2.find("weird-name") == std::string::npos);

    MetricRegistry m3;
    m3.gauge("9leading");
    m3.set("9leading", 1.0);
    EXPECT_CONTAINS(m3.to_prometheus(), "m_9leading 1");

    // JSON 导出
    const std::string j = m.to_json();
    EXPECT_CONTAINS(j, "\"ems_steps_total\"");
    EXPECT_CONTAINS(j, "\"type\": \"counter\"");
    EXPECT_CONTAINS(j, "\"value\": 86400");
    EXPECT_CONTAINS(j, "\"unit\": \"kW\"");
    EXPECT_CONTAINS(j, "\"p50\"");
    EXPECT_CONTAINS(j, "\"p99\"");
    EXPECT_CONTAINS(j, "\"count\": 3");
}

// =====================================================================
// T310 跟踪等级过滤
// =====================================================================
static void t310_level_filter() {
    std::cout << "[T310] 跟踪等级过滤：故障级永不受限\n";

    TraceControl tc;
    EXPECT(tc.global() == SoeLevel::kInfo);

    tc.set_global(SoeLevel::kWarn);
    EXPECT(tc.enabled(SoeSource::kSafety, SoeLevel::kInfo) == false);
    EXPECT(tc.enabled(SoeSource::kSafety, SoeLevel::kWarn) == true);
    EXPECT(tc.enabled(SoeSource::kSafety, SoeLevel::kError) == true);

    // ★ 即使全局调到 FATAL，ERROR/FAULT 级也必须照常输出
    tc.set_global(SoeLevel::kFatal);
    EXPECT(tc.enabled(SoeSource::kSafety, SoeLevel::kError) == true);
    EXPECT(tc.enabled(SoeSource::kSafety, SoeLevel::kFatal) == true);
    EXPECT(tc.enabled(SoeSource::kSafety, SoeLevel::kWarn) == false);
    EXPECT(tc.enabled(SoeSource::kComm,   SoeLevel::kDebug) == false);

    // 按子系统覆盖：只放开 STRATEGY，其他不受影响
    tc.set_level(SoeSource::kStrategy, SoeLevel::kDebug);
    EXPECT(tc.level(SoeSource::kStrategy) == SoeLevel::kDebug);
    EXPECT(tc.enabled(SoeSource::kStrategy, SoeLevel::kDebug) == true);
    EXPECT(tc.enabled(SoeSource::kSafety,   SoeLevel::kDebug) == false);
    EXPECT(tc.level(SoeSource::kSafety) == SoeLevel::kFatal);   // 继承全局

    tc.clear_level(SoeSource::kStrategy);
    EXPECT(tc.level(SoeSource::kStrategy) == SoeLevel::kFatal);
    EXPECT(tc.enabled(SoeSource::kStrategy, SoeLevel::kDebug) == false);

    // allow()：判定 + 统计一体
    TraceControl tc2;
    tc2.set_global(SoeLevel::kWarn);
    EXPECT(tc2.allow(SoeSource::kSafety, SoeLevel::kInfo) == false);
    EXPECT(tc2.filtered(SoeSource::kSafety) == 1);
    EXPECT(tc2.passed(SoeSource::kSafety) == 0);
    EXPECT(tc2.allow(SoeSource::kSafety, SoeLevel::kWarn) == true);
    EXPECT(tc2.passed(SoeSource::kSafety) == 1);
    EXPECT(tc2.allow(SoeSource::kSafety, SoeLevel::kError) == true);
    EXPECT(tc2.passed(SoeSource::kSafety) == 2);
    EXPECT(tc2.filtered_total() == 1);
    EXPECT(tc2.passed_total() == 2);

    const std::string sm = tc2.summary_text();
    EXPECT_CONTAINS(sm, "TRACE");
    EXPECT_CONTAINS(sm, "输出 2 条");
    EXPECT_CONTAINS(sm, "过滤 1 条");

    const std::string js = tc.to_json();
    EXPECT_CONTAINS(js, "\"global\": \"FATAL\"");
    EXPECT_CONTAINS(js, "\"per_source\"");
}

// =====================================================================
// T311 采样间隔
// =====================================================================
static void t311_sampling() {
    std::cout << "[T311] 采样间隔（按子系统独立）\n";

    TraceControl tc;
    tc.set_sample_every(SoeSource::kSafety, 10);
    EXPECT(tc.sample_every(SoeSource::kSafety) == 10);

    int hit = 0;
    for (int i = 0; i < 30; ++i) if (tc.sample(SoeSource::kSafety)) ++hit;
    EXPECT(hit == 3);       // 每 10 拍一次

    // 其他子系统不受影响
    int other = 0;
    for (int i = 0; i < 30; ++i) if (tc.sample(SoeSource::kComm)) ++other;
    EXPECT(other == 30);

    // 0 / 1 都表示不采样（每拍都通过）
    tc.set_sample_every(SoeSource::kSafety, 0);
    hit = 0;
    for (int i = 0; i < 5; ++i) if (tc.sample(SoeSource::kSafety)) ++hit;
    EXPECT(hit == 5);
    tc.set_sample_every(SoeSource::kSafety, 1);
    hit = 0;
    for (int i = 0; i < 5; ++i) if (tc.sample(SoeSource::kSafety)) ++hit;
    EXPECT(hit == 5);

    // 负数按 0 处理
    tc.set_sample_every(SoeSource::kSafety, -3);
    EXPECT(tc.sample_every(SoeSource::kSafety) == 0);

    // allow()：只有 kDebug 级才走采样；事件级（>=INFO）不受采样影响
    TraceControl tc2;
    tc2.set_global(SoeLevel::kDebug);
    tc2.set_sample_every(SoeSource::kSafety, 100);
    int dbg = 0, inf = 0;
    for (int i = 0; i < 10; ++i) {
        if (tc2.allow(SoeSource::kSafety, SoeLevel::kDebug)) ++dbg;
        if (tc2.allow(SoeSource::kSafety, SoeLevel::kInfo))  ++inf;
    }
    EXPECT(dbg == 0);      // 10 拍还没到第 100 拍
    EXPECT(inf == 10);     // 事件级不受采样影响
    EXPECT(tc2.passed(SoeSource::kSafety) == 10);
    EXPECT(tc2.filtered(SoeSource::kSafety) == 10);
}

// =====================================================================
// T312 观察者端到端：24h 默认场景
// =====================================================================
static void t312_end_to_end() {
    std::cout << "[T312] 观察者端到端：24h 默认场景\n";

    Sim24hConfig cfg = make_default_24h_config();
    RuntimeObserver obs;
    run_24h_with_observer(cfg, obs);

    const ObserverTotals& t = obs.totals();
    EXPECT(t.steps == 86400);
    EXPECT_NEAR(t.duration_s, 86399.0, 1e-9);

    // 硬不变量：任何场景、任何时刻都不得违反
    EXPECT(t.out_of_interval_ticks == 0);
    EXPECT(t.over_limit_ticks == 0);
    EXPECT(t.gated_nonzero_ticks == 0);
    EXPECT(t.hard_invariants_ok());

    // 物理量在合理范围
    EXPECT(t.max_grid_kw > 0.0);
    EXPECT(t.min_grid_kw < t.max_grid_kw);
    EXPECT(t.soc_min >= 0.0 && t.soc_max <= 1.0);
    EXPECT(t.soc_min < t.soc_max);
    EXPECT(t.e_import_kwh > 0.0);
    EXPECT(t.e_dis_kwh > 0.0);
    EXPECT(t.track_samples == 86400);
    EXPECT(t.rmse_track_kw() > 0.0);
    EXPECT(t.mean_abs_track_err_kw() > 0.0);
    EXPECT(t.mean_abs_track_err_kw() <= t.max_abs_err_kw + 1e-9);

    // 事件级压缩比：SOE 条数必须远小于拍数
    EXPECT(obs.soe().size() < 50);
    EXPECT(obs.soe().size() < static_cast<size_t>(t.steps / 1000));
    EXPECT(obs.soe().total() < 50);
    EXPECT(obs.soe().dropped() == 0);

    // 状态机迁移必须被记录，且**不被合并**（链完整）
    EXPECT(obs.soe().has(SoeCode::kFsmTransition));
    EXPECT(obs.soe().count(SoeCode::kFsmTransition) >= 2);
    EXPECT(obs.soe().total_occurrences(SoeCode::kFsmTransition) ==
           static_cast<int>(obs.soe().count(SoeCode::kFsmTransition)));
    for (const auto& e : obs.soe().filter_source(SoeSource::kFsm))
        EXPECT(e.repeat_count == 1);

    // 观察者自己起停都有记录
    EXPECT(obs.soe().has(SoeCode::kObserverStart));
    EXPECT(obs.soe().has(SoeCode::kObserverStop));

    // 指标里能看到累积量
    EXPECT(obs.metrics().value("ems_steps_total") == 86400.0);
    EXPECT(obs.metrics().count_of("ems_track_error_kw") == 86400);
    EXPECT_NEAR(obs.metrics().value("ems_cmd_travel_kw"), t.cmd_travel_kw, 1e-9);
    EXPECT_NEAR(obs.metrics().value("ems_energy_import_kwh"), t.e_import_kwh, 1e-9);
    EXPECT(obs.metrics().find("ems_demand_peak_kw") != nullptr);
    EXPECT_NEAR(obs.metrics().max_of("ems_soc"),
                obs.metrics().max_of("ems_soc"), 1e-12);   // 可读，不崩

    // 汇总文本包含各段
    const std::string s = obs.summary_text();
    EXPECT_CONTAINS(s, "观察者汇总");
    EXPECT_CONTAINS(s, "硬不变量");
    EXPECT_CONTAINS(s, "SOE:");
    EXPECT_CONTAINS(s, "TRACE:");

    // 24h 内不应出现致命事件
    EXPECT(obs.soe().count(SoeLevel::kFatal) == 0);
    EXPECT(!obs.soe().has(SoeCode::kInvariantBroken));
}

// =====================================================================
// T313 与 10/ 交叉校验
// =====================================================================
static void t313_cross_check() {
    std::cout << "[T313] 与 10/ 结果交叉校验（log_every=1）\n";

    Sim24hConfig cfg = make_default_24h_config();
    cfg.log_every = 1;                 // 10/ 的不变量统计走日志，必须逐拍才能对齐口径

    Sim24hResult r = run_sim_24h(cfg);
    EXPECT(r.ok);
    EXPECT(r.steps == 86400);
    EXPECT(r.log_rows == 86400);

    RuntimeObserver obs;
    run_24h_with_observer(cfg, obs);
    const ObserverTotals& t = obs.totals();

    // 步数一致
    EXPECT(t.steps == r.steps);

    // 关口极值一致（10/ 在日志上算，观察者在逐拍上算 —— 逐拍时二者必须相等）
    EXPECT_NEAR(t.max_grid_kw, r.max_grid_kw, 1e-6);
    EXPECT_NEAR(t.min_grid_kw, r.min_grid_kw, 1e-6);

    // 硬不变量口径一致
    EXPECT(t.out_of_interval_ticks == r.out_of_interval);
    EXPECT(t.over_limit_ticks      == r.over_limit);
    EXPECT(t.gated_nonzero_ticks   == r.gated_nonzero);
    EXPECT(r.hard_invariants_ok());
    EXPECT(t.hard_invariants_ok());

    // 能量口径一致（10/ 的 econ 也走日志）
    EXPECT_NEAR(t.e_import_kwh, r.econ.e_import_kwh, 1e-6);
    EXPECT_NEAR(t.e_dis_kwh,    r.econ.e_discharge_kwh, 1e-6);
    EXPECT_NEAR(t.e_chg_kwh,    r.econ.e_charge_kwh, 1e-6);

    // 状态迁移次数一致（10/ 的告警里 FSM 条目数 = 迁移次数）
    size_t fsm_alarms = 0;
    for (const auto& a : r.alarms) if (a.source == "FSM") ++fsm_alarms;
    EXPECT(obs.soe().count(SoeSource::kFsm) == fsm_alarms);
    EXPECT(t.state_changes == static_cast<int>(fsm_alarms));

    // ---- 对照：log_every=10 时 10/ 的口径会失真，观察者不会 ----
    Sim24hConfig cfg10 = make_default_24h_config();
    cfg10.log_every = 10;
    RuntimeObserver obs10;
    LoopMetrics naive10;
    run_24h_with_observer(cfg10, obs10, nullptr, &naive10);

    // 观察者：无论 log_every 多少，结论一致
    EXPECT_NEAR(obs10.totals().max_grid_kw, t.max_grid_kw, 1e-6);
    EXPECT_NEAR(obs10.totals().min_grid_kw, t.min_grid_kw, 1e-6);
    EXPECT(obs10.totals().out_of_interval_ticks == t.out_of_interval_ticks);
    EXPECT_NEAR(obs10.totals().e_import_kwh, t.e_import_kwh, 1e-6);
    EXPECT(obs10.totals().steps == 86400);

    // 而 07/ 的日志口径只看到 1/10 的样本
    EXPECT(naive10.samples * 10 == 86400);
    EXPECT(naive10.samples < 86400);
}

// =====================================================================
// T314 故障场景事件
// =====================================================================
static void t314_fault_events() {
    std::cout << "[T314] 故障场景事件（PCS 故障 / 通信中断）\n";

    // ---- PCS 故障：4h → 5h ----
    Sim24hConfig cfg = make_default_24h_config();
    cfg.duration_s = 8.0 * 3600.0;
    cfg.fault_windows.push_back({4.0 * 3600.0, 5.0 * 3600.0, 4, "pcs_fault"});

    RuntimeObserver obs;
    run_24h_with_observer(cfg, obs);

    // 置位事件：来源 DEVICE、码 PCS_FAULT_SET
    const SoeEvent* set_ev = obs.soe().find(SoeCode::kPcsFaultSet);
    EXPECT(set_ev != nullptr);
    if (set_ev) {
        EXPECT(set_ev->source == SoeSource::kDevice);
        EXPECT(set_ev->level >= SoeLevel::kError);
        EXPECT_NEAR(set_ev->first_t, 4.0 * 3600.0, 1.5);
        EXPECT_CONTAINS(set_ev->message, "PCS 故障");
    }
    // 清除事件：必须与置位**配对**（不是笼统的 COMM_RESTORED）
    const SoeEvent* clr_ev = obs.soe().find(SoeCode::kPcsFaultClear);
    EXPECT(clr_ev != nullptr);
    if (clr_ev) {
        EXPECT(clr_ev->source == SoeSource::kDevice);
        EXPECT_NEAR(clr_ev->first_t, 5.0 * 3600.0, 1.5);
    }

    // 故障期间必须门控（有 state_gated 拍），且门控期间指令为 0（硬不变量）
    EXPECT(obs.totals().state_gate_ticks > 0);
    EXPECT(obs.totals().gated_nonzero_ticks == 0);
    EXPECT(obs.totals().hard_invariants_ok());
    EXPECT(obs.totals().fault_ticks > 0);

    // 状态机必须迁到 FAULT 并恢复
    EXPECT(obs.soe().has(SoeCode::kFsmTransition));
    bool saw_fault_state = false, saw_recover = false;
    for (const auto& e : obs.soe().filter_source(SoeSource::kFsm)) {
        if (e.get("to") == static_cast<double>(static_cast<int>(EmsState::kFault)))
            saw_fault_state = true;
        if (e.get("from") == static_cast<double>(static_cast<int>(EmsState::kFault)))
            saw_recover = true;
    }
    EXPECT(saw_fault_state);
    EXPECT(saw_recover);
    // 迁移链完整（不被合并）
    for (const auto& e : obs.soe().filter_source(SoeSource::kFsm))
        EXPECT(e.repeat_count == 1);

    // ---- 关口电表通信中断：2h → 2.5h ----
    Sim24hConfig cfg2 = make_default_24h_config();
    cfg2.duration_s = 6.0 * 3600.0;
    cfg2.fault_windows.push_back({2.0 * 3600.0, 2.5 * 3600.0, 2, "meter_comm"});

    RuntimeObserver obs2;
    run_24h_with_observer(cfg2, obs2);

    const SoeEvent* m = obs2.soe().find(SoeCode::kCommLostMeter);
    EXPECT(m != nullptr);
    if (m) {
        EXPECT(m->source == SoeSource::kComm);
        EXPECT_NEAR(m->first_t, 2.0 * 3600.0, 1.5);
    }
    // 电表通信异常按接口规范 §6 走 HOLD_LAST 降级，不进 FAULT
    EXPECT(obs2.soe().has(SoeCode::kHoldLastStart));
    EXPECT(obs2.soe().has(SoeCode::kHoldLastEnd));
    EXPECT(obs2.totals().hold_last_ticks > 0);
    EXPECT(obs2.totals().hard_invariants_ok());
    // HOLD_LAST 期间指令区间被钉在单点 → 仍满足 p_cmd ∈ [p_lower, p_upper]
    EXPECT(obs2.totals().out_of_interval_ticks == 0);
}

// =====================================================================
// T315 观察者自身可观测
// =====================================================================
static void t315_self_observability() {
    std::cout << "[T315] 观察者自身可观测（suppressed / dropped / filtered / passed）\n";

    // ---- 抑制统计：抖动型信号让同一事件反复发生 ----
    EmsRuntime rt;
    LoopConfig lc;
    lc.dt_s = 1.0;
    lc.log_every = 1;
    lc.enable_log = true;
    lc.demand_window_s = 4.0;
    DeviceLimits dl;
    dl.transformer_capacity_kw = 1000.0;
    dl.d_target_kw = 400.0;
    SafetyParams sp;
    sp.grid_p_max_kw = 1e9;
    sp.tr_overload_th = 2.0;          // 关掉变压器过载，隔离出需量这一路
    assemble(rt, lc, dl, sp);

    RuntimeObserver obs;
    obs.start(0.0);
    const int N = 400;
    for (int i = 0; i < N; ++i) {
        rt.set_environment((i % 2 == 0) ? 380.0 : 425.0, 0.0);
        StepRecord rec = rt.step(1.0);
        obs.on_step(rt, rec);
    }
    obs.stop(static_cast<double>(N));

    // 需量越限在阈值附近反复穿越 → 被时间窗抑制合并
    const SoeEvent* br = obs.soe().find(SoeCode::kDemandBreachStart);
    EXPECT(br != nullptr);
    if (br) {
        EXPECT(br->repeat_count >= 2);
        EXPECT(br->suppressed());
    }
    EXPECT(obs.soe().suppressed() > 0);
    EXPECT(obs.soe().total() > obs.soe().size());
    // 抑制是"框架能力"，但绝不允许静默丢失：total 与 size 的差就是抑制量
    EXPECT(obs.soe().total() - obs.soe().size() >= obs.soe().suppressed());

    // ---- 容量淘汰如实上报 ----
    ObserverConfig oc;
    oc.soe.capacity = 16;
    oc.soe.suppress_enabled = false;
    RuntimeObserver obs2(oc);
    // 刻意不调 start()：先看清"纯 push 100 条"的淘汰数，避免 start 事件混进来
    for (int i = 0; i < 100; ++i) {
        SoeEvent ev;
        ev.t = static_cast<double>(i);
        ev.level = SoeLevel::kInfo;
        ev.source = SoeSource::kSystem;
        ev.code = SoeCode::kFsmTransition;
        ev.message = "m" + std::to_string(i);
        obs2.soe().push(ev);
    }
    EXPECT(obs2.soe().size() == 16);
    EXPECT(obs2.soe().dropped() == 84);
    EXPECT(obs2.soe().overflowed());
    EXPECT_CONTAINS(obs2.soe().summary_text(), "已淘汰");

    // ---- 跟踪过滤统计 ----
    RuntimeObserver obs3;
    obs3.trace().set_global(SoeLevel::kError);
    EXPECT(obs3.trace().allow(SoeSource::kSafety, SoeLevel::kInfo) == false);
    EXPECT(obs3.trace().allow(SoeSource::kSafety, SoeLevel::kWarn) == false);
    EXPECT(obs3.trace().allow(SoeSource::kSafety, SoeLevel::kError) == true);
    EXPECT(obs3.trace().filtered(SoeSource::kSafety) == 2);
    EXPECT(obs3.trace().passed(SoeSource::kSafety) == 1);
    EXPECT_CONTAINS(obs3.trace().summary_text(), "过滤 2 条");

    // ---- 指标表里能查到观察者自己的运行量 ----
    EXPECT(obs.metrics().has("ems_steps_total"));
    EXPECT(obs.metrics().has("ems_fault_ticks_total"));
    EXPECT(obs.metrics().value("ems_steps_total") == 400.0);
    EXPECT(obs.metrics().value("ems_safety_clip_ticks_total") ==
           static_cast<double>(obs.totals().safety_clip_ticks));
    EXPECT(obs.metrics().value("ems_tr_overload_ticks_total") ==
           static_cast<double>(obs.totals().tr_overload_ticks));
    EXPECT(obs.metrics().value("ems_soc_violation_ticks_total") ==
           static_cast<double>(obs.totals().soc_violation_ticks));

    // ---- reset 之后可以干净复用 ----
    obs.reset();
    EXPECT(obs.totals().steps == 0);
    EXPECT(obs.soe().size() == 0);
    EXPECT(obs.metrics().value("ems_steps_total") == 0.0);
    EXPECT(obs.trace().filtered_total() == 0);
    EXPECT(obs.soe().dropped() == 0);
}

// =====================================================================
int main() {
    std::cout << "===== P2 可观测性 单元测试 =====\n";
    t301_enums();
    t302_suppression();
    t303_no_merge_transitions();
    t304_bounded_memory();
    t305_export();
    t306_metrics();
    t307_decoupling();
    t308_quantile();
    t309_prometheus();
    t310_level_filter();
    t311_sampling();
    t312_end_to_end();
    t313_cross_check();
    t314_fault_events();
    t315_self_observability();

    std::cout << "\n----- 断言统计 -----\n";
    std::cout << "  通过 " << g_pass << " / 失败 " << g_fail << "\n";
    if (g_fail == 0) {
        std::cout << "\n=== ALL TESTS PASSED ===\n";
        return 0;
    }
    std::cout << "\n=== TESTS FAILED ===\n";
    return 1;
}
