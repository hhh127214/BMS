// =====================================================================
// P3/ 单元测试 —— Modbus 通信适配器（ModbusDeviceIO）
//
//   T31: 帧逐字节 —— MBAP/PDU 布局、CRC16 标准自检值、**字序**（ABCD vs CDAB）
//   T32: 异常响应与采集失败 —— 异常码不被当成数据；失败保留最近有效值
//   T33: 寄存器映射契约 —— 映射表 ↔ 点名真相源 ↔ 寄存器区间三者一致
//   T34: 闭环等价 —— Modbus 字节流 vs 进程内点表，400 拍逐位一致；
//        另做**量化边界**：标准 float32 映射下的偏差量级 + 决策拓扑不变
//   T35: 断链与自愈 —— 从站沉默 → 保留旧值 + 不可信；链路在但心跳丢 → HOLD_LAST
//   T36: 故障经寄存器驱动状态机（设备 → EMS 方向的真闭环）
//
// 为什么 T34 是本次的核心证据：
//   P0.5 证明过"换数据源不改算法"在进程内成立，RT_DB 证明过跨内存边界成立。
//   Modbus 与前两者的区别是：**数据不在内存里，而在报文里**。地址映射、
//   数值编码（含字序）、请求/响应配对、异常与超时都要参与，任何一环写错
//   都会让指令序列偏离。若算法在"报文介质"下仍与参考实现逐位一致，才说明
//   抽象真的立在"介质不可见"的地方。
//
// 为什么用 f64 宽精度映射做等价、用 f32 标准映射做量化边界：
//   "逐位等价"要求介质能承载 double 的全部有效位。float32 只有 24 位有效位，
//   承载不了 —— 这不是 bug，而是介质的**分辨率**。两件事必须分开验：
//     · 等价性（T34a）：映射只改"怎么传"，不该改"算出什么" → 用 f64 映射
//     · 量化边界（T34b）：f32 会带来多大偏差、会不会改变决策拓扑 → 用 f32 映射
//   混在一起验的话，等价性一旦失败就分不清是"代码写错了"还是"精度不够"。
//
// 编译：见 P3/scripts/build_test.bat
// =====================================================================

#include "data_models.h"
#include "device_io.h"
#include "ems_point_table.h"
#include "memory_device_io.h"
#include "modbus_codec.h"
#include "modbus_device_io.h"
#include "modbus_slave_sim.h"
#include "realtime_loop.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

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

