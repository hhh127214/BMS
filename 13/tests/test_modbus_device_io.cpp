// =====================================================================
// 13/ 单元测试 —— ModbusDeviceIO 适配器契约
//
//   T21: 完整快照：3 次请求读全 32 点，各点值与编码一致
//   T22: read_limits 从设备寄存器装配（含两个 BMS 禁充放安全位）
//   T23: write_command 的**原子性**：三个指令值在同一事务里下发
//   T24: 部分成功：一个分块失败时，该块的点保留旧值且标记不可信，
//        其余分块照常更新
//   T25: 解码失败（字序/NaN）不覆盖缓存，只标记不可信 ——
//        "设备没坏，是表配错了"必须和"设备答不上"分得开
//   T26: 完全未连接：data_valid 为假，值保留，不逐个点刷噪声
//   T27: 自检与能力开关
//   T28: execute() 不做物理积分（与 RtDbDeviceIO 同契约）
//   T29: BMS 禁充放位经设备寄存器生效（安全输入通路）
//
// 编译：见 13/scripts/build_test.bat
// =====================================================================

#include "modbus_device_io.h"
#include "modbus_point_map.h"
#include "fake_modbus_slave.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

using namespace ems;
using namespace ems::modbus;

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

using test::FakeModbusSlave;

// =====================================================================
// 测试夹具
// =====================================================================
namespace {

struct DeviceFixture {
    FakeModbusSlave                  slave;
    std::unique_ptr<ModbusDeviceIO>  io;

    bool up(const FakeModbusSlave::Options& opt_in = FakeModbusSlave::Options{},
            int timeout_ms = 500) {
        FakeModbusSlave::Options opt = opt_in;
        if (!slave.start(opt)) return false;
        ModbusDeviceIO::Config cfg;
        cfg.host       = "127.0.0.1";
        cfg.port       = slave.port();
        cfg.timeout_ms = timeout_ms;
        cfg.reconnect_interval_ms = 0;      // 测试里不节流，行为确定
        io.reset(new ModbusDeviceIO(cfg));
        if (!io->connect()) return false;
        return slave.wait_for_client(2000);
    }
};

// 按映射表把**业务值**写进从站寄存器 —— 测试不手写地址与字序，
// 否则映射表改一次，所有测试一起失效。
void slave_set_point(FakeModbusSlave& s, std::size_t idx, double v) {
    const PointBinding& b = kBindings[idx];
    if (b.table == Table::kInputReg) {
        std::uint16_t regs[64] = {0};
        for (std::size_t i = 0; i < 64; ++i) regs[i] = s.ir(i);
        encode_point(b, v, regs, 64, nullptr, 0);
        for (std::size_t i = 0; i < 64; ++i) s.set_ir(i, regs[i]);
    } else if (b.table == Table::kHoldingReg) {
        std::uint16_t regs[64] = {0};
        for (std::size_t i = 0; i < 64; ++i) regs[i] = s.hr(i);
        encode_point(b, v, regs, 64, nullptr, 0);
        for (std::size_t i = 0; i < 64; ++i) s.set_hr(i, regs[i]);
    } else if (b.table == Table::kDiscreteInput) {
        bool bits[64] = {false};
        for (std::size_t i = 0; i < 64; ++i) bits[i] = s.di(i);
        encode_point(b, v, nullptr, 0, bits, 64);
        for (std::size_t i = 0; i < 64; ++i) s.set_di(i, bits[i]);
    }
}

// 一套完整、自洽的"设备侧现状"
void fill_nominal(FakeModbusSlave& s) {
    slave_set_point(s, EMS_P_LOAD,  380.0);
    slave_set_point(s, EMS_P_PV,    150.0);
    slave_set_point(s, EMS_P_BAT,   -50.0);
    slave_set_point(s, EMS_P_GRID,  232.0);   // 独立给值（电表口径，不由三路推）
    slave_set_point(s, EMS_SOC,     0.55);
    slave_set_point(s, EMS_T_C,     27.5);
    slave_set_point(s, EMS_SOH,     0.98);

    slave_set_point(s, EMS_CFG_CAP_KWH,   1000.0);
    slave_set_point(s, EMS_CFG_MAX_CHG,    200.0);
    slave_set_point(s, EMS_CFG_MAX_DIS,    200.0);
    slave_set_point(s, EMS_CFG_BMS_CHG_LIM, 180.0);
    slave_set_point(s, EMS_CFG_BMS_DIS_LIM, 190.0);
    slave_set_point(s, EMS_CFG_TR_KVA,     250.0);
    slave_set_point(s, EMS_CFG_D_TARGET,   250.0);
    slave_set_point(s, EMS_CFG_TAU_S,        3.0);
    slave_set_point(s, EMS_CFG_RAMP_KW_S,   50.0);
    slave_set_point(s, EMS_CFG_STANDBY,      2.0);
    slave_set_point(s, EMS_CFG_ETA_CHG,     0.96);
    slave_set_point(s, EMS_CFG_ETA_DIS,     0.96);
    slave_set_point(s, EMS_CFG_SOC_MIN,     0.05);
    slave_set_point(s, EMS_CFG_SOC_MAX,     0.95);

    slave_set_point(s, EMS_STA_BMS,   1.0);
    slave_set_point(s, EMS_STA_PCS,   1.0);
    slave_set_point(s, EMS_STA_METER, 1.0);
    slave_set_point(s, EMS_STA_FAULT, 0.0);
    slave_set_point(s, EMS_STA_OFFLINE, 0.0);
    slave_set_point(s, EMS_STA_VALID, 1.0);
    slave_set_point(s, EMS_STA_BMS_CHG_FORBID, 0.0);
    slave_set_point(s, EMS_STA_BMS_DIS_FORBID, 0.0);
}

} // namespace

