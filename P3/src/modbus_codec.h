// =====================================================================
// P3/ — 产品化 P3（通信）：Modbus 编解码（纯函数，无状态）
//
// 定位：把「Modbus 帧」的字节布局收在一个文件里，与「谁在收发」完全解耦。
//   上层（ModbusDeviceIO / ModbusSlaveSim / 真实 TCP 传输）只调用本文件的
//   纯函数，因此：
//     · 帧格式可以用**逐字节断言**验证（T31），不需要起 socket；
//     · 换 RTU/TCP 只换传输层，编解码一行不改（RTU 复用同一条 PDU）。
//
// 覆盖范围（现场 90% 的场景只用这四个功能码）：
//   0x03 读保持寄存器 / 0x04 读输入寄存器 / 0x06 写单寄存器 / 0x10 写多寄存器
// 异常响应：功能码最高位置 1 + 1 字节异常码（0x80 | fc）。
//
// 为什么「字序」必须在类型里显式声明（现场第一大坑）：
//   Modbus 只规定寄存器是 16 bit 大端，**没有规定 32/64 bit 数值怎么切**。
//   于是同一台设备既有 ABCD（标准）也有 CDAB（"字交换"）两种实现，
//   用错字序的表现是「数值离谱但不报错」—— 比通信中断难查得多。
//   因此本文件把字序编进 RegType（kFloat32BE / kFloat32Swap），
//   寄存器映射表里逐个点声明，禁止在调用点隐式假设。
//
// 编译：纯头文件，无需单独编译。
// =====================================================================

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ems {
namespace modbus {

// ---------------------------------------------------------------------
// 功能码 / 异常码
// ---------------------------------------------------------------------
enum FuncCode : uint8_t {
    kReadHoldingRegs   = 0x03,
    kReadInputRegs     = 0x04,
    kWriteSingleReg    = 0x06,
    kWriteMultipleRegs = 0x10,
};

enum ExceptionCode : uint8_t {
    kExcNone             = 0x00,
    kExcIllegalFunction  = 0x01,
    kExcIllegalDataAddr  = 0x02,
    kExcIllegalDataValue = 0x03,
    kExcSlaveFailure     = 0x04,
};

inline const char* exception_name(ExceptionCode c) {
    switch (c) {
        case kExcNone:             return "NONE";
        case kExcIllegalFunction:  return "ILLEGAL_FUNCTION";
        case kExcIllegalDataAddr:  return "ILLEGAL_DATA_ADDRESS";
        case kExcIllegalDataValue: return "ILLEGAL_DATA_VALUE";
        case kExcSlaveFailure:     return "SLAVE_DEVICE_FAILURE";
        default:                   return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------
// 数值编码类型（字序显式；寄存器宽度由类型决定）
//
//   kFloat64BE —— 4 寄存器宽精度映射。现场设备几乎不用，但本项目的
//     「介质等价性」验收需要它：只有介质能承载 double 的全部 53 位有效位，
//     「换介质不改算法」才可能做到**逐位等价**而不是"差一点点"。
//     浮点量化对控制决策的影响单独由 T34 的量化边界用例度量，
//     两件事分开验，结论才立得住。
// ---------------------------------------------------------------------
enum class RegType {
    kUInt16,       // 1 寄存器 无符号
    kInt16,        // 1 寄存器 补码
    kUInt32BE,     // 2 寄存器 ABCD
    kFloat32BE,    // 2 寄存器 ABCD —— Modbus 标准浮点
    kFloat32Swap,  // 2 寄存器 CDAB —— 现场大量存在的"字交换"写法
    kFloat64BE,    // 4 寄存器 ABCDEFGH —— 宽精度（本项目等价性验收用）
};

inline int reg_width(RegType t) {
    switch (t) {
        case RegType::kUInt16:
        case RegType::kInt16:        return 1;
        case RegType::kFloat64BE:    return 4;
        default:                     return 2;
    }
}

inline const char* reg_type_name(RegType t) {
    switch (t) {
        case RegType::kUInt16:      return "u16";
        case RegType::kInt16:       return "i16";
        case RegType::kUInt32BE:    return "u32/ABCD";
        case RegType::kFloat32BE:   return "f32/ABCD";
        case RegType::kFloat32Swap: return "f32/CDAB";
        case RegType::kFloat64BE:   return "f64/ABCDEFGH";
        default:                    return "?";
    }
}

// ---------------------------------------------------------------------
// 字节序基本读写（Modbus 一律**大端**）
// ---------------------------------------------------------------------
inline void put_u16_be(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}
inline uint16_t get_u16_be(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) |
                                 static_cast<uint16_t>(p[1]));
}

// ---------------------------------------------------------------------
// CRC16（Modbus 标准，多项式 0xA001）—— RTU 帧校验用。
// TCP 帧不带 CRC（由以太网/IP 校验兜底），故本文件只在 RTU 路径使用它。
// 标准自检值：crc16("123456789", 9) == 0x4B37
// ---------------------------------------------------------------------
inline uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc = static_cast<uint16_t>(crc ^ static_cast<uint16_t>(data[i]));
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x0001) crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001);
            else              crc = static_cast<uint16_t>(crc >> 1);
        }
    }
    return crc;
}

