// =====================================================================
// 21/ — DL/T645-2007 主站 CLI
//
// 三种链路，一套代码：
//   --sim                内存回环 + 进程内假表（**零硬件，可自测**）
//   --tcp HOST:PORT      串口服务器透传
//   --serial COM3        本机串口（485/232）
//
// 用法示例：
//   meter645.exe --sim --count 3
//   meter645.exe --tcp 192.168.1.50:8899 --addr 000000000001 --count 10
//   meter645.exe --serial COM3 --baud 2400 --parity even --once --di 02010100
//
// ---------------------------------------------------------------------
// ★ 为什么先把 --sim 做出来，而不是先做串口
// ---------------------------------------------------------------------
// 串口这条路在本机**无法验证**（没有 485 转换器、没有真表）。如果先写它，
// 交付时能拿出的证据只有"编译通过"，而编译通过证明不了任何协议行为。
// --sim 让"组帧 / 解析 / 超时重试 / BCD 解码 / 统计口径"这五件事先有
// 真实运行证据；串口与 TCP 只是把字节搬过去的方式，不改变这些判据。
//
// 这也是本项目一贯的次序：**先让可验证的部分有证据，再谈不可验证的部分。**
// =====================================================================

#include "dlt645_codec.h"
#include "dlt645_frame.h"
#include "dlt645_master.h"
#include "dlt645_transport.h"
#include "fake_meter_slave.h"

#ifdef _WIN32
#  include "dlt645_serial_link.h"
#  include "dlt645_tcp_link.h"
#endif

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace ems::dlt645;

namespace {

struct Args {
    bool sim = false;
    std::string tcp;
    std::string serial;
    unsigned int baud = 2400;
    bool even_parity = true;
    std::string addr = "000000000001";
    std::vector<std::uint32_t> dis;
    int count = 1;
    int interval_ms = 200;
    int timeout_ms = 500;
    int retries = 2;
};

void usage() {
    std::printf(
        "用法: meter645.exe [选项]\n"
        "  --sim                 内存回环 + 进程内假表（零硬件）\n"
        "  --tcp HOST:PORT       串口服务器透传\n"
        "  --serial COM3         本机串口\n"
        "  --baud N              波特率（默认 2400）\n"
        "  --parity even|none    校验位（默认 even）\n"
        "  --addr 12位           表地址（默认 000000000001）\n"
        "  --di LIST             逗号分隔的 DI（十六进制，默认常用 4 点）\n"
        "  --count N             轮询轮数（默认 1）\n"
        "  --interval MS         轮间隔（默认 200）\n"
        "  --timeout MS          单次应答超时（默认 500）\n"
        "  --retries N           重试次数（默认 2）\n");
}

bool parse_hex_di(const std::string& s, std::uint32_t* out) {
    if (s.size() != 8) return false;
    std::uint32_t v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | static_cast<std::uint32_t>(d);
    }
    *out = v;
    return true;
}

// ---------------------------------------------------------------------
// 仿真环境：内存回环 + 假表
// ---------------------------------------------------------------------
struct SimEnv {
    Address addr;
    BytePipe to_slave;
    BytePipe to_master;
    LoopbackLink link;
    test::FakeMeterSlave slave;

    explicit SimEnv(const Address& a)
        : link(&to_slave, &to_master), slave(&to_slave, &to_master, a) {
        link.open();
    }

