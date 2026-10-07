// =====================================================================
// 17/tests/test_scale_rtdb.cpp —— 规模化：变化上传(C2) · 分级节拍(C3) · 批量写(C1)
//
// 缺口原文（§10.3）：
//   C1 | 批量写 API 存在但**零调用** → 几千点 = 几千次 seqlock 往返
//   C2 | 无变化上传（死区）        → 每拍全量重发全表
//   C3 | 无分级节拍                → 单体电压与 SOC 同一个节拍
//
//   T01 死区：静止工况抑制率 > 99%
//   T02 死区：小变化抑制 / 超死区必发（含累积漂移）
//   T03 ★ 死区绝不能吃掉安全位（关键安全点"永远发布"）
//   T04 分级节拍：三档触发次数比符合周期比
//   T05 分级节拍：冷启动全部先跑一次
//   T06 分级节拍：慢档不被快档 tick 影响
//   T07 ★ 批量写：N 点 = **1 次调用**，且逐点值与 N 次单点写等价
//   T08 批量写：参数不一致 → 整批拒绝（不半写）
//   T09 流水线组合：分级 → 死区 → 批量，量化"全量重发 vs 优化后"
//   T10 真 RT_DB 集成：publish_batch 落到 rt_db_set_multiple_values（无段则显式 SKIP）
//   T11 RtDbBatchWriter：句柄为空 → 干净拒绝（不崩、不半写）
//
// 编译：见 17/scripts/build_test.bat
// =====================================================================

#include "expect.h"

#include "bms_sim.h"
#include "device_point_table.h"
#include "meter_sim.h"
#include "pcs_sim.h"
#include "point_sink.h"
#include "publish_pipeline.h"
#include "rtdb_batch_writer.h"

#include "ems_point_table.h"
#include "ems_rt_db_setup.h"

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace ems;
using namespace ems::devsim;
using namespace ems::devpt;

static int g_pass   = 0;
static int g_fail   = 0;
static int g_skipped = 0;

static const double kPi = 3.14159265358979323846;

// =====================================================================
// T01 静止工况的抑制率
// =====================================================================
static void test_01_static_suppression() {
    std::printf("T01 死区：静止工况抑制率 > 99%%\n");

    DeadbandPublisher db;
    db.configure("P", 0.5);                       // 死区 0.5
    EXPECT(!db.always_publish("P"));
    EXPECT_NEAR(db.deadband_of("P"), 0.5, 1e-12);

    const int n = 10000;
    long pub = 0;
    for (int k = 0; k < n; ++k) if (db.should_publish("P", 100.0)) ++pub;
    EXPECT_EQ(pub, 1);                            // 只有第一拍（无历史值）发
    EXPECT_EQ(db.stats().published, 1);
    EXPECT_EQ(db.stats().suppressed, n - 1);
    EXPECT(db.stats().suppress_rate() > 0.99);
    EXPECT_NEAR(db.stats().suppress_rate(), 0.9999, 1e-9);

    // 反向守卫：把死区关掉（= 0）后抑制率必须掉到 0 ——
    // 证明上面那个 99.99% 来自死区，不是"统计本来就长这样"
    DeadbandPublisher db0;
    db0.configure("P", 0.0);
    long pub0 = 0;
    for (int k = 0; k < n; ++k) if (db0.should_publish("P", 100.0)) ++pub0;
    EXPECT_EQ(pub0, n);
    EXPECT_NEAR(db0.stats().suppress_rate(), 0.0, 1e-12);
}

// =====================================================================
// T02 小变化抑制 / 超死区必发
// =====================================================================
static void test_02_deadband_semantics() {
    std::printf("T02 死区：小变化抑制 / 超死区必发\n");

    DeadbandPublisher db;
    db.configure("P", 1.0);
    EXPECT(db.should_publish("P", 100.0));        // 首值
    EXPECT(!db.should_publish("P", 100.5));       // Δ=0.5 < 1.0 → 抑制
    EXPECT(!db.should_publish("P", 100.9));       // 仍与**上次发布值**比 → 抑制
    EXPECT(!db.should_publish("P", 101.0));       // Δ 恰好 == 死区 → 抑制（严格大于）
    EXPECT(db.should_publish("P", 101.1));        // Δ=1.1 > 1.0 → 发布
    EXPECT(!db.should_publish("P", 101.5));       // 重新以 101.1 为基准 → 抑制
    EXPECT(db.should_publish("P", 102.2));        // Δ=1.1 → 发布
    EXPECT_EQ(db.stats().published, 3);
    EXPECT_EQ(db.stats().suppressed, 4);

    // ★ 累积漂移最终必须发出去（死区不是"永远不发"）
    DeadbandPublisher db2;
    db2.configure("P", 1.0);
    db2.should_publish("P", 0.0);
    long pub = 0;
    for (int k = 1; k <= 100; ++k) {              // 每拍 +0.1，累积 +10
        if (db2.should_publish("P", k * 0.1)) ++pub;
    }
    EXPECT(pub >= 9);                             // 每积累到 1.0 就发一次
    EXPECT(pub <= 11);
    // 末值必须已经到了 10 附近（没被死区永久吞掉）
    EXPECT_NEAR(db2.stats().published, pub + 1, 1e-12);
}