// =====================================================================
// 公共装置
// =====================================================================
static bool almost_equal(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

static std::string hex_of(const uint8_t* p, size_t n) {
    static const char* d = "0123456789ABCDEF";
    std::string s;
    char buf[4];
    for (size_t i = 0; i < n; ++i) {
        buf[0] = d[(p[i] >> 4) & 0xF];
        buf[1] = d[p[i] & 0xF];
        buf[2] = '\0';
        if (i) s += ' ';
        s += buf;
    }
    return s;
}

// 环境脚本（负荷/光伏）——**按控制拍序号**给出，任何两路运行必须用同一份
static void env_at(int step, double& p_load_kw, double& p_pv_kw) {
    const double t = step * 0.1;
    p_load_kw = 380.0 + 120.0 * std::sin(t / 30.0);
    p_pv_kw   = (t > 5.0 && t < 25.0) ? 150.0 + 60.0 * std::sin(t / 5.0) : 0.0;
}

// 两路运行必须用**完全相同的装配序列**，否则比的是装配差异不是介质差异。
// 与 07/tests/test_rtdb_device_io.cpp 的 configure_runtime() 逐行一致。
static void configure_runtime(EmsRuntime& rt) {
    rt.config().dt_s = 0.1;
    rt.config().enable_realtime_correction = true;
    rt.config().l2_correction_max_kw = 100.0;
    rt.safety_params().grid_p_min_kw = -1e9;   // 隔离并网倒送约束（本测试不测它）
    rt.safety_params().ramp_kw_per_s = 1e9;    // 隔离变化率限制
    rt.apply_configs();
    rt.fsm().request_run(true);                // 状态机默认停在 READY，需显式启动许可
    rt.device_limits().transformer_capacity_kw = 800.0;  // 隔离变压器极端过载误触发
    rt.device_limits().d_target_kw = 250.0;
}

// ---- 参考实现：EmsRuntime 直连进程内点表（P0.5 已证）----
struct MemoryRig {
    MemoryDeviceIO dev;
    EmsRuntime     rt;

    MemoryRig() {
        rt.attach_device(&dev);
        configure_runtime(rt);
    }
    void run(int n, int step_base = 0) {
        rt.run(n, 0.1, [&](EmsRuntime&, int i) {
            double load = 0.0, pv = 0.0;
            env_at(step_base + i, load, pv);
            dev.set_environment(load, pv);
        });
    }
};

// ---- 被测实现：EmsRuntime —— ModbusDeviceIO ——[Modbus 报文]—— 从站 ----
struct ModbusRig {
    MemoryDeviceIO          dev;        // 设备替身（现场是 PCS/BMS/电表本体）
    ModbusSlaveSim          slave;      // 从站：寄存器文件 + 请求处理
    LoopbackModbusTransport transport;  // 环回：只跳过内核 socket
    ModbusDeviceIO          io;
    EmsRuntime              rt;

    explicit ModbusRig(MapProfile profile)
        : slave(profile), transport(&slave), io(&transport, profile, 1) {
        slave.attach_device(&dev);
        slave.publish_device_points();       // 装配前必须先把 CFG/STA/MEAS 写进寄存器
        io.set_device_pump([&](double cmd, double dt) {
            dev.execute(cmd, dt);            // 设备推进一拍
            slave.publish_device_points();   // 量测回写寄存器
        });
        io.open();
        rt.attach_device(&io);               // ← 读限值：来自 CFG.*（寄存器）
        configure_runtime(rt);
    }
    void run(int n, int step_base = 0) {
        rt.run(n, 0.1, [&](EmsRuntime&, int i) {
            double load = 0.0, pv = 0.0;
            env_at(step_base + i, load, pv);
            dev.set_environment(load, pv);   // 环境由设备侧（电表）刷新
            slave.publish_device_points();
        });
    }
};

// 逐位比较两条 StepRecord 序列，返回不同的拍数
static int diff_logs(const std::vector<StepRecord>& a, const std::vector<StepRecord>& b,
                     double eps = 0.0, bool verbose = false) {
    int diff = 0;
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const StepRecord& x = a[i];
        const StepRecord& y = b[i];
        const bool same =
            almost_equal(x.p_cmd, y.p_cmd, eps) && almost_equal(x.p_actual, y.p_actual, eps) &&
            almost_equal(x.p_grid, y.p_grid, eps) && almost_equal(x.soc, y.soc, eps) &&
            almost_equal(x.temp, y.temp, eps) && almost_equal(x.p_lower, y.p_lower, eps) &&
            almost_equal(x.p_upper, y.p_upper, eps) && almost_equal(x.plan_target, y.plan_target, eps) &&
            almost_equal(x.correction, y.correction, eps) && almost_equal(x.t, y.t, eps) &&
            x.state == y.state && x.clamped == y.clamped &&
            x.safety_clip == y.safety_clip && x.state_gated == y.state_gated &&
            x.hold_last == y.hold_last && x.fault_bits == y.fault_bits &&
            x.reason == y.reason;
        if (!same) {
            ++diff;
            if (verbose && diff <= 3) {
                std::cerr << "        第 " << i << " 拍不同: A(cmd=" << x.p_cmd
                          << " soc=" << x.soc << " state=" << state_name(x.state)
                          << " reason=" << x.reason << ")  B(cmd=" << y.p_cmd
                          << " soc=" << y.soc << " state=" << state_name(y.state)
                          << " reason=" << y.reason << ")\n";
            }
        }
    }
    return diff;
}

