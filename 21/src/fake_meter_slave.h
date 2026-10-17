// =====================================================================
// 21/ — 进程内 DL/T645 假电能表（测试用从站）
//
// 为什么必须有它（三条，缺一条都不划算）：
//   ① 单元测试不能依赖真实串口 / 真实表 —— clone 下来就得能跑；
//   ② **协议缺陷必须能被人为造出来**：少发一个字节、CS 故意错、
//      +0x33 漏加、应答迟到、回异常码。真表不会配合你演这些，
//      通用串口助手也不会。没有这些开关，"超时重试"这条路径
//      一次都不会被走过 —— 而现场第一个故障恰恰就发生在那里；
//   ③ 它是**可执行的协议文档**：怎么应答 0x11 读数据，看 handle() 就知道。
//
// ---------------------------------------------------------------------
// ★ 它与 MeterSim645 的定位差别（别把它当成"又一个模拟器"）
// ---------------------------------------------------------------------
// MeterSim645 是给人看的桌面程序（卡片墙 / 趋势 / CSV），
// 本文件是给断言用的**被测对端**：没有界面、没有配置文件，
// 一切都是内存里的确定行为 —— 因为断言需要"同输入必同输出"。
// 两者不重复：一个是产品，一个是夹具。
//
// ---------------------------------------------------------------------
// ★ 已知边界（写清楚，避免被当成"完整 645 从站"）
// ---------------------------------------------------------------------
//   · 只实现 0x11 读数据（单 DI）与 0xD1 异常应答；
//   · 不实现 0x12 读后续 / 0x14 写数据 / 0x15 写地址 / 0x08 广播校时；
//   · 不做"多帧分片"（一帧装不下时真表才需要读后续）。
// 这三条不影响主站侧被测路径，但**不能**拿它去冒充合规从站。
// =====================================================================

#pragma once

#include "dlt645_codec.h"
#include "dlt645_frame.h"
#include "dlt645_transport.h"

#include <cstdint>
#include <map>
#include <vector>

namespace ems {
namespace dlt645 {
namespace test {

class FakeMeterSlave {
public:
    // -----------------------------------------------------------------
    // 故障注入开关
    // -----------------------------------------------------------------
    // 每一条都对应现场真实发生过的现象，不是"为测而测"：
    //   drop_response      → 表在忙 / 485 冲突 / 表已掉电
    //   corrupt_cs         → 线路干扰打坏校验和（表自己算错了）
    //   corrupt_pad        → 厂家固件忘了减 0x33（国产表常见方言错）
    //   truncate_tail      → 半帧（网关转发丢尾字节，最容易被当"超时"）
    //   drop_first         → 前 N 次请求不回（表响应慢 / 首帧丢失）
    //   reject_all         → 表不支持该 DI（返回 0xD1 + 0x02 无此数据标识）
    //   wrong_address      → 一表多址 / 地址配错
    struct Options {
        bool drop_response = false;
        bool corrupt_cs = false;      // 故意把 CS 改错 1
        bool corrupt_pad = false;     // 少加 0x33（即多减了）
        bool truncate_tail = false;   // 少发最后 2 字节（CS + 16）
        int  drop_first = 0;          // 前 N 次请求不回，之后正常
        bool reject_all = false;
        bool wrong_address = false;   // 用 0xFFFFFFFFFFFF 回
        bool reply_wrong_di = false;  // 应答里塞一个不是请求的 DI
        int  lead_fe = 2;             // 应答带几个 FE
    };

    FakeMeterSlave(BytePipe* rx_from_master, BytePipe* tx_to_master, const Address& addr)
        : rx_(rx_from_master), tx_(tx_to_master), addr_(addr) {}

    void set_options(const Options& o) { opt_ = o; }
    Options& options() { return opt_; }

    // 设定一个数据项的值（工程量，含小数位语义）
    void set_value(std::uint32_t di, double value) { values_[di] = value; }
    void set_scale(std::uint32_t di, const Scale& s) { scales_[di] = s; }
    void clear_values() { values_.clear(); }

    // 覆盖某个 DI 的格式定义（用于测"非内置 DI"）
    void set_spec(const DiSpec& s) { extra_specs_[s.di] = s; }

    // 收字节 → 解析 → 回帧。测试里在主站发出请求之后显式调用。
    // 返回本次处理掉的完整帧数。
    int poll() {
        // 把管道里的字节全部搬进本地累积缓冲
        std::uint8_t tmp[512];
        const std::size_t got = rx_->take(tmp, sizeof(tmp));
        if (got) buf_.insert(buf_.end(), tmp, tmp + got);

        int handled = 0;
        for (;;) {
            if (buf_.empty()) break;
            ParseResult pr = parse(buf_.data(), buf_.size());
            if (pr.status == ParseStatus::kIncomplete) break;
            if (pr.status != ParseStatus::kOk) {
                // 坏帧：丢掉 1 字节再试（与真实设备的"重新同步"一致）
                buf_.erase(buf_.begin());
                ++bad_frames_;
                continue;
            }
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pr.consumed));
            handle(pr.frame);
            ++handled;
        }
        return handled;
    }

    // ---- 统计（断言用）----
    int rx_frames() const { return rx_frames_; }
    int bad_frames() const { return bad_frames_; }
    int tx_frames() const { return tx_frames_; }
    int ignored() const { return ignored_; }
    int last_error() const { return last_error_; }

private:
    const DiSpec* spec_of(std::uint32_t di) const {
        auto it = extra_specs_.find(di);
        if (it != extra_specs_.end()) return &it->second;
        return find_builtin(di);
    }

