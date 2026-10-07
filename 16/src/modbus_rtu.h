// =====================================================================
// 16/ — Modbus RTU（串口 / RS-485）
//
// 为什么 13/ 只做 TCP 还不够：现场大量设备（电表、BMS、部分 PCS 的维护口）
// 是 RS-485 上的 Modbus RTU。协议层已经有"帧同步"与"字节流"两件事，
// RTU 相对 TCP 只多两样：**CRC16 校验** 与 **靠静默间隔分帧**。
// 这两样恰恰是现场"偶发丢帧/偶发错码"的两大来源，所以必须自己写、逐字节可断言。
//
// ── 三个必须显式处理的坑 ────────────────────────────────────────────
//
// ★① **CRC 在线上是低字节在前**。RFC/国标里的描述是"CRC 低字节先发"，
//    而很多资料把它写成"大端" —— 写反的表现是**从站全部不响应**
//    （从站 CRC 校验失败后按规范保持静默，连异常码都不回），
//    现场看到的就是"完全没反应"，而不是"报了 CRC 错"。极难查。
//    本文件的判据：已知答案向量 `01 03 00 00 00 0A` → CRC 值 0xCDC5，
//    线上字节为 `C5 CD`（低字节 C5 在前）。该向量已用**独立实现**
//    （pymodbus 3.15 的 FramerRTU.compute_CRC）交叉核对过，见 16/docs/README.md §4。
//
// ★② **分帧只能靠 3.5 字符时间的静默**。RTU 帧**没有**长度字段也没有
//    起始/结束定界符 —— 一帧的结束只能由"之后线路安静了 3.5 个字符时间"来推断。
//    真串口时序在单元测试里造不出来（既不能精确控制也不会真的静默），
//    所以本文件的 API 把这个物理事实**显式参数化**：
//      feed(bytes, gap_ms)   // gap_ms = 这批字节**之后**线路的空闲时长
//    只有 gap_ms >= 3.5T 时缓冲区里的内容才算一帧。
//    gap_ms 是"之后"而不是"之前"：实际读取代码里，帧结束的判据就是
//    "再读一次读空了，且读空耗时 >= 3.5T"，天然对应"之后"。
//
// ★③ **CRC 错的帧必须被丢弃，且不能污染后面的帧**。现场 RS-485 上
//    干扰、终端电阻缺失、波特率不匹配都会产生垃圾字节。分帧器若在
//    CRC 失败后不清空缓冲，垃圾会一直和后续的合法帧粘在一起，
//    表现为"坏一次之后再也读不到数据"。本文件在 CRC 失败时**逐字节前移
//    重同步**：从偏移 1、2、3… 处寻找第一个 CRC 合法的帧。
//
// ── 一个真实世界的细节 ───────────────────────────────────────────────
// 3.5 字符时间的理论值在 9600 bps 下只有 4.0 ms，115200 下只有 0.33 ms。
// 但 USB 转串口芯片 + 操作系统定时器的实际粒度常在 1–16 ms，
// 所以规范本身的推荐值是 1750 µs，而工程实现必须给一个**下界**
// （本文件默认 2 ms，可调）。给不出的后果是"合法帧被从中间截断成两半"，
// 而两半各自的 CRC 都不过 —— 于是所有帧都被丢掉。
//
// 编译：纯头文件。Windows 侧串口实现需要 <windows.h>（不额外链接库）。
// =====================================================================

#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
// ★ 顺序纪律：必须先 <winsock2.h> 再 <windows.h>，否则 <windows.h> 会拉进
//   老的 <winsock.h>，与 winsock2 的定义冲突（重定义错误刷屏）。
//   本头文件自己保证顺序，不依赖调用方的 include 顺序。
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace ems {
namespace comm {

// =====================================================================
// 功能码
// =====================================================================
enum ModbusFc : std::uint8_t {
    kFcReadCoils              = 1,
    kFcReadDiscreteInputs     = 2,
    kFcReadHoldingRegisters   = 3,
    kFcReadInputRegisters     = 4,
    kFcWriteSingleCoil        = 5,
    kFcWriteSingleRegister    = 6,
    kFcWriteMultipleCoils     = 15,
    kFcWriteMultipleRegisters = 16,
};

inline bool is_known_fc(std::uint8_t fc) {
    switch (fc & 0x7F) {
        case 1: case 2: case 3: case 4: case 5: case 6: case 15: case 16:
            return true;
        default:
            return false;
    }
}

// =====================================================================
// CRC16/Modbus
//
// 反射多项式 0xA001（= 0x8005 位反转），初值 0xFFFF，
// 输入/输出均反射，无最终异或。
// =====================================================================
inline std::uint16_t crc16_modbus(const std::uint8_t* p, std::size_t n) {
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= (std::uint16_t)p[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x0001u) crc = (std::uint16_t)((crc >> 1) ^ 0xA001u);
            else               crc = (std::uint16_t)(crc >> 1);
        }
    }
    return crc;
}

