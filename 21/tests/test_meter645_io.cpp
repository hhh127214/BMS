// =====================================================================
// 21/ 单元测试（三）—— Meter645DeviceIO：645 电表 → IDeviceIO → EMS 快照
//
//   T50 内置绑定表自检全过（正向证据：checked == kBindingCount）
//   T51 自检区分四类坏表（点名 / DI 未知 / 重复绑定 / 索引越界）
//   T52 采集成功端到端：DI → BCD → 快照 p_grid_kw
//   T53 符号位端到端：反向功率（倒送）→ p_grid_kw 为负
//   T54 绑定表变比端到端：PT 变比 ×100 在适配器层生效
//   T55 ★ 超时保留旧值（本文件头号反证目标）：值还在，有效位没了
//   T56 异常应答（0xD1）：不覆盖缓存、分类计数正确
//   T57 写永远被拒：write_command / execute 返回 false 并计数
//   T58 限值全零 / 容量 0 / limits_are_live=false（0 比编造的默认值安全）
//   T59 快照未拥有字段 = "没有值"（恒 0），不是"读到 0"
//   T60 坏表注入 → 拒绝采集（map_ok=false，**零流量**）
//   T61 未绑定的点永远没有可信值（与"读到 0"严格区分）
//   T62 两点一成一败：per-point 有效位独立，data_valid 整体降级
//   T63 计数口径：scans / collected / failed_reads 各自独立
//
// ---------------------------------------------------------------------
// ★ 这组测试要回答的问题：
//   "645 通道接进 EMS 之后，值不对时能不能分清是**哪一种**不对"：
//     · 值过期（T55/T56）→ 有效位没了但值还在
//     · 通道装错（T57）→ 写被拒且有计数
//     · 表被改坏（T60）→ 一帧都不发
//     · 点根本没来源（T59/T61）→ 恒 0 且 point_valid 恒 false
// =====================================================================

#include "meter645_device_io.h"   // 被测对象
#include "meter645_point_map.h"
#include "fake_meter_slave.h"     // 21/ 进程内假表

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>

using namespace ems;
using namespace ems::dlt645;

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

