// =====================================================================
// P3 / EXT 外部设定 —— **跨共享内存边界**的测试（T73~T76）
//
// ---------------------------------------------------------------------
// 为什么必须另起一个可执行文件、而且真的建一个共享内存段
// ---------------------------------------------------------------------
// test_iec104.cpp 全是「库自带主站 ↔ 我们的从站」的自环，数据源走**内存实现**，
// 它验证不了"值到底有没有落到段里的那个点上"。本项目在 A1 上栽过一次：
// 仿真适配器的 DeviceLimits 由调用方注入，于是"设备侧真读"那条路
// **一次都没被走过** —— 三层测试全绿，照样漏掉了 BMS 禁充放。
//
//   **跨内存边界 + 夹具注入 = 测试盲区。**
//
// 所以本文件不做任何注入：
//   写 → `RtDbExtWriter`（真实 rt_db_set_value）
//   读 → `rt_db_get_value`，并且**按名字反查索引**再对账
//        （索引是运行时坐标，只有名字能发现"点表中间被插了点"）
//
// ---------------------------------------------------------------------
// ⚠ 本测试会 `ems_rt_db_setup(reset=true)` —— **清空整个共享内存段**
// ---------------------------------------------------------------------
// 跑之前请确认没有别的进程在用（初始化器 / 网关 / 联调脚本）。
// 这是它的设计意图：要在一个确定性的初始状态上验判据。
//
// 编译（见 P3/scripts/build_test.bat）：
//   g++ -std=c++17 -DNOMINMAX -I src -I ../../07/src/rtdb -I ../../07/vendor/rt_db
//       tests/test_ext_rtdb.cpp build/{rt_db_api,ems_point_table,ems_rt_db_setup}.o
// =====================================================================

// ★ 顺序有讲究：rtdb_source.h 内部会**先** include rtdb_quality.h，
//   而它必须在任何 C++ 标准库头之前（rt_db_structs.h 的 C11 原子宏会破坏
//   libstdc++ 里 std::atomic_* 的声明）。这一行必须排第一。
#include "rtdb_source.h"

