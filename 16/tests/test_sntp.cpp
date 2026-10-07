// =====================================================================
// 16/ 单元测试 —— SNTP 对时（T30~T40）
//
//   T30 48 字节报文编解码（LI/VN/Mode 与三个时间戳的**字节位置**）
//   T31 ★ 纪元换算：1900 与 1970 相差 2208988800 秒
//   T32 ★ 32.32 定点精度与往返
//   T33 ★ offset / delay 两式（符号与量级）
//   T34 ★ 2036 年秒计数回绕（定点做差必须环绕安全）
//   T35 端到端：本地 UDP 应答器（端口 12345）→ offset/delay 符号与量级
//   T36 回显校验（originate 未回显时退回用本地 T1）
//   T37 拒绝路径：长度/mode/stratum/LI/延迟上限/超时/回错包
//   T38 ★ ClockSync：回拨被钳住，墙钟绝不倒退（SOE 时标纪律）
//   T39 反复对时（含连续回拨）后仍单调不减
//   T40 ClockSync 自检：monotonic_violations 恒为 0
//
// 为什么测试里要起一个**本地** UDP 应答线程：
//   对时的核心是"两个时钟之间的四个时间戳"，若拿公网 NTP 服务器测，
//   结果不可复现、还会因网络抖动随机红绿。自己造一个"已知偏移量"的
//   假服务器，offset/delay 就变成**可精确断言**的量（见 T35）。
//
// 编译：见 16/scripts/build_test.bat
// =====================================================================

#include "net_base.h"
#include "sntp.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace ems::comm;

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT(cond)                                                      \
    do {                                                                  \
        if (cond) {                                                       \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #cond << std::endl;                     \
        }                                                                 \
    } while (0)

#define EXPECT_EQ(a, b)                                                   \
    do {                                                                  \
        long long va = (long long)(a), vb = (long long)(b);               \
        if (va == vb) {                                                   \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va << " vs " << #b << "="  \
                      << vb << std::endl;                                 \
        }                                                                 \
    } while (0)

#define EXPECT_U64_EQ(a, b)                                               \
    do {                                                                  \
        unsigned long long va_ = (unsigned long long)(a);                 \
        unsigned long long vb_ = (unsigned long long)(b);                 \
        if (va_ == vb_) {                                                 \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va_ << " vs " << #b << "=" \
                      << vb_ << std::endl;                                \
        }                                                                 \
    } while (0)

#define EXPECT_NEAR(a, b, eps)                                            \
    do {                                                                  \
        double va_ = (a), vb_ = (b);                                      \
        if (std::fabs(va_ - vb_) <= (eps)) {                              \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va_ << " vs " << #b << "=" \
                      << vb_ << " (eps " << (eps) << ")" << std::endl;    \
        }                                                                 \
    } while (0)

using ntp::NtpTimestamp;
using ntp::NtpPacket;

// =====================================================================
// 假 SNTP 服务器（进程内线程 + 本地 UDP 套接字）
//
// 它按一个**已知的**关系构造应答，于是 offset/delay 可以精确断言：
//   T2 = T1 + server_offset + out_delay        （T1 从请求的 transmit 抄回）
//   T3 = T2 + processing
//   应答的 originate = T1（回显），receive = T2，transmit = T3
// 于是真实 offset 恒等于 server_offset，真实 delay 恒等于
//   (T4-T1) - (T3-T2) = round_trip - processing
// =====================================================================
class FakeSntpServer {
public:
    explicit FakeSntpServer(std::uint16_t port) : port_(port) {}
    ~FakeSntpServer() { stop(); }

    // 服务器时钟相对客户端的偏移（正 = 服务器快）
    double  server_offset_s = 5.0;
    double  out_delay_s     = 0.04;   // 去程网络延迟
    double  processing_s    = 0.02;   // 服务器处理时间
    std::uint8_t stratum    = 2;
    std::uint8_t li         = 0;
    std::uint8_t mode       = 4;      // 4 = server
    bool    echo_originate  = true;
    int     forced_len      = 48;     // 用来造"长度异常"
    bool    reply_other_port = false; // 用来造"回错包"

    bool start(std::string& err) {
        err.clear();
        if (!net::ensure_winsock()) { err = "Winsock 初始化失败"; return false; }
        sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ == net::kInvalidSocket) { err = "创建 UDP 套接字失败"; return false; }
        int one = 1;
        ::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
        sockaddr_in a;
        std::memset(&a, 0, sizeof(a));
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port        = htons(port_);
        if (::bind(sock_, (sockaddr*)&a, (int)sizeof(a)) != 0) {
            net::close_socket(sock_);
            sock_ = net::kInvalidSocket;
            err = "绑定 UDP 端口 " + std::to_string((unsigned)port_) + " 失败（err=" +
                  std::to_string(net::last_socket_error()) + "）";
            return false;
        }
        if (reply_other_port) {
            other_ = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (other_ == net::kInvalidSocket) { err = "创建第二套接字失败"; return false; }
            ::setsockopt(other_, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
            a.sin_port = htons(0);   // 系统分配
            if (::bind(other_, (sockaddr*)&a, (int)sizeof(a)) != 0) {
                err = "绑定第二端口失败";
                return false;
            }
        }
        net::set_recv_timeout(sock_, 100);
        running_ = true;
        th_ = std::thread(&FakeSntpServer::run, this);
        return true;
    }

    void stop() {
        if (!running_) return;
        running_ = false;
        if (th_.joinable()) th_.join();
        if (sock_ != net::kInvalidSocket) { net::close_socket(sock_); sock_ = net::kInvalidSocket; }
        if (other_ != net::kInvalidSocket) { net::close_socket(other_); other_ = net::kInvalidSocket; }
    }