    // 让读回来的数"会动"：按轮次做有界波动。
    // ★ 不用随机数：断言与人工复核都需要可复现，"同一个 tick 同一个值"。
    void tick(int n) {
        const double phase = static_cast<double>(n) * 0.7;
        slave.set_value(0x02010100u, 220.0 + 2.0 * std::sin(phase));
        slave.set_value(0x02010200u, 221.0 + 2.0 * std::sin(phase + 2.1));
        slave.set_value(0x02010300u, 219.5 + 2.0 * std::sin(phase + 4.2));
        slave.set_value(0x02020100u, 12.345 + 0.5 * std::sin(phase));
        // 功率含负值：充电为负、放电为正，两侧都要能读到。
        // ★ 幅度必须 < 9.9999 —— 3 字节 BCD 带符号位后只剩 5 个半字节，
        //   上限就是 99999（= 9.9999 kW 未乘变比）。这不是"随便取的小数"：
        //   工商业现场要靠 PT/CT 变比把它放大，见 docs/README.md「DI 表边界」。
        slave.set_value(0x02030000u, 5.0 * std::sin(phase * 0.5));
        slave.set_value(0x02800002u, 50.0 + 0.05 * std::sin(phase));
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto nxt = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::printf("[FAIL] %s 缺少参数值\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (k == "--sim") a.sim = true;
        else if (k == "--tcp") a.tcp = nxt("--tcp");
        else if (k == "--serial") a.serial = nxt("--serial");
        else if (k == "--baud") a.baud = static_cast<unsigned int>(std::atoi(nxt("--baud")));
        else if (k == "--parity") {
            const std::string v = nxt("--parity");
            if (v == "even") a.even_parity = true;
            else if (v == "none") a.even_parity = false;
            else { std::printf("[FAIL] --parity 只接受 even|none\n"); return 2; }
        }
        else if (k == "--addr") a.addr = nxt("--addr");
        else if (k == "--count") a.count = std::atoi(nxt("--count"));
        else if (k == "--interval") a.interval_ms = std::atoi(nxt("--interval"));
        else if (k == "--timeout") a.timeout_ms = std::atoi(nxt("--timeout"));
        else if (k == "--retries") a.retries = std::atoi(nxt("--retries"));
        else if (k == "--di") {
            const std::string v = nxt("--di");
            std::size_t p = 0;
            while (p <= v.size()) {
                const std::size_t q = v.find(',', p);
                const std::string item = v.substr(p, (q == std::string::npos ? v.size() : q) - p);
                std::uint32_t di = 0;
                if (!item.empty()) {
                    if (!parse_hex_di(item, &di)) {
                        std::printf("[FAIL] DI 必须是 8 位十六进制: %s\n", item.c_str());
                        return 2;
                    }
                    a.dis.push_back(di);
                }
                if (q == std::string::npos) break;
                p = q + 1;
            }
        }
        else if (k == "--help" || k == "-h") { usage(); return 0; }
        else { std::printf("[FAIL] 未知参数: %s\n", k.c_str()); usage(); return 2; }
    }

    // 默认点位：三相电压 + 总有功
    if (a.dis.empty()) {
        a.dis = {0x02010100u, 0x02010200u, 0x02010300u, 0x02030000u};
    }

    Address meter;
    if (!meter.from_string(a.addr)) {
        std::printf("[FAIL] 地址必须是 12 位十进制: %s\n", a.addr.c_str());
        return 2;
    }

    MasterOptions mo;
    mo.timeout_ms = a.timeout_ms;
    mo.retries = a.retries;

    // ---- 组装链路 ----
    // 用裸指针 + 显式 delete 而不是栈对象：链路类型在编译期是三种，
    // 栈上就得为每种都构造一个（未使用的也会分配句柄/套接字）。
    SimEnv* env = nullptr;
    IByteLink* link = nullptr;
    const char* link_name = "?";

    if (a.sim) {
        env = new SimEnv(meter);
        link = &env->link;
        link_name = "sim（内存回环 + 进程内假表）";
    }
#ifdef _WIN32
    else if (!a.tcp.empty()) {
        const std::size_t c = a.tcp.find(':');
        if (c == std::string::npos) {
            std::printf("[FAIL] --tcp 格式应为 HOST:PORT\n");
            return 2;
        }
        TcpLink* t = new TcpLink(a.tcp.substr(0, c),
                                 static_cast<unsigned short>(std::atoi(a.tcp.c_str() + c + 1)));
        link = t;
        link_name = "tcp（串口服务器透传）";
    } else if (!a.serial.empty()) {
        SerialConfig sc;
        sc.baud = a.baud;
        sc.even_parity = a.even_parity;
        SerialLink* s = new SerialLink(a.serial, sc);
        link = s;
        link_name = "serial";
    }
#endif
    else {
        std::printf("[FAIL] 必须指定 --sim / --tcp / --serial 之一\n");
        usage();
        return 2;
    }

    if (!link->open()) {
        std::printf("[FAIL] 链路打开失败\n");
        return 1;
    }

    Dlt645Master master(link, meter, mo);
    if (env) {
        master.set_pump([env] { env->slave.poll(); });
        env->tick(0);
    }

    std::printf("=== DL/T645-2007 主站 ===\n");
    std::printf("表地址 : %s\n", meter.to_string().c_str());
    std::printf("链路   : %s\n", link_name);
    std::printf("超时/重试: %d ms / %d 次\n", mo.timeout_ms, mo.retries);
    std::printf("\n");

    bool any_fail = false;
    for (int round = 0; round < a.count; ++round) {
        if (env) env->tick(round);
        std::printf("--- 第 %d 轮 ---\n", round + 1);
        std::printf("%-8s %-16s %10s %10s %-8s %s\n",
                    "DI", "名称", "读数", "工程量", "单位", "状态");
        for (std::uint32_t di : a.dis) {
            const DiSpec* s = find_builtin(di);
            const ReadResult r = master.read(di);
            const char* name = s ? s->name : "(未知)";
            const char* unit = s ? s->unit : "";
            if (r.st == ReadStatus::kOk) {
                std::printf("%08X %-16s %10.3f %10.3f %-8s %s",
                            di, name, r.raw, r.value, unit, read_status_text(r.st));
                if (r.attempts > 1) std::printf(" (第 %d 次)", r.attempts);
                std::printf("\n");
            } else {
                any_fail = true;
                if (r.st == ReadStatus::kAbnormal) {
                    std::printf("%08X %-16s %10s %10s %-8s %s(%02X %s)\n",
                                di, name, "-", "-", "", read_status_text(r.st),
                                r.err_code, r.why);
                } else {
                    std::printf("%08X %-16s %10s %10s %-8s %s (%s)\n",
                                di, name, "-", "-", "", read_status_text(r.st), r.why);
                }
            }
        }
        if (round + 1 < a.count && a.interval_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(a.interval_ms));
        }
    }