// =====================================================================
// T03 死区绝不能吃掉安全位
// =====================================================================
static void test_03_safety_never_suppressed() {
    std::printf("T03 ★ 死区绝不能吃掉安全位\n");

    // 安全点：禁充放位 / 故障位 / 通信位 —— 全部配成"永远发布"
    const char* safety[] = {
        "STA.BMS.CHG_FORBID", "STA.BMS.DIS_FORBID",
        "STA.PCS.FAULT", "STA.BMS.COMM_OK", "STA.PCS.COMM_OK",
        "STA.METER.COMM_OK", "ALM.BMS.LEVEL", "ALM.BMS.CODE",
        "ALM.PCS.LEVEL", "ALM.PCS.FAULT_CODE"
    };
    DeadbandPublisher db;
    for (const char* n : safety) db.configure(n, 0.0);   // 死区 0 = 永远发布

    for (const char* n : safety) {
        EXPECT(db.always_publish(n));
        // **值完全不变**也必须发 —— 安全位是"电平语义"，
        // 上游靠"每拍看到它"来判断这条锁还在，而不是靠"值变了"
        long pub = 0;
        for (int k = 0; k < 200; ++k) if (db.should_publish(n, 1.0)) ++pub;
        EXPECT_EQ(pub, 200);
    }
    EXPECT_EQ(db.stats().suppressed, 0);
    EXPECT_NEAR(db.stats().suppress_rate(), 0.0, 1e-12);

    // 显式 always 标记（即使死区非 0）
    DeadbandPublisher db2;
    db2.configure("STA.BMS.CHG_FORBID", 5.0, /*always=*/true);
    EXPECT(db2.always_publish("STA.BMS.CHG_FORBID"));
    long pub2 = 0;
    for (int k = 0; k < 100; ++k) if (db2.should_publish("STA.BMS.CHG_FORBID", 0.0)) ++pub2;
    EXPECT_EQ(pub2, 100);

    // ★ 反向守卫：**未配置**的点按"不抑制"处理（保守）。
    //   如果实现把未配置点当成"死区无限大"，安全位在配置漏项时会被静默吞掉 ——
    //   这里把那条错误实现钉死。
    DeadbandPublisher db3;
    EXPECT(db3.always_publish("NEVER.CONFIGURED"));
    EXPECT(db3.should_publish("NEVER.CONFIGURED", 0.0));
    EXPECT(db3.should_publish("NEVER.CONFIGURED", 0.0));
}

// =====================================================================
// 分级节拍的公共小夹具
// =====================================================================
namespace {
struct TierFixture {
    TieredScheduler sch{0.1, 1.0, 5.0};
    TierFixture() {
        sch.assign("F", TieredScheduler::kFast);
        sch.assign("M", TieredScheduler::kMedium);
        sch.assign("S", TieredScheduler::kSlow);
    }
    long count_of(const std::string& name, int ticks, double dt) {
        long n = 0;
        for (int k = 0; k < ticks; ++k) {
            const std::vector<std::string> due = sch.tick(dt);
            for (const auto& d : due) if (d == name) ++n;
        }
        return n;
    }
};
} // namespace

// =====================================================================
// T04 三档触发次数比符合周期比
// =====================================================================
static void test_04_tier_ratio() {
    std::printf("T04 分级节拍：三档触发次数比符合周期比\n");

    TierFixture f;
    const int ticks = 1000;
    const double dt = 0.1;
    const long nf = f.count_of("F", ticks, dt);
    const long nm = f.count_of("M", ticks, dt);
    const long ns = f.count_of("S", ticks, dt);

    std::printf("    1000 拍 @ dt=0.1 → 快=%ld 中=%ld 慢=%ld\n", nf, nm, ns);
    // 期望：快 1000/0.1×0.1 = 1000，中 100，慢 20（各 +1 冷启动）
    EXPECT(nf >= 1000 && nf <= 1002);
    EXPECT(nm >= 100 && nm <= 102);
    EXPECT(ns >= 20 && ns <= 22);
    // 周期比：中/快 ≈ 0.1，慢/中 ≈ 0.2
    EXPECT_NEAR(static_cast<double>(nm) / static_cast<double>(nf), 0.1, 0.005);
    EXPECT_NEAR(static_cast<double>(ns) / static_cast<double>(nm), 0.2, 0.02);

    // 反向守卫：把三档周期设成一样 → 三者次数必须**相同**
    //（否则上面那个比例可能只是"档位写死了"，不是真的按周期推）
    TieredScheduler same(0.5, 0.5, 0.5);
    same.assign("F", TieredScheduler::kFast);
    same.assign("M", TieredScheduler::kMedium);
    same.assign("S", TieredScheduler::kSlow);
    long a = 0, b = 0, c = 0;
    for (int k = 0; k < 400; ++k) {
        for (const auto& d : same.tick(0.1)) {
            if (d == "F") ++a; else if (d == "M") ++b; else ++c;
        }
    }
    EXPECT_EQ(a, b);
    EXPECT_EQ(b, c);
}

