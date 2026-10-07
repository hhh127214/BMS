// =====================================================================
// 16/ — FTP 定值下发通道（FTP 客户端 + 极简被动模式）
//
// 为什么 EMS 需要 FTP：现场"定值"（保护定值表、PCS 参数组、电价曲线）
// 一般不是逐点用 Modbus 写的，而是**整表文件**下发 —— 设备侧只提供一个
// FTP 账号，让 EMS 把 `setting.dat` / `curve.csv` 推上去，再让装置自己加载。
// 于是 EMS 需要**一个够用的 FTP 客户端**，而不是"打开浏览器上传"。
//
// 范围刻意收窄（只做现场真正用到的 7 条命令）：
//   USER / PASS / TYPE I / PASV / RETR / STOR / QUIT
// 不做 PORT（主动模式）：现场工控机在 NAT/防火墙后面，主动模式基本不通；
// 而且被动模式只需要客户端发一个连接，实现量小一半。
//
// ── 三个必须显式处理的坑 ────────────────────────────────────────────
//
// ★① **多行应答**。服务器可以用 `code-文本` 连续若干行，最后以
//    `code<空格>文本` 收尾。典型的入场 banner 就是多行的：
//        220-EMS FTP server
//        220-仅供内部使用
//        220 Ready
//    只看第一行就 return 的实现，会把后两行**留在流里** ——
//    下一次 read_reply 读到的就是 banner 的第二行，
//    于是整个命令/应答序列永久错位一格。现场表现是"登录成功但后面
//    每条命令都回奇怪的东西"，且**不报错**。本文件的 read_reply()
//    必须读到"同码 + 空格"的终止行才算一条应答读完。
//
// ★② **150 / 226 分段应答**。数据命令（RETR/STOR）不是一条应答，
//    而是**两条**：先 150（"要开始传了"），传完后 226（"传完了"）。
//    只等一条应答的实现会在"数据还没传完"时就返回，
//    调用方拿到的是**半截文件**而没有任何错误 —— 定值下发半截比不传更危险。
//    正确顺序（被动模式）：
//      PASV → 227 → 连数据口 → RETR → 150 → 收数据到 EOF → 226
//
// ★③ **227 应答里的数据口地址**。格式是 `227 ...(h1,h2,h3,h4,p1,p2)`，
//    端口 = p1*256 + p2。两个常见陷阱：
//      · p1*256 + p2 写成 p1*100 + p2（十进制直觉）→ 只在 p2 >= 100 时错，
//        也就是说**大多数时候是对的**，偶发才算错 —— 最难查的一类；
//      · 端口 0 是"未定义"，必须显式报错而不是拿去 connect（会连到 0 端口）。
//    还有服务器回 `(0,0,0,0,x,y)` 表示"用控制连接的地址"，本文件按这条实现。
//
// 编译：纯头文件。Windows 侧需要 `-lws2_32`。
// =====================================================================

#pragma once

#include "net_base.h"

#include <cstdio>
#include <string>
#include <vector>

namespace ems {
namespace comm {
namespace ftp {

// =====================================================================
// 一条 FTP 应答（可能由多行组成）
// =====================================================================
struct Reply {
    int                      code      = 0;
    std::vector<std::string> lines;      // 首行 + 续行（原样，不含 CRLF）
    bool                     multiline = false;

