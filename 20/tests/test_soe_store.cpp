// =====================================================================
// 20/ 单元测试 —— SOE 持久化（SoeStore）
//
//   S01: 追加写 + 内存缓存 + 全量读回
//   S02: ★ 重启（重新构造）后读回全部 N 条，顺序与内容逐条一致
//   S03: 重启后 seq 连续（不重置）
//   S04: ★ 超容量上限**不丢**（落盘先于覆盖；内存淘汰 ≠ 数据丢失）+ 反向守卫
//   S05: 按 时间范围 / 等级 / 来源 检索
//   S06: 文件格式稳定（header / 一行一事件 / 转义往返 / 外部 parse_file）
//   S07: 与告警装配器桥接（SET/CLEAR 成对落盘）
//   S08: 损坏行 → 显式报错并指出位置（不静默跳过）
//
// 编译：见 20/scripts/build_test.bat
// =====================================================================

#include "soe_store.h"
#include "alarm_model.h"
#include "alarm_assembler.h"

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

static const char* kFile = "build/test_soe.log";

static SoeStoreRecord mk(long long /*seq*/, double t, SoeLevel lv, SoeSource src,
                         SoeCode code, bool set, double value, const std::string& msg) {
    SoeStoreRecord r;
    r.t = t; r.level = lv; r.source = src; r.code = code;
    r.is_set = set; r.value = value; r.message = msg;
    return r;
}

static bool same_record(const SoeStoreRecord& a, const SoeStoreRecord& b) {
    return a.seq == b.seq && a.level == b.level && a.source == b.source &&
           a.code == b.code && a.is_set == b.is_set &&
           std::fabs(a.t - b.t) <= 1e-6 && std::fabs(a.value - b.value) <= 1e-9 &&
           a.message == b.message;
}

// =====================================================================
static void test_01_append_readback() {
    std::printf("--- S01 追加写 / 读回 ---\n");
    std::remove(kFile);
    SoeStore st;
    std::string err;
    EXPECT(st.open(kFile, &err));
    EXPECT(err.empty());

    const int N = 20;
    for (int i = 0; i < N; ++i) {
        SoeStoreRecord r = mk(0, (double)i, i % 2 ? SoeLevel::kInfo : SoeLevel::kError,
                              i % 3 == 0 ? SoeSource::kComm : SoeSource::kFsm,
                              i % 2 ? SoeCode::kFsmTransition : SoeCode::kCommLostBms,
                              i % 2 == 0, 100.0 + i, std::string("evt ") + std::to_string(i));
        EXPECT(st.append(r, &err));
    }
    EXPECT_EQ(st.size(), N);
    EXPECT_EQ(st.memory_size(), N);
    EXPECT_EQ(st.last_seq(), N);
    EXPECT_EQ(st.dropped_from_memory(), 0);

    auto all = st.all();
    EXPECT_EQ(all.size(), N);
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(all[i].seq, i + 1);                  // seq 从 1 起，单调
        EXPECT_NEAR(all[i].t, (double)i, 1e-9);
        EXPECT_STR(all[i].message, std::string("evt ") + std::to_string(i));
    }
}

// =====================================================================
static void test_02_restart_readback() {
    std::printf("--- S02 重启后逐条一致 ---\n");
    // 写入 N 条
    std::remove(kFile);
    const int N = 64;
    std::vector<SoeStoreRecord> written;
    {
        SoeStore st;
        std::string err;
        EXPECT(st.open(kFile, &err));
        for (int i = 0; i < N; ++i) {
            SoeStoreRecord r = mk(0, 0.5 * i, (SoeLevel)(i % 5), (SoeSource)(i % 9),
                                  SoeCode::kSocLowStart, i % 2 == 0, 0.1 * i,
                                  std::string("msg,") + std::to_string(i) + "\"q\"");
            st.append(r, &err);
        }
        written = st.all();
        EXPECT_EQ(written.size(), N);
    }   // 析构 = 关闭（模拟进程退出）

    // 重新构造 → 读回
    SoeStore st2;
    std::string err;
    EXPECT(st2.open(kFile, &err));
    EXPECT(err.empty());
    auto back = st2.all();
    EXPECT_EQ(back.size(), N);
    // 顺序与内容逐条一致
    int identical = 0;
    for (int i = 0; i < N && i < (int)back.size(); ++i)
        if (same_record(written[i], back[i])) ++identical;
    EXPECT_EQ(identical, N);
    EXPECT(same_record(written[0], back[0]));
    EXPECT(same_record(written[N - 1], back[N - 1]));
}

