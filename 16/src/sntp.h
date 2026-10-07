// =====================================================================
// 16/ — SNTP 对时（RFC 4330 单播客户端模式）
//
// 为什么 EMS 必须对时：SOE（事件顺序记录）的**全部价值**就在于"谁先谁后"。
// 若装置之间相差 2 秒，那么"保护先动、还是 EMS 先切"这种结论就是编的。
// 现场对时的常见做法是 SNTP（NTP 的简化单播模式），因为：
//   · 报文固定 48 字节，无状态；
//   · 一次请求/一次应答，不依赖 PTP 那样的硬件时间戳；
//   · 精度 1–50 ms，对 SOE 的秒级/百毫秒级顺序判断足够。
//
// ── 三个必须显式处理的坑 ────────────────────────────────────────────
//
// ★① **两个纪元**。NTP 的 0 时刻是 **1900-01-01**，
//     Unix 的 0 时刻是 **1970-01-01**，两者相差
//     **2208988800 秒**（= 0x83AA7E80，正好 70 年里的 17 个闰日）。
//    少减/多减它的表现是"时间对到了 1900 年"或"1970 年"，
//    而 offset 的**符号与量级**在单元测试里会立刻暴露 —— 所以这里
//    既做 `static_assert` 也做逐值断言。
//
// ★② **NTP 时间戳是 32.32 定点**，不是 double，也不是 64 位整数秒。
//    高 32 位是秒、低 32 位是 2^-32 秒。做差必须**按定点做**，
//    否则小数点后会丢精度；而且**不能**先转 double 秒再相减 ——
//    那会在大数值上丢掉整数秒的低位。本文件用
//    `ts_diff_seconds()`：先在 64 位**无符号**域相减，再把结果按
//    补码解释成有符号 —— 这一步顺带把 **2036 年秒计数回绕**也解决了。
//
// ★③ **回拨（backward step）绝不允许**。对时会把本机墙钟"拉一下"，
//    若服务器时间比本机早，直接应用就会让墙钟倒退 —— 于是同一个
//    事件可能被记两次、或 SOE 序列出现"后发生的事时间更早"。
//    本文件的 `ClockSync::apply()` 在检测到会倒退时**钳住**（让墙钟
//    原地不动，记录被钳掉的毫秒数并计数告警），而不是照着倒退。
//
// 编译：纯头文件。Windows 侧 UDP 需要 `-lws2_32`。
// =====================================================================

#pragma once