inline std::uint16_t crc16_modbus(const std::vector<std::uint8_t>& v) {
    return crc16_modbus(v.empty() ? nullptr : v.data(), v.size());
}

// 把 CRC 按**线上顺序**（低字节在前）拼到帧尾
inline void append_crc_le(std::vector<std::uint8_t>& frame) {
    const std::uint16_t c = crc16_modbus(frame);
    frame.push_back((std::uint8_t)(c & 0xFF));         // 低字节在前 ★
    frame.push_back((std::uint8_t)((c >> 8) & 0xFF));
}

// 从线上两个字节还原 CRC 值（低字节在前）
inline std::uint16_t crc_from_wire(const std::uint8_t* p) {
    return (std::uint16_t)(((std::uint16_t)p[0]) | ((std::uint16_t)p[1] << 8));
}

// 整帧（含末尾 2 字节 CRC）校验
inline bool crc_ok(const std::uint8_t* frame, std::size_t n) {
    if (n < 4) return false;
    return crc16_modbus(frame, n - 2) == crc_from_wire(frame + n - 2);
}

// =====================================================================
// 请求构造（PDU，不含地址与 CRC）
// =====================================================================
// 读请求 PDU = fc(1) + start(2) + count(2)，共 5 字节
inline bool build_read_pdu(std::uint8_t fc, std::uint16_t start,
                           std::uint16_t count, std::vector<std::uint8_t>& out) {
    if (fc != kFcReadCoils && fc != kFcReadDiscreteInputs &&
        fc != kFcReadHoldingRegisters && fc != kFcReadInputRegisters) {
        return false;
    }
    // 规范上限：位读 0x07D0(2000)，寄存器读 0x007D(125)。超了从站回异常码 3，
    // 但"先在本地拦住"能在现场少一轮无谓往返，且错误信息更清楚。
    const std::uint16_t max_count =
        (fc == kFcReadCoils || fc == kFcReadDiscreteInputs) ? 2000 : 125;
    if (count == 0 || count > max_count) return false;
    out.clear();
    out.push_back(fc);
    out.push_back((std::uint8_t)(start >> 8));
    out.push_back((std::uint8_t)(start & 0xFF));
    out.push_back((std::uint8_t)(count >> 8));
    out.push_back((std::uint8_t)(count & 0xFF));
    return true;
}

// 写单个寄存器 PDU = fc(1) + addr(2) + value(2)
inline bool build_write_single_register_pdu(std::uint16_t addr, std::uint16_t value,
                                            std::vector<std::uint8_t>& out) {
    out.clear();
    out.push_back(kFcWriteSingleRegister);
    out.push_back((std::uint8_t)(addr >> 8));
    out.push_back((std::uint8_t)(addr & 0xFF));
    out.push_back((std::uint8_t)(value >> 8));
    out.push_back((std::uint8_t)(value & 0xFF));
    return true;
}

// 写多个寄存器 PDU = fc(1) + start(2) + count(2) + bytecount(1) + data(2N)
inline bool build_write_multiple_registers_pdu(std::uint16_t start,
                                               const std::vector<std::uint16_t>& values,
                                               std::vector<std::uint8_t>& out) {
    if (values.empty() || values.size() > 123) return false;  // 规范上限 0x7B
    const std::size_t nbytes = values.size() * 2;
    out.clear();
    out.push_back(kFcWriteMultipleRegisters);
    out.push_back((std::uint8_t)(start >> 8));
    out.push_back((std::uint8_t)(start & 0xFF));
    out.push_back((std::uint8_t)(values.size() >> 8));
    out.push_back((std::uint8_t)(values.size() & 0xFF));
    out.push_back((std::uint8_t)nbytes);
    for (std::size_t i = 0; i < values.size(); ++i) {
        out.push_back((std::uint8_t)(values[i] >> 8));
        out.push_back((std::uint8_t)(values[i] & 0xFF));
    }
    return true;
}

