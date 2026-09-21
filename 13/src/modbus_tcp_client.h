// =====================================================================
// 13/ — Modbus TCP 客户端（**协议层，不含任何业务语义**）
//
// 为什么自己写协议层而不引第三方库：
//   1. 项目纪律是 header-only + 无外部依赖（唯一的例外是 P3 的 lib60870 ——
//      IEC104 的 ASDU / CP56Time2a / 链路层复杂度确实不值得自己写）。
//      Modbus TCP 的 MBAP + PDU 只有几百行，自己写反而更可控。
//   2. **「字序 / 缩放 / 分块」是现场最容易出错的三处**，必须逐字节可断言。
//      包在第三方库里就只能测"最终值对不对"，测不出"字节有没有错位" ——
//      而现场故障恰恰大多出在字节层（高低字颠倒、半包、粘包）。
//   3. 现场从站五花八门（PLC / 电表 / 储能变流器），异常码与超时行为不一致，
//      需要一个自己能改的最小内核。
//
// 分层的边界（P0 纪律在真实适配器上更不能破）：
//   · 本文件：字节 ↔ 请求/响应。**不知道任何点名、也不知道任何业务语义**。
//   · modbus_point_map.h：点名/业务索引 ↔ 寄存器地址（**映射的唯一真相源**）。
//   · modbus_device_io.h：业务结构体 ↔ 寄存器值（IDeviceIO 实现）。
//
// 三个"看起来显然、实际必须显式处理"的点，都在本文件里：
//   ★ ① **TCP 是字节流**：一次 recv 可能只拿到半个 MBAP，也可能一次拿回两个
//        响应（粘包）。必须按 MBAP.Length 循环读到齐。现场"偶发解析错"
//        十有八九是这里没做对。
//   ★ ② **响应必须校验事务号与单元号**：否则"回错包"会被当成"发对包"，
//        在并发/重连场景下会静默串数据。
//   ★ ③ **异常响应不是通信故障**：从站回了 `FC|0x80` 说明"我听懂了但拒绝"
//        （非法地址/非法值）。两者在业务层的处置完全不同 —— 前者重试，
//        后者要报配置错误。混成一个 bool 就再也分不开了。
//
// 编译：纯头文件。MinGW 需链接 `-lws2_32`；MSVC 用下面的 pragma。
// =====================================================================

#pragma once

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX       // 本头会经 <winsock2.h> 间接拉进 <windows.h>；
#  endif                    // 项目里大量 std::max/std::min，不定义 NOMINMAX 编译不过
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  if defined(_MSC_VER)
#    pragma comment(lib, "ws2_32.lib")     // MSVC 专用；MinGW 靠 -lws2_32
#  endif
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  include <cerrno>
#  include <fcntl.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace ems {
namespace modbus {

// =====================================================================
// 协议常量
// =====================================================================
constexpr std::size_t   kMbapLen       = 7;    // 事务(2)+协议(2)+长度(2)+单元(1)
constexpr std::size_t   kMaxPduLen     = 253;  // Modbus TCP 单个 PDU 上限
constexpr std::size_t   kMaxAduLen     = 260;  // kMbapLen + kMaxPduLen
constexpr std::uint16_t kProtocolId    = 0;    // Modbus 的协议标识恒为 0

// 各功能码的单次数量上限（协议规定；越界从站应回 kIllegalDataValue）
constexpr std::uint16_t kMaxReadRegs   = 125;  // FC03 / FC04
constexpr std::uint16_t kMaxReadBits   = 2000; // FC01 / FC02
constexpr std::uint16_t kMaxWriteRegs  = 123;  // FC16

namespace fc {                                    // 功能码
constexpr std::uint8_t kReadCoils            = 0x01;
constexpr std::uint8_t kReadDiscreteInputs   = 0x02;
constexpr std::uint8_t kReadHoldingRegs      = 0x03;
constexpr std::uint8_t kReadInputRegs        = 0x04;
constexpr std::uint8_t kWriteSingleCoil      = 0x05;
constexpr std::uint8_t kWriteSingleReg       = 0x06;
constexpr std::uint8_t kWriteMultipleCoils   = 0x0F;
constexpr std::uint8_t kWriteMultipleRegs    = 0x10;
} // namespace fc

// =====================================================================
// 错误分类
//
// 为什么不用一个 bool：三类错误的**处置方式完全不同** ——
//   · 传输层（kNotConnected / kSocketError / kTimeout）→ 重连 + 保留上次有效值
//   · 协议层（kMalformed / kTransactionMismatch）→ 说明对端不是合格从站，
//     要报配置/固件问题，重试没有意义
//   · 从站异常（kException）→ **通信是好的**，"我听懂了但拒绝"。地址映射
//     写错了就是这个；必须报出来，不能吞。
// =====================================================================
enum class ModbusError {
    kNone = 0,
    kNotConnected,          // 未连接，或连接已被对端关闭
    kSocketError,           // send / recv / select 失败
    kTimeout,               // 超时：响应未到或不完整
    kMalformed,             // 响应格式非法（长度 / 功能码 / 字节数不符）
    kTransactionMismatch,   // 事务号或单元号对不上（串包）
    kException,             // 从站返回异常响应（见 exception_code）
};

struct ModbusStatus {
    ModbusError   error          = ModbusError::kNone;
    std::uint8_t  exception_code = 0;   // 仅 kException 时有意义
    int           os_error       = 0;   // 仅 kSocketError 时有意义

