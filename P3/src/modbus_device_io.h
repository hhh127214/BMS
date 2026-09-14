// =====================================================================
// P3/ — 产品化 P3（通信）：Modbus 寄存器映射 + 传输抽象 + ModbusDeviceIO
//
// 定位：IDeviceIO 的**现场设备适配器**之一。EMS 算法（05/06/07/08）只经
//       IDeviceIO 读写设备；本文件把「PCS / BMS / 电表的 Modbus 从站」接进
//       这个接口。与 SimDeviceIO(P0) / MemoryDeviceIO(P0.5) / RtDbDeviceIO
//       并列，是第四个适配器实现。
//
// ★ 与 RtDbDeviceIO 的差别，只在"介质"这一层：
//     · RtDbDeviceIO  —— 共享内存实时库（进程内也有、跨进程也有）
//     · ModbusDeviceIO —— 串行/以太网报文（**只能有一个进程是主站**）
//   两者共用同一份点名真相源（07/src/rtdb/ems_point_table.h，30 点），
//   因此可以互相做对照测试、"逐位等价"这条验收标准可以直接复用。
//
// ★ 本文件与 modbus_slave_sim.h 的分工（刻意拆开）：
//     modbus_device_io.h  —— 主站侧：映射表 + 传输接口 + 适配器（**生产代码**）
//     modbus_slave_sim.h  —— 从站侧：寄存器文件 + 设备替身 + 环回传输（测试桩）
//   把从站仿真放进生产头文件会让生产适配器凭空依赖 MemoryDeviceIO
//   （一个设备替身）。分开后，装配现场时只 include 本文件。
//
// ★ 关键纪律（与 device_io.h 的接口契约一一对应）：
//   1. 点名/寄存器地址**只允许出现在本文件与 slave_sim 内**。算法层看到的
//      永远是 RealtimeSnapshot / DeviceLimits / DeviceStatus / PowerCommand。
//      哪天算法里出现 0x0100 或 "CMD.P_BAT"，说明分层已经破了。
//   2. execute() 的返回值**不可信**：真实 Modbus 里它只是"把写请求发出去"，
//      实际功率要等下一拍读 MEAS 寄存器。闭环走 read_snapshot()。
//   3. 采集失败必须填**最近一次有效值**（接口契约①），并把可信性交给
//      read_status().data_valid 表达 —— 算法层不为"采集失败"写分支。
//
// 编译：纯头文件，实现全部 inline。
//   include 路径需含：04/src、07/src/rtdb（点名真相源）
//   链接：07/src/rtdb/ems_point_table.c（提供 EMS_POINT_NAMES/UNITS/DEFAULTS）
// =====================================================================

#pragma once

#include "device_io.h"        // 04/  IDeviceIO / DeviceStatus / DeviceActuals
#include "ems_point_table.h"  // 07/src/rtdb  30 点真相源（C 头，extern "C"）
#include "modbus_codec.h"     // 同目录：帧编解码（纯函数）

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

namespace ems {

// =====================================================================
// 寄存器映射表
//
// 每个点声明三件事：**点名 / 起始地址 / 数值类型**。地址按"块"排布，
// 块基址 256 对齐（0x0000 / 0x0100 / 0x0200 / 0x0300），落在哪个块由点名
// 前缀决定（MEAS / CMD / CFG / STA）—— 这样现场拿 Modbus 扫描器一眼就能
// 看出读的是哪一类量，排障时省掉一张纸。
//
// 方向约定（与 RT_DB 完全一致，跨介质不变）：
//   设备 → EMS：MEAS（量测）、CFG（配置/限值）、STA（状态）
//   EMS → 设备：CMD（指令，含权限区间）
// 从站只接受对 CMD 区（块 1）的写；写其它区一律回 0x02 非法地址。
// =====================================================================

enum class MapProfile {
    kStandardF32,   // 现场标准映射：每点 2 寄存器，IEEE754 单精度 ABCD
    kWideF64,       // 宽精度映射：每点 4 寄存器，IEEE754 双精度 ABCDEFGH
};

struct RegPoint {
    const char* point = nullptr;   // 点名（与 EMS_POINT_NAMES 逐字相同）
    uint16_t    addr  = 0;         // 起始寄存器地址（0-based）
    modbus::RegType type = modbus::RegType::kFloat32BE;
    int         block = 0;         // 0=MEAS 1=CMD 2=CFG 3=STA
    bool        writable = false;  // 仅 CMD 区为 true
};

struct ModbusRegisterMap {
    static constexpr int kPointCount = 30;
    static constexpr int kBlockCount = 4;