#include "rtdb_ext_sink.h"      // RtDbExtWriter / wall_clock_s
#include "ext_setpoints.h"      // ems::load_ext_setpoints / narrow_interval_by_ext
#include "ems_rt_db_setup.h"    // ems_rt_db_setup（建段 + 注册点表）

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT(cond)                                                       \
    do {                                                                   \
        if (cond) { ++g_pass; }                                            \
        else {                                                             \
            ++g_fail;                                                      \
            std::fprintf(stderr, "  FAIL  %s:%d : %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

#define EXPECT_NEAR(a, b, eps)                                             \
    do {                                                                   \
        const double va = (a), vb = (b);                                   \
        if (std::fabs(va - vb) <= (eps)) { ++g_pass; }                     \
        else {                                                             \
            ++g_fail;                                                      \
            std::fprintf(stderr, "  FAIL  %s:%d : %s=%.9g vs %s=%.9g\n",   \
                         __FILE__, __LINE__, #a, va, #b, vb);              \
        }                                                                  \
    } while (0)

// 按**名字**读一个点（不按编译期索引 —— 那才测得到"名字 ↔ 索引"是否一致）
static bool read_named(rt_db_handle_t* h, const char* name, double* v,
                       long* q_out = nullptr) {
    const std::size_t idx = rt_db_find_index_by_id(h, name);
    if (idx == static_cast<std::size_t>(-1)) return false;
    long q = 0;
    struct timespec ts{};
    const bool ok = rt_db_get_value(h, idx, v, &q, &ts);
    if (q_out != nullptr) *q_out = q;
    return ok;
}

int main() {
    std::printf("=== P3 EXT setpoint over RT_DB (T73~T76) ===\n\n");

    // ------------------------------------------------------------------
    // 建段（reset=true：要一个确定性的初始状态）
    // ------------------------------------------------------------------
    bool created = false;
    if (!ems_rt_db_setup(/*reset=*/true, &created)) {
        std::printf("[FATAL] ems_rt_db_setup 失败 —— 段建不起来，后面全无意义\n");
        return 1;
    }
    std::printf("[info] 段已重建（created=%d），点表 %d 点\n",
                created ? 1 : 0, static_cast<int>(EMS_POINT_COUNT));

    iec104::RtDbReadOnlySource src;
    if (!src.open_owned(nullptr)) {
        std::printf("[FATAL] rt_db_init 失败\n");
        return 1;
    }
    rt_db_handle_t* h = src.borrowed_handle();

    iec104::RtDbExtWriter w(h);

    // 读回调：直接吃真实 rt_db_get_value（不注入任何东西）
    auto rdb_read = [h](int idx, double& v) -> bool {
        long q = 0;
        struct timespec ts{};
        return rt_db_get_value(h, static_cast<std::size_t>(idx), &v, &q, &ts);
    };

    // ------------------------------------------------------------------
    // T73 落点自检：分区 + 名字 ↔ 索引逐位一致
    // ------------------------------------------------------------------
    std::printf("-- T73 writer self-check (zone + name<->index) --\n");
    {
        EXPECT(w.is_open());
        EXPECT(w.self_check() == 0);        // 0 = 六个点全在 EXT 区且点名可解析

        // 段里按**名字**找到的下标，必须就是点表里的下标
        for (int i = EMS_EXT_BEGIN; i < EMS_EXT_END; ++i) {
            EXPECT(ems_point_zone_of(i) == EMS_ZONE_EXT);
            const std::size_t idx = rt_db_find_index_by_id(h, EMS_POINT_NAMES[i]);
            EXPECT(idx == static_cast<std::size_t>(i));
        }
        // 反向守卫：设备侧的点也必须在段里解析得到（否则"段里没有"与
        // "我找错了名字"两件事会互相掩盖）
        EXPECT(rt_db_find_index_by_id(h, EMS_POINT_NAMES[EMS_CMD_P_BAT])
               == static_cast<std::size_t>(EMS_CMD_P_BAT));
        EXPECT(rt_db_find_index_by_id(h, "EXT.这个点不存在")
               == static_cast<std::size_t>(-1));

        // 初始态：SEQ = 0（点表默认值）→ 读侧必须判"从未收到过"
        {
            const ems::ExtSetpoints e =
                ems::load_ext_setpoints(rdb_read, iec104::wall_clock_s(), 300.0);
            EXPECT(!e.present);
            EXPECT(!e.usable());
        }
    }

    // ------------------------------------------------------------------
    // T74 六个具名 setter → 真的落到**正确的那个点**上
    //
    // 每个点写一个互不相同的值：如果 dispatch/落点串了台，读回来的值会对不上。
    // 若六个点都写同一个值，"写错点"就完全看不出来。
    // ------------------------------------------------------------------
    std::printf("-- T74 six setters land on the right points --\n");
    {
        struct Case { const char* name; double val; bool via_bool; bool b; };
        const Case cases[] = {
            {"EXT.P_SETPOINT",  123.0, false, false},
            {"EXT.D_TARGET",    456.0, false, false},
            {"EXT.P_UPPER_SET", 789.0, false, false},
            {"EXT.P_LOWER_SET", -321.0, false, false},
            {"EXT.PCS_ONOFF",     0.0, true,  false},   // 经 set_pcs_onoff(false)
            {"EXT.EMS_ENABLE",    0.0, true,  false},   // 经 set_ems_enable(false)
        };
        std::string e;
        EXPECT(w.set_p_setpoint(123.0, &e));
        EXPECT(w.set_d_target  (456.0, &e));
        EXPECT(w.set_upper     (789.0, &e));
        EXPECT(w.set_lower     (-321.0, &e));
        EXPECT(w.set_pcs_onoff (false, &e));
        EXPECT(w.set_ems_enable(false, &e));
        EXPECT(e.empty() || true);   // 成功路径不保证清空 err，这里只是别让 e 悬着

        for (const Case& c : cases) {
            double v = -999.0;
            EXPECT(read_named(h, c.name, &v));
            EXPECT_NEAR(v, c.val, 1e-9);
        }
        // 六个点全都写过 → 每个写入都提交一次发布（每次 +2）
        EXPECT(w.writes() >= 6);
        EXPECT(w.rejects() == 0);

        // 发布协议：稳定态的 SEQ 必为**偶数**（写者前/后各递增一次）
        double seq_in_shm = -1.0;
        EXPECT(read_named(h, "EXT.SEQ", &seq_in_shm));
        EXPECT(static_cast<long long>(seq_in_shm) == w.seq());
        EXPECT(static_cast<long long>(seq_in_shm) > 0);
        EXPECT(static_cast<long long>(seq_in_shm) % 2 == 0);

        // 时标：必须是**墙钟秒**，不是毫秒、也不是运行时长
        double ts = 0.0;
        EXPECT(read_named(h, "EXT.TS", &ts));
        EXPECT_NEAR(ts, iec104::wall_clock_s(), 5.0);
    }

    // ------------------------------------------------------------------
    // T75 读侧（真实 rt_db_get_value）看到的就是刚写的那一组
    // ------------------------------------------------------------------
    std::printf("-- T75 reader sees the published set --\n");
    {
        const ems::ExtSetpoints e =
            ems::load_ext_setpoints(rdb_read, iec104::wall_clock_s(), 300.0);
        EXPECT(e.present && e.consistent && e.finite && e.band_ok);
        EXPECT(!e.stale && !e.from_future);
        EXPECT(e.usable());                    // 上界 789 > 下界 -321，区间合法
        EXPECT(e.seq == w.seq());
        EXPECT_NEAR(e.p_setpoint,  123.0, 1e-9);
        EXPECT_NEAR(e.d_target,    456.0, 1e-9);
        EXPECT_NEAR(e.p_upper_set, 789.0, 1e-9);
        EXPECT_NEAR(e.p_lower_set, -321.0, 1e-9);
        EXPECT(!e.pcs_onoff);                  // 写的是 false
        EXPECT(!e.ems_enable);                 // 写的是 false

        // 顺手验一下"外部设定真能收窄区间"（A3.2 才会接线，这里只证通路端到端可用）
        double lo = -500.0, hi = 500.0;
        ems::narrow_interval_by_ext(e, &lo, &hi);
        EXPECT(lo == 0.0 && hi == 0.0);        // ems_enable=false → 强制 [0,0]
    }

    // ------------------------------------------------------------------
    // T76 clear_all 的语义：回到哨兵 + **SEQ 递增而不是归零**
    // ------------------------------------------------------------------
    std::printf("-- T76 clear_all semantics --\n");
    {
        const long long seq_before = w.seq();
        const int clears_before = w.clears();
        std::string e;
        EXPECT(w.clear_all(&e));
        EXPECT(w.clears() == clears_before + 1);
        // ★ 递增**两次**（begin + commit），不是一次 —— 这是协议的一部分
        EXPECT(w.seq() == seq_before + 2);
        // ★★ 绝不归零：归零会让读者看到"序号倒退"，
        //    而"倒退"与"没变化"在"要不要重新处理"上不可区分。
        EXPECT(w.seq() != 0);

        // 六个**载荷**点逐位回到点表默认值（不另抄常量，直接读 EMS_POINT_DEFAULTS）。
        // ★ 循环**只覆盖六个载荷**，不覆盖 SEQ / TS：
        //   它们不是"设定"，而是发布协议的元数据 —— 任何一次发布都会把它们
        //   改成新的序号 / 时标，所以"等于默认值"对它们**根本不成立**。
        //   （第一版把 8 个点全塞进这个循环，于是 SEQ=14、TS=1.79e9 判红 ——
        //     那不是缺陷，是断言的适用范围写错了。）
        const int payload[6] = {EMS_EXT_P_SETPOINT, EMS_EXT_P_UPPER_SET,
                                EMS_EXT_P_LOWER_SET, EMS_EXT_D_TARGET,
                                EMS_EXT_PCS_ONOFF, EMS_EXT_EMS_ENABLE};
        for (int i = 0; i < 6; ++i) {
            double v = -999.0;
            EXPECT(read_named(h, EMS_POINT_NAMES[payload[i]], &v));
            EXPECT_NEAR(v, EMS_POINT_DEFAULTS[payload[i]], 1e-9);
        }
        // 元数据单独验：SEQ 递增过且为偶数，TS 是刚才那一拍
        {
            double sq = -1.0, ts = 0.0;
            EXPECT(read_named(h, "EXT.SEQ", &sq));
            EXPECT(read_named(h, "EXT.TS", &ts));
            EXPECT(static_cast<long long>(sq) == w.seq());
            EXPECT(static_cast<long long>(sq) % 2 == 0);
            EXPECT(static_cast<long long>(sq)
                   != static_cast<long long>(EMS_POINT_DEFAULTS[EMS_EXT_SEQ]));
            EXPECT_NEAR(ts, iec104::wall_clock_s(), 5.0);
            EXPECT(ts != EMS_POINT_DEFAULTS[EMS_EXT_TS]);
        }

        // 清空之后读侧仍然 usable —— "无约束"是一个**有效**状态，不是失效。
        // （失效必须是另一条独立判据：陈旧。两者合并会让"清空"被误当成"坏了"。）
        const ems::ExtSetpoints es =
            ems::load_ext_setpoints(rdb_read, iec104::wall_clock_s(), 300.0);
        EXPECT(es.present);
        EXPECT(es.usable());
        EXPECT_NEAR(es.p_upper_set, 1e9, 1e-9);
        EXPECT_NEAR(es.p_lower_set, -1e9, 1e-9);
        EXPECT(es.pcs_onoff && es.ems_enable);

        double lo = -500.0, hi = 500.0;
        ems::narrow_interval_by_ext(es, &lo, &hi);
        EXPECT(lo == -500.0 && hi == 500.0);   // 清空后**逐位不动**
    }

    // ------------------------------------------------------------------
    // T76b 陈旧 / 未来时标：在**真段**上验兜底判据
    //
    // 这条判据的意义：网关进程被 kill 时没人清 EXT 区，
    // 最后一个设定会永远生效 —— 只有"按 EXT.TS 判年龄"能兜住。
    // ------------------------------------------------------------------
    std::printf("-- T76b stale / from-future --\n");
    {
        // 先写一个"有约束"的设定，再把它做老
        std::string e;
        EXPECT(w.set_upper(50.0, &e));
        {
            const ems::ExtSetpoints s =
                ems::load_ext_setpoints(rdb_read, iec104::wall_clock_s(), 300.0);
            EXPECT(s.usable());
            EXPECT_NEAR(s.p_upper_set, 50.0, 1e-9);
        }
        // 把 TS 改到 1000 s 之前
        EXPECT(rt_db_set_value(h, static_cast<std::size_t>(EMS_EXT_TS),
                               iec104::wall_clock_s() - 1000.0, iec104::rtdb_q::kGood));
        {
            const ems::ExtSetpoints s =
                ems::load_ext_setpoints(rdb_read, iec104::wall_clock_s(), 10.0);
            EXPECT(s.present);          // 收到过
            EXPECT(s.stale);            // 但过期了
            EXPECT(!s.usable());        // → 整区无效
            // 值本身仍然读得到（陈旧≠读不到，这是两种不同的处置）
            EXPECT_NEAR(s.p_upper_set, 50.0, 1e-9);
        }
        // ★ 时标来自未来 → 也必须判不可用，否则"陈旧判定"永远不成立
        EXPECT(rt_db_set_value(h, static_cast<std::size_t>(EMS_EXT_TS),
                               iec104::wall_clock_s() + 1000.0, iec104::rtdb_q::kGood));
        {
            const ems::ExtSetpoints s =
                ems::load_ext_setpoints(rdb_read, iec104::wall_clock_s(), 300.0);
            EXPECT(s.from_future);
            EXPECT(s.stale);
            EXPECT(!s.usable());
        }
        // 恢复正常（重新发布一次即可把 TS 刷新）
        {
            const ems::ExtSetpoints s =
                ems::load_ext_setpoints(rdb_read, iec104::wall_clock_s(), 300.0);
            EXPECT(!s.usable());        // 还没刷新
        }
        EXPECT(w.set_upper(50.0, &e));
        {
            const ems::ExtSetpoints s =
                ems::load_ext_setpoints(rdb_read, iec104::wall_clock_s(), 300.0);
            EXPECT(s.usable());
            EXPECT(!s.from_future && !s.stale);
        }
    }

    // ------------------------------------------------------------------
    // T76c ★ 并发：写者猛发、读者猛读 —— 不接受**半更新**快照
    //
    // 判据怎么有区分度（这是本测试的关键，别只写"读到一个数就算过"）：
    //   写者的每次提交都是**确定的**（下面的公式），所以每个 SEQ 值都对应
    //   一个**唯一**的 (lower, upper)。读者拿到一个 usable 快照后，按 SEQ
    //   反算期望值并**逐位对账**。
    //   若协议漏了"开始那次递增"，读者可能接受一个"新下界 + 旧上界"的快照 ——
    //   它的 SEQ 对应的是**上一次**提交，于是与期望值对不上 → 违规。
    //
    // ★ 为什么用公式而不是"写者边写边记一张表"（第一版的做法，失败）：
    //   读者能拿到稳定窗口的时刻，恰好就是"某个提交刚完成、下一个还没开始"
    //   那一瞬 —— 而那也正是写者**记完账**之前的微秒级窗口。实测 300 次里
    //   298 次查不到那张表（可用 300 / 未及对账 298）：看着"零违规"，
    //   其实 99% 的快照根本没被对账过。**"没报违规"与"查过之后没违规"是两件事。**
    // ------------------------------------------------------------------
    std::printf("-- T76c concurrent publish/read (no half-updated snapshot) --\n");
    {
        // 写者的配方：第 k 轮（k 从 0 起）
        //   set_lower(-100-k) → 提交序号 S0+4k+2，状态 (-100-k, U_prev(k))
        //   set_upper(100+3k) → 提交序号 S0+4k+4，状态 (-100-k, 100+3k)
        // 上界每次 +3、下界每次 -1（**速率不同**），这样"半更新"不可能
        // 与任何一个合法状态撞上 —— 撞上的话判据就没有区分度了。
        const long long S0 = w.seq();             // 起点（上一节结束时是偶数）
        double u0 = 0.0;
        EXPECT(read_named(h, "EXT.P_UPPER_SET", &u0));   // 循环开始前的上界

        auto expected_for = [&](long long q, double* lo_v, double* hi_v) -> bool {
            const long long d = q - S0;
            if (d < 2) return false;
            if (d % 4 == 2) {                     // set_lower 的提交
                const long long k = (d - 2) / 4;
                *lo_v = -100.0 - static_cast<double>(k);
                *hi_v = (k == 0) ? u0 : (100.0 + 3.0 * static_cast<double>(k - 1));
                return true;
            }
            if (d % 4 == 0) {                     // set_upper 的提交
                const long long k = d / 4 - 1;
                *lo_v = -100.0 - static_cast<double>(k);
                *hi_v = 100.0 + 3.0 * static_cast<double>(k);
                return true;
            }
            return false;                          // 奇数：本就不该被接受
        };

        std::atomic<bool> stop{false};
        std::atomic<int> reads{0}, usable_n{0}, checked{0}, violations{0}, bad_label{0};

        std::thread writer([&] {
            std::string e;
            for (long long k = 0; !stop.load(std::memory_order_relaxed); ++k) {
                w.set_lower(-100.0 - static_cast<double>(k), &e);
                w.set_upper(100.0 + 3.0 * static_cast<double>(k), &e);
            }
        });

        // 等写者真的动起来（否则读者可能在写者第一次提交之前就跑完 ——
        // 那时读到的是上一节的旧 SEQ，测试等于空跑）
        for (int i = 0; i < 3000 && w.seq() == S0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        EXPECT(w.seq() > S0);

        // 读到足够多的可用快照为止（写者一直在改，读者会频繁撞上"不一致"）
        int attempts = 0;
        while (usable_n < 300 && attempts < 400000) {
            ++attempts;
            const ems::ExtSetpoints s =
                ems::load_ext_setpoints(rdb_read, iec104::wall_clock_s(), 300.0);
            ++reads;
            if (!s.usable()) continue;      // 快照不一致 → 本拍不施加（正确处置）
            ++usable_n;

            double elo = 0.0, ehi = 0.0;
            if (!expected_for(s.seq, &elo, &ehi)) { ++bad_label; continue; }
            ++checked;
            if (s.p_lower_set != elo || s.p_upper_set != ehi) {
                if (violations < 3) {
                    std::printf("       [违规] seq=%lld 读到 (%.1f, %.1f)，"
                                "该序号应为 (%.1f, %.1f)\n",
                                s.seq, s.p_lower_set, s.p_upper_set, elo, ehi);
                }
                ++violations;
            }
        }
        stop = true;
        writer.join();

        std::printf("       读 %d 次：可用 %d / 已对账 %d / 序号异常 %d / 违规 %d\n",
                    reads.load(), usable_n.load(), checked.load(),
                    bad_label.load(), violations.load());
        // ★ 主判据：没有一个接受下来的快照违反"该序号对应的确定值"
        EXPECT(violations == 0);
        // 覆盖率守卫：不能因为"全都不可用"或"全都对不上序号"而让上面那条空过
        EXPECT(usable_n >= 300);
        EXPECT(checked == usable_n.load());
        EXPECT(bad_label == 0);
        // 段里的 SEQ 在停止后必须是**偶数**（没有停在写入窗口里）
        double seq_in_shm = -1.0;
        EXPECT(read_named(h, "EXT.SEQ", &seq_in_shm));
        EXPECT(static_cast<long long>(seq_in_shm) == w.seq());
        EXPECT(static_cast<long long>(seq_in_shm) % 2 == 0);
    }

    // ------------------------------------------------------------------
    // T76d 未连接时：写必须**失败**，而不是静默成功
    // ------------------------------------------------------------------
    std::printf("-- T76d writer without RT_DB must fail --\n");
    {
        iec104::RtDbExtWriter orphan(nullptr);
        EXPECT(!orphan.is_open());
        EXPECT(orphan.self_check() == 0);      // 未连接时只查分区，分区是对的
        std::string e;
        EXPECT(!orphan.set_p_setpoint(1.0, &e));
        EXPECT(!e.empty());                    // 失败必须带原因（进否定确认）
        EXPECT(!orphan.clear_all(&e));
        EXPECT(orphan.writes() == 0);
        EXPECT(orphan.seq() == 0);
    }

    src.close_owned();

    std::printf("\n=== P3 EXT RTDB: PASS=%d FAIL=%d ===\n", g_pass, g_fail);
    std::printf("%s\n", g_fail == 0 ? "[ALL PASS]" : "[HAS FAILURES]");
    return g_fail == 0 ? 0 : 1;
}