// 把 PDU 装成完整帧：addr(1) + PDU + crc(2)
inline std::vector<std::uint8_t> wrap_rtu_frame(std::uint8_t addr,
                                                const std::vector<std::uint8_t>& pdu) {
    std::vector<std::uint8_t> f;
    f.reserve(pdu.size() + 3);
    f.push_back(addr);
    f.insert(f.end(), pdu.begin(), pdu.end());
    append_crc_le(f);
    return f;
}

// =====================================================================
// 帧
// =====================================================================
struct RtuFrame {
    std::uint8_t               addr = 0;
    std::vector<std::uint8_t>  pdu;   // 不含 CRC

    bool is_exception() const {
        return !pdu.empty() && (pdu[0] & 0x80) != 0;
    }
    std::uint8_t function() const { return pdu.empty() ? 0 : (std::uint8_t)(pdu[0] & 0x7F); }
    // 异常码（无异常时返回 0）
    std::uint8_t exception_code() const {
        return (is_exception() && pdu.size() >= 2) ? pdu[1] : 0;
    }
    // 读类响应的字节数（非法时返回 -1）
    int byte_count() const {
        if (pdu.size() < 2) return -1;
        switch (function()) {
            case 1: case 2: case 3: case 4:
                return pdu.size() >= 2 ? (int)pdu[1] : -1;
            default: return -1;
        }
    }
    // 把读响应的数据段解成 16 位寄存器序列（每 2 字节一个，大端）
    bool to_registers(std::vector<std::uint16_t>& out) const {
        if (function() != 3 && function() != 4) return false;
        if (pdu.size() < 3) return false;
        const std::size_t bc = pdu[1];
        if (bc % 2 != 0 || 2 + bc != pdu.size()) return false;
        out.clear();
        for (std::size_t i = 0; i < bc; i += 2) {
            out.push_back((std::uint16_t)(((std::uint16_t)pdu[2 + i] << 8) |
                                          (std::uint16_t)pdu[3 + i]));
        }
        return true;
    }
};

// =====================================================================
// 分帧器：字节流 → 帧（靠静默间隔边界 + CRC 引导的重同步）
// =====================================================================
class RtuFramer {
public:
    // 最大 RTU 帧 = 地址(1) + PDU(253) + CRC(2) = 256（规范 ADU 上限）
    static constexpr std::size_t kMaxFrameBytes = 256;
    // 最短合法帧 = addr(1) + fc(1) + crc(2)
    static constexpr std::size_t kMinFrameBytes = 4;

    // 3.5 个字符时间。一个字符按 11 位算（起始 1 + 数据 8 + 校验 1 + 停止 1），
    // 故 3.5 字符 = 38.5 位 → ms = 38500 / baud。
    static int char_gap_ms_for_baud(int baud) {
        if (baud <= 0) baud = 9600;
        return (int)std::ceil(38500.0 / (double)baud);
    }

    // address = 0 表示"接收所有地址"（调试/抓包用）；否则做地址过滤。
    explicit RtuFramer(int baud = 9600, std::uint8_t address = 0,
                       int min_gap_ms = 2)
        : baud_(baud > 0 ? baud : 9600),
          address_(address),
          min_gap_ms_(min_gap_ms > 0 ? min_gap_ms : 1) {
        int t = char_gap_ms_for_baud(baud_);
        gap_threshold_ms_ = (t < min_gap_ms_) ? min_gap_ms_ : t;
    }

    int         baud() const { return baud_; }
    int         gap_threshold_ms() const { return gap_threshold_ms_; }
    void        set_min_gap_ms(int ms) {
        min_gap_ms_ = (ms > 0) ? ms : 1;
        int t = char_gap_ms_for_baud(baud_);
        gap_threshold_ms_ = (t < min_gap_ms_) ? min_gap_ms_ : t;
    }
    void        set_address(std::uint8_t a) { address_ = a; }
    std::uint8_t address() const { return address_; }