    static constexpr uint16_t kBlockBase[kBlockCount] = {0x0000, 0x0100, 0x0200, 0x0300};
    static constexpr uint16_t kBlockN[kBlockCount]    = {7, 3, 14, 6};   // 每块点数

    RegPoint pts[kPointCount] = {};
    int      width = 2;      // 每点寄存器数（由 MapProfile 决定，全表统一）

    uint16_t block_end(int b) const {
        return static_cast<uint16_t>(kBlockBase[b] + kBlockN[b] * width);
    }
    uint16_t block_qty(int b) const {
        return static_cast<uint16_t>(kBlockN[b] * width);
    }
    uint16_t total_regs() const { return block_end(kBlockCount - 1); }

    bool block_has_index(int b, int index) const {
        return index >= 0 && index < kPointCount && pts[index].block == b;
    }

    // 点名 → 映射项（找不到返回 nullptr）。只在装配/自检路径用。
    const RegPoint* find_point(const char* name) const {
        if (name == nullptr) return nullptr;
        for (int i = 0; i < kPointCount; ++i) {
            if (pts[i].point != nullptr && std::strcmp(pts[i].point, name) == 0) return &pts[i];
        }
        return nullptr;
    }
    // 寄存器地址 → 映射项（从站处理请求时用）
    const RegPoint* find_addr(uint16_t addr) const {
        for (int i = 0; i < kPointCount; ++i) {
            if (addr >= pts[i].addr &&
                addr < static_cast<uint16_t>(pts[i].addr + width)) return &pts[i];
        }
        return nullptr;
    }
    int index_of_point(const char* name) const {
        if (name == nullptr) return -1;
        for (int i = 0; i < kPointCount; ++i) {
            if (pts[i].point != nullptr && std::strcmp(pts[i].point, name) == 0) return i;
        }
        return -1;
    }
};

// 点后缀固定归属某个块：块的划分是**点名规范的一部分**，不是可选项。
inline int modbus_point_block(int index) {
    if (index <= EMS_SOH)          return 0;   // MEAS.P_LOAD .. MEAS.SOH
    if (index <= EMS_CMD_P_LOWER)  return 1;   // CMD.*
    if (index <= EMS_CFG_SOC_MAX)  return 2;   // CFG.*
    return 3;                                  // STA.*
}

// 建表：点名直接取自 ems_point_table.h（真相源），地址按块内顺序紧密排布。
// 这样"点名对不对得上"是**编译期结构**决定的，而不是靠人工抄表。
inline ModbusRegisterMap build_modbus_map(MapProfile profile) {
    ModbusRegisterMap m;
    const bool wide = (profile == MapProfile::kWideF64);
    m.width = wide ? 4 : 2;

    for (int b = 0; b < ModbusRegisterMap::kBlockCount; ++b) {
        uint16_t off = 0;
        for (int i = 0; i < ModbusRegisterMap::kPointCount; ++i) {
            if (modbus_point_block(i) != b) continue;
            m.pts[i].point    = EMS_POINT_NAMES[i];
            m.pts[i].addr     = static_cast<uint16_t>(ModbusRegisterMap::kBlockBase[b] + off);
            m.pts[i].type     = wide ? modbus::RegType::kFloat64BE
                                     : modbus::RegType::kFloat32BE;
            m.pts[i].block    = b;
            m.pts[i].writable = (b == 1);
            off = static_cast<uint16_t>(off + m.width);
        }
    }
    return m;
}

// 编译期守卫：映射表点数 == 点表点数。任何一边加了点，这里先报错。
static_assert(ModbusRegisterMap::kPointCount == EMS_POINT_COUNT,
              "Modbus 映射表点数必须与 ems_point_table.h 的 EMS_POINT_COUNT 一致");

// =====================================================================
// 传输层抽象 —— 只负责"把字节发出去、把字节收回来"
//
// 分层理由：编解码（modbus_codec.h）与链路（本接口）分开后，
//   · 帧正确性可以用逐字节断言验证（不需要 socket）；
//   · 环回实现让"闭环等价性"可以在单进程里跑到 400 拍；
//   · 现场换 TCP / RTU 只换本接口的实现，上面两层一行不改。
// =====================================================================
class IModbusTransport {
public:
    virtual ~IModbusTransport() = default;

