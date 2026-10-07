// =====================================================================
// 16/ 单元测试 —— FTP 定值下发通道（T70~T78）
//
//   T70 应答码解析（三位码 + 多行判定）
//   T71 ★ PASV（227）应答解析：6 个数字、端口 p1*256+p2、越界与端口 0
//   T72 端到端登录（含**多行 banner**）
//   T73 ★ 上传 STOR → 服务端收到的字节与本地**逐位一致**
//   T74 ★ 下载 RETR → 拿到的字节与远端**逐位一致**（含 \0 与 \r\n）
//   T75 ★ 150/226 分段：服务端不发 226 时 retrieve() 必须**失败**
//         （不能"数据到手就当成功" —— 半截定值比不传更危险）
//   T76 错误路径：USER 被拒 / PASV 非 227 / RETR 非 150 / 未登录 / 文件不存在
//   T77 大文件（64 KiB 伪随机）往返一致
//   T78 空文件往返 + QUIT 收尾
//
// 为什么在测试进程里起一个极简 FTP 服务端：
//   现场没有可用的公网 FTP，也不能依赖某台设备的真实固件行为。
//   自己写一个"只实现用到的那几条命令"的服务端，既能精确造出
//   多行 banner / 150-226 分段 / PASV 端口这些**坑**，又能逐位对账。
//   它是"可执行的 FTP 协议文档"，而不是一个 mock 桩。
//
// 端口：控制连接固定 **2121**（按任务约定），数据连接由服务端 PASV 时
//       listen(0) 让系统分配 —— 现场 FTP 也常这么做。
//
// 编译：见 16/scripts/build_test.bat
// =====================================================================

#include "ftp.h"
#include "net_base.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
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

// =====================================================================
// 测试用工具：原始 socket 上的"行"读写
// =====================================================================
namespace srv {

inline bool send_all(net::socket_t s, const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const int rc = ::send(s, data.data() + off, (int)(data.size() - off), 0);
        if (rc <= 0) return false;
        off += (std::size_t)rc;
    }
    return true;
}

// 收一行（\n 结束，去掉尾部 \r）。TCP 是字节流，所以必须自带缓冲。
class LineReader {
public:
    explicit LineReader(net::socket_t s) : s_(s) {}

    bool read_line(std::string& out, int timeout_ms) {
        for (;;) {
            const std::size_t nl = buf_.find('\n');
            if (nl != std::string::npos) {
                out.assign(buf_, 0, nl);
                buf_.erase(0, nl + 1);
                if (!out.empty() && out[out.size() - 1] == '\r') out.erase(out.size() - 1);
                return true;
            }
            net::set_recv_timeout(s_, timeout_ms);
            char tmp[2048];
            const int rc = (int)::recv(s_, tmp, (int)sizeof(tmp), 0);
            if (rc <= 0) return false;
            buf_.append(tmp, (std::size_t)rc);
        }
    }

    // 读到对端关闭为止
    bool read_until_eof(std::string& out, int timeout_ms) {
        out.clear();
        if (!buf_.empty()) { out = buf_; buf_.clear(); }
        for (;;) {
            net::set_recv_timeout(s_, timeout_ms);
            char tmp[4096];
            const int rc = (int)::recv(s_, tmp, (int)sizeof(tmp), 0);
            if (rc == 0) return true;
            if (rc < 0) return false;
            out.append(tmp, (std::size_t)rc);
        }
    }

private:
    net::socket_t s_;
    std::string   buf_;
};

}  // namespace srv

// =====================================================================
// 极简 FTP 服务端（进程内线程）
//   只实现 USER / PASS / TYPE / PASV / RETR / STOR / QUIT
// =====================================================================
class FakeFtpServer {
public:
    explicit FakeFtpServer(std::uint16_t control_port = 2121)
        : control_port_(control_port) {}
    ~FakeFtpServer() { stop(); }

    // ---- 可注入的"坑" ----
    bool        banner_multiline  = true;    // 多行 banner
    bool        reject_user       = false;   // USER 回 530
    bool        reject_pass       = false;   // PASS 回 530
    bool        pasv_garbage      = false;   // PASV 回一条非 227
    bool        retr_wrong_code   = false;   // RETR 回 450 而不是 150
    bool        st_226            = true;    // 不发 226（用来证明"分段应答"被正确处理）
    bool        data_host_zero    = false;   // PASV 回 0.0.0.0

