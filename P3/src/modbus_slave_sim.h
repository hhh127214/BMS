// =====================================================================
// P3/ — 产品化 P3（通信）：Modbus 从站仿真 + 环回传输（测试/演示桩）
//
// 为什么需要它（这是 P3 能在没有硬件的前提下做**真实验证**的唯一办法）：
//   现场把 PCS / BMS / 电表接到 EMS 的是一根网线，两个进程谁也看不见谁。
//   本文件做两件事：
//     ① ModbusSlaveSim        —— 从站侧：一块寄存器文件 + 一个设备替身
//                                 （MemoryDeviceIO，即"另一台机器上的设备"）；
//     ② LoopbackModbusTransport —— 把"主站 ↔ 从站"接在本进程内。
//   环回**只跳过内核 socket，不跳过一个字节的编解码** —— 主站发的报文与现场
//   完全一样，从站解析、回帧的流程也完全一样。因此 T34 的"逐位等价"证明的是
//   适配器真实行为，不是某个"直通捷径"。
//
// 谁拥有哪个区（写错会让事后追溯变成一团乱麻，所以用代码强制）：
//   设备侧只写 MEAS / CFG / STA；主站只写 CMD。
//   handle() 对非 CMD 区的写请求一律回 0x02 非法数据地址。
//
// 编译：纯头文件。需要 04/src、07/src、07/src/rtdb 在 include 路径上。
// =====================================================================

#pragma once

