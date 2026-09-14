// =====================================================================
// P3/ 单元测试 —— TCP 传输层（现场最后一跳）
//
//   T51: TCP 基座 —— 连接 / 带超时的连接失败 / receive 的**三态语义**
//   T52: Modbus TCP 事务 —— 往返字节与环回实现**完全一致**
//   T53: 分帧鲁棒性 —— TCP 分片重组 + 事务号错位**必须被拒绝**
//   T54: Modbus TCP 闭环等价 —— 经真 socket 跑 400 拍，与进程内点表逐位一致
//   T55: IEC104 TCP 建链 —— STARTDT / 初始化结束 / 总召唤三段式跨 socket 成立
//   T56: IEC104 TCP 闭环等价 —— 经真 socket 跑 400 拍，逐位一致
//   T57: 真断链 —— 对端关闭 → 采集不可信 → 状态机 FAULT
//
// 本文件与 test_modbus.cpp / test_iec104.cpp 的关系：
//   前面两个用 **环回传输**（同调用栈内搬字节），证明"编解码 + 适配器"正确；
//   本文件把传输层换成 **真内核 socket**，证明"换传输层不改结果"。
//   两者合起来，才是 P0 那条承诺的完整兑现：
//       SimDeviceIO → MemoryDeviceIO → RtDbDeviceIO → Loopback → **TCP**
//
// 为什么这算"真"验证而不是又一个 mock：
//   服务端是**独立线程 + 真 listen/accept/recv/send**，数据真的经过内核协议栈
//   与 127.0.0.1 网卡回路。没有跳过任何一个字节的编解码，也没有跳过内核。
//
// 编译：见 P3/scripts/build_test_tcp.bat（MinGW 需链接 -lws2_32）
// =====================================================================

#include "data_models.h"
#include "device_io.h"
#include "ems_point_table.h"
#include "iec104_codec.h"
#include "iec104_device_io.h"
#include "memory_device_io.h"
#include "modbus_codec.h"
#include "modbus_device_io.h"
#include "modbus_slave_sim.h"
#include "realtime_loop.h"
#include "tcp_transport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace ems;
using namespace ems::modbus;
using namespace ems::iec104;
using namespace ems::net;

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

// =====================================================================
// 公共装置（与 test_modbus.cpp 逐行一致，保证两路比的是介质而不是装配）
// =====================================================================
static bool almost_equal(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

static void env_at(int step, double& p_load_kw, double& p_pv_kw) {
    const double t = step * 0.1;
    p_load_kw = 380.0 + 120.0 * std::sin(t / 30.0);
    p_pv_kw   = (t > 5.0 && t < 25.0) ? 150.0 + 60.0 * std::sin(t / 5.0) : 0.0;
}

static void configure_runtime(EmsRuntime& rt) {
    rt.config().dt_s = 0.1;
    rt.config().enable_realtime_correction = true;
    rt.config().l2_correction_max_kw = 100.0;
    rt.safety_params().grid_p_min_kw = -1e9;
    rt.safety_params().ramp_kw_per_s = 1e9;
    rt.apply_configs();
    rt.fsm().request_run(true);
    rt.device_limits().transformer_capacity_kw = 800.0;
    rt.device_limits().d_target_kw = 250.0;
}

static int diff_logs(const std::vector<StepRecord>& a, const std::vector<StepRecord>& b,
                     double eps = 0.0, bool verbose = false) {
    int diff = 0;
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const StepRecord& x = a[i];
        const StepRecord& y = b[i];
        const bool same =
            almost_equal(x.p_cmd, y.p_cmd, eps) && almost_equal(x.p_actual, y.p_actual, eps) &&
            almost_equal(x.p_grid, y.p_grid, eps) && almost_equal(x.soc, y.soc, eps) &&
            almost_equal(x.temp, y.temp, eps) && almost_equal(x.p_lower, y.p_lower, eps) &&
            almost_equal(x.p_upper, y.p_upper, eps) && almost_equal(x.plan_target, y.plan_target, eps) &&
            almost_equal(x.correction, y.correction, eps) && almost_equal(x.t, y.t, eps) &&
            x.state == y.state && x.clamped == y.clamped &&
            x.safety_clip == y.safety_clip && x.state_gated == y.state_gated &&
            x.hold_last == y.hold_last && x.fault_bits == y.fault_bits &&
            x.reason == y.reason;
        if (!same) {
            ++diff;
            if (verbose && diff <= 3) {
                std::cerr.precision(17);
                std::cerr << "        第 " << i << " 拍不同:\n"
                          << "          A cmd=" << x.p_cmd << " act=" << x.p_actual
                          << " grid=" << x.p_grid << " soc=" << x.soc
                          << " temp=" << x.temp << " lo=" << x.p_lower << " up=" << x.p_upper
                          << " plan=" << x.plan_target << " corr=" << x.correction
                          << " t=" << x.t << " st=" << state_name(x.state)
                          << " cl=" << x.clamped << " sc=" << x.safety_clip
                          << " sg=" << x.state_gated << " hl=" << x.hold_last
                          << " fb=" << x.fault_bits << " rs=" << x.reason << "\n"
                          << "          B cmd=" << y.p_cmd << " act=" << y.p_actual
                          << " grid=" << y.p_grid << " soc=" << y.soc
                          << " temp=" << y.temp << " lo=" << y.p_lower << " up=" << y.p_upper
                          << " plan=" << y.plan_target << " corr=" << y.correction
                          << " t=" << y.t << " st=" << state_name(y.state)
                          << " cl=" << y.clamped << " sc=" << y.safety_clip
                          << " sg=" << y.state_gated << " hl=" << y.hold_last
                          << " fb=" << y.fault_bits << " rs=" << y.reason << "\n";
            }
        }
    }
    return diff;
}