// =====================================================================
// T21 完整快照
// =====================================================================
static void test_21_snapshot() {
    std::printf("T21 完整快照\n");
    DeviceFixture f;
    if (!f.up()) { EXPECT(false); return; }
    fill_nominal(f.slave);

    RealtimeSnapshot snap;
    const bool valid = f.io->read_snapshot(1234.0, snap);

    EXPECT(valid);
    EXPECT_NEAR(snap.timestamp, 1234.0, 1e-9);
    EXPECT_NEAR(snap.p_bat_actual_kw, -50.0, 1e-3);
    EXPECT_NEAR(snap.p_pv_kw, 150.0, 1e-3);
    // 站用电计入负荷侧（与其它三个适配器同一口径）
    EXPECT_NEAR(snap.p_load_kw, 380.0 + 2.0, 1e-3);
    // 关口功率 = 电表寄存器，**不是** 382-150-(-50) = 282
    EXPECT_NEAR(snap.p_grid_kw, 232.0, 1e-3);
    EXPECT_NEAR(snap.soc, 0.55, 1e-9);
    EXPECT_NEAR(snap.temperature_c, 27.5, 1e-9);
    EXPECT_NEAR(snap.soh, 0.98, 1e-9);
    EXPECT(!snap.has_lookahead);

    EXPECT(snap.meters_alive.at("BMS"));
    EXPECT(snap.meters_alive.at("PCS"));
    EXPECT(snap.meters_alive.at("METER"));

    // 反向守卫：口径若退回"三路相减"，上面那条 232 会变成 282
    EXPECT(std::fabs(snap.p_grid_kw - (snap.p_load_kw - snap.p_pv_kw -
                                       snap.p_bat_actual_kw)) > 40.0);

    // read_actuals 与快照同口径
    const DeviceActuals a = f.io->read_actuals();
    EXPECT_NEAR(a.p_grid_kw, snap.p_grid_kw, 1e-9);
    EXPECT_NEAR(a.p_bat_kw, snap.p_bat_actual_kw, 1e-9);
}

