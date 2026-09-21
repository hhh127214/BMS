// =====================================================================
// 13/ 单元测试 —— Modbus TCP 协议层与映射表
//
//   T01: 请求构造（字节级）
//   T02: 请求参数越界必须**返回 0**而不是静默截断
//   T03: 读响应的长度/ByteCount 校验
//   T04: 位读取的打包顺序（低位在前）
//   T05: 异常响应的识别（FC 最高位）
//   T06: 32 位浮点字序（高字在前 / 低字在前）—— 现场第一大坑
//   T07: 与 fake 从站的读回环（FC04 / FC02）
//   T08: 与 fake 从站的写回环（FC16 / FC06 / FC05）
//   T09: **半包**注入：响应被拆成两次 send
//   T10: **串包**注入：先来一个事务号不同的响应
//   T11: ByteCount 被填错
//   T12: 从站异常响应（非法功能 / 非法地址）
//   T13: 超时（从站静默）
//   T14: 映射表自检 + 分块规划
//   T15: 编解码往返（缩放 / 负数 / NaN / 越界）
//
// 编译：见 13/scripts/build_test.bat
// =====================================================================

#include "modbus_tcp_client.h"
#include "modbus_point_map.h"
#include "fake_modbus_slave.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
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
// T01 请求构造（字节级）
//
// 为什么连"几个字节"都要断言：协议层的错**不会**表现成"值不对"，
// 而是表现成"从站不回"或"回了异常码"。等到那时再来查是哪一位写错了，
// 成本比现在多几个断言高两个数量级。
// =====================================================================
static void test_01_build_requests() {
    std::printf("T01 请求构造（字节级）\n");
    std::uint8_t pdu[32];

    // FC03 读保持寄存器：地址 0x0006，数量 0x0003
    std::size_t n = pdu::build_read(fc::kReadHoldingRegs, 0x0006, 0x0003, pdu);
    EXPECT_EQ(n, 5);
    EXPECT_EQ(pdu[0], 0x03);
    EXPECT_EQ(pdu[1], 0x00); EXPECT_EQ(pdu[2], 0x06);
    EXPECT_EQ(pdu[3], 0x00); EXPECT_EQ(pdu[4], 0x03);

    // FC04 读输入寄存器：地址 0，数量 40（本项目的输入寄存器分块）
    n = pdu::build_read(fc::kReadInputRegs, 0x0000, 40, pdu);
    EXPECT_EQ(n, 5);
    EXPECT_EQ(pdu[0], 0x04);
    EXPECT_EQ(pdu[3], 0x00); EXPECT_EQ(pdu[4], 40);

    // FC16 写多个寄存器：地址 0x0010，3 个值
    const std::uint16_t vals[3] = {0x1234, 0x5678, 0x9ABC};
    n = pdu::build_write_multiple_regs(0x0010, vals, 3, pdu);
    EXPECT_EQ(n, 12);
    EXPECT_EQ(pdu[0], 0x10);
    EXPECT_EQ(pdu[1], 0x00); EXPECT_EQ(pdu[2], 0x10);
    EXPECT_EQ(pdu[3], 0x00); EXPECT_EQ(pdu[4], 3);
    EXPECT_EQ(pdu[5], 6);                       // ByteCount = qty*2
    EXPECT_EQ(pdu[6], 0x12); EXPECT_EQ(pdu[7], 0x34);
    EXPECT_EQ(pdu[8], 0x56); EXPECT_EQ(pdu[9], 0x78);
    EXPECT_EQ(pdu[10], 0x9A); EXPECT_EQ(pdu[11], 0xBC);

    // FC05 写单个线圈：**开 = 0xFF00**，不是 0x0001
    n = pdu::build_write_single(fc::kWriteSingleCoil, 0x0003, 0xFF00, pdu);
    EXPECT_EQ(n, 5);
    EXPECT_EQ(pdu[0], 0x05);
    EXPECT_EQ(pdu[3], 0xFF); EXPECT_EQ(pdu[4], 0x00);
}

