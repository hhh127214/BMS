// =====================================================================
// 16/ — 传输层安全（TLS）
//
// ★★ 本文件的第一原则：**不假装成功**。★★
//
// 为什么把这句话放在最前面：TLS 与别的模块不同，它有"看起来能用"的
// 中间态 —— 握手走完了、字节能收发，但**证书没校验**。那种实现比
// 完全不支持 TLS 危险得多，因为它会让人以为通道是可信的，
// 从而敢把定值下发、把遥控命令放上去。
//
// 所以本文件的结构是：
//   · `Backend::kSchannel` —— Windows 上**真的**走 SChannel（系统自带，
//     零第三方依赖，只需 -lsecur32 -lcrypt32）。
//   · `Backend::kNone`     —— **显式拒绝**：connect() 失败，
//     `last_error()` 给出中文原因，绝不静默退化成明文。
//   · 两条路径都被测试钉住（见 tests/test_tls.cpp T80~T89）。
//
// 证书校验的默认值是 **ON**，而且这一点本身有断言：
//   · `insecure_skip_verify == false` 是默认 —— 没有"顺手就不校验"；
//   · T84 拿自签证书去打"默认校验模式"，断言**握手必须失败**
//     （SChannel 回 SEC_E_UNTRUSTED_ROOT）。这一条是"校验真的开着"
//     的**唯一**正面证据：如果实现里悄悄加了 MANUAL_CRED_VALIDATION，
//     这条断言会立刻变红。
//
// ── 为什么选 SChannel 而不是 OpenSSL ────────────────────────────────
//   · SChannel 是**操作系统组件**，随 Windows 一起到场。工程纪律是
//     "零第三方依赖"（唯一例外 P3 的 lib60870），SChannel 不破坏它。
//   · OpenSSL 会引入两个**非技术**决策：① 许可（OpenSSL 3.x 是
//     Apache-2.0，与 1.x 的双许可不同，需要法务确认）；② 供应链
//     （多一个需要打补丁的原生库，现场量产机器上要跟 CVE）。
//   · 代价（诚实列出）：SChannel 的实现是 Windows 专有，Linux 构建下
//     本模块只能 `Backend::kNone`；且它的 API 是 SSPI 那套
//     "InitializeSecurityContext 循环 + EncryptMessage/DecryptMessage"，
//     比 OpenSSL 的 BIO 抽象啰嗦得多。
//
// ── 本文件刻意**不做**的事（见 docs/README.md §7）─────────────────
//   · 不做服务端 TLS（EMS 是客户端）
//   · 不做会话恢复 / 重新协商（SEC_I_RENEGOTIATE 直接报错）
//   · 不做客户端证书（mTLS）
//   · 不做 OCSP/CRL 吊销检查的显式控制（交给系统默认）
// =====================================================================

#pragma once

#include "net_base.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
// ★ 顺序：必须先有 <windows.h>（MinGW 的 <security.h> 依赖它），
//   而 <winsock2.h> 更要在 <windows.h> 之前 —— 这条链由 net_base.h 保证。
#  ifndef SECURITY_WIN32
#    define SECURITY_WIN32
#  endif
#  include <security.h>
#  include <schannel.h>
#  include <sspi.h>
// MinGW-w64（g++ 8.1 自带那一版）的 schannel.h 只到 TLS1.0，
// SP_PROT_TLS1_1_CLIENT / SP_PROT_TLS1_2_CLIENT / SP_PROT_TLS1_3_CLIENT
// 都没有定义。值取自 winnt.h/官方文档，自己补。
// ★ 这几个位是**位标志**，不是版本序号 —— 见 read_negotiated_info() 的注释。
#  ifndef SP_PROT_TLS1_1_CLIENT
#    define SP_PROT_TLS1_1_CLIENT 0x00000200
#  endif
#  ifndef SP_PROT_TLS1_2_CLIENT
#    define SP_PROT_TLS1_2_CLIENT 0x00000800
#  endif
#  ifndef SP_PROT_TLS1_3_CLIENT
#    define SP_PROT_TLS1_3_CLIENT 0x00002000
#  endif
#endif

namespace ems {
namespace comm {
namespace tls {

// =====================================================================
// 统一传输抽象
//
// ★ 语义必须与 net::TcpSocket 完全一致（见 net_base.h 的类头约定）：
//     recv() > 0 → 字节数
//     recv() = 0 → 对端**正常关闭**
//     recv() < 0 → 出错或超时（last_error() 给中文原因）
//   TLS 与明文在这里**不允许**有两种解释，否则上层没法写一套代码。
// =====================================================================
class ITransport {
public:
    virtual ~ITransport() {}