    int  requests() const { return requests_.load(); }
    bool saw_client() const { return requests_.load() > 0; }

private:
    void run() {
        std::uint8_t buf[512];
        for (;;) {
            if (!running_.load()) return;
            sockaddr_in from;
            std::memset(&from, 0, sizeof(from));
#ifdef _WIN32
            int flen = (int)sizeof(from);
#else
            socklen_t flen = sizeof(from);
#endif
            const int n = (int)::recvfrom(sock_, (char*)buf, (int)sizeof(buf), 0,
                                          (sockaddr*)&from, &flen);
            if (n <= 0) continue;                 // 超时 → 回去检查 running_
            ++requests_;
            if (n != 48) continue;                // 非 SNTP 报文，忽略

            const NtpTimestamp t1 = ntp::get_ts(buf + 40);

            NtpPacket req;
            std::string err;
            ntp::decode_packet(buf, 48, req, err);

            // ---- 构造应答 ----
            NtpPacket resp;
            resp.li      = li;
            resp.vn      = 4;
            resp.mode    = mode;
            resp.stratum = stratum;
            resp.poll    = 6;
            resp.precision = -20;
            // T2 = T1 + server_offset + 去程延迟
            resp.receive  = ntp::ts_from_unix_seconds(
                ntp::ts_to_unix_seconds(t1) + server_offset_s + out_delay_s);
            // T3 = T2 + 处理时间
            resp.transmit = ntp::ts_from_unix_seconds(
                ntp::ts_to_unix_seconds(resp.receive) + processing_s);
            resp.originate = echo_originate ? t1 : NtpTimestamp();
            resp.reference = resp.transmit;

            std::uint8_t out[48];
            ntp::encode_packet(resp, out);

            const int send_len = (forced_len > 0 && forced_len <= 48) ? forced_len : 48;
            net::socket_t s = reply_other_port ? other_ : sock_;
            ::sendto(s, (const char*)out, send_len, 0, (sockaddr*)&from, flen);
        }
    }

    std::uint16_t     port_;
    net::socket_t     sock_  = net::kInvalidSocket;
    net::socket_t     other_ = net::kInvalidSocket;
    std::thread       th_;
    std::atomic<bool> running_{false};
    std::atomic<int>  requests_{0};
};

// 可编程本地时钟：按顺序给出 T1、T4
struct ScriptedClock {
    std::vector<NtpTimestamp> seq;
    std::size_t               i = 0;
    NtpTimestamp operator()() {
        if (i < seq.size()) return seq[i++];
        return seq.empty() ? NtpTimestamp() : seq[seq.size() - 1];
    }
};

// =====================================================================
// T30 报文编解码
// =====================================================================
static void test_30_packet_codec() {
    std::printf("T30 48 字节报文编解码\n");

    NtpPacket p;
    p.li      = ntp::kLiNoWarning;   // 0
    p.vn      = 4;
    p.mode    = ntp::kModeClient;    // 3
    p.stratum = 0;
    p.transmit = ntp::ts_from_unix_seconds(1700000000.5);
    p.originate = p.transmit;

    std::uint8_t b[48];
    ntp::encode_packet(p, b);

    // ★ 第 1 字节 = LI(2) | VN(3) | Mode(3)。这里是 0<<6 | 4<<3 | 3 = 0x23
    EXPECT_EQ(b[0], 0x23);
    EXPECT_EQ((b[0] >> 6) & 0x03, 0);
    EXPECT_EQ((b[0] >> 3) & 0x07, 4);
    EXPECT_EQ(b[0] & 0x07, 3);
    EXPECT_EQ(b[1], 0);   // stratum 0 = 未同步（客户端请求就是 0）

    // ★ 时间戳字段偏移：reference=16, originate=24, receive=32, transmit=40
    EXPECT_U64_EQ(ntp::get_be32(b + 16), 0);       // reference 未填
    EXPECT_U64_EQ(ntp::get_ts(b + 24).seconds, p.originate.seconds);
    EXPECT_U64_EQ(ntp::get_ts(b + 40).seconds, p.transmit.seconds);
    EXPECT_U64_EQ(ntp::get_ts(b + 40).fraction, p.transmit.fraction);
    EXPECT_U64_EQ(ntp::get_ts(b + 32).seconds, 0);  // receive 未填

    // ★ 客户端的请求**不能**被自己的 decode_packet 接受：
    //   decode_packet 要求 mode ∈ {4(server), 2(symmetric passive)}，
    //   这样"回错包/回成客户端模式"就没有任何机会被当成有效应答。
    {
        NtpPacket q0;
        std::string e0;
        EXPECT(!ntp::decode_packet(b, 48, q0, e0));
        EXPECT(!e0.empty());
        EXPECT_EQ(b[0] & 0x07, 3);                 // 反向守卫：它确实是 mode 3
    }

    // ---- 服务器应答：mode=4 才收 ----
    NtpPacket srv = p;
    srv.mode = 4; srv.stratum = 2;
    srv.receive  = ntp::ts_from_unix_seconds(1700000000.25);
    srv.transmit = ntp::ts_from_unix_seconds(1700000000.27);
    std::uint8_t sb[48];
    ntp::encode_packet(srv, sb);
    EXPECT_EQ(sb[0] & 0x07, 4);
    EXPECT_EQ(sb[1], 2);

    NtpPacket q;
    std::string err;
    EXPECT(ntp::decode_packet(sb, 48, q, err));
    EXPECT(err.empty());
    EXPECT_EQ(q.vn, 4);
    EXPECT_EQ(q.mode, 4);
    EXPECT_EQ(q.li, 0);
    EXPECT_EQ(q.stratum, 2);
    EXPECT_U64_EQ(q.transmit.seconds, srv.transmit.seconds);
    EXPECT_U64_EQ(q.transmit.fraction, srv.transmit.fraction);
    EXPECT_U64_EQ(q.receive.seconds, srv.receive.seconds);
    EXPECT_NEAR(ntp::ts_to_unix_seconds(q.receive), 1700000000.25, 1e-6);
    EXPECT_NEAR(ntp::ts_to_unix_seconds(q.transmit), 1700000000.27, 1e-6);

    // ---- 拒绝路径 ----
    err.clear();
    EXPECT(!ntp::decode_packet(sb, 47, q, err));          // 长度不对
    EXPECT(!err.empty());
    err.clear();
    EXPECT(!ntp::decode_packet(sb, 49, q, err));
    EXPECT(!err.empty());

    std::uint8_t m3[48];
    std::memcpy(m3, sb, 48);
    m3[0] = (std::uint8_t)((0 << 6) | (4 << 3) | 3);      // mode 3 = 客户端，不是服务器
    err.clear();
    EXPECT(!ntp::decode_packet(m3, 48, q, err));           // ★ 必须是服务器模式
    EXPECT(!err.empty());

    std::uint8_t s0[48];
    std::memcpy(s0, sb, 48);
    s0[1] = 0;                                             // stratum 0 = kiss-o'-death
    err.clear();
    EXPECT(!ntp::decode_packet(s0, 48, q, err));
    EXPECT(!err.empty());

    // 反向守卫：合法的 mode=4 + stratum=2 必须通过（否则上面的拒绝可能只是"一直拒"）
    err.clear();
    EXPECT(ntp::decode_packet(sb, 48, q, err));
    EXPECT(err.empty());
}

