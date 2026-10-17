// =====================================================================
// 21/ — DL/T 645-2007 帧层（组帧 / 解析 / 校验）
//
// 这一层只做**字节**：地址怎么排、数据域怎么加 0x33、校验和怎么算。
// 不做任何工程量换算（那是 dlt645_codec.h 的事），也不碰 socket / 串口
// （那是 dlt645_transport.h 的事）。
//
// ---------------------------------------------------------------------
// ★ 为什么要把"显示序"和"传输序"分开写进类型里
// ---------------------------------------------------------------------
// 645 最坑的一点是**同一个方向反不反，要看字段**：
//   · 6 字节表地址：**低位在前**（"000000000001" → 01 00 00 00 00 00）
//   · 4 字节数据标识 DI：**低位在前**（0x02010100 → 00 01 01 02）
//   · 数据域本身（BCD）：**低位在前**
//   · 校验和 CS：按**发送顺序**累加
//
// 而界面上、文档里、DB 里写的一定是"高位在前"的自然写法。
// 于是在边界上必然要做一次反转 —— 而**反两次等于没反**，这种 bug
// 在读数上表现为"电压 220.5 变成 5.22"，看着像设备坏了。
//
// 本文件的约定（**改代码前先读这一段**）：
//   · 所有对外暴露的整数（地址字符串、DI 常量）一律是**显示序**；
//   · 反转只发生在 encode() / parse() **内部**，且各只有一处；
//   · 上层**永远不要**再自己 reverse 一次。
//
// 另一条度量上的事实：DI 用 uint32_t 承载 0x02010100 这种"BCD 形状"的数，
// 它不是算术意义的整数（0x02010100 + 1 没有任何意义），只是一个**编号**。
// 所以不能对它做加减，只能做相等比较与拆字节。
// =====================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ems {
namespace dlt645 {

// ---------------------------------------------------------------------
// 控制码
// ---------------------------------------------------------------------
// 从站应答 = 请求码 | 0x80（bit7 置 1 表示"这是从站发的"）；
// 异常应答再置 bit6（0x40）。于是 0x11 → 0x91（正常）/ 0xD1（异常）。
// 把它写成枚举而不是散落的魔数：0xD1 这个值在排查现场时经常被误读成
// "另一个命令"，实际上它只是"读数据 + 从站 + 异常"三件事的位组合。
enum Ctrl : std::uint8_t {
    kReadData       = 0x11,  // 主站读数据
    kReadFollow     = 0x12,  // 读后续数据（一帧装不下时分多帧）
    kWriteData      = 0x14,  // 写数据（含电表常数等）
    kWriteAddr      = 0x15,  // 写通信地址（数据域 6 字节，特殊）
    kBroadcastTime  = 0x08,  // 广播校时（地址必须是 AAAAAAAAAAAA）

    kReadDataResp   = 0x91,  // = 0x11 | 0x80
    kReadFollowResp = 0x92,
    kWriteDataResp  = 0x94,
    kWriteAddrResp  = 0x95,

    kAbnormalResp   = 0xD1,  // = 0x11 | 0x80 | 0x40
    kAbnormalMask   = 0x40,  // 与 0x40 与非零即异常应答
    kFromSlaveMask  = 0x80,
};

// 异常应答的错误码（应答帧数据域第 1 字节，**已去 0x33**）
enum ErrCode : std::uint8_t {
    kErrOther        = 0x01,
    kErrNoData       = 0x02,  // 无此数据标识
    kErrPassword     = 0x04,
    kErrCommRate     = 0x08,  // 通信速率不能更改
    kErrYearOverflow = 0x10,
};

const char* err_text(std::uint8_t code);

// ---------------------------------------------------------------------
// 6 字节通信地址
// ---------------------------------------------------------------------
// 存的是**传输序**（b[0] 先上线）。外部构造/查看一律走 BCD 字符串，
// 这样"低位在前"只在这一对函数里出现。
class Address {
public:
    Address() { std::memset(b_, 0, sizeof(b_)); }

    // "000000000001"（12 位十进制）→ 传输序字节
    // 非法输入（长度不是 12 / 含非数字）返回 false，对象保持全 0
    bool from_string(const std::string& s);

    // 传输序 → 12 位字符串。始终返回 12 位，不足补 0。
    std::string to_string() const;

    // 广播地址 AAAAAAAAAAAA。广播帧从站**不应答**（校时除外，也不应答）。
    bool is_broadcast() const;

    std::uint8_t at(int i) const { return b_[i]; }
    void set_at(int i, std::uint8_t v) { b_[i] = v; }