    virtual bool connect() = 0;
    virtual void close()    = 0;
    virtual bool connected() const = 0;

    // 发送请求帧，同步取回响应帧。
    // 返回 false = 超时 / 链路错误 / 从站沉默（此时响应内容不可信）。
    virtual bool transact(const uint8_t* req, size_t req_len,
                          uint8_t* resp, size_t resp_cap, size_t* resp_len) = 0;
};

// =====================================================================
// 主站侧适配器：Modbus 从站 → IDeviceIO
// =====================================================================
class ModbusDeviceIO : public IDeviceIO {
public:
    // 设备侧推进泵。真实系统里"设备推进一拍"由硬件完成，本进程内没有设备；
    // 设置后 execute() 会调用它，从而让闭环能在单进程里跑通。**不设置 = 纯下发**。
    using DevicePump = std::function<void(double p_cmd_kw, double dt_s)>;

    static constexpr size_t kFrameCap = 320;    // 253(PDU) + 7(MBAP) 有余量
    static constexpr size_t kBlockRegCap = 128; // 最大块 = CFG 14 点 × 4 寄存器

    ModbusDeviceIO() : map_(build_modbus_map(MapProfile::kStandardF32)) { reset_cache(); }

    ModbusDeviceIO(IModbusTransport* t, MapProfile profile = MapProfile::kStandardF32,
                   uint8_t unit_id = 1)
        : transport_(t), map_(build_modbus_map(profile)), unit_(unit_id) {
        reset_cache();
    }

    ~ModbusDeviceIO() override = default;

    // -----------------------------------------------------------------
    // 装配（由进程入口 / 测试装置调用）
    // -----------------------------------------------------------------
    void attach_transport(IModbusTransport* t) { transport_ = t; }
    void set_profile(MapProfile p) { map_ = build_modbus_map(p); reset_cache(); }
    void set_map(const ModbusRegisterMap& m) { map_ = m; reset_cache(); }
    void set_unit_id(uint8_t u) { unit_ = u; }
    void set_device_pump(DevicePump p) { pump_ = std::move(p); }

    const ModbusRegisterMap& map() const { return map_; }
    uint8_t unit_id() const { return unit_; }

    bool open() {
        if (transport_ == nullptr) return false;
        const bool ok = transport_->connect();
        if (ok) reset_cache();
        return ok;
    }
    void close() { if (transport_ != nullptr) transport_->close(); }
    bool is_open() const { return transport_ != nullptr && transport_->connected(); }

    // -----------------------------------------------------------------
    // 诊断（现场排障；不属 IDeviceIO 契约）
    // -----------------------------------------------------------------
    int stale_reads() const { return stale_reads_; }    // 读失败（超时/异常/格式错）
    int timeouts() const { return timeouts_; }           // 从站沉默次数
    int exceptions() const { return exceptions_; }       // 从站异常响应次数
    int writes() const { return writes_; }               // 成功写寄存器次数
    int transactions() const { return transactions_; }   // 收发报文总数

    double cached_value(int index) const {
        return (index >= 0 && index < ModbusRegisterMap::kPointCount) ? cache_[index] : 0.0;
    }