// =====================================================================
// T31 ★ 纪元换算
// =====================================================================
static void test_31_epoch_conversion() {
    std::printf("T31 纪元换算（1900 vs 1970）\n");

    // 编译期常量
    EXPECT_U64_EQ(ntp::kNtpToUnixEpochSecs, 2208988800ULL);
    EXPECT_U64_EQ(ntp::kNtpToUnixEpochSecs, 0x83AA7E80ULL);

    // 1970-01-01 00:00:00 UTC 在 NTP 里是 2208988800
    NtpTimestamp t1970 = ntp::ts_from_unix_seconds(0.0);
    EXPECT_U64_EQ(t1970.seconds, 2208988800ULL);
    EXPECT_U64_EQ(t1970.fraction, 0);
    EXPECT_NEAR(ntp::ts_to_unix_seconds(t1970), 0.0, 1e-9);

    // 1900-01-01 在 Unix 域里是 -2208988800（负值不该被用，但换算必须自洽）
    NtpTimestamp t1900;
    t1900.seconds = 0; t1900.fraction = 0;
    EXPECT_NEAR(ntp::ts_to_unix_seconds(t1900), -2208988800.0, 1e-3);

    // 2000-01-01 00:00:00 UTC = Unix 946684800
    EXPECT_NEAR(ntp::ts_to_unix_seconds(ntp::ts_from_unix_seconds(946684800.0)),
                946684800.0, 1e-6);
    // 2026-09-26 00:00:00 UTC = Unix 1789603200（本模块写就当天）
    EXPECT_NEAR(ntp::ts_to_unix_seconds(ntp::ts_from_unix_seconds(1789603200.0)),
                1789603200.0, 1e-6);
    // 2036-02-07 06:28:16 UTC = Unix 2085978496 = 2^32 - 2208988800，
    // 这一刻 NTP 的 32 位秒计数**回绕到 0**。
    // ★ 这是真实存在的边界，必须显式钉住而不是"顺手算一下"：
    //   回绕之后 `ts_to_unix_seconds()` 无法分辨"era 0"还是"era 1"
    //   （它用的是固定 +2208988800 偏移）。**对时本身不受影响** ——
    //   offset/delay 只用 ts_diff_seconds()，那是环绕安全的。
    //   这条边界写进 16/docs/README.md §7。
    {
        const double wrap = 2085978496.0;   // = 2^32 - 2208988800
        EXPECT_EQ(ntp::ts_from_unix_seconds(wrap).seconds, 0u);          // ★ 回绕点
        EXPECT_EQ(ntp::ts_from_unix_seconds(wrap - 1.0).seconds, 0xFFFFFFFFu);
        EXPECT_NEAR(ntp::ts_to_unix_seconds(ntp::ts_from_unix_seconds(wrap)),
                    -2208988800.0, 1e-3);   // 回绕后按 era 0 解释 → 差 2^32 秒
        // 但**差值**在跨回绕处依然正确（这正是客户端用的那条路径）
        const NtpTimestamp a = ntp::ts_from_unix_seconds(wrap - 1.0);
        const NtpTimestamp b = ntp::ts_from_unix_seconds(wrap + 1.0);
        EXPECT_NEAR(ntp::ts_diff_seconds(b, a), 2.0, 1e-6);
        // 反向守卫：回绕确实发生了（否则上面那条差值断言只是普通的一段）
        EXPECT(ntp::ts_to_u64(b) < ntp::ts_to_u64(a));
    }

    // 若有人忘了减偏移，会差整整 2208988800 秒 —— 这条断言把它钉住
    const NtpTimestamp now = ntp::ts_from_unix_seconds(1700000000.0);
    const double back = ntp::ts_to_unix_seconds(now);
    EXPECT_NEAR(back, 1700000000.0, 1e-6);
    EXPECT(std::fabs(back - (double)now.seconds) > 1e8);   // 未减偏移的写法会落进这里
}