// =====================================================================
// T31: 帧逐字节（MBAP/PDU/CRC16/字序）
// =====================================================================
static void test_31_frames_byte_exact() {
    std::cerr << "[T31] 帧逐字节：MBAP/PDU 布局、CRC16、字序（ABCD vs CDAB） ...\n";

    uint8_t f[320];

    // ---- 读保持寄存器请求：tid=1 unit=1 fc=03 addr=0x0000 qty=14（= MEAS 块 f32 宽度）----
    const size_t n1 = build_read_request(f, sizeof(f), 1, 1, kReadHoldingRegs, 0x0000, 14);
    const uint8_t exp1[] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x01,
                            0x03, 0x00, 0x00, 0x00, 0x0E};
    EXPECT(n1 == sizeof(exp1));
    EXPECT(std::memcmp(f, exp1, sizeof(exp1)) == 0);
    if (std::memcmp(f, exp1, sizeof(exp1)) != 0) {
        std::cerr << "        实际: " << hex_of(f, n1) << "\n";
    }

    // ---- 写多寄存器请求：tid=2 unit=1 addr=0x0100 qty=2 regs={0x3F80,0x0000}(=1.0f) ----
    const uint16_t regs2[2] = {0x3F80, 0x0000};
    const size_t n2 = build_write_multiple_request(f, sizeof(f), 2, 1, 0x0100, regs2, 2);
    const uint8_t exp2[] = {0x00, 0x02, 0x00, 0x00, 0x00, 0x0B, 0x01,
                            0x10, 0x01, 0x00, 0x00, 0x02, 0x04, 0x3F, 0x80, 0x00, 0x00};
    EXPECT(n2 == sizeof(exp2));
    EXPECT(std::memcmp(f, exp2, sizeof(exp2)) == 0);
    if (std::memcmp(f, exp2, sizeof(exp2)) != 0) {
        std::cerr << "        实际: " << hex_of(f, n2) << "\n";
    }

    // ---- 写单寄存器请求：tid=3 unit=1 addr=0x0110 value=0x1234 ----
    const size_t n3 = build_write_single_request(f, sizeof(f), 3, 1, 0x0110, 0x1234);
    const uint8_t exp3[] = {0x00, 0x03, 0x00, 0x00, 0x00, 0x06, 0x01,
                            0x06, 0x01, 0x10, 0x12, 0x34};
    EXPECT(n3 == sizeof(exp3));
    EXPECT(std::memcmp(f, exp3, sizeof(exp3)) == 0);

    // ---- 容量不足必须拒绝（而不是截断写坏内存）----
    EXPECT(build_read_request(f, 4, 1, 1, kReadHoldingRegs, 0, 1) == 0);

    // ---- CRC16 标准自检值 ----
    const char* chk = "123456789";
    EXPECT(crc16(reinterpret_cast<const uint8_t*>(chk), 9) == 0x4B37);

    // ---- 字序：同一个 1.0f，两种字序的寄存器数组互为逆序 ----
    uint16_t be[2] = {0, 0}, cd[2] = {0, 0};
    encode_value(1.0, RegType::kFloat32BE, be);
    encode_value(1.0, RegType::kFloat32Swap, cd);
    EXPECT(be[0] == 0x3F80 && be[1] == 0x0000);
    EXPECT(cd[0] == 0x0000 && cd[1] == 0x3F80);
    EXPECT_NEAR(decode_value(be, RegType::kFloat32BE), 1.0, 1e-12);
    EXPECT_NEAR(decode_value(cd, RegType::kFloat32Swap), 1.0, 1e-12);

    // 一般值：两种字序解出来必须完全相同，且寄存器互为交换
    uint16_t a1[2] = {0, 0}, a2[2] = {0, 0};
    encode_value(12.34, RegType::kFloat32BE, a1);
    encode_value(12.34, RegType::kFloat32Swap, a2);
    EXPECT(a1[0] == a2[1] && a1[1] == a2[0]);
    EXPECT_NEAR(decode_value(a1, RegType::kFloat32BE), decode_value(a2, RegType::kFloat32Swap),
                1e-15);
    EXPECT(std::fabs(decode_value(a1, RegType::kFloat32BE) - 12.34) < 1e-5);

    // 负值两种字序同样成立
    uint16_t b1[2] = {0, 0}, b2[2] = {0, 0};
    encode_value(-321.75, RegType::kFloat32BE, b1);
    encode_value(-321.75, RegType::kFloat32Swap, b2);
    EXPECT(b1[0] == b2[1] && b1[1] == b2[0]);
    EXPECT_NEAR(decode_value(b1, RegType::kFloat32BE), -321.75, 1e-5);

    // ---- f64 宽精度：编解码对 double 必须**位级恒等**（等价性验收的前提）----
    const double probes[] = {0.1, -1.0 / 3.0, 380.0 + 120.0 * 0.7071067811865476,
                             1e-7, 12345.6789012345};
    for (double v : probes) {
        uint16_t r[4] = {0, 0, 0, 0};
        encode_value(v, RegType::kFloat64BE, r);
        const double back = decode_value(r, RegType::kFloat64BE);
        EXPECT(std::memcmp(&v, &back, 8) == 0);
    }

    // ---- 整数类型 ----
    uint16_t i1[1] = {0};
    encode_value(-100.0, RegType::kInt16, i1);
    EXPECT(i1[0] == 0xFF9C);
    EXPECT_NEAR(decode_value(i1, RegType::kInt16), -100.0, 1e-12);

    uint16_t u1[1] = {0};
    encode_value(65535.0, RegType::kUInt16, u1);
    EXPECT(u1[0] == 0xFFFF);
    EXPECT_NEAR(decode_value(u1, RegType::kUInt16), 65535.0, 1e-12);

    uint16_t u2[2] = {0, 0};
    encode_value(70000.0, RegType::kUInt32BE, u2);
    EXPECT(u2[0] == 0x0001 && u2[1] == 0x1170);
    EXPECT_NEAR(decode_value(u2, RegType::kUInt32BE), 70000.0, 1e-12);

    // ---- MBAP 自洽校验：协议标识必须为 0；len 字段必须与实际字节数一致 ----
    MbapHeader h;
    const uint8_t bad_proto[] = {0x00, 0x01, 0x00, 0x07, 0x00, 0x06, 0x01,
                                 0x03, 0x00, 0x00, 0x00, 0x0E};
    EXPECT(!parse_mbap(bad_proto, sizeof(bad_proto), h));
    const uint8_t short_len[] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x01,
                                 0x03, 0x00, 0x00};            // 只有 10 字节，len=6 要求 12
    EXPECT(!parse_mbap(short_len, sizeof(short_len), h));
    EXPECT(parse_mbap(exp1, sizeof(exp1), h));
    EXPECT(h.tid == 1 && h.unit == 1 && h.len == 6);

    // ---- 异常响应解析：功能码最高位置 1 ----
    const uint8_t exc[] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x01, 0x83, 0x02};
    PduView pdu;
    EXPECT(parse_response(exc, sizeof(exc), h, pdu));
    ExceptionCode ec = kExcNone;
    EXPECT(is_exception(pdu, ec));
    EXPECT(ec == kExcIllegalDataAddr);
    EXPECT(pdu.fc == 0x83);
    // 正常响应不能被误判成异常
    PduView ok;
    EXPECT(parse_response(exp1, sizeof(exp1), h, ok));
    ExceptionCode ec2 = kExcNone;
    EXPECT(!is_exception(ok, ec2));

    std::cerr << "        帧长: 读=" << n1 << " 写多=" << n2 << " 写单=" << n3
              << " / CRC16(\"123456789\")=0x4B37\n";
}