    // 自检：映射表 ↔ 点名真相源 ↔ 地址区间三者一致。返回**不一致的点数**，0 = 全对。
    // 检查项刻意做成"能抓住手工改表"的：点名漂移 / 块归属漂移 / 地址越界 / 地址重叠。
    int self_check() const {
        int bad = 0;
        for (int i = 0; i < ModbusRegisterMap::kPointCount; ++i) {
            const RegPoint& p = map_.pts[i];
            if (p.point == nullptr ||
                std::strcmp(p.point, EMS_POINT_NAMES[i]) != 0) { ++bad; continue; }
            const int expect_block = modbus_point_block(i);
            if (p.block != expect_block) { ++bad; continue; }
            if (p.addr < ModbusRegisterMap::kBlockBase[p.block] ||
                static_cast<uint32_t>(p.addr) + map_.width >
                    ModbusRegisterMap::kBlockBase[p.block] + map_.block_qty(p.block)) {
                ++bad;
                continue;
            }
            // 地址不得与其它点重叠
            for (int j = 0; j < ModbusRegisterMap::kPointCount; ++j) {
                if (j == i) continue;
                const RegPoint& q = map_.pts[j];
                if (p.addr < static_cast<uint16_t>(q.addr + map_.width) &&
                    q.addr < static_cast<uint16_t>(p.addr + map_.width)) { ++bad; break; }
            }
        }
        return bad;
    }

    // -----------------------------------------------------------------
    // IDeviceIO 实现
    // -----------------------------------------------------------------

    // ① 读量测快照（冻结语义）。采集失败不抛异常、不返回半成品：
    //    保留最近一次有效值，可信性由 read_status() 表达。
    bool read_snapshot(Timestamp now, RealtimeSnapshot& out) override {
        read_block(0);      // MEAS 块
        read_block(2);      // CFG 块（站用电 CFG.PCS_STANDBY 参与负荷折算）

        const double p_bat = cache_[EMS_P_BAT];
        const double p_pv  = cache_[EMS_P_PV];
        // 站用电计入负荷侧 —— 与 PlantModel::sample() / MemoryDeviceIO / RtDbDeviceIO 同一口径
        const double p_load = std::max(0.0, cache_[EMS_P_LOAD] + cache_[EMS_CFG_STANDBY]);

        out.timestamp       = now;
        out.p_bat_actual_kw = p_bat;
        out.p_pv_kw         = p_pv;
        out.p_load_kw       = p_load;
        out.p_grid_kw       = p_load - p_pv - p_bat;
        out.soc             = clamp01(cache_[EMS_SOC]);
        out.temperature_c   = cache_[EMS_T_C];
        out.soh             = cache_[EMS_SOH];
        // 前瞻：Modbus 点表暂无预报点（预报由上层刷新）。保持 false ——
        // 与 MemoryDeviceIO / RtDbDeviceIO 一致，安全层行为与历史完全相同。
        out.has_lookahead = false;

        const bool offline = cache_[EMS_STA_OFFLINE] > 0.5;
        out.meters_alive["BMS"]   = cache_[EMS_STA_BMS]   > 0.5 && !offline;
        out.meters_alive["METER"] = cache_[EMS_STA_METER] > 0.5 && !offline;
        out.meters_alive["PCS"]   = cache_[EMS_STA_PCS]   > 0.5 && !offline;

        // 电价窗口：给独立使用时的保守默认值（EmsRuntime 会用预报曲线覆盖）。
        // 现场版应改为读电价点或由上层推送。
        const double h = std::fmod(now, 86400.0) / 3600.0;
        const bool valley = (h < 8.0 || h >= 22.0);
        out.pricing.cur_tou_type  = valley ? TouType::kValley : TouType::kPeak;
        out.pricing.cur_tou_price = valley ? 0.30 : 0.90;

        return data_valid();
    }

    // ② 读设备运行时限制。**装配阶段**（attach_device / apply_configs）调用，
    //    因此设备侧必须先把 CFG.* 写进寄存器；现场 BMS 动态降功率就是靠
    //    定期刷新这几个点生效的。
    bool read_limits(DeviceLimits& out) override {
        read_block(2);
        out = DeviceLimits{};
        out.pcs_rated_chg_kw        = cache_[EMS_CFG_MAX_CHG];
        out.pcs_rated_dis_kw        = cache_[EMS_CFG_MAX_DIS];
        out.bms_chg_limit_kw        = cache_[EMS_CFG_BMS_CHG_LIM];
        out.bms_dis_limit_kw        = cache_[EMS_CFG_BMS_DIS_LIM];
        out.transformer_capacity_kw = cache_[EMS_CFG_TR_KVA];
        out.d_target_kw             = cache_[EMS_CFG_D_TARGET];
        // 禁充/禁放位：当前点表未建模（与 MemoryDeviceIO / RtDbDeviceIO 对齐）。
        // 现场若需按 BMS 禁充放硬封锁，在点表里加点后在此处映射即可。
        out.bms_chg_forbidden = false;
        out.bms_dis_forbidden = false;
        return true;
    }

