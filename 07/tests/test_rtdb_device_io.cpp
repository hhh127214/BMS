// =====================================================================
// 07/ 单元测试 —— RT_DB 接入（共享内存实时库适配器 RtDbDeviceIO）
//
//   T25: 点表契约 —— 共享内存点表 ↔ 编译期点表 ↔ 设备侧点表（三方言必须一致）
//   T26: 跨内存边界闭环等价 —— 同一套 EmsRuntime，一路经共享内存（RtDbDeviceIO
//        + 设备侧泵），一路直连进程内点表（MemoryDeviceIO），逐拍记录逐位一致
//   T27: 两个独立连接（两次 rt_db_init）看到同一段共享内存 —— 证明"跨进程"
//        不是进程内副本；段级写计数是共享的
//   T28: 故障经共享内存驱动状态机（设备→EMS 方向的真闭环）
//
// 为什么 T26 是这次接入的核心证据：
//   P0.5 只用 MemoryDeviceIO 证明了「换数据源不改算法」在**进程内**成立。
//   真实部署换的是**另一个进程** —— 共享内存段、独立连接、两个方向各自写。
//   如果算法在两种介质下逐拍产出完全一致，才说明抽象真的立在"介质不可见"
//   的地方，而不是立在"同一个容器"的巧合上。
//
// 编译（gcc 编 C 库 + g++ 编测试，见 07/scripts/build_test_rtdb.bat）：
//   gcc -std=c11 -O2 -I vendor/rt_db -c vendor/rt_db/rt_db_api.c
//   gcc -std=c11 -O2 -I src/rtdb -I vendor/rt_db -c src/rtdb/ems_point_table.c
//   gcc -std=c11 -O2 -I src/rtdb -I vendor/rt_db -c src/rtdb/ems_rt_db_setup.c
//   g++ -std=c++17 -Wall -O2 -I src -I src/rtdb -I vendor/rt_db -I ../04/src ...
// =====================================================================

// rt_db_structs.h（只为取品质位/长度常量）会引入 <windows.h>；先定义
// NOMINMAX，否则它的 min/max 宏会破坏本文件与项目头里大量 std::min / std::max。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "data_models.h"
#include "device_io.h"
#include "ems_point_table.h"
#include "ems_rt_db_setup.h"
#include "memory_device_io.h"
#include "realtime_loop.h"
#include "rt_db_api.h"
#include "rt_db_structs.h"      // 仅用于常量/品质位漂移守卫（T25）
#include "rtdb_device_io.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

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

#define EXPECT_NEAR(a, b, eps)                                            \
    do {                                                                  \
        double va = (a), vb = (b);                                        \
        if (std::fabs(va - vb) <= (eps)) {                                \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va << " vs " << #b << "="  \
                      << vb << " (eps " << (eps) << ")" << std::endl;     \
        }                                                                 \
    } while (0)

// =====================================================================
// 公共装置
// =====================================================================

// 共享内存段是**跨进程全局**资源（Windows 上名字固定为 RT_DB_SHARED_MEMORY），
// 因此整份测试共用一个段：构造时 reset 一次，保证每次运行的初值确定。
struct RtdbEnv {
    rt_db_handle_t handle{};
    bool ok = false;

    RtdbEnv() {
        bool created = false;
        ok = ems_rt_db_setup(/*reset=*/true, &created) && rt_db_init(&handle, nullptr);
    }
    ~RtdbEnv() {
        if (ok) rt_db_cleanup(&handle);
    }
};