// ---------------------------------------------------------------------
// 值 ↔ 寄存器（纯函数；decode(encode(v)) 在类型值域内恒等）
// ---------------------------------------------------------------------
inline void encode_value(double v, RegType t, uint16_t* regs) {
    if (regs == nullptr) return;
    switch (t) {
        case RegType::kUInt16: {
            const int64_t x = static_cast<int64_t>(std::llround(v));
            regs[0] = static_cast<uint16_t>(x & 0xFFFF);
            break;
        }
        case RegType::kInt16: {
            const int32_t x = static_cast<int32_t>(std::llround(v));
            regs[0] = static_cast<uint16_t>(x & 0xFFFF);
            break;
        }
        case RegType::kUInt32BE: {
            const uint32_t x = static_cast<uint32_t>(static_cast<int64_t>(std::llround(v)));
            regs[0] = static_cast<uint16_t>((x >> 16) & 0xFFFF);
            regs[1] = static_cast<uint16_t>(x & 0xFFFF);
            break;
        }
        case RegType::kFloat32BE:
        case RegType::kFloat32Swap: {
            const float f = static_cast<float>(v);
            uint32_t bits = 0;
            std::memcpy(&bits, &f, 4);
            if (t == RegType::kFloat32BE) {          // ABCD
                regs[0] = static_cast<uint16_t>((bits >> 16) & 0xFFFF);
                regs[1] = static_cast<uint16_t>(bits & 0xFFFF);
            } else {                                  // CDAB
                regs[0] = static_cast<uint16_t>(bits & 0xFFFF);
                regs[1] = static_cast<uint16_t>((bits >> 16) & 0xFFFF);
            }
            break;
        }
        case RegType::kFloat64BE: {
            const double d = v;
            uint64_t bits = 0;
            std::memcpy(&bits, &d, 8);
            for (int i = 0; i < 4; ++i) {
                regs[i] = static_cast<uint16_t>((bits >> (48 - 16 * i)) & 0xFFFF);
            }
            break;
        }
    }
}

inline double decode_value(const uint16_t* regs, RegType t) {
    if (regs == nullptr) return 0.0;
    switch (t) {
        case RegType::kUInt16:
            return static_cast<double>(regs[0]);
        case RegType::kInt16:
            return static_cast<double>(static_cast<int16_t>(regs[0]));
        case RegType::kUInt32BE:
            return static_cast<double>((static_cast<uint32_t>(regs[0]) << 16) |
                                       static_cast<uint32_t>(regs[1]));
        case RegType::kFloat32BE:
        case RegType::kFloat32Swap: {
            uint32_t bits = 0;
            if (t == RegType::kFloat32BE) {
                bits = (static_cast<uint32_t>(regs[0]) << 16) | static_cast<uint32_t>(regs[1]);
            } else {
                bits = (static_cast<uint32_t>(regs[1]) << 16) | static_cast<uint32_t>(regs[0]);
            }
            float f = 0.0f;
            std::memcpy(&f, &bits, 4);
            return static_cast<double>(f);
        }
        case RegType::kFloat64BE: {
            uint64_t bits = 0;
            for (int i = 0; i < 4; ++i) {
                bits = (bits << 16) | static_cast<uint64_t>(regs[i]);
            }
            double d = 0.0;
            std::memcpy(&d, &bits, 8);
            return d;
        }
    }
    return 0.0;
}

// ---------------------------------------------------------------------
// 报文长度约束（协议硬限，越界必须报异常而不是截断）
// ---------------------------------------------------------------------
static constexpr size_t kMbapLen          = 7;    // tid2 + proto2 + len2 + unit1
static constexpr size_t kMaxPduLen        = 253;  // TCP 帧 PDU 上限
static constexpr uint16_t kMaxReadRegs    = 125;  // 0x03/0x04 单次读上限
static constexpr uint16_t kMaxWriteRegs   = 123;  // 0x10 单次写上限

// ---------------------------------------------------------------------
// 请求构造
// ---------------------------------------------------------------------

// 读寄存器请求（0x03 / 0x04）
inline size_t build_read_request(uint8_t* out, size_t cap, uint16_t tid, uint8_t unit,
                                 FuncCode fc, uint16_t addr, uint16_t qty) {
    const size_t n = kMbapLen + 5;
    if (out == nullptr || cap < n) return 0;
    put_u16_be(out + 0, tid);
    put_u16_be(out + 2, 0x0000);            // 协议标识固定 0
    put_u16_be(out + 4, 6);                 // 后续字节数 = unit(1) + PDU(5)
    out[6] = unit;
    out[7] = static_cast<uint8_t>(fc);
    put_u16_be(out + 8, addr);
    put_u16_be(out + 10, qty);
    return n;
}