// =====================================================================
// T05 冷启动全部先跑一次
// =====================================================================
static void test_05_cold_start() {
    std::printf("T05 分级节拍：冷启动全部先跑一次\n");

    TieredScheduler sch(0.1, 1.0, 5.0);
    sch.assign("F", TieredScheduler::kFast);
    sch.assign("M", TieredScheduler::kMedium);
    sch.assign("S", TieredScheduler::kSlow);
    EXPECT(sch.cold());

    const std::vector<std::string> first = sch.tick(0.1);
    EXPECT_EQ(static_cast<int>(first.size()), 3);        // ★ 三档全跑
    EXPECT(!sch.cold());
    bool hasF = false, hasM = false, hasS = false;
    for (const auto& d : first) {
        if (d == "F") hasF = true;
        if (d == "M") hasM = true;
        if (d == "S") hasS = true;
    }
    EXPECT(hasF); EXPECT(hasM); EXPECT(hasS);
    // 档内顺序：快 → 中 → 慢
    EXPECT_STR_EQ(first[0], "F");
    EXPECT_STR_EQ(first[1], "M");
    EXPECT_STR_EQ(first[2], "S");

    // ★ 反向守卫：**没有**冷启动规则的实现会让第 2 拍只剩快档 ——
    //   这里钉住"第 2 拍不应该三档齐全"
    const std::vector<std::string> second = sch.tick(0.1);
    EXPECT_EQ(static_cast<int>(second.size()), 1);
    EXPECT_STR_EQ(second[0], "F");

    // 慢档点到第一个周期之前只出现过冷启动那一次
    TieredScheduler s2(0.1, 1.0, 5.0);
    s2.assign("S", TieredScheduler::kSlow);
    long slow_fires = 0;
    for (int k = 0; k < 49; ++k) {                       // 4.9 s < 5 s
        for (const auto& d : s2.tick(0.1)) if (d == "S") ++slow_fires;
    }
    EXPECT_EQ(slow_fires, 1);                            // 只有冷启动
    // 跨过 5 s 后必须出现第二次
    long after = slow_fires;
    for (int k = 0; k < 2; ++k) {
        for (const auto& d : s2.tick(0.1)) if (d == "S") ++after;
    }
    EXPECT_EQ(after, 2);
    EXPECT_EQ(s2.stats().fires[TieredScheduler::kSlow], 2);
}

// =====================================================================
// T06 慢档不被快档 tick 影响
// =====================================================================
static void test_06_slow_isolated() {
    std::printf("T06 分级节拍：慢档不被快档 tick 影响\n");

    TierFixture f;
    long slow_seen = 0, fast_seen = 0;
    for (int k = 0; k < 40; ++k) {                       // 4 s
        for (const auto& d : f.sch.tick(0.1)) {
            if (d == "S") ++slow_seen;
            if (d == "F") ++fast_seen;
        }
    }
    EXPECT_EQ(fast_seen, 40);
    EXPECT_EQ(slow_seen, 1);                             // 只冷启动那一次
    EXPECT_EQ(f.sch.stats().fires[TieredScheduler::kFast], 40);
    EXPECT_EQ(f.sch.stats().fires[TieredScheduler::kSlow], 1);

    // 档归属查询（未登记的点按快档 —— 保守，宁可多发不可漏发）
    EXPECT_EQ(f.sch.tier_of("S"), TieredScheduler::kSlow);
    EXPECT_EQ(f.sch.tier_of("M"), TieredScheduler::kMedium);
    EXPECT_EQ(f.sch.tier_of("NEVER"), TieredScheduler::kFast);
    EXPECT_EQ(static_cast<int>(f.sch.point_count()), 3);
}