    // -----------------------------------------------------------------
    // 喂字节
    //
    //   p / n  ：本次新到的字节（p 可为 nullptr，此时只做"静默判定"）
    //   gap_ms ：这批字节**之后**线路的空闲时长（ms）
    //
    // 只有当 gap_ms >= gap_threshold_ms() 时，缓冲区里的内容才被当作
    // 一个"帧边界"来解析 —— 这就是 RTU 分帧的全部物理依据。
    // 返回本次调用**收下（入队）**的帧数；地址不匹配的帧不入队、也不计数在内
    // （它们记在 frames_foreign_addr() 里 —— 那不是错误）。
    // -----------------------------------------------------------------
    int feed(const std::uint8_t* p, std::size_t n, int gap_ms) {
        if (p != nullptr && n > 0) {
            buf_.insert(buf_.end(), p, p + n);
            bytes_in_ += n;
        }
        // 缓冲区超长：说明要么波特率/校验位配错，要么线上全是噪声。
        // 丢掉最老的字节 —— 保留最新的才可能重新同步上。
        if (buf_.size() > kMaxFrameBytes) {
            const std::size_t drop = buf_.size() - kMaxFrameBytes;
            buf_.erase(buf_.begin(), buf_.begin() + (std::ptrdiff_t)drop);
            bytes_dropped_ += drop;
            ++overflow_drops_;
        }
        if (gap_ms < gap_threshold_ms_) return 0;   // 静默不够 → 帧还没结束
        return flush();
    }

    int feed(const std::vector<std::uint8_t>& v, int gap_ms) {
        return feed(v.empty() ? nullptr : v.data(), v.size(), gap_ms);
    }

    // -----------------------------------------------------------------
    // 取帧
    // -----------------------------------------------------------------
    bool pop(RtuFrame& out) {
        if (queue_.empty()) return false;
        out = queue_.front();
        queue_.erase(queue_.begin());
        return true;
    }
    std::size_t pending() const { return queue_.size(); }

    // 清空一切（换对端/重连时用）
    void reset_input() {
        buf_.clear();
        queue_.clear();
    }

    // -----------------------------------------------------------------
    // 计数器（现场诊断 + 测试断言）
    // -----------------------------------------------------------------
    int frames_ok() const { return frames_ok_; }                  // 收下的合法帧
    int frames_crc_err() const { return frames_crc_err_; }        // 看起来是帧但 CRC 不过
    int frames_short() const { return frames_short_; }            // 字节太少，成不了帧
    int frames_foreign_addr() const { return frames_foreign_addr_; }  // 地址不匹配
    int bytes_dropped() const { return bytes_dropped_; }          // 累计丢弃字节
    int resyncs() const { return resyncs_; }                      // 跳垃圾前缀后重同步的次数
    int overflow_drops() const { return overflow_drops_; }
    int bytes_in() const { return bytes_in_; }

private:
    // 由功能码推出**可能**的帧长（含 addr 与 CRC）。
    // 为什么要靠功能码而不是"穷举所有长度"：穷举 4..256 在一个 256 字节的
    // 缓冲区上会产生 3 万多次候选，每次 1/65536 的巧合概率 → 每次 flush
    // 期望 ~0.5 次**假阳性** —— 那会把垃圾当帧收下，比丢帧更危险。
    // 限制到 ≤3 个候选后，假阳性概率降到 1% 以下。
    static int candidate_lengths(const std::uint8_t* b, std::size_t avail,
                                 std::size_t out[3]) {
        int k = 0;
        if (avail < 2) return 0;
        const std::uint8_t fc_raw = b[1];
        if (fc_raw & 0x80) {                  // 异常响应 = addr + fc + code + crc
            out[k++] = 5;
            return k;
        }
        switch (fc_raw & 0x7F) {
            case 1: case 2: case 3: case 4:
                // 请求固定 8 字节；响应 = addr+fc+bytecount+N+crc = 5+N
                out[k++] = 8;
                if (avail >= 3) out[k++] = (std::size_t)5 + b[2];
                break;
            case 5: case 6:
                out[k++] = 8;
                break;
            case 15: case 16:
                // 响应 8；请求 = addr+fc+start(2)+qty(2)+bytecount(1)+N+crc(2) = 9+N
                out[k++] = 8;
                if (avail >= 7) out[k++] = (std::size_t)9 + b[6];
                break;
            default:
                return 0;                      // 未知功能码 → 重同步
        }
        // 过滤掉不可能的帧长并去重
        int m = 0;
        std::size_t tmp[3];
        for (int i = 0; i < k; ++i) {
            const std::size_t L = out[i];
            if (L < kMinFrameBytes || L > kMaxFrameBytes) continue;
            bool dup = false;
            for (int j = 0; j < m; ++j) if (tmp[j] == L) dup = true;
            if (!dup) tmp[m++] = L;
        }
        for (int i = 0; i < m; ++i) out[i] = tmp[i];
        return m;
    }

