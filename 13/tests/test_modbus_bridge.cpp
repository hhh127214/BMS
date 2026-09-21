// =====================================================================
// 13/ 跨语言联调测试 —— C++ 主站 ↔ Python pymodbus 从站
//
// 为什么必须单独有这一层（13/tests/test_modbus_tcp.cpp 已经测过协议了）：
//
//   ★ **自己写两端等于自己和自己对答案。**
//     `fake_modbus_slave.h` 和 `modbus_tcp_client.h` 都是我们写的，
//     双方共同误解协议时谁都发现不了（比如都把字序记成 AB —— 那测试全绿，
//     现场接真机全错）。pymodbus 是业界事实标准，拿它当对端，
//     "我们的客户端符合协议"才第一次成为一个**有外部证据**的结论。
//
//   这一层立刻抓出了三个真问题（两个在 Python 侧、一个在 C++ 侧），
//   详见 13/sim/modbus_slave.py 文件末尾与 13/docs/README.md。
//
// ---------------------------------------------------------------------
// 测试项
// ---------------------------------------------------------------------
//   T40 点表一致性：C++ kBindings vs Python point_table() 逐字段比对
//       ★ 这是最值钱的一条：两边都是手工维护的，错一个地址的后果是
//         "值整体错一个寄存器"，而功率量级的数字彼此很像，肉眼抓不住。
//   T41 跨语言读量测：32 点全部读回，值与 Python 侧设定一致
//   T42 ★ 关口功率口径：--bias 40 下 p_grid 必须等于**电表读数**，
//       而不是主站在本地重算的 (load+standby-pv-p_bat)
//   T43 跨语言写指令：EMS 写 HR → Python 设备模型**消费**它
//       （用"实际功率向指令值演化"证明，而不是"读回自己写的值"）
//   T44 闭环收敛：连续 N 拍 write_command + read_snapshot，实际功率跟上
//   T45 BMS 安全位跨语言：--bms-dis-forbid 起从站 → read_limits 读到 true
//   T46 从站不可达：连一个没人监听的端口必须**快速失败**，不能挂住
//
// ---------------------------------------------------------------------
// 环境依赖与 skip 语义（重要）
// ---------------------------------------------------------------------
// 本测试需要外部 Python + pymodbus。**环境缺失时 skip 而非 fail** ——
// 环境问题不是代码缺陷。但必须**显式打印 SKIP 与原因**：
// 静默跳过和真的跑过了在输出上不可区分，那等于把这一层测试悄悄删掉。
//
// 区分三种结果（这个区分本身有价值）：
//   · 找不到 python          → SKIP（环境缺）
//   · python 在但 --dump-tsv 失败 → FAIL（脚本坏了，不是环境问题）
//   · 服务起不来 / 连不上     → FAIL
//
// python 解释器定位顺序：环境变量 EMS_PYTHON → PATH 上的 python。
// 构建脚本（13/scripts/build_test_bridge.bat）负责设 EMS_PYTHON。
//
// 编译：见 13/scripts/build.bat
// =====================================================================

#include "modbus_device_io.h"
#include "modbus_point_map.h"
#include "modbus_tcp_client.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#if !defined(_WIN32)
#  include <sys/stat.h>
#  include <sys/types.h>
#endif
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
// ★ 顺序不能反：`winsock2.h` 必须在 `windows.h` **之前**。
//   反了的话 windows.h 会先引入 winsock.h 的旧定义，之后 winsock2.h 被
//   保护宏挡掉 —— 表现就是 `inet_pton`/`SOCKET` 等一大批符号"未声明"，
//   而编译器的建议（改成 inet_ntoa）完全是误导。
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <process.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <signal.h>
#endif

using namespace ems;
using namespace ems::modbus;

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