// 写多寄存器请求（0x10）
inline size_t build_write_multiple_request(uint8_t* out, size_t cap, uint16_t tid, uint8_t unit,
                                           uint16_t addr, const uint16_t* regs, uint16_t qty) {
    if (out == nullptr || regs == nullptr || qty == 0) return 0;
    const size_t byte_count = static_cast<size_t>(qty) * 2;
    const size_t n = kMbapLen + 6 + byte_count;
    if (cap < n) return 0;
    put_u16_be(out + 0, tid);
    put_u16_be(out + 2, 0x0000);
    put_u16_be(out + 4, static_cast<uint16_t>(1 + 6 + byte_count));
    out[6] = unit;
    out[7] = static_cast<uint8_t>(kWriteMultipleRegs);
    put_u16_be(out + 8, addr);
    put_u16_be(out + 10, qty);
    out[12] = static_cast<uint8_t>(byte_count);
    for (size_t i = 0; i < qty; ++i) put_u16_be(out + 13 + i * 2, regs[i]);
    return n;
}

// 写单寄存器请求（0x06）
inline size_t build_write_single_request(uint8_t* out, size_t cap, uint16_t tid, uint8_t unit,
                                         uint16_t addr, uint16_t value) {
    const size_t n = kMbapLen + 5;
    if (out == nullptr || cap < n) return 0;
    put_u16_be(out + 0, tid);
    put_u16_be(out + 2, 0x0000);
    put_u16_be(out + 4, 6);
    out[6] = unit;
    out[7] = static_cast<uint8_t>(kWriteSingleReg);
    put_u16_be(out + 8, addr);
    put_u16_be(out + 10, value);
    return n;
}

// ---------------------------------------------------------------------
// 响应解析
// ---------------------------------------------------------------------
struct MbapHeader {
    uint16_t tid   = 0;
    uint16_t proto = 0;
    uint16_t len   = 0;   // 其后字节数（unit + PDU）
    uint8_t  unit  = 0;
};

struct PduView {
    uint8_t        fc  = 0;      // 原始功能码（异常响应时最高位为 1）
    const uint8_t* data = nullptr;
    size_t         len  = 0;     // 功能码之后的字节数
};

// 解析 MBAP 头并做长度自洽校验（len 字段与实际字节数必须一致）
inline bool parse_mbap(const uint8_t* buf, size_t len, MbapHeader& h) {
    if (buf == nullptr || len < kMbapLen) return false;
    h.tid   = get_u16_be(buf + 0);
    h.proto = get_u16_be(buf + 2);
    h.len   = get_u16_be(buf + 4);
    h.unit  = buf[6];
    if (h.proto != 0x0000) return false;                  // Modbus TCP 固定 0
    if (len < static_cast<size_t>(6) + h.len) return false;
    if (h.len < 2) return false;                          // 至少 fc + 1 字节
    return true;
}

inline bool parse_response(const uint8_t* buf, size_t len, MbapHeader& h, PduView& pdu) {
    if (!parse_mbap(buf, len, h)) return false;
    const size_t pdu_len = static_cast<size_t>(h.len) - 1;   // 扣掉 unit
    if (pdu_len < 1) return false;
    pdu.fc   = buf[kMbapLen];
    pdu.data = buf + kMbapLen + 1;
    pdu.len  = pdu_len - 1;
    return true;
}

// 是否异常响应；是则取出异常码
inline bool is_exception(const PduView& pdu, ExceptionCode& code) {
    if ((pdu.fc & 0x80) == 0) return false;
    if (pdu.len < 1) return false;
    code = static_cast<ExceptionCode>(pdu.data[0]);
    return true;
}

// 异常响应的构造（从站侧 / 测试桩用）
inline size_t build_exception_response(uint8_t* out, size_t cap, const MbapHeader& req,
                                       uint8_t fc, ExceptionCode code) {
    const size_t n = kMbapLen + 2;
    if (out == nullptr || cap < n) return 0;
    put_u16_be(out + 0, req.tid);
    put_u16_be(out + 2, 0x0000);
    put_u16_be(out + 4, 3);                  // unit(1) + fc(1) + code(1)
    out[6] = req.unit;
    out[7] = static_cast<uint8_t>(fc | 0x80);
    out[8] = static_cast<uint8_t>(code);
    return n;
}

// 读响应体：把 PDU 数据段还原成寄存器数组。返回解出的寄存器个数，0 = 格式非法。
inline uint16_t parse_read_response_regs(const PduView& pdu, uint16_t expected_qty,
                                         uint16_t* out_regs) {
    if ((pdu.fc & 0x80) != 0) return 0;
    if (pdu.len < 1) return 0;
    const uint16_t byte_count = pdu.data[0];
    if (byte_count != static_cast<uint16_t>(expected_qty * 2)) return 0;
    if (pdu.len < static_cast<size_t>(1) + byte_count) return 0;
    if (out_regs == nullptr) return expected_qty;
    for (uint16_t i = 0; i < expected_qty; ++i) {
        out_regs[i] = get_u16_be(pdu.data + 1 + i * 2);
    }
    return expected_qty;
}

} // namespace modbus
} // namespace ems