    // 静默已确认 → 把缓冲区里的内容全部解析成帧
    int flush() {
        int emitted = 0;
        for (;;) {
            if (buf_.empty()) break;

            if (buf_.size() < kMinFrameBytes) {
                // 静默已到还这么短 → 只能是噪声碎片
                bytes_dropped_ += (int)buf_.size();
                ++frames_short_;
                buf_.clear();
                break;
            }

            // ① 整个缓冲区正好是一帧（最常见：一次读完一帧）
            if (crc_ok(buf_.data(), buf_.size())) {
                if (emit(buf_.data(), buf_.size())) ++emitted;
                buf_.clear();
                break;
            }

            // ② 从**最小偏移**处找出一帧。两种判据，按优先级：
            //    (a) 由功能码推出的候选长度 —— 结构性判据，假阳性最低；
            //    (b) "从 off 到缓冲区末尾正好一帧" —— 帧尾天然落在静默点上。
            //    ★ 必须**逐偏移扫描**而不是只看 off=0：现场最常见的形态是
            //      「垃圾前缀 + 帧 + 垃圾尾巴」，只看 off=0 时 (a)(b) 都不成立，
            //      整批字节会被当垃圾丢掉 —— 帧就白丢了（这正是本文件坑③的
            //      第二半，实测抓出来的）。
            std::size_t uoff = 0, ulen = 0;
            bool found = false;
            for (std::size_t off = 0; off + kMinFrameBytes <= buf_.size() && !found; ++off) {
                std::size_t cand[3];
                const int nc = candidate_lengths(buf_.data() + off, buf_.size() - off, cand);
                for (int i = 0; i < nc; ++i) {
                    const std::size_t L = cand[i];
                    if (off + L > buf_.size()) continue;   // 还没收齐
                    if (crc_ok(buf_.data() + off, L)) {
                        uoff = off; ulen = L; found = true; break;
                    }
                }
                if (!found && off > 0 && crc_ok(buf_.data() + off, buf_.size() - off)) {
                    uoff = off; ulen = buf_.size() - off; found = true;
                }
            }
            if (found) {
                if (uoff > 0) { bytes_dropped_ += (int)uoff; ++resyncs_; }
                if (emit(buf_.data() + uoff, ulen)) ++emitted;
                buf_.erase(buf_.begin(), buf_.begin() + (std::ptrdiff_t)(uoff + ulen));
                continue;                     // 缓冲区里可能还有下一帧
            }

            // ③ 确实找不出一帧 → 整个缓冲区是垃圾（或一帧被 CRC 破坏）。
            //    ★ 必须清空：留着会污染后续帧（坑③）。
            ++frames_crc_err_;
            bytes_dropped_ += (int)buf_.size();
            buf_.clear();
            break;
        }
        return emitted;
    }

    // 返回 true = 已入队（本机地址）；false = 地址不匹配，只计数不入队
    bool emit(const std::uint8_t* frame, std::size_t len) {
        const std::uint8_t addr = frame[0];
        if (address_ != 0 && addr != address_) {
            // 总线上别的从站的应答：**不是错误**，只是不归我。
            // 单列一个计数，现场用它判断"485 上还挂了谁"。
            ++frames_foreign_addr_;
            return false;
        }
        RtuFrame f;
        f.addr = addr;
        f.pdu.assign(frame + 1, frame + (len - 2));   // 去掉地址与 CRC
        queue_.push_back(f);
        ++frames_ok_;
        return true;
    }

    int           baud_             = 9600;
    std::uint8_t  address_          = 0;
    int           min_gap_ms_       = 2;
    int           gap_threshold_ms_ = 5;

    std::vector<std::uint8_t> buf_;
    std::vector<RtuFrame>     queue_;

    int frames_ok_          = 0;
    int frames_crc_err_     = 0;
    int frames_short_       = 0;
    int frames_foreign_addr_= 0;
    int bytes_dropped_      = 0;
    int resyncs_            = 0;
    int overflow_drops_     = 0;
    int bytes_in_           = 0;
};

// =====================================================================
// 串口抽象
// =====================================================================
class ISerialPort {
public:
    virtual ~ISerialPort() {}
    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;
    // 返回写入字节数；<0 表示失败
    virtual int  write(const std::uint8_t* p, std::size_t n) = 0;
    // 等到至少 1 字节或超时。返回读到的字节数；**0 表示超时**（不是错误）；<0 失败。
    virtual int  read(std::uint8_t* p, std::size_t n, int timeout_ms) = 0;
    virtual std::string name() const = 0;
};