// =====================================================================
// T22 一份快照只花 3 次请求
// =====================================================================
static void test_22_request_count() {
    std::printf("T22 请求次数（分块读）\n");
    DeviceFixture f;
    if (!f.up()) { EXPECT(false); return; }
    fill_nominal(f.slave);

    const int before = f.io->client().requests();
    RealtimeSnapshot snap;
    f.io->read_snapshot(1.0, snap);
    const int used = f.io->client().requests() - before;

    // ★ 这是契约：32 个点 = 3 次请求（IR 40 + HR 6 + DI 8）。
    //   谁改回"一点一次请求"，这里立刻变 32。
    EXPECT_EQ(used, 3);
    EXPECT_EQ(f.io->full_scan_request_count(), 3);
    EXPECT_EQ(f.io->scans(), 1);
    EXPECT_EQ(f.io->partial_scans(), 0);
    EXPECT_EQ(f.io->block_fails(), 0);
    EXPECT_EQ(f.io->decode_fails(), 0);

    // read_limits 只扫它需要的两块（limits_scope_only 默认开）
    const int before2 = f.io->client().requests();
    DeviceLimits lim;
    f.io->read_limits(lim);
    EXPECT_EQ(f.io->client().requests() - before2, 2);
}

// =====================================================================
// T23 读限值（含两个 BMS 禁充放安全位）
// =====================================================================
static void test_23_limits() {
    std::printf("T23 读限值\n");
    DeviceFixture f;
    if (!f.up()) { EXPECT(false); return; }
    fill_nominal(f.slave);

    DeviceLimits lim;
    EXPECT(f.io->read_limits(lim));
    EXPECT_NEAR(lim.pcs_rated_chg_kw, 200.0, 1e-3);
    EXPECT_NEAR(lim.pcs_rated_dis_kw, 200.0, 1e-3);
    EXPECT_NEAR(lim.bms_chg_limit_kw, 180.0, 1e-3);
    EXPECT_NEAR(lim.bms_dis_limit_kw, 190.0, 1e-3);
    EXPECT_NEAR(lim.transformer_capacity_kw, 250.0, 1e-3);
    EXPECT_NEAR(lim.d_target_kw, 250.0, 1e-3);
    EXPECT(!lim.bms_chg_forbidden);
    EXPECT(!lim.bms_dis_forbidden);
    // ★ "限值是活的"必须为真：这两个位与 CFG.* 都在设备寄存器里，
    //   现场随时会变；返回 false 会让 EmsRuntime 只在装配期读一次，
    //   等于把 BMS 动态降功率与禁充放位当常量用。
    EXPECT(f.io->limits_are_live());
}

// =====================================================================
// T24 write_command 的原子性
// =====================================================================
static void test_24_write_command() {
    std::printf("T24 指令原子下发\n");
    DeviceFixture f;
    if (!f.up()) { EXPECT(false); return; }
    fill_nominal(f.slave);

    const int wr_before = f.slave.write_requests();
    const int req_before = f.io->client().requests();

    PowerCommand cmd;
    cmd.p_bat_cmd_kw = -120.0;
    cmd.p_upper      =  80.0;
    cmd.p_lower      = -200.0;
    EXPECT(f.io->write_command(cmd));

    // ★ 一次事务：三个值必须一起到，不能出现"新功率 + 旧区间"的中间态
    EXPECT_EQ(f.slave.write_requests() - wr_before, 1);
    EXPECT_EQ(f.io->client().requests() - req_before, 1);

    // 从站寄存器里三个值都正确（字序、位置都对）
    double v = 0.0;
    std::uint16_t hr[64] = {0};
    for (std::size_t i = 0; i < 64; ++i) hr[i] = f.slave.hr(i);
    EXPECT(decode_point(kBindings[EMS_CMD_P_BAT], hr, 64, nullptr, 0, &v) ==
           DecodeError::kNone);
    EXPECT_NEAR(v, -120.0, 1e-4);
    EXPECT(decode_point(kBindings[EMS_CMD_P_UPPER], hr, 64, nullptr, 0, &v) ==
           DecodeError::kNone);
    EXPECT_NEAR(v, 80.0, 1e-4);
    EXPECT(decode_point(kBindings[EMS_CMD_P_LOWER], hr, 64, nullptr, 0, &v) ==
           DecodeError::kNone);
    EXPECT_NEAR(v, -200.0, 1e-4);

    EXPECT_EQ(f.io->writes(), 1);
    EXPECT_EQ(f.io->write_failures(), 0);

    // 非法指令（非有限值）必须**拒绝下发**，而不是写一个凑合的值
    PowerCommand bad;
    bad.p_bat_cmd_kw = std::nan("");
    bad.p_upper = 0.0;
    bad.p_lower = 0.0;
    EXPECT(!f.io->write_command(bad));
    EXPECT_EQ(f.io->encode_failures(), 1);
    EXPECT_EQ(f.slave.write_requests() - wr_before, 1);   // 没有多写
}