// =====================================================================
static void test_03_seq_continues() {
    std::printf("--- S03 重启后 seq 连续 ---\n");
    SoeStore st;
    std::string err;
    EXPECT(st.open(kFile, &err));
    const long long before = st.last_seq();
    EXPECT_EQ(before, 64);
    SoeStoreRecord r = mk(0, 999.0, SoeLevel::kWarn, SoeSource::kSafety,
                          SoeCode::kSocHighStart, true, 0.95, "after restart");
    EXPECT(st.append(r, &err));
    EXPECT_EQ(st.last_seq(), 65);
    EXPECT_EQ(st.all().back().seq, 65);
}

// =====================================================================
static void test_04_no_loss_over_capacity() {
    std::printf("--- S04 超容量不丢 ---\n");
    std::remove(kFile);
    SoeStore::Config cfg;
    cfg.memory_capacity = 8;          // 故意开到很小
    SoeStore st(cfg);
    std::string err;
    EXPECT(st.open(kFile, &err));

    const int N = 500;
    for (int i = 0; i < N; ++i) {
        st.append(mk(0, (double)i, SoeLevel::kInfo, SoeSource::kFsm,
                     SoeCode::kFsmTransition, true, (double)i, "x"), &err);
    }
    // 内存有界（热缓存只有 8 条）
    EXPECT_EQ(st.memory_size(), 8);
    // ★ 反向守卫：内存确实淘汰过 —— 否则"读回 500 条"测的是别的东西
    EXPECT(st.dropped_from_memory() > 0);
    EXPECT_EQ(st.dropped_from_memory(), N - 8);
    EXPECT(st.memory_size() < st.size());
    // ★ 硬判据：一条都没丢
    EXPECT_EQ(st.size(), N);
    auto all = st.all();
    EXPECT_EQ(all.size(), N);
    // 逐条 seq / 时间 / 值都在，且顺序正确
    int ok = 0;
    for (int i = 0; i < N && i < (int)all.size(); ++i)
        if (all[i].seq == i + 1 && std::fabs(all[i].t - i) <= 1e-9) ++ok;
    EXPECT_EQ(ok, N);
    // 最先被淘汰的那几条（内存里已没有）仍能从文件读回
    EXPECT(all[0].seq == 1);
    EXPECT(all[0].message == "x");
}

// =====================================================================
static void test_05_query() {
    std::printf("--- S05 检索 ---\n");
    SoeStore st;
    std::string err;
    EXPECT(st.open(kFile, &err));       // 复用 S04 的 500 条 (t=0..499, FSM/INFO)

    // 时间范围
    auto r1 = st.query_range(100.0, 109.0);
    EXPECT_EQ(r1.size(), 10);
    EXPECT_EQ(r1.front().seq, 101);
    EXPECT_EQ(r1.back().seq, 110);

    // 等级：全部 INFO → >= WARN 为空、>= INFO 为 500
    EXPECT_EQ(st.query_level(SoeLevel::kWarn).size(), 0);
    EXPECT_EQ(st.query_level(SoeLevel::kInfo).size(), 500);

    // 来源
    EXPECT_EQ(st.query_source(SoeSource::kFsm).size(), 500);
    EXPECT_EQ(st.query_source(SoeSource::kComm).size(), 0);

    // 组合检索
    auto r2 = st.query(200.0, 299.0, SoeLevel::kInfo, SoeSource::kFsm);
    EXPECT_EQ(r2.size(), 100);
}

