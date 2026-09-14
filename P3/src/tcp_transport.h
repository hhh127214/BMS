// =====================================================================
// P3/ — 产品化 P3（通信）：TCP 传输层 —— 从"环回验证"到"接真机"的最后一跳
//
// 定位：把「字节怎么在网线上跑」单独收在一个文件里。上面两层完全不动：
//     ModbusDeviceIO  / Iec104DeviceIO        ← 适配器（不知道 socket 存在）
//     modbus_codec.h  / iec104_codec.h        ← 编解码（纯函数）
//     tcp_transport.h                          ← 本文件：只做字节进出
//
// 换掉 LoopbackTransport → TcpTransport，**适配器与算法层一行不改**。
// 这正是 P0 那条"换介质不改算法"承诺链条的最后一段：
//     SimDeviceIO → MemoryDeviceIO → RtDbDeviceIO → Loopback → **TCP**
//
// 三条实现纪律（都是现场踩出来的）：
//   1. **Modbus 必须按 MBAP 的 Length 字段分帧**，不能"发一次收一次"。
//      TCP 是字节流，一次 recv 可能只回来半个响应，也可能把两个响应粘在一起。
//      做法：先精确读 7 字节 MBAP，再按 Length 精确读剩余部分。
//   2. **IEC104 的 receive 必须是"三态"语义**：链路错误 / 没有报文 / 有 N 字节，
//      分别对应 (false) / (true, len=0) / (true, len=N)。
//      适配器的 drain_rx() 靠这三态区分"对端哑了"和"这一轮没数据"。
//   3. **connect 要带超时**。阻塞式 connect 连不通的地址会挂 20 秒以上，
//      在现场表现为"EMS 卡死"，比连不上更难查。用非阻塞 connect + select。
//
// 平台：Windows(Winsock2) / POSIX 双实现。MinGW 链接需加 -lws2_32。
// 注意：本文件 include <winsock2.h>，必须排在 <windows.h> 之前（若使用者要用后者）。
// =====================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "modbus_device_io.h"   // IModbusTransport
#include "iec104_device_io.h"   // IIec104Transport

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
using bms_socket_t  = SOCKET;
using bms_socklen_t = int;
using bms_optval_t  = char*;
#else
  #include <arpa/inet.h>
  #include <cerrno>
  #include <fcntl.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <unistd.h>
using bms_socket_t  = int;
using bms_socklen_t = socklen_t;
using bms_optval_t  = void*;
#endif

