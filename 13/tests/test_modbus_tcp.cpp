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
//   T16~T19: 运行期点表 —— 默认表分块一致 / CSV 往返 / 覆盖生效 / 非法被拒且回退
//   T20: 随包发布的点表模板（docs/point_map_template.csv）能加载且等于默认表
//   T31~T33: 约定默认路径（配置通道①）—— 无文件回退内置 / 有文件则生效 /
//            非法则硬失败且不退回默认表
//
// 编译：见 13/scripts/build_test.bat
// 运行：工作目录必须是 13\（T17~T20、T31~T33 都按这个相对路径读写文件）
// =====================================================================

#include "modbus_tcp_client.h"
#include "modbus_point_map.h"
#include "device_conn_conf.h"     // 13/  接入参数（配置通道②）
#include "fake_modbus_slave.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>

// T31~T33 需要在 build/ 下造一个临时点表目录。用 _mkdir 而不是
// <filesystem>：本模块只跑在 Windows（bat 构建 + WinSock2），
// 为一行建目录去引入 <filesystem> 不划算，老 MinGW 还要额外链 libstdc++fs。
#ifdef _WIN32
#  include <direct.h>
#  define EMS_TEST_MKDIR(p) ::_mkdir(p)
#else
#  include <sys/stat.h>
#  define EMS_TEST_MKDIR(p) ::mkdir((p), 0755)
#endif

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
// T16~T19：运行期点表（现场配置那一层）
//
// 这一组测的是"两层结构"本身，而不是协议：
//   T16 默认表推导出的分块必须与编译期 kReadBlocks **逐字段相等**
//       —— 这是"两层结构没有偷偷改变默认行为"的正面证据
//   T17 导出 → 加载 往返一致（导出件可以直接当模板发给客户）
//   T18 合法 CSV 的地址 / 缩放 / 字序真的生效（含解码结果，不只看表里的数字）
//   T19 各类非法 CSV 被拒，且活动表**一个字节都没变**（回退纪律）
// =====================================================================

static bool write_text_file(const char* path, const std::string& s) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;
    const bool ok = std::fwrite(s.data(), 1, s.size(), f) == s.size();
    std::fclose(f);
    return ok;
}

// 活动表是否**逐字段**等于编译期默认表（含分块）。
// 只看 from_csv 不够：一份"加载失败却把表重置成默认"的实现也能让 from_csv 为假。
static bool same_as_default(const PointMap& m) {
    if (m.from_csv) return false;
    if (m.count != static_cast<std::size_t>(kBindingCount)) return false;
    if (m.block_count != kReadBlockCount) return false;
    for (std::size_t i = 0; i < kBindingCount; ++i) {
        const PointBinding& a = m.items[i];
        const PointBinding& b = kBindings[i];
        if (a.index != b.index)                   return false;
        if (a.table != b.table)                   return false;
        if (a.address != b.address)               return false;
        if (a.encoding != b.encoding)             return false;
        if (a.word_order != b.word_order)         return false;
        if (a.writable != b.writable)             return false;
        if (std::fabs(a.scale - b.scale) > 1e-12) return false;
    }
    for (std::size_t k = 0; k < kReadBlockCount; ++k) {
        if (m.blocks[k].table != kReadBlocks[k].table) return false;
        if (m.blocks[k].start != kReadBlocks[k].start) return false;
        if (m.blocks[k].count != kReadBlocks[k].count) return false;
    }
    return true;
}

static std::string replace_first(const std::string& s, const std::string& from,
                                 const std::string& to) {
    const std::size_t p = s.find(from);
    if (p == std::string::npos) return s;
    return s.substr(0, p) + to + s.substr(p + from.size());
}

static std::string first_n_lines(const std::string& s, std::size_t n) {
    std::size_t pos = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) return s;
        pos = nl + 1;
    }
    return s.substr(0, pos);
}

static void test_16_default_map_blocks_match() {
    std::printf("T16 默认表推导的分块 == 编译期 kReadBlocks\n");
    reset_point_map();
    EXPECT(same_as_default(active_point_map()));
    EXPECT_EQ(self_check(), 0);
    EXPECT_EQ(mapped_point_count(), static_cast<std::size_t>(kBindingCount));
    // 分块数照旧是 3 —— "读全 32 点 = 3 次请求"这条契约没被动过
    EXPECT_EQ(full_scan_request_count(), static_cast<std::size_t>(3));
}