// =====================================================================
// T32 ★ 32.32 定点
// =====================================================================
static void test_32_fixed_point() {
    std::printf("T32 32.32 定点精度与往返\n");

    // 一个 LSB = 2^-32 秒 ≈ 0.233 ns
    EXPECT_NEAR(ntp::fraction_to_seconds(1u), 1.0 / 4294967296.0, 1e-18);
    // 0x80000000 = 0.5 秒
    EXPECT_NEAR(ntp::fraction_to_seconds(0x80000000u), 0.5, 1e-12);
    EXPECT_EQ(ntp::seconds_to_fraction(0.5), 0x80000000u);
    EXPECT_EQ(ntp::seconds_to_fraction(0.0), 0u);
    EXPECT_EQ(ntp::seconds_to_fraction(-1.0), 0u);

    // 往返：整数秒 + 各种小数
    const double cases[] = {0.0, 0.25, 0.5, 0.75, 0.1, 0.999999, 0.000001};
    for (int i = 0; i < 7; ++i) {
        const double unix_s = 1700000000.0 + cases[i];
        const NtpTimestamp t = ntp::ts_from_unix_seconds(unix_s);
        EXPECT_NEAR(ntp::ts_to_unix_seconds(t), unix_s, 1e-6);
    }

    // to_u64 / from_u64 往返
    NtpTimestamp a;
    a.seconds = 0xDEADBEEF; a.fraction = 0x12345678;
    NtpTimestamp back = ntp::ts_from_u64(ntp::ts_to_u64(a));
    EXPECT_U64_EQ(back.seconds, a.seconds);
    EXPECT_U64_EQ(back.fraction, a.fraction);

    // 定点做差
    NtpTimestamp x; x.seconds = 100; x.fraction = 0;
    NtpTimestamp y; y.seconds = 101; y.fraction = 0;
    EXPECT_NEAR(ntp::ts_diff_seconds(y, x), 1.0, 1e-9);
    EXPECT_NEAR(ntp::ts_diff_seconds(x, y), -1.0, 1e-9);
    EXPECT_NEAR(ntp::ts_diff_seconds(x, x), 0.0, 1e-12);
    NtpTimestamp h; h.seconds = 100; h.fraction = 0x80000000u;
    EXPECT_NEAR(ntp::ts_diff_seconds(h, x), 0.5, 1e-12);
}

// =====================================================================
// T33 ★ offset / delay 两式
// =====================================================================
static void test_33_offset_delay_formula() {
    std::printf("T33 offset/delay 两式\n");

    // 理想场景（也是 T35 假服务器用的模型）：
    //   T1 = 0（客户端发）
    //   服务器快 5 s；去程 0.04 s → T2 = 5.04
    //   服务器处理 0.02 s        → T3 = 5.06
    //   回程 0.06 s              → T4 = 0.10
    const NtpTimestamp t1 = ntp::ts_from_unix_seconds(1000.00);
    const NtpTimestamp t2 = ntp::ts_from_unix_seconds(1005.04);
    const NtpTimestamp t3 = ntp::ts_from_unix_seconds(1005.06);
    const NtpTimestamp t4 = ntp::ts_from_unix_seconds(1000.10);

    ntp::SyncResult r = ntp::compute_offset_delay(t1, t2, t3, t4);
    EXPECT(r.ok);
    // offset = ((T2-T1)+(T3-T4))/2 = ((5.04) + (4.96))/2 = 5.00
    EXPECT_NEAR(r.offset_s, 5.0, 1e-6);
    // delay = (T4-T1)-(T3-T2) = 0.10 - 0.02 = 0.08
    EXPECT_NEAR(r.delay_s, 0.08, 1e-6);
    EXPECT(r.offset_s > 0.0);   // 服务器快 → 正
    EXPECT(r.delay_s > 0.0);

    // 服务器**慢** 2.5 s：offset 必须变负
    const NtpTimestamp b1 = ntp::ts_from_unix_seconds(1000.00);
    const NtpTimestamp b2 = ntp::ts_from_unix_seconds(997.54);
    const NtpTimestamp b3 = ntp::ts_from_unix_seconds(997.56);
    const NtpTimestamp b4 = ntp::ts_from_unix_seconds(1000.10);
    ntp::SyncResult r2 = ntp::compute_offset_delay(b1, b2, b3, b4);
    EXPECT_NEAR(r2.offset_s, -2.5, 1e-6);
    EXPECT_NEAR(r2.delay_s, 0.08, 1e-6);
    EXPECT(r2.offset_s < 0.0);

    // 完美同步、零延迟
    ntp::SyncResult r3 = ntp::compute_offset_delay(t1, t1, t1, t1);
    EXPECT_NEAR(r3.offset_s, 0.0, 1e-12);
    EXPECT_NEAR(r3.delay_s, 0.0, 1e-12);

    // ★ 两式必须**互相独立**：只改处理时间 T3，offset 动一半、delay 反向动同样多
    const NtpTimestamp c3 = ntp::ts_from_unix_seconds(1005.16);   // 处理多了 0.10
    ntp::SyncResult r4 = ntp::compute_offset_delay(t1, t2, c3, t4);
    EXPECT_NEAR(r4.offset_s - r.offset_s, 0.05, 1e-6);
    EXPECT_NEAR(r4.delay_s - r.delay_s, -0.10, 1e-6);

    // -----------------------------------------------------------------
    // ★ 残余量公式（现场"对完了还差几十毫秒"就是这个）：
    //     offset_测 = offset_真 + out_delay + processing/2 - R/2
    //   其中 R = T4 - T1（客户端侧观测到的整个往返）。
    //   **只有** R = 2*out_delay + processing（去回程对称）时才无偏。
    //   这就是为什么 NTP 的精度上限由"路径不对称"决定，而不是由算法决定。
    // -----------------------------------------------------------------
    {
        // 非对称路径：去程 0.04、回程 0.06（R = 0.10，而 2*0.04 + 0.02 = 0.10 → 恰好对称）
        ntp::SyncResult sym = ntp::compute_offset_delay(t1, t2, t3, t4);
        EXPECT_NEAR(sym.offset_s, 5.0, 1e-6);                       // 无偏

        // 把回程拉长到 0.16（R = 0.20）：残余 = 0.04 + 0.01 - 0.10 = -0.05
        const NtpTimestamp t4b = ntp::ts_from_unix_seconds(1000.20);
        ntp::SyncResult asym = ntp::compute_offset_delay(t1, t2, t3, t4b);
        EXPECT_NEAR(asym.offset_s, 5.0 - 0.05, 1e-6);               // ★ 偏了 50 ms
        EXPECT_NEAR(asym.delay_s, 0.20 - 0.02, 1e-6);
        // 反向守卫：残余确实非零（否则上面只是重复了理想情形）
        EXPECT(std::fabs(asym.offset_s - 5.0) > 0.01);
    }
}

