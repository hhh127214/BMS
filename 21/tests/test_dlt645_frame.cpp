// =====================================================================
// 21/ 单元测试（一）—— DL/T645 帧层 + 数据项层
//
//   T01 地址 BCD 往返（低位在前）
//   T02 地址非法输入被拒（长度 / 非数字）
//   T03 黄金向量 #1：全 0 地址，CS 手算 0xB1
//   T04 黄金向量 #2：地址 01，DI=02010100，CS 手算 0xB6
//   T05 CS 错一字节 → BAD_CHECKSUM
//   T06 结束符不是 16 → BAD_END
//   T07 第二个 68 不在位置 → BAD_SECOND_START
//   T08 前导 FE 被吃掉且计数正确
//   T09 半帧 → INCOMPLETE（**不是错误**）
//   T10 +0x33 偏移边界（0xCC 是上限）
//   T11 DI 传输序（低位在前）
//   T12 encode → parse 往返（含 FE）
//   T13 BCD 电压 220.5 V
//   T14 BCD 电流（3 位小数）
//   T15 BCD 功率正值
//   T16 BCD 功率负值（符号位在最高字节 bit7）
//   T17 BCD 半字节越界 → 拒绝（★ 核心负向）
//   T18 数据长度与 DI 声明不符 → 拒绝
//   T19 encode_bcd 与 decode_bcd 对称
//   T20 超出该 DI 表示范围 → 拒绝
//   T21 变比换算
//   T22 内置 DI 表查找（命中 / 未命中）
//
// ---------------------------------------------------------------------
// ★ 为什么黄金向量要**手写字节**而不是"调 checksum() 算出来再比"
// ---------------------------------------------------------------------
// 用实现自己算期望值，只能证明"函数是它自己" —— 校验和公式错了一个
// 加减项，往返测试照样全绿。所以 T03/T04 的 CS 是**逐字节加起来手算**的：
//   T03: 0x68+0x68+0x11+0x04+0x33*4 = 433 = 0x1B1 → 0xB1
//   T04: 0x68+0x01+0x68+0x11+0x04+(0x33+0x34+0x34+0x35=0xD0)
//        = 438 = 0x1B6 → 0xB6
// 这两条一旦红，说明公式本身错了，而不是"某一层没对上"。
// =====================================================================

#include "dlt645_codec.h"
#include "dlt645_frame.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

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

#define EXPECT_STR(a, b)                                                  \
    do {                                                                  \
        std::string va = (a), vb = (b);                                   \
        if (va == vb) {                                                   \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : \"" << va << "\" vs \"" << vb << "\""        \
                      << std::endl;                                       \
        }                                                                 \
    } while (0)

static std::string hex(const std::vector<std::uint8_t>& v) {
    static const char* k = "0123456789ABCDEF";
    std::string s;
    for (std::uint8_t b : v) {
        s.push_back(k[(b >> 4) & 0x0F]);
        s.push_back(k[b & 0x0F]);
        s.push_back(' ');
    }
    if (!s.empty()) s.pop_back();
    return s;
}

// =====================================================================
// T01 地址 BCD 往返
// =====================================================================
static void t01_address_roundtrip() {
    std::printf("T01 地址 BCD 往返（低位在前）\n");

    Address a;
    EXPECT(a.from_string("000000000001"));
    // ★ 低位在前：个位字节先上线
    EXPECT_EQ(a.at(0), 0x01);
    for (int i = 1; i < 6; ++i) EXPECT_EQ(a.at(i), 0x00);
    EXPECT_STR(a.to_string(), "000000000001");

    Address b;
    EXPECT(b.from_string("123456789012"));
    // 两位一组、从右往左：12|90|78|56|34|12，低位在前
    EXPECT_EQ(b.at(0), 0x12);   // 最低两位 "12"
    EXPECT_EQ(b.at(1), 0x90);
    EXPECT_EQ(b.at(2), 0x78);
    EXPECT_EQ(b.at(3), 0x56);
    EXPECT_EQ(b.at(4), 0x34);
    EXPECT_EQ(b.at(5), 0x12);   // 最高两位
    EXPECT_STR(b.to_string(), "123456789012");

    Address c;
    EXPECT(c.from_string("000000000042"));
    EXPECT_EQ(c.at(0), 0x42);
    EXPECT_STR(c.to_string(), "000000000042");

    // 广播地址
    Address bc;
    EXPECT(bc.from_string("AAAAAAAAAAAA") == false);  // 非十进制，必须拒
    for (int i = 0; i < 6; ++i) bc.set_at(i, 0xAA);
    EXPECT(bc.is_broadcast());
    EXPECT(!a.is_broadcast());
}