// =====================================================================
// T02 越界参数必须显式拒绝
// =====================================================================
static void test_02_request_bounds() {
    std::printf("T02 请求参数越界\n");
    std::uint8_t pdu[32];

    // 寄存器读上限 125
    EXPECT_EQ(pdu::build_read(fc::kReadHoldingRegs, 0, 126, pdu), 0);
    EXPECT(pdu::build_read(fc::kReadHoldingRegs, 0, 125, pdu) == 5);   // 边界内
    // 位读上限 2000
    EXPECT_EQ(pdu::build_read(fc::kReadDiscreteInputs, 0, 2001, pdu), 0);
    EXPECT(pdu::build_read(fc::kReadDiscreteInputs, 0, 2000, pdu) == 5);
    // 数量 0 无意义
    EXPECT_EQ(pdu::build_read(fc::kReadInputRegs, 0, 0, pdu), 0);
    // 写多 上限 123
    std::uint16_t big[124] = {0};
    EXPECT_EQ(pdu::build_write_multiple_regs(0, big, 124, pdu), 0);
    EXPECT_EQ(pdu::build_write_multiple_regs(0, big, 0, pdu), 0);
}

// =====================================================================
// T03 读响应校验
// =====================================================================
static void test_03_parse_read_response() {
    std::printf("T03 读响应校验\n");
    std::uint16_t out[4];

    // 正常：FC=03，ByteCount=04，两个寄存器
    const std::uint8_t good[] = {0x03, 0x04, 0x12, 0x34, 0x56, 0x78};
    EXPECT(pdu::parse_read_regs(good, 6, 2, out));
    EXPECT_EQ(out[0], 0x1234);
    EXPECT_EQ(out[1], 0x5678);

    // ★ ByteCount 与请求数量不符：**必须拒绝**。
    //   "字节数对但计数不对"是从站固件 bug 的典型表现；凑合读 N 个值
    //   会让后面所有点的地址整体错位（很像"点名对得上、值全是隔壁点的"）。
    const std::uint8_t bad_bc[] = {0x03, 0x02, 0x12, 0x34, 0x56, 0x78};
    EXPECT(!pdu::parse_read_regs(bad_bc, 6, 2, out));

    // 总长度不符（半包被截断）
    EXPECT(!pdu::parse_read_regs(good, 5, 2, out));
    EXPECT(!pdu::parse_read_regs(good, 7, 2, out));

    // 数量 0
    EXPECT(!pdu::parse_read_regs(good, 2, 0, out));

    // 写响应回显校验
    const std::uint8_t wresp[] = {0x10, 0x00, 0x00, 0x00, 0x03};
    EXPECT(pdu::parse_write_response(wresp, 5, 0, 3));
    EXPECT(!pdu::parse_write_response(wresp, 5, 0, 2));    // 数量对不上
    EXPECT(!pdu::parse_write_response(wresp, 5, 1, 3));    // 地址对不上
    EXPECT(!pdu::parse_write_response(wresp, 4, 0, 3));    // 长度不对
}

// =====================================================================
// T04 位打包顺序（低位在前）
// =====================================================================
static void test_04_parse_bits() {
    std::printf("T04 位打包顺序\n");
    bool out[9] = {false};

    // 0x05 → bit0=1, bit1=0, bit2=1
    const std::uint8_t resp[] = {0x02, 0x01, 0b00000101};
    EXPECT(pdu::parse_read_bits(resp, 3, 3, out));
    EXPECT(out[0]);
    EXPECT(!out[1]);
    EXPECT(out[2]);

    // 9 位跨两个字节：ByteCount = 2
    const std::uint8_t resp9[] = {0x01, 0x02, 0x81, 0x01};
    EXPECT(pdu::parse_read_bits(resp9, 4, 9, out));
    EXPECT(out[0]);      // 第 0 位
    EXPECT(out[7]);      // 第 7 位（第一个字节的最高位）
    EXPECT(out[8]);      // 第 8 位（第二个字节的 bit0）
    EXPECT(!out[1]);

    // ByteCount 不符必须拒绝
    const std::uint8_t bad[] = {0x01, 0x01, 0x01};
    EXPECT(!pdu::parse_read_bits(bad, 3, 9, out));
}