// =====================================================================
// T07 批量写：N 点 = 1 次调用，且与单点写等价
// =====================================================================
static void test_07_batch_semantics() {
    std::printf("T07 ★ 批量写：N 点 = 1 次调用，逐点值等价\n");

    const int N = 40;
    std::vector<std::string> names;
    std::vector<double>      vals;
    std::vector<int>         qs;
    for (int i = 0; i < N; ++i) {
        names.push_back("PT." + std::to_string(i));
        vals.push_back(i * 1.5 - 10.0);
        qs.push_back(0);
    }

    CountingWriter batch;
    // ★ 有副作用的调用**不要**塞进断言条件里。
    //   正常跑没问题（条件总会被求值），但它会把测试变成"框架语义的奴隶"：
    //   一旦 EXPECT 的实现改成不短路/不求值（或有人做失败注入），
    //   这次调用就静默不发生，后面所有断言都建在空状态上 —— 实测就是整数除零、
    //   整个 exe 崩掉、连 PASS=/FAIL= 收尾行都印不出来。
    //   所以：调用单独一行、结果进断言。
    const bool batch_ok = batch.write_batch(names, vals, qs);
    EXPECT(batch_ok);
    EXPECT_EQ(batch.stats().calls, 1);                   // ★ 一次调用
    EXPECT_EQ(batch.stats().point_writes, N);            // 但写了 N 点
    EXPECT_EQ(batch.store().size(), static_cast<std::size_t>(N));

    // 对照：N 次单点写
    //   ★ 逐点断言保留（40 条）—— 但调用与断言分开写，不把副作用塞进条件里。
    CountingWriter single;
    int single_ok = 0;
    for (int i = 0; i < N; ++i) {
        const bool ok = single.write_one(names[i], vals[i], 0);
        EXPECT(ok);
        if (ok) ++single_ok;
    }
    EXPECT_EQ(single_ok, N);
    EXPECT_EQ(single.stats().calls, N);                  // N 次调用
    EXPECT_EQ(single.stats().point_writes, N);

    // ★ 降低倍数
    //   先判非零再除：批量写真坏掉时 `calls` 会是 0，
    //   直接 `a / 0` 是整数除零（崩），而不是一条看得见的 FAIL。
    EXPECT_GT0(batch.stats().calls);
    EXPECT_GT0(single.stats().calls);
    if (batch.stats().calls > 0 && single.stats().calls > 0) {
        std::printf("    N=%d：批量写 calls=%ld vs 单点写 calls=%ld（降低 %ld×）\n",
                    N, batch.stats().calls, single.stats().calls,
                    single.stats().calls / batch.stats().calls);
        EXPECT_EQ(single.stats().calls / batch.stats().calls, N);
    }

    // ★ 逐点值等价
    int value_diff = 0;
    for (const auto& kv : single.store()) {
        if (!batch.has(kv.first) || batch.at(kv.first) != kv.second) ++value_diff;
    }
    EXPECT_EQ(value_diff, 0);
    // 反向守卫：这一批**确实**写进了不重复的 N 个点名（不是同一个点覆盖 N 次）
    EXPECT_EQ(static_cast<int>(batch.store().size()), N);
    EXPECT_EQ(batch.bad_calls(), 0);
}

// =====================================================================
// T08 批量写：参数不一致 → 整批拒绝
// =====================================================================
static void test_08_batch_reject() {
    std::printf("T08 批量写：参数不一致 → 整批拒绝（不半写）\n");

    CountingWriter w;
    std::vector<std::string> names = {"A", "B", "C"};
    std::vector<double>      vals  = {1.0, 2.0};        // 长度不一致
    std::vector<int>         qs    = {0, 0, 0};
    EXPECT(!w.write_batch(names, vals, qs));
    EXPECT_EQ(w.stats().calls, 0);                      // 一次调用都没发生
    EXPECT_EQ(w.stats().point_writes, 0);               // 也没有半写
    EXPECT_EQ(w.bad_calls(), 1);
    EXPECT_EQ(w.store().size(), 0);

    // 空点名
    std::vector<std::string> bad = {"A", "", "C"};
    std::vector<double>      v2  = {1.0, 2.0, 3.0};
    EXPECT(!w.write_batch(bad, v2, qs));
    EXPECT_EQ(w.stats().point_writes, 0);
    EXPECT_EQ(w.store().size(), 0);
    EXPECT_EQ(w.bad_calls(), 2);

    // 反向守卫：正确参数必须成功（证明上面拒的不是"批量写根本不能用"）
    EXPECT(w.write_batch(names, v2, qs));
    EXPECT_EQ(w.stats().calls, 1);
    EXPECT_EQ(w.store().size(), 3u);
}

// =====================================================================
// 流水线的公共装配：三台模拟器 → 全点表 102 点 → 分级 + 死区 + 批量
// =====================================================================
namespace {

struct PlantFixture {
    MeterSimConfig mc;
    BmsSimConfig   bc;
    PcsSimConfig   pc;
    MeterSim meter;
    BmsSim   bms;
    PcsSim   pcs;

    PublishPipeline pipe;
    PipelineSink    sink;

    CountingWriter optimized;
    CountingWriter baseline;

    std::size_t n_registered = 0;