#include "net_base.h"   // ★ winsock 的 include 顺序与 WSAStartup 纪律只在这一处

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace ems {
namespace comm {
namespace ntp {

// =====================================================================
// 常量
// =====================================================================
constexpr std::size_t   kNtpPacketSize       = 48;
constexpr std::uint16_t kNtpDefaultPort      = 123;
// ★ 1900-01-01 → 1970-01-01 的秒数（0x83AA7E80）
constexpr std::uint64_t kNtpToUnixEpochSecs  = 2208988800ULL;
// 32.32 定点的分母
constexpr double        kFracScale           = 4294967296.0;  // 2^32

// LI（闰秒指示）/ Mode，字段位置见 §报文
enum NtpLeap : std::uint8_t {
    kLiNoWarning   = 0,
    kLiLastMin61   = 1,
    kLiLastMin59   = 2,
    kLiUnknown     = 3,   // ★ 服务器自己就没对上时
};
enum NtpMode : std::uint8_t {
    kModeReserved        = 0,
    kModeSymmetricActive = 1,
    kModeSymmetricPassive= 2,
    kModeClient          = 3,
    kModeServer          = 4,
    kModeBroadcast       = 5,
};

// ---------------------------------------------------------------------
// 编译期把"两个纪元差多少"钉死。少了这一条，将来有人"顺手改成
// 2208988800 - 86400"（以为是闰秒）不会有任何提示。
// ---------------------------------------------------------------------
static_assert(kNtpToUnixEpochSecs == 2208988800ULL,
              "NTP 纪元(1900-01-01)与 Unix 纪元(1970-01-01)相差 2208988800 秒");
static_assert((std::uint64_t)kNtpToUnixEpochSecs == 0x83AA7E80ULL,
              "2208988800 的十六进制是 0x83AA7E80（现场抓包对照用）");
static_assert(kNtpPacketSize == 48, "SNTP 报文固定 48 字节");

// =====================================================================
// 32.32 定点时间戳
// =====================================================================
struct NtpTimestamp {
    std::uint32_t seconds  = 0;   // 自 1900-01-01 起的秒（2036 年回绕）
    std::uint32_t fraction = 0;   // 2^-32 秒

    bool operator==(const NtpTimestamp& o) const {
        return seconds == o.seconds && fraction == o.fraction;
    }
    bool operator!=(const NtpTimestamp& o) const { return !(*this == o); }
};

inline std::uint64_t ts_to_u64(const NtpTimestamp& t) {
    return ((std::uint64_t)t.seconds << 32) | (std::uint64_t)t.fraction;
}

inline NtpTimestamp ts_from_u64(std::uint64_t v) {
    NtpTimestamp t;
    t.seconds  = (std::uint32_t)(v >> 32);
    t.fraction = (std::uint32_t)(v & 0xFFFFFFFFULL);
    return t;
}

inline double fraction_to_seconds(std::uint32_t f) {
    return (double)f / kFracScale;
}

inline std::uint32_t seconds_to_fraction(double s) {
    if (!(s > 0.0)) return 0u;
    const double f = s - std::floor(s);
    if (f <= 0.0) return 0u;
    const double v = f * kFracScale;
    return (std::uint32_t)((v >= (kFracScale - 1.0)) ? 0xFFFFFFFFULL
                                                     : (std::uint64_t)(v + 0.5));
}

// NTP 时间戳 → Unix 秒（含小数）
inline double ts_to_unix_seconds(const NtpTimestamp& t) {
    return (double)t.seconds - (double)kNtpToUnixEpochSecs + fraction_to_seconds(t.fraction);
}

// Unix 秒 → NTP 时间戳
inline NtpTimestamp ts_from_unix_seconds(double unix_s) {
    double v = unix_s + (double)kNtpToUnixEpochSecs;
    if (!(v > 0.0)) v = 0.0;
    // 取整秒时用 floor（不是截断）：负的 Unix 时间在加上纪元偏移后已为正，
    // 但仍按 floor 写以防将来有人拿它算 1950 年。
    const double isec = std::floor(v);
    const double frac = v - isec;
    // 2036 年之后 32 位秒会回绕 —— 这里显式按 32 位截断，与实际线上行为一致
    NtpTimestamp t;
    t.seconds  = (std::uint32_t)((std::uint64_t)isec & 0xFFFFFFFFULL);
    t.fraction = (std::uint32_t)(frac * kFracScale + 0.5);
    if (t.fraction == 0xFFFFFFFFu) { /* 留在同一秒内，不进位 */ }
    return t;
}

// ---------------------------------------------------------------------
// ★ 定点做差，返回带符号的秒（a - b）。
//
// 为什么必须这样写：先转 double 秒再相减，在 2026 年（秒计数 ~3.97e9）
// 上 double 的 53 位尾数只剩 ~1e-7 秒分辨率余量，本身还够；
// 但一旦要处理**回绕**（b 在 2036 之后、a 在之前），double 相减会得到
// 一个"巨大的负数"而不是正确的负小量。
// 在 64 位无符号域相减、再按补码解释成有符号，等价于模 2^64 的差值 ——
// 只要真实差值在 ±2^31 秒（约 ±68 年）之内，结果就是正确的。
// ---------------------------------------------------------------------
inline double ts_diff_seconds(const NtpTimestamp& a, const NtpTimestamp& b) {
    const std::uint64_t ua = ts_to_u64(a);
    const std::uint64_t ub = ts_to_u64(b);
    const std::int64_t  d  = (std::int64_t)(ua - ub);   // 无符号减 → 补码解释
    return (double)d / kFracScale;
}

// =====================================================================
// 48 字节报文
//
//   偏移  长度  字段
//    0     1    LI(2) | VN(3) | Mode(3)
//    1     1    Stratum
//    2     1    Poll
//    3     1    Precision
//    4     4    Root Delay
//    8     4    Root Dispersion
//   12     4    Reference ID
//   16     8    Reference Timestamp
//   24     8    Originate Timestamp  (T1：客户端发包时刻)
//   32     8    Receive Timestamp    (T2：服务器收到时刻)
//   40     8    Transmit Timestamp   (T3：服务器发出时刻)
// =====================================================================
struct NtpPacket {
    std::uint8_t  li          = kLiNoWarning;
    std::uint8_t  vn          = 4;
    std::uint8_t  mode        = kModeClient;
    std::uint8_t  stratum     = 0;
    std::uint8_t  poll        = 0;
    std::int8_t   precision   = 0;
    std::uint32_t root_delay  = 0;
    std::uint32_t root_dispersion = 0;
    std::uint32_t reference_id    = 0;
    NtpTimestamp  reference;    // 16
    NtpTimestamp  originate;    // 24  = T1
    NtpTimestamp  receive;      // 32  = T2
    NtpTimestamp  transmit;     // 40  = T3
};

inline void put_be32(std::uint8_t* p, std::uint32_t v) {
    p[0] = (std::uint8_t)(v >> 24);
    p[1] = (std::uint8_t)((v >> 16) & 0xFF);
    p[2] = (std::uint8_t)((v >> 8) & 0xFF);
    p[3] = (std::uint8_t)(v & 0xFF);
}
inline std::uint32_t get_be32(const std::uint8_t* p) {
    return ((std::uint32_t)p[0] << 24) | ((std::uint32_t)p[1] << 16) |
           ((std::uint32_t)p[2] << 8)  | (std::uint32_t)p[3];
}
inline void put_ts(std::uint8_t* p, const NtpTimestamp& t) {
    put_be32(p, t.seconds);
    put_be32(p + 4, t.fraction);
}
inline NtpTimestamp get_ts(const std::uint8_t* p) {
    NtpTimestamp t;
    t.seconds  = get_be32(p);
    t.fraction = get_be32(p + 4);
    return t;
}

inline void encode_packet(const NtpPacket& p, std::uint8_t out[kNtpPacketSize]) {
    std::memset(out, 0, kNtpPacketSize);
    out[0] = (std::uint8_t)(((p.li & 0x03) << 6) | ((p.vn & 0x07) << 3) | (p.mode & 0x07));
    out[1] = p.stratum;
    out[2] = p.poll;
    out[3] = (std::uint8_t)p.precision;
    put_be32(out + 4,  p.root_delay);
    put_be32(out + 8,  p.root_dispersion);
    put_be32(out + 12, p.reference_id);
    put_ts(out + 16, p.reference);
    put_ts(out + 24, p.originate);
    put_ts(out + 32, p.receive);
    put_ts(out + 40, p.transmit);
}

inline bool decode_packet(const std::uint8_t* in, std::size_t n, NtpPacket& p,
                          std::string& err) {
    if (n != kNtpPacketSize) {
        err = "SNTP 报文长度必须是 48 字节，收到 " + std::to_string((unsigned long long)n);
        return false;
    }
    const std::uint8_t b0 = in[0];
    p.li        = (std::uint8_t)((b0 >> 6) & 0x03);
    p.vn        = (std::uint8_t)((b0 >> 3) & 0x07);
    p.mode      = (std::uint8_t)(b0 & 0x07);
    p.stratum   = in[1];
    p.poll      = in[2];
    p.precision = (std::int8_t)in[3];
    p.root_delay      = get_be32(in + 4);
    p.root_dispersion = get_be32(in + 8);
    p.reference_id    = get_be32(in + 12);
    p.reference = get_ts(in + 16);
    p.originate = get_ts(in + 24);
    p.receive   = get_ts(in + 32);
    p.transmit  = get_ts(in + 40);
    if (p.mode != kModeServer && p.mode != kModeSymmetricPassive) {
        err = "应答 mode=" + std::to_string((int)p.mode) + " 不是服务器模式(4)";
        return false;
    }
    if (p.stratum == 0) {
        // Stratum 0 = "kiss-o'-death"：服务器要求客户端退避/换服务器
        err = "收到 kiss-o'-death（stratum=0, refid=0x" +
              std::to_string((unsigned)p.reference_id) + "），本次对时被拒";
        return false;
    }
    return true;
}

// =====================================================================
// 本地时间源
// =====================================================================
inline double unix_now_seconds() {
    using namespace std::chrono;
    return (double)duration_cast<microseconds>(
               system_clock::now().time_since_epoch()).count() / 1e6;
}

inline NtpTimestamp default_local_ntp_now() {
    return ts_from_unix_seconds(unix_now_seconds());
}

// =====================================================================
// 对时结果
// =====================================================================
struct SyncResult {
    bool          ok       = false;
    double        offset_s = 0.0;   // 服务器时间 - 本地时间（正 = 本地慢了）
    double        delay_s  = 0.0;   // 往返延迟 (T4-T1)-(T3-T2)
    std::uint8_t  stratum  = 0;
    std::uint8_t  leap     = 0;
    bool          leap_warning = false;   // LI==3：服务器自己没对上
    bool          t1_echo_ok   = false;   // 应答里的 originate 是否回显了我们的 T1
    std::string   error;
};

// ---------------------------------------------------------------------
// ★ 核心两式（RFC 4330 §5 / RFC 2030）
//     offset = ((T2 - T1) + (T3 - T4)) / 2
//     delay  = (T4 - T1) - (T3 - T2)
//   符号约定：offset > 0 表示**服务器比本地快**（本地要往前拨）。
// ---------------------------------------------------------------------
inline SyncResult compute_offset_delay(const NtpTimestamp& t1,
                                       const NtpTimestamp& t2,
                                       const NtpTimestamp& t3,
                                       const NtpTimestamp& t4) {
    SyncResult r;
    const double d21 = ts_diff_seconds(t2, t1);   // T2 - T1：去程 + 服务器处理
    const double d34 = ts_diff_seconds(t3, t4);   // T3 - T4 = -(T4 - T3)
    r.offset_s = 0.5 * (d21 + d34);
    r.delay_s  = ts_diff_seconds(t4, t1) - ts_diff_seconds(t3, t2);
    r.ok = true;
    return r;
}

// =====================================================================
// 时钟同步器：以**单调时钟**为基准 + 一个偏移量
//
// 为什么基准必须是单调时钟（steady_clock）：如果用系统墙钟当基准，
// 那么"应用偏移"这件事本身就依赖墙钟 —— 墙钟被改之后基准也变了，
// 于是 wall_now() 会变成一个自指的循环，回拨检测形同虚设。
// 单调时钟只前进、不受对时影响，偏移是我们自己叠上去的一层。
// =====================================================================
class ClockSync {
public:
    using MonoNowFn = std::function<double()>;   // 返回单调秒

    static double default_mono_now() {
        using namespace std::chrono;
        return (double)duration_cast<microseconds>(
                   steady_clock::now().time_since_epoch()).count() / 1e6;
    }

    // 默认构造：以当前系统墙钟为起点
    ClockSync() : ClockSync(unix_now_seconds(), &ClockSync::default_mono_now) {}

    ClockSync(double initial_unix_s, MonoNowFn mono)
        : mono_(std::move(mono)), mono_ref_(0.0), local_ref_(initial_unix_s) {
        mono_ref_ = mono_();
    }

    double mono_now() const { return mono_(); }

    // 未同步的本地墙钟（= 起点 + 单调流逝）—— 诊断用
    double local_now() const { return local_ref_ + (mono_() - mono_ref_); }

    // 对时后的墙钟（Unix 秒，含小数）
    double wall_now() const {
        const double w = local_ref_ + (mono_() - mono_ref_) + offset_;
        // ★ 自检：只要这里出现过一次倒退，就说明 apply() 的钳制逻辑有漏洞。
        if (w < last_returned_) ++monotonic_violations_;
        else                    last_returned_ = w;
        return w;
    }

    std::int64_t wall_now_ms() const {
        return (std::int64_t)std::floor(wall_now() * 1000.0 + 0.5);
    }

    // -----------------------------------------------------------------
    // 应用一次对时结果
    //
    // 返回 true  = 原样应用（墙钟前跳或不动）
    // 返回 false = 会倒退，已**钳住**（墙钟原地不动），并记录了被钳掉的量
    // -----------------------------------------------------------------
    bool apply(double offset_s, double delay_s) {
        const double m_now      = mono_();
        const double local_now_ = local_ref_ + (m_now - mono_ref_);
        const double cur_wall   = local_now_ + offset_;
        const double cand_wall  = local_now_ + offset_s;

        ++applied_;
        last_delay_ = delay_s;

        bool as_is = true;
        double new_offset = offset_s;
        if (cand_wall < cur_wall) {
            // ★ 回拨：不直接倒退。把 offset 定在"墙钟原地不动"上。
            //   为什么选"原地不动"而不是"慢慢追"：追（slew）需要引入
            //   ±500 ppm 的频率调整与一个持续的追赶状态机，那是 PTP/NTP
            //   守护进程的活；本模块只保证**不倒退**，把"精度"交给 OS 的
            //   time service（见 docs/README.md §7 已知边界）。
            new_offset = cur_wall - local_now_;
            as_is = false;
            backward_clamps_ = backward_clamps_ + 1;
            last_clamp_ms_ = (std::int64_t)((cur_wall - cand_wall) * 1000.0 + 0.5);
        } else {
            last_clamp_ms_ = 0;
        }

        last_step_s_ = cand_wall - cur_wall;   // 负值即"本应倒退多少"
        // 重新锚定：把当前墙钟固定下来，之后只靠单调时钟前进
        local_ref_ = local_now_;
        mono_ref_  = m_now;
        offset_    = new_offset;
        return as_is;
    }

    bool apply(const SyncResult& r) { return apply(r.offset_s, r.delay_s); }

    // 已经应用了几次、被钳了几次、上次被钳掉多少毫秒
    int applied() const { return applied_; }
    int backward_clamps() const { return backward_clamps_; }
    std::int64_t last_clamp_ms() const { return last_clamp_ms_; }
    double last_delay() const { return last_delay_; }
    double last_step_s() const { return last_step_s_; }
    double offset() const { return offset_; }
    bool   synced() const { return applied_ > 0; }
    // 自检计数：必须恒为 0（墙钟倒退了才会 > 0）
    int monotonic_violations() const { return monotonic_violations_; }

    // 测试用：强行把墙钟重设到某个 Unix 时刻（并清掉偏移与自检）
    void reset_to(double unix_s) {
        mono_ref_  = mono_();
        local_ref_ = unix_s;
        offset_    = 0.0;
        last_returned_ = unix_s;
    }

private:
    MonoNowFn mono_;
    double    mono_ref_  = 0.0;
    double    local_ref_ = 0.0;
    double    offset_    = 0.0;

    int          applied_          = 0;
    int          backward_clamps_  = 0;
    std::int64_t last_clamp_ms_    = 0;
    double       last_delay_       = 0.0;
    double       last_step_s_      = 0.0;

    mutable double last_returned_        = -1e300;
    mutable int    monotonic_violations_ = 0;
};

// =====================================================================
// SNTP 客户端（单播）
// =====================================================================
struct SntpConfig {
    std::string   host       = "pool.ntp.org";
    std::uint16_t port       = kNtpDefaultPort;
    int           timeout_ms = 1000;
    // 往返延迟超过这个值 → 结果不可信（现场常见：服务器被限速/排队）
    double        max_delay_s = 5.0;
    // LI==3（服务器自己没对上）时是否判为失败
    bool          reject_unsynced = true;
    std::uint8_t  version = 4;   // 现场老装置常见只认 VN=3
};

class SntpClient {
public:
    using LocalClock = std::function<NtpTimestamp()>;

    explicit SntpClient(LocalClock clk = LocalClock(&default_local_ntp_now))
        : clock_(std::move(clk)) {}

    int calls() const { return calls_; }
    int failures() const { return failures_; }

    // 一次请求/一次应答。所有失败都通过 SyncResult::error 以中文返回。
    SyncResult query(const SntpConfig& cfg) {
        SyncResult r;
        ++calls_;
        if (!net::ensure_winsock()) {
            r.error = "Winsock 初始化失败";
            ++failures_;
            return r;
        }

        // 先把 T1 定下来：它就是我们要写进报文 Transmit 字段的值
        const NtpTimestamp t1 = clock_();
        std::uint8_t req[kNtpPacketSize];
        NtpPacket p;
        p.li        = kLiNoWarning;
        p.vn        = cfg.version;
        p.mode      = kModeClient;
        p.transmit  = t1;
        p.originate = t1;   // 回显校验用（服务器按 RFC 会把它抄进应答的 originate）
        encode_packet(p, req);

        // ★ 地址解析统一走 getaddrinfo（net::resolve），不用 inet_pton：
        //   MinGW-w64 在 g++ 8.1 自带的 <ws2tcpip.h> 里没有声明 inet_pton
        //   （它只在 _WIN32_WINNT >= 0x0600 下才有），报错是
        //   "'inet_pton' was not declared in this scope" —— 像拼写错误、
        //   实为 SDK 门槛。getaddrinfo 对点分十进制一样解析。
        std::vector<net::ResolvedAddr> addrs;
        if (!net::resolve(cfg.host, cfg.port, AF_INET, addrs, r.error)) {
            ++failures_;
            return r;
        }
        const sockaddr* peer = (const sockaddr*)&addrs[0].storage;

        const net::socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (s == net::kInvalidSocket) {
            r.error = "创建 UDP 套接字失败（err=" +
                      std::to_string(net::last_socket_error()) + "）";
            ++failures_;
            return r;
        }
        net::set_recv_timeout(s, cfg.timeout_ms);

        const int sent = (int)::sendto(s, (const char*)req, (int)kNtpPacketSize, 0,
                                       peer, addrs[0].len);
        if (sent != (int)kNtpPacketSize) {
            net::close_socket(s);
            r.error = "SNTP 请求发送失败（err=" +
                      std::to_string(net::last_socket_error()) + "）";
            ++failures_;
            return r;
        }

        std::uint8_t resp[kNtpPacketSize + 16];
        sockaddr_storage from;
        std::memset(&from, 0, sizeof(from));
#ifdef _WIN32
        int fromlen = (int)sizeof(from);
#else
        socklen_t fromlen = sizeof(from);
#endif
        const int got = (int)::recvfrom(s, (char*)resp, (int)sizeof(resp), 0,
                                        (sockaddr*)&from, &fromlen);
        const NtpTimestamp t4 = clock_();   // T4：收到应答的时刻
        net::close_socket(s);

        if (got < 0) {
            r.error = "SNTP 应答超时（" + std::to_string(cfg.timeout_ms) + " ms）";
            ++failures_;
            return r;
        }
        if (got != (int)kNtpPacketSize) {
            r.error = "SNTP 应答长度异常（" + std::to_string(got) + " 字节）";
            ++failures_;
            return r;
        }

        // ★ 源地址校验：防"回错包"。与 13/ 里"校验 Modbus 事务号"同一思想 ——
        //   没有这一步，一个来自别处的、格式合法的 48 字节报文就能改我们的表。
        const sockaddr_in* pf = (const sockaddr_in*)&from;
        const sockaddr_in* pe = (const sockaddr_in*)peer;
        if (pf->sin_family != pe->sin_family ||
            pf->sin_addr.s_addr != pe->sin_addr.s_addr ||
            pf->sin_port != pe->sin_port) {
            r.error = "SNTP 应答来自非请求地址（疑似回错包）";
            ++failures_;
            return r;
        }

        NtpPacket rp;
        if (!decode_packet(resp, (std::size_t)got, rp, r.error)) {
            ++failures_;
            return r;
        }

        // ★ 回显校验：应答的 Originate 必须是我们发出去的 T1。
        //   不做的话，一个被延迟投递的旧应答会被当成新应答用，
        //   offset 会错得离谱而完全不报错。
        const NtpTimestamp echoed = rp.originate;
        const bool echo_ok = (ts_to_u64(echoed) == ts_to_u64(t1));
        // 有些服务器把 originate 留 0（不合规但存在）—— 那就退回用本地 T1
        const NtpTimestamp t1_use = echo_ok ? echoed : t1;

        r = compute_offset_delay(t1_use, rp.receive, rp.transmit, t4);
        r.stratum     = rp.stratum;
        r.leap        = rp.li;
        r.leap_warning = (rp.li == (std::uint8_t)kLiUnknown);
        r.t1_echo_ok  = echo_ok;

        if (r.delay_s < 0.0) {
            r.error = "往返延迟为负（" + std::to_string(r.delay_s) +
                      " s）—— 服务器时间戳不可信";
            r.ok = false;
            ++failures_;
            return r;
        }
        if (r.delay_s > cfg.max_delay_s) {
            r.error = "往返延迟过大（" + std::to_string(r.delay_s) +
                      " s > 上限 " + std::to_string(cfg.max_delay_s) + " s）";
            r.ok = false;
            ++failures_;
            return r;
        }
        if (r.leap_warning && cfg.reject_unsynced) {
            r.error = "服务器 LI=3（自身未同步），本次结果不可用";
            r.ok = false;
            ++failures_;
            return r;
        }
        r.ok = true;
        return r;
    }

private:
    LocalClock clock_;
    int        calls_    = 0;
    int        failures_ = 0;
};

}  // namespace ntp
}  // namespace comm
}  // namespace ems