// ---------------------------------------------------------------------
// 回环/假串口 —— 测试用，也是"可执行的串口契约文档"
//   · write() 的数据默认进 tx_（供断言"我发出去的是不是这串字节"）
//   · set_loopback(true) 时 write() 的数据还会回到 rx_（真回环）
//   · inject_rx() 直接往接收队列塞字节，模拟从站的应答
// ---------------------------------------------------------------------
class LoopbackSerialPort : public ISerialPort {
public:
    explicit LoopbackSerialPort(std::string name = "loopback",
                                bool loopback = false)
        : name_(std::move(name)), loopback_(loopback) {}

    bool open() override  { open_ = true; return true; }
    void close() override { open_ = false; }
    bool is_open() const override { return open_; }

    int write(const std::uint8_t* p, std::size_t n) override {
        if (!open_) return -1;
        if (fail_writes_) return -1;
        if (p && n) {
            tx_.insert(tx_.end(), p, p + n);
            if (loopback_) rx_.insert(rx_.end(), p, p + n);
        }
        return (int)n;
    }

    int read(std::uint8_t* p, std::size_t n, int timeout_ms) override {
        if (!open_) return -1;
        if (rx_.empty()) {
            // 真实串口在无数据时会"等到超时"。假串口默认**不等**（让分帧
            // 测试跑得快），但 RtuMaster 的超时路径需要它真的消耗时间，
            // 否则主站会在极短时间里空转上万圈 —— 设成 1 ms 就与真串口同形。
            if (empty_read_sleep_ms_ > 0) {
#ifdef _WIN32
                ::Sleep((DWORD)empty_read_sleep_ms_);
#else
                ::usleep((useconds_t)(empty_read_sleep_ms_ * 1000));
#endif
            }
            return 0;                  // 超时
        }
        const std::size_t take = (n < rx_.size()) ? n : rx_.size();
        std::memcpy(p, rx_.data(), take);
        rx_.erase(rx_.begin(), rx_.begin() + (std::ptrdiff_t)take);
        return (int)take;
    }

    std::string name() const override { return name_; }

    // --- 测试注入点 ---
    void set_loopback(bool on) { loopback_ = on; }
    void set_fail_writes(bool f) { fail_writes_ = f; }
    void set_empty_read_sleep_ms(int ms) { empty_read_sleep_ms_ = (ms > 0) ? ms : 0; }
    void inject_rx(const std::uint8_t* p, std::size_t n) {
        if (p && n) rx_.insert(rx_.end(), p, p + n);
    }
    void inject_rx(const std::vector<std::uint8_t>& v) { inject_rx(v.data(), v.size()); }
    const std::vector<std::uint8_t>& tx() const { return tx_; }
    void clear_tx() { tx_.clear(); }
    std::size_t rx_pending() const { return rx_.size(); }

private:
    std::string               name_;
    bool                      open_        = false;
    bool                      loopback_    = false;
    bool                      fail_writes_ = false;
    int                       empty_read_sleep_ms_ = 0;
    std::vector<std::uint8_t> tx_;
    std::vector<std::uint8_t> rx_;
};

#ifdef _WIN32
// ---------------------------------------------------------------------
// Windows 真串口实现（CreateFile("\\\\.\\COMx") + DCB）
//
// ★ 为什么路径要写成 "\\\\.\\COM10"：对 COM1–COM9，直接写 "COM3" 也能开，
//   但一到 **COM10 及以上**，不写 `\\.\` 前缀就会失败（Win32 把它当文件名）。
//   现场 USB 转串口一插就是 COM12/COM20，这个坑必须一次性避开。
// ---------------------------------------------------------------------
class WinSerialPort : public ISerialPort {
public:
    // parity: 'N'/'E'/'O'；stop_bits: 1/2；data_bits: 7/8
    WinSerialPort(std::string com_port, int baud = 9600, char parity = 'N',
                  int data_bits = 8, int stop_bits = 1)
        : name_(std::move(com_port)), baud_(baud), parity_(parity),
          data_bits_(data_bits), stop_bits_(stop_bits) {}

    ~WinSerialPort() override { close(); }