static void test_17_csv_roundtrip() {
    std::printf("T17 CSV 导出 → 加载 往返一致\n");
    reset_point_map();
    const std::string csv = export_point_map_csv(active_point_map());
    EXPECT(csv.size() > 100);
    const char* path = "build/_t17_roundtrip.csv";
    EXPECT(write_text_file(path, csv));

    std::string rep;
    EXPECT(load_point_map_csv(path, &rep));
    EXPECT(rep == "ok");
    EXPECT(active_point_map().from_csv);

    // 往返之后每一列都要与默认表相同 —— 否则"导出再导入"会静默改表
    for (std::size_t i = 0; i < kBindingCount; ++i) {
        const PointBinding& a = active_point_map().items[i];
        const PointBinding& b = kBindings[i];
        EXPECT(a.address == b.address);
        EXPECT(a.table == b.table);
        EXPECT(a.encoding == b.encoding);
        EXPECT(a.word_order == b.word_order);
        EXPECT_EQ(a.writable ? 1 : 0, b.writable ? 1 : 0);
        EXPECT_NEAR(a.scale, b.scale, 1e-12);
    }
    EXPECT_EQ(self_check(), 0);
    reset_point_map();
}

static void test_18_csv_overrides_take_effect() {
    std::printf("T18 合法 CSV 的地址 / 缩放 / 字序真的生效\n");
    reset_point_map();
    std::string csv = export_point_map_csv(active_point_map());
    csv = replace_first(csv, "MEAS.SOC,IR,8,u16,high,10000,ro",
                             "MEAS.SOC,IR,8,u16,high,100,ro");
    csv = replace_first(csv, "MEAS.P_PV,IR,2,f32,high",
                             "MEAS.P_PV,IR,100,f32,high");
    csv = replace_first(csv, "MEAS.P_BAT,IR,4,f32,low",
                             "MEAS.P_BAT,IR,4,f32,high");
    const char* path = "build/_t18_override.csv";
    EXPECT(write_text_file(path, csv));

    std::string rep;
    EXPECT(load_point_map_csv(path, &rep));

    EXPECT_NEAR(binding_for_index(EMS_SOC).scale, 100.0, 1e-9);
    EXPECT_EQ(binding_for_index(EMS_P_PV).address, 100);
    EXPECT(binding_for_index(EMS_P_BAT).word_order == WordOrder::kHighWordFirst);

    // P_PV 挪到 IR[100] 之后，IR 段被拆成两块 → 总块数 4。
    // 这条断言的意思是：**分块是跟着地址重算的**，不是继续用编译期那张常量表。
    EXPECT_EQ(active_block_count(), static_cast<std::size_t>(4));
    EXPECT_EQ(full_scan_request_count(), static_cast<std::size_t>(4));
    EXPECT_EQ(self_check(), 0);

    // 缩放必须体现在**解码结果**上：只看表里的 100 是测不出"生效"的
    const std::uint16_t irsoc[9] = {0, 0, 0, 0, 0, 0, 0, 0, 5500};
    double v = 0.0;
    EXPECT(decode_point(binding_for_index(EMS_SOC), irsoc, 9, nullptr, 0, &v) ==
           DecodeError::kNone);
    EXPECT_NEAR(v, 55.0, 1e-9);          // 5500 / 100，而不是 5500 / 10000
    reset_point_map();
}