    virtual bool connect() = 0;
    virtual void close()   = 0;
    virtual int  send(const std::uint8_t* p, std::size_t n) = 0;
    virtual int  recv(std::uint8_t* p, std::size_t n) = 0;
    virtual bool is_up() const = 0;

    // 以下是**有默认实现**的附加能力：不强制实现者提供，
    // 但两处都提供 —— 出问题时日志里必须有线索。
    virtual std::string        endpoint() const { return std::string(); }
    virtual const std::string& last_error() const { return last_error_; }
    virtual bool               is_encrypted() const { return false; }
    virtual const char*        name() const { return "ITransport"; }

protected:
    std::string last_error_;
};

// =====================================================================
// 明文传输（TCP）
//
// 为什么需要一个"明显不是 TLS"的对照实现：
//   ① 现场仍有走内网/专线的设备只开明文口（很多 PCS 的 502 只认明文）；
//   ② 它是 `TlsTransport` 的**对照组** —— 测试里"接明文 echo 服务端
//      返回 is_encrypted()==false、接 TLS 服务端返回 true"这一对
//      才说明 is_encrypted() 不是个恒真/恒假的常量。
// =====================================================================
class PlainTransport : public ITransport {
public:
    PlainTransport(std::string host, std::uint16_t port, int timeout_ms = 3000)
        : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms) {}

    bool connect() override {
        close();
        const bool ok = sock_.connect_to(host_, port_, timeout_ms_, last_error_);
        if (!ok) last_error_ = "明文 TCP 连接失败：" + last_error_;
        return ok;
    }

    void close() override {
        sock_.close();
    }

    int send(const std::uint8_t* p, std::size_t n) override {
        if (!sock_.is_open()) { last_error_ = "明文连接未建立"; return -1; }
        if (!sock_.send_all(p, n)) { last_error_ = sock_.error(); return -1; }
        return (int)n;
    }

    int recv(std::uint8_t* p, std::size_t n) override {
        if (!sock_.is_open()) { last_error_ = "明文连接未建立"; return -1; }
        const int rc = sock_.recv_some(p, n);
        if (rc < 0) last_error_ = sock_.error();
        return rc;
    }