// =====================================================================
// 真 socket 服务端（独立线程；listen 127.0.0.1:自动端口）
// =====================================================================
class TcpTestServer {
public:
    ~TcpTestServer() { close_listener(); }

    bool start() {
        if (!SocketRuntime::ensure()) return false;
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_fd_ == kInvalidSocket) return false;

        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&yes),
                     static_cast<bms_socklen_t>(sizeof(yes)));

        sockaddr_in a;
        std::memset(&a, 0, sizeof(a));
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port        = 0;                     // 让内核分配端口，避免测试间抢占
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&a),
                   static_cast<int>(sizeof(a))) != 0) { close_listener(); return false; }
        if (::listen(listen_fd_, 4) != 0) { close_listener(); return false; }

        bms_socklen_t sl = static_cast<bms_socklen_t>(sizeof(a));
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&a), &sl) != 0) {
            close_listener();
            return false;
        }
        port_ = ntohs(a.sin_port);
        return port_ != 0;
    }

    uint16_t port() const { return port_; }

    bms_socket_t accept_one(int timeout_ms) {
        if (listen_fd_ == kInvalidSocket) return kInvalidSocket;
        if (wait_ready(listen_fd_, /*want_write=*/false, timeout_ms) != 1) return kInvalidSocket;
        return ::accept(listen_fd_, nullptr, nullptr);
    }

    void close_listener() {
        if (listen_fd_ != kInvalidSocket) { sock_close(listen_fd_); listen_fd_ = kInvalidSocket; }
    }

private:
    bms_socket_t listen_fd_ = kInvalidSocket;
    uint16_t     port_      = 0;
};

// 服务端工作线程（析构时自动 stop + join）
struct Worker {
    std::thread       th;
    std::atomic<bool> stop{false};

    template <typename F>
    void run(F&& f) { th = std::thread(std::forward<F>(f)); }

    void join() { if (th.joinable()) th.join(); }

    ~Worker() {
        stop.store(true);
        join();
    }
};

static void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// =====================================================================
// Modbus 服务端循环：MBAP 分帧 → 从站 → 回响应（可选择分片/重复注入）
// =====================================================================
struct ModbusServerCfg {
    int chunk      = 0;    // >0：响应按该字节数分多次 send（造 TCP 分片）
    int force_tid  = -1;   // >=0：把响应的 MBAP 事务号强制改成该值（造确定性错位）
    int max_frames = -1;   // >0：处理 N 帧后主动关闭（造真断链）
};

static bool send_chunked(TcpSocket* c, const uint8_t* p, size_t n, int chunk) {
    if (chunk <= 0) return c->send_all(p, n, 2000);
    size_t off = 0;
    while (off < n) {
        const size_t k = std::min(static_cast<size_t>(chunk), n - off);
        if (!c->send_all(p + off, k, 2000)) return false;
        off += k;
        sleep_ms(1);
    }
    return true;
}

static void modbus_serve(TcpSocket* conn, ModbusSlaveSim* slave, std::mutex* mx,
                         ModbusServerCfg cfg, std::atomic<bool>* stop,
                         std::vector<std::vector<uint8_t>>* captured) {
    uint8_t hdr[kMbapLen];
    uint8_t req[512];
    uint8_t resp[512];
    int frames = 0;

    while (!stop->load()) {
        const int first = conn->recv_some(hdr, 1, 100);
        if (first < 0) break;            // 对端关闭 / 链路错误
        if (first == 0) continue;        // 超时：继续等
        if (!conn->recv_exact(hdr + 1, sizeof(hdr) - 1, 2000)) break;

        const size_t body = mbap_body_len(hdr);
        if (body == 0 || body > kMaxPduLen) break;
        std::memcpy(req, hdr, sizeof(hdr));
        if (!conn->recv_exact(req + kMbapLen, body, 2000)) break;
        const size_t req_len = kMbapLen + body;

        if (captured != nullptr) {
            std::lock_guard<std::mutex> lk(*mx);
            captured->emplace_back(req, req + req_len);
        }

        size_t rn = 0;
        {
            std::lock_guard<std::mutex> lk(*mx);
            rn = slave->handle(req, req_len, resp, sizeof(resp));
        }
        if (rn == 0) break;

        if (cfg.force_tid >= 0 && rn >= 2) {          // 事务号错位注入
            resp[0] = static_cast<uint8_t>((cfg.force_tid >> 8) & 0xFF);
            resp[1] = static_cast<uint8_t>(cfg.force_tid & 0xFF);
        }
        if (!send_chunked(conn, resp, rn, cfg.chunk)) break;

        ++frames;
        if (cfg.max_frames > 0 && frames >= cfg.max_frames) break;
    }
}

