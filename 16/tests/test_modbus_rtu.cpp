// =====================================================================
// 16/ 单元测试 —— Modbus RTU（T50~T65）
//
//   T50 ★ CRC16/Modbus 已知答案向量（含线上字节序）
//   T51 帧格式 addr + PDU + crc(2)，CRC 低字节在前
//   T52 ★ 一次喂整帧 → 收 1 帧
//   T53 ★ 分两次喂但静默间隔不够 → **不**收帧；补齐间隔后才收
//   T54 ★ 粘包：一次喂两帧 → 收 2 帧（也测"分两次带够 gap"）
//   T55 ★ CRC 错帧被丢弃，且不污染后续帧
//   T56 ★ 垃圾字节重同步（前缀垃圾 / 中段垃圾）
//   T57 地址过滤（别的从站应答不算错误，只计外来帧）
//   T58 3.5 字符时间阈值随波特率变化
//   T59 缓冲区溢出保护
//   T60 假串口：回环 / 注入 / 未打开时的读写
//   T61 RtuMaster 正常事务（读寄存器）+ 发出的字节逐位核对
//   T62 RtuMaster 超时（从站静默）
//   T63 RtuMaster 从站异常响应 + 写回显校验
//   T64 请求构造参数越界必须**返回失败**而不是静默截断
//   T65 帧解析辅助（to_registers / is_exception / byte_count）
//
// 真串口时序无法在单元测试里造，所以分帧器的 API 把"静默间隔"显式参数化：
//   feed(bytes, gap_ms)   // gap_ms = 这批字节**之后**线路的空闲时长
// 于是"半包不给 gap / 给够 gap"这种时序差别变成了普通函数参数 —— 可精确断言。
//
// 编译：见 16/scripts/build_test.bat
// =====================================================================

#include "modbus_rtu.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace ems::comm;

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

// 字符串比较：EXPECT_EQ 内部转 long long，std::string 过不去
#define EXPECT_STR_EQ(a, b)                                               \
    do {                                                                  \
        std::string va_ = (a), vb_ = (b);                                 \
        if (va_ == vb_) {                                                 \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va_ << " vs " << #b << "=" \
                      << vb_ << std::endl;                                \
        }                                                                 \
    } while (0)

// =====================================================================
// 小工具
// =====================================================================
static std::string hex(const std::vector<std::uint8_t>& v) {
    std::string s;
    char b[8];
    for (std::size_t i = 0; i < v.size(); ++i) {
        std::snprintf(b, sizeof(b), "%02X", v[i]);
        if (i) s += " ";
        s += b;
    }
    return s;
}

// 造一帧 FC03 请求（8 字节）
static std::vector<std::uint8_t> make_read_req(std::uint8_t addr, std::uint16_t start,
                                               std::uint16_t count) {
    std::vector<std::uint8_t> pdu;
    build_read_pdu(kFcReadHoldingRegisters, start, count, pdu);
    return wrap_rtu_frame(addr, pdu);
}

// 造一帧 FC03 响应（5 + 2N 字节）
static std::vector<std::uint8_t> make_read_resp(std::uint8_t addr,
                                                const std::vector<std::uint16_t>& vals) {
    std::vector<std::uint8_t> pdu;
    pdu.push_back(kFcReadHoldingRegisters);
    pdu.push_back((std::uint8_t)(vals.size() * 2));
    for (std::size_t i = 0; i < vals.size(); ++i) {
        pdu.push_back((std::uint8_t)(vals[i] >> 8));
        pdu.push_back((std::uint8_t)(vals[i] & 0xFF));
    }
    return wrap_rtu_frame(addr, pdu);
}

static const int kBigGap = 100;   // 远大于任何波特率的 3.5 字符时间

// =====================================================================
// T50 ★ CRC 已知答案向量
// =====================================================================
static void test_50_crc_known_answers() {
    std::printf("T50 CRC16/Modbus 已知答案向量\n");

    // ★ 核心向量：01 03 00 00 00 0A → CRC 值 0xCDC5，线上字节 C5 CD（低字节在前）
    //   已用**独立实现**（pymodbus 3.15 FramerRTU.compute_CRC）交叉核对。
    const std::uint8_t v1[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};
    const std::uint16_t c1 = crc16_modbus(v1, 6);
    EXPECT_EQ(c1, 0xCDC5);
    EXPECT_EQ(c1 & 0xFF, 0xC5);          // 低字节
    EXPECT_EQ((c1 >> 8) & 0xFF, 0xCD);   // 高字节

    // 其余向量（同样与 pymodbus 核对过）
    struct Vec { std::uint8_t b[11]; int n; std::uint16_t crc; };
    const std::uint8_t v2[] = {0x01, 0x04, 0x02, 0xFF, 0xFF};
    EXPECT_EQ(crc16_modbus(v2, 5), 0x80B8);            // 经典向量
    const std::uint8_t v3[] = {0x11, 0x03, 0x00, 0x6B, 0x00, 0x03};
    EXPECT_EQ(crc16_modbus(v3, 6), 0x8776);            // Modbus 规范文档里的例子
    const std::uint8_t v4[] = {0x01, 0x10, 0x00, 0x01, 0x00, 0x02,
                               0x04, 0x00, 0x0A, 0x01, 0x02};
    EXPECT_EQ(crc16_modbus(v4, 11), 0x3092);           // FC16 写两寄存器
    const std::uint8_t v5[] = {0x01, 0x06, 0x00, 0x01, 0x00, 0x03};
    EXPECT_EQ(crc16_modbus(v5, 6), 0x0B98);
    const std::uint8_t v6[] = {0x01, 0x02, 0x00, 0x00, 0x00, 0x08};
    EXPECT_EQ(crc16_modbus(v6, 6), 0xCC79);

    // 空输入 → 初值（反射算法的固定答案）
    EXPECT_EQ(crc16_modbus(nullptr, 0), 0xFFFF);

    // 单字节"0x00"与两个字节的差别 —— 用来证明 CRC 不是简单累加
    const std::uint8_t z1[] = {0x00};
    const std::uint8_t z2[] = {0x00, 0x00};
    EXPECT(crc16_modbus(z1, 1) != crc16_modbus(z2, 2));

    // ---- 线上字节序：低字节在前 ----
    std::vector<std::uint8_t> f;
    build_read_pdu(kFcReadHoldingRegisters, 0, 10, f);
    std::vector<std::uint8_t> frame;
    frame.push_back(0x01);                     // ★ 地址在最前
    frame.insert(frame.end(), f.begin(), f.end());
    append_crc_le(frame);
    EXPECT_EQ(frame.size(), 8);
    EXPECT_EQ(frame[6], 0xC5);           // ★ 先低字节
    EXPECT_EQ(frame[7], 0xCD);
    // 反向守卫：如果写反（高字节在前），下面这条会红
    EXPECT(!(frame[6] == 0xCD && frame[7] == 0xC5));

    // crc_from_wire 与 crc_ok
    EXPECT_EQ(crc_from_wire(&frame[6]), 0xCDC5);
    EXPECT(crc_ok(frame.data(), frame.size()));
    frame[6] ^= 0x01;                    // 破坏一个 CRC 字节
    EXPECT(!crc_ok(frame.data(), frame.size()));
}

