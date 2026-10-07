// =====================================================================
// 20/ 单元测试 —— 配置持久化（ConfigStore）
//
//   C01: 字段级往返（set → save → load → 逐字段一致）
//   C02: ★ 重启后值不丢
//   C03: 变更台账（字段名 + 旧值 + 新值 + 时间 + 谁）
//   C04: ★ 缺字段 → 显式报错并**指名**（不允许静默用默认值）+ 反向守卫
//   C05: 文件损坏 → 显式报错并指出行号
//   C06: 版本不匹配 → 显式报错
//   C07: ★ 与 04/ 的真实策略参数对接（EmsRuntime 调参 → 落盘 → 回读 → 回灌）
//   C08: 文件里有、必填清单里没有的字段被如实收集（不静默丢）
//   C09: get() 对不存在字段返回 false（无默认值语义）
//
// 编译：见 20/scripts/build_test.bat
// =====================================================================

#include "config_store.h"
#include "realtime_loop.h"     // EmsRuntime（C07 用，对接 04/ 的真实策略参数）
#include "data_models.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace ems;

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT(cond)                                                      \
    do {                                                                  \
        if (cond) { ++g_pass; }                                           \
        else {                                                            \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #cond << std::endl;                     \
        }                                                                 \
    } while (0)

#define EXPECT_EQ(a, b)                                                   \
    do {                                                                  \
        long long va = (long long)(a), vb = (long long)(b);               \
        if (va == vb) { ++g_pass; }                                       \
        else {                                                            \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va << " vs " << #b << "="  \
                      << vb << std::endl;                                 \
        }                                                                 \
    } while (0)

#define EXPECT_NEAR(a, b, eps)                                            \
    do {                                                                  \
        double va = (a), vb = (b);                                        \
        if (std::fabs(va - vb) <= (eps)) { ++g_pass; }                    \
        else {                                                            \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va << " vs " << #b << "="  \
                      << vb << " (eps " << (eps) << ")" << std::endl;     \
        }                                                                 \
    } while (0)

#define EXPECT_STR(a, b)                                                  \
    do {                                                                  \
        std::string va = (a), vb = (b);                                   \
        if (va == vb) { ++g_pass; }                                       \
        else {                                                            \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "='" << va << "' vs " << #b       \
                      << "='" << vb << "'" << std::endl;                  \
        }                                                                 \
    } while (0)

static const char* kFile = "build/test_config.ini";

static void write_file(const std::string& content) {
    std::remove(kFile);
    std::ofstream f(kFile, std::ios::out | std::ios::binary);
    f << content;
}

// =====================================================================
static void test_01_roundtrip() {
    std::printf("--- C01 字段级往返 ---\n");
    std::remove(kFile);
    ConfigStore a(kFile);
    a.require("S07_PEAK_VALLEY", "P_discharge");
    a.require("S04_DEMAND_MGMT", "d_target_kw");
    a.require("S05_ANTI_REVERSE", "P_rev_limit");

    std::string err;
    EXPECT(a.set("S07_PEAK_VALLEY", "P_discharge", 80.0, "operator", 100.0, &err));
    EXPECT(a.set("S04_DEMAND_MGMT", "d_target_kw", 400.0, "operator", 100.0, &err));
    EXPECT(a.set("S05_ANTI_REVERSE", "P_rev_limit", 0.05, "engineer", 101.0, &err));
    EXPECT(a.save(&err));
    EXPECT(err.empty());

    ConfigStore b(kFile);
    b.require("S07_PEAK_VALLEY", "P_discharge");
    b.require("S04_DEMAND_MGMT", "d_target_kw");
    b.require("S05_ANTI_REVERSE", "P_rev_limit");
    const ConfigLoadResult lr = b.load();
    EXPECT(lr.ok);
    EXPECT(lr.error.empty());

    double v = -1;
    EXPECT(b.get("S07_PEAK_VALLEY", "P_discharge", &v)); EXPECT_NEAR(v, 80.0, 1e-9);
    EXPECT(b.get("S04_DEMAND_MGMT", "d_target_kw", &v)); EXPECT_NEAR(v, 400.0, 1e-9);
    EXPECT(b.get("S05_ANTI_REVERSE", "P_rev_limit", &v)); EXPECT_NEAR(v, 0.05, 1e-9);
    EXPECT_EQ(b.size(), 3);
    EXPECT(b.file_version() == "1");
}