// =====================================================================
// T02 地址非法输入
// =====================================================================
static void t02_address_invalid() {
    std::printf("T02 地址非法输入被拒\n");
    Address a;
    EXPECT(!a.from_string("123"));              // 太短
    EXPECT(!a.from_string("0000000000001"));    // 太长
    EXPECT(!a.from_string("00000000000A"));     // 含非数字
    EXPECT(!a.from_string(""));
    // 被拒后必须是干净的 0，而不是"上次的内容"
    EXPECT_STR(a.to_string(), "000000000000");
}

// =====================================================================
// T03 / T04 黄金向量
// =====================================================================
static void t03_golden_all_zero() {
    std::printf("T03 黄金向量 #1（全 0 地址，CS=0xB1）\n");
    Address a;
    EXPECT(a.from_string("000000000000"));

    Frame f;
    f.addr = a;
    f.ctrl = kReadData;
    f.data.assign(4, 0x00);   // DI = 00000000

    const std::vector<std::uint8_t> raw = encode(f, 0);

    // 68 | 6×addr | 68 | C | L | DATA+0x33 | CS | 16
    const std::uint8_t want[] = {
        0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68,
        0x11, 0x04, 0x33, 0x33, 0x33, 0x33, 0xB1, 0x16};
    EXPECT_EQ(raw.size(), sizeof(want));
    if (raw.size() != sizeof(want)) {
        std::printf("    实际帧: %s\n", hex(raw).c_str());
    }
    for (std::size_t i = 0; i < sizeof(want) && i < raw.size(); ++i) {
        if (raw[i] != want[i]) {
            std::printf("    字节 %zu: got %02X want %02X\n", i, raw[i], want[i]);
        }
        EXPECT_EQ(raw[i], want[i]);
    }
}

static void t04_golden_read_voltage() {
    std::printf("T04 黄金向量 #2（读 A 相电压，CS=0xB6）\n");
    Address a;
    EXPECT(a.from_string("000000000001"));

    Frame f;
    f.addr = a;
    f.ctrl = kReadData;
    std::uint8_t diw[4];
    di_to_wire(0x02010100u, diw);
    f.data.assign(diw, diw + 4);

    const std::vector<std::uint8_t> raw = encode(f, 0);
    const std::uint8_t want[] = {
        0x68, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68,
        0x11, 0x04, 0x33, 0x34, 0x34, 0x35, 0xB6, 0x16};
    EXPECT_EQ(raw.size(), sizeof(want));
    for (std::size_t i = 0; i < sizeof(want) && i < raw.size(); ++i) {
        EXPECT_EQ(raw[i], want[i]);
    }
}

// =====================================================================
// T05 ~ T09 解析的负向路径
// =====================================================================
static void t05_bad_checksum() {
    std::printf("T05 CS 错一字节 → BAD_CHECKSUM\n");
    std::uint8_t buf[] = {0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68,
                          0x11, 0x04, 0x33, 0x33, 0x33, 0x33, 0xB2, 0x16};
    ParseResult r = parse(buf, sizeof(buf));
    EXPECT(r.status == ParseStatus::kBadChecksum);
    EXPECT_STR(parse_status_text(r.status), "BAD_CHECKSUM");
}

static void t06_bad_end() {
    std::printf("T06 结束符错 → BAD_END\n");
    std::uint8_t buf[] = {0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68,
                          0x11, 0x04, 0x33, 0x33, 0x33, 0x33, 0xB1, 0x17};
    ParseResult r = parse(buf, sizeof(buf));
    EXPECT(r.status == ParseStatus::kBadEnd);
}