    bool start(std::string& err) {
        if (!net::ensure_winsock()) { err = "Winsock 初始化失败"; return false; }
        if (!control_.listen_on(control_port_, 8, err)) return false;
        running_ = true;
        th_ = std::thread(&FakeFtpServer::run, this);
        return true;
    }

    void stop() {
        if (!running_) return;
        running_ = false;
        control_.close();                          // 让 select 里的 accept 超时退出
        if (th_.joinable()) th_.join();
    }

    void put_file(const std::string& name, const std::string& content) {
        std::lock_guard<std::mutex> lk(mu_);
        files_[name] = content;
    }
    bool has_file(const std::string& name) const {
        std::lock_guard<std::mutex> lk(mu_);
        return files_.find(name) != files_.end();
    }
    std::string file(const std::string& name) const {
        std::lock_guard<std::mutex> lk(mu_);
        std::map<std::string, std::string>::const_iterator it = files_.find(name);
        return (it == files_.end()) ? std::string() : it->second;
    }
    int  control_connections() const { return control_conns_.load(); }
    int  commands() const { return commands_.load(); }
    std::uint16_t last_data_port() const { return last_data_port_.load(); }
    int  data_connections() const { return data_conns_.load(); }

private:
    void run() {
        while (running_.load()) {
            bool timed_out = false;
            net::socket_t cs = control_.accept_one(100, timed_out);
            if (cs == net::kInvalidSocket) continue;
            ++control_conns_;
            serve_control(cs);
            net::close_socket(cs);
        }
    }

    void serve_control(net::socket_t cs) {
        srv::LineReader reader(cs);
        // ---- banner ----
        std::string banner;
        if (banner_multiline) {
            banner = "220-EMS 极简 FTP 测试服务端\r\n"
                     "220-这是第二行（多行应答的续行）\r\n"
                     "220 Ready\r\n";
        } else {
            banner = "220 Ready\r\n";
        }
        if (!srv::send_all(cs, banner)) return;

        std::string line;
        for (int guard = 0; guard < 1000; ++guard) {
            if (!reader.read_line(line, 3000)) return;
            ++commands_;
            // 切出命令与参数
            std::string cmd = line, arg;
            const std::size_t sp = line.find(' ');
            if (sp != std::string::npos) { cmd = line.substr(0, sp); arg = line.substr(sp + 1); }
            for (std::size_t i = 0; i < cmd.size(); ++i) {
                if (cmd[i] >= 'a' && cmd[i] <= 'z') cmd[i] = (char)(cmd[i] - 'a' + 'A');
            }

            if (cmd == "USER") {
                if (reject_user) { srv::send_all(cs, "530 拒绝该用户\r\n"); continue; }
                srv::send_all(cs, "331 需要密码\r\n");
            } else if (cmd == "PASS") {
                if (reject_pass) { srv::send_all(cs, "530 密码错误\r\n"); continue; }
                srv::send_all(cs, "230 登录成功\r\n");
            } else if (cmd == "TYPE") {
                srv::send_all(cs, "200 类型已设为 " + arg + "\r\n");
            } else if (cmd == "PASV") {
                if (pasv_garbage) { srv::send_all(cs, "227 不规范的应答：没有括号\r\n"); continue; }
                std::string e;
                if (!data_listener_.listen_on(0, 4, e)) {
                    srv::send_all(cs, "425 无法建立数据连接\r\n");
                    continue;
                }
                const std::uint16_t p = data_listener_.bound_port();
                last_data_port_ = p;
                char buf[128];
                const char* h = data_host_zero ? "0,0,0,0" : "127,0,0,1";
                std::snprintf(buf, sizeof(buf),
                              "227 Entering Passive Mode (%s,%d,%d)\r\n",
                              h, (int)(p / 256), (int)(p % 256));
                srv::send_all(cs, buf);
            } else if (cmd == "RETR") {
                if (retr_wrong_code) { srv::send_all(cs, "450 文件不可用\r\n"); continue; }
                if (!has_file(arg)) { srv::send_all(cs, "550 文件不存在\r\n"); continue; }
                srv::send_all(cs, "150 Opening data connection\r\n");
                std::string content = file(arg);
                bool timed_out = false;
                net::socket_t ds = data_listener_.accept_one(3000, timed_out);
                if (ds == net::kInvalidSocket) {
                    srv::send_all(cs, "425 数据连接失败\r\n");
                    data_listener_.close();
                    continue;
                }
                ++data_conns_;
                if (!content.empty()) srv::send_all(ds, content);
                net::shutdown_send(ds);
                net::close_socket(ds);
                data_listener_.close();
                // ★ 226 与 150 是**两条**应答；不发 226 时客户端必须失败
                if (st_226) srv::send_all(cs, "226 Transfer complete\r\n");
            } else if (cmd == "STOR") {
                srv::send_all(cs, "150 Ready for data\r\n");
                bool timed_out = false;
                net::socket_t ds = data_listener_.accept_one(3000, timed_out);
                if (ds == net::kInvalidSocket) {
                    srv::send_all(cs, "425 数据连接失败\r\n");
                    data_listener_.close();
                    continue;
                }
                ++data_conns_;
                std::string got;
                const bool ok = reader_for(ds).read_until_eof(got, 3000);
                net::close_socket(ds);
                data_listener_.close();
                if (ok) { std::lock_guard<std::mutex> lk(mu_); files_[arg] = got; }
                if (st_226) srv::send_all(cs, "226 Transfer complete\r\n");
            } else if (cmd == "QUIT") {
                srv::send_all(cs, "221 Bye\r\n");
                return;
            } else {
                srv::send_all(cs, "500 不支持的命令：" + cmd + "\r\n");
            }
        }
    }