namespace ems {
namespace net {

#ifdef _WIN32
inline constexpr bms_socket_t kInvalidSocket = INVALID_SOCKET;
#else
inline constexpr bms_socket_t kInvalidSocket = static_cast<bms_socket_t>(-1);
#endif

inline int last_socket_error() {
#ifdef _WIN32
    return static_cast<int>(::WSAGetLastError());
#else
    return errno;
#endif
}

inline bool sock_would_block(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return e == EWOULDBLOCK || e == EINPROGRESS || e == EAGAIN;
#endif
}

inline void sock_close(bms_socket_t s) {
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

// ---------------------------------------------------------------------
// Winsock 生命周期：进程内初始化一次
// （POSIX 下为空操作；析构在 main 之后，不需要显式清理顺序）
// ---------------------------------------------------------------------
class SocketRuntime {
public:
    static bool ensure() { return instance().started_; }

private:
    static SocketRuntime& instance() {
        static SocketRuntime rt;
        return rt;
    }
    SocketRuntime() {
#ifdef _WIN32
        WSADATA d;
        started_ = (::WSAStartup(MAKEWORD(2, 2), &d) == 0);
#else
        started_ = true;
#endif
    }
    ~SocketRuntime() {
#ifdef _WIN32
        if (started_) ::WSACleanup();
#endif
    }
    bool started_ = false;
};

// select 就绪等待：返回 1=就绪 / 0=超时 / -1=错误
inline int wait_ready(bms_socket_t s, bool want_write, int timeout_ms) {
    if (s == kInvalidSocket) return -1;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(s, &fds);
    timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = static_cast<long>(timeout_ms % 1000) * 1000;
    const int rc = ::select(static_cast<int>(s) + 1,
                            want_write ? nullptr : &fds,
                            want_write ? &fds : nullptr,
                            nullptr, &tv);
    if (rc < 0) return -1;
    return rc == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------
// 非阻塞 connect + select 超时（"连不上"要在几百毫秒内失败，不能挂 20 秒）
// ---------------------------------------------------------------------
inline bool connect_with_timeout(bms_socket_t s, const sockaddr* addr,
                                 int addr_len, int timeout_ms) {
#ifdef _WIN32
    u_long nb = 1;
    ::ioctlsocket(s, FIONBIO, &nb);
#else
    const int fl = ::fcntl(s, F_GETFL, 0);
    ::fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif

    bool ok = false;
    if (::connect(s, addr, addr_len) == 0) {
        ok = true;
    } else if (sock_would_block(last_socket_error())) {
        if (wait_ready(s, /*want_write=*/true, timeout_ms) == 1) {
            int so_err = 0;
            bms_socklen_t sl = static_cast<bms_socklen_t>(sizeof(so_err));
            if (::getsockopt(s, SOL_SOCKET, SO_ERROR,
                             reinterpret_cast<bms_optval_t>(&so_err), &sl) == 0 &&
                so_err == 0) {
                ok = true;
            }
        }
    }

#ifdef _WIN32
    u_long bl = 0;
    ::ioctlsocket(s, FIONBIO, &bl);
#else
    ::fcntl(s, F_SETFL, fl);
#endif
    return ok;
}

// ---------------------------------------------------------------------
// TcpSocket —— 一个已连接（或未连接）的 TCP 端点
// 只管字节：连接 / 断开 / 发满 / 收若干 / 收满
// ---------------------------------------------------------------------
class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket() { close(); }
    TcpSocket(const TcpSocket&)            = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    bool connect_to(const std::string& host, uint16_t port, int timeout_ms = 3000) {
        if (!SocketRuntime::ensure()) { err_ = -1; return false; }
        close();

        addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* res = nullptr;
        const std::string port_str = std::to_string(static_cast<unsigned>(port));
        if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || res == nullptr) {
            err_ = -2;
            return false;
        }

        bool ok = false;
        for (addrinfo* p = res; p != nullptr && !ok; p = p->ai_next) {
            const bms_socket_t s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (s == kInvalidSocket) continue;
            if (connect_with_timeout(s, p->ai_addr, static_cast<int>(p->ai_addrlen), timeout_ms)) {
                fd_        = s;
                connected_ = true;
                ok         = true;
            } else {
                err_ = last_socket_error();
                sock_close(s);
            }
        }
        ::freeaddrinfo(res);
        return ok;
    }

    void close() {
        if (fd_ != kInvalidSocket) sock_close(fd_);
        fd_        = kInvalidSocket;
        connected_ = false;
    }

    bool is_open() const { return fd_ != kInvalidSocket; }
    bool is_connected() const { return connected_; }
    int  last_error() const { return err_; }
    bms_socket_t raw() const { return fd_; }

    // 测试/服务端用：接管一个已经连上的 socket
    void adopt(bms_socket_t fd) {
        close();
        fd_        = fd;
        connected_ = true;
    }

    void set_nodelay(bool on) {
        if (fd_ == kInvalidSocket) return;
        const int v = on ? 1 : 0;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&v), static_cast<bms_socklen_t>(sizeof(v)));
    }

    bool send_all(const uint8_t* buf, size_t len, int timeout_ms = 1000) {
        if (!connected_ || fd_ == kInvalidSocket) { err_ = -3; return false; }
        size_t off = 0;
        while (off < len) {
            if (wait_ready(fd_, /*want_write=*/true, timeout_ms) != 1) { err_ = -4; return false; }
            const int n = ::send(fd_, reinterpret_cast<const char*>(buf + off),
                                 static_cast<int>(len - off), 0);
            if (n <= 0) {
                const int e = last_socket_error();
                if (sock_would_block(e)) continue;
                err_ = e;
                connected_ = false;
                return false;
            }
            off += static_cast<size_t>(n);
        }
        return true;
    }

    // 返回 >0 = 收到的字节数；0 = 超时（链路仍在）；-1 = 链路错误 / 对端关闭
    int recv_some(uint8_t* buf, size_t cap, int timeout_ms) {
        if (!connected_ || fd_ == kInvalidSocket) { err_ = -5; return -1; }
        if (cap == 0) return 0;
        const int r = wait_ready(fd_, /*want_write=*/false, timeout_ms);
        if (r < 0) { err_ = -6; connected_ = false; return -1; }
        if (r == 0) return 0;
        const int n = ::recv(fd_, reinterpret_cast<char*>(buf), static_cast<int>(cap), 0);
        if (n == 0) { err_ = -7; connected_ = false; return -1; }   // 对端优雅关闭
        if (n < 0) {
            const int e = last_socket_error();
            if (sock_would_block(e)) return 0;
            err_ = e;
            connected_ = false;
            return -1;
        }
        return n;
    }