static void t07_bad_second_start() {
    std::printf("T07 第二个 68 错位 → BAD_SECOND_START\n");
    std::uint8_t buf[] = {0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x69,
                          0x11, 0x04, 0x33, 0x33, 0x33, 0x33, 0xB1, 0x16};
    ParseResult r = parse(buf, sizeof(buf));
    EXPECT(r.status == ParseStatus::kBadSecondStart);
}

static void t08_lead_fe() {
    std::printf("T08 前导 FE 被吃掉且计数正确\n");
    Address a;
    a.from_string("000000000001");
    Frame f;
    f.addr = a;
    f.ctrl = kReadData;
    f.data.assign(4, 0);

    for (int lead = 0; lead <= kMaxLeadFe; ++lead) {
        const std::vector<std::uint8_t> raw = encode(f, lead);
        EXPECT_EQ(raw.size(), 16u + static_cast<std::size_t>(lead));
        for (int i = 0; i < lead; ++i) EXPECT_EQ(raw[static_cast<std::size_t>(i)], 0xFE);

        ParseResult r = parse(raw.data(), raw.size());
        EXPECT(r.status == ParseStatus::kOk);
        EXPECT_EQ(r.lead_fe, lead);
        EXPECT_EQ(r.consumed, raw.size());
        EXPECT_STR(r.frame.addr.to_string(), "000000000001");
        EXPECT_EQ(r.frame.ctrl, 0x11);
    }

    // 超过上限也应按"噪声"吃掉，不报错（现场常见）
    std::vector<std::uint8_t> noisy(6, 0xFE);
    const std::vector<std::uint8_t> body = encode(f, 0);
    noisy.insert(noisy.end(), body.begin(), body.end());
    ParseResult r = parse(noisy.data(), noisy.size());
    EXPECT(r.status == ParseStatus::kOk);
    EXPECT_EQ(r.lead_fe, 6);
}

static void t09_half_frame() {
    std::printf("T09 半帧 → INCOMPLETE（不是错误）\n");
    Address a;
    a.from_string("000000000001");
    Frame f;
    f.addr = a;
    f.ctrl = kReadData;
    f.data.assign(4, 0);
    const std::vector<std::uint8_t> raw = encode(f, 0);

    // 逐字节截断：1..n-1 长度都必须报 INCOMPLETE，不能报错、也不能报 OK
    for (std::size_t cut = 1; cut < raw.size(); ++cut) {
        ParseResult r = parse(raw.data(), cut);
        EXPECT(r.status == ParseStatus::kIncomplete);
    }

    // 长度字段被改大 → 同样 INCOMPLETE（等更多字节），不是 BAD_LENGTH
    std::vector<std::uint8_t> longer = raw;
    longer[9] = 0x08;
    ParseResult r2 = parse(longer.data(), longer.size());
    EXPECT(r2.status == ParseStatus::kIncomplete);
}

// =====================================================================
// T10 +0x33 边界
// =====================================================================
static void t10_pad_boundary() {
    std::printf("T10 +0x33 偏移边界\n");
    Address a;
    a.from_string("000000000001");
    Frame f;
    f.addr = a;
    f.ctrl = kReadData;

    // 0xCC 是能表达的上限：0xCC + 0x33 = 0xFF
    f.data.assign(1, 0xCC);
    std::vector<std::uint8_t> raw = encode(f, 0);
    EXPECT_EQ(raw[10], 0xFF);
    ParseResult r = parse(raw.data(), raw.size());
    EXPECT(r.status == ParseStatus::kOk);
    EXPECT_EQ(r.frame.data[0], 0xCC);

    // 0xCD 越界：uint8 会回绕成 0x00 —— 协议层**无法**表达这个值。
    // ★ 这是一条"已知边界"，写成断言而不是假装不存在：
    //   上层（DI / 数据域）必须保证每字节 ≤ 0xCC。DI 的 4 个字节
    //   都是 0x00~0x02 量级，实际不会触到；但改协议扩展时要记得。
    f.data.assign(1, 0xCD);
    raw = encode(f, 0);
    EXPECT_EQ(raw[10], 0x00);   // 回绕，静默
}