// =====================================================================
// T34 ★ 回绕
// =====================================================================
static void test_34_wraparound() {
    std::printf("T34 2036 年秒计数回绕\n");

    // t1 在回绕前、t4 在回绕后，真实间隔 32 秒
    NtpTimestamp t1; t1.seconds = 0xFFFFFFF0u; t1.fraction = 0;
    NtpTimestamp t4; t4.seconds = 0x00000010u; t4.fraction = 0;
    EXPECT_NEAR(ntp::ts_diff_seconds(t4, t1), 32.0, 1e-9);     // ★ 环绕安全的正差
    EXPECT_NEAR(ntp::ts_diff_seconds(t1, t4), -32.0, 1e-9);

    // 若有人把 uint64 直接当 int64 之外的做法（例如先转 double 再相减），
    // 上面会得到约 -4.29e9 而不是 +32。这里把它显式钉住：
    const double naive = (double)t4.seconds - (double)t1.seconds;   // 错的写法
    EXPECT(naive < -4.0e9);
    EXPECT(ntp::ts_diff_seconds(t4, t1) > 0.0);

    // 带小数部分的回绕
    NtpTimestamp u1; u1.seconds = 0xFFFFFFFFu; u1.fraction = 0x80000000u; // 差 0.5 s 到回绕
    NtpTimestamp u4; u4.seconds = 0x00000000u; u4.fraction = 0x80000000u; // 回绕后 0.5 s
    EXPECT_NEAR(ntp::ts_diff_seconds(u4, u1), 1.0, 1e-9);

    // 一整轮（2^32 秒）的差会回到 0 —— 这是该表示的**固有**边界，不是缺陷
    NtpTimestamp v1; v1.seconds = 0; v1.fraction = 0;
    NtpTimestamp v2; v2.seconds = 0; v2.fraction = 0;
    EXPECT_NEAR(ntp::ts_diff_seconds(v2, v1), 0.0, 1e-12);
}

// =====================================================================
// T35 端到端：本地 UDP 应答器
// =====================================================================
static void test_35_end_to_end() {
    std::printf("T35 端到端（本地 UDP 应答器 :12345）\n");

    FakeSntpServer srv(12345);
    srv.server_offset_s = 5.0;
    srv.out_delay_s     = 0.04;
    srv.processing_s    = 0.02;
    std::string err;
    if (!srv.start(err)) {
        std::cerr << "  FAIL  起假 SNTP 服务器失败：" << err << std::endl;
        ++g_fail;
        return;
    }

    // --- 客户端快？服务器快 +5 s → offset ≈ +5 ---
    {
        ScriptedClock clk;
        clk.seq.push_back(ntp::ts_from_unix_seconds(1700000000.00));   // T1
        clk.seq.push_back(ntp::ts_from_unix_seconds(1700000000.10));   // T4（往返 100 ms）
        ntp::SntpClient client(ntp::SntpClient::LocalClock(
            [&clk]() { return clk(); }));

        ntp::SntpConfig cfg;
        cfg.host = "127.0.0.1";
        cfg.port = 12345;
        cfg.timeout_ms = 2000;
        ntp::SyncResult r = client.query(cfg);

        EXPECT(r.ok);
        EXPECT(r.error.empty());
        EXPECT_EQ(r.stratum, 2);
        EXPECT(r.t1_echo_ok);                                  // 回显正确
        EXPECT_NEAR(r.offset_s, 5.0, 2e-3);                    // ★ 符号 + 量级
        EXPECT_NEAR(r.delay_s, 0.08, 2e-3);                    // ★ 0.10 - 0.02
        EXPECT(r.offset_s > 0.0);
        EXPECT_EQ(client.calls(), 1);
        EXPECT_EQ(client.failures(), 0);
    }

    // --- 服务器慢 → offset 为负 ---
    {
        FakeSntpServer slow(12346);
        slow.server_offset_s = -2.5;
        slow.out_delay_s     = 0.04;
        slow.processing_s    = 0.02;
        std::string e2;
        if (slow.start(e2)) {
            ScriptedClock clk;
            clk.seq.push_back(ntp::ts_from_unix_seconds(1700000000.00));
            clk.seq.push_back(ntp::ts_from_unix_seconds(1700000000.10));
            ntp::SntpClient client(ntp::SntpClient::LocalClock(
                [&clk]() { return clk(); }));
            ntp::SntpConfig cfg;
            cfg.host = "127.0.0.1"; cfg.port = 12346; cfg.timeout_ms = 2000;
            ntp::SyncResult r = client.query(cfg);
            EXPECT(r.ok);
            EXPECT_NEAR(r.offset_s, -2.5, 2e-3);
            EXPECT(r.offset_s < 0.0);
            EXPECT_NEAR(r.delay_s, 0.08, 2e-3);
            slow.stop();
        } else {
            std::cerr << "  FAIL  起第二台假服务器失败：" << e2 << std::endl;
            ++g_fail;
        }
    }

    // 反向守卫：假服务器确实收到了请求（否则上面的 ok 可能来自别的地方）
    EXPECT(srv.requests() > 0);
    EXPECT(srv.saw_client());
    srv.stop();
}