// =====================================================================
static void test_02_restart_keeps_values() {
    std::printf("--- C02 重启后值不丢 ---\n");
    // C01 落盘的文件直接当"上一次运行"的产物
    ConfigStore b(kFile);
    b.require("S07_PEAK_VALLEY", "P_discharge");
    b.require("S04_DEMAND_MGMT", "d_target_kw");
    b.require("S05_ANTI_REVERSE", "P_rev_limit");
    EXPECT(b.load().ok);

    // 改一个值并再存一次
    std::string err;
    EXPECT(b.set("S07_PEAK_VALLEY", "P_discharge", 120.0, "operator", 200.0, &err));
    EXPECT(b.save(&err));

    // 再重启
    ConfigStore c(kFile);
    c.require("S07_PEAK_VALLEY", "P_discharge");
    c.require("S04_DEMAND_MGMT", "d_target_kw");
    c.require("S05_ANTI_REVERSE", "P_rev_limit");
    EXPECT(c.load().ok);
    double v = 0;
    EXPECT(c.get("S07_PEAK_VALLEY", "P_discharge", &v));
    EXPECT_NEAR(v, 120.0, 1e-9);          // 新值保住了
    EXPECT(c.get("S04_DEMAND_MGMT", "d_target_kw", &v));
    EXPECT_NEAR(v, 400.0, 1e-9);          // 未动的字段也在
}

// =====================================================================
static void test_03_change_log() {
    std::printf("--- C03 变更台账 ---\n");
    std::remove(kFile);
    ConfigStore a(kFile);
    std::string err;
    a.set("S07_PEAK_VALLEY", "P_discharge", 80.0, "operator", 100.0, &err);   // created
    a.set("S07_PEAK_VALLEY", "P_discharge", 60.0, "engineer", 150.0, &err);   // 80 → 60
    a.set("S07_PEAK_VALLEY", "P_discharge", 60.0, "engineer", 160.0, &err);   // 未变 → 不记
    a.set("S07_PEAK_VALLEY", "P_discharge", 90.0, "operator", 200.0, &err);   // 60 → 90

    EXPECT_EQ(a.changes().size(), 3);      // 第 3 次未变，不记

    // 定位"字段名 + 旧值 + 新值 + 时间"
    const ConfigChange& c1 = a.changes()[1];
    EXPECT(c1.field == "S07_PEAK_VALLEY.P_discharge");
    EXPECT_NEAR(c1.old_value, 80.0, 1e-9);
    EXPECT_NEAR(c1.new_value, 60.0, 1e-9);
    EXPECT_NEAR(c1.t, 150.0, 1e-9);
    EXPECT(c1.who == "engineer");
    EXPECT(!c1.created);
    EXPECT(a.changes()[0].created);        // 首次写入 = 字段此前不存在

    // 台账也要落盘 + 回读
    EXPECT(a.save(&err));
    ConfigStore b(kFile);
    b.require("S07_PEAK_VALLEY", "P_discharge");
    EXPECT(b.load().ok);
    EXPECT_EQ(b.changes().size(), 3);
    EXPECT(b.changes()[2].who == "operator");
    EXPECT_NEAR(b.changes()[2].new_value, 90.0, 1e-9);
}