// =====================================================================
// T11 DI 传输序
// =====================================================================
static void t11_di_wire_order() {
    std::printf("T11 DI 传输序（低位在前）\n");
    std::uint8_t w[4];
    di_to_wire(0x02010100u, w);
    EXPECT_EQ(w[0], 0x00);
    EXPECT_EQ(w[1], 0x01);
    EXPECT_EQ(w[2], 0x01);
    EXPECT_EQ(w[3], 0x02);
    EXPECT_EQ(wire_to_di(w), 0x02010100u);

    di_to_wire(0x00010000u, w);
    EXPECT_EQ(w[0], 0x00);
    EXPECT_EQ(w[1], 0x00);
    EXPECT_EQ(w[2], 0x01);
    EXPECT_EQ(w[3], 0x00);
    EXPECT_EQ(wire_to_di(w), 0x00010000u);
}

// =====================================================================
// T12 encode → parse 往返
// =====================================================================
static void t12_roundtrip() {
    std::printf("T12 encode → parse 往返\n");
    Address a;
    a.from_string("000000000042");

    Frame f;
    f.addr = a;
    f.ctrl = kReadDataResp;
    for (int i = 0; i < 12; ++i) f.data.push_back(static_cast<std::uint8_t>(i * 7 % 0xCC));

    for (int lead = 0; lead <= 3; ++lead) {
        const std::vector<std::uint8_t> raw = encode(f, lead);
        ParseResult r = parse(raw.data(), raw.size());
        EXPECT(r.status == ParseStatus::kOk);
        EXPECT_EQ(r.frame.ctrl, f.ctrl);
        EXPECT_STR(r.frame.addr.to_string(), "000000000042");
        EXPECT_EQ(r.frame.data.size(), f.data.size());
        for (std::size_t i = 0; i < f.data.size(); ++i) {
            EXPECT_EQ(r.frame.data[i], f.data[i]);
        }
    }
}

// =====================================================================
// T13 ~ T16 BCD 解码
// =====================================================================
static void t13_bcd_voltage() {
    std::printf("T13 BCD 电压 220.5 V\n");
    const DiSpec* s = find_builtin(0x02010100u);
    EXPECT(s != nullptr);
    if (!s) return;
    EXPECT_EQ(s->bytes, 2);
    EXPECT_EQ(s->decimals, 1);

    // 2205 = 0x2205，低位在前 → 05 22
    const std::uint8_t p[] = {0x05, 0x22};
    DecodeResult d = decode_bcd(*s, p, 2);
    EXPECT(d.ok);
    EXPECT_NEAR(d.raw, 220.5, 1e-9);
    EXPECT_NEAR(d.value, 220.5, 1e-9);

    // 0 V 也必须解得出（合法值，不是错误）
    const std::uint8_t z[] = {0x00, 0x00};
    DecodeResult dz = decode_bcd(*s, z, 2);
    EXPECT(dz.ok);
    EXPECT_NEAR(dz.value, 0.0, 1e-12);
}

static void t14_bcd_current() {
    std::printf("T14 BCD 电流（3 位小数）\n");
    const DiSpec* s = find_builtin(0x02020100u);
    EXPECT(s != nullptr);
    if (!s) return;
    EXPECT_EQ(s->bytes, 3);

    // 123.456 A → 123456 = 12|34|56，低位在前 → 56 34 12
    // ★ 这里踩过一次：我最初写成 45 23 01（那是 12345 的编码，少一位），
    //   解出来是 12.345 —— 差 10 倍却"看起来像个正常电流"。
    //   正是手写期望值把这条抓出来的。
    const std::uint8_t p[] = {0x56, 0x34, 0x12};
    DecodeResult d = decode_bcd(*s, p, 3);
    EXPECT(d.ok);
    EXPECT_NEAR(d.raw, 123.456, 1e-9);
}

