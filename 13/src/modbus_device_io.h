// =====================================================================
// 13/ — Modbus TCP 适配器 ModbusDeviceIO（IDeviceIO 的**现场实现**）
//
// 定位：IDeviceIO 的第四个实现。前三个是
//   SimDeviceIO     —— 进程内物理模型（周期 7）
//   MemoryDeviceIO  —— 进程内点表（P0.5）
//   RtDbDeviceIO    —— 跨进程共享内存实时库（07/src/rtdb）
// 本文件是**真的过网线**的那个：EMS 作为 Modbus TCP **主站（客户端）**，
// 去问 PCS / BMS / 电表要数据。设备侧由 13/sim/modbus_slave.py 扮演
// （或现场的真实设备）。
//
// =====================================================================
// ★ 差别一：Modbus 没有品质位
//
// RT_DB 的每个点自带 data_quality_t（BAD/GOOD/UNCERTAIN），"值可信吗"
// 是**读回来的**。Modbus 协议里根本没有这个东西 —— 寄存器就是 16 位裸数据，
// 没有单位、没有时标、没有品质。
//
// 后果（接真机时最先撞上的墙）：
//   · "值不可信"必须由**主站自己合成**。
//   · 一旦主站偷懒把"读失败"当"读到 0"，算法就会拿 0 当量测用 ——
//     0 kW 的负荷、0% 的 SOC 都是**看起来完全正常**的数值，静默失败。
//   所以本文件的纪律是：**失败时保留上一次有效值，可信性另行表达**，
//   与 04/src/device_io.h 契约①（"采集失败时填最近一次有效值"）一致。
//
// ★ 差别二：一次快照不是一个请求，是**一组**请求 → 部分成功是常态
//
// RT_DB 的一次 read_snapshot 是"读 32 个点"，原子性由 seqlock 保。
// Modbus 一个事务只能读一段**连续地址**，32 个点要拆成 3 次请求
// （见 modbus_point_map.h 的分块表）。于是"输入寄存器读到了、离散输入超时了"
// 是运行常态。本文件必须能表达"这一拍的量测半新半旧"：
//   → per-**block** 的成功位（block_ok_）+ per-point 的解码位（point_ok_）。
//   两个位分开还有一个现场价值：排障时
//     · `block_fails()` 高 → 通信/地址问题（线、网关、从站地址）
//     · `decode_fails()` 高 → **编码/字序**问题（映射表配错）
//   混成一个计数就再也分不开了。
//
// ★ 差别三：指令是**写**，写下去不等于生效
//
// Modbus 写保持寄存器会回一个响应 —— 但响应只证明"设备收到了"，
// **不证明"设备照做了"**。生效与否只能等下一拍读 MEAS.P_BAT。
// 所以 execute() 依然不做物理积分，返回值依然不可信（契约 ③），
// 闭环依然走 read_snapshot()。
//
// 编译：纯头文件。MinGW 链接 -lws2_32。
// =====================================================================

#pragma once