    bool open() override {
        close();
        std::string path = name_;
        if (path.size() < 4 || path.compare(0, 4, "\\\\.\\") != 0) {
            path = "\\\\.\\" + path;      // ★ COM10+ 必须
        }
        handle_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            handle_ = nullptr;
            last_error_ = (int)GetLastError();
            return false;
        }
        DCB dcb;
        std::memset(&dcb, 0, sizeof(dcb));
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(handle_, &dcb)) { close(); return false; }
        dcb.BaudRate = (DWORD)baud_;
        dcb.ByteSize = (BYTE)data_bits_;
        dcb.Parity   = (parity_ == 'E') ? EVENPARITY : (parity_ == 'O') ? ODDPARITY : NOPARITY;
        dcb.StopBits = (stop_bits_ == 2) ? TWOSTOPBITS : ONESTOPBIT;
        dcb.fBinary  = TRUE;
        dcb.fParity  = (dcb.Parity == NOPARITY) ? FALSE : TRUE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl  = DTR_CONTROL_ENABLE;
        dcb.fRtsControl  = RTS_CONTROL_ENABLE;   // ★ 很多 485 转换器靠 RTS 收发切换
        dcb.fOutX = FALSE;
        dcb.fInX  = FALSE;
        if (!SetCommState(handle_, &dcb)) { close(); return false; }

        // 超时全部交给 read() 里的轮询循环控制：这里设成"立刻返回可用字节"
        COMMTIMEOUTS t;
        std::memset(&t, 0, sizeof(t));
        t.ReadIntervalTimeout         = MAXDWORD;
        t.ReadTotalTimeoutMultiplier  = 0;
        t.ReadTotalTimeoutConstant    = 0;
        t.WriteTotalTimeoutMultiplier = 0;
        t.WriteTotalTimeoutConstant   = 0;
        if (!SetCommTimeouts(handle_, &t)) { close(); return false; }

        SetupComm(handle_, 4096, 4096);
        PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
        open_ = true;
        return true;
    }

    void close() override {
        if (handle_) { CloseHandle(handle_); handle_ = nullptr; }
        open_ = false;
    }

    bool is_open() const override { return open_; }

    int write(const std::uint8_t* p, std::size_t n) override {
        if (!open_ || !p) return -1;
        DWORD written = 0;
        if (!WriteFile(handle_, p, (DWORD)n, &written, nullptr)) return -1;
        return (int)written;
    }

    // 轮询直到有字节或超时。为什么不用阻塞读：Windows 的阻塞读在
    // ReadIntervalTimeout=MAXDWORD 下会**立刻**返回 0 字节，
    // 而我们要的是"最多等 timeout_ms" —— 只能自己等。
    int read(std::uint8_t* p, std::size_t n, int timeout_ms) override {
        if (!open_ || !p) return -1;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(handle_, p, (DWORD)n, &got, nullptr)) return -1;
            if (got > 0) return (int)got;
            if (std::chrono::steady_clock::now() >= deadline) return 0;
            Sleep(1);
        }
    }

    std::string name() const override { return name_; }
    int last_error() const { return last_error_; }

private:
    std::string name_;
    int         baud_       = 9600;
    char        parity_     = 'N';
    int         data_bits_  = 8;
    int         stop_bits_  = 1;
    HANDLE      handle_     = nullptr;
    bool        open_       = false;
    int         last_error_ = 0;
};
#endif  // _WIN32

// =====================================================================
// 主站事务（串口 + 分帧器的装配；一次请求一次响应）
//
// ★ 为什么把"读空即帧边界"写进循环：真串口的帧结束只能靠静默判定，
//   而 read() 返回 0（超时）正是"线路安静了 timeout_ms"的证据。
//   所以 poll_ms 必须 >= gap_threshold_ms()。
// =====================================================================
class RtuMaster {
public:
    RtuMaster(ISerialPort& port, std::uint8_t address, int baud = 9600,
              int timeout_ms = 300)
        : port_(port), address_(address), baud_(baud),
          timeout_ms_(timeout_ms > 0 ? timeout_ms : 1),
          framer_(baud, address) {}

    RtuFramer&       framer()       { return framer_; }
    const RtuFramer& framer() const { return framer_; }

    int requests() const { return requests_; }
    int timeouts() const { return timeouts_; }
    int bad_frames() const { return bad_frames_; }
    int exceptions() const { return exceptions_; }
    int foreign_frames() const { return foreign_frames_; }