// =====================================================================
// T32: 异常响应与采集失败
// =====================================================================
static void test_32_exception_and_stale() {
    std::cerr << "[T32] 异常响应与采集失败：异常不当数据、失败保留旧值 ...\n";

    ModbusRig rig(MapProfile::kWideF64);
    EXPECT(rig.io.is_open());
    EXPECT(rig.io.self_check() == 0);

    rig.run(40);
    const double p_bat_before = rig.io.cached_value(EMS_P_BAT);
    const int    stale_before = rig.io.stale_reads();
    EXPECT(std::fabs(p_bat_before) > 1.0);          // 确实在出力，后面的"保留旧值"才有意义
    EXPECT(stale_before == 0);

    // ---- ① 从站回异常码：不能被当成数据 ----
    rig.transport.inject_exception(kExcIllegalDataValue, 1);
    RealtimeSnapshot s;
    EXPECT(rig.io.read_snapshot(0.0, s) == false);
    EXPECT(rig.io.exceptions() > 0);
    EXPECT(rig.io.stale_reads() > stale_before);
    EXPECT(rig.io.read_status().data_valid == false);
    // 失败时**保留最近一次有效值**（接口契约①），而不是清零
    EXPECT_NEAR(rig.io.cached_value(EMS_P_BAT), p_bat_before, 1e-9);

    // ---- ② 恢复后又可以读到 ----
    RealtimeSnapshot s2;
    EXPECT(rig.io.read_snapshot(0.0, s2));
    EXPECT(rig.io.read_status().data_valid == true);

    // ---- ③ 非法地址读 → 从站回 0x02 ----
    {
        uint8_t req[64], resp[320];
        const size_t n = build_read_request(req, sizeof(req), 0x77, 1, kReadHoldingRegs,
                                            rig.slave.regs_used(), 4);
        const size_t rl = rig.slave.handle(req, n, resp, sizeof(resp));
        MbapHeader h;
        PduView pdu;
        EXPECT(parse_response(resp, rl, h, pdu));
        ExceptionCode ec = kExcNone;
        EXPECT(is_exception(pdu, ec));
        EXPECT(ec == kExcIllegalDataAddr);
    }

    // ---- ④ 方向纪律：写 MEAS/CFG/STA 区一律非法（只有 CMD 区可写）----
    {
        const uint16_t regs[2] = {0x4145, 0x70A4};
        uint8_t req[64], resp[320];
        const size_t n = build_write_multiple_request(req, sizeof(req), 0x78, 1, 0x0000, regs, 2);
        const size_t rl = rig.slave.handle(req, n, resp, sizeof(resp));
        MbapHeader h;
        PduView pdu;
        EXPECT(parse_response(resp, rl, h, pdu));
        ExceptionCode ec = kExcNone;
        EXPECT(is_exception(pdu, ec));
        EXPECT(ec == kExcIllegalDataAddr);
        // CMD 区可写
        const size_t n2 = build_write_multiple_request(req, sizeof(req), 0x79, 1, 0x0100, regs, 2);
        const size_t rl2 = rig.slave.handle(req, n2, resp, sizeof(resp));
        EXPECT(parse_response(resp, rl2, h, pdu));
        ExceptionCode ec2 = kExcNone;
        EXPECT(!is_exception(pdu, ec2));
    }

    // ---- ⑤ 未知功能码 → 0x01 ----
    {
        const uint8_t req[] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x01, 0x99};
        uint8_t resp[320];
        const size_t rl = rig.slave.handle(req, sizeof(req), resp, sizeof(resp));
        MbapHeader h;
        PduView pdu;
        EXPECT(parse_response(resp, rl, h, pdu));
        ExceptionCode ec = kExcNone;
        EXPECT(is_exception(pdu, ec));
        EXPECT(ec == kExcIllegalFunction);
    }

    // ---- ⑥ 单元号不匹配：从站在总线上"沉默"（不吭声，不是回异常）----
    {
        const int dropped_before = rig.slave.bad_unit_drops();
        uint8_t req[64], resp[320];
        const size_t n = build_read_request(req, sizeof(req), 1, 9, kReadHoldingRegs, 0, 4);
        EXPECT(rig.slave.handle(req, n, resp, sizeof(resp)) == 0);
        EXPECT(rig.slave.bad_unit_drops() == dropped_before + 1);
    }

    // ---- ⑦ 帧计数自洽：一次全量 read_snapshot = 2 次事务（MEAS + CFG）----
    {
        const int before = rig.io.transactions();
        RealtimeSnapshot s3;
        EXPECT(rig.io.read_snapshot(0.0, s3));
        EXPECT(rig.io.transactions() == before + 2);
    }

    std::cerr << "        事务=" << rig.io.transactions() << " 超时=" << rig.io.timeouts()
              << " 异常=" << rig.io.exceptions() << " 读失败=" << rig.io.stale_reads() << "\n";
}

