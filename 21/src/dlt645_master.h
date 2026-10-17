// =====================================================================
// 21/ — DL/T 645-2007 主站（请求 / 应答 / 超时重试）
//
// 一个 645 事务只有四步：清缓冲 → 发请求 → 收字节 → 判帧。
// 听起来简单，但**每一步都有一种"看起来成功、其实是假数据"的失败方式**：
//
//   ① 不清缓冲  → 上一轮迟到的应答被当成本轮应答（值全对，但是旧的）
//   ② 不校验地址 → 隔壁表的数据被当自己的（多表共总线时必然发生）
//   ③ 不校验 DI  → 表回了个"目录帧"或别的项，被按请求的格式解码
//   ④ 把超时当 0 → 0 kW / 0 V 看起来完全正常，算法照跑
//
// 所以本类的四个校验是**并列的硬条件**，不是"锦上添花"。
// 少任何一个，现场都会表现为"通信正常但数据不对"，这是最难查的一类。
//
// ---------------------------------------------------------------------
// ★ 重试的正确位置：**整笔事务**，不是"补读几个字节"
// ---------------------------------------------------------------------
// 645 没有事务号，半双工也可能把两帧粘在一起。一旦帧边界丢了，
// 只能清缓冲重来 —— 所以重试粒度必须是"清缓冲 + 重发整帧"。
// "继续读剩下的字节"这种写法在粘包时会读进下一帧的开头，
// 然后 CS 校验失败、再重试，把一次超时放大成一串超时。
// =====================================================================

#pragma once

#include "dlt645_codec.h"
#include "dlt645_frame.h"
#include "dlt645_transport.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace ems {
namespace dlt645 {

struct MasterOptions {
    int timeout_ms = 500;   // 645 规定等待应答 T1 约 500ms（厂家可调）
    int retries    = 2;     // 重试次数（**不含首轮**）
    int lead_fe    = 2;     // 请求前导 FE 个数（唤醒）
};

enum class ReadStatus {
    kOk = 0,
    kTimeout,        // 等到超时也没凑出一帧
    kBadFrame,       // 收到字节但 CS / 结束符 / 结构错
    kAbnormal,       // 从站回 0xD1（无此数据标识等）
    kAddrMismatch,   // 应答地址不是本表
    kDiMismatch,     // 应答的数据标识与请求不符
    kNoSpec,         // 本地 DI 表里没有这个项（发不出去）
    kLinkError,      // 链路层失败
};

const char* read_status_text(ReadStatus s);

struct ReadResult {
    ReadStatus st = ReadStatus::kLinkError;
    double raw = 0.0;
    double value = 0.0;
    std::uint8_t err_code = 0;    // kAbnormal 时的从站错误码
    int attempts = 0;             // 实际发了多少次（1 = 一次成）
    const char* why = "";
};

struct MasterStats {
    long requests = 0;
    long ok = 0;
    long timeouts = 0;
    long bad_frames = 0;
    long abnormals = 0;
    long addr_mismatch = 0;
    long di_mismatch = 0;
    long retries = 0;              // 累计多发的次数
    long ok_after_retry = 0;       // ★ 重试后才成功的笔数
    // 用途：ok_after_retry 恒 0 时，"重试"这段代码可能根本没被走到。
    // 只断言 ok > 0 是看不出这件事的（一次就成功的场景也满足）。
};

class Dlt645Master {
public:
    Dlt645Master(IByteLink* link, const Address& meter, const MasterOptions& opt = MasterOptions())
        : link_(link), meter_(meter), opt_(opt) {}

    // 测试钩子：每次等应答之前调用（用来驱动进程内假表）。
    // 真机上留空即可 —— 真实字节由网卡/串口自己到。
    void set_pump(std::function<void()> fn) { pump_ = std::move(fn); }

    ReadResult read(std::uint32_t di, const Scale& scale = Scale()) {
        ReadResult r;

        const DiSpec* spec = find_builtin(di);
        if (spec == nullptr) {
            r.st = ReadStatus::kNoSpec;
            r.why = "本地 DI 表无此项，拒绝发请求";
            return r;
        }

        const int max_try = (opt_.retries < 0 ? 0 : opt_.retries) + 1;
        for (int attempt = 1; attempt <= max_try; ++attempt) {
            r = transact(di, *spec, scale);
            r.attempts = attempt;
            ++stats_.requests;
            if (attempt > 1) {
                ++stats_.retries;
            }

            if (r.st == ReadStatus::kOk) {
                ++stats_.ok;
                if (attempt > 1) ++stats_.ok_after_retry;
                return r;
            }

            // 统计口径：把失败**分类**记，不要合成一个 fail 计数。
            // 合成之后 "超时多" 与 "解码错多" 分不开，现场排查方向完全相反。
            switch (r.st) {
                case ReadStatus::kTimeout:      ++stats_.timeouts; break;
                case ReadStatus::kBadFrame:     ++stats_.bad_frames; break;
                case ReadStatus::kAbnormal:     ++stats_.abnormals; break;
                case ReadStatus::kAddrMismatch: ++stats_.addr_mismatch; break;
                case ReadStatus::kDiMismatch:   ++stats_.di_mismatch; break;
                default: break;
            }

            // 链路层失败重试也没用（线掉了 / 端口没开），直接返回
            if (r.st == ReadStatus::kLinkError) return r;

            // ★ 只有"可能是一过性的"才值得重试。
            //   超时、坏帧 → 线路 / 干扰 / 分片，重发整帧有意义；
            //   异常应答（表说"没这个点"）、地址不符、DI 不符 → **确定的回答**，
            //   连发三遍只会把一次失败拖成三轮 timeout 的时长，而结论不变。
            //   现场感受是"程序卡住"，日志上却只是"重试 2 次"，很难对上。
            if (r.st != ReadStatus::kTimeout && r.st != ReadStatus::kBadFrame) {
                return r;
            }
        }
        return r;
    }