    bool ok() const { return error == ModbusError::kNone; }
    bool is_transport() const {
        return error == ModbusError::kNotConnected ||
               error == ModbusError::kSocketError  ||
               error == ModbusError::kTimeout;
    }
    std::string describe() const {
        char buf[128];
        switch (error) {
        case ModbusError::kNone:
            return "ok";
        case ModbusError::kNotConnected:
            return "not connected";
        case ModbusError::kSocketError:
            std::snprintf(buf, sizeof(buf), "socket error (os=%d)", os_error);
            return buf;
        case ModbusError::kTimeout:
            return "timeout";
        case ModbusError::kMalformed:
            return "malformed response";
        case ModbusError::kTransactionMismatch:
            return "transaction/unit id mismatch";
        case ModbusError::kException:
            std::snprintf(buf, sizeof(buf), "slave exception 0x%02X",
                          static_cast<unsigned>(exception_code));
            return buf;
        }
        return "unknown";
    }
};

// =====================================================================
// 字节序工具（Modbus 线上**恒为大端**；与主机字节序无关）
// =====================================================================
inline void put_u16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<std::uint8_t>(v & 0xFF);
}
inline std::uint16_t get_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

// 32 位浮点 ↔ 两个寄存器的**字序**。
//
// ★ 这是 Modbus 现场第一大坑：协议只规定"每个寄存器 16 位、大端传输"，
//   **没规定** 32 位量两个寄存器谁在前。于是厂家各写各的：
//     kHighWordFirst（"ABCD"，即大端字序）—— 西门子/施耐德常见默认
//     kLowWordFirst （"CDAB"，即字交换）    —— 三菱/部分国产表常见
//   写错的表现**不是**"差一点点"，而是**数值完全离谱或成 NaN** ——
//   所以必须由映射表**逐点显式声明**，并在测试里对字节做断言。
enum class WordOrder {
    kHighWordFirst = 0,   // 寄存器[n] = 高 16 位
    kLowWordFirst  = 1,   // 寄存器[n] = 低 16 位
};

