// =====================================================================
// 13/ — 测试用 Modbus TCP 从站（**同进程**，仅用于测试）
//
// 为什么要有这个东西（而不是"直接用 Python 从站测"）：
//   ① Python 从站需要外部环境（解释器 + pymodbus）。它适合做**跨语言联调**，
//      但不适合当单元测试的前置条件 —— 单元测试必须"clone 下来就能跑"。
//   ② 协议层的缺陷（半包、串包、异常码、ByteCount 不符）**必须能被人为造出来**
//      才能验证客户端处理得对。真实从站不会配合你演这些，pymodbus 也不会。
//      本类提供这些"故障注入开关"，于是协议层第一次有了真正的负向测试。
//   ③ 它同时是**可执行的协议文档**：怎么正确应答 FC03/FC04/FC16，看
//      `handle_pdu()` 就知道 —— 比读规范快。
//
// 与 13/sim/modbus_slave.py 的分工：
//   · 本文件（C++，进程内）：协议正确性 + 异常路径 → tests/test_modbus_tcp.cpp
//   · Python 从站（跨进程，跨语言）：真实设备模型 + 闭环联调 → tests/test_modbus_bridge.cpp
//   两者**不是重复**：前者保证"我们的客户端符合协议"，后者保证"我们和别人的
//   实现能对上话"。现场出问题时要能立刻分辨是哪一类。
// =====================================================================

#pragma once

#include "modbus_tcp_client.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace ems {
namespace modbus {
namespace test {

// 复用协议层的平台抽象（Windows: SOCKET / Linux: int）
using socket_t = detail::socket_t;

class FakeModbusSlave {
public:
    static constexpr std::size_t kRegs = 64;   // 寄存器区大小
    static constexpr std::size_t kBits = 64;   // 位区大小

    // -----------------------------------------------------------------
    // 故障注入开关
    //
    // 这些不是"为了测试而测试"的花招 —— 每一条都对应现场真实发生过的现象：
    //   split_response_at  → 半包（跨运营商/VPN/网关转发时最常见）
    //   extra_response_first → 串包（上一轮超时残留的响应）
    //   corrupt_byte_count → 从站固件把 ByteCount 填错（国产表常见）
    //   reject_fc/addr     → 点表配错（读了一个不存在的地址）
    //   silent/delay_ms    → 设备忙/网络抖动
    // -----------------------------------------------------------------
    struct Options {
        std::uint16_t port       = 0;      // 0 = 由 OS 分配（测试首选）
        std::uint8_t  unit_id    = 1;
        std::size_t   split_response_at = 0;   // >0：把响应拆成两次 send
        bool          extra_response_first = false;  // 先发一个事务号不同的假响应
        std::uint8_t  corrupt_byte_count = 0;  // >0：把读响应的 ByteCount 改成它
        std::uint8_t  reject_fc  = 0;      // >0：该功能码一律回"非法功能"
        std::uint16_t reject_addr = 0xFFFF;  // 与 [reject_addr, +reject_qty) 相交则回"非法地址"
        std::uint16_t reject_qty  = 0;
        int           delay_ms   = 0;      // 收到请求后延迟再回
        bool          silent     = false;  // 完全不回（测超时）
    };

    FakeModbusSlave() = default;
    ~FakeModbusSlave() { stop(); }
    FakeModbusSlave(const FakeModbusSlave&)            = delete;
    FakeModbusSlave& operator=(const FakeModbusSlave&) = delete;