    // 多行拼成一行（日志/错误信息用）
    std::string text() const {
        std::string s;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i) s += " | ";
            s += lines[i];
        }
        return s;
    }
    // 去掉三位码与前导分隔符后的正文
    std::string body() const {
        if (lines.empty() || lines[0].size() <= 4) return std::string();
        return lines[0].substr(4);
    }
    bool is_2xx() const { return code >= 200 && code < 300; }
    bool is_1xx() const { return code >= 100 && code < 200; }
    bool is_3xx() const { return code >= 300 && code < 400; }
};

inline bool parse_code(const std::string& line, int& code) {
    if (line.size() < 3) return false;
    for (int i = 0; i < 3; ++i) {
        if (line[(std::size_t)i] < '0' || line[(std::size_t)i] > '9') return false;
    }
    code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    return true;
}

// ---------------------------------------------------------------------
// ★ 读一条完整应答（多行 + 半包安全）
//
// 半包在这里同样是真问题：TCP 是字节流，"220 Ready\r\n331 ...\r\n" 可能
// 一次 recv 全拿到（串包），也可能只拿到 "220 Re"（半包）。
// net::TcpSocket::recv_line() 自带行缓冲，两种情况都能正确切行。
// ---------------------------------------------------------------------
inline bool read_reply(net::TcpSocket& s, Reply& out, std::string& err,
                       int timeout_ms) {
    out = Reply();
    std::string line;
    if (!s.recv_line(line, timeout_ms)) {
        err = "读取 FTP 应答失败：" + s.error();
        return false;
    }
    int code = 0;
    if (!parse_code(line, code)) {
        err = "FTP 应答不是合法的三位码：" + line;
        return false;
    }
    out.code = code;
    out.lines.push_back(line);

    // 第 4 个字符是 '-' → 多行应答开始
    const bool multi = (line.size() >= 4 && line[3] == '-');
    out.multiline = multi;
    if (!multi) return true;

    // ★ 一直读到 "同码 + 空格（或只有码）" 的终止行。
    //   不这么做，续行会留在流里，把后面所有命令的应答错位一格（坑①）。
    for (int guard = 0; guard < 10000; ++guard) {
        if (!s.recv_line(line, timeout_ms)) {
            err = "多行应答未收全（" + s.error() + "）：已收 " +
                  std::to_string((unsigned long long)out.lines.size()) + " 行";
            return false;
        }
        out.lines.push_back(line);
        int c2 = 0;
        if (parse_code(line, c2) && c2 == code) {
            // 终止行 = "code SP ..." 或恰好只有 "code"
            if (line.size() == 3 || line[3] == ' ') return true;
            // "code-" 是继续行，接着读
        }
    }
    err = "多行应答超过 10000 行，判定为服务端异常";
    return false;
}

// =====================================================================
// ★ 解析 227 被动模式应答
//   227 Entering Passive Mode (127,0,0,1,7,138)
//   端口 = 7*256 + 138 = 1930
// =====================================================================
inline bool parse_pasv(const std::string& text, std::string& host,
                       std::uint16_t& port, std::string& err) {
    const std::size_t l = text.find('(');
    const std::size_t r = (l == std::string::npos) ? std::string::npos
                                                   : text.find(')', l + 1);
    if (l == std::string::npos || r == std::string::npos || r <= l + 1) {
        err = "227 应答里找不到 (h1,h2,h3,h4,p1,p2) 段：" + text;
        return false;
    }
    // 用"扫数字"而不是 split(',')：现场服务器分隔符有 ','、' , '、'.' 等变体
    std::vector<int> nums;
    int cur = -1;
    const std::string seg = text.substr(l + 1, r - l - 1);
    for (std::size_t i = 0; i < seg.size(); ++i) {
        const char c = seg[i];
        if (c >= '0' && c <= '9') {
            cur = (cur < 0 ? 0 : cur) * 10 + (c - '0');
            if (cur > 100000) { err = "227 应答里的数字过大（分段异常）"; return false; }
        } else if (cur >= 0) {
            nums.push_back(cur);
            cur = -1;
        }
    }
    if (cur >= 0) nums.push_back(cur);

    if (nums.size() != 6) {
        err = "227 应答应有 6 个数字（h1,h2,h3,h4,p1,p2），实际 " +
              std::to_string((unsigned long long)nums.size()) + " 个：" + seg;
        return false;
    }
    for (std::size_t i = 0; i < nums.size(); ++i) {
        if (nums[i] < 0 || nums[i] > 255) {
            err = "227 应答里的数字越界（必须在 0..255）：" + seg;
            return false;
        }
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
                  nums[0], nums[1], nums[2], nums[3]);
    host = buf;
    // ★ 端口必须按 256 进制算。写成 p1*100 + p2 时，p2 < 100 的情况
    //   恰好也對，只有 p2 >= 100 才错 —— 偶发故障的典型来源。
    port = (std::uint16_t)(nums[4] * 256 + nums[5]);
    if (port == 0) {
        // 0 是"未定义端口"，拿去 connect 会失败在别处、报错信息还没线索
        err = "227 应答给出的数据端口为 0（无效）";
        return false;
    }
    return true;
}

// =====================================================================
// FTP 客户端
// =====================================================================
class FtpClient {
public:
    struct Config {
        std::string   host       = "127.0.0.1";
        std::uint16_t port       = 21;
        int           timeout_ms = 5000;
        std::string   user       = "anonymous";
        std::string   pass       = "ems@example.com";
        // 数据通道的最大接收字节数（防"服务器一直发"把内存吃光）
        std::size_t   max_data_bytes = 64u * 1024u * 1024u;
    };