    srv::LineReader& reader_for(net::socket_t s) {
        // ★ 注意这个"缓存"的生命周期：只在这条数据连接的 STOR 期间使用。
        //   用一个静态局部会串到下一次调用 —— 所以按 socket 现造一个。
        reader_cache_.reset(new srv::LineReader(s));
        return *reader_cache_;
    }

    std::uint16_t               control_port_;
    net::TcpListener            control_;
    net::TcpListener            data_listener_;
    std::thread                 th_;
    std::atomic<bool>           running_{false};
    std::atomic<int>            control_conns_{0};
    std::atomic<int>            commands_{0};
    std::atomic<int>            data_conns_{0};
    std::atomic<std::uint16_t>  last_data_port_{0};
    std::unique_ptr<srv::LineReader> reader_cache_;
    mutable std::mutex                 mu_;
    std::map<std::string, std::string> files_;
};

// =====================================================================
// T70 应答码解析
// =====================================================================
static void test_70_reply_code() {
    std::printf("T70 应答码解析\n");

    int code = 0;
    EXPECT(ftp::parse_code("220 Ready", code));      EXPECT_EQ(code, 220);
    EXPECT(ftp::parse_code("220-Ready", code));      EXPECT_EQ(code, 220);
    EXPECT(ftp::parse_code("227 Entering", code));   EXPECT_EQ(code, 227);
    EXPECT(ftp::parse_code("230", code));            EXPECT_EQ(code, 230);
    EXPECT(ftp::parse_code("500 x", code));          EXPECT_EQ(code, 500);
    EXPECT(ftp::parse_code("150 Opening", code));    EXPECT_EQ(code, 150);
    EXPECT(ftp::parse_code("226 Done", code));       EXPECT_EQ(code, 226);

    // 反向守卫：非三位码必须被拒
    EXPECT(!ftp::parse_code("", code));
    EXPECT(!ftp::parse_code("22", code));
    EXPECT(!ftp::parse_code("22x Ready", code));
    EXPECT(!ftp::parse_code("x20 Ready", code));

    // 多行判定：第 4 个字符是 '-'
    ftp::Reply r;
    r.lines.push_back("220-A");
    r.lines.push_back("220-B");
    r.lines.push_back("220 C");
    r.multiline = true;
    r.code = 220;
    EXPECT(r.is_2xx());
    EXPECT(!r.is_1xx());
    EXPECT_EQ(r.lines.size(), 3);
    EXPECT_STR_EQ(r.body(), std::string("A"));
    EXPECT(r.text().find("220-B") != std::string::npos);   // 续行留在 text 里

    ftp::Reply r1;
    r1.lines.push_back("150 Opening");
    r1.code = 150;
    EXPECT(r1.is_1xx());
    EXPECT(!r1.is_2xx());
    EXPECT_STR_EQ(r1.body(), std::string("Opening"));
    EXPECT(!r1.multiline);
}