static void t15_bcd_power_positive() {
    std::printf("T15 BCD 功率正值\n");
    const DiSpec* s = find_builtin(0x02030000u);
    EXPECT(s != nullptr);
    if (!s) return;
    EXPECT(s->sign_bit);

    // 1.2345 kW → 12345 → 45 23 01
    const std::uint8_t p[] = {0x45, 0x23, 0x01};
    DecodeResult d = decode_bcd(*s, p, 3);
    EXPECT(d.ok);
    EXPECT_NEAR(d.raw, 1.2345, 1e-9);
}

static void t16_bcd_power_negative() {
    std::printf("T16 BCD 功率负值（符号位在最高字节 bit7）\n");
    const DiSpec* s = find_builtin(0x02030000u);
    EXPECT(s != nullptr);
    if (!s) return;

    // -1.2345 → 最高字节 0x01 | 0x80 = 0x81
    const std::uint8_t p[] = {0x45, 0x23, 0x81};
    DecodeResult d = decode_bcd(*s, p, 3);
    EXPECT(d.ok);
    EXPECT_NEAR(d.raw, -1.2345, 1e-9);

    // ★ 反向守卫：如果不剥离符号位，结果会是一个完全不同的正数
    const std::uint8_t q[] = {0x45, 0x23, 0x01};
    DecodeResult dp = decode_bcd(*s, q, 3);
    EXPECT(dp.ok);
    EXPECT(std::fabs(dp.raw - d.raw) > 1.0);   // 两条路径确实不同
}

// =====================================================================
// T17 BCD 半字节越界（核心负向）
// =====================================================================
static void t17_bcd_invalid_nibble() {
    std::printf("T17 BCD 半字节越界 → 拒绝\n");
    const DiSpec* s = find_builtin(0x02010100u);
    if (!s) { EXPECT(false); return; }

    // 低半字节 0x0F > 9
    const std::uint8_t p1[] = {0x0F, 0x22};
    DecodeResult d1 = decode_bcd(*s, p1, 2);
    EXPECT(!d1.ok);
    EXPECT(std::strlen(d1.why) > 0);

    // 高半字节 0x2A > 9
    const std::uint8_t p2[] = {0x05, 0x2A};
    DecodeResult d2 = decode_bcd(*s, p2, 2);
    EXPECT(!d2.ok);

    // ★ 反向守卫：若不做半字节校验，0x0F 会被算成 15 →
    //   值从 220.5 变成 661.5，而**没有任何一处会报错**
    const std::uint8_t ok[] = {0x05, 0x22};
    DecodeResult dok = decode_bcd(*s, ok, 2);
    EXPECT(dok.ok);
}

// =====================================================================
// T18 长度不符
// =====================================================================
static void t18_length_mismatch() {
    std::printf("T18 数据长度与 DI 声明不符 → 拒绝\n");
    const DiSpec* s = find_builtin(0x02010100u);   // 2 字节
    if (!s) { EXPECT(false); return; }

    const std::uint8_t p[] = {0x05, 0x22, 0x00};
    DecodeResult d = decode_bcd(*s, p, 3);
    EXPECT(!d.ok);

    // ★ 为什么这条重要：若"有多少解多少"，把电流的 3 字节响应当电压解，
    //   前 2 字节照样是合法 BCD，会解出一个**看起来正常**的数。
    DecodeResult d2 = decode_bcd(*s, p, 1);
    EXPECT(!d2.ok);
}

// =====================================================================
// T19 encode_bcd 对称
// =====================================================================
static void t19_encode_roundtrip() {
    std::printf("T19 encode_bcd 与 decode_bcd 对称\n");
    struct Case { std::uint32_t di; double v; };
    const Case cases[] = {
        {0x02010100u, 220.5},
        {0x02010200u, 0.0},
        {0x02010300u, 380.0},
        {0x02020100u, 123.456},
        {0x02030000u, 1.2345},
        {0x02030000u, -1.2345},
        {0x02060000u, -0.987},
        {0x02800002u, 50.01},
        {0x00010000u, 12345.67},
    };
    for (const Case& c : cases) {
        const DiSpec* s = find_builtin(c.di);
        if (!s) { EXPECT(false); continue; }
        std::vector<std::uint8_t> body;
        const char* why = nullptr;
        const bool enc = encode_bcd(*s, c.v, Scale(), &body, &why);
        EXPECT(enc);
        if (!enc) {
            std::printf("    %08X 编码失败: %s\n", c.di, why ? why : "?");
            continue;
        }
        DecodeResult d = decode_bcd(*s, body.data(), body.size());
        EXPECT(d.ok);
        EXPECT_NEAR(d.raw, c.v, 1e-9);
    }
}