// =====================================================================
// T51 请求构造 / 帧格式
// =====================================================================
static void test_51_frame_layout() {
    std::printf("T51 帧格式 addr + PDU + crc\n");

    std::vector<std::uint8_t> pdu;
    EXPECT(build_read_pdu(kFcReadHoldingRegisters, 0x006B, 0x0003, pdu));
    EXPECT_EQ(pdu.size(), 5);
    EXPECT_EQ(pdu[0], 3);
    EXPECT_EQ(pdu[1], 0x00);
    EXPECT_EQ(pdu[2], 0x6B);
    EXPECT_EQ(pdu[3], 0x00);
    EXPECT_EQ(pdu[4], 0x03);

    const std::vector<std::uint8_t> frame = wrap_rtu_frame(0x11, pdu);
    // = 11 03 00 6B 00 03 76 87（Modbus 规范示例）
    EXPECT_EQ(frame.size(), 8);
    EXPECT_EQ(frame[0], 0x11);            // 地址在最前
    EXPECT_EQ(frame[1], 0x03);
    EXPECT_EQ(frame[6], 0x76);            // CRC 低字节
    EXPECT_EQ(frame[7], 0x87);
    EXPECT(crc_ok(frame.data(), frame.size()));

    // CRC 必须覆盖 addr + PDU，**不含** CRC 自身
    EXPECT_EQ(crc16_modbus(frame.data(), 6), crc_from_wire(&frame[6]));

    // 写单寄存器 / 写多寄存器
    std::vector<std::uint8_t> wpdu;
    EXPECT(build_write_single_register_pdu(0x0001, 0x0003, wpdu));
    EXPECT_EQ(wpdu.size(), 5);
    const std::vector<std::uint8_t> wf = wrap_rtu_frame(0x01, wpdu);
    EXPECT_STR_EQ(hex(wf), std::string("01 06 00 01 00 03 98 0B"));

    std::vector<std::uint16_t> vals;
    vals.push_back(0x000A); vals.push_back(0x0102);
    std::vector<std::uint8_t> mpdu;
    EXPECT(build_write_multiple_registers_pdu(0x0001, vals, mpdu));
    EXPECT_EQ(mpdu.size(), 10);           // 1+2+2+1(bytecount) + 4(data)
    const std::vector<std::uint8_t> mf = wrap_rtu_frame(0x01, mpdu);
    EXPECT_STR_EQ(hex(mf), std::string("01 10 00 01 00 02 04 00 0A 01 02 92 30"));
}

// =====================================================================
// T52 ★ 一次喂整帧
// =====================================================================
static void test_52_whole_frame() {
    std::printf("T52 一次喂整帧 → 收 1 帧\n");

    RtuFramer fr(9600, 0x01);
    EXPECT_EQ(fr.gap_threshold_ms(), 5);          // ceil(38500/9600)=5

    const std::vector<std::uint8_t> f = make_read_req(0x01, 0, 10);
    EXPECT_EQ(f.size(), 8);
    EXPECT_EQ(fr.feed(f, kBigGap), 1);
    EXPECT_EQ(fr.pending(), 1);
    EXPECT_EQ(fr.frames_ok(), 1);
    EXPECT_EQ(fr.bytes_dropped(), 0);
    EXPECT_EQ(fr.frames_crc_err(), 0);

    RtuFrame got;
    EXPECT(fr.pop(got));
    EXPECT_EQ(got.addr, 0x01);
    EXPECT_EQ(got.pdu.size(), 5);
    EXPECT_EQ(got.function(), 3);
    EXPECT(!got.is_exception());
    EXPECT_EQ(got.pdu[1], 0x00);
    EXPECT_EQ(got.pdu[2], 0x00);
    EXPECT_EQ(got.pdu[3], 0x00);
    EXPECT_EQ(got.pdu[4], 0x0A);
    EXPECT(!fr.pop(got));                          // 只有一帧
    EXPECT_EQ(fr.pending(), 0);

    // 反向守卫：同一批字节若**不给** gap，就一帧都不该收
    RtuFramer fr2(9600, 0x01);
    EXPECT_EQ(fr2.feed(f, kBigGap - 1), 1);        // 100 ≥ 5 → 收
    RtuFramer fr3(9600, 0x01);
    EXPECT_EQ(fr3.feed(f, 4), 0);                  // 4 < 5 → 不收
    EXPECT_EQ(fr3.pending(), 0);
    EXPECT_EQ(fr3.frames_ok(), 0);
}