// =====================================================================
static void test_04_missing_field_explicit() {
    std::printf("--- C04 缺字段显式报错 ---\n");
    write_file("# EMS_CONFIG v1\n"
               "VERSION|1\n"
               "FIELD|S07_PEAK_VALLEY|P_discharge|80\n");

    ConfigStore a(kFile);
    a.require("S07_PEAK_VALLEY", "P_discharge");
    a.require("S04_DEMAND_MGMT", "d_target_kw");      // 文件里**没有**这个字段
    const ConfigLoadResult lr = a.load();

    EXPECT(!lr.ok);                                    // 必须失败
    EXPECT_EQ(lr.missing_fields.size(), 1);
    EXPECT_STR(lr.missing_fields[0], "S04_DEMAND_MGMT.d_target_kw");
    // 错误信息必须**指名**是哪个字段
    EXPECT(lr.error.find("S04_DEMAND_MGMT.d_target_kw") != std::string::npos);
    std::printf("      load() 报错：%s\n", lr.error.c_str());

    // ★ 反向守卫：字段缺失时**不允许**静默取默认值 —— get() 必须失败
    double v = 12345.0;
    EXPECT(!a.get("S04_DEMAND_MGMT", "d_target_kw", &v));
    EXPECT_NEAR(v, 12345.0, 1e-9);                     // out 未被改动
    // 而存在的字段（在有缺失时）也不应被"部分加载"进内存 —— 原子性
    EXPECT(!a.get("S07_PEAK_VALLEY", "P_discharge", &v));
    EXPECT_EQ(a.size(), 0);
}

// =====================================================================
static void test_05_corrupt_line() {
    std::printf("--- C05 损坏行显式报错 ---\n");
    write_file("# EMS_CONFIG v1\n"
               "VERSION|1\n"
               "FIELD|S07_PEAK_VALLEY|P_discharge|80\n"
               "FIELD|BROKEN|LINE\n"
               "FIELD|S04_DEMAND_MGMT|d_target_kw|400\n");
    ConfigStore a(kFile);
    a.require("S07_PEAK_VALLEY", "P_discharge");
    a.require("S04_DEMAND_MGMT", "d_target_kw");
    const ConfigLoadResult lr = a.load();
    EXPECT(!lr.ok);
    EXPECT_EQ(lr.corrupt_line, 4);                     // 指出行号
    EXPECT(lr.error.find("line 4") != std::string::npos);
    std::printf("      load() 报错：%s\n", lr.error.c_str());

    // 未知记录类型
    write_file("# EMS_CONFIG v1\nVERSION|1\nWAT|xxx\n");
    ConfigStore b(kFile);
    const ConfigLoadResult lr2 = b.load();
    EXPECT(!lr2.ok);
    EXPECT(lr2.error.find("unknown record type") != std::string::npos);
}

// =====================================================================
static void test_06_version_mismatch() {
    std::printf("--- C06 版本不匹配 ---\n");
    write_file("# EMS_CONFIG v99\nVERSION|99\nFIELD|A|B|1\n");
    ConfigStore a(kFile);
    const ConfigLoadResult lr = a.load();
    EXPECT(!lr.ok);
    EXPECT(lr.error.find("version mismatch") != std::string::npos);
    EXPECT(lr.error.find("99") != std::string::npos);
    std::printf("      load() 报错：%s\n", lr.error.c_str());
}