    const MasterStats& st = master.stats();
    std::printf("\n=== 统计 ===\n");
    std::printf("请求 %ld / 成功 %ld / 重试 %ld / 重试后成功 %ld\n",
                st.requests, st.ok, st.retries, st.ok_after_retry);
    std::printf("超时 %ld / 坏帧 %ld / 异常 %ld / 地址不符 %ld / DI 不符 %ld\n",
                st.timeouts, st.bad_frames, st.abnormals, st.addr_mismatch, st.di_mismatch);

    // ★ 这条不是"看起来完整"的收尾话术，而是一道自洽判据：
    //   每一次请求要么成功、要么落进五个失败分类之一。若两边不等，
    //   说明有失败走了**没被计数**的分支（例如 kLinkError / kNoSpec），
    //   而那正是"统计看起来正常、实际漏了东西"的典型征兆。
    const long classified = st.ok + st.timeouts + st.bad_frames + st.abnormals +
                            st.addr_mismatch + st.di_mismatch;
    std::printf("结论: %s（已归类 %ld / 请求 %ld）\n",
                classified == st.requests ? "所有请求均已归类" : "存在未归类的请求",
                classified, st.requests);

    link->close();
    delete env;
#ifdef _WIN32
    if (!a.tcp.empty()) delete link;
    else if (!a.serial.empty()) delete link;
#endif
    return any_fail ? 1 : 0;
}