// =====================================================================
// IEC104 服务端循环：收到的字节 → feed() → take() → 发回
//
// 与环回装置的关系：LoopbackIec104Transport 在**同一调用栈**里做
//   send() → station->feed()  与  receive() → station->take()
// 本函数把这两件事搬到两个线程 + 真 socket 上，语义完全相同。
// 从站的"周期上送"由 process() 末尾的 queue_cyclic_burst() 自动产生，
// 因此这里不需要额外定时器。
//
// ★ 跨线程带来的唯一新问题：**服务端可能比客户端慢半拍**。
//   "收帧即上送"意味着上送值是"服务端处理这一帧时设备的当前值"。若服务端还在
//   处理上一拍遗留的帧，它读到的就是上一拍的环境 —— 上送本身没错，但它会在
//   客户端已经写入本拍环境之后才被读走，表现为"整体滞后一拍"。
//   解决办法不是加等待，而是让双方**汇合（rendezvous）**：服务端每处理完一批
//   入向字节就把 round 加一，客户端用它确认"我发出去的帧已经被消化完了"。
//   见 Iec104TcpRig::sync_uplink() 的三段式。
// =====================================================================
static void iec104_serve(TcpSocket* conn, Iec104ControlledStationSim* sim,
                         std::mutex* mx, std::condition_variable* cv,
                         uint64_t* round, std::atomic<bool>* stop) {
    while (!stop->load()) {
        uint8_t buf[4096];
        const int n = conn->recv_some(buf, sizeof(buf), 5);
        if (n < 0) break;                 // 对端关闭
        if (n > 0) {
            {
                std::lock_guard<std::mutex> lk(*mx);
                // 收到帧就按周期上送（与环回装置同一份 sim 代码、同一个语义）。
                // 注意不能改成"服务端自定时无脑上送"：客户端 drain_rx(0) 的语义是
                // "一直收到没有数据为止"，对端持续上送会让它永远收不干（实测死循环）。
                sim->feed(buf, static_cast<size_t>(n));
            }
            for (;;) {
                uint8_t out[8192];
                size_t got = 0;
                {
                    std::lock_guard<std::mutex> lk(*mx);
                    got = sim->take(out, sizeof(out));
                }
                if (got == 0) break;
                if (!conn->send_all(out, got, 2000)) return;
            }
            // 本批入向字节已消化完、响应已全部发出 → 通知等待方
            {
                std::lock_guard<std::mutex> lk(*mx);
                ++(*round);
            }
            cv->notify_all();
        }
    }
}

// =====================================================================
// T51: TCP 基座 —— 连接 / 连接失败超时 / receive 三态
// =====================================================================
static void test_51_tcp_basics() {
    std::cerr << "[T51] TCP 基座：连接 / 连接失败超时 / receive 三态 ...\n";

    EXPECT(SocketRuntime::ensure());

    TcpTestServer server;
    EXPECT(server.start());
    EXPECT(server.port() != 0);

    // 无人 accept 时也应当能建立连接（内核 backlog），证明监听已就绪
    TcpSocket c;
    EXPECT(c.connect_to("127.0.0.1", server.port(), 2000));
    EXPECT(c.is_connected());
    EXPECT(c.is_open());
    EXPECT(c.last_error() == 0);

    // 服务端 accept 到它
    const bms_socket_t srv_fd = server.accept_one(2000);
    EXPECT(srv_fd != kInvalidSocket);

    if (srv_fd != kInvalidSocket) {
        TcpSocket srv;
        srv.adopt(srv_fd);

        // ---- 收发一个字节 ----
        const uint8_t tx[3] = {0xAA, 0xBB, 0xCC};
        EXPECT(c.send_all(tx, sizeof(tx), 1000));
        uint8_t rx[8] = {};
        size_t got = 0;
        EXPECT(srv.recv_exact(rx, sizeof(tx), 1000));
        got = sizeof(tx);
        if (got == 3) {
            EXPECT(rx[0] == 0xAA && rx[1] == 0xBB && rx[2] == 0xCC);
        }

        // ---- 三态之一：超时（链路仍在）----
        uint8_t nothing[8] = {};
        EXPECT(srv.recv_some(nothing, sizeof(nothing), 30) == 0);   // 0 = 超时无数据

        // ---- 三态之二：对端关闭 → -1 ----
        c.close();
        sleep_ms(30);
        EXPECT(srv.recv_some(nothing, sizeof(nothing), 500) == -1);  // -1 = 链路错误
    }

    // ---- 连接失败必须在超时预算内返回（现场"EMS 卡死"的头号原因）----
    // 注：不可路由地址的失败方式因网络环境而异（可能立即 ICMP 不可达，也可能挂到超时），
    // 所以这里只断言"快速返回 false"，不断言走的是哪条路径。
    {
        TcpTestServer s2;
        s2.start();
        const uint16_t closed_port = s2.port();
        s2.close_listener();                         // 端口已关闭 → connect 必被拒绝
        TcpSocket bad;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = bad.connect_to("127.0.0.1", closed_port, 500);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        EXPECT(!ok);
        EXPECT(!bad.is_connected());
        EXPECT(ms < 2000);
        std::cerr << "        已关闭端口连接耗时 " << ms << " ms（预算 2000 ms）\n";
    }
}

