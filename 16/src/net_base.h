// =====================================================================
// 16/ — 网络底层小工具（内部共享头，不是模块的对外交付面）
//
// 为什么要有这个文件（而不是让 sntp.h / ftp.h / tls.h 各写一遍）：
//   ① **Windows 头文件顺序纪律只有一处**：`<winsock2.h>` 必须在
//      `<windows.h>` 之前。散在三处写，迟早有人在某一处写反，
//      报错是几十行 "redefinition of 'fd_set'" 之类的噪声。
//   ② WSAStartup 必须全进程只做一次。三处各自 static 标记的话，
//      谁先被调用就决定谁生效 —— 这不是缺陷，但会让"哪一层在初始化"
//      变得不可预测。
//   ③ 三个模块的 socket 生命周期/超时/半包语义必须**完全一致**：
//      SNTP 的 48 字节报文、FTP 的数据连接、TLS 的握手字节流，
//      对"0 字节 = 对端关闭" vs "<0 = 出错/超时"的解释不能有分歧。
//
// 本文件刻意只有 ~200 行、无第三方依赖、纯头文件。
// =====================================================================

#pragma once

// ---------------------------------------------------------------------
// ★ 顺序纪律：winsock2.h → ws2tcpip.h → windows.h
//   <windows.h> 会拉进老的 <winsock.h>，与 winsock2 的类型/宏冲突。
//   WIN32_LEAN_AND_MEAN 能挡掉一部分，但挡不全（winsock.h 仍可能被间接引入），
//   所以顺序本身也要对。
// ---------------------------------------------------------------------
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX     // 否则 windows.h 里的 min/max 宏会撞 std::min/std::max
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#else
#  include <arpa/inet.h>
#  include <cerrno>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ems {
namespace comm {
namespace net {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

// ---------------------------------------------------------------------
// WSAStartup —— 全进程一次
//
// 为什么用函数内 static 而不是全局对象：全局对象的构造顺序在跨 TU 时
// 未定义，而 WSAStartup 必须在**任何** socket 调用之前完成。
// 函数内 static 的初始化在 C++11 起是线程安全的。
// ---------------------------------------------------------------------
inline bool ensure_winsock() {
#ifdef _WIN32
    static bool done = false;
    if (!done) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return false;
        done = true;
    }
#endif
    return true;
}

inline void close_socket(socket_t s) {
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

inline int last_socket_error() {
#ifdef _WIN32
    return (int)WSAGetLastError();
#else
    return errno;
#endif
}

// 把"可重试"类错误（连接进行中/超时）与"真错误"分开。
// ★ 不做这个区分的话，"连接超时"会被当成"连接被拒绝"报出来，
//   现场排查方向就完全错了（超时→查网络/路由；拒绝→查服务端端口）。
inline bool is_in_progress(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS ||
           e == WSAEALREADY   || e == WSAEINTR;
#else
    return e == EINPROGRESS || e == EWOULDBLOCK || e == EAGAIN || e == EINTR;
#endif
}

inline bool is_timeout(int e) {
#ifdef _WIN32
    return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
    return e == EAGAIN || e == EWOULDBLOCK || e == ETIMEDOUT;
#endif
}

inline bool set_nonblocking(socket_t s, bool on) {
#ifdef _WIN32
    u_long v = on ? 1 : 0;
    return ::ioctlsocket(s, FIONBIO, &v) == 0;
#else
    const int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}

inline bool set_recv_timeout(socket_t s, int timeout_ms) {
    if (timeout_ms <= 0) timeout_ms = 1;
#ifdef _WIN32
    DWORD tv = (DWORD)timeout_ms;
    return ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) == 0;
#else
    timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

inline bool set_send_timeout(socket_t s, int timeout_ms) {
    if (timeout_ms <= 0) timeout_ms = 1;
#ifdef _WIN32
    DWORD tv = (DWORD)timeout_ms;
    return ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv)) == 0;
#else
    timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

inline bool set_nodelay(socket_t s) {
    int one = 1;
    return ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one)) == 0;
}

inline void shutdown_send(socket_t s) {
    if (s == kInvalidSocket) return;
    ::shutdown(s, 1 /*SD_SEND*/);
}