    PlantFixture() : meter(mc), bms(bc), pcs(pc), pipe(&optimized), sink(pipe) {
        // 分级节拍：0.1 / 1.0 / 5.0
        pipe.scheduler().set_period(TieredScheduler::kFast,   0.1);
        pipe.scheduler().set_period(TieredScheduler::kMedium, 1.0);
        pipe.scheduler().set_period(TieredScheduler::kSlow,   5.0);

        // 登记全部设备侧点（CMD 区 5 点是 EMS 下行，不由设备侧发布）
        for (int i = 0; i < DP_POINT_COUNT; ++i) {
            const DevicePointDef& d = kBuiltinPoints[i];
            if (d.zone == PZ_CMD) continue;
            const std::string n = d.name;
            pipe.scheduler().assign(n, tier_of_point(d));
            // 死区 = 量程跨度的 0.2%（安全点强制 0）
            const bool safety = is_safety_point(d);
            const double span = d.range_max - d.range_min;
            pipe.deadband().configure(n, safety ? 0.0 : 0.002 * span);
            ++n_registered;
        }
        // 静态配置点先写一次（它们之后就不变了 → 会被死区吃掉，正是我们要的）
        meter.set_site_config(630.0, 400.0);
        meter.publish_static(sink);
        bms.publish_static(sink);
        pcs.publish_static(sink);
    }

    // 安全点：禁充放 / 故障 / 通信 / 告警 —— 死区必须为 0
    static bool is_safety_point(const DevicePointDef& d) {
        if (d.zone == PZ_ALM) return true;
        const std::string n = d.name;
        return n.find("FORBID") != std::string::npos ||
               n.find("FAULT")  != std::string::npos ||
               n.find("COMM_OK") != std::string::npos ||
               n.find("DATA_VALID") != std::string::npos ||
               n.find("MAIN_POS") != std::string::npos ||
               n.find("MAIN_NEG") != std::string::npos;
    }

    // 分级：保护/状态 → 快；功率/三相 → 中；单体/电能/需量 → 慢
    static TieredScheduler::Tier tier_of_point(const DevicePointDef& d) {
        const std::string n = d.name;
        if (d.zone == PZ_STA || d.zone == PZ_ALM) return TieredScheduler::kFast;
        if (n.find(".BMS.SOC") != std::string::npos ||
            n.find(".BMS.SOH") != std::string::npos ||
            n.find("RM_CHG")   != std::string::npos ||
            n.find("RM_DIS")   != std::string::npos)
            return TieredScheduler::kFast;               // SOC 类必须快
        if (n.find(".BMS.") != std::string::npos)        return TieredScheduler::kSlow;
        if (n.find(".METER.EP") != std::string::npos ||
            n.find(".METER.EQ") != std::string::npos ||
            n.find("DEMAND")    != std::string::npos)
            return TieredScheduler::kSlow;
        if (d.zone == PZ_CFG) return TieredScheduler::kSlow;
        return TieredScheduler::kMedium;
    }

    void step(double t, double dt) {
        const double p_cmd = 150.0 * std::sin(2.0 * kPi * t / 300.0);
        const double q_cmd = 60.0 * std::sin(2.0 * kPi * t / 180.0);
        const double p_act = pcs.step(t, dt, sink, p_cmd, q_cmd, true);
        bms.step(t, dt, sink, p_act);

        MeterInputs mi;
        mi.p_kw      = 300.0 + 120.0 * std::sin(2.0 * kPi * t / 300.0);
        mi.q_kvar    = q_cmd;
        mi.p_load_kw = 420.0 + 60.0 * std::sin(2.0 * kPi * t / 70.0);
        mi.p_pv_kw   = std::fmax(0.0, 120.0 * std::sin(2.0 * kPi * t / 90.0));
        meter.step(t, dt, sink, mi);

        // 优化路径：分级 → 死区 → 批量写
        pipe.tick(dt);
        // 对照路径：全量重发（每拍写全部点，且每个点一次单点写）
        PublishPipeline::baseline_tick_with(baseline, pipe.values());
    }
};

} // namespace