// =====================================================================
static void test_06_format_stable() {
    std::printf("--- S06 文件格式稳定 ---\n");
    std::remove(kFile);
    SoeStore st;
    std::string err;
    EXPECT(st.open(kFile, &err));

    // 含逗号 / 引号 / 换行 / 反斜杠的消息 —— 必须仍是一条一行且可往返
    const std::string tricky = "a,b \"c\" d\\e\nf";
    st.append(mk(0, 1.25, SoeLevel::kError, SoeSource::kComm, SoeCode::kCommLostBms,
                 true, 3.5, tricky), &err);
    st.append(mk(0, 2.50, SoeLevel::kError, SoeSource::kComm, SoeCode::kCommLostBms,
                 false, 3.5, "cleared"), &err);

    // 头行存在且是版本行
    {
        std::ifstream f(kFile, std::ios::in | std::ios::binary);
        std::string first;
        std::getline(f, first);
        EXPECT(first.rfind("# EMS_SOE v1", 0) == 0);
        // 总行数 = 1 头行 + 2 事件行（换行被转义 → 不会多出物理行）
        int lines = 0;
        std::string ln;
        if (!first.empty()) ++lines;
        while (std::getline(f, ln)) ++lines;
        EXPECT_EQ(lines, 3);
    }

    // 外部工具口径：parse_file 独立解析
    std::vector<SoeStoreRecord> parsed;
    std::string perr;
    EXPECT(SoeStore::parse_file(kFile, parsed, &perr));
    EXPECT(perr.empty());
    EXPECT_EQ(parsed.size(), 2);
    EXPECT_STR(parsed[0].message, tricky);            // 转义往返一致
    EXPECT_NEAR(parsed[0].t, 1.25, 1e-6);
    EXPECT_NEAR(parsed[0].value, 3.5, 1e-9);
    EXPECT(parsed[0].level == SoeLevel::kError);
    EXPECT(parsed[1].is_set == false);

    // 行格式与文档一致：seq,t,level,source,code,kind,value,"message"
    {
        std::ifstream f(kFile, std::ios::in | std::ios::binary);
        std::string ln;
        std::getline(f, ln);   // header
        std::getline(f, ln);   // 第 1 条（SET）
        EXPECT(ln.rfind("1,1.250000,ERROR,COMM,COMM_LOST_BMS,SET,3.5,\"", 0) == 0);
    }
}

// =====================================================================
static void test_07_alarm_bridge() {
    std::printf("--- S07 与告警装配器桥接 ---\n");
    std::remove(kFile);
    SoeStore st;
    std::string err;
    EXPECT(st.open(kFile, &err));

    AlarmAssembler a;
    AlarmContext ac;
    ac.soc_low = 0.10; ac.soc_high = 0.90;
    ac.transformer_capacity_kw = 0.0;   // 关掉无关判定
    ac.demand_target_kw = 0.0;
    ac.forbid_reverse = false;
    a.set_context(ac);

    int pushed = 0;
    for (int i = 0; i < 30; ++i) {
        AlarmInput in;
        in.t = (double)i;
        in.state = EmsState::kNormal;
        in.soc = 0.5;
        in.p_grid_kw = 100.0;
        in.p_load_kw = 100.0;
        in.bms_comm_ok = !(i >= 5 && i < 15);
        for (const auto& ev : a.update(in)) { st.append(ev, &err); ++pushed; }
    }
    EXPECT_EQ(pushed, 2);                     // SET + CLEAR
    auto rows = st.all();
    EXPECT_EQ(rows.size(), 2);
    EXPECT(rows[0].is_set);
    EXPECT(rows[0].code == SoeCode::kCommLostBms);
    EXPECT(rows[0].level == SoeLevel::kError);          // FAULT → ERROR（日志通道）
    EXPECT_STR(rows[0].message, "bms comm lost");
    EXPECT(!rows[1].is_set);
    EXPECT(rows[1].code == SoeCode::kCommRestored);
}

// =====================================================================
static void test_08_corrupt_line_explicit() {
    std::printf("--- S08 损坏行显式报错 ---\n");
    std::remove(kFile);
    {
        std::ofstream f(kFile, std::ios::out | std::ios::binary);
        f << SoeStore::header_line() << "\n";
        f << "1,0.500000,ERROR,COMM,COMM_LOST_BMS,SET,0,\"ok\"\n";
        f << "THIS-IS-NOT-A-VALID-LINE\n";                 // 损坏
        f << "3,1.000000,ERROR,COMM,COMM_LOST_BMS,CLEAR,0,\"ok2\"\n";
    }
    SoeStore st;
    std::string err;
    EXPECT(!st.open(kFile, &err));                 // 必须失败
    EXPECT(!err.empty());
    // 错误信息必须指出位置（offset / 字段），不能只是 "error"
    EXPECT(err.find("corrupt SOE line") != std::string::npos);
    EXPECT(err.find("offset") != std::string::npos);
    std::printf("      open() 报错：%s\n", err.c_str());
}

int main() {
    std::printf("=== 20/ SOE 持久化 单元测试 ===\n\n");
    test_01_append_readback();
    test_02_restart_readback();
    test_03_seq_continues();
    test_04_no_loss_over_capacity();
    test_05_query();
    test_06_format_stable();
    test_07_alarm_bridge();
    test_08_corrupt_line_explicit();

    std::printf("\n");
    if (g_fail == 0) std::printf("ALL TESTS PASSED\n");
    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    {
        std::ofstream f("build/test_soe_store_status.txt", std::ios::out | std::ios::binary);
        if (f.is_open()) f << "PASS=" << g_pass << " FAIL=" << g_fail << "\n";
    }
    return g_fail == 0 ? 0 : 1;
}