// =====================================================================
// T71 ★ PASV 解析
// =====================================================================
static void test_71_pasv_parse() {
    std::printf("T71 PASV(227) 解析\n");

    std::string h;
    std::uint16_t p = 0;
    std::string err;

    // ★ 端口 = p1*256 + p2 = 7*256 + 138 = 1930
    EXPECT(ftp::parse_pasv("227 Entering Passive Mode (127,0,0,1,7,138)", h, p, err));
    EXPECT_STR_EQ(h, std::string("127.0.0.1"));
    EXPECT_EQ(p, 1930);
    EXPECT(err.empty());

    // ★ 这条才是"p1*256+p2 写成 p1*100+p2"的照妖镜：p2 >= 100 时两者不同
    EXPECT(ftp::parse_pasv("227 Entering Passive Mode (192,168,1,10,195,200)", h, p, err));
    EXPECT_STR_EQ(h, std::string("192.168.1.10"));
    EXPECT_EQ(p, 195 * 256 + 200);              // = 50120
    EXPECT_EQ(p, 50120);
    EXPECT(p != 195 * 100 + 200);               // 反向守卫：两种算法结果不同

    // 大端口（验证没有用 uint8 截断）
    EXPECT(ftp::parse_pasv("227 Entering Passive Mode (10,0,0,1,255,255)", h, p, err));
    EXPECT_EQ(p, 65535);

    // 容错：用空格/其他分隔符
    EXPECT(ftp::parse_pasv("227 Entering Passive Mode (127, 0, 0, 1, 4, 1)", h, p, err));
    EXPECT_STR_EQ(h, std::string("127.0.0.1"));
    EXPECT_EQ(p, 4 * 256 + 1);

    // 0.0.0.0（NAT 后的老服务器）
    EXPECT(ftp::parse_pasv("227 Entering Passive Mode (0,0,0,0,7,138)", h, p, err));
    EXPECT_STR_EQ(h, std::string("0.0.0.0"));

    // ---- 拒绝路径 ----
    err.clear();
    EXPECT(!ftp::parse_pasv("227 没有括号", h, p, err));
    EXPECT(!err.empty());
    err.clear();
    EXPECT(!ftp::parse_pasv("227 (127,0,0,1,7)", h, p, err));       // 只有 5 个数字
    EXPECT(!err.empty());
    err.clear();
    EXPECT(!ftp::parse_pasv("227 (127,0,0,1,7,138,9)", h, p, err)); // 7 个数字
    EXPECT(!err.empty());
    err.clear();
    EXPECT(!ftp::parse_pasv("227 (127,0,0,1,0,0)", h, p, err));     // ★ 端口 0 = 无效
    EXPECT(!err.empty());
    err.clear();
    EXPECT(!ftp::parse_pasv("227 (300,0,0,1,7,138)", h, p, err));   // 数字越界
    EXPECT(!err.empty());
    err.clear();
    EXPECT(!ftp::parse_pasv("227 ()", h, p, err));
    EXPECT(!err.empty());
    // 反向守卫：合法的必须过
    err.clear();
    EXPECT(ftp::parse_pasv("227 (127,0,0,1,7,138)", h, p, err));
    EXPECT(err.empty());
}

// =====================================================================
// T72 端到端登录（多行 banner）
// =====================================================================
static void test_72_login_multiline_banner() {
    std::printf("T72 端到端登录（多行 banner）\n");

    FakeFtpServer srv(2121);
    srv.put_file("ok.txt", "hello");
    std::string err;
    if (!srv.start(err)) {
        std::cerr << "  FAIL  起假 FTP 服务端失败：" << err << std::endl;
        ++g_fail;
        return;
    }

    ftp::FtpClient c;
    ftp::FtpClient::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 2121;
    cfg.timeout_ms = 3000;
    cfg.user = "ems";
    cfg.pass = "ems";
    EXPECT(c.connect(cfg, err));
    EXPECT(err.empty());
    EXPECT(c.connected());
    EXPECT(c.logged_in());

    // ★ 多行 banner 必须被**整条**读掉：3 行
    EXPECT_EQ(c.banner_lines(), 3);
    EXPECT_EQ(c.multiline_replies(), 1);        // banner 那一行算一次多行应答
    EXPECT_EQ(c.last_code(), 200);              // 最后一条是 TYPE I 的 200
    EXPECT_EQ(c.pasv_count(), 0);

    // ★ 关键：banner 的续行不能被留在流里当成下一条应答。
    //   若 read_reply 只读第一行，那么 USER 的应答会对上 banner 第二行（"220-..."），
    //   登录就会失败 —— 上面的 EXPECT(c.logged_in()) 已经把它钉住；
    //   这里再断言"确实发过 3 条命令"（USER/PASS/TYPE）来加强。
    EXPECT_EQ(c.commands_sent(), 3);
    // 反向守卫：服务端确实处理了 3 条命令（说明不是客户端自己在演）
    EXPECT_EQ(srv.commands(), 3);
    EXPECT(srv.control_connections() >= 1);

    EXPECT(c.quit());
    EXPECT(!c.connected());
    // ★ 必须先停掉 2121 上的服务端再起第二台：
    //   Windows 上 SO_REUSEADDR 允许"抢"一个**正在监听**的端口，
    //   两台服务端同时 listen(2121) 时客户端连到哪台是不确定的 ——
    //   那样 c2 会连回上面这台（仍是多行 banner），反证就变成假失败。
    srv.stop();

    // 若 banner 是单行，banner_lines 必须变成 1（反证上面不是在数命令）
    {
        FakeFtpServer s2(2121);
        s2.banner_multiline = false;
        std::string e2;
        if (s2.start(e2)) {
            ftp::FtpClient c2;
            std::string e3;
            EXPECT(c2.connect(cfg, e3));
            EXPECT_EQ(c2.banner_lines(), 1);
            EXPECT_EQ(c2.multiline_replies(), 0);
            c2.quit();
            s2.stop();
        } else {
            ++g_fail;
        }
    }
}