    // ③ 读通信/故障状态（算法侧唯一的故障语义来源）
    DeviceStatus read_status() const override {
        read_block(3);      // STA 块
        DeviceStatus s;
        s.bms_comm_ok    = cache_[EMS_STA_BMS]     > 0.5;
        s.pcs_comm_ok    = cache_[EMS_STA_PCS]     > 0.5;
        s.meter_comm_ok  = cache_[EMS_STA_METER]   > 0.5;
        s.pcs_fault      = cache_[EMS_STA_FAULT]   > 0.5;
        s.device_offline = cache_[EMS_STA_OFFLINE] > 0.5;
        s.data_valid     = data_valid();
        return s;
    }

    // ④ 读实际运行值（记录/展示；非闭环回路 —— 闭环用 ①）
    DeviceActuals read_actuals() const override {
        read_block(0);
        DeviceActuals a;
        a.p_bat_kw      = cache_[EMS_P_BAT];
        a.p_grid_kw     = cache_[EMS_P_GRID];
        a.p_load_kw     = cache_[EMS_P_LOAD];
        a.p_pv_kw       = cache_[EMS_P_PV];
        a.soc           = cache_[EMS_SOC];
        a.temperature_c = cache_[EMS_T_C];
        return a;
    }

    // ⑤ 写功率指令（含权限区间）。CMD 区一次 0x10 写完整块。
    bool write_command(const PowerCommand& cmd) override {
        last_cmd_     = cmd;
        has_last_cmd_ = true;
        double v[ModbusRegisterMap::kPointCount] = {};
        v[EMS_CMD_P_BAT]   = cmd.p_bat_cmd_kw;
        v[EMS_CMD_P_UPPER] = cmd.p_upper;
        v[EMS_CMD_P_LOWER] = cmd.p_lower;
        return write_block(1, v);
    }

    // ⑥ 推进一拍。**真实适配器不做物理积分** —— 它只把指令写进从站寄存器，
    //    实际功率由设备在下一拍写回 MEAS。
    double execute(double p_cmd_kw, double dt_s) override {
        if (has_last_cmd_) {
            // 权限区间只有装配层调用过 write_command() 才有意义，一并发出去
            double v[ModbusRegisterMap::kPointCount] = {};
            v[EMS_CMD_P_BAT]   = p_cmd_kw;
            v[EMS_CMD_P_UPPER] = last_cmd_.p_upper;
            v[EMS_CMD_P_LOWER] = last_cmd_.p_lower;
            write_block(1, v);
        } else {
            // 没有权限区间时**只写 CMD.P_BAT 一个点**（起止地址精确覆盖该点）。
            // 为什么不整块写：整块写会把 P_UPPER/P_LOWER 一并写成 0/0，而从站
            // 若按"权限区间"执行，0/0 会被理解成"禁止动作" —— 那是危险的下发。
            // 这里也不能用 0x06（单 16 bit 寄存器）：浮点点位宽度 2/4，写一半
            // 会写出半个浮点数。0x10 允许任意起止地址，所以按点宽写即可。
            // RT_DB 适配器里守的是同一条纪律。
            write_point(EMS_CMD_P_BAT, p_cmd_kw);
        }

        // 设备侧推进（真实系统里是硬件/另一个进程；测试用泵搬进本进程）
        if (pump_) pump_(p_cmd_kw, dt_s);

        // 返回值 = 本拍量测（尽力反馈，仅供记录）。**闭环不依赖它**。
        read_block(0);
        return cache_[EMS_P_BAT];
    }

    double battery_capacity_kwh() const override {
        if (!cfg_loaded_) read_block(2);   // 首次调用拉一次 CFG 块
        return cache_[EMS_CFG_CAP_KWH];
    }

    const char* name() const override { return "ModbusDeviceIO(TCP)"; }

private:
    static double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