    // -----------------------------------------------------------------
    // 生命周期
    //
    // 为什么写成两个重载而不是 `start(const Options& = Options{})`：
    //   GCC 8/9 在"嵌套类带默认成员初始化器 + 用作默认实参"时会报
    //   "default member initializer required before the end of its
    //   enclosing class"。这是编译器限制，不是代码问题 —— 绕开即可。
    // -----------------------------------------------------------------
    bool start(const Options& opt) {
        opt_ = opt;
        if (!detail::net_ready()) return false;

        listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (!detail::socket_valid(listen_)) return false;

        int one = 1;
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&one), sizeof(one));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port        = ::htons(opt_.port);
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            detail::close_socket(listen_);
            listen_ = detail::kInvalidSocket;
            return false;
        }
        // 回读真实端口（opt.port == 0 时靠这一步拿到 OS 分配的端口）
        sockaddr_in bound{};
#ifdef _WIN32
        int blen = sizeof(bound);
#else
        socklen_t blen = sizeof(bound);
#endif
        if (::getsockname(listen_, reinterpret_cast<sockaddr*>(&bound), &blen) == 0) {
            port_ = ::ntohs(bound.sin_port);
        }
        if (::listen(listen_, 4) != 0) {
            detail::close_socket(listen_);
            listen_ = detail::kInvalidSocket;
            return false;
        }
        stop_ = false;
        thread_ = std::thread([this] { accept_loop(); });
        return true;
    }

    // 便利重载：全默认选项。
    //   **不用** `start(const Options& = Options{})` —— GCC 8/9 在
    //   "嵌套类带 NSDMI + 用作默认实参"时会报 "default member initializer
    //   required before the end of its enclosing class"。局部变量就没问题。
    bool start() {
        Options o;
        return start(o);
    }

    void stop() {
        stop_ = true;
        // 关掉 listen socket 让阻塞中的 accept 立刻返回 —— 光靠标志位
        // 是唤不醒 accept 的，这是"优雅关闭"最常见的漏点。
        detail::close_socket(listen_);
        listen_ = detail::kInvalidSocket;
        detail::close_socket(client_);
        client_ = detail::kInvalidSocket;
        if (thread_.joinable()) thread_.join();
    }

    std::uint16_t port() const { return port_; }

    // 等客户端连上来（客户端 connect 后从站才 accept，测试里需要同步点）
    bool wait_for_client(int timeout_ms = 3000) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (client_connected_) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return client_connected_;
    }

    // 运行期切换"被拒绝的功能码"（测试需要**动态**造故障：
    // 先让某一块正常读一次、再让它开始失败，才能验证"保留旧值"这件事）
    void set_reject_fc(std::uint8_t f) { dynamic_reject_fc_ = f; }

    // -----------------------------------------------------------------
    // 寄存器模型（测试直接操作；线程安全）
    // -----------------------------------------------------------------
    void set_ir(std::size_t a, std::uint16_t v) {
        std::lock_guard<std::mutex> lk(mu_);
        if (a < kRegs) ir_[a] = v;
    }
    void set_hr(std::size_t a, std::uint16_t v) {
        std::lock_guard<std::mutex> lk(mu_);
        if (a < kRegs) hr_[a] = v;
    }
    void set_di(std::size_t a, bool v) {
        std::lock_guard<std::mutex> lk(mu_);
        if (a < kBits) di_[a] = v;
    }
    void set_co(std::size_t a, bool v) {
        std::lock_guard<std::mutex> lk(mu_);
        if (a < kBits) co_[a] = v;
    }
    std::uint16_t ir(std::size_t a) const {
        std::lock_guard<std::mutex> lk(mu_);
        return (a < kRegs) ? ir_[a] : 0;
    }
    std::uint16_t hr(std::size_t a) const {
        std::lock_guard<std::mutex> lk(mu_);
        return (a < kRegs) ? hr_[a] : 0;
    }
    bool di(std::size_t a) const {
        std::lock_guard<std::mutex> lk(mu_);
        return (a < kBits) ? di_[a] : false;
    }
    bool co(std::size_t a) const {
        std::lock_guard<std::mutex> lk(mu_);
        return (a < kBits) ? co_[a] : false;
    }
    // 写浮点到输入寄存器（按指定字序）
    void set_ir_f32(std::size_t a, float v, WordOrder wo) {
        std::uint16_t two[2] = {0, 0};
        put_f32(two, v, wo);
        set_ir(a, two[0]);
        set_ir(a + 1, two[1]);
    }
    void set_hr_f32(std::size_t a, float v, WordOrder wo) {
        std::uint16_t two[2] = {0, 0};
        put_f32(two, v, wo);
        set_hr(a, two[0]);
        set_hr(a + 1, two[1]);
    }

    // -----------------------------------------------------------------
    // 统计（测试判据）
    // -----------------------------------------------------------------
    int requests()       const { return requests_; }
    int write_requests() const { return write_requests_; }
    int last_fc()        const { return last_fc_; }
    int exceptions_sent()const { return exceptions_sent_; }
    int malformed_sent() const { return malformed_sent_; }
    bool client_connected() const { return client_connected_; }

    // 重置统计（跑多段场景时用）
    void reset_counters() {
        requests_ = 0;
        write_requests_ = 0;
        last_fc_ = 0;
        exceptions_sent_ = 0;
        malformed_sent_ = 0;
    }

