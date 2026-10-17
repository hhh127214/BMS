// =====================================================================
// 21/ — Meter645DeviceIO（IDeviceIO 的第五个实现：DL/T645-2007 电表）
//
// 定位：IDeviceIO 的实现者序列
//   SimDeviceIO     —— 进程内物理模型（07/）
//   MemoryDeviceIO  —— 进程内点表（07/）
//   RtDbDeviceIO    —— 跨进程共享内存（07/src/rtdb）
//   ModbusDeviceIO  —— Modbus TCP 主站（13/）
//   Meter645DeviceIO —— 本文件，DL/T645 主站，读关口电表
//
// ---------------------------------------------------------------------
// ★ 与 ModbusDeviceIO 的三个本质差别（都是协议性质决定的，不是偷懒）
// ---------------------------------------------------------------------
//
// ① **它是"一块表"，不是"一条总线上的聚合网关"。**
//    Modbus 的默认绑定覆盖 32 点（BMS/PCS/电表挂在同一网关后面）。
//    645 电表只答得出自己测的那一路 —— 默认绑定只有 EMS_P_GRID
//    （见 meter645_point_map.h 的文件头）。于是快照里 SOC/P_BAT/温度
//    恒为 0，**这是"没有值"，不是"读到 0"**：它们的有效性由
//    meters_alive["BMS"]=false / ["PCS"]=false 表达，安全层据此走
//    bms_comm_lost → [0,0] 的独立兜底（05/）。
//
// ② **它永远只读。**
//    电表是计量设备，没有"接收功率指令"的语义。write_command() 与
//    execute() 永远返回失败并计数（rejected_writes_）—— 调用它们的
//    唯一路径是 main_field --control，而那条路径在参数解析层
//    （field_args.h）就被拒了：645 + --control 是**语义错误**，
//    不是"暂时不支持"。
//
// ③ **品质位与 Modbus 一样是合成的，但粒度更粗。**
//    Modbus 有 per-block + per-point 两级（一次快照 = 3 次请求）。
//    645 一次事务读一个 DI，所以 per-point 就是 per-transaction：
//      point_valid(i) = 本拍该 DI 的事务成功且解码通过
//    "失败保留上一次有效值"（04/ 契约①）照旧：失败只清有效位，
//    不覆盖缓存 —— 一个过期的关口功率比一个凭空捏造的 0 更诚实，
//    且 data_valid 会把"它过期了"说出来。
//
// ---------------------------------------------------------------------
// ★ data_valid 的口径（与四个兄弟适配器的对齐方式）
// ---------------------------------------------------------------------
//   本适配器只声明**自己负责的点**的有效性：全部绑定点本拍采到 → true。
//   它没有设备自报的 STA.VALID 可读（645 协议里没有这个寄存器），
//   所以"设备说自己好不好"这一层**不存在**，合成位就是唯一的品质来源。
//   这与 Modbus 的 "STA.VALID ∧ 三块全成" 不同 —— 那里设备自报位存在；
//   这里如实记录差别，不把"没有"伪装成"有"。
//
// ---------------------------------------------------------------------
// ★ 绑定表可注入（构造函数参数）
// ---------------------------------------------------------------------
//   默认用 meter645_point_map.h 的内置表；测试与现场换表时可以注入
//   自定义表。注入表**先过 self_check_map**：不过就拒绝采集
//   （map_ok_=false，全部点不可信），绝不用一张坏表去读真表 ——
//   坏表的表现是"通信正常但数全错"，比连不上更难查。
//
// 编译：纯头文件。需要 -I 04/src（device_io.h）、07/src/rtdb（ems_point_table.h）。
//       TCP 链路由调用方装配（21/src/dlt645_tcp_link.h，-lws2_32）。
// =====================================================================

#pragma once