inline void put_f32(std::uint16_t* regs, float v, WordOrder wo) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    const std::uint16_t hi = static_cast<std::uint16_t>((bits >> 16) & 0xFFFF);
    const std::uint16_t lo = static_cast<std::uint16_t>(bits & 0xFFFF);
    regs[0] = (wo == WordOrder::kHighWordFirst) ? hi : lo;
    regs[1] = (wo == WordOrder::kHighWordFirst) ? lo : hi;
}
inline float get_f32(const std::uint16_t* regs, WordOrder wo) {
    const std::uint16_t hi = (wo == WordOrder::kHighWordFirst) ? regs[0] : regs[1];
    const std::uint16_t lo = (wo == WordOrder::kHighWordFirst) ? regs[1] : regs[0];
    const std::uint32_t bits = (static_cast<std::uint32_t>(hi) << 16) |
                               static_cast<std::uint32_t>(lo);
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// =====================================================================
// PDU 编解码 —— **纯函数，不碰 socket**
//
// 为什么要把它们抽成自由函数：协议测试要能"构造一段字节，断言另一段字节"，
// 不需要真的起 TCP。现场排障时也能拿一段抓包直接喂进来。
// =====================================================================
namespace pdu {

// ---- 请求构造：返回写出的字节数；0 表示参数非法（**不静默截断**）----
inline std::size_t build_read(std::uint8_t function, std::uint16_t addr,
                              std::uint16_t qty, std::uint8_t* out) {
    // 数量上限按功能码区分：寄存器 125 / 位 2000。
    // 越界时返回 0 让调用方分块，而不是发一个必然被拒的请求。
    const std::uint16_t limit =
        (function == fc::kReadHoldingRegs || function == fc::kReadInputRegs)
            ? kMaxReadRegs : kMaxReadBits;
    if (qty == 0 || qty > limit) return 0;
    out[0] = function;
    put_u16(out + 1, addr);
    put_u16(out + 3, qty);
    return 5;
}

inline std::size_t build_write_single(std::uint8_t function, std::uint16_t addr,
                                      std::uint16_t value, std::uint8_t* out) {
    out[0] = function;
    put_u16(out + 1, addr);
    put_u16(out + 3, value);
    return 5;
}

inline std::size_t build_write_multiple_regs(std::uint16_t addr,
                                             const std::uint16_t* values,
                                             std::uint16_t qty, std::uint8_t* out) {
    if (qty == 0 || qty > kMaxWriteRegs) return 0;
    const std::uint16_t byte_count = static_cast<std::uint16_t>(qty * 2);
    out[0] = fc::kWriteMultipleRegs;
    put_u16(out + 1, addr);
    put_u16(out + 3, qty);
    out[5] = static_cast<std::uint8_t>(byte_count);
    for (std::uint16_t i = 0; i < qty; ++i) put_u16(out + 6 + i * 2, values[i]);
    return static_cast<std::size_t>(6) + byte_count;
}

// ---- 响应解析 ----
// 先看是不是异常响应（FC 最高位为 1）。**必须最先判** —— 异常响应的
// 载荷只有 1 字节异常码，按正常响应的布局去解析会直接读越界。
struct ResponseHeader {
    bool         is_exception    = false;
    std::uint8_t function        = 0;
    std::uint8_t exception_code  = 0;
    bool         valid           = false;   // 长度是否至少够放下头
};

inline ResponseHeader parse_header(const std::uint8_t* pdu, std::size_t len) {
    ResponseHeader h;
    if (len < 1 || pdu == nullptr) return h;
    h.function = static_cast<std::uint8_t>(pdu[0] & 0x7F);
    if ((pdu[0] & 0x80) != 0) {
        h.is_exception = true;
        h.valid        = (len >= 2);
        if (len >= 2) h.exception_code = pdu[1];
        return h;
    }
    h.valid = (len >= 2);
    return h;
}

// 读寄存器响应：FC(1) + ByteCount(1) + Data(2N)
// ★ ByteCount 必须**恰好**等于 2*qty：多了说明对端地址/数量解读不同，
//   少了说明包被截断 —— 两者都不能"凑合读 N 个值"。
inline bool parse_read_regs(const std::uint8_t* pdu, std::size_t len,
                            std::uint16_t qty, std::uint16_t* out) {
    if (pdu == nullptr || out == nullptr) return false;
    const std::size_t need = 2 + static_cast<std::size_t>(qty) * 2;
    if (len != need) return false;
    const std::uint16_t byte_count = pdu[1];
    if (byte_count != qty * 2) return false;
    for (std::uint16_t i = 0; i < qty; ++i) out[i] = get_u16(pdu + 2 + i * 2);
    return true;
}

// 读位响应：FC(1) + ByteCount(1) + Data(⌈qty/8⌉)
// 位打包：起始地址对应第一个字节的 bit0，**低位在前**（Modbus 规定）。
inline bool parse_read_bits(const std::uint8_t* pdu, std::size_t len,
                            std::uint16_t qty, bool* out) {
    if (pdu == nullptr || out == nullptr || qty == 0) return false;
    const std::uint16_t expected_bc = static_cast<std::uint16_t>((qty + 7) / 8);
    const std::size_t need = 2 + expected_bc;
    if (len != need) return false;
    if (pdu[1] != expected_bc) return false;
    for (std::uint16_t i = 0; i < qty; ++i) {
        const std::uint8_t byte = pdu[2 + i / 8];
        out[i] = ((byte >> (i % 8)) & 0x01) != 0;
    }
    return true;
}

// 写响应：FC(1) + Addr(2) + Qty/Value(2)，且**必须回显请求里的地址与数量**
inline bool parse_write_response(const std::uint8_t* pdu, std::size_t len,
                                 std::uint16_t expect_addr, std::uint16_t expect_val) {
    if (pdu == nullptr || len != 5) return false;
    const std::uint16_t addr = get_u16(pdu + 1);
    const std::uint16_t val  = get_u16(pdu + 3);
    return addr == expect_addr && val == expect_val;
}

} // namespace pdu

// =====================================================================
// Winsock 生命周期
//
// 用函数内 static 保证进程内只 WSAStartup 一次：WSAStartup/WSACleanup 是
// **引用计数**配对使用的，多次调用而不配对会让某次析构把还在用的网络栈关掉。
// =====================================================================
namespace detail {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

inline bool socket_valid(socket_t s) { return s != kInvalidSocket; }

inline void close_socket(socket_t s) {
    if (!socket_valid(s)) return;
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

class WinsockGuard {
public:
    WinsockGuard() {
#ifdef _WIN32
        WSADATA d;
        started_ = (::WSAStartup(MAKEWORD(2, 2), &d) == 0);
#endif
    }
    ~WinsockGuard() {
#ifdef _WIN32
        if (started_) ::WSACleanup();
#endif
    }
    bool ok() const { return started_; }
private:
    bool started_ = true;
};

inline bool net_ready() {
    static WinsockGuard g;
    return g.ok();
}

inline int last_socket_error() {
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

// ★ 超时 与 "暂时无数据" 必须**分开判断**。
//
// 曾经的写法是把两者合成一个 would_block()，于是 recv_exact() 在超时后
// 会 `continue` —— 死循环。设备静默时主站不是按契约报 kTimeout，而是
// **永远挂住**。这个缺陷不是读代码看出来的，是 fake 从站的 silent 注入
// 把测试卡死之后才暴露的（见 tests/test_modbus_tcp.cpp T13）。
//
// 阻塞 socket + SO_RCVTIMEO 下：
//   Windows: 超时 → WSAETIMEDOUT；WSAEWOULDBLOCK 只在非阻塞时出现
//   Linux  : 超时 → EAGAIN/EWOULDBLOCK（阻塞 socket 上两者同义）
inline bool is_timeout(int e) {
#ifdef _WIN32
    return e == WSAETIMEDOUT;
#else
    return e == EAGAIN || e == EWOULDBLOCK || e == ETIMEDOUT;
#endif
}

inline bool would_block(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK;
#else
    return false;   // 阻塞 socket 上 EAGAIN 已在 is_timeout() 里处理
#endif
}

// 非阻塞开关（非阻塞 connect 用）
inline int set_nonblocking(socket_t s, bool nb) {
#ifdef _WIN32
    u_long v = nb ? 1u : 0u;
    return ::ioctlsocket(s, FIONBIO, &v);
#else
    int fl = ::fcntl(s, F_GETFL, 0);
    if (fl < 0) return -1;
    fl = nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    return ::fcntl(s, F_SETFL, fl);
#endif
}

// ★ 带超时的 connect（非阻塞 connect + select 等可写）
//
// 为什么必须这么做：**阻塞 connect 在设备离线时会等 OS 的 SYN 重传超时**
// （Windows 实测约 20 秒），而控制周期只有 200 ms —— 一次掉线就把整个
// 时间轴拖垮。这是现场"设备一断、EMS 全乱"最常见的根因，而且它表现成
// "程序偶尔卡死"，最难归因到 connect。
inline bool connect_with_timeout(socket_t s, const sockaddr* addr, int addrlen,
                                 int timeout_ms) {
    set_nonblocking(s, true);
    int rc = 0;
    if (addrlen > 0) {
        rc = ::connect(s, addr, addrlen);
    } else {
        set_nonblocking(s, false);
        return false;
    }
    if (rc == 0) {                       // 本机环回常常立刻成功
        set_nonblocking(s, false);
        return true;
    }
    const int e = last_socket_error();
#ifdef _WIN32
    const bool in_progress = (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS);
#else
    const bool in_progress = (e == EINPROGRESS);
#endif
    if (!in_progress) {
        set_nonblocking(s, false);
        return false;
    }

    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int sel = ::select(static_cast<int>(s) + 1, nullptr, &wf, nullptr, &tv);
    if (sel <= 0) {                      // 超时或出错（Windows 的 select 会返回错误码）
        set_nonblocking(s, false);
        return false;
    }

    int err = 0;
#ifdef _WIN32
    int elen = sizeof(err);
#else
    socklen_t elen = sizeof(err);
#endif
    if (::getsockopt(s, SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&err), &elen) != 0) {
        set_nonblocking(s, false);
        return false;
    }
    set_nonblocking(s, false);
    return err == 0;                     // SO_ERROR 才是 connect 的真实结论
}

} // namespace detail

// =====================================================================
// Modbus TCP 客户端
//
// 单连接顺序请求：本类**不做并发**。现场一台 PCS 的一条 Modbus 连接本来就
// 只允许一个未完成事务（大多数设备不支持流水线），所以顺序模型是**贴合现场**
// 的，而不是偷懒。
// =====================================================================
class ModbusTcpClient {
public:
    ModbusTcpClient() = default;
    ~ModbusTcpClient() { close(); }
    ModbusTcpClient(const ModbusTcpClient&)            = delete;
    ModbusTcpClient& operator=(const ModbusTcpClient&) = delete;

    // ---------------- 连接管理 ----------------
    bool connect(const char* host, std::uint16_t port, int timeout_ms = 1000,
                 std::uint8_t unit_id = 1) {
        close();
        if (!detail::net_ready()) return false;

        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* res = nullptr;
        char port_str[16];
        std::snprintf(port_str, sizeof(port_str), "%u", static_cast<unsigned>(port));
        if (::getaddrinfo(host, port_str, &hints, &res) != 0 || res == nullptr) return false;

        detail::socket_t s = detail::kInvalidSocket;
        for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
            s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (!detail::socket_valid(s)) continue;
            // 带超时的 connect（见 connect_with_timeout 的说明：
            // 阻塞 connect 在设备离线时要等 ~20 s 的 SYN 重传）
            if (detail::connect_with_timeout(s, p->ai_addr,
                                             static_cast<int>(p->ai_addrlen),
                                             timeout_ms)) {
                break;
            }
            detail::close_socket(s);
            s = detail::kInvalidSocket;
        }
        ::freeaddrinfo(res);
        if (!detail::socket_valid(s)) return false;

        set_timeout(s, timeout_ms);

        // TCP_NODELAY：Modbus 请求-响应往返短，禁用 Nagle 才不会凭空多 40 ms
        // 延迟。这不是优化，是**现场可用性**问题（一拍 200 ms 的控制器里
        // 40 ms 的固定延迟会直接吃掉裕度）。
        int one = 1;
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&one), sizeof(one));

        sock_     = s;
        unit_id_  = unit_id;
        timeout_  = timeout_ms;
        return true;
    }

    void close() {
        detail::close_socket(sock_);
        sock_ = detail::kInvalidSocket;
    }
    bool is_connected() const { return detail::socket_valid(sock_); }

    // ---------------- 读 ----------------
    ModbusStatus read_holding(std::uint16_t addr, std::uint16_t qty, std::uint16_t* out) {
        return transact_read(fc::kReadHoldingRegs, addr, qty, out);
    }
    ModbusStatus read_input(std::uint16_t addr, std::uint16_t qty, std::uint16_t* out) {
        return transact_read(fc::kReadInputRegs, addr, qty, out);
    }
    // 线圈（可读写位）与离散输入（只读位）共用 FC01/FC02
    ModbusStatus read_coils(std::uint16_t addr, std::uint16_t qty, bool* out) {
        return transact_read_bits(fc::kReadCoils, addr, qty, out);
    }
    ModbusStatus read_discrete(std::uint16_t addr, std::uint16_t qty, bool* out) {
        return transact_read_bits(fc::kReadDiscreteInputs, addr, qty, out);
    }

    // ---------------- 写 ----------------
    ModbusStatus write_single_reg(std::uint16_t addr, std::uint16_t value) {
        std::uint8_t pdu[kMaxPduLen];
        const std::size_t n = pdu::build_write_single(fc::kWriteSingleReg, addr, value, pdu);
        std::uint8_t resp[kMaxPduLen];
        std::size_t  rlen = 0;
        ModbusStatus st = round_trip(pdu, n, resp, &rlen);
        if (!st.ok()) return st;
        if (!pdu::parse_write_response(resp, rlen, addr, value)) {
            return modbus::ModbusStatus{ModbusError::kMalformed, 0, 0};
        }
        ++writes_;
        return {};
    }

    ModbusStatus write_coil(std::uint16_t addr, bool on) {
        // ★ FC05 的"开"是 0xFF00 而**不是** 0x0001；写 0x0001 会被从站判为
        //   非法数据值（0x03）。这是现场手写报文时最常见的错。
        const std::uint16_t v = on ? 0xFF00 : 0x0000;
        std::uint8_t pdu[kMaxPduLen];
        const std::size_t n = pdu::build_write_single(fc::kWriteSingleCoil, addr, v, pdu);
        std::uint8_t resp[kMaxPduLen];
        std::size_t  rlen = 0;
        ModbusStatus st = round_trip(pdu, n, resp, &rlen);
        if (!st.ok()) return st;
        if (!pdu::parse_write_response(resp, rlen, addr, v)) {
            return modbus::ModbusStatus{ModbusError::kMalformed, 0, 0};
        }
        ++writes_;
        return {};
    }

    ModbusStatus write_multiple_regs(std::uint16_t addr, const std::uint16_t* values,
                                     std::uint16_t qty) {
        std::uint8_t pdu[kMaxPduLen];
        const std::size_t n = pdu::build_write_multiple_regs(addr, values, qty, pdu);
        if (n == 0) return modbus::ModbusStatus{ModbusError::kException, 0x03, 0};
        std::uint8_t resp[kMaxPduLen];
        std::size_t  rlen = 0;
        ModbusStatus st = round_trip(pdu, n, resp, &rlen);
        if (!st.ok()) return st;
        // 写多个的响应回显的是"起始地址 + 数量"（不是首值）
        if (!pdu::parse_write_response(resp, rlen, addr, qty)) {
            return modbus::ModbusStatus{ModbusError::kMalformed, 0, 0};
        }
        ++writes_;
        return {};
    }

    // ---------------- 诊断（现场排障 + 测试判据） ----------------
    int requests()   const { return requests_; }    // 发出的请求总数
    int timeouts()   const { return timeouts_; }
    int exceptions() const { return exceptions_; }  // 从站异常响应数
    int malformed()  const { return malformed_; }
    int writes()     const { return writes_; }
    // 被丢弃的"不属于本次事务"的响应数（迟到/串包）。**不是**错误计数 ——
    // 它是"链路抖过"的证据，正常站点偶尔非 0，持续增长才是问题。
    int stale_responses() const { return stale_responses_; }
    int reconnects() const { return reconnects_; }  // 重连成功次数（含初次）
    std::uint8_t  unit_id()       const { return unit_id_; }
    std::uint16_t last_transaction() const { return last_tid_; }

