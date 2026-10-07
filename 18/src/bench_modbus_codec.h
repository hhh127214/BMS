// =====================================================================
// 18/bench_modbus_codec.h — 基准 B3：Modbus PDU 编解码（纯 CPU，无 socket）
//
// 被测对象：13/src/modbus_tcp_client.h 的 pdu::build_* / parse_* 与
//   32 位浮点字序转换 put_f32 / get_f32（13/ 的协议层内核）。
//
// 为什么把编解码单独拎出来当一类基准：
//   · 它是**现场"采一帧要 3 秒"的第一嫌疑** —— 但真凶通常不是编解码本身，
//     而是分块规划（FC03 单次最多 125 寄存器）与 IO 往返。把编解码的
//     纯 CPU 代价钉住，才能把"慢"归因到 IO 而不是 CPU。
//   · 它是**纯函数、无 IO** —— 基准可复现性最好的一类，适合做
//     相对判据（SLA-M3）与注入延迟的反向验证。
//
// 只引用 13/ 的头文件（-I ../13/src），**不改 13/ 一个字节**。
// =====================================================================

#pragma once

#include "perf_clock.h"
#include "perf_stats.h"

#include "modbus_tcp_client.h"   // 13/

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace ems {
namespace perf {

namespace mb = ::ems::modbus;

struct CodecBenchArgs {
    int iters           = 50000;
    int warmup          = 500;
    std::function<double(int)> delay_per_iter_us;   // 反向验证注入
};

struct CodecBenchResult {
    std::vector<double> build_us;
    std::vector<double> parse_us;
    std::vector<double> f32_us;
    double sink = 0.0;   // 防止整个循环被优化掉
};

inline CodecBenchResult run_modbus_codec_bench(const CodecBenchArgs& a) {
    CodecBenchResult r;
    r.build_us.reserve((size_t)a.iters);
    r.parse_us.reserve((size_t)a.iters);
    r.f32_us.reserve((size_t)a.iters);

    // 构造一个合法的 FC04 读响应 PDU（qty=2 → ByteCount=4）
    const std::uint16_t qty = 2;
    std::uint8_t resp[16] = {0};
    resp[0] = mb::fc::kReadInputRegs;   // 功能码
    resp[1] = (std::uint8_t)(2 * qty);  // ByteCount
    // 数据：两个 16 位寄存器（0x4148 = 12.5f 的高字等，值不重要）
    resp[2] = 0x41; resp[3] = 0x48;
    resp[4] = 0x00; resp[5] = 0x00;
    const std::size_t resp_len = 2 + 2 * qty;

    std::uint8_t  req[32];
    std::uint16_t regs[2] = {0, 0};
    float         f = 0.0f;

    for (int i = 0; i < a.iters; ++i) {
        const double inj = a.delay_per_iter_us ? a.delay_per_iter_us(i) : 0.0;
        const double inj2 = (inj > 0.0) ? inj : 0.0;
        // ---- 请求构造 ----
        {
            const double t0 = now_us();
            const std::size_t n = mb::pdu::build_read(
                mb::fc::kReadInputRegs, (std::uint16_t)(i & 0xFFFF), qty, req);
            if (inj2 > 0.0) busy_wait_us(inj2);   // 计时区间内注入
            const double t1 = now_us();
            r.build_us.push_back(t1 - t0);
            r.sink += (double)n;
        }
        // ---- 响应解析 ----
        {
            const double t0 = now_us();
            const bool ok = mb::pdu::parse_read_regs(resp, resp_len, qty, regs);
            if (inj2 > 0.0) busy_wait_us(inj2);
            const double t1 = now_us();
            r.parse_us.push_back(t1 - t0);
            r.sink += ok ? (double)regs[0] : 0.0;
        }
        // ---- 浮点字序往返 ----
        {
            std::uint16_t two[2];
            const double t0 = now_us();
            mb::put_f32(two, 12.5f + (float)(i & 7), mb::WordOrder::kHighWordFirst);
            f = mb::get_f32(two, mb::WordOrder::kHighWordFirst);
            if (inj2 > 0.0) busy_wait_us(inj2);
            const double t1 = now_us();
            r.f32_us.push_back(t1 - t0);
            r.sink += (double)f;
        }
    }
    return r;
}

} // namespace perf
} // namespace ems