// =====================================================================
// Modbus TCP 装置
// =====================================================================
struct ModbusTcpRig {
    MemoryDeviceIO         dev;
    ModbusSlaveSim         slave;
    TcpTestServer          server;
    std::mutex             mx;        // 声明在 worker 之前：析构时 worker 先停，再拆 mx
    std::vector<std::vector<uint8_t>> captured;
    ModbusTcpTransport     transport;
    ModbusDeviceIO         io;
    EmsRuntime             rt;
    Worker                 worker;

    explicit ModbusTcpRig(MapProfile profile, ModbusServerCfg cfg = ModbusServerCfg{})
        : slave(profile), io(&transport, profile, 1) {
        if (!server.start()) { std::cerr << "  FAIL server.start()\n"; ++g_fail; return; }

        transport.set_endpoint("127.0.0.1", server.port());
        transport.set_timeout_ms(2000);
        transport.set_connect_timeout_ms(2000);
        transport.set_expect_unit_id(1);

        slave.attach_device(&dev);
        slave.publish_device_points();

        ModbusSlaveSim*  s  = &slave;
        std::mutex*      m  = &mx;
        std::atomic<bool>* sp = &worker.stop;
        auto* cap = &captured;
        worker.run([this, s, m, sp, cap, cfg] {
            TcpSocket c;
            c.adopt(server.accept_one(5000));
            if (c.is_connected()) {
                modbus_serve(&c, s, m, cfg, sp, cap);
                c.close();
            }
        });

        io.set_device_pump([this](double cmd, double dt) {
            dev.execute(cmd, dt);
            std::lock_guard<std::mutex> lk(mx);
            slave.publish_device_points();
        });

        io.open();
        rt.attach_device(&io);
        configure_runtime(rt);
    }

    void run(int n, int step_base = 0) {
        rt.run(n, 0.1, [this, step_base](EmsRuntime&, int i) {
            double load = 0.0, pv = 0.0;
            env_at(step_base + i, load, pv);
            dev.set_environment(load, pv);
            std::lock_guard<std::mutex> lk(mx);
            slave.publish_device_points();
        });
    }
};

// ---- 参考实现：EmsRuntime 直连进程内点表 ----
struct MemoryRig {
    MemoryDeviceIO dev;
    EmsRuntime     rt;

    MemoryRig() {
        rt.attach_device(&dev);
        configure_runtime(rt);
    }
    void run(int n, int step_base = 0) {
        rt.run(n, 0.1, [&](EmsRuntime&, int i) {
            double load = 0.0, pv = 0.0;
            env_at(step_base + i, load, pv);
            dev.set_environment(load, pv);
        });
    }
};

// =====================================================================
// T52: Modbus TCP 事务 —— 往返字节与环回实现一致
// =====================================================================
static void test_52_modbus_tcp_transaction() {
    std::cerr << "[T52] Modbus TCP 事务：往返字节与环回实现一致 ...\n";

    ModbusTcpRig rig(MapProfile::kStandardF32);

    EXPECT(rig.transport.connected());
    EXPECT(rig.io.is_open());
    EXPECT(rig.transport.connects() == 1);

    // ---- 读一次快照（底层 = 读 MEAS 块报文）----
    RealtimeSnapshot snap;
    const bool ok = rig.io.read_snapshot(0.0, snap);
    EXPECT(ok);
    EXPECT(rig.transport.transactions() > 0);
    EXPECT(rig.transport.format_errors() == 0);
    EXPECT(rig.transport.link_errors() == 0);
    EXPECT(rig.io.stale_reads() == 0);
    EXPECT(rig.io.exceptions() == 0);

    // ---- 报文确实经过了 socket：服务端捕获到的每一帧都必须自洽 ----
    {
        std::lock_guard<std::mutex> lk(rig.mx);
        EXPECT(!rig.captured.empty());
        bool all_pid_zero = true, all_unit_ok = true, all_reads = true;
        bool saw_meas_block = false;   // 是否出现过"从 0x0000 起读 MEAS 块"的请求
        for (const std::vector<uint8_t>& rq : rig.captured) {
            if (rq.size() < static_cast<size_t>(kMbapLen) + 5) { all_reads = false; continue; }
            // MBAP: TID(2) PID(2) LEN(2) UID(1)
            if (!(rq[2] == 0x00 && rq[3] == 0x00)) all_pid_zero = false;
            if (rq[6] != 0x01)                     all_unit_ok  = false;
            const uint8_t fc = rq[7];
            if (fc != kReadHoldingRegs && fc != kReadInputRegs &&
                fc != kWriteMultipleRegs && fc != kWriteSingleReg) all_reads = false;
            if (fc == kReadHoldingRegs || fc == kReadInputRegs) {
                const uint16_t addr = static_cast<uint16_t>((rq[8] << 8) | rq[9]);
                if (addr == 0x0000) saw_meas_block = true;
            }
        }
        EXPECT(all_pid_zero);
        EXPECT(all_unit_ok);
        EXPECT(all_reads);
        EXPECT(saw_meas_block);        // 读 snapshot 一定会读 MEAS 块
    }

    // ---- 与设备真值对照：SOC 必须与设备侧一致 ----
    const double soc_dev = rig.dev.get(EMS_POINT_NAMES[EMS_SOC]);
    EXPECT_NEAR(snap.soc, soc_dev, 1e-6);

    // ---- 写指令也要真的过 socket ----
    PowerCommand pc;
    pc.p_bat_cmd_kw = 42.5;
    pc.p_lower      = -50.0;
    pc.p_upper      = 60.0;
    EXPECT(rig.io.write_command(pc));
    EXPECT(rig.transport.transactions() > 1);
    EXPECT(rig.transport.format_errors() == 0);

    std::cerr << "        事务=" << rig.transport.transactions()
              << " 链路错误=" << rig.transport.link_errors()
              << " 格式错=" << rig.transport.format_errors() << "\n";
}