// 解析 host（域名或点分十进制）为 IPv4/IPv6 地址列表。
// 统一走 getaddrinfo —— 不用 inet_pton：MinGW-w64 的 <ws2tcpip.h> 在
// g++ 8.1 自带的那一版里没有声明它（只有当 _WIN32_WINNT >= 0x0600 时才有），
// 报错是 "'inet_pton' was not declared in this scope"，像拼写错误、实为 SDK 门槛。
struct ResolvedAddr {
    sockaddr_storage storage;
    int              len = 0;
};

inline bool resolve(const std::string& host, std::uint16_t port,
                    int family, std::vector<ResolvedAddr>& out, std::string& err) {
    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family   = family;          // AF_UNSPEC = 两者都要
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), port_str, &hints, &res);
    if (rc != 0 || res == nullptr) {
#ifdef _WIN32
        err = "无法解析主机 " + host + ":" + port_str;
#else
        err = std::string("无法解析主机 ") + host + ":" + port_str + "（" + gai_strerror(rc) + "）";
#endif
        return false;
    }
    out.clear();
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        if (ai->ai_addrlen == 0 || ai->ai_addrlen > (int)sizeof(sockaddr_storage)) continue;
        ResolvedAddr a;
        std::memcpy(&a.storage, ai->ai_addr, (std::size_t)ai->ai_addrlen);
        a.len = (int)ai->ai_addrlen;
        out.push_back(a);
    }
    ::freeaddrinfo(res);
    if (out.empty()) { err = "主机无可用地址：" + host; return false; }
    return true;
}

