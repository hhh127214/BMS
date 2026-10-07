// =====================================================================
// 16/ 单元测试 —— 传输层安全（TLS，T80~T89）
//
//   T80 明文对照组（PlainTransport）：能连通、is_encrypted()==false
//   T81 ★ 把 TLS 打到明文回显口 → **必须失败**，且不得自称"已加密"
//   T82 ★ 无后端时**显式拒绝**（Backend::kNone）—— 逐字钉住拒绝原因
//   T83 ★ 真 TLS 端到端回显（SChannel × Python ssl/OpenSSL），
//         断言 is_encrypted()==true、协商到 TLS1.2、多轮握手、对端证书
//   T84 ★ 证书校验为**开**时自签证书**必须**被拒（SEC_E_UNTRUSTED_ROOT），
//         且反向守卫"关掉校验后同一台服务端连得上"
//   T85 大报文（256 KiB 伪随机）经 TLS 往返逐位一致（跨多条 TLS 记录）
//   T86 ★ 版本协商是真的：只收 TLS1.1 的服务端能连（协商出 TLS1.1）、
//         只收 TLS1.3 的服务端连不上
//   T87 ★ tls12_only 的窄化**可观测**：同一个 TLS1.1 服务端，
//         默认能连、tls12_only=true 连不上
//   T88 关闭/重连语义 + 对"确定没在听"的端口必须失败
//   T89 统一抽象 ITransport：同一套调用在明文/TLS 两种实现下结果不同
//
// ── 为什么需要一个**进程外**的 TLS 服务端 ──────────────────────────
// TLS 客户端要"端到端"证明，对面必须是一个**别人写的** TLS 实现。
// 用 SChannel 自己再写一个服务端来对接，等于自己批改自己的卷子：
// 双方共享同一个 bug 时测试照样全绿。
// 所以这里拉起 Python 的 ssl 模块（OpenSSL 3.x）——
// 它能和我们握手成功，才说明我们发出去的真的是 TLS。
//
// 服务端进程由本测试自己 CreateProcess 拉起、结束时 TerminateProcess 收掉；
// 找不到 python 时**显式 SKIP**（计入 SKIPPED），绝不静默变成 PASS。
//
// 端口：default=2443 / tls11only=2445 / tls13only=2447
//
// 编译：见 16/scripts/build_test.bat（需要 -lsecur32 -lcrypt32 -lws2_32）
// =====================================================================

#include "tls.h"
#include "net_base.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

using namespace ems::comm;

static int g_pass = 0;
static int g_fail = 0;
static int g_skip = 0;

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

// 字符串比较：EXPECT_EQ 内部转 long long，std::string 过不去
#define EXPECT_STR_EQ(a, b)                                               \
    do {                                                                  \
        std::string va_ = (a), vb_ = (b);                                 \
        if (va_ == vb_) {                                                 \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va_ << " vs " << #b << "=" \
                      << vb_ << std::endl;                                \
        }                                                                 \
    } while (0)

#define EXPECT_CONTAINS(hay, needle)                                      \
    do {                                                                  \
        std::string h_ = (hay), n_ = (needle);                            \
        if (h_.find(n_) != std::string::npos) {                           \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : 期望包含 \"" << n_ << "\"，实际为 \"" << h_  \
                      << "\"" << std::endl;                               \
        }                                                                 \
    } while (0)

// ★ 显式跳过：打出来、记一笔，但**不计** pass 也不计 fail
#define SKIP(why)                                                         \
    do {                                                                  \
        ++g_skip;                                                         \
        std::printf("  SKIP  %s\n", why);                                 \
    } while (0)

// =====================================================================
// 小工具
// =====================================================================
static std::vector<std::uint8_t> pseudo_random(std::size_t n, std::uint32_t seed) {
    std::vector<std::uint8_t> v(n);
    std::uint32_t x = seed ? seed : 0x2545F491u;
    for (std::size_t i = 0; i < n; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;      // xorshift32
        v[i] = (std::uint8_t)(x >> 11);
    }
    return v;
}

// 收满 want 字节（TLS 一次 recv 可能只给一段，必须循环收）
static bool recv_exact(tls::ITransport& t, std::vector<std::uint8_t>& out,
                       std::size_t want, int timeout_ms) {
    out.clear();
    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    while (out.size() < want) {
        const long long el = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0).count();
        if (el > timeout_ms) return false;
        std::uint8_t buf[8192];
        const int n = t.recv(buf, sizeof(buf));
        if (n < 0) return false;
        if (n == 0) break;                       // 对端正常关闭
        out.insert(out.end(), buf, buf + (std::size_t)n);
    }
    return out.size() == want;
}