static void test_19_invalid_csv_rejected_and_rollback() {
    std::printf("T19 非法 CSV 被拒，且活动表一个字节都没变\n");

    // 先加载一份**合法但改过**的表当基线。
    // 为什么不拿默认表当基线：那样"加载失败却把表重置成默认"的实现也会通过 ——
    // 基线必须与"失败后的错误状态"不同，断言才有区分度。
    reset_point_map();
    std::string good = export_point_map_csv(active_point_map());
    good = replace_first(good, "MEAS.SOC,IR,8,u16,high,10000,ro",
                               "MEAS.SOC,IR,8,u16,high,100,ro");
    const char* goodPath = "build/_t19_good.csv";
    EXPECT(write_text_file(goodPath, good));
    std::string rep;
    EXPECT(load_point_map_csv(goodPath, &rep));
    EXPECT_NEAR(binding_for_index(EMS_SOC).scale, 100.0, 1e-9);

    auto must_reject = [&](const char* label, const std::string& csv) {
        const char* path = "build/_t19_bad.csv";
        EXPECT(write_text_file(path, csv));
        std::string r;
        const bool ok = load_point_map_csv(path, &r);
        EXPECT(!ok);
        std::printf("     %-14s → 拒绝：%s\n", label, r.c_str());
        // ★ 关键断言：失败之后活动表仍是 T19 开头那份**改过的**表，
        //   而不是默认表 —— "失败即回退到上一个可用状态"。
        EXPECT(active_point_map().from_csv);
        EXPECT_NEAR(binding_for_index(EMS_SOC).scale, 100.0, 1e-9);
        EXPECT_EQ(self_check(), 0);
    };

    must_reject("行数不足",   first_n_lines(good, 20));
    must_reject("字序非法",   replace_first(good, ",f32,high,", ",f32,wiggle,"));
    must_reject("点名写错",   replace_first(good, "MEAS.P_LOAD,IR,0", "MEAS.P_LOADS,IR,0"));
    must_reject("地址重叠",   replace_first(good, "MEAS.P_PV,IR,2", "MEAS.P_PV,IR,0"));
    must_reject("缩放为0",    replace_first(good, "MEAS.SOC,IR,8,u16,high,100,ro",
                                                 "MEAS.SOC,IR,8,u16,high,0,ro"));
    must_reject("指令不连续", replace_first(good, "CMD.P_UPPER,HR,2", "CMD.P_UPPER,HR,8"));
    must_reject("只读标可写", replace_first(good, "MEAS.SOC,IR,8,u16,high,100,ro",
                                                 "MEAS.SOC,IR,8,u16,high,100,rw"));
    must_reject("编码配错表", replace_first(good, "MEAS.SOC,IR,8,u16", "MEAS.SOC,DI,8,u16"));
    must_reject("空文件",     std::string(""));

    reset_point_map();
    EXPECT(same_as_default(active_point_map()));
}

// =====================================================================
// T20：随包发布的点表模板（docs/point_map_template.csv）
//
// 为什么单独给一份**文档文件**写判据：
//   这份模板是发给客户去改的起点。它一旦与内置默认表脱钩（默认表改了
//   地址而模板没跟着改），客户照它填出来的表会在**加载成功**的前提下
//   读出错误的点 —— 这正是本项目最忌讳的那类"看起来在工作"。
//
// ★ 与 T17 的分工：T17 验"导出再导入"这条**机制**；T20 验"我们实际
//   交付出去的那个文件"本身。机制是对的、而发出的文件是旧的，这两件
//   事完全可以同时成立 —— 所以必须各有一条判据。
// =====================================================================

// 忽略 from_csv 标志：模板加载成功后它必为真，而默认表为假，
// 所以这里不能直接复用 same_as_default()。
// 不符时打印**第一处**差异的定位信息，避免只报一句"不相等"。
static bool template_matches_compiled_default(const PointMap& m) {
    if (m.count != static_cast<std::size_t>(kBindingCount)) {
        std::printf("     模板点数 %zu != 默认 %d\n",
                    m.count, static_cast<int>(kBindingCount));
        return false;
    }
    if (m.block_count != kReadBlockCount) {
        std::printf("     模板分块数 %zu != 默认 %d\n",
                    m.block_count, static_cast<int>(kReadBlockCount));
        return false;
    }
    for (std::size_t i = 0; i < kBindingCount; ++i) {
        const PointBinding& a = m.items[i];
        const PointBinding& b = kBindings[i];
        const bool same = a.index == b.index && a.table == b.table &&
                          a.address == b.address && a.encoding == b.encoding &&
                          a.word_order == b.word_order &&
                          a.writable == b.writable &&
                          std::fabs(a.scale - b.scale) <= 1e-12;
        if (!same) {
            std::printf("     模板第 %zu 行与默认表不符："
                        "addr %u/%u  enc %d/%d  word %d/%d  scale %g/%g\n",
                        i, static_cast<unsigned>(a.address),
                        static_cast<unsigned>(b.address),
                        static_cast<int>(a.encoding), static_cast<int>(b.encoding),
                        static_cast<int>(a.word_order),
                        static_cast<int>(b.word_order), a.scale, b.scale);
            return false;
        }
    }
    for (std::size_t k = 0; k < kReadBlockCount; ++k) {
        if (m.blocks[k].table != kReadBlocks[k].table ||
            m.blocks[k].start != kReadBlocks[k].start ||
            m.blocks[k].count != kReadBlocks[k].count) {
            std::printf("     模板第 %zu 个分块与默认表不符\n", k);
            return false;
        }
    }
    return true;
}