    bool is_up() const override { return sock_.is_open(); }
    std::string endpoint() const override {
        return host_ + ":" + std::to_string((unsigned)port_) + "（明文）";
    }
    bool        is_encrypted() const override { return false; }
    const char* name() const override { return "PlainTransport"; }

private:
    std::string    host_;
    std::uint16_t  port_;
    int            timeout_ms_;
    net::TcpSocket sock_;
};

// =====================================================================
// TLS 后端选择
// =====================================================================
enum class Backend {
    kDefault,    // 用本平台可用的最好后端（Windows → SChannel）
    kSchannel,   // 显式要求 SChannel
    kNone,       // 显式**不要**任何后端 → connect() 必须失败（诚实拒绝）
};

// "没有后端"时的中文原因。它同时是**文档**和**断言目标**：
// 测试直接拿这个串做相等断言，所以不能随手改文案。
inline const std::string& missing_backend_reason() {
    static const std::string s =
        "未编译 TLS 支持：零第三方依赖下需接入 SChannel/OpenSSL"
        "（Windows 上 SChannel 是系统组件，接入步骤见 16/docs/README.md §6；"
        "OpenSSL 需先过许可与供应链决策，见同节）";
    return s;
}

inline bool schannel_compiled_in() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

inline Backend resolve_backend(Backend b) {
    if (b == Backend::kDefault) return schannel_compiled_in() ? Backend::kSchannel : Backend::kNone;
    if (b == Backend::kSchannel && !schannel_compiled_in()) return Backend::kNone;
    return b;
}

inline const char* backend_name(Backend b) {
    switch (resolve_backend(b)) {
        case Backend::kSchannel: return "schannel";
        case Backend::kNone:     return "none";
        default:                 return "none";
    }
}

// 把 SECURITY_STATUS 翻成中文 —— **未收录的码也要原样打出来**，
// 否则现场只会看到"握手失败"四个字，完全没法查。
inline std::string sec_status_text(long s) {
    switch ((unsigned long)s) {
        case 0x00000000UL: return "SEC_E_OK（成功）";
        case 0x00090312UL: return "SEC_I_CONTINUE_NEEDED（握手中间态，正常）";
        case 0x80090318UL: return "SEC_E_INCOMPLETE_MESSAGE（TLS 记录不完整，需再读）";
        case 0x00090317UL: return "SEC_I_CONTEXT_EXPIRED（对端发了 close_notify，正常关闭）";
        case 0x80090325UL:
            return "SEC_E_UNTRUSTED_ROOT（证书链根 CA 不在本机信任库；自签证书必然如此）";
        case 0x80090322UL: return "SEC_E_WRONG_PRINCIPAL（证书主机名与连接目标不符）";
        case 0x80090326UL:
            return "SEC_E_ILLEGAL_MESSAGE（TLS 报文非法：常见于对 TLS 端口发明文，或反之）";
        case 0x80090330UL: return "SEC_E_DECRYPT_FAILURE（解密失败：记录被篡改或序号失步）";
        case 0x80090308UL: return "SEC_E_INVALID_TOKEN（报文格式非法，可能对端不是 TLS）";
        case 0x8009030EUL: return "SEC_E_NO_CREDENTIALS（没有可用凭据）";
        case 0x80090302UL: return "SEC_E_UNSUPPORTED_FUNCTION（协议栈不支持该功能）";
        case 0x80090331UL: return "SEC_E_ALGORITHM_MISMATCH（算法协商不一致，老服务器只支持弱算法）";
        case 0x8009030FUL: return "SEC_E_MESSAGE_ALTERED（报文被篡改）";
        case 0x80090310UL: return "SEC_E_OUT_OF_SEQUENCE（TLS 序号失步）";
        case 0x80090328UL: return "SEC_E_CERT_EXPIRED（证书已过期）";
        case 0x00090321UL: return "SEC_I_RENEGOTIATE（对端要求重新协商，本模块不支持）";
        case 0x80090304UL: return "SEC_E_INTERNAL_ERROR（SChannel 内部错误）";
        case 0x80090321UL: return "SEC_E_BUFFER_TOO_SMALL（输出缓冲过小）";
        default: break;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "SECURITY_STATUS=0x%08lX（未收录）",
                  (unsigned long)s);
    return std::string(buf);
}

// =====================================================================
// TLS 传输（SChannel，Windows）
// =====================================================================
class TlsTransport : public ITransport {
public:
    struct Config {
        std::string   host = "127.0.0.1";
        std::uint16_t port = 443;
        int           timeout_ms = 5000;
        // SNI / 证书主机名校验用的名字。空 → 用 host。
        std::string   sni_host;
        // ★ 默认 false = **校验证书**。把它设成 true 只允许在
        //   实验室/自签场景，且调用方必须显式写出来 ——
        //   任何"默认不校验"的实现都是缺陷。
        bool          insecure_skip_verify = false;
        // 只启用 TLS1.2（现场很多老设备/中间盒对 1.3 支持不全）
        bool          tls12_only = false;
    };

    explicit TlsTransport(Config cfg, Backend backend = Backend::kDefault)
        : cfg_(std::move(cfg)), backend_(resolve_backend(backend)),
          requested_backend_(backend) {
        if (cfg_.sni_host.empty()) cfg_.sni_host = cfg_.host;
    }

    ~TlsTransport() override { close(); }

    TlsTransport(const TlsTransport&) = delete;
    TlsTransport& operator=(const TlsTransport&) = delete;

    // -----------------------------------------------------------------
    // 可观测（测试与现场诊断）
    // -----------------------------------------------------------------
    bool        insecure() const { return cfg_.insecure_skip_verify; }
    Backend     backend() const { return backend_; }
    Backend     requested_backend() const { return requested_backend_; }
    // 本实例的传输层是否**真的**被加密了。
    // ★ 只有在握手真的完成之后才是 true —— 构造出来不会立刻变 true。
    bool        is_encrypted() const override { return handshake_done_; }
    bool        is_up() const override { return handshake_done_ && tcp_.is_open(); }
    std::string endpoint() const override {
        return cfg_.host + ":" + std::to_string((unsigned)cfg_.port) +
               (cfg_.insecure_skip_verify ? "（TLS，未校验证书）" : "（TLS）");
    }
    const char* name() const override { return "TlsTransport"; }