// =====================================================================
// T20 表示范围
// =====================================================================
static void t20_range_overflow() {
    std::printf("T20 超出该 DI 表示范围 → 拒绝\n");
    const DiSpec* s = find_builtin(0x02030000u);   // 3 字节，带符号 → 5 位有效
    if (!s) { EXPECT(false); return; }

    std::vector<std::uint8_t> body;
    const char* why = nullptr;
    EXPECT(!encode_bcd(*s, 12.3456, Scale(), &body, &why));   // 123456 六位，超
    EXPECT(why != nullptr && std::strlen(why) > 0);

    // 负值给不带符号位的 DI 也必须拒
    const DiSpec* v = find_builtin(0x02010100u);
    if (v) {
        EXPECT(!encode_bcd(*v, -1.0, Scale(), &body, &why));
    }
}

// =====================================================================
// T21 变比
// =====================================================================
static void t21_scale() {
    std::printf("T21 变比换算\n");
    const DiSpec* s = find_builtin(0x02010100u);
    if (!s) { EXPECT(false); return; }

    Scale sc;
    sc.mul = 40.0;   // PT 变比 40:1
    sc.add = 0.0;

    const std::uint8_t p[] = {0x05, 0x22};   // 220.5
    DecodeResult d = decode_bcd(*s, p, 2, sc);
    EXPECT(d.ok);
    EXPECT_NEAR(d.raw, 220.5, 1e-9);      // raw 不受变比影响
    EXPECT_NEAR(d.value, 8820.0, 1e-9);   // 220.5 × 40

    // ★ raw 与 value 必须分开给：现场核对时先看 raw（表上显示的就是它），
    //   再看 value（进算法的是它）。合成一个数会让"变比配错"无从判断。
    EXPECT(std::fabs(d.raw - d.value) > 1e-6);
}

// =====================================================================
// T22 内置 DI 表
// =====================================================================
static void t22_builtin_table() {
    std::printf("T22 内置 DI 表查找\n");
    std::size_t n = 0;
    builtin_specs(&n);
    EXPECT(n >= 10);

    EXPECT(find_builtin(0x02010100u) != nullptr);
    EXPECT(find_builtin(0x00010000u) != nullptr);
    EXPECT(find_builtin(0xDEADBEEFu) == nullptr);   // 未命中必须是 nullptr

    // 表内不得有重复 DI（重复会让 find 的结果取决于顺序）
    const DiSpec* p = builtin_specs(&n);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            if (p[i].di == p[j].di) {
                std::printf("    重复 DI: %08X\n", p[i].di);
                EXPECT(false);
            }
        }
    }
    // 每个项的字节数与小数位必须自洽（1~4 字节，0~4 位小数）
    for (std::size_t i = 0; i < n; ++i) {
        EXPECT(p[i].bytes >= 1 && p[i].bytes <= 4);
        EXPECT(p[i].decimals >= 0 && p[i].decimals <= 4);
        EXPECT(std::strlen(p[i].name) > 0);
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    t01_address_roundtrip();
    t02_address_invalid();
    t03_golden_all_zero();
    t04_golden_read_voltage();
    t05_bad_checksum();
    t06_bad_end();
    t07_bad_second_start();
    t08_lead_fe();
    t09_half_frame();
    t10_pad_boundary();
    t11_di_wire_order();
    t12_roundtrip();
    t13_bcd_voltage();
    t14_bcd_current();
    t15_bcd_power_positive();
    t16_bcd_power_negative();
    t17_bcd_invalid_nibble();
    t18_length_mismatch();
    t19_encode_roundtrip();
    t20_range_overflow();
    t21_scale();
    t22_builtin_table();

    std::printf("\nPASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