#define EXPECT_NEAR(a, b, eps)                                            \
    do {                                                                  \
        double va = (a), vb = (b);                                        \
        if (std::fabs(va - vb) <= (eps)) {                                \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va << " vs " << #b << "="  \
                      << vb << " (eps " << (eps) << ")" << std::endl;     \
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

// =====================================================================
// 子进程管理
// =====================================================================

namespace {

constexpr std::uint16_t kBridgePort = 15031;

std::string g_python;      // 解析出的 python 解释器（为空 = 环境缺）
std::string g_script;      // 从站脚本路径

// ---- 解释器定位 -----------------------------------------------------
// ★ 只在**一个**函数里决定"用哪个 python"，其余地方不再关心。
//   现场/CI 用 EMS_PYTHON 覆盖，本地开发走 PATH。
std::string which_python() {
    const char* env = std::getenv("EMS_PYTHON");
    if (env && *env) {
        // 显式指定的路径**不做存在性探测**：如果指定了却用不了，
        // 应该报错让人知道，而不是悄悄回退到 PATH 上的另一个解释器
        // （那个解释器很可能没装 pymodbus，于是表现为"服务起不来"，
        //   排查方向就全歪了）。
        return env;
    }
#if defined(_WIN32)
    return "python.exe";
#else
    return "python3";
#endif
}

// 从站脚本路径：13/sim/modbus_slave.py。测试从 13/ 目录跑。
std::string slave_script_path() {
    const char* env = std::getenv("EMS_SLAVE_SCRIPT");
    if (env && *env) return env;
    return "sim/modbus_slave.py";
}

// ---- 子进程：**不走 shell** ------------------------------------------
//
// ★★ 为什么不 `_popen` / `system` / `cmd /c start`（这是本文件踩过的最大的坑）：
//   它们都要经过 `cmd.exe`。本机 `cmd.exe` 被安全策略禁用 —— 而失败**极其隐蔽**：
//   `_popen` 不返回 null，而是返回一段 **GBK 编码的 "'' 不是内部或外部命令"**。
//   后果有两层：
//     ① 探活永远失败 → 全部用例 SKIP（看起来像"环境没装"，实际是机制错）；
//     ② 那段 GBK 字节混进 stdout 后，**整条输出流被解码器判定为非法 UTF-8**，
//        于是连本程序自己打印的 UTF-8 中文也一起变成乱码 ——
//        排查时会把注意力全引到"编码问题"上，而真因是子进程起不来。
//   所以：**直接用 `CreateProcess`（Windows）/ `fork+exec`（POSIX）**，
//   传 argv 数组而不是命令行字符串。附带好处是彻底没有 shell 引号转义问题。
struct ProcResult {
    int exit_code = -1;
    std::string output;
    bool started = false;
};

// 把 argv 拼成 CreateProcess 需要的一整行（按 Windows 规则加引号）
std::string join_cmdline(const std::vector<std::string>& argv) {
    std::string s;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) s += ' ';
        s += '"';
        s += argv[i];
        s += '"';
    }
    return s;
}

#if defined(_WIN32)
// 读回一个小文件（子进程输出的落点）
std::string read_file_bytes(const std::string& path, std::size_t max_bytes) {
    std::string out;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    char buf[1024];
    DWORD got = 0;
    while (out.size() < max_bytes && ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) {
        out.append(buf, got);
    }
    CloseHandle(h);
    return out;
}

std::string temp_path(const char* tag) {
    char dir[MAX_PATH] = {0};
    DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0) return std::string(tag);
    std::string p(dir);
    p += "ems13_";
    p += tag;
    return p;
}
#endif

// 同步跑一个子进程，捕获 stdout+stderr（用于 `--dump-tsv` 这类一次性调用）
ProcResult run_capture(const std::vector<std::string>& argv, int timeout_ms = 60000) {
    ProcResult r;
#if defined(_WIN32)
    const std::string out_path = temp_path("capture.txt");
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hout = CreateFileA(out_path.c_str(), GENERIC_WRITE | GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hout == INVALID_HANDLE_VALUE) return r;

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hout;
    si.hStdError = hout;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::string cmd = join_cmdline(argv);
    std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back('\0');

    const BOOL ok = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr,
                                   TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hout);
    if (!ok) return r;
    r.started = true;

    const DWORD w = WaitForSingleObject(pi.hProcess, (DWORD)timeout_ms);
    DWORD code = 0;
    if (w == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 4242);
        code = 4242;
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    r.exit_code = (int)code;
    r.output = read_file_bytes(out_path, 65536);
    DeleteFileA(out_path.c_str());
#else
    int fds[2];
    if (pipe(fds) != 0) return r;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        std::vector<char*> c;
        for (const auto& a : argv) c.push_back(const_cast<char*>(a.c_str()));
        c.push_back(nullptr);
        execvp(c[0], c.data());
        _exit(127);
    }
    close(fds[1]);
    char buf[1024];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
        r.output.append(buf, (std::size_t)n);
        if (r.output.size() > 65536) break;
    }
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    r.exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    r.started = true;
#endif
    return r;
}

// ---- 端口探测 --------------------------------------------------------
// ★ 用"端口能不能连上"当就绪信号，而不是解析子进程日志：
//   日志格式会变，端口通了才是真的通了。
bool port_listening(const char* host, std::uint16_t port, int timeout_ms) {
    detail::WinsockGuard guard;
    if (!detail::net_ready()) return false;
    detail::socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!detail::socket_valid(s)) return false;

    struct sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    // 用 inet_addr 而不是 inet_pton：MinGW 的 ws2tcpip.h 里 inet_pton 的
    // 可见性受 _WIN32_WINNT 门控，本机 toolchain 上它并不总是声明。
    // 本测试只连 127.0.0.1，inet_addr 足够且处处可用。
    a.sin_addr.s_addr = ::inet_addr(host);
    if (a.sin_addr.s_addr == INADDR_NONE) {
        detail::close_socket(s);
        return false;
    }
    const bool ok = detail::connect_with_timeout(
        s, reinterpret_cast<const sockaddr*>(&a), sizeof(a), timeout_ms);
    detail::close_socket(s);
    return ok;
}