// =====================================================================
// T36 回显校验
// =====================================================================
static void test_36_originate_echo() {
    std::printf("T36 originate 回显校验\n");

    FakeSntpServer srv(12347);
    srv.server_offset_s = 1.25;
    srv.out_delay_s     = 0.01;
    srv.processing_s    = 0.005;
    srv.echo_originate  = false;          // ★ 不回显（不合规但现场存在）
    std::string err;
    if (!srv.start(err)) {
        std::cerr << "  FAIL  起假服务器失败：" << err << std::endl;
        ++g_fail;
        return;
    }
    ScriptedClock clk;
    // ★ R = T4 - T1 必须等于 2*out_delay + processing = 0.025，
    //   这时测得的 offset 才**无偏**（见 T33 的残余量公式）。
    clk.seq.push_back(ntp::ts_from_unix_seconds(1700000000.000));
    clk.seq.push_back(ntp::ts_from_unix_seconds(1700000000.025));
    ntp::SntpClient client(ntp::SntpClient::LocalClock([&clk]() { return clk(); }));
    ntp::SntpConfig cfg;
    cfg.host = "127.0.0.1"; cfg.port = 12347; cfg.timeout_ms = 2000;
    ntp::SyncResult r = client.query(cfg);

    EXPECT(r.ok);                     // 仍可用：退回用本地 T1
    EXPECT(!r.t1_echo_ok);            // ★ 但必须**如实标记**"服务器没回显"
    EXPECT_NEAR(r.offset_s, 1.25, 1e-3);     // R 已配成无偏 → 应当很准
    EXPECT_NEAR(r.delay_s, 0.020, 1e-3);     // delay = R - processing = 0.025 - 0.005
    srv.stop();

    // 对照组：回显正常时 t1_echo_ok 必须为 true（否则上面那条没区分度）
    FakeSntpServer srv2(12347);
    srv2.server_offset_s = 1.25;
    std::string e2;
    if (srv2.start(e2)) {
        ScriptedClock c2;
        c2.seq.push_back(ntp::ts_from_unix_seconds(1700000000.000));
        c2.seq.push_back(ntp::ts_from_unix_seconds(1700000000.025));
        ntp::SntpClient cl(ntp::SntpClient::LocalClock([&c2]() { return c2(); }));
        ntp::SyncResult rr = cl.query(cfg);
        EXPECT(rr.ok);
        EXPECT(rr.t1_echo_ok);
        srv2.stop();
    } else {
        ++g_fail;
    }
}