// =====================================================================
// T53: 分帧鲁棒性 —— TCP 分片重组 + 事务号错位必须被拒绝
// =====================================================================
static void test_53_fragmentation_and_misalignment() {
    std::cerr << "[T53] 分帧：TCP 分片重组 / 事务号错位拒绝 ...\n";

    // ---- (a) 服务端每次只发 2 字节：客户端必须自己重组 ----
    {
        ModbusServerCfg cfg;
        cfg.chunk = 2;
        ModbusTcpRig rig(MapProfile::kStandardF32, cfg);

        RealtimeSnapshot snap;
        EXPECT(rig.io.read_snapshot(0.0, snap));
        EXPECT(rig.transport.link_errors() == 0);
        EXPECT(rig.transport.format_errors() == 0);
        // 分片送达不应造成任何"采集失败"
        EXPECT(rig.io.stale_reads() == 0);
        EXPECT_NEAR(snap.soc, rig.dev.get(EMS_POINT_NAMES[EMS_SOC]), 1e-6);

        std::cerr << "        分片=2B 事务=" << rig.transport.transactions()
                  << " 格式错=" << rig.transport.format_errors() << "\n";
    }

    // ---- (b) 服务端强制回错事务号 → 必须被拒绝（不能把错位数据当正确响应）----
    {
        TcpTestServer server;
        EXPECT(server.start());

        MemoryDeviceIO dev;
        ModbusSlaveSim slave(MapProfile::kStandardF32);
        slave.attach_device(&dev);
        slave.publish_device_points();

        Worker      w;
        std::mutex  mx;
        ModbusServerCfg cfg;
        cfg.force_tid = 0x0001;    // 无论请求事务号是多少，响应一律回 0x0001

        TcpTestServer*     srv = &server;
        ModbusSlaveSim*    s   = &slave;
        std::mutex*        m   = &mx;
        std::atomic<bool>* sp  = &w.stop;
        w.run([srv, s, m, sp, cfg] {
            TcpSocket c;
            c.adopt(srv->accept_one(3000));
            if (c.is_connected()) {
                modbus_serve(&c, s, m, cfg, sp, nullptr);
                c.close();
            }
        });

        ModbusTcpTransport t;
        t.set_endpoint("127.0.0.1", server.port());
        t.set_timeout_ms(1000);
        t.set_expect_unit_id(1);
        EXPECT(t.connect());

        uint8_t req[64];
        uint8_t resp[64];
        size_t  rn = 0;

        // 请求 #1：TID = 0x0001，与强制值一致 → 正常通过
        size_t n = build_read_request(req, sizeof(req), 0x0001, 1, kReadHoldingRegs, 0x0000, 14);
        EXPECT(n > 0);
        EXPECT(t.transact(req, n, resp, sizeof(resp), &rn));
        EXPECT(rn > 0);
        EXPECT(t.format_errors() == 0);

        // 请求 #2：TID = 0x0002，响应被改成 0x0001 → 必须拒绝
        n = build_read_request(req, sizeof(req), 0x0002, 1, kReadHoldingRegs, 0x0000, 14);
        EXPECT(!t.transact(req, n, resp, sizeof(resp), &rn));
        EXPECT(rn == 0);
        EXPECT(t.format_errors() == 1);
        EXPECT(t.connected());     // 错位是报文层问题，不该被误判成断链

        std::cerr << "        强制错位 → 格式错=" << t.format_errors()
                  << " 仍在线=" << (t.connected() ? "yes" : "no") << "\n";
    }
}