// =====================================================================
// T73 ★ 上传 STOR
// =====================================================================
static void test_73_store() {
    std::printf("T73 上传 STOR（逐位一致）\n");

    FakeFtpServer srv(2121);
    std::string err;
    if (!srv.start(err)) { ++g_fail; return; }

    ftp::FtpClient c;
    ftp::FtpClient::Config cfg;
    cfg.host = "127.0.0.1"; cfg.port = 2121; cfg.timeout_ms = 3000;
    cfg.user = "ems"; cfg.pass = "ems";
    EXPECT(c.connect(cfg, err));

    const std::string content = "EMS-SETTING-V1\r\nP_MAX=5000\r\nP_MIN=-5000\r\n";
    std::vector<std::uint8_t> data(content.begin(), content.end());

    EXPECT(c.store("setting.txt", data, err));
    EXPECT(err.empty());
    EXPECT(srv.has_file("setting.txt"));
    // ★ 服务端收到的字节必须与本地**逐位一致**
    EXPECT_EQ(srv.file("setting.txt").size(), content.size());
    EXPECT_STR_EQ(srv.file("setting.txt"), content);
    EXPECT(srv.data_connections() >= 1);
    EXPECT(c.pasv_count() >= 1);
    EXPECT(c.last_pasv_port() != 0);
    EXPECT_EQ(c.last_pasv_port(), srv.last_data_port());   // ★ PASV 解析的端口真的拿到了
    EXPECT_EQ(c.last_code(), 226);                         // 收尾在 226

    // 二进制内容（含 \0 与所有字节值）—— 证明我们没做任何"文本模式"转换
    {
        std::vector<std::uint8_t> bin;
        for (int i = 0; i < 256; ++i) bin.push_back((std::uint8_t)i);
        bin.push_back(0x00); bin.push_back(0x0D); bin.push_back(0x0A);
        bin.push_back(0x1A);   // DOS 的 EOF 字符：文本模式会在这里截断
        EXPECT(c.store("bin.dat", bin, err));
        EXPECT(err.empty());
        const std::string got = srv.file("bin.dat");
        EXPECT_EQ(got.size(), bin.size());
        bool same = (got.size() == bin.size());
        for (std::size_t i = 0; i < bin.size() && same; ++i) {
            if ((std::uint8_t)got[i] != bin[i]) same = false;
        }
        EXPECT(same);
        EXPECT_EQ((int)(std::uint8_t)got[got.size() - 1], 0x1A);   // ★ 末尾 0x1A 未被截断
    }

    c.quit();
    srv.stop();
}

