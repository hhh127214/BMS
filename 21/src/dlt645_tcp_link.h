// =====================================================================
// 21/ — TCP 透传链路（串口服务器）
//
// 现场更常见的形态不是"电脑插 485 线"，而是**串口服务器把 COM 口映射成
// TCP 端口**：字节流原样透传，645 帧一个字都不改。
// 所以对协议层来说，"串口" 与 "TCP 透传" 没有区别 —— 都是字节。
//
// ---------------------------------------------------------------------
// ★ 三个必须绕开的坑（都是 13/ Modbus 那边实测踩出来的，这里照搬）
// ---------------------------------------------------------------------
// ① `winsock2.h` **必须在 `windows.h` 之前**。反了会被保护宏挡掉，
//    SOCKET / inet_* 一批符号全"未声明"，而编译器的报错建议
//    （"是否想用 inet_ntoa"）完全是误导。
// ② 阻塞 connect 要等 OS 的 SYN 重传超时（Windows 约 20 s）。
//    设备离线时整个轮询卡 20 秒，看起来像"程序死了"。
//    改**非阻塞 connect + select + SO_ERROR**，实测 300ms 级返回。
// ③ `inet_pton` 的可见性受 `_WIN32_WINNT` 门控 → 用 `inet_addr`。
//
// ---------------------------------------------------------------------
// ★★ 跨平台状态（不许含糊）
// ---------------------------------------------------------------------
// 本文件**同时提供 Windows 与 POSIX 两条 TcpLink 实现**（#ifdef _WIN32 /
// #else）。两条实现接口与行为一致，仅系统调用不同：
//   · Windows：winsock2（WSAStartup / socket / ioctlsocket / closesocket）
//   · POSIX  ：Berkeley socket（socket / fcntl O_NONBLOCK / close）
// 这样 `07/src/main_field.cpp` 经 TCP 接 645 表在 Linux（XM3568 / aarch64）
// 也能直接编过、跑通。
// ⚠ 未实测：本环境是 MinGW（_WIN32 预定义），POSIX 分支无法在此处编译；
//   需在目标 Linux 上做一次 build-verify（与 13/modbus_tcp_client.h 的
//   POSIX 分支同等待遇——那条也只在 Linux 上才被编译）。
// =====================================================================

#pragma once

#include "dlt645_transport.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace ems {
namespace dlt645 {

// Winsock 的初始化/清理。做成"首次使用时初始化一次"，
// 避免每个 link 对象都 WSAStartup（重复调用虽然合法，但会漏 WSACleanup）。
class WinsockGuard {
public:
    static bool ensure() {
        static WinsockGuard g;
        return g.ok_;
    }

private:
    WinsockGuard() {
        WSADATA d;
        ok_ = (WSAStartup(MAKEWORD(2, 2), &d) == 0);
    }
    ~WinsockGuard() { WSACleanup(); }
    bool ok_ = false;
};

class TcpLink : public IByteLink {
public:
    TcpLink(std::string host, unsigned short port)
        : host_(std::move(host)), port_(port) {}

    ~TcpLink() override { close(); }

    bool open() override {
        close();
        if (!WinsockGuard::ensure()) return false;

        sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET) return false;

        sockaddr_in sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port_);
        sa.sin_addr.s_addr = ::inet_addr(host_.c_str());
        if (sa.sin_addr.s_addr == INADDR_NONE) {
            // 不是 IPv4 字面量 → 试 localhost
            if (host_ == "localhost") {
                sa.sin_addr.s_addr = ::inet_addr("127.0.0.1");
            } else {
                ::closesocket(sock_);
                sock_ = INVALID_SOCKET;
                return false;
            }
        }

        // ② 非阻塞 connect
        u_long nb = 1;
        ::ioctlsocket(sock_, FIONBIO, &nb);

        const int rc = ::connect(sock_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
        if (rc == SOCKET_ERROR) {
            const int err = ::WSAGetLastError();
            if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
                ::closesocket(sock_);
                sock_ = INVALID_SOCKET;
                return false;
            }
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(sock_, &wfds);
            timeval tv;
            tv.tv_sec = 3;      // 连接预算 3s：串口服务器通常局域网内，够用
            tv.tv_usec = 0;
            const int sel = ::select(0, nullptr, &wfds, nullptr, &tv);
            if (sel <= 0) {
                ::closesocket(sock_);
                sock_ = INVALID_SOCKET;
                return false;
            }
            int so_err = 0;
            int len = sizeof(so_err);
            ::getsockopt(sock_, SOL_SOCKET, SO_ERROR,
                         reinterpret_cast<char*>(&so_err), &len);
            if (so_err != 0) {
                ::closesocket(sock_);
                sock_ = INVALID_SOCKET;
                return false;
            }
        }