private:
    // 单次请求-响应。**顺序模型**：一个未完成事务，收到响应才算完。
    ModbusStatus round_trip(const std::uint8_t* req_pdu, std::size_t req_len,
                            std::uint8_t* resp_pdu, std::size_t* resp_len) {
        if (!is_connected()) return modbus::ModbusStatus{ModbusError::kNotConnected, 0, 0};
        if (req_len == 0 || req_len > kMaxPduLen) {
            return modbus::ModbusStatus{ModbusError::kException, 0x03, 0};
        }

        std::uint8_t adu[kMaxAduLen];
        const std::uint16_t tid = ++transaction_;
        put_u16(adu + 0, tid);
        put_u16(adu + 2, kProtocolId);
        put_u16(adu + 4, static_cast<std::uint16_t>(req_len + 1));   // +1 = Unit ID
        adu[6] = unit_id_;
        std::memcpy(adu + kMbapLen, req_pdu, req_len);

        ++requests_;
        if (!send_all(adu, kMbapLen + req_len)) {
            const int e = detail::last_socket_error();
            close();
            return modbus::ModbusStatus{ModbusError::kSocketError, 0, e};
        }

        // ★ 读响应：**允许丢弃迟到的响应后继续等**。
        //
        // 为什么必须容忍而不是直接报错：
        //   上一次请求超时后，那个响应可能在几毫秒后才回到 socket 上。
        //   于是本次请求读到的**第一个**包其实是上一次的（事务号是上一次的）。
        //   若此时判"事务号不匹配 → 失败"，就等于把"上一次慢"升级成
        //   "这一拍也失败" —— 一次抖动会连锁成两拍数据缺失。
        //   现场正确做法：认事务号，不是我的就丢掉继续读。
        //   丢 N 次还等不到才认为对端有问题（超时/故障），报 kTransactionMismatch。
        constexpr int kMaxStaleResponses = 4;
        for (int attempt = 0; attempt <= kMaxStaleResponses; ++attempt) {
            std::uint8_t hdr[kMbapLen];
            if (!recv_exact(hdr, kMbapLen)) {
                ++timeouts_;
                close();
                return modbus::ModbusStatus{ModbusError::kTimeout, 0, 0};
            }
            if (get_u16(hdr + 2) != kProtocolId) {   // 协议标识必须为 0
                ++malformed_;
                return modbus::ModbusStatus{ModbusError::kMalformed, 0, 0};
            }
            const std::uint16_t mbap_len = get_u16(hdr + 4);   // = 1(unit) + pdu_len
            if (mbap_len < 2 || mbap_len > kMaxPduLen + 1) {
                ++malformed_;
                return modbus::ModbusStatus{ModbusError::kMalformed, 0, 0};
            }
            const std::size_t need_pdu = static_cast<std::size_t>(mbap_len) - 1;

            // ★ 事务号 / 单元号必须对得上：对不上说明这不是本次请求的响应
            //   （上一轮超时残留、或对端串包）。**绝不能当成正常响应继续解析。**
            const std::uint16_t rx_tid = get_u16(hdr + 0);
            const std::uint8_t  rx_uid = hdr[6];
            if (rx_tid != tid || rx_uid != unit_id_) {
                if (!drain(need_pdu)) {   // 把这个不属于我们的包读走，保持流同步
                    close();
                    return modbus::ModbusStatus{ModbusError::kSocketError, 0,
                                                detail::last_socket_error()};
                }
                last_tid_ = rx_tid;
                ++stale_responses_;
                continue;   // 继续等真正属于本次事务的响应
            }
            last_tid_ = tid;

            if (need_pdu > *resp_len) { *resp_len = need_pdu; }  // 让调用方能报"缓冲区不够"
            if (!recv_exact(resp_pdu, need_pdu)) {
                ++timeouts_;
                close();
                return modbus::ModbusStatus{ModbusError::kTimeout, 0, 0};
            }
            *resp_len = need_pdu;

            const pdu::ResponseHeader h = pdu::parse_header(resp_pdu, need_pdu);
            if (!h.valid) {
                ++malformed_;
                return modbus::ModbusStatus{ModbusError::kMalformed, 0, 0};
            }
            if (h.is_exception) {
                ++exceptions_;
                return modbus::ModbusStatus{ModbusError::kException, h.exception_code, 0};
            }
            return {};
        }

        // 连续丢了 N 个不属于本次事务的响应 → 对端/链路有问题，不再等
        ++malformed_;
        return modbus::ModbusStatus{ModbusError::kTransactionMismatch, 0, 0};
    }

    ModbusStatus transact_read(std::uint8_t function, std::uint16_t addr,
                               std::uint16_t qty, std::uint16_t* out) {
        std::uint8_t pdu[kMaxPduLen];
        const std::size_t n = pdu::build_read(function, addr, qty, pdu);
        if (n == 0) return modbus::ModbusStatus{ModbusError::kException, 0x03, 0};
        std::uint8_t resp[kMaxPduLen];
        std::size_t  rlen = sizeof(resp);
        ModbusStatus st = round_trip(pdu, n, resp, &rlen);
        if (!st.ok()) return st;
        const pdu::ResponseHeader h = pdu::parse_header(resp, rlen);
        if (h.function != function) {
            ++malformed_;
            return modbus::ModbusStatus{ModbusError::kMalformed, 0, 0};
        }
        if (!pdu::parse_read_regs(resp, rlen, qty, out)) {
            ++malformed_;
            return modbus::ModbusStatus{ModbusError::kMalformed, 0, 0};
        }
        return {};
    }

    ModbusStatus transact_read_bits(std::uint8_t function, std::uint16_t addr,
                                    std::uint16_t qty, bool* out) {
        std::uint8_t pdu[kMaxPduLen];
        const std::size_t n = pdu::build_read(function, addr, qty, pdu);
        if (n == 0) return modbus::ModbusStatus{ModbusError::kException, 0x03, 0};
        std::uint8_t resp[kMaxPduLen];
        std::size_t  rlen = sizeof(resp);
        ModbusStatus st = round_trip(pdu, n, resp, &rlen);
        if (!st.ok()) return st;
        const pdu::ResponseHeader h = pdu::parse_header(resp, rlen);
        if (h.function != function) {
            ++malformed_;
            return modbus::ModbusStatus{ModbusError::kMalformed, 0, 0};
        }
        if (!pdu::parse_read_bits(resp, rlen, qty, out)) {
            ++malformed_;
            return modbus::ModbusStatus{ModbusError::kMalformed, 0, 0};
        }
        return {};
    }

    // ---------------- 收发原语 ----------------
    bool send_all(const std::uint8_t* buf, std::size_t len) {
        std::size_t sent = 0;
        while (sent < len) {
            const int n = ::send(sock_, reinterpret_cast<const char*>(buf + sent),
                                 static_cast<int>(len - sent), 0);
            if (n <= 0) {
                const int e = detail::last_socket_error();
                if (detail::is_timeout(e)) return false;   // 超时 → 放弃
                if (detail::would_block(e)) continue;      // 暂时无数据 → 重试
                return false;
            }
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    // ★ 这个函数就是"半包"的解药：必须循环，直到拿到 exactly len 字节。
    //   一次 recv 返回的字节数**没有任何保证**（可能 1 字节，也可能两包一起）。
    //   注意循环的退出条件：超时必须 return false，**不能 continue** ——
    //   把超时当"再等等"会让设备静默时本函数永不返回（见 is_timeout 的说明）。
    bool recv_exact(std::uint8_t* buf, std::size_t len) {
        std::size_t got = 0;
        while (got < len) {
            const int n = ::recv(sock_, reinterpret_cast<char*>(buf + got),
                                 static_cast<int>(len - got), 0);
            if (n == 0) return false;                      // 对端正常关闭
            if (n < 0) {
                const int e = detail::last_socket_error();
                if (detail::is_timeout(e)) return false;   // 超时 → 放弃
                if (detail::would_block(e)) continue;      // 暂时无数据 → 重试
                return false;
            }
            got += static_cast<std::size_t>(n);
        }
        return true;
    }

    // 丢弃 n 字节（用于事务号不匹配时把流同步回来）
    bool drain(std::size_t n) {
        std::uint8_t scratch[64];
        while (n > 0) {
            const std::size_t chunk = (n < sizeof(scratch)) ? n : sizeof(scratch);
            if (!recv_exact(scratch, chunk)) return false;
            n -= chunk;
        }
        return true;
    }

    static void set_timeout(detail::socket_t s, int timeout_ms) {
#ifdef _WIN32
        DWORD tv = static_cast<DWORD>(timeout_ms);
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    }

    detail::socket_t sock_    = detail::kInvalidSocket;
    std::uint8_t     unit_id_ = 1;
    int              timeout_ = 1000;

    std::uint16_t transaction_ = 0;
    std::uint16_t last_tid_    = 0;
    int requests_   = 0;
    int timeouts_   = 0;
    int exceptions_ = 0;
    int malformed_  = 0;
    int writes_     = 0;
    int reconnects_ = 0;
    int stale_responses_ = 0;
};

} // namespace modbus
} // namespace ems