// =====================================================================
// T74 ★ 下载 RETR
// =====================================================================
static void test_74_retrieve() {
    std::printf("T74 下载 RETR（逐位一致）\n");

    FakeFtpServer srv(2121);
    // 内容刻意带 \r\n 与 \0 —— 现场定值文件里有换行，用文本模式处理会变形
    // ★ 显式塞 NUL：证明我们没做任何"文本模式"处理（C 字符串 + 长度 12 会越界读）
    std::string content = "A\r\n";     // 0,1,2
    content.push_back('\0');            // 3
    content += "B\r\n";                // 4,5,6
    content.push_back('\0');            // 7
    content += "C\r\n";                // 8,9,10
    EXPECT_EQ(content.size(), std::size_t(11));
    srv.put_file("curve.csv", content);
    std::string err;
    if (!srv.start(err)) { ++g_fail; return; }

    ftp::FtpClient c;
    ftp::FtpClient::Config cfg;
    cfg.host = "127.0.0.1"; cfg.port = 2121; cfg.timeout_ms = 3000;
    cfg.user = "ems"; cfg.pass = "ems";
    EXPECT(c.connect(cfg, err));

    std::vector<std::uint8_t> out;
    EXPECT(c.retrieve("curve.csv", out, err));
    EXPECT(err.empty());
    EXPECT_EQ(out.size(), content.size());
    EXPECT_EQ(out[0], 'A');
    EXPECT_EQ(out[1], '\r');
    EXPECT_EQ(out[2], '\n');
    EXPECT_EQ(out[3], 0x00);                 // ★ NUL 必须原样保留
    EXPECT_EQ(out[7], 0x00);
    EXPECT_EQ(out[10], '\n');
    // 逐位对账
    {
        const std::string got((const char*)out.data(), out.size());
        EXPECT_STR_EQ(got, content);
    }

    // 空文件
    {
        srv.put_file("empty.dat", "");
        std::vector<std::uint8_t> e;
        std::string e2;
        EXPECT(c.retrieve("empty.dat", e, e2));
        EXPECT(e2.empty());
        EXPECT_EQ(e.size(), std::size_t(0));
    }

    // 同一连接里连续下载两个文件（PASV 每次都要重新协商）
    {
        srv.put_file("two.dat", "xx");
        std::vector<std::uint8_t> o2;
        std::string e3;
        EXPECT(c.retrieve("two.dat", o2, e3));
        EXPECT_EQ(o2.size(), std::size_t(2));
        EXPECT_EQ(o2[0], 'x');
        EXPECT(c.pasv_count() >= 3);        // store/retrieve 各一次 → 累计 ≥3
    }

    c.quit();
    srv.stop();
}

// =====================================================================
// T75 ★ 150/226 分段应答
// =====================================================================
static void test_75_split_replies() {
    std::printf("T75 150/226 分段应答\n");

    // ---- 正常：150 → 数据 → 226 ----
    {
        FakeFtpServer srv(2121);
        srv.put_file("f.dat", "0123456789");
        std::string err;
        if (!srv.start(err)) { ++g_fail; return; }
        ftp::FtpClient c;
        ftp::FtpClient::Config cfg;
        cfg.host = "127.0.0.1"; cfg.port = 2121; cfg.timeout_ms = 3000;
        cfg.user = "ems"; cfg.pass = "ems";
        EXPECT(c.connect(cfg, err));
        std::vector<std::uint8_t> out;
        std::string e2;
        EXPECT(c.retrieve("f.dat", out, e2));
        EXPECT(e2.empty());
        EXPECT_EQ(out.size(), std::size_t(10));
        // ★ 收尾必须停在第 226 条（说明两条应答都被读掉了，没留残留在流里）
        EXPECT_EQ(c.last_code(), 226);
        // 再发一条命令必须仍然同步（若 226 没读掉，这里会对上 226）
        c.quit();
        srv.stop();
    }
    // ---- ★ 服务端**不发** 226：retrieve() 必须失败，绝不能"数据到手就算成功" ----
    {
        FakeFtpServer srv(2121);
        srv.put_file("f.dat", "0123456789");
        srv.st_226 = false;
        std::string err;
        if (!srv.start(err)) { ++g_fail; return; }
        ftp::FtpClient c;
        ftp::FtpClient::Config cfg;
        cfg.host = "127.0.0.1"; cfg.port = 2121; cfg.timeout_ms = 400;
        cfg.user = "ems"; cfg.pass = "ems";
        EXPECT(c.connect(cfg, err));
        std::vector<std::uint8_t> out;
        std::string e2;
        const bool ok = c.retrieve("f.dat", out, e2);
        EXPECT(!ok);                       // ★ 半截定值不允许被当成成功
        EXPECT(!e2.empty());
        // 反向守卫：数据其实**已经**到了（10 字节），失败是"等 226 等不到"造成的，
        // 不是"没收到数据" —— 否则这条断言测的是别的东西
        EXPECT_EQ(out.size(), std::size_t(10));
        c.close();
        srv.stop();
    }
}