        // 恢复阻塞语义：645 的收发包本来就是"一问一答"，
        // 用 select 控超时比全非阻塞好写，也不容易漏 EAGAIN。
        nb = 0;
        ::ioctlsocket(sock_, FIONBIO, &nb);
        open_ = true;
        return true;
    }

    void close() override {
        if (sock_ != INVALID_SOCKET) {
            ::closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        open_ = false;
    }

    bool is_open() const override { return open_; }

    int write(const std::uint8_t* p, std::size_t n) override {
        if (!open_) return -1;
        const int sent = ::send(sock_, reinterpret_cast<const char*>(p),
                                static_cast<int>(n), 0);
        return sent;
    }

    int read(std::uint8_t* p, std::size_t n, int timeout_ms) override {
        if (!open_) return -1;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock_, &rfds);
        timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        const int sel = ::select(0, &rfds, nullptr, nullptr, &tv);
        if (sel == 0) return 0;          // 超时
        if (sel < 0) return -1;
        const int got = ::recv(sock_, reinterpret_cast<char*>(p),
                               static_cast<int>(n), 0);
        if (got == 0) return -1;         // 对端关闭
        return got;
    }

    void flush_input() override {
        if (!open_) return;
        // 非阻塞地把残留字节抽干（最多 4KB，避免死循环）
        u_long nb = 1;
        ::ioctlsocket(sock_, FIONBIO, &nb);
        std::uint8_t tmp[512];
        int guard = 0;
        while (guard++ < 8) {
            const int got = ::recv(sock_, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
            if (got <= 0) break;
        }
        nb = 0;
        ::ioctlsocket(sock_, FIONBIO, &nb);
    }

private:
    std::string host_;
    unsigned short port_;
    SOCKET sock_ = INVALID_SOCKET;
    bool open_ = false;
};

}  // namespace dlt645
}  // namespace ems

#else  // !_WIN32  →  POSIX（Linux / aarch64，如 XM3568）

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace ems {
namespace dlt645 {

// POSIX 下无需 WSAStartup（BSD socket 随进程自动可用），故无 WinsockGuard 等价物。
// 行为与 Windows 分支逐行对应，仅系统调用不同。
class TcpLink : public IByteLink {
public:
    TcpLink(std::string host, unsigned short port)
        : host_(std::move(host)), port_(port) {}

    ~TcpLink() override { close(); }

    bool open() override {
        close();

        sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ < 0) return false;

        sockaddr_in sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port_);
        sa.sin_addr.s_addr = ::inet_addr(host_.c_str());
        if (sa.sin_addr.s_addr == INADDR_NONE) {
            if (host_ == "localhost") {
                sa.sin_addr.s_addr = ::inet_addr("127.0.0.1");
            } else {
                ::close(sock_);
                sock_ = -1;
                return false;
            }
        }

        // ② 非阻塞 connect（POSIX：fcntl O_NONBLOCK 替代 ioctlsocket FIONBIO）
        int flags = ::fcntl(sock_, F_GETFL, 0);
        ::fcntl(sock_, F_SETFL, flags | O_NONBLOCK);

        const int rc = ::connect(sock_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
        if (rc < 0 && errno != EINPROGRESS) {
            ::close(sock_);
            sock_ = -1;
            return false;
        }
        if (rc < 0) {   // EINPROGRESS → 等可写
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(sock_, &wfds);
            timeval tv;
            tv.tv_sec = 3;      // 连接预算 3s
            tv.tv_usec = 0;
            // POSIX select 第一参是 nfds = max_fd + 1（Windows 传 0）
            const int sel = ::select(sock_ + 1, nullptr, &wfds, nullptr, &tv);
            if (sel <= 0) {
                ::close(sock_);
                sock_ = -1;
                return false;
            }
            int so_err = 0;
            socklen_t len = sizeof(so_err);
            ::getsockopt(sock_, SOL_SOCKET, SO_ERROR, &so_err, &len);
            if (so_err != 0) {
                ::close(sock_);
                sock_ = -1;
                return false;
            }
        }

        // 恢复阻塞语义
        ::fcntl(sock_, F_SETFL, flags);
        open_ = true;
        return true;
    }

    void close() override {
        if (sock_ >= 0) {
            ::close(sock_);
            sock_ = -1;
        }
        open_ = false;
    }

    bool is_open() const override { return open_; }

    int write(const std::uint8_t* p, std::size_t n) override {
        if (!open_) return -1;
        const int sent = ::send(sock_, reinterpret_cast<const char*>(p),
                                static_cast<int>(n), 0);
        return sent;
    }

    int read(std::uint8_t* p, std::size_t n, int timeout_ms) override {
        if (!open_) return -1;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock_, &rfds);
        timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        const int sel = ::select(sock_ + 1, &rfds, nullptr, nullptr, &tv);
        if (sel == 0) return 0;          // 超时
        if (sel < 0) return -1;
        const int got = ::recv(sock_, reinterpret_cast<char*>(p),
                               static_cast<int>(n), 0);
        if (got == 0) return -1;         // 对端关闭
        return got;
    }

    void flush_input() override {
        if (!open_) return;
        int flags = ::fcntl(sock_, F_GETFL, 0);
        ::fcntl(sock_, F_SETFL, flags | O_NONBLOCK);
        std::uint8_t tmp[512];
        int guard = 0;
        while (guard++ < 8) {
            const int got = ::recv(sock_, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
            if (got <= 0) break;
        }
        ::fcntl(sock_, F_SETFL, flags);
    }

private:
    std::string host_;
    unsigned short port_;
    int sock_ = -1;
    bool open_ = false;
};

}  // namespace dlt645
}  // namespace ems

#endif  // _WIN32