static bool almost_equal(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

// 把共享内存段**就地**重置为点表初值（连接不用重建：索引由点名决定，不因重置失效）。
// 每个用例开头调一次，保证"起点确定"——跨用例的残留点值是最难查的假失败来源。
static void reset_segment() {
    bool created = false;
    ems_rt_db_setup(/*reset=*/true, &created);
}

// 设备侧 → 共享内存：只写**设备侧拥有的点**（MEAS / STA / CFG）。
// CMD.* 是 EMS 侧的下行区，设备侧只读不写 —— 现场就是靠「谁拥有这个点」
// 划清两个进程的边界；写错会让事后追溯变成一团乱麻。
static void publish_device_points(const MemoryDeviceIO& dev, RtDbPointWriter& w) {
    for (std::size_t i = 0; i < EMS_POINT_COUNT; ++i) {
        if (i >= static_cast<std::size_t>(EMS_CMD_P_BAT) &&
            i <= static_cast<std::size_t>(EMS_CMD_P_LOWER)) {
            continue;
        }
        w.write(i, dev.get(EMS_POINT_NAMES[i]));
    }
}

// 环境脚本（负荷/光伏）——**按控制拍序号**给出，两次运行必须用同一份
static void env_at(int step, double& p_load_kw, double& p_pv_kw) {
    const double t = step * 0.1;
    p_load_kw = 380.0 + 120.0 * std::sin(t / 30.0);
    p_pv_kw   = (t > 5.0 && t < 25.0) ? 150.0 + 60.0 * std::sin(t / 5.0) : 0.0;
}

// 两路运行必须用**完全相同的装配序列**，否则比的是装配差异不是介质差异
static void configure_runtime(EmsRuntime& rt) {
    rt.config().dt_s = 0.1;
    rt.config().enable_realtime_correction = true;
    rt.config().l2_correction_max_kw = 100.0;
    // 安全参数必须在 apply_configs() 之前 —— 它内部会把 safety_params_ 推给引擎
    rt.safety_params().grid_p_min_kw = -1e9;   // 隔离并网倒送约束（本测试不测它）
    rt.safety_params().ramp_kw_per_s = 1e9;    // 隔离变化率限制
    rt.apply_configs();
    // 状态机默认停在 READY（不允许输出），需要显式"启动"许可
    rt.fsm().request_run(true);
    // apply_configs() 用 dev_ 里的 PCS 额定算协同层参数，但不会刷变压器/需量
    rt.device_limits().transformer_capacity_kw = 800.0;  // 隔离变压器极端过载误触发
    rt.device_limits().d_target_kw = 250.0;
}

// =====================================================================
// T25: 点表契约（三方一致 + 漂移守卫）
// =====================================================================
static void test_25_point_table_contract(RtdbEnv& env) {
    std::cerr << "[T25] 点表契约：共享内存点表 ↔ 编译期点表 ↔ 设备侧点表 ...\n";
    EXPECT(env.ok);
    if (!env.ok) return;

    // ---- 常量漂移守卫：本适配器镜像了 rt_db_structs.h 的常量（见头文件说明），
    //      一旦 vendor 改了长度/品质位定义，这里必须失败。
    EXPECT(kRtdbMaxPointIdLen == static_cast<std::size_t>(MAX_POINT_ID_LEN));
    EXPECT(kRtdbMaxUnitLen == static_cast<std::size_t>(MAX_UNIT_LEN));
    EXPECT(kRtdbMaxPoints == rt_db_get_max_data_points());
    EXPECT(rtdb_quality::kGood == static_cast<long>(QUALITY_GOOD));
    EXPECT(rtdb_quality::kBad == static_cast<long>(QUALITY_BAD));
    EXPECT(rtdb_quality::kUncertain == static_cast<long>(QUALITY_UNCERTAIN));

    RtDbDeviceIO io(&env.handle);
    EXPECT(io.is_open());
    EXPECT(io.self_check() == 0);   // 30 点的点名 / 单位 / 索引全部对得上

    // ---- 点名 → 索引：适配器（共享内存）与设备侧（进程内）必须得到同一个索引
    RtDbPointWriter w(&env.handle);
    for (std::size_t i = 0; i < EMS_POINT_COUNT; ++i) {
        EXPECT(rt_db_find_index_by_id(&env.handle, EMS_POINT_NAMES[i]) == i);
    }

    // ---- 设备侧点表（MemoryDeviceIO）与 EMS 点表**逐字相同**
    MemoryDeviceIO dev;
    std::size_t alias = 0;
    for (std::size_t i = 0; i < EMS_POINT_COUNT; ++i) {
        if (dev.has_point(EMS_POINT_NAMES[i])) ++alias;
    }
    EXPECT(alias == static_cast<std::size_t>(EMS_POINT_COUNT));

    // ---- 设备侧写点（MEAS / STA / CFG）→ EMS 侧读量测（跨内存边界，设备→EMS）
    w.write(EMS_P_LOAD, 321.0);
    w.write(EMS_P_PV,   111.0);
    w.write(EMS_P_BAT,   55.0);
    w.write(EMS_SOC,      0.42);
    w.write(EMS_T_C,     31.5);

    RealtimeSnapshot snap;
    EXPECT(io.read_snapshot(0.0, snap));
    EXPECT_NEAR(snap.p_bat_actual_kw, 55.0, 1e-9);
    EXPECT_NEAR(snap.p_load_kw, 321.0 + 2.0, 1e-9);                 // 站用电 2kW 计入负荷侧
    EXPECT_NEAR(snap.p_grid_kw, 321.0 + 2.0 - 111.0 - 55.0, 1e-9);  // P_grid = P_load − P_pv − P_bat
    EXPECT_NEAR(snap.soc, 0.42, 1e-9);
    EXPECT_NEAR(snap.temperature_c, 31.5, 1e-9);
    EXPECT(io.stale_reads() == 0);

    // ---- EMS 侧下发（CMD）→ 设备侧读指令（跨内存边界，EMS→设备）
    PowerCommand cmd;
    cmd.p_bat_cmd_kw = 123.0;
    cmd.p_upper      = 150.0;
    cmd.p_lower      = -80.0;
    EXPECT(io.write_command(cmd));

    double v = 0.0;
    long   q = 0;
    EXPECT(w.read(EMS_CMD_P_BAT, &v, &q) && almost_equal(v, 123.0));
    EXPECT(q == static_cast<long>(QUALITY_GOOD));
    EXPECT(w.read(EMS_CMD_P_UPPER, &v, &q) && almost_equal(v, 150.0));
    EXPECT(w.read(EMS_CMD_P_LOWER, &v, &q) && almost_equal(v, -80.0));

    // ---- 按名字读写与按索引读写等价（现场脚本用名字，算法用索引）
    EXPECT(w.write_by_name("MEAS.P_PV", 88.0));
    EXPECT(w.read(EMS_P_PV, &v, &q) && almost_equal(v, 88.0));
    EXPECT(w.read_by_name("MEAS.P_BAT", &v, &q) && almost_equal(v, 55.0));

    // ---- 限值映射：CFG.* → DeviceLimits
    w.write(EMS_CFG_MAX_CHG, 180.0);
    w.write(EMS_CFG_MAX_DIS, 160.0);
    w.write(EMS_CFG_BMS_CHG_LIM, 120.0);
    w.write(EMS_CFG_BMS_DIS_LIM, 140.0);
    w.write(EMS_CFG_TR_KVA, 630.0);
    w.write(EMS_CFG_D_TARGET, 500.0);
    DeviceLimits lim;
    EXPECT(io.read_limits(lim));
    EXPECT_NEAR(lim.pcs_rated_chg_kw, 180.0, 1e-9);
    EXPECT_NEAR(lim.pcs_rated_dis_kw, 160.0, 1e-9);
    EXPECT_NEAR(lim.bms_chg_limit_kw, 120.0, 1e-9);
    EXPECT_NEAR(lim.bms_dis_limit_kw, 140.0, 1e-9);
    EXPECT_NEAR(lim.transformer_capacity_kw, 630.0, 1e-9);
    EXPECT_NEAR(lim.d_target_kw, 500.0, 1e-9);
    EXPECT(!lim.bms_chg_forbidden && !lim.bms_dis_forbidden);
    EXPECT_NEAR(io.battery_capacity_kwh(), 1000.0, 1e-9);   // CFG.BAT_CAP_KWH 默认值

    // ---- 质量位：BAD 品质 → 该量测点判为不可信（data_valid=false）
    w.write(EMS_P_PV, 88.0, static_cast<long>(QUALITY_BAD));
    RealtimeSnapshot snap2;
    EXPECT(io.read_snapshot(0.0, snap2) == false);
    EXPECT(io.read_status().data_valid == false);
    EXPECT_NEAR(snap2.p_pv_kw, 88.0, 1e-9);   // 值仍然读得到，但可信性由品质位表达

    // 品质位"随采集刷新"：把它改回 GOOD 之后，必须**再采集一次**才恢复可信 ——
    // 这正是"数据是否可信"该有的语义（不是翻一下就变好，而是下一次采集确认）。
    // EmsRuntime 每拍都是 ①read_snapshot → ②read_status，顺序天然正确。
    w.write(EMS_P_PV, 111.0, static_cast<long>(QUALITY_GOOD));
    EXPECT(io.read_status().data_valid == false);   // 未重新采集 → 仍按上次的品质
    RealtimeSnapshot snap3;
    EXPECT(io.read_snapshot(0.0, snap3));
    EXPECT(io.read_status().data_valid == true);
    EXPECT_NEAR(snap3.p_pv_kw, 111.0, 1e-9);

    // ---- 段自检：写计数是**段级**元数据，可被任意连接观察
    const std::size_t before = rt_db_get_write_count(&env.handle);
    EXPECT(w.write(EMS_SOH, 0.97));
    EXPECT(rt_db_get_write_count(&env.handle) == before + 1);
    EXPECT(io.writes() > 0);
}

// =====================================================================
// T26: 跨内存边界闭环等价性（RT_DB vs 进程内点表）
//
//   运行 A：EmsRuntime ── MemoryDeviceIO（进程内点表，P0.5 已证）
//   运行 B：EmsRuntime ── RtDbDeviceIO ──[共享内存]── 设备侧泵（MemoryDeviceIO）
//
// 除"数据走不走共享内存"以外，两路的装配序列、环境脚本、算法参数**完全相同**。
// 逐拍记录必须逐位一致 —— 这就是"换数据源不改算法"在**跨进程介质**上的证明。
// =====================================================================
static void test_26_cross_memory_closed_loop(RtdbEnv& env) {
    std::cerr << "[T26] 跨内存边界闭环等价性（RT_DB vs 进程内点表） ...\n";
    EXPECT(env.ok);
    if (!env.ok) return;

    reset_segment();                       // 起点确定
    const int    N  = 400;
    const double DT = 0.1;

    // -----------------------------------------------------------------
    // 运行 B：所有数据经共享内存
    // -----------------------------------------------------------------
    MemoryDeviceIO    dev_b;               // 设备侧模型（真实现场是另一个进程）
    RtDbPointWriter   w(&env.handle);
    publish_device_points(dev_b, w);       // 装配前必须先把 CFG/STA/MEAS 写进去

    RtDbDeviceIO io_b(&env.handle);
    io_b.set_device_pump([&](double cmd, double dt) {
        dev_b.execute(cmd, dt);            // 设备推进一拍
        publish_device_points(dev_b, w);   // 量测回写共享内存
    });

    EmsRuntime rt_b;
    rt_b.attach_device(&io_b);             // ← 读限值：来自 CFG.*（共享内存）
    configure_runtime(rt_b);
    rt_b.run(N, DT, [&](EmsRuntime&, int i) {
        double load = 0.0, pv = 0.0;
        env_at(i, load, pv);
        dev_b.set_environment(load, pv);   // 环境由设备侧（电表）刷新
        publish_device_points(dev_b, w);
    });

    // -----------------------------------------------------------------
    // 运行 A：同一套算法直连进程内点表（参考）
    // -----------------------------------------------------------------
    MemoryDeviceIO dev_a;
    EmsRuntime rt_a;
    rt_a.attach_device(&dev_a);
    configure_runtime(rt_a);
    rt_a.run(N, DT, [&](EmsRuntime&, int i) {
        double load = 0.0, pv = 0.0;
        env_at(i, load, pv);
        dev_a.set_environment(load, pv);
    });

    // -----------------------------------------------------------------
    // 逐拍比对
    // -----------------------------------------------------------------
    EXPECT(rt_b.log().size() == static_cast<std::size_t>(N));
    EXPECT(rt_a.log().size() == static_cast<std::size_t>(N));
    EXPECT(std::string(rt_b.device()->name()) == "RtDbDeviceIO(SharedMemory)");
    EXPECT(std::string(rt_a.device()->name()) == "MemoryDeviceIO(PointTable)");

    int diff = 0;
    const std::size_t n = std::min(rt_a.log().size(), rt_b.log().size());
    for (std::size_t i = 0; i < n; ++i) {
        const StepRecord& a = rt_a.log()[i];
        const StepRecord& b = rt_b.log()[i];
        const bool same =
            almost_equal(a.p_cmd, b.p_cmd) && almost_equal(a.p_actual, b.p_actual) &&
            almost_equal(a.p_grid, b.p_grid) && almost_equal(a.soc, b.soc) &&
            almost_equal(a.temp, b.temp) && almost_equal(a.p_lower, b.p_lower) &&
            almost_equal(a.p_upper, b.p_upper) && almost_equal(a.plan_target, b.plan_target) &&
            almost_equal(a.correction, b.correction) && almost_equal(a.t, b.t) &&
            a.state == b.state && a.clamped == b.clamped &&
            a.safety_clip == b.safety_clip && a.state_gated == b.state_gated &&
            a.hold_last == b.hold_last && a.fault_bits == b.fault_bits &&
            a.reason == b.reason;
        if (!same) ++diff;
        EXPECT(same);
    }
    EXPECT(diff == 0);
    EXPECT(rt_b.fsm().state() == rt_a.fsm().state());
    EXPECT(rt_b.fsm().history().size() == rt_a.fsm().history().size());

    // 反向守卫：不能是"两边都恒 0"的假等价
    double max_cmd_a = 0.0, max_cmd_b = 0.0;
    for (const auto& r : rt_a.log()) max_cmd_a = std::max(max_cmd_a, std::fabs(r.p_cmd));
    for (const auto& r : rt_b.log()) max_cmd_b = std::max(max_cmd_b, std::fabs(r.p_cmd));
    EXPECT(max_cmd_a > 50.0);
    EXPECT(almost_equal(max_cmd_a, max_cmd_b));
    EXPECT(std::fabs(rt_a.log().back().soc - 0.5) > 1e-6);   // SOC 真的动了
    EXPECT(io_b.stale_reads() == 0);                          // 共享内存读点零失败
    EXPECT(io_b.self_check() == 0);

    std::cerr << "       峰值指令 " << max_cmd_a << " kW / SOC "
              << rt_a.log().front().soc << " → " << rt_a.log().back().soc
              << " / 逐拍差异 " << diff << " 拍\n";
}

// =====================================================================
// T27: 两个独立连接（两次 rt_db_init）看到同一段共享内存
//
// 这是"跨进程"的最小可验证模型：**两个互不知情的连接**，各自持有自己的映射
// 视图与索引表，但读写的是同一段物理内存。若哪天有人把适配器改成"进程内缓存"，
// 本用例立刻失败 —— 这正是它存在的意义。
// =====================================================================
static void test_27_two_connections_same_segment(RtdbEnv& env) {
    std::cerr << "[T27] 两个独立连接看同一段共享内存 ...\n";
    EXPECT(env.ok);
    if (!env.ok) return;

    reset_segment();

    rt_db_handle_t h2{};
    // ⚠ 段级连接计数会被 ems_rt_db_setup(reset=true) 清零（memset 整个段），
    //   所以这里断言的是"新连接让计数 +1"，而不是"绝对值 ≥2"。
    //   部署含义：初始化器必须在**所有连接建立之前**调用（现场就是先起初始化器）。
    const std::size_t clients_before = rt_db_get_connected_clients(&env.handle);
    EXPECT(rt_db_init(&h2, nullptr));       // 第二个连接（现场就是第二个进程）
    EXPECT(h2.shm_addr != nullptr);
    EXPECT(rt_db_get_connected_clients(&env.handle) == clients_before + 1);

    RtDbDeviceIO io1(&env.handle);          // 连接 1（EMS 侧）
    RtDbDeviceIO io2(&h2);                  // 连接 2（设备侧 / 另一个进程）

    // 连接 1 写 → 连接 2 读
    RtDbPointWriter w1(&env.handle);
    EXPECT(w1.write(EMS_P_LOAD, 777.0));
    RealtimeSnapshot s;
    EXPECT(io2.read_snapshot(0.0, s));
    EXPECT_NEAR(s.p_load_kw, 777.0 + 2.0, 1e-9);

    // 连接 2 写 → 连接 1 读（反方向同样成立）
    RtDbPointWriter w2(&h2);
    EXPECT(w2.write(EMS_P_PV, 33.0));
    EXPECT(io1.read_snapshot(0.0, s));
    EXPECT_NEAR(s.p_pv_kw, 33.0, 1e-9);

    // 写计数是**段级**元数据：一个连接写，另一个连接看得见
    const std::size_t c = rt_db_get_write_count(&env.handle);
    EXPECT(w2.write(EMS_SOC, 0.42));
    EXPECT(rt_db_get_write_count(&env.handle) == c + 1);
    EXPECT(rt_db_get_write_count(&h2) == c + 1);

    // 点表自检与索引映射不依赖具体连接
    EXPECT(io2.self_check() == 0);
    EXPECT(io1.self_check() == 0);
    EXPECT(io2.stale_reads() == 0);

    rt_db_cleanup(&h2);
}

// =====================================================================
// T28: 故障经共享内存驱动状态机（设备 → EMS 方向的真闭环）
//
// 故障点由"设备侧"写进实时库，EMS 侧只经 RtDbDeviceIO 读 DeviceStatus。
// 期望：进 FAULT → 指令归零（门控）→ 撤销运行许可；故障消失后停在 READY，
// **不自动带载**（安全要求：故障后必须由上层显式重启）。
// =====================================================================
static void test_28_fault_over_shared_memory(RtdbEnv& env) {
    std::cerr << "[T28] 故障经共享内存驱动状态机 ...\n";
    EXPECT(env.ok);
    if (!env.ok) return;

    reset_segment();

    MemoryDeviceIO  dev;
    RtDbPointWriter w(&env.handle);
    publish_device_points(dev, w);

    RtDbDeviceIO io(&env.handle);
    io.set_device_pump([&](double cmd, double dt) {
        dev.execute(cmd, dt);
        publish_device_points(dev, w);
    });

    EmsRuntime rt;
    rt.attach_device(&io);
    configure_runtime(rt);

    // ---- 第一阶段：正常运行（PCS 故障位 = 0）----
    rt.run(80, 0.1, [&](EmsRuntime&, int i) {
        double load = 0.0, pv = 0.0;
        env_at(i, load, pv);
        dev.set_environment(load, pv);
        publish_device_points(dev, w);
    });
    EXPECT(rt.fsm().state() == EmsState::kNormal);
    EXPECT(rt.log().back().fault_bits == 0);
    EXPECT(std::fabs(rt.log().back().p_cmd) > 1.0);      // 确实在出力
    const int step_base = 80;

    // ---- 第二阶段：设备侧报 PCS 故障（写进共享内存）----
    dev.set_pcs_fault(true);
    EXPECT(w.write(EMS_STA_FAULT, 1.0));
    rt.run(20, 0.1, [&](EmsRuntime&, int i) {
        double load = 0.0, pv = 0.0;
        env_at(step_base + i, load, pv);
        dev.set_environment(load, pv);
        publish_device_points(dev, w);
    });
    const StepRecord& fr = rt.log().back();
    EXPECT((fr.fault_bits & (1 << 3)) != 0);             // bit3 = pcs_fault
    EXPECT(rt.fsm().state() == EmsState::kFault);
    EXPECT(std::fabs(fr.p_cmd) < 1e-9);                  // FAULT → 门控归零
    EXPECT(fr.state_gated);
    EXPECT(rt.fsm().output_enabled() == false);          // 运行许可已撤销

    // ---- 第三阶段：故障消失 → 回到 READY，但不自动带载 ----
    dev.set_pcs_fault(false);
    EXPECT(w.write(EMS_STA_FAULT, 0.0));
    rt.run(30, 0.1, [&](EmsRuntime&, int i) {
        double load = 0.0, pv = 0.0;
        env_at(step_base + 20 + i, load, pv);
        dev.set_environment(load, pv);
        publish_device_points(dev, w);
    });
    EXPECT(rt.log().back().fault_bits == 0);
    EXPECT(rt.fsm().state() != EmsState::kFault);
    EXPECT(rt.fsm().output_enabled() == false);          // 需上层显式重启
    EXPECT(std::fabs(rt.log().back().p_cmd) < 1e-9);
    EXPECT(io.stale_reads() == 0);
}

// =====================================================================
int main() {
    std::cerr << "=========================================\n"
              << " 07/ RT_DB 接入：共享内存实时库适配器 单元测试\n"
              << "=========================================\n";

    // 一份共享内存段，四个用例共用（每个用例自己 reset 到确定起点）
    RtdbEnv env;
    if (!env.ok) {
        std::cerr << "  [环境] 共享内存初始化失败 —— RT_DB 段建不起来，"
                     "后续断言会整体失败\n";
    }

    test_25_point_table_contract(env);
    test_26_cross_memory_closed_loop(env);
    test_27_two_connections_same_segment(env);
    test_28_fault_over_shared_memory(env);

    std::cerr << "=========================================\n"
              << " PASS=" << g_pass << "  FAIL=" << g_fail << "\n"
              << "=========================================\n";
    return (g_fail == 0) ? 0 : 1;
}