// =====================================================================
// T05 异常响应识别
// =====================================================================
static void test_05_exception_header() {
    std::printf("T05 异常响应识别\n");

    const std::uint8_t exc[] = {0x83, 0x02};
    const pdu::ResponseHeader h = pdu::parse_header(exc, 2);
    EXPECT(h.valid);
    EXPECT(h.is_exception);
    EXPECT_EQ(h.function, 0x03);          // 基功能码是原功能码 & 0x7F
    EXPECT_EQ(h.exception_code, 0x02);

    const std::uint8_t norm[] = {0x03, 0x04, 0, 0, 0, 0};
    const pdu::ResponseHeader h2 = pdu::parse_header(norm, 6);
    EXPECT(h2.valid);
    EXPECT(!h2.is_exception);
    EXPECT_EQ(h2.function, 0x03);

    // 异常响应的载荷只有 1 字节 → 长度 1 时不能算 valid
    const pdu::ResponseHeader h3 = pdu::parse_header(exc, 1);
    EXPECT(!h3.valid);
}

// =====================================================================
// T06 32 位浮点字序 —— 现场第一大坑
// =====================================================================
static void test_06_word_order() {
    std::printf("T06 32 位浮点字序\n");
    std::uint16_t regs[2] = {0, 0};

    // 100.0f = 0x42C80000 → 高字 0x42C8，低字 0x0000
    put_f32(regs, 100.0f, WordOrder::kHighWordFirst);
    EXPECT_EQ(regs[0], 0x42C8);
    EXPECT_EQ(regs[1], 0x0000);

    put_f32(regs, 100.0f, WordOrder::kLowWordFirst);
    EXPECT_EQ(regs[0], 0x0000);
    EXPECT_EQ(regs[1], 0x42C8);

    // 各按自己的字序解回来都是 100
    EXPECT_NEAR(get_f32(regs, WordOrder::kLowWordFirst), 100.0, 1e-6);
    put_f32(regs, 100.0f, WordOrder::kHighWordFirst);
    EXPECT_NEAR(get_f32(regs, WordOrder::kHighWordFirst), 100.0, 1e-6);

    // ★ 反向守卫：**用错字序就解不出 100**。
    //   没有这一条，"字序写死成一种"的实现照样能过全部正向断言。
    EXPECT(std::fabs(get_f32(regs, WordOrder::kLowWordFirst) - 100.0) > 1.0);

    // 负值与小数同样往返一致
    for (double v : {-273.5, -0.125, 12345.75}) {
        put_f32(regs, (float)v, WordOrder::kHighWordFirst);
        EXPECT_NEAR(get_f32(regs, WordOrder::kHighWordFirst), v, 1e-3);
    }
}

// =====================================================================
// 公共：起一个从站 + 连上客户端
// =====================================================================
namespace {

struct Fixture {
    FakeModbusSlave slave;
    ModbusTcpClient client;
    bool started = false;

    bool up(const FakeModbusSlave::Options& opt = FakeModbusSlave::Options{},
            int timeout_ms = 1000) {
        started = slave.start(opt);
        if (!started) return false;
        if (!client.connect("127.0.0.1", slave.port(), timeout_ms, opt.unit_id)) return false;
        return slave.wait_for_client(2000);
    }
};

} // namespace

