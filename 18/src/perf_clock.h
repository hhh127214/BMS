// =====================================================================
// 18/perf_clock.h — 高精度计时与时钟自证
//
// 为什么不能直接写 std::chrono::steady_clock::now() 就完事：
//
//   本机实测（见 18/docs/README.md「坑 1」与基准报告的「背景噪声」一节）：
//     · steady_clock::period 报 1/1000000000（自称 1 ns 分辨率）
//     · **真实最小正增量是 100 ns**（QPC 周期），即自称比实际细 100 倍
//     · 一次 now() 调用的实测开销 p99 = 0.20 µs、p50 = 0.00 µs
//
//   这带来两条纪律：
//     ① **报"分辨率"必须报"实测最小正增量"**，不能抄 period。一个
//        自称 1 ns、实际 100 ns 的时钟，在 100 ns 量级上是"假精度"。
//     ② 计时开销（≤0.2 µs）相对被测对象（单拍 ~10 µs）占约 2%，
//        必须写进已知边界；被测对象越小，这个占比越不可忽略。
//
//   本文件把"怎么测时钟"本身做成可复现的函数（probe_steady_clock），
//   而不是文档里的一句口头声明 —— 换机器可重测。
//
// 后端选择：std::chrono::steady_clock。本机它由 QPC 驱动（is_steady==true，
//   实测粒度 100 ns），且与 EmsRuntime::step() 内部用的是同一个时钟，
//   外部测量与内部测量口径一致。QPC 直读亦提供（now_qpc_us）作交叉校验。
//
// 编译：需 <windows.h>（只取计时/内存/CPU 时间 API）。本头先定义
//   NOMINMAX / WIN32_LEAN_AND_MEAN，避免 windows.h 的 min/max 宏破坏
//   项目里大量 std::min / std::max（与 13/ 的 modbus 头同一套处理）。
// =====================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <psapi.h>      // GetProcessMemoryInfo / PROCESS_MEMORY_COUNTERS（需 -lpsapi）

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

namespace ems {
namespace perf {

// =====================================================================
// 时钟自证：实测最小正增量（真实粒度）
// =====================================================================
struct ClockProbe {
    const char* backend              = "std::chrono::steady_clock";
    bool        is_steady            = false;
    double      nominal_resolution_ns = 0.0;   // period 声称的分辨率
    double      observed_granularity_ns = 0.0; // 实测最小正增量（真·粒度）
    long long   reads                = 0;      // 采样次数
    long long   changes              = 0;      // 相邻读数发生变化的次数