// =====================================================================
// TCP 客户端套接字
//
// 语义约定（三个模块共用，不允许各写一套）：
//   recv_some()  > 0 → 读到的字节数
//                = 0 → **对端正常关闭**（EOF）
//                < 0 → 出错或超时（用 error() / timed_out() 区分）
//   send_all()   true = 全部写完；false = 出错（error() 给出原因）
// =====================================================================
class TcpSocket {
public:
    TcpSocket() {}
    ~TcpSocket() { close(); }
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    // 带超时的连接：先非阻塞 connect，再 select 等可写。
    // 为什么不用阻塞 connect：对一个"路由黑洞"地址，阻塞 connect 会挂到
    // 操作系统自己的超时（Windows 上默认约 21 秒），一个不该联网的
    // 模块能因此把一个控制周期拖死。
    bool connect_to(const std::string& host, std::uint16_t port, int timeout_ms,
                    std::string& err) {
        err.clear();
        if (!ensure_winsock()) { err = "Winsock 初始化失败"; return false; }
        close();

        std::vector<ResolvedAddr> addrs;
        if (!resolve(host, port, AF_UNSPEC, addrs, err)) return false;

        std::string last_err = "连接失败";
        for (std::size_t i = 0; i < addrs.size(); ++i) {
            const socket_t s = ::socket(addrs[i].storage.ss_family, SOCK_STREAM, 0);
            if (s == kInvalidSocket) {
                last_err = "创建套接字失败（err=" + std::to_string(last_socket_error()) + "）";
                continue;
            }
            set_nonblocking(s, true);
            const int rc = ::connect(s, (sockaddr*)&addrs[i].storage, addrs[i].len);
            bool ok = (rc == 0);
            if (!ok) {
                const int e = last_socket_error();
                if (!is_in_progress(e)) {
                    last_err = "连接被拒绝（err=" + std::to_string(e) + "）";
                    close_socket(s);
                    continue;
                }
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(s, &wfds);
                timeval tv;
                tv.tv_sec  = timeout_ms / 1000;
                tv.tv_usec = (timeout_ms % 1000) * 1000;
                const int sel = ::select((int)(s + 1), nullptr, &wfds, nullptr, &tv);
                if (sel == 0) {
                    last_err = "连接超时（" + std::to_string(timeout_ms) + " ms）";
                    close_socket(s);
                    continue;
                }
                if (sel < 0) {
                    last_err = "select 失败（err=" + std::to_string(last_socket_error()) + "）";
                    close_socket(s);
                    continue;
                }
                int soerr = 0;
#ifdef _WIN32
                int slen = (int)sizeof(soerr);
#else
                socklen_t slen = sizeof(soerr);
#endif
                ::getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &slen);
                if (soerr != 0) {
                    last_err = "连接失败（err=" + std::to_string(soerr) + "）";
                    close_socket(s);
                    continue;
                }
                ok = true;
            }
            set_nonblocking(s, false);
            set_recv_timeout(s, timeout_ms);
            set_send_timeout(s, timeout_ms);
            set_nodelay(s);
            sock_ = s;
            peer_ = host + ":" + std::to_string((unsigned)port);
            buffer_.clear();
            return true;
        }
        err = last_err;
        return false;
    }

    void close() {
        if (sock_ != kInvalidSocket) { close_socket(sock_); sock_ = kInvalidSocket; }
        buffer_.clear();
    }

    bool is_open() const { return sock_ != kInvalidSocket; }
    socket_t handle() const { return sock_; }
    const std::string& peer() const { return peer_; }
    const std::string& error() const { return error_; }
    int  recv_timeouts() const { return recv_timeouts_; }
    std::size_t buffered() const { return buffer_.size(); }

    void shutdown_send() { net::shutdown_send(sock_); }

    bool send_all(const std::uint8_t* p, std::size_t n) {
        if (sock_ == kInvalidSocket) { error_ = "套接字未打开"; return false; }
        std::size_t sent = 0;
        while (sent < n) {
            const int rc = ::send(sock_, (const char*)(p + sent), (int)(n - sent), 0);
            if (rc <= 0) {
                error_ = "发送失败（err=" + std::to_string(last_socket_error()) + "）";
                return false;
            }
            sent += (std::size_t)rc;
        }
        return true;
    }

    bool send_all(const std::string& s) {
        return send_all((const std::uint8_t*)s.data(), s.size());
    }

    // 见类头的语义约定
    int recv_some(std::uint8_t* p, std::size_t n) {
        if (sock_ == kInvalidSocket) { error_ = "套接字未打开"; return -1; }
        const int rc = ::recv(sock_, (char*)p, (int)n, 0);
        if (rc == 0) return 0;                       // 对端关闭
        if (rc < 0) {
            const int e = last_socket_error();
            if (is_timeout(e)) { ++recv_timeouts_; error_ = "接收超时"; }
            else               { error_ = "接收失败（err=" + std::to_string(e) + "）"; }
            return -1;
        }
        return rc;
    }

    bool timed_out() const {
        return error_.find("超时") != std::string::npos;
    }

    // 读到对端关闭为止（有上限，防"永不关闭的对端"把内存吃光）
    int recv_until_eof(std::vector<std::uint8_t>& out, std::size_t max_bytes,
                       std::size_t chunk = 8192) {
        std::vector<std::uint8_t> tmp(chunk);
        int rounds = 0;
        for (;;) {
            const int n = recv_some(tmp.data(), chunk);
            if (n == 0) return 0;                    // 正常 EOF
            if (n < 0)  return timed_out() ? 1 : -1; // 1 = 超时但已有数据可用
            out.insert(out.end(), tmp.begin(), tmp.begin() + n);
            if (out.size() >= max_bytes) return 2;   // 达到上限，主动截断
            ++rounds;
            if (rounds > 1000000) return 3;
        }
    }

    // 读一行（以 \n 结束；行尾的 \r 去掉）。返回 false 表示 EOF 或出错。
    bool recv_line(std::string& line, int timeout_ms) {
        line.clear();
        if (sock_ == kInvalidSocket) { error_ = "套接字未打开"; return false; }
        for (;;) {
            const std::size_t nl = buffer_.find('\n');
            if (nl != std::string::npos) {
                line.assign(buffer_, 0, nl);
                buffer_.erase(0, nl + 1);
                if (!line.empty() && line[line.size() - 1] == '\r') {
                    line.erase(line.size() - 1);
                }
                return true;
            }
            set_recv_timeout(sock_, timeout_ms);
            std::uint8_t tmp[4096];
            const int rc = ::recv(sock_, (char*)tmp, (int)sizeof(tmp), 0);
            if (rc == 0) { error_ = "对端关闭连接"; return false; }
            if (rc < 0) {
                const int e = last_socket_error();
                if (is_timeout(e)) { ++recv_timeouts_; error_ = "接收超时"; }
                else               { error_ = "接收失败（err=" + std::to_string(e) + "）"; }
                return false;
            }
            buffer_.append((const char*)tmp, (std::size_t)rc);
        }
    }

    // `::select` 只接受"最大 fd + 1"，Windows 下忽略该参数但也要传对
    bool wait_readable(int timeout_ms) const {
        if (sock_ == kInvalidSocket) return false;
        if (!buffer_.empty()) return true;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock_, &rfds);
        timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        return ::select((int)(sock_ + 1), &rfds, nullptr, nullptr, &tv) > 0;
    }