static void test_20_shipped_template_matches_default() {
    std::printf("T20 随包点表模板可加载，且逐字段等于内置默认表\n");
    reset_point_map();
    const char* path = "docs/point_map_template.csv";
    std::string rep;
    // 注意：这条 EXPECT 同时守住"文件还在不在" —— 模板被删掉/改名
    // 是 FAIL 而不是 SKIP，因为"发给客户的那个文件"本身就是交付物。
    EXPECT(load_point_map_csv(path, &rep));
    EXPECT(rep == "ok");
    EXPECT(template_matches_compiled_default(active_point_map()));
    // "读全 32 点 = 3 次请求"这条契约对模板同样成立
    EXPECT_EQ(full_scan_request_count(), static_cast<std::size_t>(3));
    EXPECT(active_point_map().from_csv);
    reset_point_map();
}

// ---------------------------------------------------------------------
// T31~T33: 约定默认路径（配置通道①：14/ 落盘 → 13/ 启动时读）
//
// 这是「客户在界面上填完点表，13/ 就能读到」的落点，所以要守住三件事：
//   ① 目录里没有文件 → 回退内置默认表。现场还没配置是正常状态，
//      程序必须能起来 —— 这是"开箱可用"的底线。
//   ② 文件合法     → 用它，而且活动表**真的被替换**（不能只是"报告成功"）
//   ③ 文件非法     → **硬失败**，且活动表一个字节都不改
// ③ 是这段的全部意义：静默回退会让「平台写错了表」与「平台没写表」
//   在现场表现完全一样，而前者会让人以为配置已生效、实际跑在另一张表上。
// ---------------------------------------------------------------------
static const char* kDefaultDir = "build/_t31_map";

static void ensure_dir(const char* path) { EMS_TEST_MKDIR(path); }

static void test_31_default_path_missing_falls_back() {
    std::printf("T31 约定路径下没有文件时回退内置默认表（不算错误）\n");
    reset_point_map();
    std::string rep, used;
    MapSource src = MapSource::kExplicit;      // 先设成别的值，验证它确实被改写
    const bool ok = load_point_map_default_at("build/_t31_no_such_dir", &rep, &src, &used);
    EXPECT(ok);                                       // ① 不是错误
    EXPECT(src == MapSource::kBuiltin);
    EXPECT(!used.empty());
    EXPECT(rep.find("没有点表文件") != std::string::npos);
    EXPECT(same_as_default(active_point_map()));      // 活动表没被碰
    EXPECT_EQ(full_scan_request_count(), static_cast<std::size_t>(3));
    reset_point_map();
}

static void test_32_default_path_loads_and_takes_effect() {
    std::printf("T32 约定路径有合法点表时被加载，且真的生效\n");
    reset_point_map();
    ensure_dir(kDefaultDir);
    // 造一份"合法但改过"的表：SOC 缩放 10000→100，P_PV 从 IR2 挪到 IR100
    std::string csv = export_point_map_csv(make_default_map());
    csv = replace_first(csv, "MEAS.SOC,IR,8,u16,high,10000,ro",
                             "MEAS.SOC,IR,8,u16,high,100,ro");
    csv = replace_first(csv, "MEAS.P_PV,IR,2,f32,high",
                             "MEAS.P_PV,IR,100,f32,high");
    const std::string path = std::string(kDefaultDir) + "/active.csv";
    EXPECT(write_text_file(path.c_str(), csv));

    std::string rep, used;
    MapSource src = MapSource::kBuiltin;
    EXPECT(load_point_map_default_at(kDefaultDir, &rep, &src, &used));
    EXPECT(src == MapSource::kDefaultPath);
    EXPECT(active_point_map().from_csv);
    EXPECT(!same_as_default(active_point_map()));     // 确实换了表，不是"报告成功"
    // 真的生效：缩放值对得上、分块数从 3 变成 4（P_PV 挪远了）
    EXPECT_NEAR(binding_for_index(EMS_SOC).scale, 100.0, 1e-9);
    EXPECT_EQ(full_scan_request_count(), static_cast<std::size_t>(4));
    reset_point_map();
}