    void reset_cache() {
        for (int i = 0; i < ModbusRegisterMap::kPointCount; ++i) {
            cache_[i]      = EMS_POINT_DEFAULTS[i];
            quality_ok_[i] = true;
        }
        cfg_loaded_ = false;
    }

    // ---- 读一个块（一次 0x03，点少块小，无需分批）----
    bool read_block(int block) const {
        const uint16_t addr = ModbusRegisterMap::kBlockBase[block];
        const uint16_t qty  = map_.block_qty(block);
        if (!read_regs(addr, qty, block_regs_)) {
            // 整块不可信：**保留旧值**（接口契约①），只把品质位拉低。
            // 算法不会读到"半成品"，它读到的是一份陈旧但完整的快照，
            // 是否可用由 read_status().data_valid 决定。
            for (int i = 0; i < ModbusRegisterMap::kPointCount; ++i) {
                if (map_.pts[i].block == block) quality_ok_[i] = false;
            }
            return false;
        }
        for (int i = 0; i < ModbusRegisterMap::kPointCount; ++i) {
            if (map_.pts[i].block != block) continue;
            const uint16_t off = static_cast<uint16_t>(map_.pts[i].addr - addr);
            cache_[i]      = modbus::decode_value(block_regs_ + off, map_.pts[i].type);
            quality_ok_[i] = true;
        }
        if (block == 2) cfg_loaded_ = true;
        return true;
    }

    // 读寄存器（自动按 125 上限分批；本项目的块都远小于上限，此处只为健壮）
    bool read_regs(uint16_t addr, uint16_t qty, uint16_t* out) const {
        if (transport_ == nullptr || !transport_->connected()) { ++stale_reads_; return false; }
        uint16_t got = 0;
        while (got < qty) {
            const uint16_t chunk = std::min<uint16_t>(
                static_cast<uint16_t>(modbus::kMaxReadRegs),
                static_cast<uint16_t>(qty - got));
            const size_t n = modbus::build_read_request(
                req_, sizeof(req_), ++tid_, unit_, modbus::kReadHoldingRegs,
                static_cast<uint16_t>(addr + got), chunk);
            if (n == 0) { ++stale_reads_; return false; }

            size_t rlen = 0;
            ++transactions_;
            if (!transport_->transact(req_, n, resp_, sizeof(resp_), &rlen)) {
                ++timeouts_;
                ++stale_reads_;
                return false;
            }
            modbus::MbapHeader h;
            modbus::PduView    pdu;
            if (!modbus::parse_response(resp_, rlen, h, pdu) || h.unit != unit_) {
                ++stale_reads_;
                return false;
            }
            modbus::ExceptionCode ec = modbus::kExcNone;
            if (modbus::is_exception(pdu, ec)) { ++exceptions_; ++stale_reads_; return false; }
            if (pdu.fc != static_cast<uint8_t>(modbus::kReadHoldingRegs)) {
                ++stale_reads_;
                return false;
            }
            if (modbus::parse_read_response_regs(pdu, chunk, out + got) != chunk) {
                ++stale_reads_;
                return false;
            }
            got = static_cast<uint16_t>(got + chunk);
        }
        return true;
    }

    // 写一个块（0x10）。vals 以**点索引**为下标，写该块全部点。
    bool write_block(int block, const double* vals) {
        if (transport_ == nullptr || !transport_->connected()) return false;
        uint16_t n = 0;
        for (int i = 0; i < ModbusRegisterMap::kPointCount; ++i) {
            if (map_.pts[i].block != block) continue;
            modbus::encode_value(vals[i], map_.pts[i].type, block_regs_ + n);
            n = static_cast<uint16_t>(n + map_.width);
        }
        const size_t len = modbus::build_write_multiple_request(
            req_, sizeof(req_), ++tid_, unit_, ModbusRegisterMap::kBlockBase[block],
            block_regs_, n);
        if (len == 0) return false;

        size_t rlen = 0;
        ++transactions_;
        if (!transport_->transact(req_, len, resp_, sizeof(resp_), &rlen)) {
            ++timeouts_;
            return false;
        }
        modbus::MbapHeader h;
        modbus::PduView    pdu;
        if (!modbus::parse_response(resp_, rlen, h, pdu) || h.unit != unit_) return false;
        modbus::ExceptionCode ec = modbus::kExcNone;
        if (modbus::is_exception(pdu, ec)) { ++exceptions_; return false; }
        // 0x10 正常响应：fc + addr(2) + qty(2)
        if (pdu.fc != static_cast<uint8_t>(modbus::kWriteMultipleRegs) || pdu.len < 4) {
            return false;
        }
        const uint16_t echo_addr = modbus::get_u16_be(pdu.data);
        const uint16_t echo_qty  = modbus::get_u16_be(pdu.data + 2);
        if (echo_addr != ModbusRegisterMap::kBlockBase[block] || echo_qty != n) return false;
        ++writes_;
        return true;
    }