// =====================================================================
// T54: Modbus TCP 闭环等价（400 拍）
// =====================================================================
static void test_54_modbus_tcp_closed_loop() {
    std::cerr << "[T54] 闭环等价：Modbus over TCP vs 进程内点表（400 拍）...\n";

    MemoryRig ref;
    ref.run(400, 0);
    const std::vector<StepRecord> base = ref.rt.log();

    ModbusTcpRig rig(MapProfile::kWideF64);
    rig.run(400, 0);
    const std::vector<StepRecord> test = rig.rt.log();

    EXPECT(base.size() == 400);
    EXPECT(test.size() == 400);
    const int diff = diff_logs(base, test, 0.0, /*verbose=*/true);
    EXPECT(diff == 0);

    // 反向守卫：不能"两边都恒零"骗过等价性
    double peak = 0.0;
    for (const StepRecord& r : test) peak = std::max(peak, std::fabs(r.p_cmd));
    EXPECT(peak > 50.0);
    EXPECT(test.front().soc != test.back().soc);
    EXPECT(rig.io.stale_reads() == 0);
    EXPECT(rig.transport.link_errors() == 0);
    EXPECT(rig.transport.format_errors() == 0);

    std::cerr << "        逐位差异 " << diff << " 拍 / 峰值指令 " << peak
              << " kW / SOC " << test.front().soc << " → " << test.back().soc
              << " / 事务 " << rig.transport.transactions() << "\n";
}

// =====================================================================
// IEC104 TCP 装置
//
// 与 Modbus 路的关键差别：104 没有"一问一答"，上送是**服务端主动**产生的
// （收到任何一帧就按周期上送一轮）。跨真 socket + 跨线程之后，就多了一个
// 纯粹由**调度时序**产生的问题：
//
//   服务端在 T 时刻处理一个"上一拍发出去的帧"时，读到的设备值是**上一拍的**；
//   这一轮上送的字节却可能在客户端已经写入本拍环境之后才被收走。
//   于是逐拍比对表现为"整体滞后一拍"—— 数值全部正确，只是错位。
//
// 这不是传输层的缺陷（字节一个没少、编解码一字不差），而是**装置装配**的问题。
// 环回装置不会有它：同调用栈内"发帧 → 上送 → 取值"是原子的。
//
// 三条纪律（本装置的全部秘密）：
//   1. **先汇合，再改环境**。写入本拍环境之前，必须确认"之前发出去的帧已经
//      全部被服务端消化"。做法：发一帧触发词 + 等 round 自增（服务端每消化完
//      一批入向字节就把 round 加一），再把这一轮收到的旧值全部丢弃。
//   2. **改完环境再触发**。此后服务端的任何一轮上送都必然带本拍值。
//   3. **收干 = 一直取到没有为止**。TCP 是字节流，一轮上送可能跨多个 recv。
// =====================================================================
struct Iec104TcpRig {
    MemoryDeviceIO             dev;
    Iec104ControlledStationSim sim;
    TcpTestServer              server;
    std::mutex                 mx;        // 声明在 worker 之前：析构时 worker 先停，再拆 mx
    std::condition_variable    cv;        // 服务端 → 客户端的"这一批已消化"信号
    uint64_t                   round = 0; // 服务端每消化完一批入向字节 +1
    Iec104TcpTransport         transport;
    Iec104DeviceIO             io;
    EmsRuntime                 rt;
    Worker                     worker;

    int resyncs              = 0;  // 需要第二次触发才对上的拍数（时序抖动，非错误）
    int resync_failures      = 0;  // sync 连 4 轮都对不上的拍数（真问题，应为 0）
    int exec_refresh_failures = 0; // execute 后 cache 追不上设备的拍数（应为 0）

    explicit Iec104TcpRig(Iec104Profile profile)
        : sim(profile, 1), io(&transport, profile, 1) {
        if (!server.start()) { std::cerr << "  FAIL server.start()\n"; ++g_fail; return; }

        transport.set_endpoint("127.0.0.1", server.port());
        transport.set_send_timeout_ms(2000);
        transport.set_connect_timeout_ms(2000);
        // open() 用 drain_rx(0) 等 STARTDT 确认 —— 跨 socket 需要一个等待窗口
        transport.set_min_wait_ms(50);

        sim.attach_device(&dev);

        Iec104ControlledStationSim* s = &sim;
        std::mutex* m = &mx;
        std::condition_variable* cvp = &cv;
        uint64_t* rnd = &round;
        std::atomic<bool>* sp = &worker.stop;
        worker.run([this, s, m, cvp, rnd, sp] {
            TcpSocket c;
            c.adopt(server.accept_one(5000));
            if (c.is_connected()) {
                iec104_serve(&c, s, m, cvp, rnd, sp);
                c.close();
            }
        });

        io.set_device_pump([this](double cmd, double dt) {
            double p_bat = 0.0;
            {
                std::lock_guard<std::mutex> lk(mx);
                p_bat = dev.execute(cmd, dt);
            }
            (void)p_bat;
            refresh_after_execute();
        });

        const bool opened = io.open();
        transport.set_min_wait_ms(0);      // 运行期回到严格非阻塞
        EXPECT(opened);
        rt.attach_device(&io);
        configure_runtime(rt);
    }