    FtpClient() {}
    ~FtpClient() { close(); }
    FtpClient(const FtpClient&) = delete;
    FtpClient& operator=(const FtpClient&) = delete;

    // 建立控制连接 + 读 banner + 登录 + TYPE I
    bool connect(const Config& cfg, std::string& err) {
        cfg_ = cfg;
        transcript_.clear();
        if (!ctrl_.connect_to(cfg.host, cfg.port, cfg.timeout_ms, err)) {
            return false;
        }
        // ★ 入场 banner：**必须**在这里读掉，而且是完整读掉（见坑①）
        Reply banner;
        if (!read_reply(ctrl_, banner, err, cfg.timeout_ms)) return false;
        note_reply(banner);
        if (banner.code != 220) {
            err = "控制连接 banner 不是 220：" + banner.text();
            return false;
        }
        banners_ = (int)banner.lines.size();

        Reply r;
        if (!cmd("USER", cfg.user, r, err)) return false;
        // 331 = 需要密码；230 = 匿名直接通过
        if (r.code != 331 && r.code != 230) {
            err = "USER 被拒（" + std::to_string(r.code) + "）：" + r.text();
            return false;
        }
        if (r.code == 331) {
            if (!cmd("PASS", cfg.pass, r, err)) return false;
            if (r.code != 230) {
                err = "PASS 被拒（" + std::to_string(r.code) + "）：" + r.text();
                return false;
            }
        }
        if (!cmd("TYPE", "I", r, err)) return false;
        if (!r.is_2xx()) {
            err = "TYPE I 被拒（" + std::to_string(r.code) + "）：" + r.text();
            return false;
        }
        logged_in_ = true;
        return true;
    }

    bool connected() const { return ctrl_.is_open(); }
    bool logged_in() const { return logged_in_; }

    // -----------------------------------------------------------------
    // 下载：PASV → RETR → 150 → 收数据到 EOF → 226
    // -----------------------------------------------------------------
    bool retrieve(const std::string& remote, std::vector<uint8_t>& out,
                  std::string& err) {
        out.clear();
        if (!guard(err)) return false;

        std::string dhost;
        std::uint16_t dport = 0;
        if (!open_passive(dhost, dport, err)) return false;

        net::TcpSocket data;
        if (!data.connect_to(dhost, dport, cfg_.timeout_ms, err)) {
            err = "数据连接失败（" + dhost + ":" + std::to_string((unsigned)dport) +
                  "）：" + err;
            return false;
        }

        Reply r;
        if (!cmd("RETR", remote, r, err)) return false;
        if (!r.is_1xx()) {
            err = "RETR 未返回 150（" + std::to_string(r.code) + "）：" + r.text();
            return false;
        }

        // ★ 顺序是硬的：**先**把数据读到 EOF（服务器靠关数据口表示传完），
        //   **再**读 226。反过来（等 226 再收数据）会死锁 —— 服务器
        //   在客户端把数据收走之前不会发 226。
        const int rc = data.recv_until_eof(out, cfg_.max_data_bytes);
        if (rc < 0) { err = "接收数据失败：" + data.error(); return false; }
        if (rc == 2) { err = "数据超过上限，已截断（可能不是文件而是流）"; return false; }

        Reply done;
        if (!read_reply(ctrl_, done, err, cfg_.timeout_ms * 4)) return false;
        note_reply(done);
        if (done.code != 226) {
            err = "数据传完后未收到 226（" + std::to_string(done.code) + "）：" + done.text();
            return false;
        }
        return true;
    }