// =====================================================================
// T25 部分成功
//
// Modbus 一次快照是**一组**请求，"输入寄存器读到了、离散输入超时了"是
// 运行常态。若实现把整帧判死，一次抖动就会让整拍量测不可用；
// 若实现把失败块的点偷偷填 0，算法会拿 0 当量测用（0 kW 负荷看起来很正常）。
// =====================================================================
static void test_25_partial_success() {
    std::printf("T25 部分成功\n");
    DeviceFixture f;
    if (!f.up()) { EXPECT(false); return; }
    fill_nominal(f.slave);

    // 第一次全好
    RealtimeSnapshot snap;
    EXPECT(f.io->read_snapshot(1.0, snap));
    EXPECT_NEAR(snap.p_bat_actual_kw, -50.0, 1e-3);
    EXPECT(f.io->point_valid(EMS_STA_BMS));

    // 让离散输入（状态位）这一块开始失败 —— 只有 DI 块，IR/HR 照常
    f.slave.set_reject_fc(fc::kReadDiscreteInputs);

    // 同时把量测改掉，用于验证"IR 块仍然更新"
    slave_set_point(f.slave, EMS_P_BAT, -77.0);

    RealtimeSnapshot snap2;
    f.io->read_snapshot(2.0, snap2);

    // IR 块成功 → 量测更新了
    EXPECT_NEAR(snap2.p_bat_actual_kw, -77.0, 1e-3);
    // DI 块失败 → 状态位**保留旧值**但标记不可信
    EXPECT(!f.io->point_valid(EMS_STA_BMS));
    EXPECT(!f.io->point_valid(EMS_STA_PCS));
    // 量测点仍然可信
    EXPECT(f.io->point_valid(EMS_P_BAT));
    EXPECT(f.io->point_valid(EMS_P_GRID));

    EXPECT(f.io->partial_scans() >= 1);
    EXPECT(f.io->block_fails() >= 1);
    // ★ 解码失败**为 0** —— 这一条把"通信问题"与"映射表配错"分开了。
    //   现场排障时这两个方向的排查路径完全不同。
    EXPECT_EQ(f.io->decode_fails(), 0);

    // data_valid 变假（有块失败），此时静默换上去的值不会进安全层
    EXPECT(!f.io->read_status().data_valid);
    // 但 meters_alive 也必须假（否则 05/ 会拿一个没采到的"通信正常位"当真）
    EXPECT(!snap2.meters_alive.at("BMS"));
}

// =====================================================================
// T26 解码失败不覆盖缓存
// =====================================================================
static void test_26_decode_failure() {
    std::printf("T26 解码失败（字序/NaN）\n");
    DeviceFixture f;
    if (!f.up()) { EXPECT(false); return; }
    fill_nominal(f.slave);

    RealtimeSnapshot snap;
    EXPECT(f.io->read_snapshot(1.0, snap));
    EXPECT_NEAR(snap.p_bat_actual_kw, -50.0, 1e-3);

    // 从站把 MEAS.P_BAT 写成 NaN（字序配错时的典型产物）
    const float nan_f = std::nanf("");
    std::uint16_t two[2] = {0, 0};
    std::memcpy(two, &nan_f, 4);
    f.slave.set_ir(4, two[0]);
    f.slave.set_ir(5, two[1]);

    RealtimeSnapshot snap2;
    f.io->read_snapshot(2.0, snap2);

    // ★ 值**保留旧值**（不是 0、不是 NaN）
    EXPECT_NEAR(snap2.p_bat_actual_kw, -50.0, 1e-3);
    EXPECT(std::isfinite(snap2.p_bat_actual_kw));
    // 但这一点**必须标记不可信**
    EXPECT(!f.io->point_valid(EMS_P_BAT));
    EXPECT(f.io->decode_fails() >= 1);
    EXPECT(f.io->last_decode_error() == DecodeError::kNonFinite);
    // 同一块里的其它点不受影响
    EXPECT(f.io->point_valid(EMS_P_GRID));
    // 通信是好的（块请求成功）—— "设备没坏，是表配错了"
    EXPECT_EQ(f.io->block_fails(), 0);
}