    // 收满 need 字节（Modbus 分帧用：先 7 字节 MBAP，再 Length 指定长度）
    bool recv_exact(uint8_t* buf, size_t need, int timeout_ms) {
        size_t off = 0;
        while (off < need) {
            const int n = recv_some(buf + off, need - off, timeout_ms);
            if (n <= 0) { err_ = -8; return false; }
            off += static_cast<size_t>(n);
        }
        return true;
    }

private:
    bms_socket_t fd_        = kInvalidSocket;
    bool         connected_ = false;
    int          err_       = 0;
};

// ---------------------------------------------------------------------
// MBAP 分帧：Length 字段含 UnitId(1) + PDU，故"MBAP 之后的字节数" = Length - 1
// 整帧 ADU 长度 = 6 + Length = 7 + (Length - 1)
// ---------------------------------------------------------------------
inline size_t mbap_body_len(const uint8_t* mbap) {
    const size_t length = (static_cast<size_t>(mbap[4]) << 8) | static_cast<size_t>(mbap[5]);
    return (length >= 1) ? (length - 1) : 0;
}

// =====================================================================
// ModbusTcpTransport —— 现场：EMS(主站) ↔ PCS / BMS / 电表
// =====================================================================
class ModbusTcpTransport : public IModbusTransport {
public:
    ModbusTcpTransport() = default;
    ModbusTcpTransport(std::string host, uint16_t port)
        : host_(std::move(host)), port_(port) {}

    void set_endpoint(std::string host, uint16_t port) {
        host_ = std::move(host);
        port_ = port;
    }
    void set_timeout_ms(int ms)      { timeout_ms_ = ms; }
    void set_connect_timeout_ms(int ms) { connect_timeout_ms_ = ms; }
    // 0 = 不校验；非 0 = 响应 UnitId 必须一致（防串话到总线上别的从站）
    void set_expect_unit_id(uint8_t uid) { expect_unit_ = uid; }

    bool connect() override {
        if (!sock_.connect_to(host_, port_, connect_timeout_ms_)) { ++link_errors_; return false; }
        sock_.set_nodelay(true);
        ++connects_;
        return true;
    }
    void close() override { sock_.close(); }
    bool connected() const override { return sock_.is_connected(); }

    bool transact(const uint8_t* req, size_t req_len,
                  uint8_t* resp, size_t resp_cap, size_t* resp_len) override {
        if (resp_len != nullptr) *resp_len = 0;
        if (req == nullptr || resp == nullptr ||
            req_len < modbus::kMbapLen + 1 || resp_cap < modbus::kMbapLen + 1) {
            ++format_errors_;
            return false;
        }
        ++transactions_;
        if (!sock_.send_all(req, req_len, timeout_ms_)) { ++link_errors_; return false; }

        // 1) 精确读 7 字节 MBAP
        uint8_t hdr[modbus::kMbapLen];
        if (!sock_.recv_exact(hdr, sizeof(hdr), timeout_ms_)) { ++timeouts_; return false; }

        // 2) 按 Length 精确读剩余
        const size_t body = mbap_body_len(hdr);
        if (body == 0 || body > modbus::kMaxPduLen) { ++format_errors_; return false; }
        if (7 + body > resp_cap)                    { ++format_errors_; return false; }

        // 3) 事务号回显一致（不一致 = 错位/串话，直接丢弃而不是喂给解析器）
        if (hdr[0] != req[0] || hdr[1] != req[1])   { ++format_errors_; return false; }
        if (expect_unit_ != 0 && hdr[6] != expect_unit_) { ++format_errors_; return false; }

        std::memcpy(resp, hdr, sizeof(hdr));
        if (!sock_.recv_exact(resp + modbus::kMbapLen, body, timeout_ms_)) {
            ++timeouts_;
            return false;
        }
        *resp_len = modbus::kMbapLen + body;
        return true;
    }

    const std::string& host() const { return host_; }
    uint16_t           port() const { return port_; }