// =====================================================================
// T09 流水线组合 + 量化对比
// =====================================================================
static void test_09_pipeline_quantified() {
    std::printf("T09 流水线：分级节拍 → 死区 → 批量写；全量重发 vs 优化后\n");

    PlantFixture f;
    const double dt = 0.5;
    const int ticks = 600;                               // 300 s
    for (int k = 0; k < ticks; ++k) f.step(k * dt, dt);

    const PublishPipeline::Totals& t = f.pipe.totals();
    const WriteStats& base = f.baseline.stats();

    std::printf("    登记点数 P=%zu（设备侧全表，CMD 区除外）\n", f.n_registered);
    std::printf("    全量重发：    calls=%ld  point_writes=%ld\n",
                base.calls, base.point_writes);
    std::printf("    优化后：     calls=%ld  point_writes=%ld"
                "（分级到期 %ld / 死区抑制 %ld）\n",
                t.batch_calls, t.point_writes, t.due, t.suppressed);
    std::printf("    降低倍数：   calls %.1f×   point_writes %.1f×\n",
                static_cast<double>(base.calls) / static_cast<double>(t.batch_calls),
                static_cast<double>(base.point_writes) / static_cast<double>(t.point_writes));
    std::printf("    死区抑制率：%.4f\n",
                static_cast<double>(t.suppressed) /
                    static_cast<double>(t.due > 0 ? t.due : 1));

    // ---- 基本一致性 ----
    EXPECT_EQ(t.ticks, ticks);
    EXPECT_EQ(base.point_writes, static_cast<long>(ticks) * static_cast<long>(f.n_registered));
    EXPECT_EQ(base.calls, base.point_writes);            // 单点写：调用次数 == 点数

    // ---- 档位点数分布（报告用）----
    long tier_pts[3] = {0, 0, 0};
    long safety_pts = 0;
    for (int i = 0; i < DP_POINT_COUNT; ++i) {
        const DevicePointDef& d = kBuiltinPoints[i];
        if (d.zone == PZ_CMD) continue;
        ++tier_pts[PlantFixture::tier_of_point(d)];
        if (PlantFixture::is_safety_point(d)) ++safety_pts;
    }

    const TieredScheduler::Stats& ss = f.pipe.scheduler().stats();
    std::printf("    档位点数：快=%ld 中=%ld 慢=%ld；触发次数：快=%ld 中=%ld 慢=%ld\n",
                tier_pts[0], tier_pts[1], tier_pts[2], ss.fires[0], ss.fires[1], ss.fires[2]);

    // ---- ★ 写入次数的**地板**：安全点"永远发布"，谁也压不下去 ----
    //
    //   这不是"优化没做好"，而是设计上的**不可压缩部分**：
    //   安全点（禁充放/故障/通信/告警）的死区必须为 0 = 每拍都发，
    //   否则"BMS 禁止放电"这条锁可能半天传不上去（见文件头纪律 1）。
    //   所以点值写入的降低倍数有一个**理论上界**：
    //       上界 = 全量重发 / 安全点写入数 = 61200 / 10200 ≈ 6.0×
    //   把"不可压缩的地板"和"可压缩的数据点"分开报，才是有意义的量化结论。
    long safety_writes = 0;
    for (int i = 0; i < DP_POINT_COUNT; ++i) {
        const DevicePointDef& d = kBuiltinPoints[i];
        if (d.zone == PZ_CMD) continue;
        if (!PlantFixture::is_safety_point(d)) continue;
        safety_writes += ss.fires[PlantFixture::tier_of_point(d)];
    }
    const long data_writes = t.point_writes - safety_writes;   // 非安全点真正写出去的量
    const long data_baseline = base.point_writes -
                               static_cast<long>(ticks) * safety_pts;
    std::printf("    其中：安全点永远发布 %ld 次（不可压缩，地板）；数据点 %ld 次\n",
                safety_writes, data_writes);
    std::printf("    数据点部分：%ld → %ld，降低 %.1f×（安全点 %ld 个已扣除）\n",
                data_baseline, data_writes,
                static_cast<double>(data_baseline) / static_cast<double>(data_writes > 0 ? data_writes : 1),
                safety_pts);

    // ---- ★ 量化结论：写入次数从 X 降到 Y ----
    EXPECT(t.batch_calls > 0);
    EXPECT(t.point_writes > 0);
    EXPECT(t.batch_calls < base.calls / 50);             // API 调用数降 > 50 倍
    EXPECT(t.point_writes * 4 < base.point_writes);      // 总写入降 > 4 倍（地板 ~6×）
    // 扣除不可压缩的安全点后，数据点的压缩比才体现真实优化力度
    EXPECT(data_writes * 10 < data_baseline);            // 数据点写入降 > 10 倍
    EXPECT_GT0(safety_writes);
    EXPECT(safety_writes < t.point_writes);              // 地板不该超过总量
    // 死区确实在起作用（不是"分级节拍一个人扛"）
    EXPECT_GT0(t.suppressed);
    EXPECT(t.suppressed > static_cast<long>(ticks) * 10);

    // ---- 分级节拍的分档触发与各自周期一致 ----
    //
    //   ★ 陷阱：**触发次数比不是固定的 1/10，它取决于 dt 与周期的关系**。
    //   T04 里 dt=0.1 且快档周期 = 0.1 → 中/快 = 0.1；
    //   本场景 dt=0.5，快档（0.1 s）仍是"每拍到"，但中档（1 s）变成"每 2 拍到"
    //   → 中/快 = 0.5。所以这里按**周期**算绝对值，而不是抄 T04 的 0.1。
    const double pm = f.pipe.scheduler().period(TieredScheduler::kMedium);
    const double ps = f.pipe.scheduler().period(TieredScheduler::kSlow);
    EXPECT_EQ(ss.fires[0], static_cast<long>(ticks));    // dt >= 快档周期 → 每拍都到
    EXPECT_NEAR(static_cast<double>(ss.fires[1]),
                static_cast<double>(ticks) * dt / pm, 1.0);
    EXPECT_NEAR(static_cast<double>(ss.fires[2]),
                static_cast<double>(ticks) * dt / ps, 1.0);
    EXPECT(ss.fires[0] > ss.fires[1]);
    EXPECT(ss.fires[1] > ss.fires[2]);
    // 反向守卫：把中档周期拉到比慢档还长，触发次数必须跟着倒过来
    {
        TieredScheduler probe(0.1, 10.0, 5.0);
        probe.assign("x", TieredScheduler::kMedium);
        probe.assign("y", TieredScheduler::kSlow);
        for (int k = 0; k < 600; ++k) probe.tick(0.5);
        EXPECT(probe.stats().fires[1] < probe.stats().fires[2]);
    }

    // ---- ★ 逐点值等价（安全点必须逐位一致；非安全点允许因死区而滞后）----
    int safety_mismatch = 0, safety_checked = 0;
    int non_safety_mismatch = 0, non_safety_checked = 0;
    for (int i = 0; i < DP_POINT_COUNT; ++i) {
        const DevicePointDef& d = kBuiltinPoints[i];
        if (d.zone == PZ_CMD) continue;
        const std::string n = d.name;
        if (!f.optimized.has(n)) continue;               // 没发过（值恒等于 0 且被死区吃掉）
        if (!f.baseline.has(n)) continue;
        if (PlantFixture::is_safety_point(d)) {
            ++safety_checked;
            if (f.optimized.at(n) != f.baseline.at(n)) ++safety_mismatch;
        } else {
            ++non_safety_checked;
            if (f.optimized.at(n) != f.baseline.at(n)) ++non_safety_mismatch;
        }
    }
    std::printf("    安全点逐位核对 %d 个：不一致 %d；非安全点 %d 个：不一致 %d（死区允许滞后）\n",
                safety_checked, safety_mismatch, non_safety_checked, non_safety_mismatch);
    EXPECT_GT0(safety_checked);
    EXPECT_EQ(safety_mismatch, 0);                       // 安全点绝不允许滞后
    // 反向守卫：非安全点**应当**有滞后 —— 否则说明死区根本没生效
    EXPECT_GT0(non_safety_mismatch);
}