private:
    socket_t    sock_    = kInvalidSocket;
    std::string peer_;
    std::string error_;
    std::string buffer_;      // recv_line 的行缓冲（TCP 是字节流，半行的归属权在这）
    int         recv_timeouts_ = 0;
};

// =====================================================================
// TCP 监听（测试用的进程内服务端，以及将来"EMS 当服务端"的场景）
// =====================================================================
class TcpListener {
public:
    ~TcpListener() { close(); }
    TcpListener() {}
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    bool listen_on(std::uint16_t port, int backlog, std::string& err,
                   const char* bind_addr = "127.0.0.1") {
        err.clear();
        if (!ensure_winsock()) { err = "Winsock 初始化失败"; return false; }
        close();
        std::vector<ResolvedAddr> addrs;
        // 监听用 SOCK_STREAM 的地址解析：这里临时把 type 换掉
        char port_str[16];
        std::snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
        addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags    = AI_PASSIVE;
        addrinfo* res = nullptr;
        if (::getaddrinfo(bind_addr, port_str, &hints, &res) != 0 || res == nullptr) {
            err = "监听地址解析失败";
            return false;
        }
        const socket_t s = ::socket(res->ai_family, SOCK_STREAM, 0);
        if (s == kInvalidSocket) {
            ::freeaddrinfo(res);
            err = "创建监听套接字失败";
            return false;
        }
        // ★ SO_REUSEADDR：测试里反复起同一个端口（2121/12345）时，
        //   TIME_WAIT 会让 bind 失败 —— 这是"第二次跑测试才红"的经典来源。
        int one = 1;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
        if (::bind(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
            ::freeaddrinfo(res);
            close_socket(s);
            err = "绑定端口失败（err=" + std::to_string(last_socket_error()) + "）";
            return false;
        }
        ::freeaddrinfo(res);
        if (::listen(s, backlog) != 0) {
            close_socket(s);
            err = "listen 失败";
            return false;
        }
        sock_ = s;
        return true;
    }

    // 端口填 0 → 由系统分配，用 bound_port() 取回实际值
    std::uint16_t bound_port() const {
        if (sock_ == kInvalidSocket) return 0;
        sockaddr_in a;
        std::memset(&a, 0, sizeof(a));
#ifdef _WIN32
        int len = (int)sizeof(a);
#else
        socklen_t len = sizeof(a);
#endif
        if (::getsockname(sock_, (sockaddr*)&a, &len) != 0) return 0;
        return ntohs(a.sin_port);
    }

    // 有连接就接受；超时返回 kInvalidSocket（并把 timed_out 置位）
    socket_t accept_one(int timeout_ms, bool& timed_out) {
        timed_out = false;
        if (sock_ == kInvalidSocket) return kInvalidSocket;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock_, &rfds);
        timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        const int sel = ::select((int)(sock_ + 1), &rfds, nullptr, nullptr, &tv);
        if (sel == 0) { timed_out = true; return kInvalidSocket; }
        if (sel < 0) return kInvalidSocket;
        return ::accept(sock_, nullptr, nullptr);
    }

    void close() {
        if (sock_ != kInvalidSocket) { close_socket(sock_); sock_ = kInvalidSocket; }
    }
    bool is_open() const { return sock_ != kInvalidSocket; }

private:
    socket_t sock_ = kInvalidSocket;
};

}  // namespace net
}  // namespace comm
}  // namespace ems