static void test_33_default_path_invalid_is_hard_failure() {
    std::printf("T33 约定路径的表非法时硬失败，且活动表不被改动\n");
    reset_point_map();
    ensure_dir(kDefaultDir);

    // 先加载一份"合法但改过"的表当基线。为什么必须先加载一份而不是用默认表：
    // 否则"失败后活动表还是默认表"这种**假通过**就区分不出来 ——
    // 而这段代码要证明的是"失败没有把表退回默认"。
    std::string good = export_point_map_csv(make_default_map());
    good = replace_first(good, "MEAS.SOC,IR,8,u16,high,10000,ro",
                               "MEAS.SOC,IR,8,u16,high,100,ro");
    const std::string path = std::string(kDefaultDir) + "/active.csv";
    EXPECT(write_text_file(path.c_str(), good));
    std::string rep, used;
    MapSource src = MapSource::kBuiltin;
    EXPECT(load_point_map_default_at(kDefaultDir, &rep, &src, &used));
    EXPECT(src == MapSource::kDefaultPath);
    EXPECT_NEAR(binding_for_index(EMS_SOC).scale, 100.0, 1e-9);

    // 再把同一路径换成一份非法的（点名被改坏）
    const std::string bad = replace_first(good, "MEAS.P_LOAD,IR,0", "MEAS.WRONG,IR,0");
    EXPECT(write_text_file(path.c_str(), bad));
    rep.clear();
    used.clear();
    src = MapSource::kBuiltin;
    EXPECT(!load_point_map_default_at(kDefaultDir, &rep, &src, &used));  // ② 硬失败
    EXPECT(rep.find("非法") != std::string::npos);   // 说清是"非法"而不是"没有"
    // ★ 关键：活动表仍是上一次成功加载的那份，**没有退回默认表**
    EXPECT(active_point_map().from_csv);
    EXPECT(!same_as_default(active_point_map()));
    EXPECT_NEAR(binding_for_index(EMS_SOC).scale, 100.0, 1e-9);
    EXPECT_EQ(full_scan_request_count(), static_cast<std::size_t>(3));

    std::remove(path.c_str());
    reset_point_map();
}

// ---------------------------------------------------------------------
// T34~T38: 配置通道② —— 接入参数（14/ 落盘 → 13/ 启动时读）
//
// 与 T31~T33 同一个套路，守的是接入参数那一半。为什么两半都要守：
//   点表全对、IP 错一个数字 → 读到的是**另一台设备**的数据，
//   而点表自检、分块、解码全部通过，现场没有任何异常迹象。
// ---------------------------------------------------------------------
static const char* kConnDir = "build/_t34_conn";

// 与 14/src/connconf.py 的 dumps() 数据段同构（注释头另加）
static std::string conn_text(const char* host = "192.168.1.10",
                             int port = 502, int unit = 3,
                             const char* enabled = "1") {
    std::string s;
    s += std::string("format=") + conn_format_tag() + "\n";
    s += "device_id=DEV-BMS-01\n";
    s += "protocol=modbus_tcp\n";
    s += std::string("host=") + host + "\n";
    s += "port=" + std::to_string(port) + "\n";
    s += "unit_id=" + std::to_string(unit) + "\n";
    s += "poll_period_ms=100\n";
    s += "timeout_ms=1500\n";
    s += "auto_reconnect=1\n";
    s += std::string("enabled=") + enabled + "\n";
    s += "saved_at=2026-09-29 02:00:00\n";
    s += "username=admin\n";
    return s;
}

static std::string to_crlf(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '\n') o += '\r'; o += c; }
    return o;
}

