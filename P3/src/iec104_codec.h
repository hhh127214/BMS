// =====================================================================
// P3/ — 产品化 P3（通信）：IEC 60870-5-104 编解码（纯函数，无状态）
//
// 定位：把「104 报文」的字节布局收在一个文件里，与「谁在收发」解耦。
//   上层（Iec104DeviceIO / 被控站仿真 / 真实 TCP 传输）只调用本文件的纯函数，
//   因此帧格式可以用**逐字节断言**验证（T41/T42），不需要起 socket。
//
// 104 与 Modbus 最大的不同（也是现场最容易踩坑的地方）：
//   1. **序号是协议状态机的一部分**。I 帧带 N(S)/N(R)，k=12 / w=8 是窗口约束。
//      序号错位不会立刻报错，而是表现为"对端在若干帧之后突然不认账"。
//   2. **字节序与 Modbus 相反**。104 的 IOA(3 字节)、CA(2 字节)、
//      短浮点(4 字节)、COT 都是**低字节在前**（little-endian）。
//      从 Modbus 迁过来的人几乎必踩这一脚，所以本文件每个多字节量都单独
//      写注释标注字节序。
//   3. **U 帧是"没有序号的控制帧"**：STARTDT / STOPDT / TESTFR，
//      每种都有 act(请求) 与 con(确认) 两个方向，靠一个字节区分。
//
// 覆盖的 ASDU 子集（够与主流调度主站/网关互操作的最小集合）：
//   类型 100 C_IC_NA_1  总召唤（EMS 侧发起）
//   类型   1 M_SP_NA_1  单点信息（STA 位，SQ=1 批量）
//   类型  13 M_ME_NC_1  短浮点量测（MEAS/CFG，SQ=1 批量）
//   类型  50 C_SE_NC_1  短浮点设定值（功率遥调，带 S/E 选择-执行）
//   类型  45 C_SC_NA_1  单点命令
//   类型  46 C_DC_NA_1  双点命令
//   类型  70 M_EI_NA_1  初始化结束
//   类型 200            **本项目私有**：宽精度量测（IOA+IEEE754 double+QDS）
//                       标准集里没有 64 bit 浮点。私有类型只用于
//                       「链路本身不改变算法行为」的逐位等价验收，现场不使用。
//                       现场要带时标请用标准类型 36 M_ME_TF_1（float32 + CP56Time2a）。
//
// 编译：纯头文件，无需单独编译。
// =====================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ems {
namespace iec104 {

// ---------------------------------------------------------------------
// 帧型 / U 帧功能
// ---------------------------------------------------------------------
enum FrameKind { kFrameI = 0, kFrameS = 1, kFrameU = 2 };

enum UFunc : uint8_t {
    kStartDtAct = 0x07,
    kStartDtCon = 0x0B,
    kStopDtAct  = 0x13,
    kStopDtCon  = 0x23,
    kTestFrAct  = 0x43,
    kTestFrCon  = 0x83,
};

inline const char* ufunc_name(uint8_t f) {
    switch (f) {
        case kStartDtAct: return "STARTDT act";
        case kStartDtCon: return "STARTDT con";
        case kStopDtAct:  return "STOPDT act";
        case kStopDtCon:  return "STOPDT con";
        case kTestFrAct:  return "TESTFR act";
        case kTestFrCon:  return "TESTFR con";
        default:          return "U?";
    }
}

// ---------------------------------------------------------------------
// ASDU 类型标识 / 传送原因
// ---------------------------------------------------------------------
enum TypeId : uint8_t {
    kM_SP_NA_1 = 1,
    kM_ME_NC_1 = 13,
    kC_SC_NA_1 = 45,
    kC_DC_NA_1 = 46,
    kC_SE_NC_1 = 50,
    kM_EI_NA_1 = 70,
    kC_IC_NA_1 = 100,
    kM_ME_WIDE = 200,   // 本项目私有宽精度（见文件头）
};

inline const char* type_name(uint8_t t) {
    switch (t) {
        case kM_SP_NA_1: return "M_SP_NA_1(1)";
        case kM_ME_NC_1: return "M_ME_NC_1(13)";
        case kC_SC_NA_1: return "C_SC_NA_1(45)";
        case kC_DC_NA_1: return "C_DC_NA_1(46)";
        case kC_SE_NC_1: return "C_SE_NC_1(50)";
        case kM_EI_NA_1: return "M_EI_NA_1(70)";
        case kC_IC_NA_1: return "C_IC_NA_1(100)";
        case kM_ME_WIDE: return "M_ME_WIDE(200, private)";
        default:         return "unknown";
    }
}

enum Cause : uint8_t {
    kCOT_Per       = 1,    // 周期/循环
    kCOT_Spont     = 3,    // 突发
    kCOT_Init      = 4,    // 初始化
    kCOT_Req       = 5,    // 请求/被请求
    kCOT_Act       = 6,    // 激活
    kCOT_ActCon    = 7,    // 激活确认
    kCOT_Deact     = 8,
    kCOT_DeactCon  = 9,
    kCOT_ActTerm   = 10,   // 激活终止
    kCOT_Introgen  = 20,   // 响应站召唤
};

inline const char* cause_name(uint8_t c) {
    switch (c) {
        case kCOT_Per:      return "PER";
        case kCOT_Spont:    return "SPONT";
        case kCOT_Init:     return "INIT";
        case kCOT_Req:      return "REQ";
        case kCOT_Act:      return "ACT";
        case kCOT_ActCon:   return "ACTCON";
        case kCOT_Deact:    return "DEACT";
        case kCOT_DeactCon: return "DEACTCON";
        case kCOT_ActTerm:  return "ACTTERM";
        case kCOT_Introgen: return "INTROGEN";
        default:            return "COT?";
    }
}

// ---------------------------------------------------------------------
// 品质位（QDS 与 SIQ 的高 4 位相同；低 4 位在 SIQ 里是状态值）
// ---------------------------------------------------------------------
enum Qds : uint8_t {
    kQdsGood      = 0x00,
    kQdsOverflow  = 0x01,   // OV
    kQdsBlocked   = 0x10,   // BL
    kQdsSubstituted = 0x20, // SB
    kQdsNotTopical = 0x40,  // NT
    kQdsInvalid   = 0x80,   // IV
};

inline bool qds_valid(uint8_t q) { return (q & kQdsInvalid) == 0; }
inline uint8_t qds_to_siq(uint8_t qds, bool value) {
    return static_cast<uint8_t>((qds & 0xF0) | (value ? 0x01 : 0x00));
}
inline bool siq_value(uint8_t siq) { return (siq & 0x01) != 0; }
inline uint8_t siq_to_qds(uint8_t siq) { return static_cast<uint8_t>(siq & 0xF0); }

// ---------------------------------------------------------------------
// 命令限定词 QOC/QOS：bit7 = S/E（1 = 选择，0 = 执行），低 5 位 = 限定词
// ---------------------------------------------------------------------
inline uint8_t make_qoc(bool select, uint8_t ql = 0) {
    return static_cast<uint8_t>((select ? 0x80 : 0x00) | (ql & 0x1F));
}
inline bool qoc_is_select(uint8_t q) { return (q & 0x80) != 0; }

// ---------------------------------------------------------------------
// 长度 / 窗口常量
// ---------------------------------------------------------------------
static constexpr size_t kApciLen   = 6;     // 0x68 + LEN + 4 字节控制域
static constexpr size_t kMaxApdu   = 255;   // 0x68 + LEN(≤253)
static constexpr size_t kMaxAsdu   = 249;   // 253 - 4
static constexpr size_t kAsduHdr   = 6;     // 类型 + VSQ + COT(2) + CA(2)
static constexpr int    kWindowK   = 12;    // 未被确认的 I 帧上限
static constexpr int    kWindowW   = 8;     // 收到 w 个 I 帧必须确认

// ---------------------------------------------------------------------
// 字节序基本读写（**104 一律低字节在前**，与 Modbus 相反）
// ---------------------------------------------------------------------
inline void put_u16_le(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
inline uint16_t get_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 (static_cast<uint16_t>(p[1]) << 8));
}
inline void put_u24_le(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
}
inline uint32_t get_u24_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16);
}
inline void put_f32_le(uint8_t* p, float f) {
    uint32_t b = 0;
    std::memcpy(&b, &f, 4);
    p[0] = static_cast<uint8_t>(b & 0xFF);
    p[1] = static_cast<uint8_t>((b >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((b >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((b >> 24) & 0xFF);
}
inline float get_f32_le(const uint8_t* p) {
    const uint32_t b = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    float f = 0.0f;
    std::memcpy(&f, &b, 4);
    return f;
}
inline void put_f64_le(uint8_t* p, double d) {
    uint64_t b = 0;
    std::memcpy(&b, &d, 8);
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((b >> (8 * i)) & 0xFF);
}
inline double get_f64_le(const uint8_t* p) {
    uint64_t b = 0;
    for (int i = 0; i < 8; ++i) b |= static_cast<uint64_t>(p[i]) << (8 * i);
    double d = 0.0;
    std::memcpy(&d, &b, 8);
    return d;
}

// ---------------------------------------------------------------------
// APCI：三种帧型的组装与解析
// ---------------------------------------------------------------------
inline size_t build_i_apci(uint8_t* out, size_t cap, uint16_t ns, uint16_t nr) {
    if (out == nullptr || cap < 4) return 0;
    ns = static_cast<uint16_t>(ns & 0x7FFF);
    nr = static_cast<uint16_t>(nr & 0x7FFF);
    out[0] = static_cast<uint8_t>((ns << 1) & 0xFE);
    out[1] = static_cast<uint8_t>((ns >> 7) & 0xFF);
    out[2] = static_cast<uint8_t>((nr << 1) & 0xFE);
    out[3] = static_cast<uint8_t>((nr >> 7) & 0xFF);
    return 4;
}
inline size_t build_s_apci(uint8_t* out, size_t cap, uint16_t nr) {
    if (out == nullptr || cap < 4) return 0;
    nr = static_cast<uint16_t>(nr & 0x7FFF);
    out[0] = 0x01;
    out[1] = 0x00;
    out[2] = static_cast<uint8_t>((nr << 1) & 0xFE);
    out[3] = static_cast<uint8_t>((nr >> 7) & 0xFF);
    return 4;
}
inline size_t build_u_apci(uint8_t* out, size_t cap, uint8_t ufunc) {
    if (out == nullptr || cap < 4) return 0;
    out[0] = 0x03;
    out[1] = ufunc;
    out[2] = 0x00;
    out[3] = 0x00;
    return 4;
}

// 整帧组装：0x68 + LEN + 控制域 + ASDU
inline size_t wrap_i_frame(uint8_t* out, size_t cap, uint16_t ns, uint16_t nr,
                           const uint8_t* asdu, size_t asdu_len) {
    if (out == nullptr || asdu == nullptr) return 0;
    if (asdu_len == 0 || asdu_len > kMaxAsdu) return 0;
    const size_t total = kApciLen + asdu_len;
    if (cap < total) return 0;
    out[0] = 0x68;
    out[1] = static_cast<uint8_t>(4 + asdu_len);
    build_i_apci(out + 2, 4, ns, nr);
    std::memcpy(out + kApciLen, asdu, asdu_len);
    return total;
}
inline size_t wrap_s_frame(uint8_t* out, size_t cap, uint16_t nr) {
    if (out == nullptr || cap < kApciLen) return 0;
    out[0] = 0x68;
    out[1] = 4;
    build_s_apci(out + 2, 4, nr);
    return kApciLen;
}
inline size_t wrap_u_frame(uint8_t* out, size_t cap, uint8_t ufunc) {
    if (out == nullptr || cap < kApciLen) return 0;
    out[0] = 0x68;
    out[1] = 4;
    build_u_apci(out + 2, 4, ufunc);
    return kApciLen;
}

// ---------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------
struct ApciHeader {
    FrameKind kind = kFrameI;
    uint16_t  ns   = 0;   // 仅 I 帧有效
    uint16_t  nr   = 0;   // I / S 帧有效
    uint8_t   ufunc = 0;  // 仅 U 帧有效
};

// 解析一个 APDU 的头；返回该 APDU 的总长度，0 = 长度不足或格式非法。
// 注意：TCP 流是**字节流**，一次 read 可能收到半个帧或两个半帧 ——
// 调用方必须用返回值做"攒够再解析"的边界判断（见 Iec104DeviceIO）。
inline size_t parse_apdu(const uint8_t* buf, size_t len, ApciHeader& out) {
    if (buf == nullptr || len < kApciLen) return 0;
    if (buf[0] != 0x68) return 0;
    const size_t len_field = buf[1];
    if (len_field < 4) return 0;
    if (len < 2 + len_field) return 0;

    const uint8_t c0 = buf[2], c1 = buf[3], c2 = buf[4], c3 = buf[5];
    if ((c0 & 0x01) == 0) {
        out.kind = kFrameI;
        out.ns = static_cast<uint16_t>(((c0 >> 1) | (c1 << 7)) & 0x7FFF);
        out.nr = static_cast<uint16_t>(((c2 >> 1) | (c3 << 7)) & 0x7FFF);
        out.ufunc = 0;
    } else if ((c0 & 0x03) == 0x01) {
        out.kind = kFrameS;
        out.ns = 0;
        out.nr = static_cast<uint16_t>(((c2 >> 1) | (c3 << 7)) & 0x7FFF);
        out.ufunc = 0;
    } else {
        out.kind = kFrameU;
        out.ns = 0;
        out.nr = 0;
        out.ufunc = c1;
    }
    return 2 + len_field;
}

struct AsduHeader {
    uint8_t  type_id = 0;
    bool     sq      = false;
    uint8_t  count   = 0;
    uint8_t  cot     = 0;
    bool     pn      = false;   // 否定确认
    bool     test    = false;   // 试验位
    uint8_t  oa      = 0;       // 源发站地址
    uint16_t ca      = 0;       // 公共地址（**小端**）
};

inline size_t write_asdu_header(uint8_t* out, size_t cap, uint8_t type_id, bool sq, uint8_t num,
                                uint8_t cot, bool pn, bool test, uint8_t oa, uint16_t ca) {
    if (out == nullptr || cap < kAsduHdr) return 0;
    out[0] = type_id;
    out[1] = static_cast<uint8_t>((sq ? 0x80 : 0x00) | (num & 0x7F));
    out[2] = static_cast<uint8_t>((cot & 0x3F) | (pn ? 0x40 : 0x00) | (test ? 0x80 : 0x00));
    out[3] = oa;
    put_u16_le(out + 4, ca);
    return kAsduHdr;
}

inline size_t read_asdu_header(const uint8_t* p, size_t len, AsduHeader& h) {
    if (p == nullptr || len < kAsduHdr) return 0;
    h.type_id = p[0];
    h.sq      = (p[1] & 0x80) != 0;
    h.count   = static_cast<uint8_t>(p[1] & 0x7F);
    h.cot     = static_cast<uint8_t>(p[2] & 0x3F);
    h.pn      = (p[2] & 0x40) != 0;
    h.test    = (p[2] & 0x80) != 0;
    h.oa      = p[3];
    h.ca      = get_u16_le(p + 4);
    return kAsduHdr;
}

struct AsduView {
    AsduHeader     h{};
    const uint8_t* info = nullptr;   // 信息对象区起点
    size_t         info_len = 0;
};

// 从 I 帧里取出 ASDU
inline bool parse_asdu(const uint8_t* apdu, size_t apdu_len, const ApciHeader& apci,
                       AsduView& out) {
    if (apci.kind != kFrameI) return false;
    if (apdu == nullptr || apdu_len < kApciLen + kAsduHdr) return false;
    const uint8_t* p   = apdu + kApciLen;
    const size_t   len = apdu_len - kApciLen;
    if (read_asdu_header(p, len, out.h) == 0) return false;
    out.info     = p + kAsduHdr;
    out.info_len = len - kAsduHdr;
    return true;
}

// ---------------------------------------------------------------------
// 信息对象
// ---------------------------------------------------------------------

// 信息对象的字节数（SQ=1 时 IOA 只在开头出现一次，故对象本身不含 IOA）
static constexpr size_t kObjMeasure  = 5;   // float32(4) + QDS(1)
static constexpr size_t kObjSingle   = 1;   // SIQ(1)
static constexpr size_t kObjWide     = 9;   // float64(8) + QDS(1)
static constexpr size_t kObjSetpoint = 8;   // IOA(3) + float32(4) + QOS(1)，SQ=0

struct InfoFloat {
    uint32_t ioa   = 0;
    double   value = 0.0;
    uint8_t  qds   = kQdsGood;
};

struct InfoSingle {
    uint32_t ioa   = 0;
    bool     value = false;
    uint8_t  qds   = kQdsGood;
};

// 类型 13 M_ME_NC_1，SQ=1（批量，IOA 连续）。返回 ASDU 长度。
inline size_t make_asdu_measurements(uint8_t* out, size_t cap, uint8_t cot, uint16_t ca,
                                     const InfoFloat* items, int n) {
    if (out == nullptr || items == nullptr || n <= 0 || n > 127) return 0;
    const size_t total = kAsduHdr + 3 + static_cast<size_t>(n) * kObjMeasure;
    if (cap < total) return 0;
    write_asdu_header(out, cap, kM_ME_NC_1, true, static_cast<uint8_t>(n), cot, false, false, 0, ca);
    size_t p = kAsduHdr;
    put_u24_le(out + p, items[0].ioa);
    p += 3;
    for (int i = 0; i < n; ++i) {
        put_f32_le(out + p, static_cast<float>(items[i].value));
        out[p + 4] = items[i].qds;
        p += kObjMeasure;
    }
    return p;
}

// 类型 200（私有）宽精度，SQ=1
inline size_t make_asdu_wide(uint8_t* out, size_t cap, uint8_t cot, uint16_t ca,
                             const InfoFloat* items, int n) {
    if (out == nullptr || items == nullptr || n <= 0 || n > 127) return 0;
    const size_t total = kAsduHdr + 3 + static_cast<size_t>(n) * kObjWide;
    if (cap < total) return 0;
    write_asdu_header(out, cap, kM_ME_WIDE, true, static_cast<uint8_t>(n), cot, false, false, 0, ca);
    size_t p = kAsduHdr;
    put_u24_le(out + p, items[0].ioa);
    p += 3;
    for (int i = 0; i < n; ++i) {
        put_f64_le(out + p, items[i].value);
        out[p + 8] = items[i].qds;
        p += kObjWide;
    }
    return p;
}

// 类型 1 M_SP_NA_1，SQ=1
inline size_t make_asdu_single_points(uint8_t* out, size_t cap, uint8_t cot, uint16_t ca,
                                      const InfoSingle* items, int n) {
    if (out == nullptr || items == nullptr || n <= 0 || n > 127) return 0;
    const size_t total = kAsduHdr + 3 + static_cast<size_t>(n) * kObjSingle;
    if (cap < total) return 0;
    write_asdu_header(out, cap, kM_SP_NA_1, true, static_cast<uint8_t>(n), cot, false, false, 0, ca);
    size_t p = kAsduHdr;
    put_u24_le(out + p, items[0].ioa);
    p += 3;
    for (int i = 0; i < n; ++i) {
        out[p] = qds_to_siq(items[i].qds, items[i].value);
        p += kObjSingle;
    }
    return p;
}

// 类型 50 C_SE_NC_1：IOA(3,LE) + float32(4,LE) + QOS(1) = 8 字节（SQ=0）
inline size_t make_asdu_setpoint_float(uint8_t* out, size_t cap, uint8_t cot, uint16_t ca,
                                       uint32_t ioa, double value, uint8_t qos) {
    const size_t total = kAsduHdr + kObjSetpoint;
    if (out == nullptr || cap < total) return 0;
    write_asdu_header(out, cap, kC_SE_NC_1, false, 1, cot, false, false, 0, ca);
    put_u24_le(out + kAsduHdr, ioa);
    put_f32_le(out + kAsduHdr + 3, static_cast<float>(value));
    out[kAsduHdr + 7] = qos;
    return total;
}

// 类型 45 C_SC_NA_1：IOA(3) + SCO(1)
inline size_t make_asdu_single_command(uint8_t* out, size_t cap, uint8_t cot, uint16_t ca,
                                       uint32_t ioa, bool on, bool select) {
    const size_t total = kAsduHdr + 4;
    if (out == nullptr || cap < total) return 0;
    write_asdu_header(out, cap, kC_SC_NA_1, false, 1, cot, false, false, 0, ca);
    put_u24_le(out + kAsduHdr, ioa);
    // SCO: bit0 = SCS(合/分), bit7 = S/E
    out[kAsduHdr + 3] = static_cast<uint8_t>((on ? 0x01 : 0x00) | (select ? 0x80 : 0x00));
    return total;
}

// 类型 46 C_DC_NA_1：IOA(3) + DCO(1)。DCO 低 2 位：1=分, 2=合
inline size_t make_asdu_double_command(uint8_t* out, size_t cap, uint8_t cot, uint16_t ca,
                                       uint32_t ioa, uint8_t dcs, bool select) {
    const size_t total = kAsduHdr + 4;
    if (out == nullptr || cap < total) return 0;
    write_asdu_header(out, cap, kC_DC_NA_1, false, 1, cot, false, false, 0, ca);
    put_u24_le(out + kAsduHdr, ioa);
    out[kAsduHdr + 3] = static_cast<uint8_t>((dcs & 0x03) | (select ? 0x80 : 0x00));
    return total;
}

// 类型 100 C_IC_NA_1：IOA(3) + QOI(1)。QOI=20 = 站召唤
inline size_t make_asdu_general_interrogation(uint8_t* out, size_t cap, uint8_t cot, uint16_t ca,
                                              uint8_t qoi = 20) {
    const size_t total = kAsduHdr + 4;
    if (out == nullptr || cap < total) return 0;
    write_asdu_header(out, cap, kC_IC_NA_1, false, 1, cot, false, false, 0, ca);
    put_u24_le(out + kAsduHdr, 0);
    out[kAsduHdr + 3] = qoi;
    return total;
}

// 类型 70 M_EI_NA_1：IOA(3)=0 + COI(1)
inline size_t make_asdu_init_end(uint8_t* out, size_t cap, uint16_t ca, uint8_t coi = 0) {
    const size_t total = kAsduHdr + 4;
    if (out == nullptr || cap < total) return 0;
    write_asdu_header(out, cap, kM_EI_NA_1, false, 1, kCOT_Init, false, false, 0, ca);
    put_u24_le(out + kAsduHdr, 0);
    out[kAsduHdr + 3] = coi;
    return total;
}

// ---------------------------------------------------------------------
// 信息对象解码
// ---------------------------------------------------------------------
inline bool decode_measurements(const AsduView& v, std::vector<InfoFloat>& out) {
    out.clear();
    if (v.h.type_id != kM_ME_NC_1 || !v.h.sq) return false;
    const size_t need = 3 + static_cast<size_t>(v.h.count) * kObjMeasure;
    if (v.info_len < need) return false;
    const uint32_t base = get_u24_le(v.info);
    size_t p = 3;
    for (uint8_t i = 0; i < v.h.count; ++i) {
        InfoFloat f;
        f.ioa   = base + i;
        f.value = static_cast<double>(get_f32_le(v.info + p));
        f.qds   = v.info[p + 4];
        out.push_back(f);
        p += kObjMeasure;
    }
    return true;
}

inline bool decode_wide(const AsduView& v, std::vector<InfoFloat>& out) {
    out.clear();
    if (v.h.type_id != kM_ME_WIDE || !v.h.sq) return false;
    const size_t need = 3 + static_cast<size_t>(v.h.count) * kObjWide;
    if (v.info_len < need) return false;
    const uint32_t base = get_u24_le(v.info);
    size_t p = 3;
    for (uint8_t i = 0; i < v.h.count; ++i) {
        InfoFloat f;
        f.ioa   = base + i;
        f.value = get_f64_le(v.info + p);
        f.qds   = v.info[p + 8];
        out.push_back(f);
        p += kObjWide;
    }
    return true;
}

inline bool decode_single_points(const AsduView& v, std::vector<InfoSingle>& out) {
    out.clear();
    if (v.h.type_id != kM_SP_NA_1 || !v.h.sq) return false;
    const size_t need = 3 + static_cast<size_t>(v.h.count) * kObjSingle;
    if (v.info_len < need) return false;
    const uint32_t base = get_u24_le(v.info);
    for (uint8_t i = 0; i < v.h.count; ++i) {
        InfoSingle s;
        s.ioa   = base + i;
        s.value = siq_value(v.info[3 + i]);
        s.qds   = siq_to_qds(v.info[3 + i]);
        out.push_back(s);
    }
    return true;
}

inline bool decode_setpoint_float(const AsduView& v, uint32_t* ioa, double* value, uint8_t* qos) {
    if (v.h.type_id != kC_SE_NC_1 && v.h.type_id != kC_SC_NA_1 && v.h.type_id != kC_DC_NA_1) {
        return false;
    }
    if (v.info_len < 4) return false;
    if (ioa != nullptr) *ioa = get_u24_le(v.info);
    if (v.h.type_id == kC_SE_NC_1) {
        if (v.info_len < kObjSetpoint) return false;
        if (value != nullptr) *value = static_cast<double>(get_f32_le(v.info + 3));
        if (qos != nullptr) *qos = v.info[7];
    } else {
        if (value != nullptr) *value = 0.0;
        if (qos != nullptr) *qos = v.info[3];
    }
    return true;
}

inline bool decode_general_interrogation(const AsduView& v, uint8_t* qoi) {
    if (v.h.type_id != kC_IC_NA_1) return false;
    if (v.info_len < 4) return false;
    if (qoi != nullptr) *qoi = v.info[3];
    return true;
}

// ASDU 长度自洽校验：SQ=1 时按 count 算出的长度必须与 info_len 相符。
// 未知类型不做长度判断（它可能带时标等扩展），由调用方按 unknown_type
// 记数并丢弃 —— 把"不认识"和"格式错"混成一类会让现场排障丢掉关键信息。
inline bool asdu_length_ok(const AsduView& v) {
    switch (v.h.type_id) {
        case kM_ME_NC_1:
            return !v.h.sq || v.info_len == 3 + static_cast<size_t>(v.h.count) * kObjMeasure;
        case kM_ME_WIDE:
            return !v.h.sq || v.info_len == 3 + static_cast<size_t>(v.h.count) * kObjWide;
        case kM_SP_NA_1:
            return !v.h.sq || v.info_len == 3 + static_cast<size_t>(v.h.count) * kObjSingle;
        case kC_SE_NC_1:
            return !v.h.sq && v.info_len == kObjSetpoint;
        case kC_SC_NA_1:
        case kC_DC_NA_1:
        case kC_IC_NA_1:
        case kM_EI_NA_1:
            return !v.h.sq && v.info_len == 4;
        default:
            return true;   // 未知类型：本函数不表态
    }
}

} // namespace iec104
} // namespace ems