// =====================================================================
// T07 读回环（FC04 / FC02）
// =====================================================================
static void test_07_read_roundtrip() {
    std::printf("T07 读回环（FC04/FC02）\n");
    Fixture f;
    if (!f.up()) { EXPECT(false); return; }

    f.slave.set_ir(0, 0x1234);
    f.slave.set_ir(1, 0x5678);
    f.slave.set_ir(7, 0xABCD);

    std::uint16_t regs[8] = {0};
    const ModbusStatus st = f.client.read_input(0, 8, regs);
    EXPECT(st.ok());
    EXPECT_EQ(regs[0], 0x1234);
    EXPECT_EQ(regs[1], 0x5678);
    EXPECT_EQ(regs[7], 0xABCD);
    EXPECT_EQ(f.client.requests(), 1);
    EXPECT_EQ(f.slave.requests(), 1);
    EXPECT_EQ(f.slave.last_fc(), fc::kReadInputRegs);

    // 离散输入
    f.slave.set_di(0, true);
    f.slave.set_di(5, true);
    bool bits[8] = {false};
    EXPECT(f.client.read_discrete(0, 8, bits).ok());
    EXPECT(bits[0]);
    EXPECT(!bits[1]);
    EXPECT(bits[5]);
    EXPECT(!bits[7]);

    // 保持寄存器
    f.slave.set_hr(3, 0x00FF);
    std::uint16_t hr[4] = {0};
    EXPECT(f.client.read_holding(0, 4, hr).ok());
    EXPECT_EQ(hr[3], 0x00FF);
}

// =====================================================================
// T08 写回环（FC16 / FC06 / FC05）
// =====================================================================
static void test_08_write_roundtrip() {
    std::printf("T08 写回环（FC16/FC06/FC05）\n");
    Fixture f;
    if (!f.up()) { EXPECT(false); return; }

    // FC16 写多个
    const std::uint16_t vals[3] = {0x42C8, 0x0000, 0x1234};
    EXPECT(f.client.write_multiple_regs(0, vals, 3).ok());
    EXPECT_EQ(f.slave.hr(0), 0x42C8);
    EXPECT_EQ(f.slave.hr(1), 0x0000);
    EXPECT_EQ(f.slave.hr(2), 0x1234);
    EXPECT_EQ(f.slave.write_requests(), 1);
    EXPECT_EQ(f.client.writes(), 1);

    // FC06 写单个
    EXPECT(f.client.write_single_reg(10, 0xBEEF).ok());
    EXPECT_EQ(f.slave.hr(10), 0xBEEF);

    // FC05 写线圈：开 / 关
    EXPECT(f.client.write_coil(3, true).ok());
    EXPECT(f.slave.co(3));
    EXPECT(f.client.write_coil(3, false).ok());
    EXPECT(!f.slave.co(3));

    // 越界写：从站回非法地址 0x02，客户端必须把它**分类成 kException**
    // 而不是通信故障 —— 两者在业务层的处置完全不同（重试 vs 报配置错）
    const ModbusStatus st = f.client.write_multiple_regs(62, vals, 3);   // 62+3 > 64
    EXPECT(!st.ok());
    EXPECT(st.error == ModbusError::kException);
    EXPECT_EQ(st.exception_code, 0x02);
    EXPECT(!st.is_transport());

    // 边界内（62+2 == 64）必须成功 —— 反向守卫：
    // 证明上面那三条红的不是"写操作本来就永远失败"
    const std::uint16_t two[2] = {0x00AA, 0x00BB};
    EXPECT(f.client.write_multiple_regs(62, two, 2).ok());
    EXPECT_EQ(f.slave.hr(63), 0x00BB);
}