static void test_34_conn_missing_is_not_invalid() {
    std::printf("T34 约定路径下没有接入参数文件 → kMissing（**不编默认地址**）\n");
    DeviceConn c;
    c.host    = "SENTINEL";     // 若被改写，说明函数擅自填了值
    c.unit_id = 99;
    std::string rep, used;
    const ConnLoad s = load_device_conn_at("build/_t34_no_such_dir", &c, &rep, &used);
    EXPECT(s == ConnLoad::kMissing);          // 「没配」不是「配错」
    EXPECT(!used.empty());
    EXPECT(rep.find("没有接入参数文件") != std::string::npos);
    // ★ 关键：*out 一个字节都不该被碰 —— 这就是"不编默认值"的落点。
    //   编一个 127.0.0.1 出来，程序就会**真的去连它**，而屏幕上一切正常。
    EXPECT(c.host == "SENTINEL");
    EXPECT_EQ(c.unit_id, 99);
}

static void test_35_conn_loads_fields() {
    std::printf("T35 合法接入参数被加载，字段逐一对上（含 BOM/CRLF/注释头）\n");
    ensure_dir(kConnDir);
    const std::string path = std::string(kConnDir) + "/active.conn";

    // 按 14/ 真实落盘的样子造：BOM + 注释头 + CRLF
    const std::string real = std::string("\xEF\xBB\xBF") +
        "# ============ 注释头 ============\r\n"
        "# 设备  DEV-BMS-01\r\n" + to_crlf(conn_text());
    EXPECT(write_text_file(path.c_str(), real));

    DeviceConn c;
    std::string rep, used;
    const ConnLoad s = load_device_conn_at(kConnDir, &c, &rep, &used);
    EXPECT(s == ConnLoad::kOk);
    EXPECT(c.device_id == "DEV-BMS-01");
    EXPECT(c.protocol == "modbus_tcp");
    EXPECT(c.host == "192.168.1.10");
    EXPECT_EQ(c.port, 502);
    EXPECT_EQ(c.unit_id, 3);
    EXPECT_EQ(c.poll_period_ms, 100);
    EXPECT_EQ(c.timeout_ms, 1500);
    EXPECT(c.auto_reconnect);
    EXPECT(c.enabled);
    EXPECT(c.saved_at == "2026-09-29 02:00:00");   // 元信息也要读出来
    EXPECT(c.username == "admin");
}

static void test_36_conn_invalid_is_hard_failure() {
    std::printf("T36 接入参数非法 → kInvalid，且 *out 不被改动\n");
    ensure_dir(kConnDir);
    const std::string path = std::string(kConnDir) + "/active.conn";

    struct Case { const char* label; std::string text; };
    const Case cases[] = {
        {"键名拼错（hots=）", replace_first(conn_text(), "host=", "hots=")},
        {"缺必填键（删掉 host）", replace_first(conn_text(), "host=192.168.1.10\n", "")},
        {"格式版本不认识",
         replace_first(conn_text(), conn_format_tag(), "ems-device-conn/9")},
        {"端口越界", conn_text("192.168.1.10", 70000, 3)},
        {"从站号 0（广播）", conn_text("192.168.1.10", 502, 0)},
        {"从站号 248（超 247）", conn_text("192.168.1.10", 502, 248)},
        {"host 含非法字符", conn_text("192.168.1.10; rm -rf")},
        {"不是 key=value", conn_text() + "这行没有等号\n"},
        {"enabled 不是 0/1", conn_text("192.168.1.10", 502, 3, "maybe")},
        {"协议不认识",
         replace_first(conn_text(), "protocol=modbus_tcp", "protocol=iec61850")},
    };

    for (const Case& cs : cases) {
        EXPECT(write_text_file(path.c_str(), cs.text));
        DeviceConn c;
        c.host    = "SENTINEL";
        c.unit_id = 99;
        std::string rep, used;
        const ConnLoad s = load_device_conn_at(kConnDir, &c, &rep, &used);
        if (s != ConnLoad::kInvalid) {
            std::cerr << "  未拒绝: " << cs.label << " -> " << conn_load_name(s)
                      << std::endl;
        }
        EXPECT(s == ConnLoad::kInvalid);            // ① 硬失败，不回退
        EXPECT(c.host == "SENTINEL");                // ② *out 不被碰
        EXPECT_EQ(c.unit_id, 99);
        EXPECT(!rep.empty());
    }

    // 反证：换回合法件必须立刻恢复（否则上面的"全部拒绝"可能是因为函数坏了）
    EXPECT(write_text_file(path.c_str(), conn_text()));
    DeviceConn c2;
    std::string rep2, used2;
    EXPECT(load_device_conn_at(kConnDir, &c2, &rep2, &used2) == ConnLoad::kOk);
    EXPECT(c2.host == "192.168.1.10");
}