static int block_first_index(int block) {
    for (int i = 0; i < ModbusRegisterMap::kPointCount; ++i) {
        if (modbus_point_block(i) == block) return i;
    }
    return 0;
}

// =====================================================================
// T33: 寄存器映射契约（映射表 ↔ 点名真相源 ↔ 寄存器区间）
// =====================================================================
static void test_33_map_contract() {
    std::cerr << "[T33] 寄存器映射契约：30 点点名/块归属/地址区间 ...\n";

    for (MapProfile prof : {MapProfile::kStandardF32, MapProfile::kWideF64}) {
        ModbusRegisterMap m = build_modbus_map(prof);
        const bool wide = (prof == MapProfile::kWideF64);
        EXPECT(m.width == (wide ? 4 : 2));
        EXPECT(m.total_regs() == static_cast<uint16_t>(0x0300 + 6 * m.width));

        for (int i = 0; i < ModbusRegisterMap::kPointCount; ++i) {
            // 点名逐字来自真相源
            EXPECT(std::strcmp(m.pts[i].point, EMS_POINT_NAMES[i]) == 0);
            // 块归属 = 点名前缀决定的块
            EXPECT(m.pts[i].block == modbus_point_block(i));
            // 只有 CMD 区可写
            EXPECT(m.pts[i].writable == (i >= EMS_CMD_P_BAT && i <= EMS_CMD_P_LOWER));
            // 类型随 profile
            EXPECT(m.pts[i].type == (wide ? RegType::kFloat64BE : RegType::kFloat32BE));
            // 块内紧密排布、基址对齐
            const int b = m.pts[i].block;
            const uint16_t expect_addr = static_cast<uint16_t>(
                ModbusRegisterMap::kBlockBase[b] + (i - block_first_index(b)) * m.width);
            EXPECT(m.pts[i].addr == expect_addr);
        }

        // 地址无重叠（self_check 内含重叠检测）——用适配器跑一遍才算"真的过"
        ModbusSlaveSim slave(prof);
        LoopbackModbusTransport tr(&slave);
        ModbusDeviceIO io(&tr, prof, 1);
        EXPECT(io.self_check() == 0);

        // 点名 → 映射项 反查一致
        for (int i = 0; i < ModbusRegisterMap::kPointCount; ++i) {
            const RegPoint* p = m.find_point(EMS_POINT_NAMES[i]);
            EXPECT(p != nullptr);
            if (p != nullptr) EXPECT(p->addr == m.pts[i].addr);
            EXPECT(m.index_of_point(EMS_POINT_NAMES[i]) == i);
        }
        EXPECT(m.find_point("NO.SUCH.POINT") == nullptr);
    }

    // ---- 设备侧替身（MemoryDeviceIO）与真相源**逐字同方言**（跨介质可互换）----
    MemoryDeviceIO dev;
    std::size_t alias = 0;
    for (int i = 0; i < EMS_POINT_COUNT; ++i) {
        if (dev.has_point(EMS_POINT_NAMES[i])) ++alias;
    }
    EXPECT(alias == static_cast<std::size_t>(EMS_POINT_COUNT));
    EXPECT(dev.point_count() == static_cast<std::size_t>(EMS_POINT_COUNT));

    // ---- 块划分自洽 ----
    ModbusRegisterMap ms = build_modbus_map(MapProfile::kStandardF32);
    EXPECT(ms.block_qty(0) == 14 && ms.block_qty(1) == 6);
    EXPECT(ms.block_qty(2) == 28 && ms.block_qty(3) == 12);
    EXPECT(ms.total_regs() == 0x0300 + 12);

    std::cerr << "        f32 总寄存器 " << ms.total_regs() << " / f64 总寄存器 "
              << build_modbus_map(MapProfile::kWideF64).total_regs() << "\n";
}