    int handshake_rounds() const { return handshake_rounds_; }
    const std::string& negotiated_protocol() const { return protocol_; }
    const std::string& peer_cert_subject() const { return peer_subject_; }

    // -----------------------------------------------------------------
    // 建立连接（TCP + TLS 握手）
    // -----------------------------------------------------------------
    bool connect() override {
        close();
        if (backend_ != Backend::kSchannel) {
            // ★ 显式拒绝。绝不静默退化成明文 —— 那正是本文件开头
            //   说的"最危险的中间态"。
            last_error_ = missing_backend_reason();
            if (requested_backend_ == Backend::kNone) {
                last_error_ += "；本实例显式选择了 Backend::kNone";
            } else {
                last_error_ += "；当前平台没有编译进任何 TLS 后端";
            }
            return false;
        }
#ifdef _WIN32
        if (!tcp_.connect_to(cfg_.host, cfg_.port, cfg_.timeout_ms, last_error_)) {
            last_error_ = "TLS 底层 TCP 连接失败：" + last_error_;
            return false;
        }
        if (!handshake()) {
            tcp_.close();
            return false;
        }
        return true;
#else
        last_error_ = missing_backend_reason();
        return false;
#endif
    }

    void close() override {
        handshake_done_ = false;
        protocol_.clear();
        peer_subject_.clear();
        handshake_rounds_ = 0;
        raw_.clear();
        plain_.clear();
#ifdef _WIN32
        if (ctx_valid_) { ::DeleteSecurityContext(&ctx_); ctx_valid_ = false; }
        if (cred_valid_) { ::FreeCredentialsHandle(&cred_); cred_valid_ = false; }
#endif
        tcp_.close();
    }

    // 返回实际写出的**明文**字节数；<0 表示失败
    int send(const std::uint8_t* p, std::size_t n) override {
        if (!is_up()) { last_error_ = "TLS 连接未建立"; return -1; }
#ifdef _WIN32
        std::size_t off = 0;
        while (off < n) {
            const std::size_t chunk =
                ((n - off) < sizes_.cbMaximumMessage) ? (n - off) : sizes_.cbMaximumMessage;
            std::vector<std::uint8_t> buf(sizes_.cbHeader + chunk + sizes_.cbTrailer);
            std::memcpy(buf.data() + sizes_.cbHeader, p + off, chunk);

            SecBuffer b[4];
            b[0].BufferType = SECBUFFER_STREAM_HEADER;
            b[0].pvBuffer   = buf.data();
            b[0].cbBuffer   = sizes_.cbHeader;
            b[1].BufferType = SECBUFFER_DATA;
            b[1].pvBuffer   = buf.data() + sizes_.cbHeader;
            b[1].cbBuffer   = (unsigned long)chunk;
            b[2].BufferType = SECBUFFER_STREAM_TRAILER;
            b[2].pvBuffer   = buf.data() + sizes_.cbHeader + chunk;
            b[2].cbBuffer   = sizes_.cbTrailer;
            b[3].BufferType = SECBUFFER_EMPTY;
            b[3].pvBuffer   = nullptr;
            b[3].cbBuffer   = 0;
            SecBufferDesc d;
            d.ulVersion = SECBUFFER_VERSION;
            d.cBuffers  = 4;
            d.pBuffers  = b;

            const SECURITY_STATUS ss = ::EncryptMessage(&ctx_, 0, &d, 0);
            if (ss != SEC_E_OK) {
                last_error_ = "TLS 加密失败：" + sec_status_text((long)ss);
                return -1;
            }
            const std::size_t total = b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer;
            if (!tcp_.send_all(buf.data(), total)) {
                last_error_ = "TLS 发送失败：" + tcp_.error();
                return -1;
            }
            off += chunk;
        }
        return (int)n;
#else
        last_error_ = missing_backend_reason();
        return -1;
#endif
    }