    // -----------------------------------------------------------------
    // 上传：PASV → STOR → 150 → 发数据 → 关数据口 → 226
    // -----------------------------------------------------------------
    bool store(const std::string& remote, const std::vector<uint8_t>& data,
               std::string& err) {
        if (!guard(err)) return false;

        std::string dhost;
        std::uint16_t dport = 0;
        if (!open_passive(dhost, dport, err)) return false;

        net::TcpSocket dsock;
        if (!dsock.connect_to(dhost, dport, cfg_.timeout_ms, err)) {
            err = "数据连接失败（" + dhost + ":" + std::to_string((unsigned)dport) +
                  "）：" + err;
            return false;
        }

        Reply r;
        if (!cmd("STOR", remote, r, err)) return false;
        if (!r.is_1xx()) {
            err = "STOR 未返回 150（" + std::to_string(r.code) + "）：" + r.text();
            return false;
        }

        if (!data.empty() && !dsock.send_all(data.data(), data.size())) {
            err = "发送数据失败：" + dsock.error();
            return false;
        }
        // ★ 必须显式关掉数据口（发 FIN）：服务器靠"数据连接 EOF"来判定
        //   "文件传完了"。只 shutdown 不 close 在某些服务器上会让它一直等。
        dsock.shutdown_send();
        dsock.close();

        Reply done;
        if (!read_reply(ctrl_, done, err, cfg_.timeout_ms * 4)) return false;
        note_reply(done);
        if (done.code != 226) {
            err = "上传后未收到 226（" + std::to_string(done.code) + "）：" + done.text();
            return false;
        }
        return true;
    }

    bool quit() {
        if (!ctrl_.is_open()) return true;
        Reply r;
        std::string err;
        cmd("QUIT", "", r, err);   // 221；失败也不影响本端关闭
        close();
        return true;
    }

    void close() {
        ctrl_.close();
        logged_in_ = false;
    }

    // -----------------------------------------------------------------
    // 可观测
    // -----------------------------------------------------------------
    const std::vector<std::string>& transcript() const { return transcript_; }
    const Reply& last_reply() const { return last_; }
    int  last_code() const { return last_.code; }
    int  multiline_replies() const { return multiline_replies_; }
    int  pasv_count() const { return pasv_count_; }
    int  banner_lines() const { return banners_; }
    std::uint16_t last_pasv_port() const { return last_pasv_port_; }
    const std::string& last_pasv_host() const { return last_pasv_host_; }
    int  commands_sent() const { return commands_sent_; }

private:
    bool guard(std::string& err) const {
        if (!ctrl_.is_open()) { err = "控制连接未建立"; return false; }
        if (!logged_in_)      { err = "尚未登录"; return false; }
        return true;
    }

    void log(const Reply& r) {
        for (std::size_t i = 0; i < r.lines.size(); ++i) {
            transcript_.push_back("< " + r.lines[i]);
        }
    }

    // 收到一条应答后统一记账：落 transcript、计多行、更新"最后一条应答"。
    // ★ 三条路径（banner / cmd / 数据传完的收尾应答）必须都走这里，
    //   否则 last_code() 会停在 150 而实际已经收到 226 —— 可观测值骗人。
    void note_reply(const Reply& r) {
        log(r);
        if (r.multiline) ++multiline_replies_;
        last_ = r;
    }

    bool cmd(const std::string& name, const std::string& arg, Reply& r,
             std::string& err) {
        std::string line = name;
        if (!arg.empty()) line += " " + arg;
        transcript_.push_back("> " + line);
        ++commands_sent_;
        std::string wire = line + "\r\n";
        if (!ctrl_.send_all(wire)) {
            err = "发送命令失败：" + ctrl_.error();
            return false;
        }
        if (!read_reply(ctrl_, r, err, cfg_.timeout_ms)) return false;
        note_reply(r);
        return true;
    }

    // PASV → 解析 → 打开数据监听/连接信息
    bool open_passive(std::string& host, std::uint16_t& port, std::string& err) {
        Reply r;
        if (!cmd("PASV", "", r, err)) return false;
        ++pasv_count_;
        if (r.code != 227) {
            err = "PASV 未返回 227（" + std::to_string(r.code) + "）：" + r.text();
            return false;
        }
        if (!parse_pasv(r.text(), host, port, err)) return false;

        // ★ 服务器回 0.0.0.0 时（NAT 后的老服务器常见）必须改用**控制连接**
        //   的地址，否则会去连 0.0.0.0 而失败。
        if (host == "0.0.0.0") host = cfg_.host;

        last_pasv_host_ = host;
        last_pasv_port_ = port;
        return true;
    }

    Config        cfg_;
    net::TcpSocket ctrl_;

    bool logged_in_   = false;
    int  multiline_replies_ = 0;
    int  pasv_count_  = 0;
    int  banners_     = 0;
    int  commands_sent_ = 0;
    std::uint16_t last_pasv_port_ = 0;
    std::string   last_pasv_host_;

    Reply                    last_;
    std::vector<std::string> transcript_;
};

}  // namespace ftp
}  // namespace comm
}  // namespace ems