// =====================================================================
// T34: 闭环等价 + 量化边界
// =====================================================================
static void test_34_closed_loop_equivalence() {
    std::cerr << "[T34] 闭环等价（Modbus 报文 vs 进程内点表） ...\n";

    const int N = 400;

    // ---- 参考实现 ----
    MemoryRig ref;
    ref.run(N);

    // ---- 运行 B：所有数据经 Modbus 报文，介质用 f64 宽精度（承载 double 全部有效位）----
    ModbusRig wide(MapProfile::kWideF64);
    wide.run(N);

    EXPECT(ref.rt.log().size() == static_cast<std::size_t>(N));
    EXPECT(wide.rt.log().size() == static_cast<std::size_t>(N));
    EXPECT(std::string(ref.rt.device()->name()) == "MemoryDeviceIO(PointTable)");
    EXPECT(std::string(wide.rt.device()->name()) == "ModbusDeviceIO(TCP)");

    const int diff = diff_logs(ref.rt.log(), wide.rt.log(), 0.0, true);
    EXPECT(diff == 0);
    EXPECT(wide.rt.fsm().state() == ref.rt.fsm().state());
    EXPECT(wide.rt.fsm().history().size() == ref.rt.fsm().history().size());

    // 反向守卫：不能是"两边都恒 0"的假等价
    double max_cmd_ref = 0.0, max_cmd_wide = 0.0;
    for (const auto& r : ref.rt.log())  max_cmd_ref  = std::max(max_cmd_ref,  std::fabs(r.p_cmd));
    for (const auto& r : wide.rt.log()) max_cmd_wide = std::max(max_cmd_wide, std::fabs(r.p_cmd));
    EXPECT(max_cmd_ref > 50.0);
    EXPECT(almost_equal(max_cmd_ref, max_cmd_wide));
    EXPECT(std::fabs(ref.rt.log().back().soc - 0.5) > 1e-6);   // SOC 真的动了
    EXPECT(wide.io.stale_reads() == 0);                        // 报文零失败
    EXPECT(wide.io.self_check() == 0);
    EXPECT(wide.slave.bad_unit_drops() == 0);                  // 单元号从未错
    EXPECT(wide.slave.exception_count() == 0);                 // 从未收到异常响应

    // ---- 运行 C：同一套算法换成**现场标准 float32 映射** → 量化边界 ----
    ModbusRig std32(MapProfile::kStandardF32);
    std32.run(N);

    // ① 决策拓扑必须完全一致：量化不该改变状态/门控/故障判定/reason
    int topo_diff = 0;
    double max_abs_cmd_diff = 0.0;
    double max_rel_cmd_diff = 0.0;
    const std::size_t n = std::min(ref.rt.log().size(), std32.rt.log().size());
    for (std::size_t i = 0; i < n; ++i) {
        const StepRecord& a = ref.rt.log()[i];
        const StepRecord& c = std32.rt.log()[i];
        if (a.state != c.state || a.state_gated != c.state_gated ||
            a.safety_clip != c.safety_clip || a.hold_last != c.hold_last ||
            a.fault_bits != c.fault_bits || a.clamped != c.clamped ||
            a.reason != c.reason) {
            ++topo_diff;
        }
        const double d = std::fabs(a.p_cmd - c.p_cmd);
        max_abs_cmd_diff = std::max(max_abs_cmd_diff, d);
        const double denom = std::max(1.0, std::fabs(a.p_cmd));
        max_rel_cmd_diff = std::max(max_rel_cmd_diff, d / denom);
    }
    EXPECT(topo_diff == 0);
    EXPECT(std32.rt.fsm().state() == ref.rt.fsm().state());
    EXPECT(std32.io.stale_reads() == 0);
    EXPECT(std32.slave.exception_count() == 0);

    // ② 偏差必须在 float32 分辨率该有的量级内（不是"差不多就行"，
    //    而是"量化误差在一阶控制回路里没有被放大"）
    EXPECT(max_abs_cmd_diff < 0.5);       // 指令级偏差 < 0.5 kW（PCS 200 kW 的 0.25%）
    EXPECT(max_rel_cmd_diff < 5e-4);

    // ③ 累计电量差异同样受限（SOC 是最容易积累误差的量）
    EXPECT(std::fabs(ref.rt.log().back().soc - std32.rt.log().back().soc) < 1e-4);

    std::cerr << "        逐位差异 " << diff << " 拍 / 峰值指令 " << max_cmd_ref
              << " kW / SOC " << ref.rt.log().front().soc << " → "
              << ref.rt.log().back().soc << "\n";
    std::cerr << "        f32 量化边界: 决策拓扑差异 " << topo_diff
              << " 拍, |Δcmd|max=" << max_abs_cmd_diff
              << " kW, 相对 " << max_rel_cmd_diff
              << ", |ΔSOC|=" << std::fabs(ref.rt.log().back().soc - std32.rt.log().back().soc)
              << "\n";
}