#include "device_io.h"             // 04/  IDeviceIO 契约
#include "ems_point_table.h"       // 07/  40 点抽象表
#include "dlt645_master.h"         // 21/  主站事务层
#include "meter645_point_map.h"    // 21/  EMS 点 ↔ DI 绑定

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ems {

class Meter645DeviceIO : public IDeviceIO {
public:
    // 绑定表行数上限：一块 645 表能供给 EMS 的点是个位数（关口功率、
    // 也许加分路），给 16 是"宽松但有限"——超过它说明把别的设备的点
    // 塞进了电表通道，那正是要被拦下来的装配错误。
    static constexpr std::size_t kMaxBindings = 16;

    Meter645DeviceIO(dlt645::IByteLink* link, const dlt645::Address& meter,
                     const dlt645::MasterOptions& opt = dlt645::MasterOptions(),
                     const meter645::PointBinding* table = meter645::kBindings,
                     std::size_t count = meter645::kBindingCount)
        : master_(link, meter, opt), table_(table), count_(count) {
        // 注入表先自检；行数超上限同样视为坏表（数组容量是硬边界）
        if (count_ > kMaxBindings) {
            map_ok_ = false;
        } else {
            map_ok_ = (meter645::self_check_map(table_, count_).total == 0);
        }
    }

    // =================================================================
    // IDeviceIO 实现
    // =================================================================
    // ① 读量测快照（冻结语义）
    bool read_snapshot(Timestamp now, RealtimeSnapshot& out) override {
        scan(now);

        // 本适配器默认只拥有 EMS_P_GRID；其余字段保持缓存初值 0（= "没有值"，
        // 不是"读到 0"），其有效性由 meters_alive / data_valid 另行表达。
        out.timestamp       = now;
        out.p_bat_actual_kw = cache_[EMS_P_BAT];
        out.p_pv_kw         = cache_[EMS_P_PV];
        // 站用电计入负荷侧 —— 与另外四个适配器同一口径（公式照抄，
        // 两个点在本适配器默认无来源，结果恒 0，不特例化）。
        out.p_load_kw       = std::max(0.0, cache_[EMS_P_LOAD] + cache_[EMS_CFG_STANDBY]);
        out.p_grid_kw       = cache_[EMS_P_GRID];
        out.soc             = clamp(cache_[EMS_SOC], 0.0, 1.0);
        out.temperature_c   = cache_[EMS_T_C];
        out.soh             = clamp(cache_[EMS_SOH], 0.0, 1.0);
        out.has_lookahead   = false;

        // 三路"活着"判定：本通道只有电表一路；BMS/PCS 不经由本适配器，
        // 恒 false —— 这是诚实的"我不知道"，不是"它们断了"。
        // 安全层对 bms_comm_ok=false 的反应是 [0,0] 兜底（05/），方向安全。
        out.meters_alive["BMS"]   = false;
        out.meters_alive["PCS"]   = false;
        out.meters_alive["METER"] = meter_fresh_;

        // 电价窗口：独立使用时的保守默认（与 ModbusDeviceIO 同一口径，
        // EmsRuntime 会用预报曲线覆盖）。
        const double h = std::fmod(now, 86400.0) / 3600.0;
        const bool valley = (h < 8.0 || h >= 22.0);
        out.pricing.cur_tou_type  = valley ? TouType::kValley : TouType::kPeak;
        out.pricing.cur_tou_price = valley ? 0.30 : 0.90;

        return meter_fresh_;
    }

    // ② 读设备运行时限值
    //
    // 645 电表**没有**限值寄存器可读（协议里就没有这个语义）：
    //   变压器容量、需量目标是装配配置（17/ 第 1 层把它们挂在 CFG.METER.*），
    //   不是从表里读出来的。逐字段显式置 0 ——
    //   ★ 不能用 `DeviceLimits{}`：它的成员默认值是工程值（100/250…），
    //     "电表通道给出一组编造的限值"比"没有限值"危险得多
    //     （上层看到 0 会立刻发现"没有容量信息"，看到 250 会安静地算错）。
    // 禁充放位 false：本通道没有 BMS，"false + meters_alive[BMS]=false"
    //   会被安全层走 comm_lost 兜底，不会误放。
    bool read_limits(DeviceLimits& out) override {
        out = DeviceLimits{};
        out.pcs_rated_chg_kw        = 0.0;
        out.pcs_rated_dis_kw        = 0.0;
        out.bms_chg_limit_kw        = 0.0;
        out.bms_dis_limit_kw        = 0.0;
        out.transformer_capacity_kw = 0.0;
        out.d_target_kw             = 0.0;
        out.bms_chg_forbidden       = false;
        out.bms_dis_forbidden       = false;
        return true;
    }

    // ③ 读通信/故障状态（全部合成，645 没有品质位）
    DeviceStatus read_status() const override {
        DeviceStatus s;
        s.bms_comm_ok    = false;   // 不经本通道（见 ①）
        s.pcs_comm_ok    = false;   // 同上
        s.meter_comm_ok  = meter_fresh_;
        s.pcs_fault      = false;   // 电表无从知晓 PCS 故障
        s.device_offline = (scans_ > 0) && !meter_fresh_;
        s.data_valid     = meter_fresh_;
        s.last_update    = last_scan_ts_;
        return s;
    }

    // ④ 读实际运行值（记录/展示）
    //
    // ★ DeviceActuals 的默认值是"仿真味"的（soc=0.5、temperature_c=25.0）——
    //   本通道没有这两个量，若照抄默认值，实录 CSV 里会出现一条
    //   "SOC 恒 50%、温度恒 25℃"的假曲线。显式置 0：0 可读作"没有值"，
    //   50% 读不出来。有效性仍以 read_status() / meters_alive 为准。
    DeviceActuals read_actuals() const override {
        DeviceActuals a;
        a.p_grid_kw     = cache_[EMS_P_GRID];   // 本通道默认唯一的真实量测
        a.soc           = 0.0;
        a.temperature_c = 0.0;
        return a;
    }

    // ⑤ 写功率指令：永远拒绝（见文件头 ②）。
    //    计数必须留下 —— "有人试图往电表写指令"这件事本身是装配错误的信号。
    bool write_command(const PowerCommand& cmd) override {
        (void)cmd;
        ++rejected_writes_;
        return false;
    }

    // ⑥ 推进一拍：同样拒绝写。返回值是最近一次采到的关口功率
    //    （尽力反馈，闭环不得依赖它 —— 接口契约 ③）。
    double execute(double p_cmd_kw, double dt_s) override {
        (void)p_cmd_kw;
        (void)dt_s;
        ++rejected_writes_;
        return cache_[EMS_P_GRID];
    }

    // ⑦ 电池容量：电表不知道 → 0（理由同 read_limits）
    double battery_capacity_kwh() const override { return 0.0; }

    const char* name() const override { return "Meter645DeviceIO(DL/T645-2007)"; }

    // 限值不从设备来（见 ②），恒为装配配置 → false。
    bool limits_are_live() const override { return false; }

    // =================================================================
    // 诊断（现场排障 + 测试判据）
    // =================================================================
    int scans()           const { return scans_; }
    int collected()       const { return collected_; }   // 累计采到的点次数
    int failed_reads()    const { return failed_reads_; }
    int rejected_writes() const { return rejected_writes_; }
    bool map_ok()         const { return map_ok_; }      // 注入表自检结果
    dlt645::ReadStatus last_read_status() const { return last_status_; }
    Timestamp last_scan_timestamp() const { return last_scan_ts_; }

    // 本点的值当前是否可信 = 本拍该点的事务成功且解码通过。
    // 645 一次事务一个 DI，所以 per-point 就是 per-transaction（文件头 ③）。
    bool point_valid(std::size_t index) const {
        for (std::size_t i = 0; i < count_; ++i) {
            if (table_[i].index == index) return point_ok_[i];
        }
        return false;   // 未绑定的点：永远没有可信值（与"读到 0"严格区分）
    }

    double cached(std::size_t index) const {
        return (index < static_cast<std::size_t>(EMS_EXT_BEGIN)) ? cache_[index] : 0.0;
    }

    // 主站本体（测试要挂 pump 钩子驱动进程内假表；现场排障看 stats()）
    dlt645::Dlt645Master& master() { return master_; }
    const dlt645::Dlt645Master& master() const { return master_; }

private:
    // -----------------------------------------------------------------
    // 扫描：逐绑定发事务，成功写缓存、失败只清有效位（契约①）
    // -----------------------------------------------------------------
    void scan(Timestamp ts) {
        if (!map_ok_) {
            // 坏表不采集：全部点不可信。这比"照着坏表读出一堆怪数"安全。
            for (std::size_t i = 0; i < count_; ++i) point_ok_[i] = false;
            meter_fresh_ = false;
            ++scans_;
            return;
        }

        bool all_ok = (count_ > 0);
        for (std::size_t i = 0; i < count_; ++i) {
            const meter645::PointBinding& b = table_[i];
            const dlt645::ReadResult r = master_.read(b.di, b.scale);
            if (r.st == dlt645::ReadStatus::kOk) {
                cache_[b.index] = r.value;
                point_ok_[i]    = true;
                ++collected_;
            } else {
                // ★ 失败**不覆盖缓存**：保留上一次有效值。
                //   与 Modbus 的 apply_block(ok=false) 同一条纪律：
                //   "值还在，但 point_valid 会告诉你它过期了"。
                point_ok_[i]  = false;
                last_status_  = r.st;
                all_ok        = false;
                ++failed_reads_;
            }
        }
        meter_fresh_ = all_ok;
        ++scans_;
        // ★ 时标只在**采到了**的拍推进：DeviceStatus.last_update 的契约是
        //   "最近一次成功采集时刻"（04/device_io.h），失败的拍推进它会
        //   让"数据多久没刷新"永远显示 0 秒 —— 恰好掩盖断链。
        if (meter_fresh_ && ts > 0.0) last_scan_ts_ = ts;
    }

    static double clamp(double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    dlt645::Dlt645Master master_;
    const meter645::PointBinding* table_ = meter645::kBindings;
    std::size_t count_ = meter645::kBindingCount;

    // 值缓存：初值 0。未采集前不瞎猜（与 ModbusDeviceIO 同一条注释的口径）。
    double cache_[EMS_EXT_BEGIN] = {0.0};
    bool   point_ok_[kMaxBindings] = {false};

    bool map_ok_      = false;   // 构造时自检注入表
    bool meter_fresh_ = false;   // 本拍全部绑定点采到
    dlt645::ReadStatus last_status_ = dlt645::ReadStatus::kLinkError;

    int scans_           = 0;
    int collected_       = 0;
    int failed_reads_    = 0;
    int rejected_writes_ = 0;

    Timestamp last_scan_ts_ = 0.0;
};

} // namespace ems