    // 诊断（与适配器的 stale_reads/timeouts 互补：这里看链路层）
    int transactions()  const { return transactions_; }
    int timeouts()      const { return timeouts_; }
    int link_errors()   const { return link_errors_; }
    int format_errors() const { return format_errors_; }
    int connects()      const { return connects_; }

private:
    TcpSocket   sock_;
    std::string host_ = "127.0.0.1";
    uint16_t    port_ = 502;
    int         timeout_ms_         = 500;
    int         connect_timeout_ms_ = 3000;
    uint8_t     expect_unit_        = 0;

    int transactions_  = 0;
    int timeouts_      = 0;
    int link_errors_   = 0;
    int format_errors_ = 0;
    int connects_      = 0;
};

// =====================================================================
// Iec104TcpTransport —— 现场：EMS(受控站) ↔ 调度主站 / 网关
//
// 与 Modbus 的差别：没有"一问一答"。STARTDT / 总召唤由上层适配器发起，
// 本层只负责把字节搬进搬出。**粘包由上层 parse_apdu 处理**（返回 0 = 半个帧）。
// =====================================================================
class Iec104TcpTransport : public IIec104Transport {
public:
    Iec104TcpTransport() = default;
    Iec104TcpTransport(std::string host, uint16_t port)
        : host_(std::move(host)), port_(port) {}

    void set_endpoint(std::string host, uint16_t port) {
        host_ = std::move(host);
        port_ = port;
    }
    void set_send_timeout_ms(int ms)    { send_timeout_ms_ = ms; }
    void set_connect_timeout_ms(int ms) { connect_timeout_ms_ = ms; }

    // 最小等待窗口。适配器的 open()/总召唤用 drain_rx(0)（非阻塞）等确认帧，
    // 在**环回**下响应是同调用栈内产生的所以立刻可见；跨 TCP 时响应要等对端调度，
    // 非阻塞会一直收不到。给一个最小等待窗口即可让同一份适配器代码在两种介质下都成立。
    // 0 = 严格非阻塞（默认，现场自定时从站用这个）。
    void set_min_wait_ms(int ms) { min_wait_ms_ = ms; }
    int  min_wait_ms() const { return min_wait_ms_; }

    bool connect() override {
        if (!sock_.connect_to(host_, port_, connect_timeout_ms_)) { ++link_errors_; return false; }
        sock_.set_nodelay(true);
        ++connects_;
        return true;
    }
    void close() override { sock_.close(); }
    bool connected() const override { return sock_.is_connected(); }

    bool send(const uint8_t* buf, size_t len) override {
        if (!sock_.send_all(buf, len, send_timeout_ms_)) { ++link_errors_; return false; }
        bytes_tx_ += len;
        ++sends_;
        return true;
    }

    // 三态：false = 链路错误；true 且 *len == 0 = 这一轮没有报文；true 且 *len > 0 = 收到数据
    bool receive(uint8_t* buf, size_t cap, size_t* len, int timeout_ms) override {
        if (len != nullptr) *len = 0;
        const int wait = (timeout_ms > min_wait_ms_) ? timeout_ms : min_wait_ms_;
        const int n = sock_.recv_some(buf, cap, wait);
        if (n < 0) { ++link_errors_; return false; }
        if (n == 0) { ++idle_rounds_; return true; }
        *len       = static_cast<size_t>(n);
        bytes_rx_ += static_cast<size_t>(n);
        ++receives_;
        return true;
    }

    const std::string& host() const { return host_; }
    uint16_t           port() const { return port_; }

    int sends()      const { return sends_; }
    int receives()   const { return receives_; }
    int idle_rounds() const { return idle_rounds_; }
    int link_errors() const { return link_errors_; }
    int connects()   const { return connects_; }
    size_t bytes_tx() const { return bytes_tx_; }
    size_t bytes_rx() const { return bytes_rx_; }

private:
    TcpSocket   sock_;
    std::string host_ = "127.0.0.1";
    uint16_t    port_ = 2404;
    int         send_timeout_ms_    = 1000;
    int         connect_timeout_ms_ = 3000;
    int         min_wait_ms_        = 0;

    int    sends_       = 0;
    int    receives_    = 0;
    int    idle_rounds_ = 0;
    int    link_errors_ = 0;
    int    connects_    = 0;
    size_t bytes_tx_    = 0;
    size_t bytes_rx_    = 0;
};

} // namespace net
} // namespace ems