// =====================================================================
// T35: 断链与自愈（两种故障模式必须区分开）
//
//   模式 A 链路静默（超时/异常）：数据不可信 → data_valid=false → 状态机 FAULT
//   模式 B 链路在、心跳丢（STA.METER_COMM_OK=0）：→ 采集层超时 → HOLD_LAST
// 这两种在现场表现完全不同（一个是网线，一个是设备侧不喂狗），
// 混成一条路径会让排障时无从下手，因此本用例把两者分别钉住。
// =====================================================================
static void test_35_link_loss_and_recovery() {
    std::cerr << "[T35] 断链与自愈：链路静默 → 不可信；心跳丢 → HOLD_LAST ...\n";

    ModbusRig rig(MapProfile::kWideF64);
    rig.run(60);
    EXPECT(rig.io.stale_reads() == 0);
    EXPECT(rig.io.read_status().data_valid == true);

    // ---- 模式 A：从站静默（网线掉了）----
    const double p_bat_before = rig.io.cached_value(EMS_P_BAT);
    const int stale_before = rig.io.stale_reads();
    rig.transport.inject_timeout(3);
    RealtimeSnapshot s;
    EXPECT(rig.io.read_snapshot(0.0, s) == false);
    EXPECT(rig.io.timeouts() > 0);
    EXPECT(rig.io.stale_reads() > stale_before);
    EXPECT(rig.io.read_status().data_valid == false);
    EXPECT_NEAR(rig.io.cached_value(EMS_P_BAT), p_bat_before, 1e-9);  // 旧值被保留

    // 链路持续断开 → 状态机必须把它归结成 FAULT（数据不可信时不能继续动作）。
    // 注：这里用 set_link_up(false) 而不是再 injection —— 后者是"丢一帧"，
    // 只覆盖单次采集失败；持续断链才是现场"网线掉了"的真实形态。
    rig.transport.set_link_up(false);
    rig.run(5, 60);
    EXPECT((rig.rt.log().back().fault_bits & (1 << 4)) != 0);          // bit4 = data_invalid
    EXPECT(rig.rt.fsm().state() == EmsState::kFault);
    EXPECT(std::fabs(rig.rt.log().back().p_cmd) < 1e-9);

    // ---- 自愈：链路恢复后必须能重新读到、可信性恢复 ----
    rig.transport.set_link_up(true);
    RealtimeSnapshot s2;
    EXPECT(rig.io.read_snapshot(0.0, s2));
    EXPECT(rig.io.read_status().data_valid == true);
    const int t_after = rig.io.transactions();
    rig.run(30, 65);
    EXPECT(rig.io.transactions() > t_after);       // 确实在继续收发
    EXPECT(rig.io.exceptions() == 0);

    // ---- 模式 B：链路在、心跳丢（设备侧不喂狗）→ HOLD_LAST ----
    ModbusRig rig2(MapProfile::kWideF64);
    rig2.run(80);
    EXPECT(rig2.rt.fsm().state() == EmsState::kNormal);
    EXPECT(rig2.rt.log().back().hold_last == false);

    rig2.dev.set_comm_meter(false);        // 设备侧：电表心跳丢失
    rig2.slave.publish_device_points();    // 但链路还在 —— 报文能正常收回来
    rig2.run(80, 80);

    // 首次进入 HOLD_LAST 的位置：comm_stale_threshold_s = 5.0 s / 0.1 s ≈ 50 拍
    const std::vector<StepRecord>& lg = rig2.rt.log();
    int first_hold = -1;
    for (std::size_t i = 0; i < lg.size(); ++i) {
        if (lg[i].hold_last) { first_hold = static_cast<int>(i); break; }
    }
    EXPECT(first_hold > 0);
    EXPECT(first_hold >= 128 && first_hold <= 133);   // 80 + ~50 拍
    EXPECT(first_hold > 0 && lg[first_hold - 1].hold_last == false);

    // 核心不变量：一旦进入 HOLD_LAST，指令必须**冻住**（既不归零也不继续跟随）
    double frozen = (first_hold > 0) ? lg[static_cast<std::size_t>(first_hold)].p_cmd : 0.0;
    int hold_cnt = 0, frozen_violations = 0, ungated_after_hold = 0;
    for (std::size_t i = (first_hold > 0 ? static_cast<std::size_t>(first_hold) : 0);
         i < lg.size(); ++i) {
        if (!lg[i].hold_last) continue;
        ++hold_cnt;
        if (std::fabs(lg[i].p_cmd - frozen) > 1e-12) ++frozen_violations;
        if (std::fabs(lg[i].p_lower - lg[i].p_cmd) > 1e-12 ||
            std::fabs(lg[i].p_upper - lg[i].p_cmd) > 1e-12) ++ungated_after_hold;
    }
    EXPECT(hold_cnt >= 25);
    EXPECT(frozen_violations == 0);
    EXPECT(ungated_after_hold == 0);     // 权限区间被钉成单点，硬不变量仍成立
    EXPECT(std::fabs(frozen) > 1.0);     // 冻的是"上一拍真实出力"，不是 0

    const StepRecord& h = lg.back();
    EXPECT(h.hold_last == true);
    EXPECT(h.reason == "hold_last");
    EXPECT((h.fault_bits & (1 << 2)) != 0);   // bit2 = meter_comm_lost
    EXPECT_NEAR(h.p_cmd, frozen, 1e-12);
    EXPECT(rig2.io.stale_reads() == 0);       // 链路本身没失败 —— 与模式 A 的区别
    EXPECT(rig2.rt.fsm().state() != EmsState::kFault);

    std::cerr << "        模式A: data_valid=false → FAULT / 模式B: meter_comm_lost → "
              << "第 " << first_hold << " 拍起 HOLD_LAST 冻结在 " << frozen << " kW\n";
}