bool wait_port(const char* host, std::uint16_t port, int timeout_ms) {
    const auto t0 = std::chrono::steady_clock::now();
    while (true) {
        if (port_listening(host, port, 200)) return true;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (ms > timeout_ms) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// ---- Python 从站子进程 ----------------------------------------------

// ★ 运行期产物一律落在 build\ 下，**不落模块根**。
//   为什么：`build_test.bat` 的 CWD 是模块根，相对路径 `bridge_slave.log`
//   会在 `13\` 根目录攒下运行期垃圾（第一次跑就留了个 0 字节的）。而 `13\`
//   根目录是**源码目录**，源码目录里出现运行期文件，会让"这次改了什么"
//   越来越难看清。默认取 `build\`，若不存在（直接跑 exe 的场景）退回当前目录。
//   可用 EMS_BRIDGE_LOG / EMS_BRIDGE_STATUS 覆盖。
std::string build_rel(const char* name) {
    const std::string p = std::string("build/") + name;
#if defined(_WIN32)
    if (GetFileAttributesA("build") != INVALID_FILE_ATTRIBUTES) return p;
#else
    struct stat st;
    if (::stat("build", &st) == 0 && S_ISDIR(st.st_mode)) return p;
#endif
    return name;
}
std::string slave_log_path() {
    static const std::string p = [] {
        const char* e = std::getenv("EMS_BRIDGE_LOG");
        return (e && *e) ? std::string(e) : build_rel("bridge_slave.log");
    }();
    return p;
}
// 机器可读的状态行文件：**给 build_test.bat 判断"第三层到底跑了没有"用**。
// ★ 为什么需要它：`SKIPPED=7` 是中文上下文里的一行文本，.bat 里 grep 它要处理
//   编码；而这个文件是纯 ASCII 的键值行，`findstr /C:"SKIPPED=0"` 稳。
// ★ 为什么必须让"第三层没跑"在 .bat 里可见：SKIP 是**有意的**（环境问题不是
//   代码缺陷），但如果构建脚本仍然印「三层测试全部通过」，那么「静默跳过」
//   与「真的跑过」在最终输出上**仍然不可区分** —— 这正是本项目最忌讳的一类失败。
std::string bridge_status_path() {
    static const std::string p = [] {
        const char* e = std::getenv("EMS_BRIDGE_STATUS");
        return (e && *e) ? std::string(e) : build_rel("bridge_status.txt");
    }();
    return p;
}
void write_bridge_status(int pass, int fail, int skipped) {
    if (FILE* f = std::fopen(bridge_status_path().c_str(), "wb")) {
        std::fprintf(f, "PASS=%d FAIL=%d SKIPPED=%d\n", pass, fail, skipped);
        std::fclose(f);
    }
}

class PythonSlave {
public:
    // 参数直接拼给 modbus_slave.py（argv 风格，便于测试表达"现场工况"）
    bool start(const std::string& extra_args, int ready_timeout_ms = 10000) {
        std::vector<std::string> argv{g_python, g_script,
                                      "--port", std::to_string(kBridgePort)};
        // 把 extra_args 按空格切开（测试只传简单开关，不需要引号处理）
        {
            std::string cur;
            for (char c : extra_args) {
                if (c == ' ') { if (!cur.empty()) { argv.push_back(cur); cur.clear(); } }
                else cur += c;
            }
            if (!cur.empty()) argv.push_back(cur);
        }

#if defined(_WIN32)
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        // ★ 输出落盘而不是丢弃：Python 侧的报错（缺 pymodbus、端口占用）
        //   只在 stderr 上。不落盘的话测试只会说"起不来"，而**为什么**
        //   起不来没有任何证据 —— 那等于把最贵的信息扔掉。
        HANDLE hout = CreateFileA(slave_log_path().c_str(), GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hout == INVALID_HANDLE_VALUE) return false;

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hout;
        si.hStdError = hout;
        si.hStdInput = nullptr;

        PROCESS_INFORMATION pi{};
        std::string cmd = join_cmdline(argv);
        std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
        mutable_cmd.push_back('\0');
        const BOOL ok = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr,
                                       TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                       &si, &pi);
        CloseHandle(hout);
        if (!ok) return false;
        CloseHandle(pi.hThread);
        hproc_ = pi.hProcess;
        pid_ = (long)pi.dwProcessId;
#else
        const pid_t p = fork();
        if (p == 0) {
            FILE* f = std::freopen(slave_log_path().c_str(), "w", stdout);
            (void)f;
            std::freopen(slave_log_path().c_str(), "a", stderr);
            std::vector<char*> c;
            for (const auto& a : argv) c.push_back(const_cast<char*>(a.c_str()));
            c.push_back(nullptr);
            execvp(c[0], c.data());
            _exit(127);
        }
        if (p < 0) return false;
        pid_ = (long)p;
#endif
        started_ = true;
        ready_ = wait_port(host(), kBridgePort, ready_timeout_ms);
        return ready_;
    }

    void stop() {
        if (!started_) return;
#if defined(_WIN32)
        if (hproc_) {
            // ★ 用 TerminateProcess 而不是 `taskkill`：taskkill 又要经过 shell，
            //   而本机 shell 不可用（见 run_capture 的说明）。
            TerminateProcess(static_cast<HANDLE>(hproc_), 0);
            WaitForSingleObject(static_cast<HANDLE>(hproc_), 3000);
            CloseHandle(static_cast<HANDLE>(hproc_));
            hproc_ = nullptr;
        }
#else
        ::kill((pid_t)pid_, SIGTERM);
        int st = 0;
        ::waitpid((pid_t)pid_, &st, 0);
#endif
        started_ = false;
        ready_ = false;
        // 等端口真的释放，否则下一个用例 bind 失败
        for (int i = 0; i < 40 && port_listening(host(), kBridgePort, 100); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    ~PythonSlave() { stop(); }

    bool ready() const { return ready_; }
    const char* host() const { return "127.0.0.1"; }

    std::string log_tail() const {
#if defined(_WIN32)
        const std::string s = read_file_bytes(slave_log_path().c_str(), 65536);
        return s.size() > 2500 ? s.substr(s.size() - 2500) : s;
#else
        std::string s;
        if (FILE* f = std::fopen(slave_log_path().c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END);
            long n = std::ftell(f);
            long from = n > 2500 ? n - 2500 : 0;
            std::fseek(f, from, SEEK_SET);
            char buf[512];
            std::size_t r;
            while ((r = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, r);
            std::fclose(f);
        }
        return s;
#endif
    }

private:
    long pid_ = -1;
    void* hproc_ = nullptr;
    bool started_ = false;
    bool ready_ = false;
};

// ---- 环境探测 --------------------------------------------------------
//
// ★ 两级探测，因为两种"环境缺失"的处置完全不同：
//
//   第 1 级 `--dump-tsv`：**纯点表打印，不 import pymodbus**（脚本里是惰性
//     导入）。它证明"python 与脚本都在"。失败 → 没有 python / 路径不对。
//
//   第 2 级 `--check-pymodbus`：显式问"服务起得来吗"。
//     ★ 为什么必须有第 2 级：`--dump-tsv` 不需要 pymodbus，所以
//       "有 python 但没装 pymodbus"会**通过第 1 级**，然后在起服务时失败 ——
//       被报成**代码缺陷**（FAIL），而它其实是环境缺失（该 SKIP）。
//       这个区分不做，构建脚本就会在没装 pymodbus 的机器上永久假红。
//
// 三级结论：
//   ① 第 1 级失败                  → SKIP（没有 python / 脚本路径不对）
//   ② 第 1 级过、第 2 级失败        → SKIP（环境缺 pymodbus，并打印安装指引）
//   ③ 两级都过                     → 正常跑，任何失败都是真 FAIL
static bool g_env_python_ok = false;     // 第 1 级
static bool g_env_pymodbus_ok = false;   // 第 2 级

bool ensure_env() {
    static int cached = -1;      // -1 未判定 / 0 缺 / 1 有
    if (cached >= 0) return cached == 1;

    // ---- 第 1 级：python + 脚本 ----
    const ProcResult r = run_capture({g_python, g_script, "--dump-tsv"}, 20000);
    if (!r.started || r.output.find("MEAS.P_LOAD") == std::string::npos) {
        std::printf("  [SKIP] 找不到可用的 python 或从站脚本\n"
                    "         python  = %s\n"
                    "         脚本    = %s\n"
                    "         启动    = %s  退出码 = %d\n"
                    "         提示：设 EMS_PYTHON=<python.exe> 指向隔离环境；\n"
                    "               设 EMS_SLAVE_SCRIPT=<path> 覆盖脚本路径\n"
                    "         输出    : %s\n",
                    g_python.c_str(), g_script.c_str(),
                    r.started ? "成功" : "失败", r.exit_code,
                    r.output.empty() ? "(空)" : r.output.substr(0, 300).c_str());
        cached = 0;
        return false;
    }
    g_env_python_ok = true;

    // ---- 第 2 级：pymodbus ----
    const ProcResult p = run_capture({g_python, g_script, "--check-pymodbus"}, 20000);
    g_env_pymodbus_ok = (p.started && p.exit_code == 0);
    if (!g_env_pymodbus_ok) {
        std::printf("  [SKIP] python 与脚本可用，但**没有 pymodbus** —— 本层测试跳过\n"
                    "         python = %s\n"
                    "         探测    : %s\n"
                    "         安装    : python -m venv .venv\n"
                    "                   .venv\\Scripts\\pip install -r 13\\sim\\requirements.txt\n"
                    "                   然后 set EMS_PYTHON=<...>\\.venv\\Scripts\\python.exe\n"
                    "         ★ 这是**环境缺失**，不是代码缺陷 —— 但必须在报告里出现，\n"
                    "           否则「静默跳过」与「真的跑过」在输出上不可区分。\n",
                    g_python.c_str(),
                    p.output.empty() ? "(无输出)" : p.output.substr(0, 200).c_str());
        cached = 0;
        return false;
    }

    cached = 1;
    return true;
}

// 起一个从站；失败时打印日志尾部（**证据**），返回 false
bool up(PythonSlave& s, const std::string& args) {
    if (!s.start(args)) {
        std::printf("  [FAIL] Python 从站未能就绪（两级环境探测已通过，"
                    "所以这是**真失败**）\n");
        const std::string t = s.log_tail();
        std::printf("---------- %s (tail) ----------\n%s\n"
                    "-------------------------------------------\n",
                    slave_log_path().c_str(), t.c_str());
        return false;
    }
    return true;
}

} // namespace

// =====================================================================
// T40 点表一致性 —— C++ 与 Python 逐字段比对
// =====================================================================
static const char* table_name(Table t) {
    switch (t) {
        case Table::kInputReg:      return "IR";
        case Table::kHoldingReg:    return "HR";
        case Table::kDiscreteInput: return "DI";
        case Table::kCoil:          return "CO";
    }
    return "??";
}

static const char* encoding_name(Encoding e) {
    switch (e) {
        case Encoding::kF32: return "f32";
        case Encoding::kU16: return "u16";
        case Encoding::kI16: return "i16";
        case Encoding::kBit: return "bit";
    }
    return "??";
}

std::string binding_tsv() {
    std::string out;
    for (std::size_t i = 0; i < kBindingCount; ++i) {
        const PointBinding& b = kBindings[i];
        char line[256];
        const char* wo = (b.encoding == Encoding::kF32)
                             ? (b.word_order == WordOrder::kLowWordFirst ? "BA" : "AB")
                             : "-";
        char scale[32];
        std::snprintf(scale, sizeof(scale), "%g", b.scale);
        std::snprintf(line, sizeof(line), "%d\t%s\t%s\t%u\t%s\t%s\t%s\t%d",
                      static_cast<int>(i), b.name, table_name(b.table),
                      static_cast<unsigned>(b.address), encoding_name(b.encoding),
                      wo, scale, b.writable ? 1 : 0);
        out += line;
        out += "\n";
    }
    return out;
}

static void test_40_point_table_parity() {
    std::printf("T40 点表一致性（C++ kBindings vs Python point_table）\n");
    if (!ensure_env()) { ++g_skip; return; }

    const ProcResult r = run_capture({g_python, g_script, "--dump-tsv"}, 20000);
    EXPECT(r.started);
    EXPECT_EQ(r.exit_code, 0);
    // ★ 反向守卫：如果 Python 侧输出**不含任何点**，下面的逐行比对会因为
    //   "两边都为空"而全部通过 —— 那是最典型的假绿。
    EXPECT(r.output.find("MEAS.P_LOAD") != std::string::npos);
    const std::string py = r.output;

    const std::string cpp = binding_tsv();

    // 逐行比对，**报第一个不一致的字段**（整体 diff 说不清哪一项错）
    std::vector<std::string> pl, cl;
    {
        std::string cur;
        for (char c : py) { if (c == '\n') { if (!cur.empty()) pl.push_back(cur); cur.clear(); } else if (c != '\r') cur += c; }
        if (!cur.empty()) pl.push_back(cur);
    }
    {
        std::string cur;
        for (char c : cpp) { if (c == '\n') { if (!cur.empty()) cl.push_back(cur); cur.clear(); } else if (c != '\r') cur += c; }
        if (!cur.empty()) cl.push_back(cur);
    }

    EXPECT_EQ(pl.size(), cl.size());
    const std::size_t n = pl.size() < cl.size() ? pl.size() : cl.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (pl[i] == cl[i]) {
            ++g_pass;
            continue;
        }
        ++g_fail;
        std::cerr << "  FAIL  点表第 " << i << " 行不一致\n"
                  << "        C++    : " << cl[i] << "\n"
                  << "        Python : " << pl[i] << std::endl;
    }
    EXPECT_EQ(n, static_cast<std::size_t>(kBindingCount));
}

// ---- 建一个连到 Python 从站的适配器 ---------------------------------
struct BridgeFixture {
    PythonSlave* slave = nullptr;
    std::unique_ptr<ModbusDeviceIO> io;

    bool up_with(const std::string& args, int timeout_ms = 1500) {
        if (!up(*slave, args)) return false;
        ModbusDeviceIO::Config cfg;
        cfg.host = slave->host();
        cfg.port = kBridgePort;
        cfg.unit_id = 1;
        cfg.timeout_ms = timeout_ms;
        io.reset(new ModbusDeviceIO(cfg));
        return io->connect();
    }
};

// =====================================================================
// T41 跨语言读量测
// =====================================================================
static void test_41_read_measurements() {
    std::printf("T41 跨语言读量测（32 点全读回）\n");
    if (!ensure_env()) { ++g_skip; return; }
    PythonSlave s;
    BridgeFixture f;
    f.slave = &s;
    if (!f.up_with("--load 380 --pv 150 --tau-s 0.3 --ramp-kw-s 400")) {
        EXPECT(false); ++g_skip; return;
    }

    RealtimeSnapshot snap;
    EXPECT(f.io->read_snapshot(1000.0, snap));

    // Python 侧设定：load=380, pv=150, standby=2, soc0=0.55, soh=0.98, T=25
    EXPECT_NEAR(snap.p_load_kw, 380.0 + 2.0, 0.5);   // 站用电计入负荷侧
    EXPECT_NEAR(snap.p_pv_kw, 150.0, 0.5);
    EXPECT_NEAR(snap.soc, 0.55, 0.01);
    EXPECT_NEAR(snap.soh, 0.98, 0.01);
    EXPECT_NEAR(snap.temperature_c, 25.0, 0.5);

    // ★ 32 个点**全部**可信（不是"读到了就算"：Modbus 没有品质位，
    //   可信性由主站合成 —— 这里断言合成结果确实为真）
    EXPECT_EQ(f.io->stale_points(), 0);
    EXPECT(f.io->read_status().data_valid);
    // 3 次分块请求的契约跨语言也成立
    EXPECT_EQ(f.io->client().requests(), 3);
    EXPECT_EQ(f.io->partial_scans(), 0);
    EXPECT_EQ(f.io->block_fails(), 0);
    EXPECT_EQ(f.io->decode_fails(), 0);
}

// =====================================================================
// T42 ★ 关口功率口径（A2 缺口的跨语言验证）
//
// 这是本文件最重要的一条。Python 侧的电表读数 = 平衡值 + bias(40)。
// 主站若在本地重算 (load+standby-pv-p_bat)，会得到比电表**小 40** 的值。
// 两个写法在 bias=0 时**逐位相等** —— 所以必须用非 0 bias 把两者拉开。
// =====================================================================
static void test_42_grid_meter_scope() {
    std::printf("T42 关口功率口径（--bias 40：读电表 vs 本地重算）\n");
    if (!ensure_env()) { ++g_skip; return; }
    PythonSlave s;
    BridgeFixture f;
    f.slave = &s;
    if (!f.up_with("--load 380 --pv 150 --bias 40 --tau-s 0.3 --ramp-kw-s 400")) {
        EXPECT(false); ++g_skip; return;
    }

    RealtimeSnapshot snap;
    EXPECT(f.io->read_snapshot(1000.0, snap));

    // 电表读数 = 380 + 2 - 150 - p_bat + 40；刚起来 p_bat≈0
    EXPECT_NEAR(snap.p_grid_kw, 272.0, 1.0);

    // ★ 反向守卫：证明"本地重算"这条错路**确实会给出不同的值**。
    //   没有这一条，上面那条断言在"两个口径碰巧相等"时也会过。
    const double recomputed =
        snap.p_load_kw - snap.p_pv_kw - snap.p_bat_actual_kw;
    EXPECT(std::fabs(snap.p_grid_kw - recomputed) > 30.0);
    EXPECT_NEAR(snap.p_grid_kw - recomputed, 40.0, 1.0);

    // read_actuals 与 read_snapshot 必须同源（同一物理量的两个出口）
    const DeviceActuals a = f.io->read_actuals();
    EXPECT_NEAR(a.p_grid_kw, snap.p_grid_kw, 1e-9);
}

// =====================================================================
// T43 跨语言写指令 —— 验证 Python 设备模型真的**消费**了它
//
// ★ 关键：不能只"读回自己写的值"来证明通路。读回自己写的值只证明
//   寄存器可写，**不证明对端把它当指令用了**。
//   这里用"实际功率向指令值演化"当证据 —— 那是 Python 侧模型的输出。
// =====================================================================
static void test_43_write_command_consumed() {
    std::printf("T43 跨语言写指令（Python 设备模型消费指令）\n");
    if (!ensure_env()) { ++g_skip; return; }
    PythonSlave s;
    BridgeFixture f;
    f.slave = &s;
    if (!f.up_with("--load 380 --pv 150 --tau-s 0.3 --ramp-kw-s 400")) {
        EXPECT(false); ++g_skip; return;
    }

    RealtimeSnapshot snap;
    EXPECT(f.io->read_snapshot(1000.0, snap));
    const double p_bat_before = snap.p_bat_actual_kw;
    EXPECT(std::fabs(p_bat_before) < 5.0);     // 初始 ≈ 0

    // 下发"放电 100 kW"，权限区间给足
    PowerCommand cmd;
    cmd.p_bat_cmd_kw = 100.0;
    cmd.p_upper = 150.0;
    cmd.p_lower = -150.0;
    EXPECT(f.io->write_command(cmd));
    EXPECT_EQ(f.io->writes(), 1);              // 一次事务，原子
    EXPECT_EQ(f.io->write_failures(), 0);

    // 等模型追上（tau=0.3s，约 1.5s 后应接近指令）
    double p_bat_after = 0.0;
    for (int i = 0; i < 60; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!f.io->read_snapshot(1000.0 + i * 0.05, snap)) continue;
        p_bat_after = snap.p_bat_actual_kw;
        if (p_bat_after > 90.0) break;
    }
    // ★ 实际功率**变了**（证明指令被消费）
    EXPECT(p_bat_after > 80.0);
    EXPECT_NEAR(p_bat_after, 100.0, 12.0);

    // ★ 反向守卫：指令没被消费的实现会让 p_bat 停在 0 附近
    EXPECT(std::fabs(p_bat_after - p_bat_before) > 60.0);
}

// =====================================================================
// T44 闭环收敛：连续多拍 write_command + read_snapshot
// =====================================================================
static void test_44_closed_loop() {
    std::printf("T44 跨语言闭环（多拍指令 + 量测）\n");
    if (!ensure_env()) { ++g_skip; return; }
    PythonSlave s;
    BridgeFixture f;
    f.slave = &s;
    if (!f.up_with("--load 380 --pv 150 --bias 20 --tau-s 0.2 --ramp-kw-s 800")) {
        EXPECT(false); ++g_skip; return;
    }

    RealtimeSnapshot snap;
    if (!f.io->read_snapshot(0.0, snap)) { EXPECT(false); ++g_skip; return; }

    int ok_ticks = 0, cmd_bad = 0;
    double last_p = 0.0;
    for (int t = 0; t < 30; ++t) {
        // 目标：把电池功率推到一个变化的目标上（模拟调度给出新目标）
        const double target = (t < 15) ? 60.0 : 20.0;
        PowerCommand cmd;
        cmd.p_bat_cmd_kw = target;
        cmd.p_upper = 150.0;
        cmd.p_lower = -150.0;
        if (!f.io->write_command(cmd)) ++cmd_bad;

        if (!f.io->read_snapshot(t * 0.1, snap)) continue;
        ++ok_ticks;
        last_p = snap.p_bat_actual_kw;

        // 每一拍都要求：值可信、无分块失败、无解码失败
        if (f.io->stale_points() != 0) ++cmd_bad;
        if (f.io->decode_fails() != 0) ++cmd_bad;

        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    EXPECT(ok_ticks >= 28);            // 30 拍里至少 28 拍读成功
    EXPECT_EQ(cmd_bad, 0);             // 没有任何一拍出现坏点或解码错
    // 目标降到 20 后，最后一拍应明显低于峰值段
    EXPECT(last_p < 45.0);
    EXPECT(last_p > 0.0);
    EXPECT_EQ(f.io->read_status().data_valid, true);
}

// =====================================================================
// T45 BMS 安全位跨语言
//
// ★ 这条走的是完整的**安全链路**：
//     Python 从站（BMS 网关替身）→ DI 位 → ModbusDeviceIO::read_limits
//     → DeviceLimits{bms_dis_forbidden} → 04/S01(kBmsForbid, L0) → 05/
//   A1 的教训是这个通路"三层测试全绿也照不出来"（仿真适配器的
//   DeviceLimits 由调用方注入，"设备侧真读"那条路一次都没被走过）。
//   这里是**跨了内存边界**的真读。
// =====================================================================
static void test_45_bms_forbid_bridge() {
    std::printf("T45 BMS 禁充放安全位（跨语言通路）\n");
    if (!ensure_env()) { ++g_skip; return; }
    PythonSlave s;
    BridgeFixture f;
    f.slave = &s;
    if (!f.up_with("--load 380 --pv 150 --bms-dis-forbid --bms-chg-forbid")) {
        EXPECT(false); ++g_skip; return;
    }

    DeviceLimits lim;
    EXPECT(f.io->read_limits(lim));
    // Python 侧命令行**显式模拟**了 BMS 上报两个禁字
    EXPECT(lim.bms_dis_forbidden);
    EXPECT(lim.bms_chg_forbidden);
    // 配置也从设备读回来（不是装配期注入）
    EXPECT_NEAR(f.io->battery_capacity_kwh(), 1000.0, 1.0);
    EXPECT_NEAR(lim.pcs_rated_chg_kw, 200.0, 1.0);
    EXPECT_NEAR(lim.pcs_rated_dis_kw, 200.0, 1.0);
    EXPECT_NEAR(lim.transformer_capacity_kw, 250.0, 1.0);
    // 能力开关：Modbus 路线的限值是**运行期可变**的
    EXPECT(f.io->limits_are_live());
}

static void test_46_bms_forbid_clear() {
    std::printf("T46 BMS 禁充放位可恢复（不是锁存）\n");
    if (!ensure_env()) { ++g_skip; return; }
    PythonSlave s;
    BridgeFixture f;
    f.slave = &s;
    if (!f.up_with("--load 380 --pv 150")) {   // 不带任何 --bms-*-forbid
        EXPECT(false); ++g_skip; return;
    }
    DeviceLimits lim;
    EXPECT(f.io->read_limits(lim));
    EXPECT(!lim.bms_dis_forbidden);
    EXPECT(!lim.bms_chg_forbidden);
}

// =====================================================================
// T47 从站不可达必须**快速失败**
//
// ★ connect() 在设备离线时若走阻塞路径，会等 OS 的 SYN 重传超时
//   （Windows 实测 ~20 s），而控制周期只有 200 ms。
//   现场表现是"程序偶尔卡死"，最难归因。
// =====================================================================
static void test_47_unreachable_fast_fail() {
    std::printf("T47 从站不可达：快速失败（不挂住）\n");
    ModbusDeviceIO::Config cfg;
    cfg.host = "127.0.0.1";
    // 一个**没人监听**的端口（高位随机，且我们不 bind）
    cfg.port = 15999;
    cfg.timeout_ms = 400;
    ModbusDeviceIO io(cfg);

    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = io.connect();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT(!ok);                  // 连不上
    EXPECT(ms < 3000);            // ★ 而且**快**（不是 20 秒）

    // 未连接时读快照必须立刻失败，且全部点标记不可信
    RealtimeSnapshot snap;
    EXPECT(!io.read_snapshot(0.0, snap));
    EXPECT_EQ(io.stale_points(), (int)kBindingCount);
    EXPECT(!io.read_status().data_valid);
}

// =====================================================================
int main() {
    std::printf("=== 13/ 跨语言联调（C++ 主站 ↔ Python pymodbus 从站） ===\n");
    std::printf("\n");

    g_python = which_python();
    g_script = slave_script_path();
    std::printf("python = %s\n脚本   = %s\n\n", g_python.c_str(), g_script.c_str());
    std::fflush(stdout);

    test_40_point_table_parity();
    test_41_read_measurements();
    test_42_grid_meter_scope();
    test_43_write_command_consumed();
    test_44_closed_loop();
    test_45_bms_forbid_bridge();
    test_46_bms_forbid_clear();
    test_47_unreachable_fast_fail();

    std::printf("\n");
    // ★ SKIPPED **总是**打印（哪怕是 0）。
    //   理由：`grep "SKIPPED=0"` 是**判定全量基线口径**的唯一判据。
    //   只有 skip 时才打印的话，"第三层确实跑了"在日志里**没有正面证据**，
    //   只能靠"没有那一行"去推断 —— 而"缺行"与"grep 写错 / 日志被截断"
    //   完全无法区分。**正面证据优于"没有反面证据"。**
    std::printf("SKIPPED=%d%s\n", g_skip,
                g_skip > 0 ? " （环境缺失，不是代码缺陷 —— 但必须在报告里显式出现）" : "");
    if (g_fail == 0) {
        std::printf("ALL TESTS PASSED\n");
    }
    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    // 纯 ASCII 状态行，给 build_test.bat 判断"本层到底跑了没有"用（见函数注释）
    write_bridge_status(g_pass, g_fail, g_skip);
    return g_fail == 0 ? 0 : 1;
}