// =====================================================================
// T27 完全未连接
// =====================================================================
static void test_27_unconnected() {
    std::printf("T27 未连接\n");
    // 起一个从站拿到端口后立刻关掉 → 该端口上没有监听者
    std::uint16_t dead_port = 0;
    {
        FakeModbusSlave tmp;
        tmp.start();
        dead_port = tmp.port();
    }

    ModbusDeviceIO::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = dead_port;
    cfg.timeout_ms = 300;
    cfg.reconnect_interval_ms = 0;
    ModbusDeviceIO io(cfg);

    EXPECT(!io.connect());
    EXPECT(!io.is_connected());

    RealtimeSnapshot snap;
    const bool valid = io.read_snapshot(1.0, snap);
    EXPECT(!valid);
    EXPECT(!io.read_status().data_valid);
    // ★ 未连接时全部 32 个点都不可信 —— 这是**一个**事实的完整表达，
    //   而不是"逐个点记 32 条 stale 日志"那种噪声
    EXPECT_EQ(io.stale_points(), (int)kBindingCount);
    EXPECT(io.unconnected_scans() >= 1);
    // 值保持默认 0（**没有**编造一个"看起来正常"的值）
    EXPECT_NEAR(snap.p_bat_actual_kw, 0.0, 1e-12);
    EXPECT(!io.point_valid(EMS_P_BAT));
    // 容量读不到时返回 0（而不是一个会让优化层安静算错的默认容量）
    EXPECT_NEAR(io.battery_capacity_kwh(), 0.0, 1e-12);
}

// =====================================================================
// T28 自检与能力开关
// =====================================================================
static void test_28_self_check() {
    std::printf("T28 自检\n");
    DeviceFixture f;
    if (!f.up()) { EXPECT(false); return; }

    EXPECT_EQ(f.io->self_check(), 0);
    EXPECT(f.io->limits_are_live());
    EXPECT(std::strcmp(f.io->name(), "ModbusDeviceIO(Modbus TCP)") == 0);

    // 映射表逐点覆盖点表（32 点）
    EXPECT_EQ(mapped_point_count(), (std::size_t)kBindingCount);
    for (std::size_t i = 0; i < kBindingCount; ++i) {
        EXPECT(binding_for_name(EMS_POINT_NAMES[i]) != nullptr);
    }
}