    void handle(const Frame& req) {
        ++rx_frames_;

        // ★ 真实从站只应答"发给自己的"帧（广播除外）。
        //   这条不能省：多表共 485 总线时，主站地址配错的表现正是
        //   "表明明在线却一个字节都不回" —— 假表若无脑应答，
        //   ADDR_MISMATCH 那条路径就永远测不出来。
        if (!(req.addr == addr_) && !req.addr.is_broadcast()) {
            ++ignored_;
            return;
        }

        // "前 N 次请求不回" —— 用来考主站的重试路径。
        // ★ 为什么不模拟"延迟"而是模拟"丢"：主站的判据是"在 timeout 内
        //   有没有凑出一帧"，延迟与丢失在它眼里完全一样；而"丢"能精确控制
        //   "第几次成功"，让 ok_after_retry 这条断言变成确定性的。
        if (opt_.drop_first > 0) {
            --opt_.drop_first;
            return;
        }
        respond(req);
    }

    void respond(const Frame& req) {
        if (opt_.drop_response) return;

        Address reply_addr = opt_.wrong_address ? Address() : req.addr;
        if (opt_.wrong_address) {
            // 0xFFFFFFFFFFFF 不是合法 BCD，但正好用来测"地址不匹配"
            for (int i = 0; i < 6; ++i) reply_addr.set_at(i, 0xFF);
        }

        Frame resp;
        resp.addr = reply_addr;

        // 只支持 0x11 读数据。其它控制码一律回异常（真实表也这么干）。
        if (req.ctrl != kReadData) {
            resp.ctrl = kAbnormalResp;
            resp.data.push_back(kErrNoData);
            send(resp);
            return;
        }
        if (req.data.size() != 4) {
            resp.ctrl = kAbnormalResp;
            resp.data.push_back(kErrOther);
            send(resp);
            return;
        }

        const std::uint32_t di = wire_to_di(&req.data[0]);
        const DiSpec* spec = spec_of(di);
        if (opt_.reject_all || spec == nullptr) {
            last_error_ = kErrNoData;
            resp.ctrl = kAbnormalResp;
            resp.data.push_back(kErrNoData);
            send(resp);
            return;
        }

        auto it = values_.find(di);
        if (it == values_.end()) {
            last_error_ = kErrNoData;
            resp.ctrl = kAbnormalResp;
            resp.data.push_back(kErrNoData);
            send(resp);
            return;
        }

        auto sit = scales_.find(di);
        const Scale sc = (sit == scales_.end()) ? Scale() : sit->second;

        // 应答数据域：DI(4, 传输序) + 数据(BCD, 低位在前)
        std::uint8_t diw[4];
        if (opt_.reply_wrong_di) {
            // 模拟"表回了当前项目录而不是请求项"：DI 故意换成 0
            const std::uint32_t other = 0x00000000u;
            di_to_wire(other, diw);
        } else {
            di_to_wire(di, diw);
        }
        resp.data.assign(diw, diw + 4);

        std::vector<std::uint8_t> body;
        const char* why = nullptr;
        if (!encode_bcd(*spec, it->second, sc, &body, &why)) {
            last_error_ = kErrOther;
            resp.ctrl = kAbnormalResp;
            resp.data.clear();
            resp.data.push_back(kErrOther);
            send(resp);
            return;
        }
        resp.data.insert(resp.data.end(), body.begin(), body.end());
        resp.ctrl = kReadDataResp;
        send(resp);
    }

    void send(Frame& resp) {
        std::vector<std::uint8_t> raw = encode(resp, opt_.lead_fe);

        if (opt_.corrupt_pad) {
            // ★ 模拟"厂家固件忘了给数据区加 0x33"：只破坏 DI 之后的字节，
            //   **DI 保持正确** —— 这样主站的 DI 校验能通过，真正被考到的是
            //   BCD 解码那一步。若连 DI 也一起打坏，主站会先报 DI_MISMATCH，
            //   这条注入就测不到它本来要测的东西了。
            //   注意 CS 要按**改动后**的字节重算：真实设备是"一致地算错"，
            //   不是"CS 与数据互相矛盾"（那是 corrupt_cs 那种故障）。
            const std::size_t lead = static_cast<std::size_t>(opt_.lead_fe);
            const std::size_t data_begin = lead + 10;
            const std::size_t L = resp.data.size();
            for (std::size_t i = 4; i < L; ++i) {
                raw[data_begin + i] = resp.data[i];   // 漏了 +0x33
            }
            raw[data_begin + L] = checksum(raw.data() + lead, data_begin + L - lead);
        }

        if (opt_.corrupt_cs) {
            const std::size_t cs_pos = static_cast<std::size_t>(opt_.lead_fe) + 10 + resp.data.size();
            raw[cs_pos] = static_cast<std::uint8_t>(raw[cs_pos] + 1);
        }

        if (opt_.truncate_tail && raw.size() >= 2) {
            raw.resize(raw.size() - 2);   // 少发 CS + 16
        }

        tx_->push(raw.data(), raw.size());
        ++tx_frames_;
    }

    BytePipe* rx_;
    BytePipe* tx_;
    Address addr_;
    Options opt_;
    std::vector<std::uint8_t> buf_;
    std::map<std::uint32_t, double> values_;
    std::map<std::uint32_t, Scale> scales_;
    std::map<std::uint32_t, DiSpec> extra_specs_;

    int rx_frames_ = 0;
    int bad_frames_ = 0;
    int tx_frames_ = 0;
    int ignored_ = 0;
    int last_error_ = 0;
};

}  // namespace test
}  // namespace dlt645
}  // namespace ems