// =====================================================================
// T53 ★ 半包（静默不够）
// =====================================================================
static void test_53_split_without_gap() {
    std::printf("T53 分两次喂但 gap 不够 → 不收帧\n");

    const std::vector<std::uint8_t> f = make_read_req(0x01, 0, 10);

    RtuFramer fr(9600, 0x01);
    std::vector<std::uint8_t> h1(f.begin(), f.begin() + 4);
    std::vector<std::uint8_t> h2(f.begin() + 4, f.end());

    EXPECT_EQ(fr.feed(h1, 1), 0);              // 1 < 5 → 帧还没结束
    EXPECT_EQ(fr.pending(), 0);
    EXPECT_EQ(fr.frames_ok(), 0);
    EXPECT_EQ(fr.frames_crc_err(), 0);         // ★ 也不能被当成"CRC 错帧"丢掉
    EXPECT_EQ(fr.frames_short(), 0);

    EXPECT_EQ(fr.feed(h2, 1), 0);              // 仍然不够
    EXPECT_EQ(fr.pending(), 0);

    // ★ 补齐静默间隔 → 这一帧才被收下
    EXPECT_EQ(fr.feed(nullptr, 0, kBigGap), 1);
    EXPECT_EQ(fr.frames_ok(), 1);
    RtuFrame got;
    EXPECT(fr.pop(got));
    EXPECT_EQ(got.addr, 0x01);
    EXPECT_EQ(got.pdu.size(), 5);

    // 逐字节喂 + 最后一次给够 gap（最细的时序）
    RtuFramer fr2(9600, 0x01);
    for (std::size_t i = 0; i + 1 < f.size(); ++i) {
        EXPECT_EQ(fr2.feed(&f[i], 1, 1), 0);
    }
    EXPECT_EQ(fr2.feed(&f[f.size() - 1], 1, kBigGap), 1);
    EXPECT_EQ(fr2.frames_ok(), 1);
}

// =====================================================================
// T54 ★ 粘包
// =====================================================================
static void test_54_batch_frames() {
    std::printf("T54 粘包：两帧 → 2 帧\n");

    const std::vector<std::uint8_t> a = make_read_req(0x01, 0, 10);       // 8 字节请求
    const std::vector<std::uint16_t> v;                                   // 空
    std::vector<std::uint16_t> regs;
    regs.push_back(0x1234); regs.push_back(0x5678);
    const std::vector<std::uint8_t> b = make_read_resp(0x01, regs);       // 9 字节响应

    // ---- ① 两个不同的帧拼在一次 feed 里 ----
    {
        RtuFramer fr(9600, 0x01);
        std::vector<std::uint8_t> both = a;
        both.insert(both.end(), b.begin(), b.end());
        EXPECT_EQ(fr.feed(both, kBigGap), 2);
        EXPECT_EQ(fr.pending(), 2);
        EXPECT_EQ(fr.frames_ok(), 2);
        EXPECT_EQ(fr.bytes_dropped(), 0);      // ★ 两帧都合法，一个字节都不该丢
        EXPECT_EQ(fr.frames_crc_err(), 0);
        RtuFrame f1, f2;
        EXPECT(fr.pop(f1));
        EXPECT(fr.pop(f2));
        EXPECT_EQ(f1.pdu.size(), 5);           // 请求：fc+start2+qty2
        EXPECT_EQ(f2.pdu.size(), 6);           // 响应：fc+bytecount+4 数据（CRC 不算在 PDU 里）
        std::vector<std::uint16_t> out;
        EXPECT(f2.to_registers(out));
        EXPECT_EQ(out.size(), 2);
        EXPECT_EQ(out[0], 0x1234);
        EXPECT_EQ(out[1], 0x5678);
    }
    // ---- ② 同样的两帧分两次喂，每次都给够 gap ----
    {
        RtuFramer fr(9600, 0x01);
        EXPECT_EQ(fr.feed(a, kBigGap), 1);
        EXPECT_EQ(fr.feed(b, kBigGap), 1);
        EXPECT_EQ(fr.frames_ok(), 2);
    }
    // ---- ③ 三帧连喂 ----
    {
        RtuFramer fr(9600, 0x01);
        std::vector<std::uint8_t> three = a;
        three.insert(three.end(), b.begin(), b.end());
        three.insert(three.end(), a.begin(), a.end());
        EXPECT_EQ(fr.feed(three, kBigGap), 3);
        EXPECT_EQ(fr.frames_ok(), 3);
        EXPECT_EQ(fr.bytes_dropped(), 0);
    }
    (void)v;
}

// =====================================================================
// T55 ★ CRC 错帧被丢弃且不污染后续
// =====================================================================
static void test_55_crc_error_frame() {
    std::printf("T55 CRC 错帧被丢弃且不污染后续\n");

    std::vector<std::uint8_t> bad = make_read_req(0x01, 0, 10);
    bad[7] ^= 0xFF;                        // 破坏 CRC 高字节

    RtuFramer fr(9600, 0x01);
    EXPECT_EQ(fr.feed(bad, kBigGap), 0);   // ★ 一帧都不收
    EXPECT_EQ(fr.pending(), 0);
    EXPECT_EQ(fr.frames_ok(), 0);
    EXPECT_EQ(fr.frames_crc_err(), 1);     // ★ 计数到"CRC 错帧"
    EXPECT_EQ(fr.bytes_dropped(), 8);      // ★ 8 个字节全丢
    // 反向守卫：计数器动了，说明确实走了"错帧"这条路而不是"根本没数据"

    // ★ 不污染后续：紧接着来一帧合法帧，必须正常收下
    const std::vector<std::uint8_t> good = make_read_req(0x01, 0, 10);
    EXPECT_EQ(fr.feed(good, kBigGap), 1);
    EXPECT_EQ(fr.frames_ok(), 1);
    RtuFrame got;
    EXPECT(fr.pop(got));
    EXPECT_EQ(got.addr, 0x01);
    EXPECT_EQ(got.pdu.size(), 5);
    // 后续帧的内容必须**逐位正确**（不是"收到了个东西"）
    EXPECT_EQ(got.pdu[0], 0x03);
    EXPECT_EQ(got.pdu[4], 0x0A);

    // 坏帧 + 好帧粘在一起（同一批字节）：好帧必须被抢救出来
    {
        RtuFramer f2(9600, 0x01);
        std::vector<std::uint8_t> mixed = bad;
        mixed.insert(mixed.end(), good.begin(), good.end());
        EXPECT_EQ(f2.feed(mixed, kBigGap), 1);       // 只收下好的那一帧
        EXPECT_EQ(f2.frames_ok(), 1);
        EXPECT(f2.resyncs() >= 1);                   // ★ 经过重同步
        EXPECT_EQ(f2.bytes_dropped(), 8);            // 坏帧那 8 字节被丢
        RtuFrame g2;
        EXPECT(f2.pop(g2));
        EXPECT_EQ(g2.pdu[4], 0x0A);
    }
    // 坏帧在**后面**：好帧先被切出来
    {
        RtuFramer f3(9600, 0x01);
        std::vector<std::uint8_t> mixed = good;
        mixed.insert(mixed.end(), bad.begin(), bad.end());
        EXPECT_EQ(f3.feed(mixed, kBigGap), 1);       // 好帧收下，坏帧丢弃
        EXPECT_EQ(f3.frames_ok(), 1);
        EXPECT_EQ(f3.bytes_dropped(), 8);
    }
}