    const MasterStats& stats() const { return stats_; }
    void reset_stats() { stats_ = MasterStats(); }
    const Address& address() const { return meter_; }

    // 组一个读数据请求帧（测试要直接比对字节时用）
    static Frame make_read_request(const Address& addr, std::uint32_t di) {
        Frame f;
        f.addr = addr;
        f.ctrl = kReadData;
        f.data.assign(4, 0);
        di_to_wire(di, &f.data[0]);
        return f;
    }

private:
    ReadResult transact(std::uint32_t di, const DiSpec& spec, const Scale& scale) {
        ReadResult r;
        if (link_ == nullptr || !link_->is_open()) {
            r.st = ReadStatus::kLinkError;
            r.why = "链路未打开";
            return r;
        }

        // ① 清缓冲：迟到的上一帧必须丢在这里，不能进入解析
        link_->flush_input();

        const Frame req = make_read_request(meter_, di);
        const std::vector<std::uint8_t> raw = encode(req, opt_.lead_fe);
        if (link_->write(raw.data(), raw.size()) != static_cast<int>(raw.size())) {
            r.st = ReadStatus::kLinkError;
            r.why = "写链路失败";
            return r;
        }

        // ② 收 + 判
        std::vector<std::uint8_t> buf;
        bool have_frame = false;
        Frame resp;

        // 预算：单帧最多两轮 timeout。为什么不是一轮：
        // 首轮可能只到了半帧（网关分片），再给一轮把尾巴收齐；
        // 三轮以上就不该再等了 —— 645 帧在 2400bps 下最长约 1.1s，
        // 等太久会把"表已离线"这件事拖成一次很长的阻塞。
        for (int round = 0; round < 2 && !have_frame; ++round) {
            if (pump_) pump_();

            std::uint8_t tmp[256];
            const int got = link_->read(tmp, sizeof(tmp), opt_.timeout_ms);
            if (got < 0) {
                r.st = ReadStatus::kLinkError;
                r.why = "读链路失败";
                return r;
            }
            if (got == 0 && buf.empty()) {
                r.st = ReadStatus::kTimeout;
                r.why = "等待应答超时";
                continue;
            }
            buf.insert(buf.end(), tmp, tmp + got);

            ParseResult pr = parse(buf.data(), buf.size());
            if (pr.status == ParseStatus::kOk) {
                have_frame = true;
                resp = pr.frame;
            } else if (pr.status == ParseStatus::kIncomplete) {
                r.st = ReadStatus::kTimeout;
                r.why = "帧不完整（半帧）";
            } else {
                r.st = ReadStatus::kBadFrame;
                r.why = parse_status_text(pr.status);
                return r;   // 帧结构坏了，重试只能重发整帧
            }
        }
        if (!have_frame) {
            if (r.st != ReadStatus::kTimeout) {
                r.st = ReadStatus::kTimeout;
                r.why = "未收到完整应答";
            }
            return r;
        }

        // ③ 地址校验：多表共总线时，这是唯一挡住"串表"的闸
        if (!(resp.addr == meter_)) {
            r.st = ReadStatus::kAddrMismatch;
            r.why = "应答地址与本表不符";
            return r;
        }

        // ④ 异常应答优先判：0xD1 的数据域只有 1 字节错误码，
        //    若先按"读数据应答"解析会因长度不符报成 BAD_FRAME，
        //    把"表说没有这个点"误诊成"线路坏了" —— 排查方向完全相反。
        if (resp.is_abnormal()) {
            r.st = ReadStatus::kAbnormal;
            r.err_code = resp.data.empty() ? 0 : resp.data[0];
            r.why = err_text(r.err_code);
            return r;
        }
        if (resp.ctrl != kReadDataResp) {
            r.st = ReadStatus::kBadFrame;
            r.why = "控制码不是读数据应答";
            return r;
        }
        if (resp.data.size() < 4) {
            r.st = ReadStatus::kBadFrame;
            r.why = "应答数据域不足 4 字节（缺 DI）";
            return r;
        }

        // ⑤ DI 校验：表可能回"当前项目录"或别的项
        const std::uint32_t got_di = wire_to_di(&resp.data[0]);
        if (got_di != di) {
            r.st = ReadStatus::kDiMismatch;
            r.why = "应答数据标识与请求不符";
            return r;
        }

        const std::size_t body = resp.data.size() - 4;
        DecodeResult d = decode_bcd(spec, resp.data.data() + 4, body, scale);
        if (!d.ok) {
            r.st = ReadStatus::kBadFrame;
            r.why = d.why;
            return r;
        }

        r.st = ReadStatus::kOk;
        r.raw = d.raw;
        r.value = d.value;
        return r;
    }

    IByteLink* link_;
    Address meter_;
    MasterOptions opt_;
    MasterStats stats_;
    std::function<void()> pump_;
};

inline const char* read_status_text(ReadStatus s) {
    switch (s) {
        case ReadStatus::kOk:           return "OK";
        case ReadStatus::kTimeout:      return "TIMEOUT";
        case ReadStatus::kBadFrame:     return "BAD_FRAME";
        case ReadStatus::kAbnormal:     return "ABNORMAL";
        case ReadStatus::kAddrMismatch: return "ADDR_MISMATCH";
        case ReadStatus::kDiMismatch:   return "DI_MISMATCH";
        case ReadStatus::kNoSpec:       return "NO_SPEC";
        case ReadStatus::kLinkError:    return "LINK_ERROR";
    }
    return "UNKNOWN";
}

}  // namespace dlt645
}  // namespace ems