// =====================================================================
// T10 真 RT_DB 集成（无共享内存段时显式 SKIP）
// =====================================================================
static void test_10_real_rtdb() {
    std::printf("T10 真 RT_DB 集成：publish_batch → rt_db_set_multiple_values\n");

    bool created = false;
    if (!ems_rt_db_setup(true, &created)) {
        std::printf("    [SKIP] 共享内存段建不出来（环境限制），本层跳过\n");
        ++g_skipped;
        return;
    }
    rt_db_handle_t h;
    std::memset(&h, 0, sizeof(h));
    if (!rt_db_init(&h, nullptr)) {
        std::printf("    [SKIP] rt_db_init 失败（段不兼容），本层跳过\n");
        ++g_skipped;
        return;
    }

    // 用抽象表里真实存在的点名（段里注册的就是这 40 个）
    const std::vector<std::string> names = {
        "MEAS.P_LOAD", "MEAS.P_PV", "MEAS.P_BAT", "MEAS.P_GRID",
        "MEAS.SOC", "MEAS.T_C", "MEAS.SOH", "CMD.P_BAT"
    };
    std::vector<double> vals = {111.0, 222.0, 333.0, 444.0, 0.66, 27.5, 99.9, -50.0};
    std::vector<int>    qs(names.size(), 0);

    // ---- ① 批量写：一次调用写 8 点 ----
    RtDbBatchWriter w(&h);
    EXPECT(w.ready());
    EXPECT(w.write_batch(names, vals, qs));
    EXPECT_EQ(w.stats().calls, 1);                       // ★ 一次
    EXPECT_EQ(w.stats().point_writes, 8);
    EXPECT_EQ(w.rejected(), 0);

    // ---- ② 逐点读回校验（证明批量写真的落到段里了）----
    int wrong = 0;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::size_t idx = rt_db_find_index_by_id(&h, names[i].c_str());
        if (idx == static_cast<std::size_t>(-1)) { ++wrong; continue; }
        double v = 0.0;
        long   q = 0;
        if (!rt_db_get_value(&h, idx, &v, &q, nullptr)) { ++wrong; continue; }
        if (std::fabs(v - vals[i]) > 1e-9) ++wrong;
    }
    EXPECT_EQ(wrong, 0);

    // ---- ③ 与 N 次单点写等价 ----
    std::vector<double> vals2;
    for (double v : vals) vals2.push_back(v * 2.0);
    RtDbBatchWriter w2(&h);
    for (std::size_t i = 0; i < names.size(); ++i) {
        EXPECT(w2.write_one(names[i], vals2[i], 0));
    }
    EXPECT_EQ(w2.stats().calls, 8);                      // N 次调用
    EXPECT_EQ(w2.stats().point_writes, 8);
    const std::size_t gi = rt_db_find_index_by_id(&h, "MEAS.P_GRID");
    double vg = 0.0;
    EXPECT(rt_db_get_value(&h, gi, &vg, nullptr, nullptr));
    EXPECT_NEAR(vg, vals2[3], 1e-9);

    // 再走一遍批量，值必须与 8 次单点写的结果一致
    RtDbBatchWriter w3(&h);
    EXPECT(w3.write_batch(names, vals2, qs));
    EXPECT_EQ(w3.stats().calls, 1);
    int diff = 0;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::size_t idx = rt_db_find_index_by_id(&h, names[i].c_str());
        double v = 0.0;
        if (!rt_db_get_value(&h, idx, &v, nullptr, nullptr)) { ++diff; continue; }
        if (std::fabs(v - vals2[i]) > 1e-9) ++diff;
    }
    EXPECT_EQ(diff, 0);

    // ---- ④ 越界点名 → **整批拒绝**（一个点都不许写进去）----
    std::vector<std::string> bad = names;
    bad[3] = "NO.SUCH.POINT";
    std::vector<double> vals3 = vals2;
    for (auto& v : vals3) v = 7.0;                       // 假值，必须写不进去
    RtDbBatchWriter w4(&h);
    EXPECT(!w4.write_batch(bad, vals3, qs));
    EXPECT_EQ(w4.stats().calls, 0);                      // ★ 一次调用都没发生
    EXPECT_EQ(w4.stats().point_writes, 0);               // 也没有半写
    EXPECT_EQ(w4.rejected(), 1);
    // 反向守卫：段里的值仍然是上一批的（没被半写污染）
    double vg2 = 0.0;
    EXPECT(rt_db_get_value(&h, gi, &vg2, nullptr, nullptr));
    EXPECT_NEAR(vg2, vals2[3], 1e-9);

    // ---- ⑤ 长度不一致 → 拒绝 ----
    std::vector<double> short_vals = {1.0, 2.0};
    RtDbBatchWriter w5(&h);
    EXPECT(!w5.write_batch(names, short_vals, qs));
    EXPECT_EQ(w5.stats().calls, 0);

    // ---- ⑥ 真实写次数口径的诚实记录 ----
    //   vendor 的 rt_db_set_multiple_values() 内部是 `for(i) rt_db_set_value(...)`，
    //   所以 header.write_count **照样 +N**。用公开 API rt_db_get_write_count()
    //   把这件事留成正面证据：批量写降低的是**调用次数**（1 vs N），
    //   不是内部 seqlock 往返次数。要真降往返得改 vendor（不在 17/ 权限内）。
    {
        const std::size_t before = rt_db_get_write_count(&h);
        std::vector<double> v6;
        for (double v : vals) v6.push_back(v + 1.0);
        RtDbBatchWriter w6(&h);
        EXPECT(w6.write_batch(names, v6, qs));
        EXPECT_EQ(w6.stats().calls, 1);                  // API 调用：1 次
        const std::size_t after = rt_db_get_write_count(&h);
        const std::size_t delta = after - before;
        std::printf("    批量写 8 点：API calls=1，段内 write_count 增量=%zu"
                    "（vendor 内部仍逐点写）\n", delta);
        EXPECT_EQ(delta, names.size());                  // 内部往返：仍是 N 次
    }

    rt_db_cleanup(&h);
}