// =====================================================================
// T09 半包注入
//
// TCP 是**字节流**：一次 recv 可能只拿到半个 MBAP。跨运营商/VPN/网关
// 转发时这非常常见，而且表现为"偶发解析错误" —— 最难查的一类现场问题。
// =====================================================================
static void test_09_split_response() {
    std::printf("T09 半包注入\n");
    FakeModbusSlave::Options opt;
    opt.split_response_at = 5;    // 拆在 MBAP 中间

    Fixture f;
    if (!f.up(opt)) { EXPECT(false); return; }

    f.slave.set_ir(0, 0x1111);
    f.slave.set_ir(1, 0x2222);
    f.slave.set_ir(2, 0x3333);
    f.slave.set_ir(3, 0x4444);

    std::uint16_t regs[4] = {0};
    const ModbusStatus st = f.client.read_input(0, 4, regs);
    EXPECT(st.ok());
    EXPECT_EQ(regs[0], 0x1111);
    EXPECT_EQ(regs[3], 0x4444);

    // 换个拆分点（拆在 PDU 中间）再跑一次
    f.client.close();
    FakeModbusSlave slave2;
    FakeModbusSlave::Options opt2;
    opt2.split_response_at = 11;
    EXPECT(slave2.start(opt2));
    ModbusTcpClient c2;
    EXPECT(c2.connect("127.0.0.1", slave2.port(), 1000, 1));
    slave2.set_ir(0, 0xAAAA);
    std::uint16_t one[1] = {0};
    EXPECT(c2.read_input(0, 1, one).ok());
    EXPECT_EQ(one[0], 0xAAAA);
}

// =====================================================================
// T10 串包注入
//
// 上一次请求超时后，它的响应可能在几毫秒后才到。于是本次请求读到的第一个
// 包其实是上一次的。若此时直接判失败，一次抖动会连锁成两拍数据缺失。
// 正确做法：按事务号认包，不是我的就丢掉继续等。
// =====================================================================
static void test_10_extra_response() {
    std::printf("T10 串包注入\n");
    FakeModbusSlave::Options opt;
    opt.extra_response_first = true;

    Fixture f;
    if (!f.up(opt)) { EXPECT(false); return; }

    f.slave.set_ir(0, 0x0BEE);
    std::uint16_t one[1] = {0};
    const ModbusStatus st = f.client.read_input(0, 1, one);
    EXPECT(st.ok());                       // ★ 自愈成功，而不是失败
    EXPECT_EQ(one[0], 0x0BEE);
    EXPECT(f.client.stale_responses() >= 1);   // 反向守卫：确实丢过包
    EXPECT_EQ(f.client.malformed(), 0);        // 丢包**不算**报文格式错误
}

// =====================================================================
// T11 ByteCount 被填错
// =====================================================================
static void test_11_corrupt_byte_count() {
    std::printf("T11 ByteCount 填错\n");
    FakeModbusSlave::Options opt;
    opt.corrupt_byte_count = 2;    // 读 2 个寄存器时真值应是 4

    Fixture f;
    if (!f.up(opt)) { EXPECT(false); return; }

    f.slave.set_ir(0, 0x1234);
    f.slave.set_ir(1, 0x5678);
    std::uint16_t regs[2] = {0};
    const ModbusStatus st = f.client.read_input(0, 2, regs);
    EXPECT(!st.ok());
    EXPECT(st.error == ModbusError::kMalformed);
    EXPECT(f.client.malformed() >= 1);
}

// =====================================================================
// T12 从站异常响应
// =====================================================================
static void test_12_exceptions() {
    std::printf("T12 从站异常响应\n");
    {
        FakeModbusSlave::Options opt;
        opt.reject_fc = fc::kReadInputRegs;
        Fixture f;
        if (!f.up(opt)) { EXPECT(false); return; }
        std::uint16_t regs[2] = {0};
        const ModbusStatus st = f.client.read_input(0, 2, regs);
        EXPECT(!st.ok());
        EXPECT(st.error == ModbusError::kException);
        EXPECT_EQ(st.exception_code, 0x01);      // 非法功能
        EXPECT(f.client.exceptions() >= 1);
        EXPECT(f.slave.exceptions_sent() >= 1);
    }
    {
        FakeModbusSlave::Options opt;
        opt.reject_addr = 10;
        opt.reject_qty  = 2;
        Fixture f;
        if (!f.up(opt)) { EXPECT(false); return; }
        std::uint16_t regs[4] = {0};
        const ModbusStatus st = f.client.read_input(10, 2, regs);
        EXPECT(st.error == ModbusError::kException);
        EXPECT_EQ(st.exception_code, 0x02);      // 非法地址
        // 区间不相交的请求照常成功
        EXPECT(f.client.read_input(0, 2, regs).ok());
    }
}