// =====================================================================
// T56 ★ 垃圾字节重同步
// =====================================================================
static void test_56_resync_after_garbage() {
    std::printf("T56 垃圾字节重同步\n");

    const std::vector<std::uint8_t> good = make_read_req(0x01, 0, 10);

    // ---- ① 前缀垃圾（现场最常见的形态：485 上电/插拔时的电平毛刺） ----
    {
        RtuFramer fr(9600, 0x01);
        std::vector<std::uint8_t> j;
        j.push_back(0xFF); j.push_back(0xAA);
        j.insert(j.end(), good.begin(), good.end());
        EXPECT_EQ(fr.feed(j, kBigGap), 1);        // ★ 从偏移 2 处找到帧
        EXPECT_EQ(fr.frames_ok(), 1);
        EXPECT_EQ(fr.resyncs(), 1);
        EXPECT_EQ(fr.bytes_dropped(), 2);
        RtuFrame got;
        EXPECT(fr.pop(got));
        EXPECT_EQ(got.addr, 0x01);
        EXPECT_EQ(got.pdu[4], 0x0A);              // 内容逐位正确
    }
    // ---- ② 帧后尾巴垃圾（后面还跟着对不上的字节） ----
    {
        RtuFramer fr(9600, 0x01);
        std::vector<std::uint8_t> j = good;
        j.push_back(0x11); j.push_back(0x22);
        EXPECT_EQ(fr.feed(j, kBigGap), 1);
        EXPECT_EQ(fr.frames_ok(), 1);
        EXPECT_EQ(fr.bytes_dropped(), 2);
    }
    // ---- ③ 垃圾前缀 + 帧 + 垃圾尾巴 ----
    {
        RtuFramer fr(9600, 0x01);
        std::vector<std::uint8_t> j;
        j.push_back(0x5A);
        j.insert(j.end(), good.begin(), good.end());
        j.push_back(0x99);
        EXPECT_EQ(fr.feed(j, kBigGap), 1);
        EXPECT_EQ(fr.frames_ok(), 1);
        EXPECT_EQ(fr.bytes_dropped(), 2);
    }
    // ---- ④ 未知功能码的字节流应当被丢弃而不是当成帧 ----
    {
        RtuFramer fr(9600, 0x01);
        std::vector<std::uint8_t> j;
        j.push_back(0x01); j.push_back(0x7F); j.push_back(0x00); j.push_back(0x00);
        j.push_back(0x00); j.push_back(0x00); j.push_back(0xAB); j.push_back(0xCD);
        EXPECT_EQ(fr.feed(j, kBigGap), 0);
        EXPECT_EQ(fr.frames_ok(), 0);
        EXPECT_EQ(fr.bytes_dropped(), 8);
    }
    // ---- ⑤ 一堆纯噪声（100 字节 0xFF）必须被吃干净、不产生帧 ----
    {
        RtuFramer fr(9600, 0x01);
        std::vector<std::uint8_t> noise(100, 0xFF);
        EXPECT_EQ(fr.feed(noise, kBigGap), 0);
        EXPECT_EQ(fr.frames_ok(), 0);
        EXPECT_EQ(fr.bytes_dropped(), 100);
        EXPECT_EQ(fr.pending(), 0);
        // 之后来的好帧仍能被收下
        EXPECT_EQ(fr.feed(good, kBigGap), 1);
        EXPECT_EQ(fr.frames_ok(), 1);
    }
}

// =====================================================================
// T57 地址过滤
// =====================================================================
static void test_57_address_filter() {
    std::printf("T57 地址过滤\n");

    RtuFramer fr(9600, 0x01);                 // 本机只收地址 1
    const std::vector<std::uint8_t> mine = make_read_req(0x01, 0, 10);
    const std::vector<std::uint8_t> other = make_read_req(0x02, 0, 10);
    const std::vector<std::uint8_t> other2 = make_read_req(0x11, 0, 10);

    EXPECT_EQ(fr.feed(other, kBigGap), 0);    // 地址不匹配 → 不入队
    EXPECT_EQ(fr.pending(), 0);
    EXPECT_EQ(fr.frames_foreign_addr(), 1);
    EXPECT_EQ(fr.frames_ok(), 0);
    EXPECT_EQ(fr.bytes_dropped(), 0);         // ★ 外来帧**不是**错误，不算丢字节

    EXPECT_EQ(fr.feed(other2, kBigGap), 0);
    EXPECT_EQ(fr.frames_foreign_addr(), 2);

    EXPECT_EQ(fr.feed(mine, kBigGap), 1);     // 自己的收下
    EXPECT_EQ(fr.frames_ok(), 1);
    EXPECT_EQ(fr.frames_foreign_addr(), 2);
    EXPECT_EQ(fr.pending(), 1);

    // address=0 = 接收所有（抓包/调试模式）
    RtuFramer any(9600, 0x00);
    EXPECT_EQ(any.feed(other, kBigGap), 1);
    EXPECT_EQ(any.feed(mine, kBigGap), 1);
    EXPECT_EQ(any.frames_ok(), 2);
    EXPECT_EQ(any.frames_foreign_addr(), 0);
}