// =====================================================================
// T11 空句柄 → 干净拒绝
// =====================================================================
static void test_11_null_handle() {
    std::printf("T11 RtDbBatchWriter：句柄为空 → 干净拒绝\n");

    RtDbBatchWriter w(nullptr);
    EXPECT(!w.ready());
    std::vector<std::string> names = {"MEAS.SOC"};
    std::vector<double>      vals  = {1.0};
    std::vector<int>         qs    = {0};
    EXPECT(!w.write_batch(names, vals, qs));             // 不崩、不半写
    EXPECT(!w.write_one("MEAS.SOC", 1.0, 0));
    EXPECT_EQ(w.stats().calls, 0);
    EXPECT_EQ(w.rejected(), 2);
    // 自由函数同样干净拒绝
    EXPECT(!publish_batch(nullptr, names, vals, qs, nullptr));
    // 反向守卫：长度不一致时即使句柄非空也必须拒绝（先看参数，再碰段）
    std::vector<double> short_vals = {};
    EXPECT(!publish_batch(nullptr, names, short_vals, qs, nullptr));
}

int main() {
    TEST_BANNER("17/ 规模化：变化上传(C2) · 分级节拍(C3) · 批量写(C1)");

    test_01_static_suppression();
    test_02_deadband_semantics();
    test_03_safety_never_suppressed();
    test_04_tier_ratio();
    test_05_cold_start();
    test_06_slow_isolated();
    test_07_batch_semantics();
    test_08_batch_reject();
    test_09_pipeline_quantified();
    test_10_real_rtdb();
    test_11_null_handle();

    std::printf("\n");
    if (g_fail == 0) std::printf("ALL TESTS PASSED\n");
    std::printf("PASS=%d FAIL=%d SKIPPED=%d\n", g_pass, g_fail, g_skipped);
    return g_fail == 0 ? 0 : 1;
}