// =====================================================================
// T13 超时（从站静默）
// =====================================================================
static void test_13_timeout() {
    std::printf("T13 超时\n");
    FakeModbusSlave slave;
    FakeModbusSlave::Options opt;
    opt.silent = true;
    EXPECT(slave.start(opt));

    ModbusTcpClient c;
    EXPECT(c.connect("127.0.0.1", slave.port(), 300, 1));   // 300 ms 超时
    std::uint16_t regs[2] = {0};
    const ModbusStatus st = c.read_input(0, 2, regs);
    EXPECT(!st.ok());
    EXPECT(st.error == ModbusError::kTimeout);
    EXPECT(st.is_transport());
    EXPECT(c.timeouts() >= 1);
    // 超时后连接被关闭（下一次调用要先重连）——
    // 留着半死的 socket 会让后续每个请求都白等一个超时
    EXPECT(!c.is_connected());
}

// =====================================================================
// T14 映射表自检 + 分块规划
// =====================================================================
static void test_14_point_map() {
    std::printf("T14 映射表自检\n");

    // 与 ems_point_table.h 的点名逐字一致
    EXPECT_EQ(self_check(), 0);
    EXPECT_EQ(mapped_point_count(), (std::size_t)kBindingCount);
    // ★ A3.1 起点表是 40 点：前 32 点是设备/EMS 侧（Modbus 绑定它们），
    //   后 8 点是 EXT 外部设定（调度侧概念，**不经设备总线**）。
    //   这里同时钉住两个数 —— 只钉一个的话，"EXT 区被误当设备点"或
    //   "EXT 区被误当已绑定"都不会红。
    EXPECT_EQ(EMS_EXT_BEGIN, 32);      // 设备侧点数（= Modbus 绑定数）
    EXPECT_EQ(EMS_POINT_COUNT, 40);    // 点表总点数
    EXPECT_EQ(EMS_EXT_END, EMS_POINT_COUNT);

    // ★ 分块规划：读全 32 点只需 3 次请求。
    //   这个数字是**契约**：谁把实现改回"一点一次请求"，它变成 32。
    EXPECT_EQ(full_scan_request_count(), 3);
    EXPECT_EQ(kReadBlocks[0].count, 40);   // 输入寄存器 0..39
    EXPECT_EQ(kReadBlocks[1].count, 6);    // 保持寄存器 0..5（指令）
    EXPECT_EQ(kReadBlocks[2].count, 8);    // 离散输入 0..7（状态位）

    // 点名查询
    EXPECT(binding_for_name("MEAS.SOC") != nullptr);
    EXPECT(binding_for_name("STA.BMS_DIS_FORBID") != nullptr);
    EXPECT(binding_for_name("STA.BMS_DIS_FORBID")->table == Table::kDiscreteInput);
    EXPECT(binding_for_name("CMD.P_UPPER")->writable);
    EXPECT(binding_for_name("MEAS.P_BAT")->encoding == Encoding::kF32);
    EXPECT(!binding_for_name("MEAS.P_BAT")->writable);

    // ★ 查不到必须返回 nullptr —— 不能退化成"返回默认点"（那是采了个假值）
    EXPECT(binding_for_name("NO.SUCH.POINT") == nullptr);
    EXPECT(binding_for_name(nullptr) == nullptr);

    // 三个指令点连续（原子下发的前提）
    EXPECT_EQ(kBindings[EMS_CMD_P_BAT].address, 0);
    EXPECT_EQ(kBindings[EMS_CMD_P_UPPER].address, 2);
    EXPECT_EQ(kBindings[EMS_CMD_P_LOWER].address, 4);
    EXPECT(kBindings[EMS_CMD_P_BAT].table == Table::kHoldingReg);

    // ★ 量测/配置/状态一律只读（越权写安全位是设计上不允许的）
    for (std::size_t i = 0; i < kBindingCount; ++i) {
        const bool is_cmd = (i >= (std::size_t)EMS_CMD_P_BAT &&
                             i <= (std::size_t)EMS_CMD_P_LOWER);
        EXPECT_EQ(kBindings[i].writable ? 1 : 0, is_cmd ? 1 : 0);
    }

    // MEAS.P_BAT 用低字在前（**刻意**，见映射表注释）——这里把它钉住，
    // 免得后人"统一字序"时把这条区分度抹掉而毫无察觉
    EXPECT(kBindings[EMS_P_BAT].word_order == WordOrder::kLowWordFirst);
    EXPECT(kBindings[EMS_P_GRID].word_order == WordOrder::kHighWordFirst);
}