#include "device_io.h"            // 04/  IDeviceIO / DeviceStatus / DeviceActuals
#include "ems_point_table.h"      // 07/  点表真相源（40 点；本模块只绑设备侧 32 点）
#include "modbus_tcp_client.h"    // 13/  协议层
#include "modbus_point_map.h"     // 13/  映射表（真相源）

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace ems {

class ModbusDeviceIO : public IDeviceIO {
public:
    struct Config {
        std::string   host       = "127.0.0.1";
        std::uint16_t port       = 502;
        std::uint8_t  unit_id    = 1;      // 从站地址（现场一台设备一个）
        int           timeout_ms = 1000;   // 单次请求超时
        // 掉线后是否自动重连。默认开 —— 现场没有人工介入的机会。
        bool          auto_reconnect = true;
        // 重连**最小间隔**。为什么需要：connect() 本身要等到超时（默认 1 s），
        // 若每拍都试，一个离线设备会把 200 ms 的控制周期拖成 1.2 s，
        // 连带把 Pacing 的时间轴拉歪。所以只允许按节奏试。
        int           reconnect_interval_ms = 1000;
        // read_limits 只扫它需要的两块（输入寄存器含 CFG、离散输入含禁充放位），
        // 不读保持寄存器区（那是指令的读回区）。默认开 —— 省一次请求，
        // 而且"读参数"与"读量测"在现场常常就是两组不同的寄存器。
        bool          limits_scope_only = true;
    };

    ModbusDeviceIO() = default;
    explicit ModbusDeviceIO(Config cfg) : cfg_(std::move(cfg)) {}

    // =================================================================
    // 连接管理
    // =================================================================
    bool connect() {
        last_connect_attempt_ = clock::now();
        reconnect_attempted_  = true;
        if (!client_.connect(cfg_.host.c_str(), cfg_.port, cfg_.timeout_ms, cfg_.unit_id)) {
            return false;
        }
        ++reconnects_;
        // 连接成功**不**清空缓存：上一次会话的值比"没有值"安全，
        // 而且立刻清空会让"重连后第一拍"读到一个假零点（0 kW 负荷）。
        return true;
    }
    void disconnect() { client_.close(); }
    bool is_connected() const { return client_.is_connected(); }

    // =================================================================
    // IDeviceIO 实现
    // =================================================================
    // ① 读量测快照（冻结语义）
    bool read_snapshot(Timestamp now, RealtimeSnapshot& out) override {
        scan(/*with_holding=*/true, now);

        const double p_bat  = value(EMS_P_BAT);
        const double p_pv   = value(EMS_P_PV);
        // 站用电计入负荷侧 —— 与 PlantModel::sample() / MemoryDeviceIO /
        // RtDbDeviceIO 同一口径（四个适配器对同一个量的语义必须一致，
        // 否则"换数据源不改算法"在数值上就不成立）
        const double p_load = std::max(0.0, value(EMS_P_LOAD) + value(EMS_CFG_STANDBY));

        out.timestamp       = now;
        out.p_bat_actual_kw = p_bat;
        out.p_pv_kw         = p_pv;
        out.p_load_kw       = p_load;
        // 关口功率：读**电表寄存器**，不由三路相减推算（缺口 A2 的口径统一）。
        // Modbus 场景下这一点尤其硬：三个功率量可能来自**三台不同设备**
        // （PCS 报电池、逆变器报光伏、电表报关口），采样时刻差几百毫秒是常态，
        // 相减出来的"关口功率"完全没有物理意义。
        out.p_grid_kw       = value(EMS_P_GRID);
        out.soc             = clamp(value(EMS_SOC), 0.0, 1.0);
        out.temperature_c   = value(EMS_T_C);
        out.soh             = clamp(value(EMS_SOH), 0.0, 1.0);
        // 本点表暂无预报点（现场预报由上层刷新）。保持 false —— 与其它三个
        // 适配器一致，安全层行为与历史相同。
        out.has_lookahead = false;

        const bool offline = value(EMS_STA_OFFLINE) > 0.5;
        out.meters_alive["BMS"]   = point_valid(EMS_STA_BMS)   && value(EMS_STA_BMS)   > 0.5 && !offline;
        out.meters_alive["PCS"]   = point_valid(EMS_STA_PCS)   && value(EMS_STA_PCS)   > 0.5 && !offline;
        out.meters_alive["METER"] = point_valid(EMS_STA_METER) && value(EMS_STA_METER) > 0.5 && !offline;

        // 电价窗口：独立使用时的保守默认（EmsRuntime 会用预报曲线覆盖）
        const double h = std::fmod(now, 86400.0) / 3600.0;
        const bool valley = (h < 8.0 || h >= 22.0);
        out.pricing.cur_tou_type  = valley ? TouType::kValley : TouType::kPeak;
        out.pricing.cur_tou_price = valley ? 0.30 : 0.90;

        return data_valid();
    }

    // ② 读设备运行时限值
    //
    // 现场语义比仿真重要得多：BMS 动态降功率、禁充放位、PCS 额定都会在
    // 运行中变，而它们是从**设备寄存器**读来的。读失败时保留上次有效值 ——
    // 对禁充放位而言"上次读到 1（禁止）保留 1"偏保守，"上次读到 0（允许）
    // 保留 0"偏开放；后者由 05/ 的 bms_comm_lost → [0,0] **独立**兜底
    // （判据是 meters_alive["BMS"]，即 STA.BMS_COMM_OK，而它经 point_valid()
    // 已经含了"本拍真的采到了"这一条）。
    bool read_limits(DeviceLimits& out) override {
        scan(/*with_holding=*/!cfg_.limits_scope_only, 0.0);

        out = DeviceLimits{};
        out.pcs_rated_chg_kw        = value(EMS_CFG_MAX_CHG);
        out.pcs_rated_dis_kw        = value(EMS_CFG_MAX_DIS);
        out.bms_chg_limit_kw        = value(EMS_CFG_BMS_CHG_LIM);
        out.bms_dis_limit_kw        = value(EMS_CFG_BMS_DIS_LIM);
        out.transformer_capacity_kw = value(EMS_CFG_TR_KVA);
        out.d_target_kw             = value(EMS_CFG_D_TARGET);
        // ★ 安全输入：这两个 bool 是 04/ S01(kBmsForbid, **L0 最底层**)与
        //   05/ check_bms_forbid() 的唯一输入。与 RT_DB 路线（A1 修复）
        //   逐点一致 —— 四条数据源路径对同一个安全量必须给出同一个语义，
        //   否则"换个数据源"就换了一套安全边界。
        out.bms_chg_forbidden = value(EMS_STA_BMS_CHG_FORBID) > 0.5;
        out.bms_dis_forbidden = value(EMS_STA_BMS_DIS_FORBID) > 0.5;
        return true;
    }

    // ③ 读通信/故障状态
    //
    // ★ 这里的每个 bit 都是**合成**的，不是读来的（Modbus 没有品质位）：
    //   bms/pcs/meter_comm_ok ← 设备自报的通信位 **且** 本拍真的采到了那一位
    //   data_valid            ← 见 data_valid()
    DeviceStatus read_status() const override {
        DeviceStatus s;
        s.bms_comm_ok    = point_valid(EMS_STA_BMS)   && value(EMS_STA_BMS)   > 0.5;
        s.pcs_comm_ok    = point_valid(EMS_STA_PCS)   && value(EMS_STA_PCS)   > 0.5;
        s.meter_comm_ok  = point_valid(EMS_STA_METER) && value(EMS_STA_METER) > 0.5;
        s.pcs_fault      = point_valid(EMS_STA_FAULT) && value(EMS_STA_FAULT) > 0.5;
        s.device_offline = value(EMS_STA_OFFLINE) > 0.5;
        s.data_valid     = data_valid();
        s.last_update    = last_scan_ts_;
        return s;
    }

    // ④ 读实际运行值（记录/展示；非闭环回路 —— 闭环用 ①）
    DeviceActuals read_actuals() const override {
        DeviceActuals a;
        a.p_bat_kw      = value(EMS_P_BAT);
        a.p_grid_kw     = value(EMS_P_GRID);
        a.p_load_kw     = value(EMS_P_LOAD);
        a.p_pv_kw       = value(EMS_P_PV);
        a.soc           = value(EMS_SOC);
        a.temperature_c = value(EMS_T_C);
        return a;
    }

    // ⑤ 写功率指令（下发 PCS）
    //
    // 一次 FC16 把 (p_bat, p_upper, p_lower) 写下去 —— 三个值必须在**同一个
    // 事务**里，否则设备可能执行到"新功率 + 旧权限区间"的中间态，而那一瞬间
    // 新指令可能已经越过旧区间。映射表的 `cmd_points_contiguous()` 静态断言
    // 保证了地址连续，这里才能这么写。
    bool write_command(const PowerCommand& cmd) override {
        last_cmd_     = cmd;
        has_last_cmd_ = true;

        const std::uint16_t base = modbus::kBindings[EMS_CMD_P_BAT].address;
        const std::uint16_t span = 6;   // 3 个 f32

        std::uint16_t regs[6] = {0};
        const modbus::DecodeError e1 = modbus::encode_point(
            modbus::kBindings[EMS_CMD_P_BAT],   cmd.p_bat_cmd_kw, regs, 6, nullptr, 0);
        const modbus::DecodeError e2 = modbus::encode_point(
            modbus::kBindings[EMS_CMD_P_UPPER], cmd.p_upper,     regs, 6, nullptr, 0);
        const modbus::DecodeError e3 = modbus::encode_point(
            modbus::kBindings[EMS_CMD_P_LOWER], cmd.p_lower,     regs, 6, nullptr, 0);
        if (e1 != modbus::DecodeError::kNone ||
            e2 != modbus::DecodeError::kNone ||
            e3 != modbus::DecodeError::kNone) {
            // 编码失败（NaN / 非有限）= 上层给了非法指令。
            // **不写**一个凑合的值出去：把坏指令写进 PCS 比不写危险。
            ++encode_failures_;
            last_decode_error_ = (e1 != modbus::DecodeError::kNone) ? e1 :
                                 ((e2 != modbus::DecodeError::kNone) ? e2 : e3);
            return false;
        }

        if (!ensure_connected()) { ++write_failures_; return false; }
        const modbus::ModbusStatus st = client_.write_multiple_regs(base, regs, span);
        if (!st.ok()) {
            ++write_failures_;
            last_transport_error_ = st.error;
            if (st.is_transport()) client_.close();
            return false;
        }
        ++writes_;
        return true;
    }

    // ⑥ 推进一拍。**不做物理积分**（接口契约 ③）：
    //    真实系统的"推进"由设备自己完成，本函数只把指令发出去。
    //    返回值是**尽力反馈**（最近一次采到的实际功率），闭环不得依赖它。
    double execute(double p_cmd_kw, double dt_s) override {
        (void)dt_s;
        // 只更新功率指令点，**不动权限区间** —— 区间由 write_command() 负责。
        // 这里越俎代庖会把"装配层从未下发过区间"变成"下发过一个凭空造的区间"，
        // 而后者在设备侧看起来完全合法。
        if (has_last_cmd_) {
            std::uint16_t regs[6] = {0};
            if (modbus::encode_point(modbus::kBindings[EMS_CMD_P_BAT], p_cmd_kw, regs, 6,
                                     nullptr, 0) == modbus::DecodeError::kNone &&
                ensure_connected() &&
                client_.write_multiple_regs(modbus::kBindings[EMS_CMD_P_BAT].address, regs, 2).ok()) {
                ++writes_;
            } else {
                ++write_failures_;
            }
        }
        return value(EMS_P_BAT);
    }

    // ⑦ 电池额定容量：来自设备寄存器 CFG.BAT_CAP_KWH。
    //    读不到时返回 0 —— **0 比一个默认容量安全**：0 会让优化层立刻发现
    //    "没有容量信息"，而 1000 会让它安静地算错。
    double battery_capacity_kwh() const override { return value(EMS_CFG_CAP_KWH); }

    const char* name() const override { return "ModbusDeviceIO(Modbus TCP)"; }

    // 限值是"活的"：CFG.* 与禁充放位都在设备寄存器里，随时可能变
    // → EmsRuntime 每拍刷新 dev_（见 LoopConfig::refresh_limits_each_step）。
    // 这与 RtDbDeviceIO 一致、与两个仿真适配器相反 —— 判据是"限值会不会
    // 在脚下变"，不是"实现类是谁"。
    bool limits_are_live() const override { return true; }

    // =================================================================
    // 诊断（现场排障 + 测试判据）
    // =================================================================
    int  scans()          const { return scans_; }
    int  partial_scans()  const { return partial_scans_; }    // 有分块失败的扫描数
    int  block_fails()    const { return block_fails_; }      // 分块请求失败次数
    int  decode_fails()   const { return decode_fails_; }     // **解码/字序**失败次数
    int  transport_fails()const { return transport_fails_; }
    int  unconnected_scans() const { return unconnected_scans_; }
    // 重连尝试次数。现场排障的**第一个**要看的量：
    // 它持续增长说明链路在抖动，"读到的值时好时坏"就不是算法问题。
    int  reconnects()     const { return reconnects_; }
    int  writes()         const { return writes_; }
    int  write_failures() const { return write_failures_; }
    int  encode_failures()const { return encode_failures_; }
    modbus::DecodeError   last_decode_error()   const { return last_decode_error_; }
    modbus::ModbusError   last_transport_error()const { return last_transport_error_; }
    Timestamp last_scan_timestamp() const { return last_scan_ts_; }

    const modbus::ModbusTcpClient& client() const { return client_; }
    const Config& config() const { return cfg_; }

    // 当前有多少个点的值**不可信**（所在分块本拍没成功，或该点解码失败）。
    //
    // 注意语义：这不是"出错了多少次"，而是"现在有多少个值不能用"。
    // 完全未连接时它等于**全部被绑定的点数**（当前 32）—— 那是**一个**事实的
    // 完整表达，比"逐个点记 32 条 stale 日志"更清楚。
    // ★ 用 modbus::kBindingCount 而不是 EMS_POINT_COUNT：本类只管设备总线上被绑定的点，
    //   EXT 外部设定根本不从这条路过（它们由网关的 RtDbExtWriter 写 RT_DB）。
    int stale_points() const {
        int n = 0;
        for (std::size_t i = 0; i < modbus::kBindingCount; ++i) {
            if (!point_valid(i)) ++n;
        }
        return n;
    }

    double cached(std::size_t index) const {
        return (index < modbus::kBindingCount) ? cache_[index] : 0.0;
    }

    // ★ 本点的值当前是否可信 = **所在分块本拍成功** 且 **该点解码成功**。
    //   两个条件缺一不可：块成功但解码失败（字序错）时必须报 false，
    //   否则一个 NaN 或天文数字会被当作"采到了"。
    bool point_valid(std::size_t index) const {
        if (index >= modbus::kBindingCount) return false;
        const int b = block_of(index);
        if (b < 0 || !block_ok_[b]) return false;
        return point_ok_[index];
    }

    // 自检：映射表 ↔ 点表是否逐字一致（现场配置错误在这里报出来）
    int self_check() const { return modbus::self_check(); }
    // 读全 32 点需要几次请求（= 分块数，应为 3）
    std::size_t full_scan_request_count() const { return modbus::full_scan_request_count(); }

private:
    using clock = std::chrono::steady_clock;

    // 点 → 所属分块下标（-1 = 没有被任何分块覆盖；static_assert 已排除）
    static int block_of(std::size_t index) {
        const modbus::PointBinding& b = modbus::kBindings[index];
        const std::uint16_t width =
            (b.encoding == modbus::Encoding::kBit) ? 1 : b.reg_count();
        for (std::size_t k = 0; k < modbus::kReadBlockCount; ++k) {
            if (modbus::kReadBlocks[k].table != b.table) continue;
            if (b.address >= modbus::kReadBlocks[k].start &&
                static_cast<std::uint32_t>(b.address) + width <=
                    static_cast<std::uint32_t>(modbus::kReadBlocks[k].start) +
                        modbus::kReadBlocks[k].count) {
                return static_cast<int>(k);
            }
        }
        return -1;
    }

    bool ensure_connected() {
        if (client_.is_connected()) return true;
        return try_reconnect();
    }

    // -----------------------------------------------------------------
    // 扫描：分块请求 + 逐点解码
    //
    // `ts > 0` 时推进 last_scan_ts_；read_limits 传 0 表示"读参数不是量测"，
    // 不该把量测时标往前推（否则 read_status().last_update 会报告一个
    // 并没有发生量测的时刻）。
    // -----------------------------------------------------------------
    void scan(bool with_holding, Timestamp ts) {
        if (!client_.is_connected() && !try_reconnect()) {
            // 完全没连上：一个块都没机会试。**不**逐个点去标 stale ——
            // 那会制造 32 条噪声记录，掩盖"其实只是没连上"这一个事实。
            for (std::size_t k = 0; k < modbus::kReadBlockCount; ++k) block_ok_[k] = false;
            ++unconnected_scans_;
            ++scans_;
            return;
        }

        int failed = 0;
        for (std::size_t k = 0; k < modbus::kReadBlockCount; ++k) {
            const modbus::ReadBlock& blk = modbus::kReadBlocks[k];
            if (!with_holding && blk.table == modbus::Table::kHoldingReg) continue;

            modbus::ModbusStatus st;
            bool ok = false;
            switch (blk.table) {
            case modbus::Table::kInputReg:
                st = client_.read_input(blk.start, blk.count, ir_regs_.data());
                break;
            case modbus::Table::kHoldingReg:
                st = client_.read_holding(blk.start, blk.count, hr_regs_.data());
                break;
            case modbus::Table::kDiscreteInput:
                st = client_.read_discrete(blk.start, blk.count, di_bits_.data());
                break;
            case modbus::Table::kCoil:
                st = client_.read_coils(blk.start, blk.count, co_bits_.data());
                break;
            }
            ok = st.ok();
            block_ok_[k] = ok;

            if (!ok) {
                ++failed;
                ++block_fails_;
                last_transport_error_ = st.error;
                if (st.is_transport()) {
                    ++transport_fails_;
                    client_.close();   // 传输层坏了：关掉，下一拍按节奏重连
                }
            }
            apply_block(k, ok);
        }

        ++scans_;
        if (failed > 0) ++partial_scans_;
        if (ts > 0.0) last_scan_ts_ = ts;

        if (!client_.is_connected() && cfg_.auto_reconnect) try_reconnect();
    }

    // 把一个分块的结果落到点缓存上。
    // ok=false → 该块覆盖的点**保留旧值**，但 block_ok_ 已置 false，
    //            于是 point_valid() 返回 false（契约①的"可信性另行表达"）。
    void apply_block(std::size_t block_index, bool ok) {
        if (!ok) return;   // block_ok_ 已在 scan() 里置好，点值原地保留

        const modbus::ReadBlock& blk = modbus::kReadBlocks[block_index];
        for (std::size_t i = 0; i < modbus::kBindingCount; ++i) {
            const modbus::PointBinding& b = modbus::kBindings[i];
            if (b.table != blk.table) continue;
            const std::uint16_t width =
                (b.encoding == modbus::Encoding::kBit) ? 1 : b.reg_count();
            if (b.address < blk.start ||
                static_cast<std::uint32_t>(b.address) + width >
                    static_cast<std::uint32_t>(blk.start) + blk.count) {
                continue;   // 不属于本块
            }

            double v = 0.0;
            const modbus::DecodeError e = modbus::decode_point(
                b, regs_of(blk.table), reg_count_of(blk.table),
                bits_of(blk.table), bit_count_of(blk.table), &v);
            if (e != modbus::DecodeError::kNone) {
                // ★ 解码失败**不覆盖缓存**：保留上一次有效值。
                //   字序配错的现场表现就是这里持续报 kNonFinite，
                //   而值还停在上一拍 —— "设备没坏，是表配错了"。
                point_ok_[i]       = false;
                last_decode_error_ = e;
                ++decode_fails_;
                continue;
            }
            cache_[i]    = v;
            point_ok_[i] = true;
        }
    }

    // 分块缓冲（大小取 64/64/16；分块表的上限由 modbus_point_map.h 的
    // static_assert 守着 —— 那边一旦调大超过这里，编译就会红。
    // 用取地址 + sizeof 的双重校验：运行期也给一次硬检查。）
    std::uint16_t* regs_of(modbus::Table t) {
        return (t == modbus::Table::kInputReg) ? ir_regs_.data() : hr_regs_.data();
    }
    std::size_t reg_count_of(modbus::Table t) const {
        return (t == modbus::Table::kInputReg) ? ir_regs_.size() : hr_regs_.size();
    }
    bool* bits_of(modbus::Table t) {
        return (t == modbus::Table::kDiscreteInput) ? di_bits_.data() : co_bits_.data();
    }
    std::size_t bit_count_of(modbus::Table t) const {
        return (t == modbus::Table::kDiscreteInput) ? di_bits_.size() : co_bits_.size();
    }

    // 带节奏的重连（见 Config::reconnect_interval_ms 的说明）。
    // 不做无限忙等：现场一个离线设备不能把控制周期拖垮。
    bool try_reconnect() {
        if (!cfg_.auto_reconnect) return false;
        const auto now = clock::now();
        if (reconnect_attempted_ &&
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_connect_attempt_).count() < cfg_.reconnect_interval_ms) {
            return client_.is_connected();
        }
        return connect();
    }

    double value(std::size_t index) const { return cache_[index]; }

    static double clamp(double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // 数据可信性 = 设备自报的有效位 **且** 本拍没有分块失败。
    //
    // 为什么要两层：设备报 valid=1 只说明"我这边的数据是好的"，
    // 不代表"你要的三块我都答上了"；反过来三块都答上了但设备说 valid=0
    // （它自己正在重启）也不能用。两者都满足才敢把量测交给安全层。
    bool data_valid() const {
        if (value(EMS_STA_VALID) <= 0.5) return false;
        for (std::size_t k = 0; k < modbus::kReadBlockCount; ++k) {
            if (!block_ok_[k]) return false;
        }
        return true;
    }

    Config cfg_{};

    // 值缓存：初值 = 0。未采集前不瞎猜（点表默认值在设备侧，
    // 主站没读到时"没有值"比"凭空给一个默认值"更诚实）。
    double cache_[modbus::kBindingCount]   = {0.0};
    bool   point_ok_[modbus::kBindingCount] = {false};
    bool   block_ok_[modbus::kReadBlockCount] = {false};

    mutable std::array<std::uint16_t, 64> ir_regs_{};
    mutable std::array<std::uint16_t, 64> hr_regs_{};
    mutable std::array<bool, 16>          di_bits_{};
    mutable std::array<bool, 16>          co_bits_{};

    modbus::ModbusTcpClient client_{};
    modbus::DecodeError last_decode_error_    = modbus::DecodeError::kNone;
    modbus::ModbusError last_transport_error_ = modbus::ModbusError::kNone;

    int scans_             = 0;
    int partial_scans_     = 0;
    int block_fails_       = 0;
    int decode_fails_      = 0;
    int transport_fails_   = 0;
    int unconnected_scans_ = 0;
    int writes_            = 0;
    int write_failures_    = 0;
    int encode_failures_   = 0;
    int reconnects_        = 0;

    bool reconnect_attempted_ = false;
    clock::time_point last_connect_attempt_{};

    Timestamp last_scan_ts_ = 0.0;

    PowerCommand last_cmd_{};
    bool         has_last_cmd_ = false;
};

} // namespace ems