    std::string to_string() const {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "%s is_steady=%d nominal=%.3f ns observed_granularity=%.3f ns "
                      "(changed %lld/%lld reads)",
                      backend, is_steady ? 1 : 0, nominal_resolution_ns,
                      observed_granularity_ns, changes, reads);
        return std::string(buf);
    }
};

// 实测 steady_clock 的最小正增量。每次调用重新测量（约 40 万次读，几 ms）。
inline ClockProbe probe_steady_clock(long long reads = 400000) {
    ClockProbe p;
    p.is_steady = std::chrono::steady_clock::is_steady;
    p.nominal_resolution_ns =
        1e9 * (double)std::chrono::steady_clock::period::num /
        (double)std::chrono::steady_clock::period::den;
    p.reads = reads;

    long long min_pos = (long long)9e18;
    long long prev = std::chrono::steady_clock::now().time_since_epoch().count();
    for (long long i = 0; i < reads; ++i) {
        const long long cur =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const long long d = cur - prev;
        if (d > 0) {
            if (d < min_pos) min_pos = d;
            ++p.changes;
        }
        prev = cur;
    }
    const double ns_per_tick =
        1e9 * (double)std::chrono::steady_clock::period::num /
        (double)std::chrono::steady_clock::period::den;
    p.observed_granularity_ns = (min_pos == (long long)9e18)
                                    ? 0.0
                                    : (double)min_pos * ns_per_tick;
    return p;
}

// 进程级缓存：只在首次调用时测量（探测量会占几 ms，不宜每拍跑）
inline const ClockProbe& cached_clock_probe() {
    static ClockProbe p = probe_steady_clock();
    return p;
}

// =====================================================================
// 计时原语
// =====================================================================
inline double now_us() {
    using namespace std::chrono;
    return duration<double, std::micro>(steady_clock::now().time_since_epoch())
        .count();
}

// QPC 直读（交叉校验用；本机与 steady_clock 同源，粒度同为 100 ns）
inline double now_qpc_us() {
    static const double inv_hz = []() {
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        return 1.0 / (double)f.QuadPart;
    }();
    LARGE_INTEGER c;
    ::QueryPerformanceCounter(&c);
    return (double)c.QuadPart * inv_hz * 1e6;
}

// 进程 CPU 时间（内核 + 用户），微秒。用于"CPU 时间"口径的基准 ——
// 与墙钟不同，它**剔除了被 OS 挂起的等待**，因此更适合比较两段
// 纯计算代码的代价（与 12/ A4 的墙钟口径互补）。
inline double process_cpu_us() {
    FILETIME c, e, k, u;
    if (!::GetProcessTimes(::GetCurrentProcess(), &c, &e, &k, &u)) return 0.0;
    auto to_us = [](const FILETIME& ft) {
        ULARGE_INTEGER v;
        v.LowPart  = ft.dwLowDateTime;
        v.HighPart = ft.dwHighDateTime;
        return (double)v.QuadPart / 10.0;   // 100 ns 单位 → µs
    };
    return to_us(k) + to_us(u);
}

struct Timer {
    double t0 = 0.0;
    void start() { t0 = now_us(); }
    double stop_us() const { return now_us() - t0; }
};

// 忙等指定微秒。用于反向验证（SLA 的"怎么逼红"）。
// 用**忙等**而非 Sleep：Sleep 粒度是 ms 级且会让出 CPU，
// 无法用来精确构造"这段代码慢了 X µs"。
inline void busy_wait_us(double us) {
    if (us <= 0.0) return;
    const double t0 = now_us();
    while (now_us() - t0 < us) { /* spin */ }
}

// =====================================================================
// 进程资源（内存有界 / 句柄不泄漏）
// =====================================================================
inline double rss_mb() {
    PROCESS_MEMORY_COUNTERS pmc;
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
    return 0.0;
}

inline long long handle_count() {
    DWORD n = 0;
    ::GetProcessHandleCount(::GetCurrentProcess(), &n);
    return (long long)n;
}

// =====================================================================
// 背景噪声：空转基线 + 系统 CPU 忙率
//
// 为什么必须在测量前跑：性能数字的可信边界取决于"测量时机器有多忙"。
// 本项目历史上的一切性能数字都没有附带这个证据 —— 于是"单拍 9.5 µs"
// 既可以是在空转机器上测的，也可以是"恰好那 20000 拍被调度器放过"。
// 拿到忙率与噪声基线，才能说清"这个数在什么条件下成立"。
// =====================================================================

// 系统 CPU 忙率（%），在 interval_ms 的空闲窗口内采样两次 GetSystemTimes。
// 返回 -1 表示取不到。
inline double system_cpu_busy_percent(int interval_ms = 1000) {
    auto to_u64 = [](const FILETIME& ft) {
        ULARGE_INTEGER v;
        v.LowPart  = ft.dwLowDateTime;
        v.HighPart = ft.dwHighDateTime;
        return (unsigned long long)v.QuadPart;
    };
    FILETIME i0, k0, u0, i1, k1, u1;
    if (!::GetSystemTimes(&i0, &k0, &u0)) return -1.0;
    ::Sleep((DWORD)interval_ms);
    if (!::GetSystemTimes(&i1, &k1, &u1)) return -1.0;

    const unsigned long long idle =
        to_u64(i1) - to_u64(i0);
    const unsigned long long kern =
        to_u64(k1) - to_u64(k0);   // 含 idle
    const unsigned long long user =
        to_u64(u1) - to_u64(u0);
    const unsigned long long total = kern + user;
    if (total == 0) return -1.0;
    const double busy = 1.0 - (double)idle / (double)total;
    double pct = 100.0 * busy;
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    return pct;
}

// 计时器自身的噪声地板：测 N 次"读一对时钟、什么都不做"的耗时分布。
// 它给出"本测量装置能分辨的最小差异"—— 任何小于它的结论都不可信。
struct NoiseBaseline {
    long long n        = 0;
    double    p50_us   = 0.0;
    double    p99_us   = 0.0;
    double    max_us   = 0.0;
    double    min_us   = 0.0;
    double    mean_us  = 0.0;
    double    cv_pct   = 0.0;   // 变异系数（标准差/均值），调度抖动的指标
};

inline double pct_of(const double* sorted, long long n, double q) {
    if (n <= 0) return 0.0;
    long long rank = (long long)std::ceil(q * (double)n);
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    return sorted[rank - 1];
}

// 空转基线：把"读一对时钟"当被测对象，得到测量装置本身的噪声地板。
inline NoiseBaseline measure_timer_noise(long long n = 20000) {
    NoiseBaseline b;
    b.n = n;
    if (n <= 0) return b;
    std::string dummy;
    double* v = new double[(size_t)n];
    for (long long i = 0; i < n; ++i) {
        const double a = now_us();
        const double c = now_us();
        v[i] = c - a;
        dummy += "";                 // 防优化掉循环
    }
    double* s = new double[(size_t)n];
    for (long long i = 0; i < n; ++i) s[i] = v[i];
    // 插入排序最坏 O(n^2)，改用简单堆排替代：这里直接用 std::sort 不可用
    // （本头不引 <algorithm>，避免与调用方重排 include）。手写快排。
    struct Q {
        static void sort(double* a, long long lo, long long hi) {
            while (lo < hi) {
                const double pivot = a[(lo + hi) / 2];
                long long i = lo, j = hi;
                while (i <= j) {
                    while (a[i] < pivot) ++i;
                    while (a[j] > pivot) --j;
                    if (i <= j) {
                        const double t = a[i]; a[i] = a[j]; a[j] = t;
                        ++i; --j;
                    }
                }
                if (j - lo < hi - i) { sort(a, lo, j); lo = i; }
                else                 { sort(a, i, hi); hi = j; }
            }
        }
    };
    Q::sort(s, 0, n - 1);

    double sum = 0.0;
    for (long long i = 0; i < n; ++i) sum += s[i];
    b.mean_us = sum / (double)n;
    double ss = 0.0;
    for (long long i = 0; i < n; ++i) {
        const double d = s[i] - b.mean_us;
        ss += d * d;
    }
    const double sd = std::sqrt(ss / (double)(n > 1 ? n - 1 : 1));
    b.cv_pct  = (b.mean_us > 1e-12) ? 100.0 * sd / b.mean_us : 0.0;
    b.min_us  = s[0];
    b.p50_us  = pct_of(s, n, 0.50);
    b.p99_us  = pct_of(s, n, 0.99);
    b.max_us  = s[n - 1];
    delete[] v;
    delete[] s;
    (void)dummy;
    return b;
}

// =====================================================================
// 机器 / 编译器信息（报告头用）
// =====================================================================
struct MachineInfo {
    std::string cpu_identifier;
    std::string cores;
    std::string os;
    std::string arch;
    std::string compiler;
    std::string build_date;
    std::string stamp;
    std::string hostname;
};

inline std::string env_or(const char* k, const char* def) {
    const char* v = std::getenv(k);
    return std::string(v ? v : def);
}

inline MachineInfo machine_info() {
    MachineInfo m;
    m.cpu_identifier = env_or("PROCESSOR_IDENTIFIER", "(unknown)");
    m.cores          = env_or("NUMBER_OF_PROCESSORS", "(unknown)");
    m.os             = env_or("OS", "(unknown)");
    m.arch           = env_or("PROCESSOR_ARCHITECTURE", "(unknown)");
    char cb[64];
    std::snprintf(cb, sizeof(cb), "g++ %d.%d.%d", __GNUC__, __GNUC_MINOR__,
                  __GNUC_PATCHLEVEL__);
    m.compiler = cb;
#if defined(__cplusplus)
    m.compiler += std::string(" (C++") +
                  std::to_string((long)(__cplusplus / 100 % 100)) + ")";
#endif
    std::snprintf(cb, sizeof(cb), "%s %s", __DATE__, __TIME__);
    m.build_date = cb;
    {
        std::time_t tt = std::time(nullptr);
        std::tm tmv{};
#if defined(_WIN32)
        localtime_s(&tmv, &tt);
#else
        localtime_r(&tt, &tmv);
#endif
        char sb[32];
        std::snprintf(sb, sizeof(sb), "%04d-%02d-%02d %02d:%02d:%02d",
                      tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                      tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        m.stamp = sb;
    }
    {
        char hb[MAX_COMPUTERNAME_LENGTH + 1] = {0};
        DWORD n = sizeof(hb);
        if (::GetComputerNameA(hb, &n)) m.hostname = hb;
        else                            m.hostname = "(unknown)";
    }
    return m;
}

} // namespace perf
} // namespace ems