// =====================================================================
// T36: 故障经寄存器驱动状态机（设备 → EMS 方向的真闭环）
// =====================================================================
static void test_36_fault_over_registers() {
    std::cerr << "[T36] 故障经 Modbus 寄存器驱动状态机 ...\n";

    ModbusRig rig(MapProfile::kWideF64);

    // ---- 第一阶段：正常运行 ----
    rig.run(80);
    EXPECT(rig.rt.fsm().state() == EmsState::kNormal);
    EXPECT(rig.rt.log().back().fault_bits == 0);
    EXPECT(std::fabs(rig.rt.log().back().p_cmd) > 1.0);      // 确实在出力

    // ---- 第二阶段：设备侧报 PCS 故障（写进寄存器）----
    rig.dev.set_pcs_fault(true);
    rig.slave.publish_device_points();
    rig.run(20, 80);
    const StepRecord& fr = rig.rt.log().back();
    EXPECT((fr.fault_bits & (1 << 3)) != 0);                 // bit3 = pcs_fault
    EXPECT(rig.rt.fsm().state() == EmsState::kFault);
    EXPECT(std::fabs(fr.p_cmd) < 1e-9);                      // FAULT → 门控归零
    EXPECT(fr.state_gated);
    EXPECT(rig.rt.fsm().output_enabled() == false);           // 运行许可已撤销

    // ---- 第三阶段：故障消失 → 不自动带载（必须由上层显式重启）----
    rig.dev.set_pcs_fault(false);
    rig.slave.publish_device_points();
    rig.run(40, 100);
    EXPECT(rig.rt.log().back().fault_bits == 0);
    EXPECT(rig.rt.fsm().state() != EmsState::kFault);
    EXPECT(rig.rt.fsm().output_enabled() == false);
    EXPECT(std::fabs(rig.rt.log().back().p_cmd) < 1e-9);
    EXPECT(rig.io.stale_reads() == 0);
    EXPECT(rig.io.exceptions() == 0);

    // ---- 设备侧能否正确读到主站下行的指令（Cmd 区方向）----
    rig.rt.fsm().request_run(true);
    rig.run(30, 140);
    rig.slave.apply_command_regs();
    const double dev_cmd = rig.dev.get(EMS_POINT_NAMES[EMS_CMD_P_BAT]);
    EXPECT(std::fabs(dev_cmd - rig.rt.last_command().p_bat_cmd_kw) < 1e-9);

    std::cerr << "        卡关点=" << rig.rt.log().back().reason
              << " / 设备侧读回指令=" << dev_cmd << " kW\n";
}

// =====================================================================
int main() {
    std::cerr << "=========================================\n"
              << " P3/ Modbus 通信适配器 单元测试\n"
              << "=========================================\n";

    test_31_frames_byte_exact();
    test_33_map_contract();      // 契约先行：后面两个用例依赖映射表正确
    test_32_exception_and_stale();
    test_34_closed_loop_equivalence();
    test_35_link_loss_and_recovery();
    test_36_fault_over_registers();

    std::cerr << "=========================================\n"
              << " PASS=" << g_pass << "  FAIL=" << g_fail << "\n"
              << "=========================================\n";
    return (g_fail == 0) ? 0 : 1;
}