    // 发一帧请求，等一帧响应（地址必须是自己）。err 为中文失败原因。
    bool transact(const std::vector<std::uint8_t>& request_pdu, RtuFrame& response,
                  std::string& err) {
        err.clear();
        if (!port_.is_open()) { err = "串口未打开"; return false; }

        const std::vector<std::uint8_t> frame = wrap_rtu_frame(address_, request_pdu);
        ++requests_;
        framer_.reset_input();
        if (port_.write(frame.data(), frame.size()) != (int)frame.size()) {
            err = "串口写入不完整";
            return false;
        }

        const int poll_ms = (framer_.gap_threshold_ms() > timeout_ms_)
                                ? timeout_ms_ : framer_.gap_threshold_ms();
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms_);
        std::uint8_t tmp[256];
        // 迭代上限只是防"假串口永不推进"的死循环，不是正常退出路径
        const int kMaxPollIterations = 1000000;
        bool deadline_hit = false;
        for (int guard = 0; guard < kMaxPollIterations; ++guard) {
            const int n = port_.read(tmp, sizeof(tmp), poll_ms);
            if (n < 0) { err = "串口读取失败"; return false; }
            if (n > 0) {
                framer_.feed(tmp, (std::size_t)n, 0);          // 还有数据，静默未到
            } else {
                // ★ 读空 = 线路安静了 poll_ms（>= 3.5T）→ 此刻缓冲区里的即一帧
                framer_.feed(nullptr, 0, poll_ms);
            }
            RtuFrame f;
            while (framer_.pop(f)) {
                if (f.addr != address_) { ++foreign_frames_; continue; }
                response = f;
                if (f.is_exception()) ++exceptions_;
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) { deadline_hit = true; break; }
        }
        ++timeouts_;
        // ★ 两类失败必须分开报：一个是"对端没应"，一个是"串口驱动没推进"，
        //   现场排查方向完全不同。混成一句"超时"会把人指向错误的硬件。
        err = deadline_hit ? "等待应答超时（从站无响应或帧被 CRC 丢弃）"
                           : "轮询次数超出上限：串口 read() 一直没有推进";
        return false;
    }

    // 读保持/输入寄存器（FC03 / FC04）
    bool read_registers(bool input_registers, std::uint16_t start,
                        std::uint16_t count, std::vector<std::uint16_t>& out,
                        std::string& err) {
        std::vector<std::uint8_t> pdu;
        const std::uint8_t fc = input_registers ? kFcReadInputRegisters
                                                : kFcReadHoldingRegisters;
        if (!build_read_pdu(fc, start, count, pdu)) {
            err = "读请求参数越界（count 1..125）";
            return false;
        }
        RtuFrame resp;
        if (!transact(pdu, resp, err)) return false;
        if (resp.is_exception()) {
            err = "从站异常码 " + std::to_string((int)resp.exception_code());
            return false;
        }
        if (!resp.to_registers(out) || out.size() != count) {
            ++bad_frames_;
            err = "响应长度与请求不符";
            return false;
        }
        return true;
    }

    // 写多个寄存器（FC16）
    bool write_registers(std::uint16_t start,
                         const std::vector<std::uint16_t>& values,
                         std::string& err) {
        std::vector<std::uint8_t> pdu;
        if (!build_write_multiple_registers_pdu(start, values, pdu)) {
            err = "写请求参数越界（1..123 个寄存器）";
            return false;
        }
        RtuFrame resp;
        if (!transact(pdu, resp, err)) return false;
        if (resp.is_exception()) {
            err = "从站异常码 " + std::to_string((int)resp.exception_code());
            return false;
        }
        // 正常响应回显 start 与 count（8 字节），校验一下
        if (resp.pdu.size() != 5) { ++bad_frames_; err = "写响应长度异常"; return false; }
        const std::uint16_t r_start =
            (std::uint16_t)(((std::uint16_t)resp.pdu[1] << 8) | resp.pdu[2]);
        const std::uint16_t r_count =
            (std::uint16_t)(((std::uint16_t)resp.pdu[3] << 8) | resp.pdu[4]);
        if (r_start != start || r_count != (std::uint16_t)values.size()) {
            ++bad_frames_;
            err = "写响应回显与请求不符";
            return false;
        }
        return true;
    }

private:
    ISerialPort& port_;
    std::uint8_t address_;
    int          baud_;
    int          timeout_ms_;
    RtuFramer    framer_;

    int requests_       = 0;
    int timeouts_       = 0;
    int bad_frames_     = 0;
    int exceptions_     = 0;
    int foreign_frames_ = 0;
};

}  // namespace comm
}  // namespace ems