// =====================================================================
static void test_07_real_strategy_params() {
    std::printf("--- C07 对接 04/ 真实策略参数 ---\n");
    // 用一个真实的 EmsRuntime（它注册了 04/ 的 5 个策略 + 计划跟踪）
    EmsRuntime rt;
    rt.init();

    // 现场调参：改两个真实参数
    std::string err;
    bool changed = false;
    try {
        rt.manager().set_param(strategy_id::kPeakValley, "P_discharge", 88.0);
        rt.manager().set_param(strategy_id::kDemandMgmt, "d_target_kw", 450.0);
        changed = true;
    } catch (...) { changed = false; }
    EXPECT(changed);

    // 导出当前内存配置（含 __weight__ 内部键，一并落盘 —— 它是运行模式的一部分）
    std::vector<std::string> ids = rt.manager().list_ids();
    EXPECT(ids.size() >= 6);
    std::vector<ConfigField> snapshot;

    std::remove(kFile);
    ConfigStore cs(kFile);
    for (const auto& id : ids) {
        StrategyPtr sp = rt.manager().get_strategy(id);
        if (!sp) continue;
        for (const auto& kv : sp->params()) {
            cs.put_silent(id, kv.first, kv.second);
            cs.require(id, kv.first);
            ConfigField f; f.group = id; f.name = kv.first; f.value = kv.second;
            snapshot.push_back(f);
        }
    }
    EXPECT(!snapshot.empty());
    // 至少要有我们改过的这两个真实参数（其余策略多数用默认值、没有显式参数）
    int found_pv = 0, found_dm = 0;
    for (const auto& f : snapshot) {
        if (f.group == strategy_id::kPeakValley && f.name == "P_discharge") ++found_pv;
        if (f.group == strategy_id::kDemandMgmt && f.name == "d_target_kw") ++found_dm;
    }
    EXPECT_EQ(found_pv, 1);
    EXPECT_EQ(found_dm, 1);
    EXPECT(cs.save(&err));

    // "重启"：新 store，重建同一份必填清单，回读
    ConfigStore cs2(kFile);
    for (const auto& f : snapshot) cs2.require(f.group, f.name);
    const ConfigLoadResult lr = cs2.load();
    EXPECT(lr.ok);
    EXPECT(lr.error.empty());
    EXPECT_EQ(cs2.size(), snapshot.size());

    // 逐字段一致
    int same = 0;
    for (const auto& f : snapshot) {
        double v = 0;
        if (cs2.get(f.group, f.name, &v) && std::fabs(v - f.value) <= 1e-9) ++same;
    }
    EXPECT_EQ(same, (int)snapshot.size());

    // 回灌到一个全新的 runtime：参数必须生效
    EmsRuntime rt2;
    rt2.init();
    for (const auto& f : cs2.fields()) {
        rt2.manager().set_param(f.group, f.name, f.value);
    }
    StrategyPtr pv = rt2.manager().get_strategy(strategy_id::kPeakValley);
    EXPECT(pv != nullptr);
    EXPECT_NEAR(pv->get_param("P_discharge", -1.0), 88.0, 1e-9);
    StrategyPtr dm = rt2.manager().get_strategy(strategy_id::kDemandMgmt);
    EXPECT(dm != nullptr);
    EXPECT_NEAR(dm->get_param("d_target_kw", -1.0), 450.0, 1e-9);
}

// =====================================================================
static void test_08_unknown_fields() {
    std::printf("--- C08 未知字段如实收集 ---\n");
    write_file("# EMS_CONFIG v1\n"
               "VERSION|1\n"
               "FIELD|A|B|1\n"
               "FIELD|X|Y|2\n");
    ConfigStore a(kFile);
    a.require("A", "B");                               // 只认 A.B
    const ConfigLoadResult lr = a.load();
    EXPECT(lr.ok);
    EXPECT_EQ(lr.unknown_fields.size(), 1);
    EXPECT(lr.unknown_fields[0] == "X.Y");
    EXPECT_EQ(a.size(), 2);                            // 未知字段也保留（不静默丢）
}

// =====================================================================
static void test_09_no_default() {
    std::printf("--- C09 无默认值语义 ---\n");
    write_file("# EMS_CONFIG v1\nVERSION|1\nFIELD|A|B|1\n");
    ConfigStore a(kFile);
    a.require("A", "B");
    EXPECT(a.load().ok);
    double v = 0;
    EXPECT(!a.get("NOPE", "NONE", &v));                // 不存在 → false
    EXPECT(!a.get("A", "Z", &v));
    EXPECT(!a.has("A", "Z"));
    EXPECT(a.has("A", "B"));
    EXPECT(a.get("A", "B", &v));
    EXPECT_NEAR(v, 1.0, 1e-9);
    // 没有 get_or_default 这种接口 —— 缺字段只有"报错"一条路（见 C04）
}

int main() {
    std::printf("=== 20/ 配置持久化 单元测试 ===\n\n");
    test_01_roundtrip();
    test_02_restart_keeps_values();
    test_03_change_log();
    test_04_missing_field_explicit();
    test_05_corrupt_line();
    test_06_version_mismatch();
    test_07_real_strategy_params();
    test_08_unknown_fields();
    test_09_no_default();

    std::printf("\n");
    if (g_fail == 0) std::printf("ALL TESTS PASSED\n");
    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    {
        std::ofstream f("build/test_config_store_status.txt", std::ios::out | std::ios::binary);
        if (f.is_open()) f << "PASS=" << g_pass << " FAIL=" << g_fail << "\n";
    }
    return g_fail == 0 ? 0 : 1;
}