// =====================================================================
// T58 3.5 字符时间随波特率变化
// =====================================================================
static void test_58_gap_threshold_by_baud() {
    std::printf("T58 3.5 字符时间阈值\n");

    // 38500 / baud 向上取整
    EXPECT_EQ(RtuFramer::char_gap_ms_for_baud(1200), 33);    // 32.08 → 33
    EXPECT_EQ(RtuFramer::char_gap_ms_for_baud(9600), 5);     // 4.01 → 5
    EXPECT_EQ(RtuFramer::char_gap_ms_for_baud(19200), 3);    // 2.005 → 3
    EXPECT_EQ(RtuFramer::char_gap_ms_for_baud(38400), 2);    // 1.002 → 2
    EXPECT_EQ(RtuFramer::char_gap_ms_for_baud(115200), 1);   // 0.334 → 1
    EXPECT_EQ(RtuFramer::char_gap_ms_for_baud(0), 5);        // 非法波特率按 9600 兜底

    // ★ 下界：115200 理论值 1 ms，但实测给 1 ms 会把合法帧从中间截断，
    //   所以构造时带一个最小间隔（默认 2 ms）。
    {
        RtuFramer fr(115200);
        EXPECT_EQ(fr.gap_threshold_ms(), 2);      // max(1, 最小 2)
        fr.set_min_gap_ms(1);
        EXPECT_EQ(fr.gap_threshold_ms(), 1);
        fr.set_min_gap_ms(10);
        EXPECT_EQ(fr.gap_threshold_ms(), 10);     // 手动调大也生效
    }
    {
        RtuFramer fr(9600);
        EXPECT_EQ(fr.gap_threshold_ms(), 5);      // 理论 5 > 最小 2 → 用 5
    }

    // 阈值边界：gap == 阈值 → 收；gap == 阈值-1 → 不收
    const std::vector<std::uint8_t> f = make_read_req(0x01, 0, 10);
    {
        RtuFramer fr(9600, 0x01);
        EXPECT_EQ(fr.feed(f, 5), 1);
    }
    {
        RtuFramer fr(9600, 0x01);
        EXPECT_EQ(fr.feed(f, 4), 0);
    }
    {
        RtuFramer fr(1200, 0x01);
        EXPECT_EQ(fr.gap_threshold_ms(), 33);
        EXPECT_EQ(fr.feed(f, 32), 0);
        EXPECT_EQ(fr.feed(nullptr, 0, 33), 1);
    }
}

// =====================================================================
// T59 缓冲区溢出保护
// =====================================================================
static void test_59_overflow_protection() {
    std::printf("T59 缓冲区溢出保护\n");

    RtuFramer fr(9600, 0x01);
    // 一直灌字节但永远不给够 gap（模拟波特率配错，两边各说各的）
    std::vector<std::uint8_t> junk(64);
    for (int i = 0; i < 5000; ++i) {
        for (std::size_t k = 0; k < junk.size(); ++k) junk[k] = (std::uint8_t)(i + k);
        fr.feed(junk, 0);                       // gap=0 → 永不 flush
    }
    EXPECT(fr.overflow_drops() > 0);            // ★ 触发了溢出保护
    EXPECT(fr.bytes_dropped() > 0);
    EXPECT_EQ(fr.frames_ok(), 0);
    EXPECT_EQ(fr.pending(), 0);
    // 反向守卫：确实灌进了远多于一个缓冲区的字节
    EXPECT(fr.bytes_in() > (int)RtuFramer::kMaxFrameBytes * 10);
    // 内存没有无界增长：缓冲区被钳在 256 以内（用一个合法帧把状态拉回正常）
    EXPECT_EQ(fr.feed(make_read_req(0x01, 0, 10), kBigGap), 1);
    EXPECT_EQ(fr.frames_ok(), 1);
}

// =====================================================================
// T60 假串口
// =====================================================================
static void test_60_loopback_port() {
    std::printf("T60 假串口（回环/注入）\n");

    LoopbackSerialPort p("COM_TEST");
    EXPECT_STR_EQ(p.name(), std::string("COM_TEST"));
    EXPECT(!p.is_open());
    EXPECT_EQ(p.write(nullptr, 0), -1);         // 未打开 → 失败
    std::uint8_t buf[16];
    EXPECT_EQ(p.read(buf, sizeof(buf), 10), -1);

    EXPECT(p.open());
    EXPECT(p.is_open());

    const std::uint8_t data[] = {0x01, 0x02, 0x03};
    EXPECT_EQ(p.write(data, 3), 3);
    EXPECT_EQ(p.tx().size(), std::size_t(3));
    EXPECT_EQ(p.tx()[0], 0x01);
    EXPECT_EQ(p.rx_pending(), std::size_t(0));   // 非回环 → 收不到自己发的

    // 开回环 → 自己发的能收到
    p.set_loopback(true);
    EXPECT_EQ(p.write(data, 3), 3);
    EXPECT_EQ(p.tx().size(), std::size_t(6));
    EXPECT_EQ(p.rx_pending(), std::size_t(3));
    const int n = p.read(buf, sizeof(buf), 10);
    EXPECT_EQ(n, 3);
    EXPECT_EQ(buf[0], 0x01);
    EXPECT_EQ(buf[2], 0x03);
    EXPECT_EQ(p.rx_pending(), std::size_t(0));
    EXPECT_EQ(p.read(buf, sizeof(buf), 10), 0);  // 空 → 0 = 超时（不是错误）

    // 注入从站应答
    p.inject_rx(make_read_resp(0x01, std::vector<std::uint16_t>(1, 0x00AB)));
    EXPECT_EQ(p.rx_pending(), std::size_t(7));   // 5 + 2*1
    const int n2 = p.read(buf, sizeof(buf), 10);
    EXPECT_EQ(n2, 7);
    EXPECT_EQ(buf[0], 0x01);
    EXPECT_EQ(buf[1], 0x03);
    EXPECT_EQ(buf[2], 0x02);

    // 部分读取：一次只取 3 字节，剩下留着
    p.clear_tx();
    p.inject_rx(make_read_req(0x01, 0, 10));
    EXPECT_EQ(p.read(buf, 3, 10), 3);
    EXPECT_EQ(p.rx_pending(), std::size_t(5));
    EXPECT_EQ(p.read(buf, 16, 10), 5);
    EXPECT_EQ(p.rx_pending(), std::size_t(0));

    // 写失败路径
    p.set_fail_writes(true);
    EXPECT_EQ(p.write(data, 3), -1);
    p.set_fail_writes(false);

    p.close();
    EXPECT(!p.is_open());
    EXPECT_EQ(p.write(data, 3), -1);
}

