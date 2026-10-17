// =====================================================================
// 21/ — Win32 串口链路（RS485 / RS232）
//
// 645 的原生物理层就是串口：默认 2400 bps · 8 数据位 · **偶校验** · 1 停止位。
// 现场大量表仍是 8E1，也有厂家改成 8N1 / 9600 / 2400——所以参数必须可配。
//
// ---------------------------------------------------------------------
// ★★ 本文件的验证状态（不许含糊）
// ---------------------------------------------------------------------
// **未在真实 RS485 硬件上验证。**
// 本机没有 485 转换器，也没有真表，所以这条路只做到了"编译通过 +
// 参数设置符合 Windows API 语义"，**没有一次真实收发**。
// 上现场前必须做的三件事（按顺序）：
//   1) 用 21/scripts/run_loop.bat --sim 先确认主站逻辑本身没问题；
//   2) 用 --tcp 接串口服务器（`dlt645_tcp_link.h`，那条路可自测），
//      确认组帧/解析/超时在真实链路上成立；
//   3) 最后才接真表，且先用 --once 读一个已知点（如 A 相电压）核对 BCD 小数位。
// 把"未验证"写成注释而不是留在脑子里，是为了避免下一次交付时
// 有人把"编译通过"当成"能用"。
//
// ---------------------------------------------------------------------
// ★ 两个串口特有的坑
// ---------------------------------------------------------------------
// ① **每次请求前必须 PurgeComm**。485 是半双工：主站发完切到收，
//    若上一轮的超时应答迟到，它会在驱动缓冲里躺着，被下一轮读成
//    "本次应答" —— 值全对，但全是上一次的。
// ② 645 要求**帧内字节间隔 ≤ T0.5s**，超时按"帧"算而不是按"字节"算。
//    所以 COMMTIMEOUTS 要配成"有数据立即返回、没数据等 total timeout"，
//    而不是"等满 timeout 才返回整块"——后者会把一条慢表的应答切碎。
// =====================================================================

#pragma once

#include "dlt645_transport.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <string>

namespace ems {
namespace dlt645 {

struct SerialConfig {
    unsigned int baud = 2400;
    bool even_parity = true;   // 645 默认 8E1
    int stop_bits = 1;
};

class SerialLink : public IByteLink {
public:
    SerialLink(std::string port, const SerialConfig& cfg = SerialConfig())
        : port_(std::move(port)), cfg_(cfg) {}

    ~SerialLink() override { close(); }

    bool open() override {
        close();
        // COM10 及以上必须用 \\.\ 前缀，COM1~9 用也行
        std::string path = port_;
        if (path.rfind("\\\\.\\", 0) != 0) path = "\\\\.\\" + path;

        h_ = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            last_err_ = ::GetLastError();
            h_ = nullptr;
            return false;
        }

        DCB dcb;
        std::memset(&dcb, 0, sizeof(dcb));
        dcb.DCBlength = sizeof(dcb);
        if (!::GetCommState(h_, &dcb)) {
            close();
            return false;
        }
        dcb.BaudRate = cfg_.baud;
        dcb.ByteSize = 8;
        dcb.Parity = cfg_.even_parity ? EVENPARITY : NOPARITY;
        dcb.StopBits = (cfg_.stop_bits == 2) ? TWOSTOPBITS : ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = cfg_.even_parity ? TRUE : FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;   // 485 方向由转换器自动切
        if (!::SetCommState(h_, &dcb)) {
            close();
            return false;
        }

        ::SetupComm(h_, 1024, 1024);
        open_ = true;
        return true;
    }

    void close() override {
        if (h_ != nullptr) {
            ::CloseHandle(h_);
            h_ = nullptr;
        }
        open_ = false;
    }

    bool is_open() const override { return open_; }

    int write(const std::uint8_t* p, std::size_t n) override {
        if (!open_) return -1;
        DWORD wrote = 0;
        if (!::WriteFile(h_, p, static_cast<DWORD>(n), &wrote, nullptr)) return -1;
        return static_cast<int>(wrote);
    }

    int read(std::uint8_t* p, std::size_t n, int timeout_ms) override {
        if (!open_) return -1;

        COMMTIMEOUTS to;
        std::memset(&to, 0, sizeof(to));
        // ① 立即返回已到达的字节（间隔超时给满）
        to.ReadIntervalTimeout = MAXDWORD;
        // ② 一个字节都没有时，最多等 timeout_ms
        to.ReadTotalTimeoutMultiplier = MAXDWORD;
        to.ReadTotalTimeoutConstant = static_cast<DWORD>(timeout_ms);
        ::SetCommTimeouts(h_, &to);

        DWORD got = 0;
        if (!::ReadFile(h_, p, static_cast<DWORD>(n), &got, nullptr)) return -1;
        return static_cast<int>(got);   // 0 = 超时
    }

    void flush_input() override {
        if (!open_) return;
        ::PurgeComm(h_, PURGE_RXCLEAR | PURGE_TXCLEAR);
    }

    unsigned long last_error() const { return last_err_; }

private:
    std::string port_;
    SerialConfig cfg_;
    HANDLE h_ = nullptr;
    bool open_ = false;
    unsigned long last_err_ = 0;
};

}  // namespace dlt645
}  // namespace ems

#endif  // _WIN32