    // >0 明文字节数；0 = 对端正常关闭；<0 = 出错/超时
    int recv(std::uint8_t* p, std::size_t n) override {
        if (!is_up()) { last_error_ = "TLS 连接未建立"; return -1; }
#ifdef _WIN32
        for (int guard = 0; guard < 100000; ++guard) {
            if (!plain_.empty()) {
                const std::size_t take = (n < plain_.size()) ? n : plain_.size();
                std::memcpy(p, plain_.data(), take);
                plain_.erase(plain_.begin(), plain_.begin() + (std::ptrdiff_t)take);
                return (int)take;
            }
            if (raw_.empty()) {
                std::uint8_t tmp[8192];
                const int rc = tcp_.recv_some(tmp, sizeof(tmp));
                if (rc == 0) { handshake_done_ = false; return 0; }   // 对端关闭
                if (rc < 0) {
                    last_error_ = "TLS 接收失败：" + tcp_.error();
                    return -1;
                }
                raw_.insert(raw_.end(), tmp, tmp + rc);
            }

            SecBuffer b[4];
            b[0].BufferType = SECBUFFER_DATA;
            b[0].pvBuffer   = raw_.data();
            b[0].cbBuffer   = (unsigned long)raw_.size();
            for (int i = 1; i < 4; ++i) {
                b[i].BufferType = SECBUFFER_EMPTY;
                b[i].pvBuffer   = nullptr;
                b[i].cbBuffer   = 0;
            }
            SecBufferDesc d;
            d.ulVersion = SECBUFFER_VERSION;
            d.cBuffers  = 4;
            d.pBuffers  = b;

            const SECURITY_STATUS ss = ::DecryptMessage(&ctx_, &d, 0, nullptr);
            if (ss == SEC_E_INCOMPLETE_MESSAGE) {
                // ★ 输入**没有被消费**：必须原样留着，再去收更多。
                //   这里若清空 raw_，半条记录就永久丢了（表现为
                //   "大文件传到某个长度就卡死"）。
                std::uint8_t tmp[8192];
                const int rc = tcp_.recv_some(tmp, sizeof(tmp));
                if (rc == 0) { handshake_done_ = false; return 0; }
                if (rc < 0) { last_error_ = "TLS 接收失败：" + tcp_.error(); return -1; }
                raw_.insert(raw_.end(), tmp, tmp + rc);
                continue;
            }
            if (ss == SEC_I_CONTEXT_EXPIRED) {
                handshake_done_ = false;
                return 0;                                   // 对端发了 close_notify
            }
            if (ss != SEC_E_OK) {
                last_error_ = "TLS 解密失败：" + sec_status_text((long)ss);
                handshake_done_ = false;
                return -1;
            }
            // 收集所有 DATA 段
            for (int i = 0; i < 4; ++i) {
                if (b[i].BufferType == SECBUFFER_DATA && b[i].cbBuffer > 0) {
                    const std::uint8_t* q = (const std::uint8_t*)b[i].pvBuffer;
                    plain_.insert(plain_.end(), q, q + b[i].cbBuffer);
                }
            }
            // 处理跨记录残留
            std::vector<std::uint8_t> tail;
            for (int i = 0; i < 4; ++i) {
                if (b[i].BufferType == SECBUFFER_EXTRA && b[i].cbBuffer > 0) {
                    tail.assign(raw_.end() - b[i].cbBuffer, raw_.end());
                }
            }
            raw_.swap(tail);
        }
        last_error_ = "TLS 接收循环超出上限";
        return -1;
#else
        last_error_ = missing_backend_reason();
        return -1;
#endif
    }

private:
#ifdef _WIN32
    // 握手：InitializeSecurityContext 循环。
    // 注意这是**多轮**的：ClientHello → ServerHello..Certificate..Finished →
    // ClientKeyExchange/Finished。所以这里是个循环，不是一次调用。
    bool handshake() {
        const bool verify = !cfg_.insecure_skip_verify;
        SCHANNEL_CRED sc;
        std::memset(&sc, 0, sizeof(sc));
        sc.dwVersion = SCHANNEL_CRED_VERSION;
        // 默认给 1.0/1.1/1.2 三档（现场老设备 + 中间盒最吃得下的组合）。
        // ★ 默认**不含 TLS1.3**：① MinGW 这一版头文件连常量都没有；
        //   ② SChannel 的 1.3 要 Win11/Server2022 起才有，老现场盒子上
        //   置这一位可能直接协商失败。要 1.3 得显式评估目标机后再放开。
        // tls12_only=true 时**只**留 1.2 —— 这一窄化是可观测的：
        // 对着"只收 TLS1.1"的服务端，默认能连上、tls12_only 连不上。
        sc.grbitEnabledProtocols =
            cfg_.tls12_only
                ? SP_PROT_TLS1_2_CLIENT
                : (SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_1_CLIENT | SP_PROT_TLS1_CLIENT);
        // ★ 这两个 flag 就是"不校验证书"本身。测试 T53 会证明：
        //   不加它们（verify 模式）时自签证书**必须**被拒。
        sc.dwFlags = SCH_CRED_NO_DEFAULT_CREDS;
        if (!verify) {
            sc.dwFlags |= SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_SERVERNAME_CHECK;
        }

        TimeStamp ts;
        SECURITY_STATUS ss = ::AcquireCredentialsHandleA(
            nullptr, (SEC_CHAR*)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, nullptr, &sc,
            nullptr, nullptr, &cred_, &ts);
        if (ss != SEC_E_OK) {
            last_error_ = "AcquireCredentialsHandle 失败：" + sec_status_text((long)ss);
            return false;
        }
        cred_valid_ = true;

        DWORD req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                    ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR | ISC_REQ_STREAM;
        DWORD flags = req;
        std::vector<std::uint8_t> out(0x4000);
        SEC_CHAR* target = verify ? (SEC_CHAR*)cfg_.sni_host.c_str() : nullptr;

        // 第 1 轮：没有输入 token
        SecBuffer  ob[1];
        SecBufferDesc od;
        ob[0].BufferType = SECBUFFER_TOKEN;
        ob[0].pvBuffer   = out.data();
        ob[0].cbBuffer   = (unsigned long)out.size();
        od.ulVersion = SECBUFFER_VERSION;
        od.cBuffers  = 1;
        od.pBuffers  = ob;

        ss = ::InitializeSecurityContextA(&cred_, nullptr, target, flags, 0, 0,
                                          nullptr, 0, &ctx_, &od, &flags, &ts);
        if (ss != SEC_I_CONTINUE_NEEDED && ss != SEC_E_OK) {
            last_error_ = "TLS 握手第 1 轮失败：" + sec_status_text((long)ss);
            return false;
        }
        ctx_valid_ = true;
        if (ob[0].cbBuffer > 0 && !tcp_.send_all(out.data(), ob[0].cbBuffer)) {
            last_error_ = "ClientHello 发送失败：" + tcp_.error();
            return false;
        }
        handshake_rounds_ = 1;

        for (int round = 0; round < kMaxHandshakeRounds && ss != SEC_E_OK; ++round) {
            // 输入不完整或还没收到东西 → 再读
            if (ss == SEC_E_INCOMPLETE_MESSAGE || raw_.empty()) {
                std::uint8_t tmp[8192];
                const int rc = tcp_.recv_some(tmp, sizeof(tmp));
                if (rc == 0) {
                    last_error_ = "对端在 TLS 握手完成前关闭了连接";
                    return false;
                }
                if (rc < 0) {
                    last_error_ = "TLS 握手期间接收失败：" + tcp_.error();
                    return false;
                }
                raw_.insert(raw_.end(), tmp, tmp + rc);
            }

            SecBuffer ib[2];
            ib[0].BufferType = SECBUFFER_TOKEN;
            ib[0].pvBuffer   = raw_.data();
            ib[0].cbBuffer   = (unsigned long)raw_.size();
            ib[1].BufferType = SECBUFFER_EMPTY;
            ib[1].pvBuffer   = nullptr;
            ib[1].cbBuffer   = 0;
            SecBufferDesc id;
            id.ulVersion = SECBUFFER_VERSION;
            id.cBuffers  = 2;
            id.pBuffers  = ib;

            ob[0].BufferType = SECBUFFER_TOKEN;
            ob[0].pvBuffer   = out.data();
            ob[0].cbBuffer   = (unsigned long)out.size();
            od.ulVersion = SECBUFFER_VERSION;
            od.cBuffers  = 1;
            od.pBuffers  = ob;

            flags = req;
            ss = ::InitializeSecurityContextA(&cred_, &ctx_, target, flags, 0, 0,
                                              &id, 0, nullptr, &od, &flags, &ts);
            ++handshake_rounds_;

            if (ob[0].cbBuffer > 0 && !tcp_.send_all(out.data(), ob[0].cbBuffer)) {
                last_error_ = "握手报文发送失败：" + tcp_.error();
                return false;
            }

            // ★ 只在"输入被消费"时才动 raw_。
            if (ss != SEC_E_INCOMPLETE_MESSAGE) {
                if (ib[1].BufferType == SECBUFFER_EXTRA && ib[1].cbBuffer > 0) {
                    std::vector<std::uint8_t> tail(raw_.end() - ib[1].cbBuffer, raw_.end());
                    raw_.swap(tail);
                } else {
                    raw_.clear();
                }
            }

            if (ss == SEC_E_OK) break;
            if (ss == SEC_I_CONTINUE_NEEDED) continue;
            if (ss == SEC_E_INCOMPLETE_MESSAGE) continue;
            if (ss == SEC_I_RENEGOTIATE) {
                last_error_ = "服务器要求 TLS 重新协商，本模块不支持：" +
                              sec_status_text((long)ss);
                return false;
            }
            last_error_ = std::string(verify ? "TLS 握手失败（证书校验为**开**）："
                                             : "TLS 握手失败（证书校验为关）：") +
                          sec_status_text((long)ss);
            return false;
        }
        if (ss != SEC_E_OK) {
            last_error_ = "TLS 握手轮次超过上限（" + std::to_string(kMaxHandshakeRounds) +
                          "），疑似死循环";
            return false;
        }

        ss = ::QueryContextAttributesA(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sizes_);
        if (ss != SEC_E_OK) {
            last_error_ = "查询 TLS 分片尺寸失败：" + sec_status_text((long)ss);
            return false;
        }
        read_negotiated_info();
        handshake_done_ = true;
        return true;
    }