// =====================================================================
// T61 RtuMaster 正常事务
// =====================================================================
static void test_61_master_transact() {
    std::printf("T61 RtuMaster 正常事务\n");

    LoopbackSerialPort port("COM_TEST");
    port.open();
    port.set_empty_read_sleep_ms(1);
    RtuMaster m(port, 0x01, 9600, 120);
    m.framer().set_min_gap_ms(2);

    // 在"线路"上预置一帧从站应答：寄存器 0x10、0x20
    std::vector<std::uint16_t> regs;
    regs.push_back(0x0010);
    regs.push_back(0x0020);
    port.inject_rx(make_read_resp(0x01, regs));

    std::vector<std::uint16_t> out;
    std::string err;
    const bool ok = m.read_registers(false, 0x0000, 2, out, err);
    EXPECT(ok);
    EXPECT(err.empty());
    EXPECT_EQ(out.size(), 2);
    EXPECT_EQ(out[0], 0x0010);
    EXPECT_EQ(out[1], 0x0020);
    EXPECT_EQ(m.requests(), 1);
    EXPECT_EQ(m.timeouts(), 0);

    // ★ 发出的字节必须逐位正确（主站"发错帧"是从站不理人的第一原因）
    const std::vector<std::uint8_t> expect = make_read_req(0x01, 0x0000, 2);
    EXPECT_EQ(port.tx().size(), expect.size());
    EXPECT_STR_EQ(hex(port.tx()), hex(expect));
    EXPECT(crc_ok(port.tx().data(), port.tx().size()));

    // 从站异常响应（非法地址）→ 必须报出异常码，而不是当成通信故障
    {
        LoopbackSerialPort p2("COM2");
        p2.open();
        p2.set_empty_read_sleep_ms(1);
        RtuMaster m2(p2, 0x01, 9600, 120);
        std::vector<std::uint8_t> pdu;
        pdu.push_back((std::uint8_t)(kFcReadHoldingRegisters | 0x80));
        pdu.push_back(0x02);                              // 非法数据地址
        p2.inject_rx(wrap_rtu_frame(0x01, pdu));
        std::vector<std::uint16_t> o2;
        std::string e2;
        EXPECT(!m2.read_registers(false, 0x0000, 2, o2, e2));
        EXPECT(!e2.empty());
        EXPECT_EQ(m2.exceptions(), 1);
        EXPECT_EQ(m2.timeouts(), 0);                      // ★ 不是超时
    }
    // 写多个寄存器：回显校验
    {
        LoopbackSerialPort p3("COM3");
        p3.open();
        p3.set_empty_read_sleep_ms(1);
        RtuMaster m3(p3, 0x01, 9600, 120);
        std::vector<std::uint8_t> echo;
        echo.push_back(kFcWriteMultipleRegisters);
        echo.push_back(0x00); echo.push_back(0x05);
        echo.push_back(0x00); echo.push_back(0x02);
        p3.inject_rx(wrap_rtu_frame(0x01, echo));
        std::vector<std::uint16_t> vals;
        vals.push_back(0x1111); vals.push_back(0x2222);
        std::string e3;
        EXPECT(m3.write_registers(0x0005, vals, e3));
        EXPECT(e3.empty());
        // 发出的帧核对（FC16 长度为 9+2N）
        std::vector<std::uint8_t> wpdu;
        build_write_multiple_registers_pdu(0x0005, vals, wpdu);
        EXPECT_STR_EQ(hex(p3.tx()), hex(wrap_rtu_frame(0x01, wpdu)));

        // 回显不匹配 → 必须失败
        LoopbackSerialPort p4("COM4");
        p4.open();
        p4.set_empty_read_sleep_ms(1);
        RtuMaster m4(p4, 0x01, 9600, 120);
        std::vector<std::uint8_t> bad = echo;
        bad[3] = 0x09;                                   // 回显的 count 不对
        p4.inject_rx(wrap_rtu_frame(0x01, bad));
        std::string e4;
        EXPECT(!m4.write_registers(0x0005, vals, e4));
        EXPECT(!e4.empty());
    }
}