// =====================================================================
// T15 编解码往返（缩放 / 负数 / NaN / 越界）
// =====================================================================
static void test_15_codec_roundtrip() {
    std::printf("T15 编解码往返\n");
    double v = 0.0;

    // ★ 参数语义：encode/decode 收的是**整块缓冲区**，下标 = 点的绝对地址。
    //   所以这里开的是整块（和设备侧一致），不是按点宽度切的小数组。
    std::uint16_t ir[64] = {0};
    std::uint16_t hr[64] = {0};

    // SOC：IR[8]，u16 ×10000（0..1 映射到 0..10000，精度 1e-4）
    const PointBinding& soc = kBindings[EMS_SOC];
    EXPECT_EQ(soc.address, 8);
    EXPECT(encode_point(soc, 0.5, ir, 64, nullptr, 0) == DecodeError::kNone);
    EXPECT_EQ(ir[8], 5000);
    EXPECT(decode_point(soc, ir, 64, nullptr, 0, &v) == DecodeError::kNone);
    EXPECT_NEAR(v, 0.5, 1e-9);

    // 温度：IR[9]，i16 ×10，含**负值**
    // （负温只有 i16 能表达；若误用 u16，-10.5 会变成 +6552.5）
    const PointBinding& tc = kBindings[EMS_T_C];
    EXPECT_EQ(tc.address, 9);
    EXPECT(encode_point(tc, -10.5, ir, 64, nullptr, 0) == DecodeError::kNone);
    EXPECT(decode_point(tc, ir, 64, nullptr, 0, &v) == DecodeError::kNone);
    EXPECT_NEAR(v, -10.5, 1e-9);
    EXPECT(tc.encoding == Encoding::kI16);
    // 反向守卫：同样的位模式按 u16 解会是 6552.5，差得离谱
    EXPECT(std::fabs((double)ir[9] / 10.0 - (-10.5)) > 1000.0);

    // NaN 必须被拒（写一个 NaN 进寄存器 = 给设备一个无法执行的指令）
    EXPECT(encode_point(tc, std::nan(""), ir, 64, nullptr, 0) == DecodeError::kNonFinite);

    // ★ 越界必须**报错而不是静默饱和**：饱和会把"5000 kW"变成"65535"，
    //   设备照做就出事。宁可让上层知道这条线走不通。
    EXPECT(encode_point(soc, 10.0, ir, 64, nullptr, 0) == DecodeError::kOutOfRange);
    EXPECT(encode_point(soc, -1.0, ir, 64, nullptr, 0) == DecodeError::kOutOfRange);
    EXPECT(encode_point(tc, 99999.0, ir, 64, nullptr, 0) == DecodeError::kOutOfRange);

    // f32 + 字序：MEAS.P_BAT 在 IR[4..5]，**低字在前**
    const PointBinding& pb = kBindings[EMS_P_BAT];
    EXPECT_EQ(pb.address, 4);
    EXPECT(pb.word_order == WordOrder::kLowWordFirst);
    EXPECT(encode_point(pb, -137.25, ir, 64, nullptr, 0) == DecodeError::kNone);
    EXPECT(decode_point(pb, ir, 64, nullptr, 0, &v) == DecodeError::kNone);
    EXPECT_NEAR(v, -137.25, 1e-4);
    // 反向守卫：同一段字节按**高字在前**解不出 -137.25
    EXPECT(std::fabs(get_f32(ir + 4, WordOrder::kHighWordFirst) + 137.25) > 1.0);

    // 指令点（可比写的 f32，高字在前）
    const PointBinding& hi = kBindings[EMS_CMD_P_UPPER];
    EXPECT(encode_point(hi, 200.0, hr, 64, nullptr, 0) == DecodeError::kNone);
    EXPECT(decode_point(hi, hr, 64, nullptr, 0, &v) == DecodeError::kNone);
    EXPECT_NEAR(v, 200.0, 1e-4);

    // ★ NaN 解码守卫：0x7FFFFFFF = 指数全 1、尾数非 0 的 NaN。
    //   字序配错的现场症状就是这样 —— 解出 NaN。放它进算法，
    //   05/ 的区间求交会得到 NaN 权限区间，而"数据无效"与"数据是 NaN"
    //   在故障语义上完全是两回事。
    ir[4] = 0xFFFF;
    ir[5] = 0x7FFF;
    EXPECT(decode_point(pb, ir, 64, nullptr, 0, &v) == DecodeError::kNonFinite);

    // 位类型：STA.BMS_DIS_FORBID 在 DI[7]
    const PointBinding& forbid = kBindings[EMS_STA_BMS_DIS_FORBID];
    EXPECT_EQ(forbid.address, 7);
    bool bits[8] = {false};
    EXPECT(encode_point(forbid, 1.0, nullptr, 0, bits, 8) == DecodeError::kNone);
    EXPECT(bits[7]);
    EXPECT(!bits[0] && !bits[6]);          // 不多写别的位
    EXPECT(decode_point(forbid, nullptr, 0, bits, 8, &v) == DecodeError::kNone);
    EXPECT_NEAR(v, 1.0, 1e-9);

    // 缓冲区不够必须报错（不能越界读）：cb 只覆盖到 IR[4]，
    // 而 P_BAT 是 f32 需要 IR[4..5]；soc 需要 IR[8]
    EXPECT(decode_point(pb, ir, 5, nullptr, 0, &v) == DecodeError::kNotEnoughData);
    EXPECT(decode_point(soc, ir, 8, nullptr, 0, &v) == DecodeError::kNotEnoughData);
    // 位越界同理
    EXPECT(decode_point(forbid, nullptr, 0, bits, 6, &v) == DecodeError::kNotEnoughData);
}

// =====================================================================
int main() {
    std::printf("=== 13/ Modbus TCP 协议层 + 映射表 单元测试 ===\n\n");

    test_01_build_requests();
    test_02_request_bounds();
    test_03_parse_read_response();
    test_04_parse_bits();
    test_05_exception_header();
    test_06_word_order();
    test_07_read_roundtrip();
    test_08_write_roundtrip();
    test_09_split_response();
    test_10_extra_response();
    test_11_corrupt_byte_count();
    test_12_exceptions();
    test_13_timeout();
    test_14_point_map();
    test_15_codec_roundtrip();

    std::printf("\n");
    if (g_fail == 0) {
        std::printf("ALL TESTS PASSED\n");
    }
    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