    bool operator==(const Address& o) const {
        return std::memcmp(b_, o.b_, sizeof(b_)) == 0;
    }

private:
    std::uint8_t b_[6];
};

// ---------------------------------------------------------------------
// 一帧
// ---------------------------------------------------------------------
struct Frame {
    Address addr;
    std::uint8_t ctrl = 0;
    // **已去掉 0x33 偏移**的原始数据域。
    // 为什么在结构体里就存"已解码"的形态：0x33 是传输层的伪装，
    // 不是语义的一部分。存进去会让每个下游都要记得减一次 ——
    // 那正是"反两次"事故的温床。编解码各做一次，中间层看不见它。
    std::vector<std::uint8_t> data;

    bool is_from_slave() const { return (ctrl & kFromSlaveMask) != 0; }
    bool is_abnormal() const { return (ctrl & kAbnormalMask) != 0; }
};

// 前导 FE 的个数上限。645 允许连续 FE 唤醒，实测 0~4 个都见过；
// 超过 4 个按"线路噪声"处理，parse() 会吃掉但计入 padding 统计。
constexpr int kMaxLeadFe = 4;

// ---------------------------------------------------------------------
// 组帧
// ---------------------------------------------------------------------
// 产出： [FE * lead_fe] 68 A0..A5 68 C L DATA(+0x33) CS 16
// data 传入的是**已解码**形态，本函数负责 +0x33。
std::vector<std::uint8_t> encode(const Frame& f, int lead_fe = 0);

// ---------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------
enum class ParseStatus {
    kOk = 0,
    kIncomplete,      // 还没收全（需要更多字节），**不是错误**
    kBadStart,        // 没有起始符 68
    kBadSecondStart,  // 第二个 68 不在 L+9 位置
    kBadChecksum,
    kBadEnd,          // 结束符不是 16
};

const char* parse_status_text(ParseStatus s);

struct ParseResult {
    ParseStatus status = ParseStatus::kIncomplete;
    Frame frame;
    std::size_t consumed = 0;   // 成功时 = 整帧字节数（含前导 FE）
    std::size_t lead_fe = 0;    // 吃掉的前导 FE 个数
};

// 从缓冲区里解析一帧。
// kIncomplete 时 consumed 无意义（调用方应继续收）。
ParseResult parse(const std::uint8_t* buf, std::size_t n);

// 算术和：从第一个 68 到 DATA 最后一字节（**含 +0x33 后的字节**）
std::uint8_t checksum(const std::uint8_t* p, std::size_t n);

// ---------------------------------------------------------------------
// DI（数据标识）的两个方向
// ---------------------------------------------------------------------
// di 是显示序（0x02010100），wire 是 4 字节低位在前。
// **这两个函数是唯一允许做 DI 反转的地方。**
inline void di_to_wire(std::uint32_t di, std::uint8_t out[4]) {
    out[0] = static_cast<std::uint8_t>(di & 0xFF);
    out[1] = static_cast<std::uint8_t>((di >> 8) & 0xFF);
    out[2] = static_cast<std::uint8_t>((di >> 16) & 0xFF);
    out[3] = static_cast<std::uint8_t>((di >> 24) & 0xFF);
}

inline std::uint32_t wire_to_di(const std::uint8_t in[4]) {
    return static_cast<std::uint32_t>(in[0]) |
           (static_cast<std::uint32_t>(in[1]) << 8) |
           (static_cast<std::uint32_t>(in[2]) << 16) |
           (static_cast<std::uint32_t>(in[3]) << 24);
}

// =====================================================================
// 实现（header-only）
// =====================================================================

inline const char* err_text(std::uint8_t code) {
    switch (code) {
        case kErrOther:        return "其它错误";
        case kErrNoData:       return "无此数据标识";
        case kErrPassword:     return "密码错/未授权";
        case kErrCommRate:     return "通信速率不能更改";
        case kErrYearOverflow: return "年时区数超";
        default:               return "未知错误码";
    }
}

inline bool Address::from_string(const std::string& s) {
    std::memset(b_, 0, sizeof(b_));
    if (s.size() != 12) return false;
    for (char ch : s) {
        if (ch < '0' || ch > '9') return false;
    }
    // s[0] 是最高位（十一位），要落到 b_[5] 的高半字节。
    // 逐位填：第 i 位（从左数）在十进制里的位置是 11-i，字节号 = (11-i)/2，
    // 半字节 = ((11-i) % 2 == 0) ? 高 : 低。
    for (int i = 0; i < 12; ++i) {
        int pos = 11 - i;            // 0 = 个位
        int byte = pos / 2;
        int nib = pos % 2;           // 0 = 低半字节，1 = 高半字节
        std::uint8_t v = static_cast<std::uint8_t>(s[i] - '0');
        if (nib == 0) b_[byte] = static_cast<std::uint8_t>((b_[byte] & 0xF0) | v);
        else          b_[byte] = static_cast<std::uint8_t>((b_[byte] & 0x0F) | (v << 4));
    }
    return true;
}

inline std::string Address::to_string() const {
    // 反向：pos 0..11，pos 偶数 → 低半字节，奇数 → 高半字节
    std::string out(12, '0');
    for (int pos = 0; pos < 12; ++pos) {
        int byte = pos / 2;
        int nib = pos % 2;
        std::uint8_t v = (nib == 0) ? static_cast<std::uint8_t>(b_[byte] & 0x0F)
                                    : static_cast<std::uint8_t>((b_[byte] >> 4) & 0x0F);
        out[11 - pos] = static_cast<char>('0' + v);
    }
    return out;
}

inline bool Address::is_broadcast() const {
    for (int i = 0; i < 6; ++i) {
        if (b_[i] != 0xAA) return false;
    }
    return true;
}

inline std::uint8_t checksum(const std::uint8_t* p, std::size_t n) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < n; ++i) sum += p[i];
    return static_cast<std::uint8_t>(sum & 0xFF);
}