static void test_37_disabled_is_readable() {
    std::printf("T37 enabled=0 能被读出来（调用方据此拒绝连接）\n");
    ensure_dir(kConnDir);
    const std::string path = std::string(kConnDir) + "/active.conn";
    EXPECT(write_text_file(path.c_str(), conn_text("192.168.1.10", 502, 3, "0")));
    DeviceConn c;
    std::string rep, used;
    EXPECT(load_device_conn_at(kConnDir, &c, &rep, &used) == ConnLoad::kOk);
    // 加载本身是成功的（文件合法），「未启用」是**语义**，由调用方决定怎么办。
    // 这样设计是为了让 probe 与 07/ 各自给出恰当的处置，而不是在这里一刀切。
    EXPECT(!c.enabled);
    EXPECT(c.host == "192.168.1.10");     // 参数读得到，只是被标记为未启用
    // "0" / "false" / "no" / "off" 四种写法都该认
    EXPECT(write_text_file(path.c_str(),
                           replace_first(conn_text(), "enabled=1", "enabled=false")));
    DeviceConn c2;
    std::string r2, u2;
    EXPECT(load_device_conn_at(kConnDir, &c2, &r2, &u2) == ConnLoad::kOk);
    EXPECT(!c2.enabled);
}

static void test_38_conn_dir_matches_point_map_dir() {
    std::printf("T38 接入参数与点表**同目录**（目录约定只有一处）\n");
    // 两侧分叉 = 「平台把参数写到 A、端侧去 B 找」= 配置永远不生效，
    // 而两边各自都"成功"。所以这条必须钉死。
    const std::string p = default_device_conn_path();
    const std::string m = default_point_map_path();
    const std::size_t ps = p.find_last_of("\\/");
    const std::size_t ms = m.find_last_of("\\/");
    EXPECT(ps != std::string::npos && ms != std::string::npos);
    EXPECT(p.substr(0, ps) == m.substr(0, ms));
    EXPECT(p.substr(ps + 1) == "active.conn");
    EXPECT(m.substr(ms + 1) == "active.csv");
    // 显式给目录时也一样
    EXPECT(path_in_dir_conn("build/_t34_conn") != path_in_dir_conn("build/other"));
    EXPECT(path_in_dir_conn("build/_t34_conn").find("active.conn") != std::string::npos);
}

// =====================================================================
int main() {
    // ★ stdout 必须**无缓冲**。构建脚本会把本进程输出重定向到文件，而 stdio
    //   一旦重定向就改成**全缓冲**（4 KB）。本测试整份输出约 2.4 KB，一个缓冲区
    //   装得下 —— 于是全份输出都要等进程退出时那一次 flush 才落盘；那次 flush
    //   一旦没发生，**整份输出连同 PASS=597 一起消失，而退出码仍然是 0**
    //   （实测连续 20 次里出现过 1 次 0 字节）。无缓冲后写一条落一条。
    //   批处理侧还有第二道闸，见 13/scripts/build_test.bat 尾部的 :check_summary。
    std::setvbuf(stdout, nullptr, _IONBF, 0);

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
    test_16_default_map_blocks_match();
    test_17_csv_roundtrip();
    test_18_csv_overrides_take_effect();
    test_19_invalid_csv_rejected_and_rollback();
    test_20_shipped_template_matches_default();
    test_31_default_path_missing_falls_back();
    test_32_default_path_loads_and_takes_effect();
    test_33_default_path_invalid_is_hard_failure();
    test_34_conn_missing_is_not_invalid();
    test_35_conn_loads_fields();
    test_36_conn_invalid_is_hard_failure();
    test_37_disabled_is_readable();
    test_38_conn_dir_matches_point_map_dir();

    std::printf("\n");
    if (g_fail == 0) {
        std::printf("ALL TESTS PASSED\n");
    }
    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