// =====================================================================
// T76 错误路径
// =====================================================================
static void test_76_error_paths() {
    std::printf("T76 错误路径\n");

    ftp::FtpClient::Config cfg;
    cfg.host = "127.0.0.1"; cfg.port = 2121; cfg.timeout_ms = 800;
    cfg.user = "ems"; cfg.pass = "ems";

    // ① USER 被拒
    {
        FakeFtpServer srv(2121);
        srv.reject_user = true;
        std::string e; EXPECT(srv.start(e));
        ftp::FtpClient c;
        std::string e2;
        EXPECT(!c.connect(cfg, e2));
        EXPECT(!e2.empty());
        EXPECT(!c.logged_in());
        c.close();
        srv.stop();
    }
    // ② PASS 被拒
    {
        FakeFtpServer srv(2121);
        srv.reject_pass = true;
        std::string e; EXPECT(srv.start(e));
        ftp::FtpClient c;
        std::string e2;
        EXPECT(!c.connect(cfg, e2));
        EXPECT(!e2.empty());
        c.close();
        srv.stop();
    }
    // ③ 未登录就 RETR
    {
        ftp::FtpClient c;
        std::vector<std::uint8_t> out;
        std::string e;
        EXPECT(!c.retrieve("x", out, e));
        EXPECT(!e.empty());
        EXPECT(!c.connected());
    }
    // ④ 控制口连不上（没有人监听）
    {
        ftp::FtpClient c;
        ftp::FtpClient::Config bad = cfg;
        bad.port = 2199;
        bad.timeout_ms = 400;
        std::string e;
        EXPECT(!c.connect(bad, e));
        EXPECT(!e.empty());
        EXPECT(!c.connected());
    }
    // ⑤ PASV 回了一条不是 227 的应答
    {
        FakeFtpServer srv(2121);
        srv.pasv_garbage = true;
        std::string e; EXPECT(srv.start(e));
        ftp::FtpClient c;
        std::string e2;
        EXPECT(c.connect(cfg, e2));
        std::vector<std::uint8_t> out;
        std::string e3;
        EXPECT(!c.retrieve("x", out, e3));
        EXPECT(!e3.empty());
        c.close();
        srv.stop();
    }
    // ⑥ RETR 回 450（不是 150）
    {
        FakeFtpServer srv(2121);
        srv.retr_wrong_code = true;
        srv.put_file("x", "abc");
        std::string e; EXPECT(srv.start(e));
        ftp::FtpClient c;
        std::string e2;
        EXPECT(c.connect(cfg, e2));
        std::vector<std::uint8_t> out;
        std::string e3;
        EXPECT(!c.retrieve("x", out, e3));
        EXPECT(!e3.empty());
        EXPECT_EQ(out.size(), std::size_t(0));
        c.close();
        srv.stop();
    }
    // ⑦ 文件不存在 → 550
    {
        FakeFtpServer srv(2121);
        std::string e; EXPECT(srv.start(e));
        ftp::FtpClient c;
        std::string e2;
        EXPECT(c.connect(cfg, e2));
        std::vector<std::uint8_t> out;
        std::string e3;
        EXPECT(!c.retrieve("no-such-file", out, e3));
        EXPECT(!e3.empty());
        EXPECT(e3.find("550") != std::string::npos || e3.find("150") != std::string::npos);
        c.close();
        srv.stop();
    }
    // ⑧ PASV 回 0.0.0.0 → 客户端改回用控制连接的地址（NAT 场景）
    {
        FakeFtpServer srv(2121);
        srv.data_host_zero = true;
        srv.put_file("n.dat", "NAT");
        std::string e; EXPECT(srv.start(e));
        ftp::FtpClient c;
        std::string e2;
        EXPECT(c.connect(cfg, e2));
        std::vector<std::uint8_t> out;
        std::string e3;
        EXPECT(c.retrieve("n.dat", out, e3));      // ★ 仍然应当成功
        EXPECT(e3.empty());
        EXPECT_EQ(out.size(), std::size_t(3));
        EXPECT_STR_EQ(c.last_pasv_host(), std::string("127.0.0.1"));   // 已被替换
        c.close();
        srv.stop();
    }
}