// =====================================================================
// T62 RtuMaster 超时
// =====================================================================
static void test_62_master_timeout() {
    std::printf("T62 RtuMaster 超时\n");

    LoopbackSerialPort port("COM_TEST");
    port.open();
    port.set_empty_read_sleep_ms(1);
    RtuMaster m(port, 0x01, 9600, 60);      // 60 ms 超时

    // 什么都不注入 → 从站静默
    std::vector<std::uint16_t> out;
    std::string err;
    EXPECT(!m.read_registers(false, 0x0000, 2, out, err));
    EXPECT(!err.empty());
    EXPECT(err.find("超时") != std::string::npos);       // ★ 报的是"超时"
    EXPECT_EQ(m.timeouts(), 1);
    EXPECT_EQ(m.requests(), 1);
    // 请求确实发出去了（否则"超时"没意义）
    EXPECT_EQ(port.tx().size(), std::size_t(8));

    // 未打开的串口 → 立刻失败，不进入轮询
    {
        LoopbackSerialPort p2("COM2");           // 没 open()
        RtuMaster m2(p2, 0x01, 9600, 1000);
        std::vector<std::uint16_t> o2;
        std::string e2;
        EXPECT(!m2.read_registers(false, 0, 2, o2, e2));
        EXPECT(!e2.empty());
        EXPECT_EQ(m2.timeouts(), 0);             // ★ 不是"超时"，是"串口没打开"
    }
    // 写失败 → 立刻失败
    {
        LoopbackSerialPort p3("COM3");
        p3.open();
        p3.set_fail_writes(true);
        RtuMaster m3(p3, 0x01, 9600, 1000);
        std::vector<std::uint16_t> o3;
        std::string e3;
        EXPECT(!m3.read_registers(false, 0, 2, o3, e3));
        EXPECT(!e3.empty());
        EXPECT_EQ(m3.timeouts(), 0);
    }
    // 应答 CRC 被破坏 → 超时（帧被丢弃），且 framer 记到了 CRC 错
    {
        LoopbackSerialPort p4("COM4");
        p4.open();
        p4.set_empty_read_sleep_ms(1);
        RtuMaster m4(p4, 0x01, 9600, 60);
        std::vector<std::uint16_t> regs(1, 0x0001);
        std::vector<std::uint8_t> bad = make_read_resp(0x01, regs);
        bad[bad.size() - 1] ^= 0xFF;
        p4.inject_rx(bad);
        std::vector<std::uint16_t> o4;
        std::string e4;
        EXPECT(!m4.read_registers(false, 0, 1, o4, e4));
        EXPECT_EQ(m4.timeouts(), 1);
        EXPECT(m4.framer().frames_crc_err() >= 1);       // ★ 能区分"没回"与"回了但坏了"
    }
}

// =====================================================================
// T63 帧解析辅助
// =====================================================================
static void test_63_frame_helpers() {
    std::printf("T63 帧解析辅助\n");

    // 正常响应
    std::vector<std::uint16_t> regs;
    regs.push_back(0x0102); regs.push_back(0xFFFE);
    const std::vector<std::uint8_t> rf = make_read_resp(0x01, regs);
    EXPECT_EQ(rf.size(), std::size_t(9));      // 1+1+1+4+2
    EXPECT(crc_ok(rf.data(), rf.size()));

    RtuFramer fr(9600, 0x01);
    EXPECT_EQ(fr.feed(rf, kBigGap), 1);
    RtuFrame f;
    EXPECT(fr.pop(f));
    EXPECT_EQ(f.addr, 0x01);
    EXPECT_EQ(f.function(), 3);
    EXPECT(!f.is_exception());
    EXPECT_EQ(f.exception_code(), 0);
    EXPECT_EQ(f.byte_count(), 4);
    std::vector<std::uint16_t> out;
    EXPECT(f.to_registers(out));
    EXPECT_EQ(out.size(), 2);
    EXPECT_EQ(out[0], 0x0102);
    EXPECT_EQ(out[1], 0xFFFE);

    // 异常响应
    std::vector<std::uint8_t> ex;
    ex.push_back((std::uint8_t)(kFcReadInputRegisters | 0x80));
    ex.push_back(0x03);
    const std::vector<std::uint8_t> ef = wrap_rtu_frame(0x01, ex);
    EXPECT_EQ(ef.size(), std::size_t(5));
    RtuFramer fr2(9600, 0x01);
    EXPECT_EQ(fr2.feed(ef, kBigGap), 1);
    RtuFrame e;
    EXPECT(fr2.pop(e));
    EXPECT(e.is_exception());
    EXPECT_EQ(e.exception_code(), 3);
    EXPECT_EQ(e.function(), 4);                // 去掉最高位后的功能码
    std::vector<std::uint16_t> no;
    EXPECT(!e.to_registers(no));               // 异常帧解不出寄存器

    // ByteCount 与 PDU 长度不符 → to_registers 必须失败
    {
        std::vector<std::uint8_t> pdu;
        pdu.push_back(kFcReadHoldingRegisters);
        pdu.push_back(0x06);                   // 声称 6 字节
        pdu.push_back(0x00); pdu.push_back(0x01);   // 实际只有 2 字节
        const std::vector<std::uint8_t> bf = wrap_rtu_frame(0x01, pdu);
        RtuFramer f3(9600, 0x01);
        EXPECT_EQ(f3.feed(bf, kBigGap), 1);    // CRC 对 → 帧本身合法
        RtuFrame g;
        EXPECT(f3.pop(g));
        EXPECT_EQ(g.byte_count(), 6);
        std::vector<std::uint16_t> o2;
        EXPECT(!g.to_registers(o2));           // ★ 但解不出（长度不符）
    }
    // 奇数字节数也必须被拒（寄存器是 2 字节）
    {
        std::vector<std::uint8_t> pdu;
        pdu.push_back(kFcReadInputRegisters);
        pdu.push_back(0x03);
        pdu.push_back(0x00); pdu.push_back(0x01); pdu.push_back(0x02);
        const std::vector<std::uint8_t> bf = wrap_rtu_frame(0x01, pdu);
        RtuFramer f4(9600, 0x01);
        EXPECT_EQ(f4.feed(bf, kBigGap), 1);
        RtuFrame g;
        EXPECT(f4.pop(g));
        std::vector<std::uint16_t> o3;
        EXPECT(!g.to_registers(o3));
    }
    // 对位读响应调 to_registers → 不适用
    {
        std::vector<std::uint8_t> pdu;
        pdu.push_back(kFcReadDiscreteInputs);
        pdu.push_back(0x01);
        pdu.push_back(0xA5);
        const std::vector<std::uint8_t> bf = wrap_rtu_frame(0x01, pdu);
        RtuFramer f5(9600, 0x01);
        EXPECT_EQ(f5.feed(bf, kBigGap), 1);
        RtuFrame g;
        EXPECT(f5.pop(g));
        EXPECT_EQ(g.function(), 2);
        std::vector<std::uint16_t> o4;
        EXPECT(!g.to_registers(o4));
    }
}