    // 把"已经收到的字节"全部取走（非阻塞，取到没有为止）。
    // 只做接收，不发触发帧 —— 否则对端会被问出新数据，永远收不干。
    void drain_dry() {
        io.set_poll_keepalive(false);
        while (io.poll(0)) { }
    }

    // 发一帧触发词 + 等服务端确认"这一批已消化完"（0 = 超时）
    bool trigger_round(int wait_ms = 2000) {
        uint64_t r0 = 0;
        {
            std::lock_guard<std::mutex> lk(mx);
            r0 = round;
        }
        io.set_poll_keepalive(true);
        io.poll(0);
        io.set_poll_keepalive(false);
        std::unique_lock<std::mutex> lk(mx);
        return cv.wait_for(lk, std::chrono::milliseconds(wait_ms),
                           [&] { return round > r0; });
    }

    // 现场语义：把本拍环境写进设备，**等到从站把它上送回来**再进入算法拍。
    // 现场也是这样：算法用的是"本周期采到的"数据，不是"上一周期采到的"。
    void sync_uplink(double load, double pv) {
        // ① 汇合：把上一拍遗留的帧消化掉，并丢弃它们那一轮"旧值上送"
        drain_dry();
        trigger_round();
        drain_dry();

        // ② 写环境。此后服务端读到的设备值就是本拍值
        double want_load = 0.0, want_pv = 0.0;
        {
            std::lock_guard<std::mutex> lk(mx);
            dev.set_environment(load, pv);
            want_load = dev.get(EMS_POINT_NAMES[EMS_P_LOAD]);
            want_pv   = dev.get(EMS_POINT_NAMES[EMS_P_PV]);
        }

        // ③ 触发 → 收干 → 核对。TCP 是字节流，一轮上送可能跨多个 recv，
        //    故这里用"核对到对上为止"兜住分片，而不是假设一次 poll 就够。
        for (int k = 0; k < 4; ++k) {
            trigger_round();
            drain_dry();
            if (std::fabs(io.cached_value(EMS_P_LOAD) - want_load) < 1e-12 &&
                std::fabs(io.cached_value(EMS_P_PV)   - want_pv)   < 1e-12) {
                if (k > 0) ++resyncs;
                return;
            }
        }
        ++resync_failures;
    }

    // 把设备推进一拍之后**换一轮上送**，让 cache 追上设备。
    //
    // 为什么这一步不可省：EMS_P_GRID 不是"外部注入量"，而是 MemoryDeviceIO
    // 在 execute() 里按  P_LOAD + 站用电 − P_PV − P_BAT  现算出来的派生点
    // （SOC / T_C / P_BAT 同理）。step() 第 ⑪ 步用 read_actuals() 记录真值，
    // 而 104 适配器的 read_actuals() 读的是**最近一次上送的 cache**。
    // 若不在这里刷新，记录的就会是"上一拍 execute() 算出来的 GRID" ——
    // 数值全对、整体滞后一拍，正是最容易被当成"传输层有问题"的那类假象。
    // 环回装置不需要它：那里 read_actuals() 直接读设备。
    //
    // 这里同样**核对**而不是"发一次就假定到货"：execute() 刚刚发过 3 个遥调帧，
    // 服务端可能把它们拆成几批消化，其中一批的响应可能正好落在本函数之后 ——
    // 那种"差半步"的时序如果只靠等待，会间歇性地漏一拍。
    void refresh_after_execute() {
        for (int k = 0; k < 4; ++k) {
            trigger_round();
            drain_dry();
            double p_bat_dev = 0.0, soc_dev = 0.0;
            {
                std::lock_guard<std::mutex> lk(mx);
                p_bat_dev = dev.get(EMS_POINT_NAMES[EMS_P_BAT]);
                soc_dev   = dev.get(EMS_POINT_NAMES[EMS_SOC]);
            }
            if (std::fabs(io.cached_value(EMS_P_BAT) - p_bat_dev) < 1e-12 &&
                std::fabs(io.cached_value(EMS_SOC)   - soc_dev)   < 1e-12) {
                return;
            }
        }
        ++exec_refresh_failures;
    }

    void run(int n, int step_base = 0) {
        rt.run(n, 0.1, [this, step_base](EmsRuntime&, int i) {
            double load = 0.0, pv = 0.0;
            env_at(step_base + i, load, pv);
            sync_uplink(load, pv);
        });
    }
};