    // 写单点（0x10，起止地址恰好覆盖该点的寄存器宽度）。
    // 用于"没有权限区间时只写 CMD.P_BAT"这一条纪律 —— 既不多写别的点，
    // 也不会像 0x06 那样把多寄存器浮点写成半个数。
    bool write_point(int index, double value) {
        if (transport_ == nullptr || !transport_->connected()) return false;
        if (index < 0 || index >= ModbusRegisterMap::kPointCount) return false;
        const RegPoint& p = map_.pts[index];
        uint16_t regs[4] = {0, 0, 0, 0};
        modbus::encode_value(value, p.type, regs);
        const uint16_t qty = static_cast<uint16_t>(map_.width);

        const size_t len = modbus::build_write_multiple_request(
            req_, sizeof(req_), ++tid_, unit_, p.addr, regs, qty);
        if (len == 0) return false;
        size_t rlen = 0;
        ++transactions_;
        if (!transport_->transact(req_, len, resp_, sizeof(resp_), &rlen)) {
            ++timeouts_;
            return false;
        }
        modbus::MbapHeader h;
        modbus::PduView    pdu;
        if (!modbus::parse_response(resp_, rlen, h, pdu) || h.unit != unit_) return false;
        modbus::ExceptionCode ec = modbus::kExcNone;
        if (modbus::is_exception(pdu, ec)) { ++exceptions_; return false; }
        if (pdu.fc != static_cast<uint8_t>(modbus::kWriteMultipleRegs) || pdu.len < 4) {
            return false;
        }
        if (modbus::get_u16_be(pdu.data) != p.addr ||
            modbus::get_u16_be(pdu.data + 2) != qty) return false;
        ++writes_;
        // 本地下行镜像：写成功即视该点品质良好
        cache_[index]      = value;
        quality_ok_[index] = true;
        return true;
    }

    // 数据可信性 = 数据有效位 **且** 量测点全程未出现读失败。
    // 只信"值"不信"品质"是现场最常见的坑（陈旧值被当有效值用）。
    bool data_valid() const {
        bool ok = cache_[EMS_STA_VALID] > 0.5;
        for (int i = 0; i <= static_cast<int>(EMS_SOH); ++i) ok = ok && quality_ok_[i];
        return ok;
    }

    IModbusTransport* transport_ = nullptr;
    ModbusRegisterMap map_;
    uint8_t           unit_ = 1;

    // read_* 是 const 接口，而采集必须更新缓存 —— 与 RtDbDeviceIO 同一写法。
    mutable double  cache_[ModbusRegisterMap::kPointCount]      = {};
    mutable bool    quality_ok_[ModbusRegisterMap::kPointCount] = {};
    mutable int     stale_reads_  = 0;
    mutable int     timeouts_     = 0;
    mutable int     exceptions_   = 0;
    mutable int     writes_       = 0;
    mutable int     transactions_ = 0;
    mutable uint16_t tid_          = 0;      // 事务号（自增，回绕无妨）
    mutable uint8_t  req_[kFrameCap]      = {};
    mutable uint8_t  resp_[kFrameCap]     = {};
    mutable uint16_t block_regs_[kBlockRegCap] = {};
    mutable bool     cfg_loaded_  = false;

    PowerCommand last_cmd_{};
    bool         has_last_cmd_ = false;
    DevicePump   pump_{};
};

} // namespace ems