// =====================================================================
// T37 拒绝路径
// =====================================================================
static void test_37_rejections() {
    std::printf("T37 拒绝路径\n");

    // 统一的"跑一次查询"：注入一个 T1/T4 相差 100 ms 的脚本化本地时钟
    auto run_once = [](const ntp::SntpConfig& cfg, ntp::SyncResult& out) {
        ScriptedClock clk;
        clk.seq.push_back(ntp::ts_from_unix_seconds(1700000000.00));
        clk.seq.push_back(ntp::ts_from_unix_seconds(1700000000.10));
        ntp::SntpClient client(ntp::SntpClient::LocalClock([&clk]() { return clk(); }));
        out = client.query(cfg);
    };

    ntp::SntpConfig base;
    base.host = "127.0.0.1"; base.port = 12350; base.timeout_ms = 1000;

    // ① 长度异常
    {
        FakeSntpServer s(12350);
        s.forced_len = 30;
        std::string e;
        EXPECT(s.start(e));
        ntp::SyncResult r;
        run_once(base, r);
        EXPECT(!r.ok);
        EXPECT(!r.error.empty());
        s.stop();
    }
    // ② mode 不对（回的是客户端模式）
    {
        FakeSntpServer s(12350);
        s.mode = 3;
        std::string e;
        EXPECT(s.start(e));
        ntp::SyncResult r;
        run_once(base, r);
        EXPECT(!r.ok);
        EXPECT(!r.error.empty());
        s.stop();
    }
    // ③ stratum 0 = kiss-o'-death
    {
        FakeSntpServer s(12350);
        s.stratum = 0;
        std::string e;
        EXPECT(s.start(e));
        ntp::SyncResult r;
        run_once(base, r);
        EXPECT(!r.ok);
        EXPECT(!r.error.empty());
        s.stop();
    }
    // ④ LI = 3（服务器自己没对上）：默认拒绝，允许时放行但**必须**打标记
    {
        FakeSntpServer s(12350);
        s.li = 3;
        std::string e;
        EXPECT(s.start(e));
        ntp::SyncResult r;
        run_once(base, r);
        EXPECT(!r.ok);                       // ★ 默认拒绝
        EXPECT(!r.error.empty());
        s.stop();

        FakeSntpServer s2(12350);
        s2.li = 3; s2.server_offset_s = 0.5;
        std::string e2;
        EXPECT(s2.start(e2));
        ntp::SntpConfig cfg2 = base;
        cfg2.reject_unsynced = false;        // 显式放行
        ntp::SyncResult r2;
        run_once(cfg2, r2);
        EXPECT(r2.ok);
        EXPECT(r2.leap_warning);             // ★ 但必须如实标记
        EXPECT_EQ(r2.leap, 3);
        s2.stop();
    }
    // ⑤ 往返延迟超过上限
    //    ★ 注意：delay 的量纲是**往返**(T4-T1)-(T3-T2)，单程延迟大小会被
    //      折进 offset 而不是 delay。所以这里必须让客户端的 T1→T4 真的拉长。
    {
        FakeSntpServer s(12350);
        s.server_offset_s = 0.0;
        s.out_delay_s     = 1.0;
        s.processing_s    = 0.0;
        std::string e;
        EXPECT(s.start(e));
        ntp::SntpConfig cfg = base;
        cfg.max_delay_s = 1.0;               // 上限 1 s
        ScriptedClock clk;
        clk.seq.push_back(ntp::ts_from_unix_seconds(1700000000.00));   // T1
        clk.seq.push_back(ntp::ts_from_unix_seconds(1700000005.00));   // T4：往返 5 s
        ntp::SntpClient client(ntp::SntpClient::LocalClock([&clk]() { return clk(); }));
        ntp::SyncResult r = client.query(cfg);
        EXPECT(!r.ok);
        EXPECT(!r.error.empty());
        EXPECT(r.delay_s > 4.0);             // 往返确实是 5 s，不是 0.1 s
        s.stop();
    }
    // ⑥ 超时（没有人监听）
    {
        ntp::SntpConfig cfg = base;
        cfg.port = 12399;
        cfg.timeout_ms = 400;
        ScriptedClock clk;
        clk.seq.push_back(ntp::ts_from_unix_seconds(1700000000.00));
        ntp::SntpClient client(ntp::SntpClient::LocalClock([&clk]() { return clk(); }));
        ntp::SyncResult r = client.query(cfg);
        EXPECT(!r.ok);
        EXPECT(!r.error.empty());
        EXPECT_EQ(client.failures(), 1);
    }
    // ⑦ 主机解析失败
    {
        ntp::SntpConfig cfg = base;
        cfg.host = "no-such-host.invalid";
        ScriptedClock clk;
        ntp::SntpClient client(ntp::SntpClient::LocalClock([&clk]() { return clk(); }));
        ntp::SyncResult r = client.query(cfg);
        EXPECT(!r.ok);
        EXPECT(!r.error.empty());
    }
    // ⑧ ★ 回错包：应答来自别的端口
    {
        FakeSntpServer s(12350);
        s.reply_other_port = true;
        std::string e;
        EXPECT(s.start(e));
        ntp::SyncResult r;
        run_once(base, r);
        EXPECT(!r.ok);                        // ★ 源端口校验必须拦下
        EXPECT(!r.error.empty());
        s.stop();
    }
}

// =====================================================================
// T38 ★ ClockSync：回拨钳制
// =====================================================================
struct FakeMono {
    double t = 1000.0;
};

static void test_38_clock_sync_backstep() {
    std::printf("T38 ClockSync 回拨被钳住\n");

    FakeMono mono;
    ntp::ClockSync cs(1700000000.0,
                      ntp::ClockSync::MonoNowFn([&mono]() { return mono.t; }));

    EXPECT(!cs.synced());
    EXPECT_NEAR(cs.wall_now(), 1700000000.0, 1e-6);
    EXPECT_EQ(cs.applied(), 0);

    // 单调时钟前进 1 s → 墙钟跟着走
    mono.t += 1.0;
    EXPECT_NEAR(cs.wall_now(), 1700000001.0, 1e-6);

    // ---- 正向偏移：允许（墙钟前跳）----
    {
        const double before = cs.wall_now();
        const bool as_is = cs.apply(+0.5, 0.01);
        EXPECT(as_is);                                  // 原样应用
        EXPECT_NEAR(cs.offset(), 0.5, 1e-9);
        EXPECT_NEAR(cs.last_step_s(), 0.5, 1e-6);
        EXPECT_NEAR(cs.wall_now(), before + 0.5, 1e-6);
        EXPECT_EQ(cs.backward_clamps(), 0);
        EXPECT_EQ(cs.applied(), 1);
        EXPECT(cs.synced());
    }

    // ---- ★ 负向偏移：会回拨 → 必须被钳住 ----
    {
        const double before = cs.wall_now();
        const bool as_is = cs.apply(-3.0, 0.02);
        EXPECT(!as_is);                                 // 返回 false = 被钳
        EXPECT_EQ(cs.backward_clamps(), 1);
        EXPECT(cs.last_clamp_ms() > 0);                 // 记录了被钳掉的毫秒
        EXPECT_NEAR(cs.last_clamp_ms(), 3500, 2);       // 想退 3.5 s（当前 offset 是 +0.5）
        EXPECT(cs.last_step_s() < 0.0);                 // 本应倒退
        // ★ 墙钟**不倒退**
        EXPECT_NEAR(cs.wall_now(), before, 1e-6);
        EXPECT(cs.wall_now() >= before - 1e-9);
        EXPECT_EQ(cs.monotonic_violations(), 0);
    }

    // ---- 再来一次更小的回拨，依然钳住 ----
    {
        const double before = cs.wall_now();
        const bool as_is = cs.apply(-100.0, 0.02);
        EXPECT(!as_is);
        EXPECT(cs.wall_now() >= before - 1e-9);
        EXPECT(cs.last_clamp_ms() >= 100000);           // 100 s
        EXPECT_EQ(cs.backward_clamps(), 2);
    }

    // ---- 钳住之后再给一个正偏移，应当能正常前跳 ----
    {
        const double before = cs.wall_now();
        const bool as_is = cs.apply(cs.offset() + 0.25, 0.02);
        EXPECT(as_is);
        EXPECT(cs.wall_now() > before);
    }

    // ---- 自检计数：任何时刻都不允许墙钟倒退 ----
    EXPECT_EQ(cs.monotonic_violations(), 0);
    EXPECT_EQ(cs.applied(), 4);
    EXPECT_EQ(cs.backward_clamps(), 2);
}