// =====================================================================
// T64 请求参数越界
// =====================================================================
static void test_64_request_bounds() {
    std::printf("T64 请求参数越界必须失败\n");

    std::vector<std::uint8_t> pdu;

    // FC03/04：count 1..125
    EXPECT(!build_read_pdu(kFcReadHoldingRegisters, 0, 0, pdu));      // count=0
    EXPECT(!build_read_pdu(kFcReadHoldingRegisters, 0, 126, pdu));    // 超上限
    EXPECT(build_read_pdu(kFcReadHoldingRegisters, 0, 125, pdu));     // 恰好上限
    EXPECT(build_read_pdu(kFcReadHoldingRegisters, 0, 1, pdu));       // 下限
    // FC01/02：count 1..2000
    EXPECT(!build_read_pdu(kFcReadCoils, 0, 0, pdu));
    EXPECT(!build_read_pdu(kFcReadCoils, 0, 2001, pdu));
    EXPECT(build_read_pdu(kFcReadCoils, 0, 2000, pdu));
    // 不支持的功能码
    EXPECT(!build_read_pdu(6, 0, 1, pdu));
    EXPECT(!build_read_pdu(0x80, 0, 1, pdu));
    // 反向守卫：失败时不能留下半截 PDU（否则调用方可能把它发出去）
    pdu.clear();
    EXPECT(!build_read_pdu(kFcReadHoldingRegisters, 0, 0, pdu));
    EXPECT(pdu.empty());

    // 写多寄存器：1..123 个
    std::vector<std::uint16_t> none;
    EXPECT(!build_write_multiple_registers_pdu(0, none, pdu));
    EXPECT(pdu.empty());
    std::vector<std::uint16_t> many(124, 0);
    EXPECT(!build_write_multiple_registers_pdu(0, many, pdu));
    std::vector<std::uint16_t> ok(123, 0x1234);
    EXPECT(build_write_multiple_registers_pdu(0, ok, pdu));
    EXPECT_EQ(pdu.size(), std::size_t(6 + 123 * 2));       // fc+start2+count2+bc1+data
    EXPECT_EQ(pdu[5], 246);                                // bytecount = 2*123
}

// =====================================================================
// T65 大小端与字节序（现场第一大坑的 RTU 版本）
// =====================================================================
static void test_65_byte_order() {
    std::printf("T65 寄存器字节序（大端）\n");

    // Modbus 规定寄存器**大端**传输：高字节在前
    std::vector<std::uint16_t> vals;
    vals.push_back(0x1234);
    std::vector<std::uint8_t> pdu;
    EXPECT(build_write_multiple_registers_pdu(0x0000, vals, pdu));
    EXPECT_EQ(pdu[pdu.size() - 2], 0x12);       // ★ 高字节在前
    EXPECT_EQ(pdu[pdu.size() - 1], 0x34);

    // 读回时也要按大端重组
    std::vector<std::uint16_t> regs;
    regs.push_back(0x1234);
    const std::vector<std::uint8_t> rf = make_read_resp(0x01, regs);
    EXPECT_EQ(rf[3], 0x12);
    EXPECT_EQ(rf[4], 0x34);
    RtuFramer fr(9600, 0x01);
    EXPECT_EQ(fr.feed(rf, kBigGap), 1);
    RtuFrame f;
    EXPECT(fr.pop(f));
    std::vector<std::uint16_t> out;
    EXPECT(f.to_registers(out));
    EXPECT_EQ(out[0], 0x1234);                  // ★ 重组正确

    // 反向守卫：若按小端重组会得到 0x3412
    EXPECT(out[0] != 0x3412);

    // 单寄存器写也是大端
    std::vector<std::uint8_t> sp;
    EXPECT(build_write_single_register_pdu(0x0001, 0xABCD, sp));
    EXPECT_EQ(sp[3], 0xAB);
    EXPECT_EQ(sp[4], 0xCD);

    // 起止地址也是大端
    std::vector<std::uint8_t> rp;
    EXPECT(build_read_pdu(kFcReadHoldingRegisters, 0x0102, 0x0002, rp));
    EXPECT_EQ(rp.size(), std::size_t(5));
    EXPECT_EQ(rp[1], 0x01);     // 起址高字节
    EXPECT_EQ(rp[2], 0x02);     // 起址低字节
    EXPECT_EQ(rp[3], 0x00);     // 数量高字节
    EXPECT_EQ(rp[4], 0x02);     // 数量低字节
}

// =====================================================================
int main() {
    std::printf("=== 16/ Modbus RTU（串口） 单元测试 ===\n\n");

    test_50_crc_known_answers();
    test_51_frame_layout();
    test_52_whole_frame();
    test_53_split_without_gap();
    test_54_batch_frames();
    test_55_crc_error_frame();
    test_56_resync_after_garbage();
    test_57_address_filter();
    test_58_gap_threshold_by_baud();
    test_59_overflow_protection();
    test_60_loopback_port();
    test_61_master_transact();
    test_62_master_timeout();
    test_63_frame_helpers();
    test_64_request_bounds();
    test_65_byte_order();

    std::printf("\n");
    if (g_fail == 0) std::printf("ALL TESTS PASSED\n");
    // 本用例没有"跳过"路径，恒为 0；仍按约定打出来，
    // 让每层的状态行格式一致（见 新模块开发约定.md §4.5）
    std::printf("PASS=%d FAIL=%d SKIPPED=0\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