// =====================================================================
// T55: IEC104 TCP 建链会话
// =====================================================================
static void test_55_iec104_tcp_session() {
    std::cerr << "[T55] IEC104 TCP 建链：STARTDT / 初始化结束 / 总召唤 ...\n";

    Iec104TcpRig rig(Iec104Profile::kWidePrivate);

    EXPECT(rig.transport.connected());
    EXPECT(rig.io.is_open());
    EXPECT(rig.io.startdt_ok());          // 从站已确认 STARTDT
    EXPECT(rig.io.gi_done());             // 总召唤三段式走完
    EXPECT(rig.io.unacked_tx() == 0);     // 建链后没有悬挂的未确认帧

    {
        std::lock_guard<std::mutex> lk(rig.mx);
        EXPECT(rig.sim.gi_count() == 1);      // 恰好一次总召唤
        EXPECT(rig.sim.started());
        EXPECT(rig.sim.bad_ca() == 0);
        EXPECT(rig.sim.malformed() == 0);
    }
    EXPECT(rig.transport.link_errors() == 0);
    EXPECT(rig.transport.bytes_tx() > 0);
    EXPECT(rig.transport.bytes_rx() > 0);

    // 建链即拉全量：SOC 应已到达
    EXPECT(rig.io.cached_value(EMS_SOC) > 0.0);

    std::cerr << "        bytes tx=" << rig.transport.bytes_tx()
              << " rx=" << rig.transport.bytes_rx()
              << " / 总召回数=" << rig.sim.gi_count() << "\n";
}

// =====================================================================
// T56: IEC104 TCP 闭环等价（400 拍）
// =====================================================================
static void test_56_iec104_tcp_closed_loop() {
    std::cerr << "[T56] 闭环等价：IEC104 over TCP vs 进程内点表（400 拍）...\n";

    MemoryRig ref;
    ref.run(400, 0);
    const std::vector<StepRecord> base = ref.rt.log();

    Iec104TcpRig rig(Iec104Profile::kWidePrivate);
    rig.run(400, 0);
    const std::vector<StepRecord> test = rig.rt.log();

    EXPECT(base.size() == 400);
    EXPECT(test.size() == 400);
    const int diff = diff_logs(base, test, 0.0, /*verbose=*/true);
    EXPECT(diff == 0);

    double peak = 0.0;
    for (const StepRecord& r : test) peak = std::max(peak, std::fabs(r.p_cmd));
    EXPECT(peak > 50.0);
    EXPECT(test.front().soc != test.back().soc);
    // 时序抖动会造成"第二轮才对上"，但绝不允许"对不上"
    EXPECT(rig.resync_failures == 0);
    EXPECT(rig.exec_refresh_failures == 0);
    EXPECT(rig.transport.link_errors() == 0);

    std::cerr << "        逐位差异 " << diff << " 拍 / 峰值指令 " << peak
              << " kW / SOC " << test.front().soc << " → " << test.back().soc
              << " / 次轮对齐 " << rig.resyncs << " 拍 / 对齐失败 "
              << rig.resync_failures << " / 执行后回追失败 "
              << rig.exec_refresh_failures
              << " / bytes tx=" << rig.transport.bytes_tx()
              << " rx=" << rig.transport.bytes_rx() << "\n";
}

// =====================================================================
// T57: 真断链 —— 对端关闭 → 采集不可信 → FAULT
// =====================================================================
static void test_57_link_drop_to_fault() {
    std::cerr << "[T57] 真断链：对端关闭 → 采集不可信 → FAULT ...\n";

    // 每拍约 7 次事务（4 次读 + 3 次写），800 帧 ≈ 114 拍后服务端主动关闭。
    // 先跑 60 拍确认"断链前是正常出力的"，再继续跑，让断链落在后半段。
    ModbusServerCfg cfg;
    cfg.max_frames = 800;
    ModbusTcpRig rig(MapProfile::kWideF64, cfg);

    rig.run(60, 0);                     // 断链前：应当正常出力
    double peak = 0.0;
    for (const StepRecord& r : rig.rt.log()) peak = std::max(peak, std::fabs(r.p_cmd));
    EXPECT(rig.transport.connected());  // 这一阶段链路必须还在
    EXPECT(peak > 1.0);

    rig.run(200, 60);                   // 继续跑：帧数耗尽后服务端关闭连接

    const StepRecord& last = rig.rt.log().back();
    EXPECT(!rig.transport.connected());                       // 链路确实断了
    EXPECT(rig.io.stale_reads() > 0 || rig.transport.link_errors() > 0);
    EXPECT(rig.rt.fsm().state() == EmsState::kFault);
    EXPECT(std::fabs(last.p_cmd) < 1e-9);
    EXPECT(last.state_gated);

    std::cerr << "        断链前峰值 " << peak
              << " kW / 采集失败=" << rig.io.stale_reads()
              << " 链路错误=" << rig.transport.link_errors()
              << " 状态=" << state_name(rig.rt.fsm().state()) << "\n";
}

// =====================================================================
int main() {
    std::cerr << "=========================================\n"
              << " P3/ TCP 传输层 单元测试（真 socket）\n"
              << "=========================================\n";

    test_51_tcp_basics();
    test_52_modbus_tcp_transaction();
    test_53_fragmentation_and_misalignment();
    test_54_modbus_tcp_closed_loop();
    test_55_iec104_tcp_session();
    test_56_iec104_tcp_closed_loop();
    test_57_link_drop_to_fault();

    std::cerr << "=========================================\n"
              << " PASS=" << g_pass << "  FAIL=" << g_fail << "\n"
              << "=========================================\n";
    return (g_fail == 0) ? 0 : 1;
}