// =====================================================================
// T77 大文件往返
// =====================================================================
static void test_77_large_binary() {
    std::printf("T77 大文件往返（64 KiB）\n");

    FakeFtpServer srv(2121);
    std::string err;
    if (!srv.start(err)) { ++g_fail; return; }

    // 伪随机（确定性）：保证测试可复现
    std::vector<std::uint8_t> big(64 * 1024);
    std::uint32_t s = 0x12345678u;
    for (std::size_t i = 0; i < big.size(); ++i) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        big[i] = (std::uint8_t)(s & 0xFF);
    }

    ftp::FtpClient c;
    ftp::FtpClient::Config cfg;
    cfg.host = "127.0.0.1"; cfg.port = 2121; cfg.timeout_ms = 5000;
    cfg.user = "ems"; cfg.pass = "ems";
    EXPECT(c.connect(cfg, err));

    // 上传
    std::string e1;
    EXPECT(c.store("big.bin", big, e1));
    EXPECT(e1.empty());
    const std::string stored = srv.file("big.bin");
    EXPECT_EQ(stored.size(), big.size());
    bool same = (stored.size() == big.size());
    std::size_t first_diff = 0;
    for (std::size_t i = 0; i < big.size() && same; ++i) {
        if ((std::uint8_t)stored[i] != big[i]) { same = false; first_diff = i; }
    }
    EXPECT(same);
    if (!same) std::cerr << "    首个不同字节在偏移 " << first_diff << std::endl;

    // 下载回本地并逐位对账
    std::vector<std::uint8_t> back;
    std::string e2;
    EXPECT(c.retrieve("big.bin", back, e2));
    EXPECT(e2.empty());
    EXPECT_EQ(back.size(), big.size());
    bool same2 = (back.size() == big.size());
    for (std::size_t i = 0; i < big.size() && same2; ++i) {
        if (back[i] != big[i]) same2 = false;
    }
    EXPECT(same2);

    c.quit();
    srv.stop();
}

// =====================================================================
// T78 空文件 + 多文件 + 收尾
// =====================================================================
static void test_78_misc() {
    std::printf("T78 空文件与多文件\n");

    FakeFtpServer srv(2121);
    std::string err;
    if (!srv.start(err)) { ++g_fail; return; }

    ftp::FtpClient c;
    ftp::FtpClient::Config cfg;
    cfg.host = "127.0.0.1"; cfg.port = 2121; cfg.timeout_ms = 3000;
    cfg.user = "ems"; cfg.pass = "ems";
    EXPECT(c.connect(cfg, err));

    // 上传空文件
    std::vector<std::uint8_t> empty;
    std::string e1;
    EXPECT(c.store("zero.dat", empty, e1));
    EXPECT(e1.empty());
    EXPECT(srv.has_file("zero.dat"));
    EXPECT_EQ(srv.file("zero.dat").size(), std::size_t(0));

    // 覆盖上传（同名第二次应当改内容）
    std::vector<std::uint8_t> v1, v2;
    v1.push_back('1'); v2.push_back('2'); v2.push_back('2');
    std::string e2, e3;
    EXPECT(c.store("same.dat", v1, e2));
    EXPECT_EQ(srv.file("same.dat").size(), std::size_t(1));
    EXPECT(c.store("same.dat", v2, e3));
    EXPECT_EQ(srv.file("same.dat").size(), std::size_t(2));
    EXPECT_EQ(srv.file("same.dat")[0], '2');

    // 事务记录（诊断用）
    EXPECT(c.transcript().size() > 0);
    EXPECT(c.commands_sent() >= 5);

    EXPECT(c.quit());
    EXPECT(!c.connected());
    // QUIT 之后再用必须失败
    std::vector<std::uint8_t> o;
    std::string e4;
    EXPECT(!c.retrieve("same.dat", o, e4));
    EXPECT(!e4.empty());

    srv.stop();
}

// =====================================================================
int main() {
    std::printf("=== 16/ FTP 定值下发通道 单元测试 ===\n\n");

    test_70_reply_code();
    test_71_pasv_parse();
    test_72_login_multiline_banner();
    test_73_store();
    test_74_retrieve();
    test_75_split_replies();
    test_76_error_paths();
    test_77_large_binary();
    test_78_misc();

    std::printf("\n");
    if (g_fail == 0) std::printf("ALL TESTS PASSED\n");
    // 本用例没有"跳过"路径，恒为 0；仍按约定打出来，
    // 让每层的状态行格式一致（见 新模块开发约定.md §4.5）
    std::printf("PASS=%d FAIL=%d SKIPPED=0\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