static bool bytes_equal(const std::vector<std::uint8_t>& a,
                        const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

// =====================================================================
// 进程内明文回显服务端（PlainTransport 的对照组，以及"TLS 打到明文口"的靶子）
// =====================================================================
class PlainEchoServer {
public:
    ~PlainEchoServer() { stop(); }

    bool start(std::string& err) {
        if (!listener_.listen_on(0, 8, err)) return false;
        port_ = listener_.bound_port();
        if (port_ == 0) { err = "拿不到监听端口"; return false; }
        th_ = std::thread(&PlainEchoServer::loop, this);
        return true;
    }
    std::uint16_t port() const { return port_; }

    void stop() {
        stop_.store(true);
        listener_.close();
        if (th_.joinable()) th_.join();
    }

private:
    void loop() {
        while (!stop_.load()) {
            bool timed_out = false;
            const net::socket_t s = listener_.accept_one(100, timed_out);
            if (s == net::kInvalidSocket) continue;
            net::set_recv_timeout(s, 2000);
            std::uint8_t buf[4096];
            const int n = ::recv(s, (char*)buf, (int)sizeof(buf), 0);
            if (n > 0) {
                std::size_t off = 0;
                while (off < (std::size_t)n) {
                    const int w = ::send(s, (const char*)buf + off,
                                         (int)((std::size_t)n - off), 0);
                    if (w <= 0) break;
                    off += (std::size_t)w;
                }
            }
            net::close_socket(s);
        }
    }

    net::TcpListener listener_;
    std::uint16_t     port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread       th_;
};

// =====================================================================
// 进程外 TLS 回显服务端（Python ssl / OpenSSL）
// =====================================================================
class PyTlsServer {
public:
    PyTlsServer(int port, const std::string& mode) : port_(port), mode_(mode) {}
    ~PyTlsServer() { stop(); }

    bool up() const { return up_; }
    const std::string& why() const { return why_; }
    int port() const { return port_; }

    bool start() {
        const std::string script = find_script();
        if (script.empty()) {
            why_ = "找不到 tests/fixtures/tls_echo_server.py（工作目录不对？）";
            return false;
        }
        const std::string py = find_python();
        if (py.empty()) {
            why_ = "找不到可用的 python（EMS_PYTHON 未设置或无效，PATH 上也没有 python.exe / py.exe）";
            return false;
        }

        std::string cmd = "\"" + py + "\" \"" + script + "\" " +
                          std::to_string(port_) + " " + mode_;
        std::vector<char> buf(cmd.begin(), cmd.end());
        buf.push_back('\0');

        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = NULL;
        sa.bInheritHandle = TRUE;
        // 子进程的 stdout/stderr 落到 build/tls_srv_<port>.log：
        // 直接继承控制台会把 "READY"/"conn_error" 混进测试输出，
        // 而 .bat 的输出要能被人一眼读。
        char logpath[128];
        std::snprintf(logpath, sizeof(logpath), "build/tls_srv_%d.log", port_);
        HANDLE logf = ::CreateFileA(logpath, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        HANDLE nul = ::CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

        STARTUPINFOA si;
        std::memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        if (logf != INVALID_HANDLE_VALUE) {
            si.dwFlags     = STARTF_USESTDHANDLES;
            si.hStdOutput  = logf;
            si.hStdError   = logf;
            si.hStdInput   = (nul != INVALID_HANDLE_VALUE) ? nul : logf;
        } else if (nul != INVALID_HANDLE_VALUE) {
            si.dwFlags     = STARTF_USESTDHANDLES;
            si.hStdOutput  = nul;
            si.hStdError   = nul;
            si.hStdInput   = nul;
        }

        PROCESS_INFORMATION pi;
        std::memset(&pi, 0, sizeof(pi));
        const BOOL ok = ::CreateProcessA(py.c_str(), buf.data(), NULL, NULL,
                                         TRUE, CREATE_NO_WINDOW, NULL, NULL,
                                         &si, &pi);
        const DWORD last = ::GetLastError();
        if (logf != INVALID_HANDLE_VALUE) ::CloseHandle(logf);
        if (nul != INVALID_HANDLE_VALUE) ::CloseHandle(nul);
        if (!ok) {
            why_ = "CreateProcess 拉起 python 失败（err=" + std::to_string((long)last) + "）";
            return false;
        }
        proc_ = pi.hProcess;
        thread_ = pi.hThread;
        up_ = wait_port(10000);
        if (!up_) why_ = "python 服务端起不来（10 s 内端口 " + std::to_string(port_) +
                         " 未监听；最后一次连接尝试：" + last_connect_error_ +
                         "；子进程输出见 build/tls_srv_" + std::to_string(port_) + ".log）";
        return up_;
    }

    void stop() {
        if (proc_ != NULL) {
            ::TerminateProcess(proc_, 0);
            ::WaitForSingleObject(proc_, 3000);
            ::CloseHandle(proc_);
            proc_ = NULL;
        }
        if (thread_ != NULL) { ::CloseHandle(thread_); thread_ = NULL; }
        up_ = false;
    }

private:
    static bool file_exists(const std::string& p) {
        const DWORD a = ::GetFileAttributesA(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    static std::string find_script() {
        const char* cands[] = {
            "tests/fixtures/tls_echo_server.py",
            "16/tests/fixtures/tls_echo_server.py",
            "../tests/fixtures/tls_echo_server.py",
        };
        for (int i = 0; i < 3; ++i) {
            if (file_exists(cands[i])) return cands[i];
        }
        return std::string();
    }

    // ★ EMS_PYTHON 一旦设了就以它为准，**不做静默回退**：
    //   设错路径却还被 PATH 上的 python 悄悄顶替，会让"我明明指定了那个解释器"
    //   变成一句假话 —— 现场最难查的一类问题。
    static std::string find_python() {
        char env[512];
        std::memset(env, 0, sizeof(env));
        const DWORD n = ::GetEnvironmentVariableA("EMS_PYTHON", env, sizeof(env));
        if (n > 0) {
            if (file_exists(env)) return std::string(env);
            return std::string();
        }

        const char* names[] = {"python.exe", "py.exe"};
        for (int i = 0; i < 2; ++i) {
            char got[MAX_PATH];
            std::memset(got, 0, sizeof(got));
            if (::SearchPathA(NULL, names[i], NULL, MAX_PATH, got, NULL) > 0) {
                return std::string(got);
            }
        }
        return std::string();
    }

    bool wait_port(int timeout_ms) {
        for (int waited = 0; waited < timeout_ms; waited += 100) {
            net::TcpSocket s;
            std::string e;
            if (s.connect_to("127.0.0.1", (std::uint16_t)port_, 250, e)) {
                s.close();
                return true;
            }
            last_connect_error_ = e;
            ::Sleep(100);
        }
        return false;
    }

    int          port_;
    std::string  mode_;
    bool         up_ = false;
    std::string  why_;
    std::string  last_connect_error_;
    HANDLE       proc_   = NULL;
    HANDLE       thread_ = NULL;
};

// =====================================================================
// T80 明文对照组
// =====================================================================
static void test_80_plain_baseline() {
    std::printf("T80 明文对照组（PlainTransport）\n");

    PlainEchoServer srv;
    std::string err;
    if (!srv.start(err)) { std::cerr << "  FAIL  起明文回显口失败：" << err << std::endl; ++g_fail; return; }

    tls::PlainTransport p("127.0.0.1", srv.port(), 2000);
    const bool ok = p.connect();
    const std::string ce = p.last_error();
    EXPECT(ok);
    if (!ok) std::cerr << "    (connect 失败：" << ce << ")" << std::endl;

    EXPECT(p.is_up());
    EXPECT(!p.is_encrypted());                     // ★ 对照组：明文**不是**加密的
    EXPECT_STR_EQ(std::string(p.name()), std::string("PlainTransport"));
    EXPECT_CONTAINS(p.endpoint(), "明文");

    const std::string msg = "EMS-PLAIN-0123456789";
    EXPECT_EQ(p.send((const std::uint8_t*)msg.data(), msg.size()), (long long)msg.size());

    std::vector<std::uint8_t> got;
    EXPECT(recv_exact(p, got, msg.size(), 3000));
    EXPECT_STR_EQ(std::string((const char*)got.data(), got.size()), msg);

    // 反向守卫：没连上时 send/recv 必须**失败**，不能假装成功
    tls::PlainTransport dead("127.0.0.1", srv.port(), 500);
    EXPECT_EQ(dead.send((const std::uint8_t*)msg.data(), 4), -1);
    EXPECT(!dead.last_error().empty());
    std::uint8_t one = 0;
    EXPECT(dead.recv(&one, 1) < 0);
    EXPECT(!dead.is_up());

    p.close();
    EXPECT(!p.is_up());
    srv.stop();
}

// =====================================================================
// T81 ★ TLS 打到明文口
// =====================================================================
static void test_81_tls_to_plain_port() {
    std::printf("T81 ★ TLS 打到明文口必须失败（不得静默退化成明文）\n");

    PlainEchoServer srv;
    std::string err;
    if (!srv.start(err)) { std::cerr << "  FAIL  起明文回显口失败：" << err << std::endl; ++g_fail; return; }

    tls::TlsTransport::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = srv.port();
    cfg.timeout_ms = 2000;
    cfg.sni_host = "localhost";
    cfg.insecure_skip_verify = true;

    tls::TlsTransport t(cfg);
    const bool ok = t.connect();
    const std::string e = t.last_error();
    if (!ok) std::cout << "    (预期内的失败：" << e << ")" << std::endl;

    EXPECT(!ok);                                   // ★ 核心
    EXPECT(!e.empty());
    EXPECT(!t.is_up());
    EXPECT(!t.is_encrypted());                     // ★ 失败后绝不许自称"已加密"
    std::uint8_t one = 0;
    EXPECT_EQ(t.send(&one, 1), -1);

    // ★ 反向守卫：同一个端口用**明文**是通的 ——
    //   所以上面的失败原因真的是 TLS，而不是"端口根本没开"
    tls::PlainTransport p("127.0.0.1", srv.port(), 2000);
    EXPECT(p.connect());
    p.close();

    t.close();
    srv.stop();
}

// =====================================================================
// T82 ★ 显式拒绝
// =====================================================================
static void test_82_explicit_refusal() {
    std::printf("T82 ★ 无后端时显式拒绝（Backend::kNone）\n");

    // 拒绝原因串本身是**文档 + 断言目标**：不能随手改文案
    const std::string& why = tls::missing_backend_reason();
    EXPECT(!why.empty());
    EXPECT_CONTAINS(why, "未编译 TLS 支持");
    EXPECT_CONTAINS(why, "SChannel");
    EXPECT_CONTAINS(why, "OpenSSL");
    EXPECT_CONTAINS(why, "16/docs/README.md");

    tls::TlsTransport::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 1;
    cfg.timeout_ms = 300;

    tls::TlsTransport t(cfg, tls::Backend::kNone);
    EXPECT(t.backend() == tls::Backend::kNone);
    EXPECT(t.requested_backend() == tls::Backend::kNone);
    EXPECT(!t.insecure());                          // 默认**校验**证书

    const bool ok = t.connect();
    const std::string e = t.last_error();
    EXPECT(!ok);                                    // ★ 显式拒绝 = 连不上
    EXPECT_EQ(e.compare(0, why.size(), why), 0);    // 原因串是整条错误的**开头**
    EXPECT_CONTAINS(e, "Backend::kNone");           // 且说清是"本实例显式选的"
    EXPECT(!t.is_up());
    EXPECT(!t.is_encrypted());                      // ★ 拒绝之后绝不自称加密
    std::uint8_t one = 0;
    EXPECT_EQ(t.send(&one, 1), -1);
    EXPECT_EQ(t.recv(&one, 1), -1);

    EXPECT_STR_EQ(std::string(tls::backend_name(tls::Backend::kNone)), "none");

    // 默认后端在本平台解析到哪 —— 这是"有条件才跑真 TLS"的开关
    const tls::Backend def = tls::resolve_backend(tls::Backend::kDefault);
    if (tls::schannel_compiled_in()) {
        EXPECT(def == tls::Backend::kSchannel);
        EXPECT_STR_EQ(std::string(tls::backend_name(tls::Backend::kDefault)), "schannel");
        EXPECT_STR_EQ(std::string(tls::backend_name(tls::Backend::kSchannel)), "schannel");
    } else {
        EXPECT(def == tls::Backend::kNone);
        EXPECT_STR_EQ(std::string(tls::backend_name(tls::Backend::kDefault)), "none");
        SKIP("本平台没编译进 SChannel —— 真 TLS 用例跳过");
    }

    // SECURITY_STATUS 翻译：**未收录的码也必须原样打出来**
    EXPECT_STR_EQ(tls::sec_status_text(0L), std::string("SEC_E_OK（成功）"));
    EXPECT_CONTAINS(tls::sec_status_text((long)0x80090325UL), "SEC_E_UNTRUSTED_ROOT");
    EXPECT_CONTAINS(tls::sec_status_text((long)0x80090325UL), "信任库");
    EXPECT_CONTAINS(tls::sec_status_text((long)0x80090318UL), "不完整");
    EXPECT_CONTAINS(tls::sec_status_text((long)0x12345678L), "0x12345678");
    EXPECT_CONTAINS(tls::sec_status_text((long)0x12345678L), "未收录");
}

// =====================================================================
// T83 ★ 真 TLS 端到端回显
// =====================================================================
static void test_83_real_tls_echo(PyTlsServer& srv) {
    std::printf("T83 ★ 真 TLS 端到端回显（SChannel ↔ Python ssl/OpenSSL）\n");

    if (!srv.up()) { SKIP(srv.why().c_str()); return; }

    tls::TlsTransport::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = (std::uint16_t)srv.port();
    cfg.timeout_ms = 5000;
    cfg.sni_host = "localhost";
    cfg.insecure_skip_verify = true;      // 自签证书 + 只验通道本身

    tls::TlsTransport t(cfg);
    EXPECT(t.insecure());                 // 配置如实反映
    EXPECT(!t.is_encrypted());            // ★ 构造出来**不能**立刻自称加密
    EXPECT(!t.is_up());

    const bool ok = t.connect();
    const std::string e = t.last_error();
    if (!ok) std::cerr << "    (connect 失败：" << e << ")" << std::endl;
    EXPECT(ok);
    if (!ok) return;

    EXPECT(t.is_up());
    EXPECT(t.is_encrypted());             // ★ 握手完成后才 true
    EXPECT(t.backend() == tls::Backend::kSchannel);
    EXPECT_STR_EQ(std::string(t.name()), std::string("TlsTransport"));
    EXPECT(t.handshake_rounds() >= 3);    // 多轮握手：不是一次调用搞定
    // ★ 版本是不是真的协商出来的：对面是 OpenSSL，它也只能按 ClientHello 来答
    EXPECT_STR_EQ(t.negotiated_protocol(), std::string("TLS1.2"));
    EXPECT_CONTAINS(t.peer_cert_subject(), "localhost");   // 证书真的被读到了
    EXPECT_CONTAINS(t.endpoint(), "未校验证书");
    EXPECT_CONTAINS(t.endpoint(), "2443");

    const std::string msg = "EMS-TLS-PING-0123456789-ABCDEF";
    EXPECT_EQ(t.send((const std::uint8_t*)msg.data(), msg.size()), (long long)msg.size());

    std::vector<std::uint8_t> got;
    EXPECT(recv_exact(t, got, msg.size(), 5000));
    EXPECT_EQ(got.size(), msg.size());
    EXPECT_STR_EQ(std::string((const char*)got.data(), got.size()), msg);

    t.close();
    EXPECT(!t.is_up());
    EXPECT(!t.is_encrypted());            // ★ 关掉后不许还自称加密
}

// =====================================================================
// T84 ★ 证书校验为开 → 自签证书必须被拒
// =====================================================================
static void test_84_cert_verify_on(PyTlsServer& srv) {
    std::printf("T84 ★ 证书校验为开时自签证书必须被拒\n");

    if (!srv.up()) { SKIP(srv.why().c_str()); return; }

    tls::TlsTransport::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = (std::uint16_t)srv.port();
    cfg.timeout_ms = 5000;
    cfg.sni_host = "localhost";
    // ★ 故意**不写** cfg.insecure_skip_verify —— 用默认值
    EXPECT(!cfg.insecure_skip_verify);     // 默认必须是"校验"

    tls::TlsTransport t(cfg);
    EXPECT(!t.insecure());

    const bool ok = t.connect();
    const std::string e = t.last_error();
    std::cout << "    (预期内的拒绝：" << e << ")" << std::endl;

    EXPECT(!ok);                                   // ★ 自签证书必须被拒
    EXPECT_CONTAINS(e, "证书校验为**开**");         // 失败原因必须写明校验是**开**的
    EXPECT_CONTAINS(e, "SEC_E_UNTRUSTED_ROOT");    // 而且是被"根 CA 不信任"拒的
    EXPECT(!t.is_up());
    EXPECT(!t.is_encrypted());

    // ★ 反向守卫：把校验关掉，**同一台服务端**必须连得上。
    //   否则"被拒"的原因就不是证书（比如端口不通），上面那条断言就白测了。
    tls::TlsTransport::Config cfg2 = cfg;
    cfg2.insecure_skip_verify = true;
    tls::TlsTransport t2(cfg2);
    const bool ok2 = t2.connect();
    if (!ok2) std::cerr << "    (关校验后仍失败：" << t2.last_error() << ")" << std::endl;
    EXPECT(ok2);
    EXPECT(t2.is_encrypted());
    t2.close();
    t.close();
}

// =====================================================================
// T85 大报文往返
// =====================================================================
static void test_85_large_payload(PyTlsServer& srv) {
    std::printf("T85 大报文经 TLS 往返（256 KiB 伪随机）\n");

    if (!srv.up()) { SKIP(srv.why().c_str()); return; }

    tls::TlsTransport::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = (std::uint16_t)srv.port();
    cfg.timeout_ms = 15000;
    cfg.sni_host = "localhost";
    cfg.insecure_skip_verify = true;

    tls::TlsTransport t(cfg);
    const bool ok = t.connect();
    if (!ok) {
        std::cerr << "    (connect 失败：" << t.last_error() << ")" << std::endl;
        ++g_fail;
        return;
    }
    EXPECT(ok);

    const std::size_t N = 256u * 1024u;
    const std::vector<std::uint8_t> payload = pseudo_random(N, 0xC0FFEE11u);

    // send() 内部按 cbMaximumMessage 分段加密 —— 一次 256 KiB 会跨多条 TLS 记录
    EXPECT_EQ(t.send(payload.data(), payload.size()), (long long)payload.size());

    std::vector<std::uint8_t> got;
    EXPECT(recv_exact(t, got, N, 30000));
    EXPECT_EQ(got.size(), N);
    EXPECT(bytes_equal(got, payload));             // ★ 逐位一致

    // 反向守卫：故意改一个字节，比较函数必须说"不相等"（防止 bytes_equal 恒真）
    std::vector<std::uint8_t> tampered = got;
    if (!tampered.empty()) tampered[N / 2] ^= 0x01;
    EXPECT(!bytes_equal(tampered, payload));

    t.close();
}

// =====================================================================
// T86 ★ 版本协商
// =====================================================================
static void test_86_version_negotiation(PyTlsServer& srv11, PyTlsServer& srv13) {
    std::printf("T86 ★ 版本协商：只收 TLS1.1 的能连、只收 TLS1.3 的连不上\n");

    if (!srv11.up() || !srv13.up()) {
        SKIP("TLS1.1/TLS1.3 专用服务端没起来");
        return;
    }

    // ---- 默认（1.0/1.1/1.2 都提供）打 TLS1.1 服务端：应当协商到 TLS1.1 ----
    {
        tls::TlsTransport::Config cfg;
        cfg.host = "127.0.0.1";
        cfg.port = (std::uint16_t)srv11.port();
        cfg.timeout_ms = 5000;
        cfg.sni_host = "localhost";
        cfg.insecure_skip_verify = true;
        tls::TlsTransport t(cfg);
        const bool ok = t.connect();
        if (!ok) std::cerr << "    (TLS1.1 连接失败：" << t.last_error() << ")" << std::endl;
        EXPECT(ok);
        if (ok) {
            EXPECT_STR_EQ(t.negotiated_protocol(), std::string("TLS1.1"));
            EXPECT(t.is_encrypted());
        }
        t.close();
    }

    // ---- 默认打"只收 TLS1.3"的服务端：我们**不**提供 1.3 → 必须失败 ----
    {
        tls::TlsTransport::Config cfg;
        cfg.host = "127.0.0.1";
        cfg.port = (std::uint16_t)srv13.port();
        cfg.timeout_ms = 5000;
        cfg.sni_host = "localhost";
        cfg.insecure_skip_verify = true;
        tls::TlsTransport t(cfg);
        const bool ok = t.connect();
        const std::string e = t.last_error();
        if (!ok) std::cout << "    (预期内的失败：" << e << ")" << std::endl;
        EXPECT(!ok);                               // ★ 版本不匹配就是连不上
        EXPECT(!e.empty());
        EXPECT(!t.is_encrypted());
        t.close();
    }
}

// =====================================================================
// T87 ★ tls12_only 的窄化可观测
// =====================================================================
static void test_87_tls12_only(PyTlsServer& srv11, PyTlsServer& srvDef) {
    std::printf("T87 ★ tls12_only 的窄化必须可观测\n");

    if (!srv11.up() || !srvDef.up()) { SKIP("服务端没起来"); return; }

    // ---- tls12_only=true 打"只收 TLS1.1"的服务端 → 必须连不上 ----
    //      （T86 里同一个端口、默认配置是能连上的 —— 差异就来自这个开关）
    {
        tls::TlsTransport::Config cfg;
        cfg.host = "127.0.0.1";
        cfg.port = (std::uint16_t)srv11.port();
        cfg.timeout_ms = 5000;
        cfg.sni_host = "localhost";
        cfg.insecure_skip_verify = true;
        cfg.tls12_only = true;
        tls::TlsTransport t(cfg);
        const bool ok = t.connect();
        const std::string e = t.last_error();
        if (!ok) std::cout << "    (预期内的失败：" << e << ")" << std::endl;
        EXPECT(!ok);
        EXPECT(!t.is_encrypted());
        t.close();
    }

    // ---- 反向守卫：tls12_only=true 打 TLS1.2 服务端 → 必须连上，
    //      且协商出的就是 TLS1.2（不是"连上了但其实是 1.1"）----
    {
        tls::TlsTransport::Config cfg;
        cfg.host = "127.0.0.1";
        cfg.port = (std::uint16_t)srvDef.port();
        cfg.timeout_ms = 5000;
        cfg.sni_host = "localhost";
        cfg.insecure_skip_verify = true;
        cfg.tls12_only = true;
        tls::TlsTransport t(cfg);
        const bool ok = t.connect();
        if (!ok) std::cerr << "    (connect 失败：" << t.last_error() << ")" << std::endl;
        EXPECT(ok);
        if (ok) EXPECT_STR_EQ(t.negotiated_protocol(), std::string("TLS1.2"));
        t.close();
    }
}

// =====================================================================
// T88 关闭 / 重连 / 对空端口
// =====================================================================
static void test_88_close_reconnect(PyTlsServer& srv) {
    std::printf("T88 关闭、重连与「对空端口」必须失败\n");

    // ---- 对"确定没在听"的端口：先占一个端口再放掉 ----
    {
        net::TcpListener l;
        std::string err;
        EXPECT(l.listen_on(0, 4, err));
        const std::uint16_t dead_port = l.bound_port();
        EXPECT(dead_port != 0);
        l.close();

        tls::TlsTransport::Config cfg;
        cfg.host = "127.0.0.1";
        cfg.port = dead_port;
        cfg.timeout_ms = 1500;
        tls::TlsTransport t(cfg);
        const bool ok = t.connect();
        const std::string e = t.last_error();
        EXPECT(!ok);
        EXPECT_CONTAINS(e, "TCP 连接失败");        // 失败发生在 TCP 层，且原因写明
        EXPECT(!t.is_up());
        EXPECT(!t.is_encrypted());
    }

    if (!srv.up()) { SKIP(srv.why().c_str()); return; }

    tls::TlsTransport::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = (std::uint16_t)srv.port();
    cfg.timeout_ms = 5000;
    cfg.sni_host = "localhost";
    cfg.insecure_skip_verify = true;

    tls::TlsTransport t(cfg);
    EXPECT(t.connect());
    EXPECT(t.is_up());
    EXPECT(t.is_encrypted());
    EXPECT(t.handshake_rounds() >= 3);

    t.close();                                      // 关掉
    EXPECT(!t.is_up());
    EXPECT(!t.is_encrypted());
    EXPECT_EQ(t.handshake_rounds(), 0);             // 轮次归零，不是残留上次的
    EXPECT(t.endpoint().find("127.0.0.1") != std::string::npos);  // 描述信息仍在

    // 同一个对象重连必须可用（现场就是断线重连）
    const bool again = t.connect();
    if (!again) std::cerr << "    (重连失败：" << t.last_error() << ")" << std::endl;
    EXPECT(again);
    EXPECT(t.is_up());
    EXPECT(t.is_encrypted());
    const std::string msg = "RECONNECT-OK";
    EXPECT_EQ(t.send((const std::uint8_t*)msg.data(), msg.size()), (long long)msg.size());
    std::vector<std::uint8_t> got;
    EXPECT(recv_exact(t, got, msg.size(), 5000));
    EXPECT_STR_EQ(std::string((const char*)got.data(), got.size()), msg);
    t.close();
}

// =====================================================================
// T89 统一抽象
// =====================================================================
static void test_89_transport_abstraction() {
    std::printf("T89 统一抽象 ITransport：同一套调用、两种实现\n");

    PlainEchoServer srv;
    std::string err;
    if (!srv.start(err)) { std::cerr << "  FAIL  起明文回显口失败：" << err << std::endl; ++g_fail; return; }

    tls::PlainTransport plain("127.0.0.1", srv.port(), 2000);
    tls::TlsTransport::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = srv.port();
    cfg.timeout_ms = 500;
    tls::TlsTransport tlsr(cfg, tls::Backend::kNone);   // 不真连，只看接口行为

    // ★ 通过基类指针调用：is_encrypted()/name() 必须是**虚**的，不能是常量
    std::vector<tls::ITransport*> v;
    v.push_back(&plain);
    v.push_back(&tlsr);

    EXPECT_STR_EQ(std::string(v[0]->name()), std::string("PlainTransport"));
    EXPECT_STR_EQ(std::string(v[1]->name()), std::string("TlsTransport"));
    EXPECT(v[0]->is_encrypted() == false);
    EXPECT(v[1]->is_encrypted() == false);          // 未握手 → false（不是恒真）

    EXPECT(v[0]->connect());
    EXPECT(v[0]->is_encrypted() == false);          // ★ 明文连上了也仍然是 false
    EXPECT(v[0]->is_up());

    const std::string msg = "ABSTRACT-OK";
    EXPECT_EQ(v[0]->send((const std::uint8_t*)msg.data(), msg.size()), (long long)msg.size());
    std::vector<std::uint8_t> got;
    EXPECT(recv_exact(*v[0], got, msg.size(), 3000));
    EXPECT_STR_EQ(std::string((const char*)got.data(), got.size()), msg);

    // 同一个抽象、同一个调用：TLS 实现在没有后端时**必须**拒绝
    EXPECT(!v[1]->connect());
    EXPECT(v[1]->is_up() == false);
    EXPECT(v[1]->is_encrypted() == false);
    EXPECT(!v[1]->last_error().empty());

    // endpoint() 两种实现的措辞必须能区分（现场日志靠它判断走没走加密）
    EXPECT(v[0]->endpoint() != v[1]->endpoint());
    EXPECT_CONTAINS(v[0]->endpoint(), "明文");
    EXPECT_CONTAINS(v[1]->endpoint(), "TLS");

    v[0]->close();
    EXPECT(!plain.is_up());
    srv.stop();
}

// =====================================================================
int main() {
    std::printf("=== 16/ 传输层安全（TLS） 单元测试 ===\n\n");

    test_80_plain_baseline();
    test_81_tls_to_plain_port();
    test_82_explicit_refusal();

    // 真 TLS 的三个服务端：默认 / 只收 TLS1.1 / 只收 TLS1.3
    PyTlsServer srv_def(2443, "default");
    PyTlsServer srv_11 (2445, "tls11only");
    PyTlsServer srv_13 (2447, "tls13only");
    if (tls::schannel_compiled_in()) {
        srv_def.start();
        srv_11.start();
        srv_13.start();
    }

    test_83_real_tls_echo(srv_def);
    test_84_cert_verify_on(srv_def);
    test_85_large_payload(srv_def);
    test_86_version_negotiation(srv_11, srv_13);
    test_87_tls12_only(srv_11, srv_def);
    test_88_close_reconnect(srv_def);
    test_89_transport_abstraction();

    srv_def.stop();
    srv_11.stop();
    srv_13.stop();

    // ★ 这一行必须与"本次到底跑了什么"一致 —— 缺 python 时不能还印"端到端已验"。
    //   声明与事实不一致，正是本模块最要防的那种失败。
    if (tls::schannel_compiled_in()) {
        if (g_skip == 0) {
            std::printf("\nTLS 状态：SChannel（Windows 系统组件）—— 真 TLS，端到端已验，"
                        "证书校验默认开\n");
        } else {
            std::printf("\nTLS 状态：SChannel 已编译进，但**本次端到端未验证**"
                        "（缺 python，SKIPPED=%d）；证书校验默认开这一条只由 T82 静态钉住\n",
                        g_skip);
        }
    } else {
        std::printf("\nTLS 状态：本平台无后端 —— connect() 显式拒绝，绝不退化成明文\n");
    }

    std::printf("\n");
    if (g_fail == 0) std::printf("ALL TESTS PASSED\n");
    std::printf("PASS=%d FAIL=%d SKIPPED=%d\n", g_pass, g_fail, g_skip);
    return g_fail == 0 ? 0 : 1;
}