#define EXPECT_EQ(a, b)                                                   \
    do {                                                                  \
        long long va = (long long)(a), vb = (long long)(b);               \
        if (va == vb) {                                                   \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va << " vs " << #b << "="  \
                      << vb << std::endl;                                 \
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

// ---------------------------------------------------------------------
// 测试台：内存回环链路 + 一台假表 + 一个 Meter645DeviceIO
// ---------------------------------------------------------------------
// ★ 功率 DI（0x02030000）是 3 字节带符号位 BCD，**未乘变比时上限
//   9.9999 kW**（5 个半字节）。测试取值必须落在量程内 ——
//   §70 踩过：单测用 1.2345 全绿，CLI 灌 10.29 才暴露溢出。
//   本文件的取值一律 ≤ 9.9 kW（T54 除外：那里靠变比把工程量放大）。
// ---------------------------------------------------------------------
struct Rig {
    Address addr;
    BytePipe to_slave;    // 主站 → 表
    BytePipe to_master;   // 表 → 主站
    LoopbackLink link;
    test::FakeMeterSlave slave;
    Meter645DeviceIO io;

    Rig(const meter645::PointBinding* table = meter645::kBindings,
        std::size_t count = meter645::kBindingCount)
        : link(&to_slave, &to_master),
          slave(&to_slave, &to_master, addr),
          io(&link, addr, MasterOptions(), table, count) {
        addr.from_string("000000000001");
        slave.set_options(test::FakeMeterSlave::Options());
        io.master().set_pump([this] { slave.poll(); });
        link.open();
    }
};

// =====================================================================
// T50 内置绑定表自检全过
// =====================================================================
static void test_t50_builtin_map_clean() {
    std::cout << "\n[T50] 内置绑定表自检\n";

    const meter645::MapCheck r = meter645::self_check_map();
    EXPECT_EQ(r.total, 0);
    EXPECT_EQ(r.checked, (long long)meter645::kBindingCount);   // 正向证据
    EXPECT(meter645::kBindingCount >= 1);
    EXPECT(meter645::binding_for_index(EMS_P_GRID) != nullptr);
    EXPECT(meter645::binding_for_index(EMS_SOC) == nullptr);    // 电表没有 SOC
}

// =====================================================================
// T51 自检区分四类坏表
// =====================================================================
static void test_t51_map_check_catches_bad_tables() {
    std::cout << "\n[T51] 自检对四类坏表的区分度\n";

    {   // 点名错：抽象表 EMS_P_GRID 的名字是 "MEAS.P_GRID"
        const meter645::PointBinding bad[] = {
            {EMS_P_GRID, "MEAS.WRONG", 0x02030000u, {1.0, 0.0}},
        };
        const meter645::MapCheck r = meter645::self_check_map(bad, 1);
        EXPECT_EQ(r.name_mismatch, 1);
        EXPECT_EQ(r.di_unknown, 0);
        EXPECT_EQ(r.total, 1);
    }
    {   // DI 未知：内置 DI 表查无此项
        const meter645::PointBinding bad[] = {
            {EMS_P_GRID, "MEAS.P_GRID", 0x99999999u, {1.0, 0.0}},
        };
        const meter645::MapCheck r = meter645::self_check_map(bad, 1);
        EXPECT_EQ(r.di_unknown, 1);
        EXPECT_EQ(r.total, 1);
    }
    {   // 重复绑定：同一个抽象点被绑两次
        const meter645::PointBinding bad[] = {
            {EMS_P_GRID, "MEAS.P_GRID", 0x02030000u, {1.0, 0.0}},
            {EMS_P_GRID, "MEAS.P_GRID", 0x02030000u, {1.0, 0.0}},
        };
        const meter645::MapCheck r = meter645::self_check_map(bad, 2);
        EXPECT(r.dup_index >= 1);
    }
    {   // 索引越界：EXT 区是调度侧的点，不经设备总线
        const meter645::PointBinding bad[] = {
            {(std::size_t)EMS_EXT_P_SETPOINT, "EXT.P_SETPOINT", 0x02030000u, {1.0, 0.0}},
        };
        const meter645::MapCheck r = meter645::self_check_map(bad, 1);
        EXPECT_EQ(r.index_out_of_range, 1);
    }
}

// =====================================================================
// T52 采集成功端到端
// =====================================================================
static void test_t52_happy_path() {
    std::cout << "\n[T52] 采集成功：DI → BCD → 快照\n";

    Rig rig;
    rig.slave.set_value(0x02030000u, 5.4321);   // 5.4321 kW（量程内）

    RealtimeSnapshot snap;
    const bool ok = rig.io.read_snapshot(1000.0, snap);

    EXPECT(ok);
    EXPECT_NEAR(snap.p_grid_kw, 5.4321, 1e-9);
    EXPECT(rig.io.point_valid(EMS_P_GRID));
    EXPECT(snap.meters_alive.at("METER") == true);
    EXPECT(snap.meters_alive.at("BMS") == false);    // 不经本通道
    EXPECT(snap.meters_alive.at("PCS") == false);

    const DeviceStatus st = rig.io.read_status();
    EXPECT(st.meter_comm_ok);
    EXPECT(st.data_valid);
    EXPECT(!st.device_offline);
    EXPECT(!st.bms_comm_ok);
    EXPECT(!st.pcs_comm_ok);
    EXPECT_NEAR(st.last_update, 1000.0, 1e-9);

    EXPECT_EQ(rig.io.scans(), 1);
    EXPECT_EQ(rig.io.collected(), 1);
    EXPECT_EQ(rig.io.failed_reads(), 0);
    EXPECT_EQ((long long)rig.io.master().stats().ok, 1);
}

// =====================================================================
// T53 符号位端到端：反向功率（倒送电网）
// =====================================================================
static void test_t53_negative_power() {
    std::cout << "\n[T53] 反向功率：符号位不能丢\n";

    Rig rig;
    rig.slave.set_value(0x02030000u, -2.5);   // 倒送 2.5 kW

    RealtimeSnapshot snap;
    EXPECT(rig.io.read_snapshot(1000.0, snap));
    EXPECT_NEAR(snap.p_grid_kw, -2.5, 1e-9);
}

// =====================================================================
// T54 绑定表变比端到端（PT ×100）
// =====================================================================
static void test_t54_scale() {
    std::cout << "\n[T54] 变比：工程量 543.21 kW 经 PT×100 落到线上 5.4321\n";

    // 注入带变比的绑定表（现场 35kV 站的真实形态）
    const meter645::PointBinding table[] = {
        {EMS_P_GRID, "MEAS.P_GRID", 0x02030000u, {100.0, 0.0}},
    };
    Rig rig(table, 1);
    EXPECT(rig.io.map_ok());
    // 假表按同一变比回帧（工程量 543.21 → 线上 5.4321，量程内）
    rig.slave.set_scale(0x02030000u, {100.0, 0.0});
    rig.slave.set_value(0x02030000u, 543.21);

    RealtimeSnapshot snap;
    EXPECT(rig.io.read_snapshot(1000.0, snap));
    EXPECT_NEAR(snap.p_grid_kw, 543.21, 1e-6);
}

// =====================================================================
// T55 ★ 超时保留旧值（本文件头号反证目标）
//
// 契约①（04/）：采集失败时填最近一次有效值，可信性另行表达。
// 若有人把 scan() 的失败分支改成"清缓存"，第一条 EXPECT_NEAR 立刻红。
// =====================================================================
static void test_t55_stale_value_kept() {
    std::cout << "\n[T55] 超时：值还在，有效位没了\n";

    Rig rig;
    rig.slave.set_value(0x02030000u, 5.0);

    RealtimeSnapshot snap;
    EXPECT(rig.io.read_snapshot(1000.0, snap));        // 第一轮：成功
    EXPECT_NEAR(snap.p_grid_kw, 5.0, 1e-9);
    EXPECT(rig.io.point_valid(EMS_P_GRID));

    // 第二轮：表不应答（掉电/485 冲突）
    rig.slave.options().drop_response = true;
    const bool ok2 = rig.io.read_snapshot(1010.0, snap);

    EXPECT(!ok2);                                       // 快照整体不可信
    EXPECT_NEAR(snap.p_grid_kw, 5.0, 1e-9);             // ★ 值保留
    EXPECT(!rig.io.point_valid(EMS_P_GRID));            // ★ 有效位没了
    EXPECT_NEAR(rig.io.cached(EMS_P_GRID), 5.0, 1e-9);  // 缓存也没被清

    const DeviceStatus st = rig.io.read_status();
    EXPECT(!st.meter_comm_ok);
    EXPECT(!st.data_valid);
    EXPECT(st.device_offline);                          // scans>0 且本拍没采到
    EXPECT_NEAR(st.last_update, 1000.0, 1e-9);          // 时标停在最后一次成功
    EXPECT_EQ(rig.io.failed_reads(), 1);
    EXPECT_EQ((long long)rig.io.master().stats().timeouts, 3);  // 1+2 重试
}

// =====================================================================
// T56 异常应答：表说"没这个点" → 不覆盖缓存、分类计数
// =====================================================================
static void test_t56_abnormal_reply() {
    std::cout << "\n[T56] 异常应答 0xD1：确定的回答不该被重试拖慢\n";

    Rig rig;
    rig.slave.set_value(0x02030000u, 3.0);
    RealtimeSnapshot snap;
    EXPECT(rig.io.read_snapshot(1000.0, snap));
    EXPECT_NEAR(snap.p_grid_kw, 3.0, 1e-9);

    rig.slave.options().reject_all = true;
    EXPECT(!rig.io.read_snapshot(1010.0, snap));
    EXPECT_NEAR(snap.p_grid_kw, 3.0, 1e-9);             // 值保留
    EXPECT(!rig.io.point_valid(EMS_P_GRID));
    EXPECT(rig.io.last_read_status() == ReadStatus::kAbnormal);
    EXPECT_EQ((long long)rig.io.master().stats().abnormals, 1);
    // ★ 异常应答是"确定的回答"，主站纪律是不重试（§70 教训③）：
    //   1 次失败就该只发 1 帧，而不是 3 帧。
    EXPECT_EQ((long long)rig.io.master().stats().requests, 2);  // 1 成功 + 1 异常
}

// =====================================================================
// T57 写永远被拒
// =====================================================================
static void test_t57_writes_rejected() {
    std::cout << "\n[T57] 电表不能接收功率指令：写被拒且有计数\n";

    Rig rig;
    rig.slave.set_value(0x02030000u, 5.0);
    RealtimeSnapshot snap;
    EXPECT(rig.io.read_snapshot(1000.0, snap));

    PowerCommand cmd{};
    cmd.p_bat_cmd_kw = -100.0;
    EXPECT(!rig.io.write_command(cmd));
    const double fb = rig.io.execute(-100.0, 0.1);
    EXPECT_EQ(rig.io.rejected_writes(), 2);
    // 尽力反馈 = 最近一次采到的关口功率（闭环不得依赖它，契约③）
    EXPECT_NEAR(fb, 5.0, 1e-9);
    // 写尝试不得产生任何线上流量
    EXPECT_EQ((long long)rig.io.master().stats().requests, 1);
}

// =====================================================================
// T58 限值 / 容量 / live 标志
// =====================================================================
static void test_t58_limits_and_capacity() {
    std::cout << "\n[T58] 限值全零、容量 0、limits_are_live=false\n";

    Rig rig;
    DeviceLimits lim;
    EXPECT(rig.io.read_limits(lim));
    EXPECT_NEAR(lim.pcs_rated_chg_kw, 0.0, 1e-12);
    EXPECT_NEAR(lim.pcs_rated_dis_kw, 0.0, 1e-12);
    EXPECT_NEAR(lim.bms_chg_limit_kw, 0.0, 1e-12);
    EXPECT_NEAR(lim.bms_dis_limit_kw, 0.0, 1e-12);
    EXPECT_NEAR(lim.transformer_capacity_kw, 0.0, 1e-12);
    EXPECT_NEAR(lim.d_target_kw, 0.0, 1e-12);
    EXPECT(!lim.bms_chg_forbidden);
    EXPECT(!lim.bms_dis_forbidden);

    EXPECT_NEAR(rig.io.battery_capacity_kwh(), 0.0, 1e-12);
    EXPECT(!rig.io.limits_are_live());
    EXPECT(std::string(rig.io.name()).find("DL/T645") != std::string::npos);
}

// =====================================================================
// T59 快照未拥有字段 = "没有值"
// =====================================================================
static void test_t59_unowned_fields_are_zero() {
    std::cout << "\n[T59] 未拥有字段恒 0（不是读到 0）\n";

    Rig rig;
    rig.slave.set_value(0x02030000u, 5.0);
    RealtimeSnapshot snap;
    EXPECT(rig.io.read_snapshot(1000.0, snap));

    EXPECT_NEAR(snap.p_bat_actual_kw, 0.0, 1e-12);
    EXPECT_NEAR(snap.p_pv_kw, 0.0, 1e-12);
    EXPECT_NEAR(snap.p_load_kw, 0.0, 1e-12);
    EXPECT_NEAR(snap.soc, 0.0, 1e-12);
    EXPECT_NEAR(snap.temperature_c, 0.0, 1e-12);
    EXPECT_NEAR(snap.soh, 0.0, 1e-12);
    EXPECT(!snap.has_lookahead);

    // read_actuals：本通道唯一的真实量是 p_grid；soc/温度显式 0
    // （DeviceActuals 的默认值 0.5/25.0 是仿真味的，必须被盖掉）
    const DeviceActuals ac = rig.io.read_actuals();
    EXPECT_NEAR(ac.p_grid_kw, 5.0, 1e-9);
    EXPECT_NEAR(ac.soc, 0.0, 1e-12);
    EXPECT_NEAR(ac.temperature_c, 0.0, 1e-12);
}

// =====================================================================
// T60 坏表注入 → 拒绝采集（零流量）
// =====================================================================
static void test_t60_bad_map_refused() {
    std::cout << "\n[T60] 绑定表坏了：一帧都不许发\n";

    const meter645::PointBinding bad[] = {
        {EMS_P_GRID, "MEAS.WRONG", 0x02030000u, {1.0, 0.0}},   // 点名不符
    };
    Rig rig(bad, 1);
    EXPECT(!rig.io.map_ok());

    RealtimeSnapshot snap;
    EXPECT(!rig.io.read_snapshot(1000.0, snap));
    EXPECT(!rig.io.point_valid(EMS_P_GRID));
    // ★ 硬判据：线上零流量（对照 T52 的 requests==1）
    EXPECT_EQ((long long)rig.io.master().stats().requests, 0);
    EXPECT_EQ(rig.slave.rx_frames(), 0);
}

// =====================================================================
// T61 未绑定的点永远没有可信值
// =====================================================================
static void test_t61_unbound_never_valid() {
    std::cout << "\n[T61] 未绑定的点：point_valid 恒 false\n";

    Rig rig;
    rig.slave.set_value(0x02030000u, 5.0);
    RealtimeSnapshot snap;
    EXPECT(rig.io.read_snapshot(1000.0, snap));
    EXPECT(rig.io.read_snapshot(1010.0, snap));

    EXPECT(!rig.io.point_valid(EMS_SOC));
    EXPECT(!rig.io.point_valid(EMS_P_LOAD));
    EXPECT(!rig.io.point_valid(EMS_P_PV));
    EXPECT(!rig.io.point_valid(EMS_STA_METER));   // 状态位是合成的，不走绑定
    // 但它们的缓存值是诚实的 0，不是假数据
    EXPECT_NEAR(rig.io.cached(EMS_SOC), 0.0, 1e-12);
}

// =====================================================================
// T62 两点一成一败：per-point 有效位独立
//
// ★ 诚实声明：这里把 EMS_P_LOAD 绑到"A 相电压"DI 纯属**夹具构造** ——
//   语义上荒谬（负荷≠电压），只为制造"一成一败"。测的是 per-point
//   独立性这个机制本身，不是物理口径。默认表只有一行（文件头注释），
//   真实的多点绑定要等"分路表"形态出现才有意义。
// =====================================================================
static void test_t62_partial_success() {
    std::cout << "\n[T62] 一成一败：有效位逐点独立，整体降级\n";

    const meter645::PointBinding table[] = {
        {EMS_P_GRID, "MEAS.P_GRID", 0x02030000u, {1.0, 0.0}},   // 功率（夹具语义）
        {EMS_P_LOAD, "MEAS.P_LOAD", 0x02010100u, {1.0, 0.0}},   // ← 夹具：故意不给值
    };
    Rig rig(table, 2);
    EXPECT(rig.io.map_ok());

    rig.slave.set_value(0x02030000u, 5.0);   // 只有功率有值；电压 DI 表会回 0xD1
    RealtimeSnapshot snap;
    const bool ok = rig.io.read_snapshot(1000.0, snap);

    EXPECT(!ok);                              // 部分失败 → 整体不可信
    EXPECT_NEAR(snap.p_grid_kw, 5.0, 1e-9);   // 成的点照常更新
    EXPECT(rig.io.point_valid(EMS_P_GRID));
    EXPECT(!rig.io.point_valid(EMS_P_LOAD));  // 败的点：值不动、位清零
    EXPECT_NEAR(snap.p_load_kw, 0.0, 1e-12);
    EXPECT_EQ(rig.io.collected(), 1);
    EXPECT_EQ(rig.io.failed_reads(), 1);
}

// =====================================================================
// T63 计数口径
// =====================================================================
static void test_t63_counters() {
    std::cout << "\n[T63] scans / collected / failed_reads 各自独立\n";

    Rig rig;
    rig.slave.set_value(0x02030000u, 5.0);
    RealtimeSnapshot snap;

    rig.io.read_snapshot(1000.0, snap);    // 成
    rig.slave.options().drop_response = true;
    rig.io.read_snapshot(1010.0, snap);    // 败
    rig.slave.options().drop_response = false;
    rig.io.read_snapshot(1020.0, snap);    // 又成

    EXPECT_EQ(rig.io.scans(), 3);
    EXPECT_EQ(rig.io.collected(), 2);
    EXPECT_EQ(rig.io.failed_reads(), 1);
    EXPECT(rig.io.point_valid(EMS_P_GRID));   // 第三拍恢复 → 有效位回来
}

// =====================================================================
int main() {
    std::cout << "=========================================================\n"
              << " 21/ Meter645DeviceIO 单元测试（T50 ~ T63）\n"
              << "=========================================================\n";

    test_t50_builtin_map_clean();
    test_t51_map_check_catches_bad_tables();
    test_t52_happy_path();
    test_t53_negative_power();
    test_t54_scale();
    test_t55_stale_value_kept();
    test_t56_abnormal_reply();
    test_t57_writes_rejected();
    test_t58_limits_and_capacity();
    test_t59_unowned_fields_are_zero();
    test_t60_bad_map_refused();
    test_t61_unbound_never_valid();
    test_t62_partial_success();
    test_t63_counters();

    // ★ 汇总行必须从第 0 列起（build_test.bat 的 findstr /B 按行首匹配），
    //   且必须 flush —— stdout 重定向到文件时是全缓冲，进程异常终止会把
    //   汇总行吞掉而退出码仍是 0。
    std::cout << "\n=========================================\n";
    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    std::cout << "=========================================\n";
    std::fflush(stdout);
    return (g_fail == 0) ? 0 : 1;
}
