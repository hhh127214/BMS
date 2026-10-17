// =====================================================================
// 21/ — TCP 仿真从站（测试辅助工具，不属于交付物）
//
// 用途：把进程内假表挂到一个 TCP 端口上，**模拟串口服务器**。
// 它可以让 `meter645.exe --tcp` 这条路第一次有真实收发证据 ——
// 否则 TcpLink 只有"编译通过"，那不叫验证。
//
// 用法：
//   sim_tcp_server.exe [--port 8899] [--seconds 20] [--addr 000000000001]
//                      [--corrupt-cs] [--drop-first N]
//
// ★ 为什么单独做一个 exe 而不是塞进测试里：
//   TCP 是**跨进程**的。同进程里 socket 回环也能跑，但那样测的是
//   "winsock 本身"，而不是"我们的链路实现对着一个真实的、会分片的
//   字节流能不能收敛"。跨进程之后，recv 返回半个帧是常态 —— 那才是
//   现场的样子。
// =====================================================================

#include "dlt645_frame.h"
#include "dlt645_transport.h"
#include "fake_meter_slave.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace ems::dlt645;

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    unsigned short port = 8899;
    int seconds = 20;
    std::string addr_s = "000000000001";
    bool corrupt_cs = false;
    int drop_first = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        if (k == "--port" && i + 1 < argc) port = static_cast<unsigned short>(std::atoi(argv[++i]));
        else if (k == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (k == "--addr" && i + 1 < argc) addr_s = argv[++i];
        else if (k == "--corrupt-cs") corrupt_cs = true;
        else if (k == "--drop-first" && i + 1 < argc) drop_first = std::atoi(argv[++i]);
        else {
            std::printf("[FAIL] 未知参数: %s\n", k.c_str());
            return 2;
        }
    }

    Address addr;
    if (!addr.from_string(addr_s)) {
        std::printf("[FAIL] 地址非法: %s\n", addr_s.c_str());
        return 2;
    }

    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
        std::printf("[FAIL] WSAStartup\n");
        return 1;
    }

    SOCKET srv = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) { std::printf("[FAIL] socket\n"); return 1; }

    BOOL yes = TRUE;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(srv, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        std::printf("[FAIL] bind %u\n", port);
        return 1;
    }
    if (::listen(srv, 1) != 0) { std::printf("[FAIL] listen\n"); return 1; }

    std::printf("[READY] listening on 127.0.0.1:%u, addr=%s\n", port, addr_s.c_str());

    // 只服务一个连接：这是联调工具，不是网关。
    SOCKET cli = ::accept(srv, nullptr, nullptr);
    if (cli == INVALID_SOCKET) { std::printf("[FAIL] accept\n"); return 1; }
    std::printf("[CONN] client connected\n");

    BytePipe from_client;
    BytePipe to_client;
    test::FakeMeterSlave slave(&from_client, &to_client, addr);
    test::FakeMeterSlave::Options opt;
    opt.corrupt_cs = corrupt_cs;
    opt.drop_first = drop_first;
    slave.set_options(opt);

    // 灌一批会变的数：与 CLI 的 --sim 同一套，便于对照
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        const double el = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        if (el > seconds) {
            std::printf("[DONE] %d s 到期\n", seconds);
            break;
        }
        const double phase = el * 0.7;
        slave.set_value(0x02010100u, 220.0 + 2.0 * std::sin(phase));
        slave.set_value(0x02010200u, 221.0 + 2.0 * std::sin(phase + 2.1));
        slave.set_value(0x02010300u, 219.5 + 2.0 * std::sin(phase + 4.2));
        slave.set_value(0x02030000u, 5.0 * std::sin(phase * 0.5));

        // 收：socket → pipe（**允许半帧**，这正是要测的）
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(cli, &rfds);
        timeval tv{0, 20000};   // 20 ms
        const int sel = ::select(0, &rfds, nullptr, nullptr, &tv);
        if (sel > 0) {
            std::uint8_t buf[512];
            const int got = ::recv(cli, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (got == 0) { std::printf("[DONE] 对端关闭\n"); break; }
            if (got < 0) { std::printf("[FAIL] recv\n"); break; }
            from_client.push(buf, static_cast<std::size_t>(got));
        }

        // 驱动假表
        slave.poll();

        // 发：pipe → socket
        std::uint8_t out[512];
        for (;;) {
            const std::size_t n = to_client.take(out, sizeof(out));
            if (n == 0) break;
            if (::send(cli, reinterpret_cast<const char*>(out), static_cast<int>(n), 0) < 0) {
                std::printf("[FAIL] send\n");
                break;
            }
        }
    }

    std::printf("[STAT] rx_frames=%d tx_frames=%d bad_frames=%d ignored=%d\n",
                slave.rx_frames(), slave.tx_frames(), slave.bad_frames(), slave.ignored());

    ::closesocket(cli);
    ::closesocket(srv);
    WSACleanup();
    return 0;
}