private:
    // -----------------------------------------------------------------
    void accept_loop() {
        while (!stop_) {
            sockaddr_in peer{};
#ifdef _WIN32
            int plen = sizeof(peer);
#else
            socklen_t plen = sizeof(peer);
#endif
            socket_t c = ::accept(listen_, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (!detail::socket_valid(c)) {
                if (stop_) break;
                continue;   // 关掉 listen 会走到这里
            }
            set_timeout(c, 200);
            client_ = c;
            client_connected_ = true;
            serve(c);
            client_connected_ = false;
            detail::close_socket(c);
            client_ = detail::kInvalidSocket;
            if (stop_) break;
        }
    }

    void serve(socket_t c) {
        while (!stop_) {
            std::uint8_t hdr[kMbapLen];
            // 区分"超时"与"对端关闭"很重要：超时必须 continue（继续等下一
            // 个请求），关闭才 return。若把两者都当结束，测试里只要客户端
            // 在两次请求之间多停了 200 ms，连接就被从站自己掐了。
            const int r = recv_exact2(c, hdr, kMbapLen);
            if (r == 0) return;        // 对端关闭
            if (r < 0)  { if (stop_) return; continue; }   // 超时：继续等

            const std::uint16_t tid  = get_u16(hdr + 0);
            const std::uint16_t mbap = get_u16(hdr + 4);
            const std::uint8_t  uid  = hdr[6];
            if (mbap < 2) return;
            const std::size_t pdu_len = static_cast<std::size_t>(mbap) - 1;
            if (pdu_len > kMaxPduLen) return;

            std::uint8_t pdu[kMaxPduLen];
            if (recv_exact2(c, pdu, pdu_len) != 1) return;

            ++requests_;
            last_fc_ = pdu[0];

            std::uint8_t resp[kMaxPduLen];
            std::size_t  rlen = handle_pdu(pdu, pdu_len, resp);

            if (opt_.silent) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
            if (opt_.delay_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(opt_.delay_ms));
            }
            if (rlen == 0) continue;   // 无响应（不应发生）

            std::uint8_t adu[kMaxAduLen];
            put_u16(adu + 0, tid);
            put_u16(adu + 2, kProtocolId);
            put_u16(adu + 4, static_cast<std::uint16_t>(rlen + 1));
            adu[6] = uid;
            std::memcpy(adu + kMbapLen, resp, rlen);

            // 串包注入：先发一个**事务号不同**的假响应，客户端必须靠
            // 事务号校验把它识别出来并丢弃，而不是当成本次的结果。
            if (opt_.extra_response_first) {
                std::uint8_t junk[kMaxAduLen];
                std::memcpy(junk, adu, kMbapLen + rlen);
                put_u16(junk + 0, static_cast<std::uint16_t>(tid ^ 0x5A5A));
                send_all(c, junk, kMbapLen + rlen);
            }

            const std::size_t total = kMbapLen + rlen;
            if (opt_.split_response_at > 0 && opt_.split_response_at < total) {
                // 半包注入：分两次 send，客户端必须按 MBAP.Length 循环读齐
                send_all(c, adu, opt_.split_response_at);
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                send_all(c, adu + opt_.split_response_at, total - opt_.split_response_at);
            } else {
                send_all(c, adu, total);
            }
        }
    }

    // -----------------------------------------------------------------
    // 唯一的协议实现处 —— 也是这个类作为"可执行协议文档"的价值所在
    // -----------------------------------------------------------------
    std::size_t handle_pdu(const std::uint8_t* pdu, std::size_t len, std::uint8_t* resp) {
        const std::uint8_t function = pdu[0];

        // ① 功能码不支持 → 异常 0x01
        //    静态开关（启动时定）与动态开关（运行中切）都要看 ——
        //    后者是 T25"部分成功"能成立的前提。
        const std::uint8_t dyn = dynamic_reject_fc_.load();
        if ((dyn != 0 && function == dyn) ||
            (opt_.reject_fc != 0 && function == opt_.reject_fc)) {
            return make_exception(function, 0x01, resp);
        }

        switch (function) {
        case fc::kReadHoldingRegs:
        case fc::kReadInputRegs: {
            if (len != 5) return make_exception(function, 0x03, resp);
            const std::uint16_t addr = get_u16(pdu + 1);
            const std::uint16_t qty  = get_u16(pdu + 3);
            if (qty == 0 || qty > kMaxReadRegs) return make_exception(function, 0x03, resp);
            if (addr_in_reject_range(addr, qty))  return make_exception(function, 0x02, resp);
            if (static_cast<std::size_t>(addr) + qty > kRegs) {
                return make_exception(function, 0x02, resp);
            }
            const std::uint16_t bc = static_cast<std::uint16_t>(qty * 2);
            resp[0] = function;
            // ByteCount 故意填错（测客户端能否发现"长度对但计数不对"）
            resp[1] = (opt_.corrupt_byte_count != 0) ? opt_.corrupt_byte_count
                                                     : static_cast<std::uint8_t>(bc);
            std::lock_guard<std::mutex> lk(mu_);
            const std::uint16_t* src = (function == fc::kReadInputRegs) ? ir_ : hr_;
            for (std::uint16_t i = 0; i < qty; ++i) put_u16(resp + 2 + i * 2, src[addr + i]);
            if (opt_.corrupt_byte_count != 0) ++malformed_sent_;
            return static_cast<std::size_t>(2) + bc;
        }
        case fc::kReadCoils:
        case fc::kReadDiscreteInputs: {
            if (len != 5) return make_exception(function, 0x03, resp);
            const std::uint16_t addr = get_u16(pdu + 1);
            const std::uint16_t qty  = get_u16(pdu + 3);
            if (qty == 0 || qty > kMaxReadBits) return make_exception(function, 0x03, resp);
            if (addr_in_reject_range(addr, qty)) return make_exception(function, 0x02, resp);
            if (static_cast<std::size_t>(addr) + qty > kBits) {
                return make_exception(function, 0x02, resp);
            }
            const std::uint16_t bc = static_cast<std::uint16_t>((qty + 7) / 8);
            resp[0] = function;
            resp[1] = static_cast<std::uint8_t>(bc);
            std::memset(resp + 2, 0, bc);
            std::lock_guard<std::mutex> lk(mu_);
            const bool* src = (function == fc::kReadDiscreteInputs) ? di_ : co_;
            for (std::uint16_t i = 0; i < qty; ++i) {
                if (src[addr + i]) resp[2 + i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
            }
            return static_cast<std::size_t>(2) + bc;
        }
        case fc::kWriteSingleReg: {
            if (len != 5) return make_exception(function, 0x03, resp);
            const std::uint16_t addr = get_u16(pdu + 1);
            const std::uint16_t val  = get_u16(pdu + 3);
            if (addr_in_reject_range(addr, 1)) return make_exception(function, 0x02, resp);
            if (addr >= kRegs) return make_exception(function, 0x02, resp);
            {
                std::lock_guard<std::mutex> lk(mu_);
                hr_[addr] = val;
            }
            ++write_requests_;
            std::memcpy(resp, pdu, 5);   // 响应 = 回显请求
            return 5;
        }
        case fc::kWriteMultipleRegs: {
            if (len < 6) return make_exception(function, 0x03, resp);
            const std::uint16_t addr = get_u16(pdu + 1);
            const std::uint16_t qty  = get_u16(pdu + 3);
            const std::uint8_t  bc   = pdu[5];
            if (qty == 0 || qty > kMaxWriteRegs || bc != qty * 2) {
                return make_exception(function, 0x03, resp);
            }
            // ★ 长度校验：6 + bc 必须恰好等于收到的 PDU 长度。
            //   不查这一条，多余的字节会被当成下一个请求的开头，流就乱了。
            if (len != static_cast<std::size_t>(6) + bc) {
                return make_exception(function, 0x03, resp);
            }
            if (addr_in_reject_range(addr, qty)) return make_exception(function, 0x02, resp);
            if (static_cast<std::size_t>(addr) + qty > kRegs) {
                return make_exception(function, 0x02, resp);
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                for (std::uint16_t i = 0; i < qty; ++i) {
                    hr_[addr + i] = get_u16(pdu + 6 + i * 2);
                }
            }
            ++write_requests_;
            resp[0] = function;
            put_u16(resp + 1, addr);
            put_u16(resp + 3, qty);      // ★ 写多个的响应回显"数量"，不是"首值"
            return 5;
        }
        case fc::kWriteSingleCoil: {
            if (len != 5) return make_exception(function, 0x03, resp);
            const std::uint16_t addr = get_u16(pdu + 1);
            const std::uint16_t val  = get_u16(pdu + 3);
            // ★ FC05 只接受 0xFF00 / 0x0000；其它值回 0x03。
            //   很多手写报文就错在这里（写成 0x0001）。
            if (val != 0xFF00 && val != 0x0000) {
                return make_exception(function, 0x03, resp);
            }
            if (addr >= kBits) return make_exception(function, 0x02, resp);
            {
                std::lock_guard<std::mutex> lk(mu_);
                co_[addr] = (val == 0xFF00);
            }
            ++write_requests_;
            std::memcpy(resp, pdu, 5);
            return 5;
        }
        default:
            return make_exception(function, 0x01, resp);
        }
    }

    std::size_t make_exception(std::uint8_t function, std::uint8_t code, std::uint8_t* resp) {
        resp[0] = static_cast<std::uint8_t>(function | 0x80);
        resp[1] = code;
        ++exceptions_sent_;
        return 2;
    }

    bool addr_in_reject_range(std::uint16_t addr, std::uint16_t qty) const {
        if (opt_.reject_qty == 0) return false;
        const std::uint32_t a0 = addr, a1 = a0 + qty;
        const std::uint32_t r0 = opt_.reject_addr, r1 = r0 + opt_.reject_qty;
        return a0 < r1 && r0 < a1;
    }

    bool send_all(socket_t s, const std::uint8_t* buf, std::size_t len) {
        std::size_t sent = 0;
        while (sent < len) {
            const int n = ::send(s, reinterpret_cast<const char*>(buf + sent),
                                 static_cast<int>(len - sent), 0);
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }
    bool recv_exact(socket_t s, std::uint8_t* buf, std::size_t len) {
        std::size_t got = 0;
        while (got < len) {
            const int n = ::recv(s, reinterpret_cast<char*>(buf + got),
                                 static_cast<int>(len - got), 0);
            if (n <= 0) return false;
            got += static_cast<std::size_t>(n);
        }
        return true;
    }

    // 1 = 读齐；0 = 对端关闭；-1 = 完全没数据（超时，可以继续等）
    int recv_exact2(socket_t s, std::uint8_t* buf, std::size_t len) {
        std::size_t got = 0;
        while (got < len) {
            const int n = ::recv(s, reinterpret_cast<char*>(buf + got),
                                 static_cast<int>(len - got), 0);
            if (n == 0) return 0;
            if (n < 0) {
                const int e = detail::last_socket_error();
                if (detail::is_timeout(e)) {
                    if (got == 0) return -1;   // 一个字节都没等到 = 空闲
                    continue;                   // 半包中，继续等剩下的
                }
                if (detail::would_block(e)) continue;
                return 0;
            }
            got += static_cast<std::size_t>(n);
        }
        return 1;
    }
    static void set_timeout(socket_t s, int ms) {
#ifdef _WIN32
        DWORD tv = static_cast<DWORD>(ms);
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        timeval tv;
        tv.tv_sec  = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    Options opt_{};
    socket_t listen_ = detail::kInvalidSocket;
    socket_t client_ = detail::kInvalidSocket;
    std::uint16_t port_ = 0;
    std::atomic<bool> stop_{true};
    std::atomic<bool> client_connected_{false};
    std::thread thread_{};

    mutable std::mutex mu_;
    std::uint16_t ir_[kRegs] = {0};
    std::uint16_t hr_[kRegs] = {0};
    bool          di_[kBits] = {false};
    bool          co_[kBits] = {false};

    std::atomic<int> requests_{0};
    std::atomic<int> write_requests_{0};
    std::atomic<int> last_fc_{0};
    std::atomic<int> exceptions_sent_{0};
    std::atomic<int> malformed_sent_{0};
    std::atomic<std::uint8_t> dynamic_reject_fc_{0};
};

} // namespace test
} // namespace modbus
} // namespace ems