    // 记录协商到的协议版本与对端证书主体 —— 现场排查时这两条最有价值
    void read_negotiated_info() {
        SecPkgContext_ConnectionInfo ci;
        std::memset(&ci, 0, sizeof(ci));
        if (::QueryContextAttributesA(&ctx_, SECPKG_ATTR_CONNECTION_INFO, &ci) == SEC_E_OK) {
            // ★ dwProtocol 返回的是 SP_PROT_xxx 的**位标志**，而且实测
            //   SChannel 给的是 *_CLIENT 那一位（例如 TLS1.2 → 0x00000800）。
            //   常量值来自 schannel.h：
            //     SP_PROT_SSL3_CLIENT  0x00000020
            //     SP_PROT_TLS1_CLIENT  0x00000080
            //     SP_PROT_TLS1_1_CLIENT 0x00000200
            //     SP_PROT_TLS1_2_CLIENT 0x00000800
            //     SP_PROT_TLS1_3_CLIENT 0x00002000
            //   用"按高位优先做位测试"而不是精确相等：某些 SDK 会同时置
            //   SERVER|CLIENT 两位（SP_PROT_TLS1_2 = 0x1800），精确相等会漏。
            //   （第一版这里写的是 0x04/0x08/0x10/0x80 —— 全错，实测被抓出来。）
            const unsigned long v = ci.dwProtocol;
            if (v & 0x00002000UL)      protocol_ = "TLS1.3";
            else if (v & 0x00000800UL) protocol_ = "TLS1.2";
            else if (v & 0x00000200UL) protocol_ = "TLS1.1";
            else if (v & 0x00000080UL) protocol_ = "TLS1.0";
            else if (v & 0x00000020UL) protocol_ = "SSL3.0";
            else                       protocol_ = "unknown(0x" + hex32(v) + ")";
        }
        PCCERT_CONTEXT pc = nullptr;
        if (::QueryContextAttributesA(&ctx_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &pc) == SEC_E_OK &&
            pc != nullptr) {
            char buf[512];
            std::memset(buf, 0, sizeof(buf));
            const DWORD got = ::CertGetNameStringA(pc, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0,
                                                   nullptr, buf, (DWORD)sizeof(buf));
            if (got > 1) peer_subject_ = buf;
            ::CertFreeCertificateContext(pc);
        }
    }

    static std::string hex32(unsigned long v) {
        char b[16];
        std::snprintf(b, sizeof(b), "%08lX", v);
        return std::string(b);
    }
#endif  // _WIN32

    static constexpr int kMaxHandshakeRounds = 32;

    Config      cfg_;
    Backend     backend_;
    Backend     requested_backend_;
    net::TcpSocket tcp_;

    bool        handshake_done_  = false;
    int         handshake_rounds_ = 0;
    std::string protocol_;
    std::string peer_subject_;

    std::vector<std::uint8_t> raw_;    // 未消费的 TLS 记录（密文）
    std::vector<std::uint8_t> plain_;  // 已解密、尚未被取走的明文

#ifdef _WIN32
    CredHandle cred_;
    bool       cred_valid_ = false;
    CtxtHandle ctx_;
    bool       ctx_valid_  = false;
    SecPkgContext_StreamSizes sizes_;
#endif
};

}  // namespace tls
}  // namespace comm
}  // namespace ems