inline std::vector<std::uint8_t> encode(const Frame& f, int lead_fe) {
    if (lead_fe < 0) lead_fe = 0;
    if (lead_fe > kMaxLeadFe) lead_fe = kMaxLeadFe;

    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(lead_fe) + 12 + f.data.size());

    // 唤醒前导。★ 它**不计入 CS** —— CS 从第一个 68 开始。
    for (int i = 0; i < lead_fe; ++i) out.push_back(0xFE);

    out.push_back(0x68);
    for (int i = 0; i < 6; ++i) out.push_back(f.addr.at(i));  // 已是传输序
    out.push_back(0x68);
    out.push_back(f.ctrl);
    out.push_back(static_cast<std::uint8_t>(f.data.size()));  // L 是**加 0x33 前**的长度
    for (std::uint8_t b : f.data) {
        out.push_back(static_cast<std::uint8_t>(b + 0x33));
    }
    // CS 覆盖：从第一个 68 到 DATA 最后一字节
    const std::size_t cs_begin = static_cast<std::size_t>(lead_fe);
    std::uint8_t cs = checksum(out.data() + cs_begin, out.size() - cs_begin);
    out.push_back(cs);
    out.push_back(0x16);
    return out;
}

inline ParseResult parse(const std::uint8_t* buf, std::size_t n) {
    ParseResult r;

    // 1) 吃掉前导 FE
    std::size_t i = 0;
    while (i < n && buf[i] == 0xFE) ++i;
    r.lead_fe = i;

    if (i >= n) {
        r.status = ParseStatus::kIncomplete;
        return r;
    }
    if (buf[i] != 0x68) {
        r.status = ParseStatus::kBadStart;
        return r;
    }

    // 最小帧：68 + 6 地址 + 68 + C + L + CS + 16 = 12 字节（L=0）
    if (n - i < 12) {
        r.status = ParseStatus::kIncomplete;
        return r;
    }
    if (buf[i + 7] != 0x68) {
        r.status = ParseStatus::kBadSecondStart;
        return r;
    }

    const std::uint8_t L = buf[i + 9];
    const std::size_t total = 12 + static_cast<std::size_t>(L);
    if (n - i < total) {
        r.status = ParseStatus::kIncomplete;
        return r;
    }

    const std::uint8_t cs_calc = checksum(buf + i, 10 + L);  // 68..DATA 末
    const std::uint8_t cs_recv = buf[i + 10 + L];
    if (cs_calc != cs_recv) {
        r.status = ParseStatus::kBadChecksum;
        return r;
    }
    if (buf[i + 11 + L] != 0x16) {
        r.status = ParseStatus::kBadEnd;
        return r;
    }

    r.status = ParseStatus::kOk;
    r.consumed = i + total;   // 含前导 FE

    for (int k = 0; k < 6; ++k) r.frame.addr.set_at(k, buf[i + 1 + k]);
    r.frame.ctrl = buf[i + 8];
    r.frame.data.resize(L);
    for (std::size_t k = 0; k < L; ++k) {
        // ★ 减 0x33 只在这里做一次
        r.frame.data[k] = static_cast<std::uint8_t>(buf[i + 10 + k] - 0x33);
    }
    return r;
}

inline const char* parse_status_text(ParseStatus s) {
    switch (s) {
        case ParseStatus::kOk:             return "OK";
        case ParseStatus::kIncomplete:     return "INCOMPLETE";
        case ParseStatus::kBadStart:       return "BAD_START";
        case ParseStatus::kBadSecondStart: return "BAD_SECOND_START";
        case ParseStatus::kBadChecksum:    return "BAD_CHECKSUM";
        case ParseStatus::kBadEnd:         return "BAD_END";
    }
    return "UNKNOWN";
}

}  // namespace dlt645
}  // namespace ems