#include "memory_device_io.h"   // 07/src  设备替身（点表 = 设备内部真值）
#include "modbus_device_io.h"   // 同目录  映射表 / 传输接口 / 编解码

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ems {

// =====================================================================
// 从站：寄存器文件 + 设备替身 + 请求处理
// =====================================================================
class ModbusSlaveSim {
public:
    static constexpr size_t kRegCap = 1024;

    ModbusSlaveSim() : map_(build_modbus_map(MapProfile::kStandardF32)) { reset_regs(); }
    explicit ModbusSlaveSim(MapProfile profile) : map_(build_modbus_map(profile)) {
        reset_regs();
    }

    void set_profile(MapProfile profile) { map_ = build_modbus_map(profile); reset_regs(); }
    void set_unit_id(uint8_t u) { unit_ = u; }
    uint8_t unit_id() const { return unit_; }
    const ModbusRegisterMap& map() const { return map_; }

    // 设备替身（真实现场是 PCS/BMS/电表本体，运行在另一台机器上）
    void attach_device(MemoryDeviceIO* dev) { dev_ = dev; }
    MemoryDeviceIO* device() const { return dev_; }

    // -----------------------------------------------------------------
    // 两个方向的数据搬运（现场由"设备进程"与"EMS 进程"各自完成）
    // -----------------------------------------------------------------
    // 设备 → 寄存器：MEAS / CFG / STA。跳过 CMD 区（那是主站的下行区）。
    void publish_device_points() {
        if (dev_ == nullptr) return;
        for (int b = 0; b < ModbusRegisterMap::kBlockCount; ++b) {
            if (b == 1) continue;
            uint16_t n = 0;
            for (int i = 0; i < ModbusRegisterMap::kPointCount; ++i) {
                if (map_.pts[i].block != b) continue;
                const double v = dev_->get(EMS_POINT_NAMES[i]);
                modbus::encode_value(v, map_.pts[i].type, regs_ + map_.pts[i].addr);
                n = static_cast<uint16_t>(n + map_.width);
            }
            (void)n;
        }
    }

    // 寄存器 → 设备：把主站下行的 CMD.* 落到设备点表（设备侧读指令用）
    void apply_command_regs() {
        if (dev_ == nullptr) return;
        for (int i = 0; i < ModbusRegisterMap::kPointCount; ++i) {
            if (map_.pts[i].block != 1) continue;
            dev_->set(EMS_POINT_NAMES[i],
                      modbus::decode_value(regs_ + map_.pts[i].addr, map_.pts[i].type));
        }
    }

    // -----------------------------------------------------------------
    // 请求处理：返回响应帧长度；0 = 不响应（总线上"沉默"）
    // -----------------------------------------------------------------
    size_t handle(const uint8_t* req, size_t req_len, uint8_t* resp, size_t resp_cap) {
        modbus::MbapHeader h;
        if (!modbus::parse_mbap(req, req_len, h)) return 0;
        if (h.unit != unit_) { ++bad_unit_drops_; return 0; }   // 非本站报文：不吭声

        const uint8_t  fc   = req[modbus::kMbapLen];
        const uint8_t* p    = req + modbus::kMbapLen + 1;
        const size_t   plen = static_cast<size_t>(h.len) - 2;   // 扣掉 unit 与 fc
        ++frames_handled_;

        switch (fc) {
            case modbus::kReadHoldingRegs:
            case modbus::kReadInputRegs: {
                if (plen < 4) return exception(h, fc, modbus::kExcIllegalDataValue,
                                               resp, resp_cap);
                const uint16_t addr = modbus::get_u16_be(p);
                const uint16_t qty  = modbus::get_u16_be(p + 2);
                if (qty < 1 || qty > modbus::kMaxReadRegs)
                    return exception(h, fc, modbus::kExcIllegalDataValue, resp, resp_cap);
                if (static_cast<uint32_t>(addr) + qty > regs_used_)
                    return exception(h, fc, modbus::kExcIllegalDataAddr, resp, resp_cap);
                // 响应：fc + byte_count + data
                const size_t byte_count = static_cast<size_t>(qty) * 2;
                const size_t total = modbus::kMbapLen + 2 + byte_count;
                if (resp == nullptr || resp_cap < total) return 0;
                modbus::put_u16_be(resp + 0, h.tid);
                modbus::put_u16_be(resp + 2, 0x0000);
                modbus::put_u16_be(resp + 4, static_cast<uint16_t>(3 + byte_count));
                resp[6] = h.unit;
                resp[7] = fc;
                resp[8] = static_cast<uint8_t>(byte_count);
                for (size_t i = 0; i < qty; ++i) {
                    modbus::put_u16_be(resp + 9 + i * 2, regs_[addr + i]);
                }
                return total;
            }
            case modbus::kWriteMultipleRegs: {
                if (plen < 5) return exception(h, fc, modbus::kExcIllegalDataValue,
                                               resp, resp_cap);
                const uint16_t addr       = modbus::get_u16_be(p);
                const uint16_t qty        = modbus::get_u16_be(p + 2);
                const uint16_t byte_count = p[4];
                if (qty < 1 || qty > modbus::kMaxWriteRegs ||
                    byte_count != static_cast<uint16_t>(qty * 2) || plen < 5u + byte_count) {
                    return exception(h, fc, modbus::kExcIllegalDataValue, resp, resp_cap);
                }
                if (static_cast<uint32_t>(addr) + qty > regs_used_)
                    return exception(h, fc, modbus::kExcIllegalDataAddr, resp, resp_cap);
                for (uint16_t i = 0; i < qty; ++i) {
                    if (!is_writable_reg(static_cast<uint16_t>(addr + i))) {
                        return exception(h, fc, modbus::kExcIllegalDataAddr, resp, resp_cap);
                    }
                }
                for (uint16_t i = 0; i < qty; ++i) {
                    regs_[addr + i] = modbus::get_u16_be(p + 5 + i * 2);
                }
                ++reg_writes_;
                // 正常响应：fc + addr + qty
                const size_t total = modbus::kMbapLen + 5;
                if (resp == nullptr || resp_cap < total) return 0;
                modbus::put_u16_be(resp + 0, h.tid);
                modbus::put_u16_be(resp + 2, 0x0000);
                modbus::put_u16_be(resp + 4, 6);
                resp[6] = h.unit;
                resp[7] = fc;
                modbus::put_u16_be(resp + 8, addr);
                modbus::put_u16_be(resp + 10, qty);
                return total;
            }
            case modbus::kWriteSingleReg: {
                if (plen < 4) return exception(h, fc, modbus::kExcIllegalDataValue,
                                               resp, resp_cap);
                const uint16_t addr = modbus::get_u16_be(p);
                const uint16_t val  = modbus::get_u16_be(p + 2);
                if (addr >= regs_used_)
                    return exception(h, fc, modbus::kExcIllegalDataAddr, resp, resp_cap);
                if (!is_writable_reg(addr))
                    return exception(h, fc, modbus::kExcIllegalDataAddr, resp, resp_cap);
                regs_[addr] = val;
                ++reg_writes_;
                const size_t total = modbus::kMbapLen + 5;
                if (resp == nullptr || resp_cap < total) return 0;
                modbus::put_u16_be(resp + 0, h.tid);
                modbus::put_u16_be(resp + 2, 0x0000);
                modbus::put_u16_be(resp + 4, 6);
                resp[6] = h.unit;
                resp[7] = fc;
                modbus::put_u16_be(resp + 8, addr);
                modbus::put_u16_be(resp + 10, val);
                return total;
            }
            default:
                return exception(h, fc, modbus::kExcIllegalFunction, resp, resp_cap);
        }
    }

    // -----------------------------------------------------------------
    // 诊断 / 注入
    // -----------------------------------------------------------------
    int  frames_handled() const { return frames_handled_; }
    int  bad_unit_drops() const { return bad_unit_drops_; }
    int  exception_count() const { return exception_count_; }
    int  reg_writes() const { return reg_writes_; }
    uint16_t reg_at(uint16_t a) const { return (a < kRegCap) ? regs_[a] : 0xFFFF; }
    // 越界直写（测试注入用：模拟寄存器被外部改写 / 设备侧异常）
    void force_reg(uint16_t a, uint16_t v) { if (a < kRegCap) regs_[a] = v; }

    uint16_t regs_used() const { return regs_used_; }

private:
    void reset_regs() {
        std::memset(regs_, 0, sizeof(regs_));
        regs_used_ = map_.total_regs();
        // 初值 = 点表默认值（未采集前不瞎猜：现场"连不上却报 0"比"报默认值"更危险）
        for (int i = 0; i < ModbusRegisterMap::kPointCount; ++i) {
            modbus::encode_value(EMS_POINT_DEFAULTS[i], map_.pts[i].type,
                                 regs_ + map_.pts[i].addr);
        }
    }

    // 只有 CMD 区可写 —— 方向纪律由代码强制，不靠约定。
    bool is_writable_reg(uint16_t addr) const {
        for (int i = 0; i < ModbusRegisterMap::kPointCount; ++i) {
            if (addr >= map_.pts[i].addr &&
                addr < static_cast<uint16_t>(map_.pts[i].addr + map_.width)) {
                return map_.pts[i].writable;
            }
        }
        return false;
    }

    size_t exception(const modbus::MbapHeader& h, uint8_t fc, modbus::ExceptionCode code,
                     uint8_t* resp, size_t resp_cap) {
        ++exception_count_;
        return modbus::build_exception_response(resp, resp_cap, h, fc, code);
    }

    ModbusRegisterMap map_;
    uint8_t           unit_ = 1;
    MemoryDeviceIO*   dev_  = nullptr;

    uint16_t regs_[kRegCap] = {};
    uint16_t regs_used_     = 0;

    int frames_handled_  = 0;
    int bad_unit_drops_  = 0;
    int exception_count_ = 0;
    int reg_writes_      = 0;
};

// =====================================================================
// 环回传输：把主站与从站接在本进程内（**字节流与现场完全一样**）
//
// 支持三种故障注入 —— 它们对应现场最常遇到的三种"数据不可信"：
//   inject_timeout(n)  从站沉默（网线掉了 / 从站不过来）→ transact 返回 false
//   inject_exception() 从站回异常码（地址表不匹配 / 量程不对）
//   set_link_up(false) 链路整体断开
// =====================================================================
class LoopbackModbusTransport : public IModbusTransport {
public:
    LoopbackModbusTransport() = default;
    explicit LoopbackModbusTransport(ModbusSlaveSim* slave) : slave_(slave) {}

    void attach(ModbusSlaveSim* slave) { slave_ = slave; }
    ModbusSlaveSim* slave() const { return slave_; }

    bool connect() override { up_ = (slave_ != nullptr); return up_; }
    void close()   override { up_ = false; }
    bool connected() const override { return up_; }

    void set_link_up(bool u) { up_ = u; }
    void inject_timeout(int n) { timeout_budget_ = n; }
    void inject_exception(modbus::ExceptionCode c, int n = 1) {
        exc_code_ = c; exc_budget_ = n;
    }

    int tx_frames() const { return tx_frames_; }
    int rx_frames() const { return rx_frames_; }
    int timeouts()  const { return timeouts_; }

    bool transact(const uint8_t* req, size_t req_len,
                  uint8_t* resp, size_t resp_cap, size_t* resp_len) override {
        if (resp_len != nullptr) *resp_len = 0;
        if (!up_ || slave_ == nullptr) { ++timeouts_; return false; }
        if (timeout_budget_ > 0) { --timeout_budget_; ++timeouts_; return false; }

        ++tx_frames_;

        if (exc_budget_ > 0) {
            --exc_budget_;
            modbus::MbapHeader h;
            if (!modbus::parse_mbap(req, req_len, h)) { ++timeouts_; return false; }
            const uint8_t fc = (req != nullptr && req_len > modbus::kMbapLen)
                                   ? req[modbus::kMbapLen] : 0x03;
            const size_t n = modbus::build_exception_response(resp, resp_cap, h, fc, exc_code_);
            if (n == 0) { ++timeouts_; return false; }
            if (resp_len != nullptr) *resp_len = n;
            ++rx_frames_;
            return true;
        }

        const size_t n = slave_->handle(req, req_len, resp, resp_cap);
        if (n == 0) { ++timeouts_; return false; }   // 从站沉默
        if (resp_len != nullptr) *resp_len = n;
        ++rx_frames_;
        return true;
    }

private:
    ModbusSlaveSim* slave_ = nullptr;
    bool up_ = false;
    int  timeout_budget_ = 0;
    int  exc_budget_     = 0;
    modbus::ExceptionCode exc_code_ = modbus::kExcNone;
    int  tx_frames_ = 0;
    int  rx_frames_ = 0;
    int  timeouts_  = 0;
};

} // namespace ems
