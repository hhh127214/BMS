// =====================================================================
// 21/ — 传输抽象（字节链路）
//
// 645 在物理层是 RS485 半双工串口（2400 bps · 8E1 是出厂默认），
// 但现在现场大量是"串口服务器透传"（TCP 连过去，字节流还是 645 帧）。
// 两者的差别只在**怎么把字节搬过去**，帧怎么组、怎么校验完全一样。
//
// 所以这里把"搬字节"抽成一个接口，协议层一行都不用改。
//
// ---------------------------------------------------------------------
// ★ read() 的返回语义（设计要点，改之前先想清楚）
// ---------------------------------------------------------------------
// 返回 0 **不是** "对端没数据"，而是"等到超时也没有新字节"。
// 645 是变长帧（L 字段决定），主站必须**边收边判**：
//   收到半帧 → parse() 返回 kIncomplete → 继续等
// 因此 read() 必须允许返回**少于请求的字节数**（部分读），
// 绝不能写成"读满 n 字节才返回" —— 那会让主站永远卡在第一帧上，
// 而现象是"通信超时"，很容易被误判成设备离线。
//
// 另一个必做的动作：**请求前清空输入缓冲**。
// 半双工总线上，上一轮超时后迟到的应答会在缓冲区里躺着；不清就会
// 把"上一帧的应答"当"这一帧的应答"解析 —— 值全对、但全是上一次的。
// 这类错在界面上表现为"数据不刷新"，而通信计数一切正常。
// =====================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ems {
namespace dlt645 {

class IByteLink {
public:
    virtual ~IByteLink() = default;

    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    // 返回实际写入字节数；<0 表示错误
    virtual int write(const std::uint8_t* p, std::size_t n) = 0;

    // 返回读到的字节数（可 < n）；0 = 超时；<0 = 错误
    virtual int read(std::uint8_t* p, std::size_t n, int timeout_ms) = 0;

    // 丢弃接收缓冲里的残留字节（发请求前必须调）
    virtual void flush_input() = 0;
};

// ---------------------------------------------------------------------
// 单向字节管道（内存回环用）
// ---------------------------------------------------------------------
// 单线程使用，**不加锁**：测试是同步的，加锁只会掩盖时序问题，
// 也会让"读到半帧"这类边界更难复现。
class BytePipe {
public:
    void push(const std::uint8_t* p, std::size_t n) {
        buf_.insert(buf_.end(), p, p + n);
    }
    std::size_t take(std::uint8_t* p, std::size_t n) {
        const std::size_t k = (n < buf_.size()) ? n : buf_.size();
        if (k == 0) return 0;
        std::memcpy(p, buf_.data(), k);
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(k));
        return k;
    }
    std::size_t size() const { return buf_.size(); }
    void clear() { buf_.clear(); }

private:
    std::vector<std::uint8_t> buf_;
};

// ---------------------------------------------------------------------
// 内存回环链路
// ---------------------------------------------------------------------
// 写进 tx，从 rx 读。把主站的 tx 接到假表的 rx、假表的 tx 接到主站的 rx，
// 就是一条"零硬件"的完整链路（MeterSim645 的仿真模式是同一个思路）。
//
// ★ 为什么不用 socket 回环做这件事：单元测试不该依赖端口占用、
//   也不该受防火墙影响；而且进程内能精确注入"少发一个字节"这种故障。
class LoopbackLink : public IByteLink {
public:
    LoopbackLink(BytePipe* tx, BytePipe* rx) : tx_(tx), rx_(rx), open_(false) {}

    bool open() override { open_ = true; return true; }
    void close() override { open_ = false; }
    bool is_open() const override { return open_; }

    int write(const std::uint8_t* p, std::size_t n) override {
        if (!open_) return -1;
        tx_->push(p, n);
        return static_cast<int>(n);
    }

    // ★ 无数据 → 返回 0（= 超时）。测试里**不真的睡**，
    //   否则一堆超时用例会把构建时间拖成分钟级。
    int read(std::uint8_t* p, std::size_t n, int /*timeout_ms*/) override {
        if (!open_) return -1;
        return static_cast<int>(rx_->take(p, n));
    }

    void flush_input() override { rx_->clear(); }

private:
    BytePipe* tx_;
    BytePipe* rx_;
    bool open_;
};

}  // namespace dlt645
}  // namespace ems