// =====================================================================
// T29 execute() 不做物理积分
//
// 与 RtDbDeviceIO 同契约（04/src/device_io.h ③）：真实适配器只负责
// "把指令发出去"，实际功率要等下一拍的量测。返回值仅供记录。
// 如果这里偷偷做了积分，仿真与现场就会走出两条不同的闭环 ——
// 而"测过的不是要跑的"正是 P0 抽象要消灭的东西。
// =====================================================================
static void test_29_execute_no_integration() {
    std::printf("T29 execute 不做积分\n");
    DeviceFixture f;
    if (!f.up()) { EXPECT(false); return; }
    fill_nominal(f.slave);

    PowerCommand cmd;
    cmd.p_bat_cmd_kw = -20.0;
    cmd.p_upper = 50.0;
    cmd.p_lower = -100.0;
    f.io->write_command(cmd);

    RealtimeSnapshot snap;
    f.io->read_snapshot(1.0, snap);          // 此时设备侧 P_BAT = -50

    // 请求 0 kW，execute 只把指令写下去；返回值是**上一拍量测**
    const double ret = f.io->execute(0.0, 0.1);
    EXPECT_NEAR(ret, -50.0, 1e-3);           // 不是 0，也不是积分结果

    // 设备侧确实收到了 0（FC16 写到 CMD.P_BAT）
    std::uint16_t hr[64] = {0};
    for (std::size_t i = 0; i < 64; ++i) hr[i] = f.slave.hr(i);
    double v = 0.0;
    EXPECT(decode_point(kBindings[EMS_CMD_P_BAT], hr, 64, nullptr, 0, &v) ==
           DecodeError::kNone);
    EXPECT_NEAR(v, 0.0, 1e-6);

    // ★ execute **不能**动权限区间（那是 write_command 的职责）。
    //   越俎代庖会把"装配层从未下发过区间"变成"下发过一个凭空造的区间"，
    //   而后者在设备侧看起来完全合法。
    EXPECT(decode_point(kBindings[EMS_CMD_P_UPPER], hr, 64, nullptr, 0, &v) ==
           DecodeError::kNone);
    EXPECT_NEAR(v, 50.0, 1e-4);              // 仍是 write_command 下的值
    EXPECT(decode_point(kBindings[EMS_CMD_P_LOWER], hr, 64, nullptr, 0, &v) ==
           DecodeError::kNone);
    EXPECT_NEAR(v, -100.0, 1e-4);
}

// =====================================================================
// T30 BMS 禁充放位（安全输入通路）
//
// 这两个 bool 是 04/ S01(kBmsForbid, **L0 最底层**)与 05/ check_bms_forbid()
// 的唯一输入。它在 Modbus 路线上必须真的从**设备寄存器**读进来 ——
// 若适配器硬写 false，等于断言"BMS 永远允许充放"，而三层测试全绿
// （A1 就是这么栽的）。
// =====================================================================
static void test_30_bms_forbid() {
    std::printf("T30 BMS 禁充放安全位\n");
    DeviceFixture f;
    if (!f.up()) { EXPECT(false); return; }
    fill_nominal(f.slave);

    DeviceLimits lim;
    EXPECT(f.io->read_limits(lim));
    EXPECT(!lim.bms_chg_forbidden);
    EXPECT(!lim.bms_dis_forbidden);

    // 设备上报"禁止放电"
    slave_set_point(f.slave, EMS_STA_BMS_DIS_FORBID, 1.0);
    EXPECT(f.io->read_limits(lim));
    EXPECT(lim.bms_dis_forbidden);
    EXPECT(!lim.bms_chg_forbidden);          // 不串扰：禁放只禁放

    // 两个都禁
    slave_set_point(f.slave, EMS_STA_BMS_CHG_FORBID, 1.0);
    EXPECT(f.io->read_limits(lim));
    EXPECT(lim.bms_chg_forbidden);
    EXPECT(lim.bms_dis_forbidden);

    // 撤回 —— 安全位必须**可恢复**（不是锁存）。若实现把它锁存住，
    // 一次瞬时上报会让整套系统永久停摆，现场表现为"要重启才行"。
    slave_set_point(f.slave, EMS_STA_BMS_CHG_FORBID, 0.0);
    slave_set_point(f.slave, EMS_STA_BMS_DIS_FORBID, 0.0);
    EXPECT(f.io->read_limits(lim));
    EXPECT(!lim.bms_chg_forbidden);
    EXPECT(!lim.bms_dis_forbidden);
}

// =====================================================================
int main() {
    std::printf("=== 13/ ModbusDeviceIO 适配器契约 单元测试 ===\n\n");

    test_21_snapshot();
    test_22_request_count();
    test_23_limits();
    test_24_write_command();
    test_25_partial_success();
    test_26_decode_failure();
    test_27_unconnected();
    test_28_self_check();
    test_29_execute_no_integration();
    test_30_bms_forbid();

    std::printf("\n");
    if (g_fail == 0) {
        std::printf("ALL TESTS PASSED\n");
    }
    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