// =====================================================================
// T39 ★ 反复对时（含连续回拨）后仍单调不减
// =====================================================================
static void test_39_repeated_sync_monotonic() {
    std::printf("T39 反复对时后墙钟仍单调不减\n");

    FakeMono mono;
    ntp::ClockSync cs(1700000000.0,
                      ntp::ClockSync::MonoNowFn([&mono]() { return mono.t; }));

    // 固定的"服务器偏移序列"（含大幅回拨、小幅修正、正向跳变）
    const double offsets[] = {+0.1, -1.0, +0.05, -5.0, +0.02, -0.3, +10.0, -20.0, 0.0, -0.001};
    double last_wall = cs.wall_now();
    int clamped = 0;
    for (int i = 0; i < 10; ++i) {
        // 每两次对时之间推进 0.2 s
        mono.t += 0.2;
        const bool as_is = cs.apply(offsets[i], 0.003);
        if (!as_is) ++clamped;
        const double w = cs.wall_now();
        EXPECT(w >= last_wall - 1e-9);          // ★ 单调不减（每一步都查）
        last_wall = w;
    }
    EXPECT(clamped > 0);                        // 反向守卫：确实有过回拨尝试
    EXPECT_EQ(cs.monotonic_violations(), 0);    // 自检计数器恒为 0
    EXPECT_EQ(cs.applied(), 10);
    EXPECT_EQ(cs.backward_clamps(), clamped);

    // 墙钟与 (本地单调流逝 + 当前偏移) 必须自洽
    EXPECT_NEAR(cs.wall_now(), cs.local_now() + cs.offset(), 1e-6);
    // 10 次对时里有多次是"想回拨"，但墙钟一次都没退
    EXPECT(cs.wall_now() >= 1700000000.0);

    // reset_to 之后自检仍为 0
    cs.reset_to(1600000000.0);
    EXPECT_NEAR(cs.wall_now(), 1600000000.0, 1e-6);
    EXPECT_EQ(cs.monotonic_violations(), 0);
}

// =====================================================================
// T40 ClockSync 与真实 SNTP 结果串联
// =====================================================================
static void test_40_clock_sync_with_real_query() {
    std::printf("T40 ClockSync 消费真实查询结果\n");

    FakeSntpServer srv(12360);
    srv.server_offset_s = 7.5;      // 服务器快 7.5 s
    srv.out_delay_s     = 0.01;
    srv.processing_s    = 0.005;
    std::string err;
    if (!srv.start(err)) {
        std::cerr << "  FAIL  起假服务器失败：" << err << std::endl;
        ++g_fail;
        return;
    }

    FakeMono mono;
    ntp::ClockSync cs(1700000000.0,
                      ntp::ClockSync::MonoNowFn([&mono]() { return mono.t; }));

    ScriptedClock clk;
    clk.seq.push_back(ntp::ts_from_unix_seconds(1700000000.000));
    clk.seq.push_back(ntp::ts_from_unix_seconds(1700000000.025));
    ntp::SntpClient client(ntp::SntpClient::LocalClock([&clk]() { return clk(); }));
    ntp::SntpConfig cfg;
    cfg.host = "127.0.0.1"; cfg.port = 12360; cfg.timeout_ms = 2000;

    const ntp::SyncResult r = client.query(cfg);
    EXPECT(r.ok);
    EXPECT_NEAR(r.offset_s, 7.5, 1e-3);

    const double before = cs.wall_now();
    const bool as_is = cs.apply(r);                 // 直接消费 SyncResult
    EXPECT(as_is);
    EXPECT_NEAR(cs.wall_now() - before, r.offset_s, 1e-6);
    EXPECT_NEAR(cs.last_delay(), r.delay_s, 1e-12);
    EXPECT(cs.synced());

    // 反向守卫：如果 offset 真的是 0（服务器没偏），上面那条位移断言就没有区分度
    EXPECT(std::fabs(r.offset_s) > 1.0);

    srv.stop();
}

// =====================================================================
int main() {
    std::printf("=== 16/ SNTP 对时 单元测试 ===\n\n");

    test_30_packet_codec();
    test_31_epoch_conversion();
    test_32_fixed_point();
    test_33_offset_delay_formula();
    test_34_wraparound();
    test_35_end_to_end();
    test_36_originate_echo();
    test_37_rejections();
    test_38_clock_sync_backstep();
    test_39_repeated_sync_monotonic();
    test_40_clock_sync_with_real_query();

    std::printf("\n");
    if (g_fail == 0) std::printf("ALL TESTS PASSED\n");
    // 本用例没有"跳过"路径，恒为 0；仍按约定打出来，
    // 让每层的状态行格式一致（见 新模块开发约定.md §4.5）
    std::printf("PASS=%d FAIL=%d SKIPPED=0\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
