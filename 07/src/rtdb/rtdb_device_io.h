// =====================================================================
// 07/rtdb — 共享内存实时库适配器 RtDbDeviceIO（RT_DB 接入）
//
// 定位：IDeviceIO 的**现场实现**之一。EMS 算法（05/06/07/08）只经
//       IDeviceIO 读写设备；本文件把「共享内存实时库 RT_DB」接进这个接口。
//
// 与 MemoryDeviceIO（P0.5 进程内点表）的关系：
//   · **同一份点表** —— src/rtdb/ems_point_table.h（30 点），点名与
//     memory_device_io.h 的 mem_point:: 逐字相同；
//   · 唯一区别是存储介质：MemoryDeviceIO 是进程内 unordered_map，
//     本适配器是**跨进程共享内存**（Windows: CreateFileMapping /
//     MapViewOfFile；Linux: shmget / shmat）。
//   于是「换数据源不改算法」从 P0.5 的**进程内证明**升级为**跨进程证明**。
//
// 关键纪律（P0 的纪律在真实适配器上更不能破）：
//   1. **点名只允许出现在本文件**（含设备侧写点器）。算法层看到的永远是
//      RealtimeSnapshot / DeviceLimits / DeviceStatus / PowerCommand。
//      若哪天算法里写了 "MEAS.SOC"，说明分层已经破了。
//   2. **execute() 的返回值不可信**。真实系统里 execute() 只是「把指令写进
//      实时库 + 等一拍」，实际功率由设备在下一拍写回 MEAS.P_BAT；
//      闭环必须走 read_snapshot()（接口契约 ③）。
//   3. **采集失败必须填最近一次有效值**（接口契约 ①）。本文件维护一份点值
//      缓存：读失败或质量位异常时保留缓存并累计 stale_reads()，可信性统一
//      由 read_status().data_valid 表达。
//
// 谁负责建点表：RT_DB 没有「注册点表」的公开 API（其 build_index_map 里
//   `(void)config_path` 把配置参数丢弃了），点表必须由调用方写进共享内存。
//   → src/rtdb/ems_rt_db_setup.c 负责装配阶段建段 + 注册 30 点；
//   → 本适配器只做**读写**，并在 self_check() 里校验索引/点名/单位。
//
// 编译（见 07/scripts/build_test_rtdb.bat）：
//   · 头文件搜索路径：-I src -I src/rtdb -I vendor/rt_db -I ../04/src
//   · 需要链接 RT_DB 的 C 实现：
//       vendor/rt_db/rt_db_api.c、src/rtdb/ems_point_table.c、
//       src/rtdb/ems_rt_db_setup.c
//
// 为什么本文件不 include rt_db_structs.h：它会带进 <windows.h>，其中的
//   min/max 宏会破坏项目里大量 std::max / std::min（不定义 NOMINMAX 就编译
//   不过）。因此这里只**镜像**它需要的常量，并在测试里做漂移守卫（T25）。
// =====================================================================

#pragma once

#include "device_io.h"        // 04/  IDeviceIO / DeviceStatus / DeviceActuals
#include "ems_point_table.h"  // 07/src/rtdb  点表真相源（C 头，extern "C"）
#include "rt_db_api.h"        // 07/vendor/rt_db  RT_DB C API

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <utility>

namespace ems {

// ---------------------------------------------------------------------
// 镜像自 vendor/rt_db/rt_db_structs.h 的常量（见文件头说明）
// ---------------------------------------------------------------------
static constexpr std::size_t kRtdbMaxPointIdLen = 64;     // ≡ MAX_POINT_ID_LEN
static constexpr std::size_t kRtdbMaxUnitLen    = 16;     // ≡ MAX_UNIT_LEN
static constexpr std::size_t kRtdbMaxPoints     = 20000;  // ≡ MAX_DATA_POINTS

namespace rtdb_quality {                                  // ≡ data_quality_t
constexpr long kBad       = 0;
constexpr long kGood      = 1;
constexpr long kUncertain = 2;
} // namespace rtdb_quality

// =====================================================================
// 设备侧写点器 —— 真实系统里这是**另一个进程**（设备 / SCADA 进程）
//
// 为什么单独抽一个类：它与 RtDbDeviceIO 是**相反方向**的两个人
//   设备侧：把测点写进实时库（MEAS / STA / CFG）
//   EMS 侧：从实时库读量测，把指令写进实时库（CMD）
// 混在一起会让「谁写谁读」不可追溯，也会给算法层误用的机会。
// 测试/演示用它模拟设备进程（见 tests/test_rtdb_device_io.cpp）。
// =====================================================================
class RtDbPointWriter {
public:
    RtDbPointWriter() = default;
    explicit RtDbPointWriter(rt_db_handle_t* h) : h_(h) {}

    void attach(rt_db_handle_t* h) { h_ = h; }
    rt_db_handle_t* handle() const { return h_; }
    bool ok() const { return h_ != nullptr && h_->shm_addr != nullptr; }

    // 按索引写（索引来自 ems_point_table.h，运行时不做字符串比较）
    bool write(std::size_t index, double value, long quality = rtdb_quality::kGood) {
        return ok() && rt_db_set_value(h_, index, value, quality);
    }
    // 按点名写（现场脚本 / 自检用；批量路径请用索引）
    bool write_by_name(const char* point_id, double value,
                       long quality = rtdb_quality::kGood) {
        if (!ok() || point_id == nullptr) return false;
        const std::size_t index = rt_db_find_index_by_id(h_, point_id);
        return index != static_cast<std::size_t>(-1) &&
               rt_db_set_value(h_, index, value, quality);
    }
    bool read(std::size_t index, double* out, long* quality = nullptr) const {
        return ok() && rt_db_get_value(h_, index, out, quality, nullptr);
    }
    bool read_by_name(const char* point_id, double* out,
                      long* quality = nullptr) const {
        if (!ok() || point_id == nullptr) return false;
        const std::size_t index = rt_db_find_index_by_id(h_, point_id);
        return index != static_cast<std::size_t>(-1) &&
               rt_db_get_value(h_, index, out, quality, nullptr);
    }

private:
    rt_db_handle_t* h_ = nullptr;
};

// =====================================================================
// EMS 侧适配器：共享内存实时库 → IDeviceIO
// =====================================================================
class RtDbDeviceIO : public IDeviceIO {
public:
    // 设备侧推进泵。真实系统里「设备推进一拍」由硬件/另一个进程完成，
    // 本进程内没有设备 —— 设置本回调后 execute() 会调用它，从而让
    // 「跨内存边界闭环」可以在单进程测试里跑通。**不设置 = 纯下发**。
    using DevicePump = std::function<void(double p_cmd_kw, double dt_s)>;

    RtDbDeviceIO() { reset_cache(); }
    explicit RtDbDeviceIO(rt_db_handle_t* borrowed) : external_(borrowed) {
        reset_cache();
    }
    ~RtDbDeviceIO() override { close_owned(); }

    // =================================================================
    // 连接管理
    // =================================================================
    // 借用装配层持有的连接（**推荐**：一个进程一条连接，生命周期由装配层管）
    void attach_handle(rt_db_handle_t* borrowed) { external_ = borrowed; }

    // 自持连接：内部调 rt_db_init()。返回 false 表示共享内存不存在
    // （提示：先跑 ems_rt_db_setup() 建立段与点表）。
    bool open_owned(const char* config_path = nullptr) {
        close_owned();
        std::memset(&owned_, 0, sizeof(owned_));
        if (!rt_db_init(&owned_, config_path)) return false;
        owned_open_ = true;
        reset_cache();
        return true;
    }
    void close_owned() {
        if (owned_open_) {
            rt_db_cleanup(&owned_);
            owned_open_ = false;
        }
    }

    bool is_open() const { return handle() != nullptr; }
    rt_db_handle_t* handle() const {
        if (external_ != nullptr && external_->shm_addr != nullptr) return external_;
        if (owned_open_) return &owned_;
        return nullptr;
    }

    // =================================================================
    // 自检 / 诊断（现场排障：点表对不对得上只有共享内存知道）
    // =================================================================
    // 校验「共享内存里的点表」与「编译期点表 ems_point_table.h」是否一致
    // （点名 / 单位 / 索引三者全查）。返回不一致的点数，0 = 完全一致。
    int self_check() const {
        const rt_db_handle_t* h = handle();
        if (h == nullptr) return static_cast<int>(EMS_POINT_COUNT);

        int bad = 0;
        for (std::size_t i = 0; i < EMS_POINT_COUNT; ++i) {
            char point_id[kRtdbMaxPointIdLen] = {0};
            char units[kRtdbMaxUnitLen]       = {0};
            const bool info_ok = rt_db_get_point_info(h, i, point_id, units);
            const std::size_t found = rt_db_find_index_by_id(h, EMS_POINT_NAMES[i]);

            const bool name_ok  = info_ok && std::strcmp(point_id, EMS_POINT_NAMES[i]) == 0;
            const bool unit_ok  = info_ok && std::strcmp(units, EMS_POINT_UNITS[i]) == 0;
            const bool index_ok = (found == i);
            if (!(name_ok && unit_ok && index_ok)) ++bad;
        }
        return bad;
    }

    // 读点失败/质量位异常的次数。点表未初始化、共享内存未建立时会持续增长
    // —— 现场自检应把它当告警，而不是当成"读到 0.0"。
    int stale_reads() const { return stale_reads_; }
    // 成功写进实时库的点数（下发指令都算）
    int writes() const { return writes_; }
    // 本地缓存里的点值（诊断用；与共享内存同源，但不是实时值）
    double cached_value(std::size_t index) const {
        return (index < EMS_POINT_COUNT) ? cache_[index] : 0.0;
    }

    // 设备侧推进泵（**非** IDeviceIO 契约，仅测试/演示用）
    void set_device_pump(DevicePump pump) { pump_ = std::move(pump); }

    // =================================================================
    // IDeviceIO 实现
    // =================================================================
    // ① 读量测快照（冻结语义）。采集失败不抛异常、不返回半成品：
    //    保留最近一次有效值（见 refresh()），可信性由 read_status() 表达。
    bool read_snapshot(Timestamp now, RealtimeSnapshot& out) override {
        refresh_all();

        const double p_bat = cached_value(EMS_P_BAT);
        const double p_pv  = cached_value(EMS_P_PV);
        // 站用电计入负荷侧（与 PlantModel::sample() / MemoryDeviceIO 同一口径）
        const double p_load =
            std::max(0.0, cached_value(EMS_P_LOAD) + cached_value(EMS_CFG_STANDBY));

        out.timestamp       = now;
        out.p_bat_actual_kw = p_bat;
        out.p_pv_kw         = p_pv;
        out.p_load_kw       = p_load;
        out.p_grid_kw       = p_load - p_pv - p_bat;
        out.soc             = clamp01(cached_value(EMS_SOC));
        out.temperature_c   = cached_value(EMS_T_C);
        out.soh             = cached_value(EMS_SOH);
        // 前瞻：本点表暂无预报点（现场预报由上层刷新，或后续在点表里加点）。
        // 保持 false —— 与 MemoryDeviceIO 一致，安全层行为与历史完全相同。
        out.has_lookahead = false;

        const bool offline = cached_value(EMS_STA_OFFLINE) > 0.5;
        out.meters_alive["BMS"]   = cached_value(EMS_STA_BMS)   > 0.5 && !offline;
        out.meters_alive["METER"] = cached_value(EMS_STA_METER) > 0.5 && !offline;
        out.meters_alive["PCS"]   = cached_value(EMS_STA_PCS)   > 0.5 && !offline;

        // 电价窗口：给独立使用时的保守默认值（EmsRuntime 会用预报曲线覆盖）。
        // 现场版应改为读实时库里的电价点。
        const double h = std::fmod(now, 86400.0) / 3600.0;
        const bool valley = (h < 8.0 || h >= 22.0);
        out.pricing.cur_tou_type  = valley ? TouType::kValley : TouType::kPeak;
        out.pricing.cur_tou_price = valley ? 0.30 : 0.90;

        return data_valid();
    }

    // ② 读设备运行时限值。**装配阶段**（attach_device / apply_configs）调用，
    //    因此设备侧必须先把 CFG.* 写进实时库；现场 BMS 动态降功率就是靠
    //    定期刷新这几个点生效的。
    bool read_limits(DeviceLimits& out) override {
        refresh_all();
        out = DeviceLimits{};
        out.pcs_rated_chg_kw        = cached_value(EMS_CFG_MAX_CHG);
        out.pcs_rated_dis_kw        = cached_value(EMS_CFG_MAX_DIS);
        out.bms_chg_limit_kw        = cached_value(EMS_CFG_BMS_CHG_LIM);
        out.bms_dis_limit_kw        = cached_value(EMS_CFG_BMS_DIS_LIM);
        out.transformer_capacity_kw = cached_value(EMS_CFG_TR_KVA);
        out.d_target_kw             = cached_value(EMS_CFG_D_TARGET);
        // 禁充/禁放位：当前点表未建模（与 MemoryDeviceIO 对齐）。
        // 现场若需按 BMS 禁充放硬封锁，在点表里加点后在此处映射即可。
        out.bms_chg_forbidden = false;
        out.bms_dis_forbidden = false;
        return true;
    }

    // ③ 读通信/故障状态（算法侧唯一的故障语义来源）
    DeviceStatus read_status() const override {
        refresh(EMS_STA_BMS);
        refresh(EMS_STA_PCS);
        refresh(EMS_STA_METER);
        refresh(EMS_STA_FAULT);
        refresh(EMS_STA_OFFLINE);
        refresh(EMS_STA_VALID);

        DeviceStatus s;
        s.bms_comm_ok    = cached_value(EMS_STA_BMS)     > 0.5;
        s.pcs_comm_ok    = cached_value(EMS_STA_PCS)     > 0.5;
        s.meter_comm_ok  = cached_value(EMS_STA_METER)   > 0.5;
        s.pcs_fault      = cached_value(EMS_STA_FAULT)   > 0.5;
        s.device_offline = cached_value(EMS_STA_OFFLINE) > 0.5;
        s.data_valid     = data_valid();
        return s;
    }

    // ④ 读实际运行值（记录/展示；非闭环回路 —— 闭环用 ①）
    DeviceActuals read_actuals() const override {
        refresh(EMS_P_BAT);
        refresh(EMS_P_GRID);
        refresh(EMS_P_LOAD);
        refresh(EMS_P_PV);
        refresh(EMS_SOC);
        refresh(EMS_T_C);

        DeviceActuals a;
        a.p_bat_kw      = cached_value(EMS_P_BAT);
        a.p_grid_kw     = cached_value(EMS_P_GRID);
        a.p_load_kw     = cached_value(EMS_P_LOAD);
        a.p_pv_kw       = cached_value(EMS_P_PV);
        a.soc           = cached_value(EMS_SOC);
        a.temperature_c = cached_value(EMS_T_C);
        return a;
    }

    // ⑤ 写功率指令（含权限区间）到实时库；设备/SCADA 进程从这里取。
    bool write_command(const PowerCommand& cmd) override {
        last_cmd_     = cmd;
        has_last_cmd_ = true;

        bool ok = true;
        ok = set_point(EMS_CMD_P_BAT,   cmd.p_bat_cmd_kw) && ok;
        ok = set_point(EMS_CMD_P_UPPER, cmd.p_upper)      && ok;
        ok = set_point(EMS_CMD_P_LOWER, cmd.p_lower)      && ok;
        return ok;
    }

    // ⑥ 推进一拍。**真实适配器不做物理积分** —— 它只负责「把指令写进实时库」
    //    然后等设备把实际值写在 MEAS.P_BAT 上（下一拍 read_snapshot 才看得到）。
    double execute(double p_cmd_kw, double dt_s) override {
        // 权限区间只有装配层调用过 write_command() 时才有意义；没有它就**不要写**
        // —— 写 0/0 会被设备侧误读成"禁止动作"，那是危险的动作。
        set_point(EMS_CMD_P_BAT, p_cmd_kw);
        if (has_last_cmd_) {
            set_point(EMS_CMD_P_UPPER, last_cmd_.p_upper);
            set_point(EMS_CMD_P_LOWER, last_cmd_.p_lower);
        }

        // 设备侧推进（真实系统里是另一个进程/硬件；测试用泵搬进本进程）
        if (pump_) pump_(p_cmd_kw, dt_s);

        // 返回值 = 本拍量测（尽力反馈，仅供记录）。**闭环不依赖它**。
        refresh(EMS_P_BAT);
        return cached_value(EMS_P_BAT);
    }

    double battery_capacity_kwh() const override {
        refresh(EMS_CFG_CAP_KWH);
        return cached_value(EMS_CFG_CAP_KWH);
    }

    const char* name() const override { return "RtDbDeviceIO(SharedMemory)"; }

private:
    static double clamp01(double v) {
        return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    }

    // 缓存初值 = 点表默认值（点表默认已与 DeviceLimits / PlantConfig 对齐）。
    // 未采集前不瞎猜：现场"连不上却报 0"比"报默认值"更危险。
    void reset_cache() {
        for (std::size_t i = 0; i < EMS_POINT_COUNT; ++i) {
            cache_[i]      = EMS_POINT_DEFAULTS[i];
            quality_ok_[i] = true;
        }
    }

    // 读一个点：成功 → 更新缓存；失败/质量异常 → **保留缓存**并计数。
    // 这就是接口契约 ① 的落地：「采集失败时填最近一次有效值」。
    bool refresh(std::size_t index) const {
        const rt_db_handle_t* h = handle();
        if (h == nullptr || index >= EMS_POINT_COUNT) {
            ++stale_reads_;
            return false;
        }
        double v = 0.0;
        long q   = rtdb_quality::kBad;
        if (!rt_db_get_value(h, index, &v, &q, nullptr)) {
            ++stale_reads_;
            quality_ok_[index] = false;
            return false;
        }
        cache_[index]      = v;
        quality_ok_[index] = (q == rtdb_quality::kGood);
        return true;
    }
    void refresh_all() const {
        for (std::size_t i = 0; i < EMS_POINT_COUNT; ++i) refresh(i);
    }

    // 数据可信性 = 实时库的数据有效位 **且** 量测点品质位全为 GOOD。
    // 只信"值"不信"品质位"是现场最常见的坑（NaN / 陈旧值会被当有效值用）。
    // 注意：品质位是**随采集刷新**的（read_snapshot / read_actuals），因此
    // read_status() 的可信性反映"最近一次采集"。EmsRuntime 每拍固定先
    // read_snapshot 再 read_status（step() 的 ①→②），顺序天然正确。
    bool data_valid() const {
        bool ok = cached_value(EMS_STA_VALID) > 0.5;
        for (std::size_t i = 0; i <= static_cast<std::size_t>(EMS_SOH); ++i) {
            ok = ok && quality_ok_[i];
        }
        return ok;
    }

    bool set_point(std::size_t index, double value) {
        rt_db_handle_t* h = handle();
        if (h == nullptr || index >= EMS_POINT_COUNT) return false;
        const bool ok = rt_db_set_value(h, index, value, rtdb_quality::kGood);
        if (ok) {
            ++writes_;
            cache_[index]      = value;
            quality_ok_[index] = true;
        }
        return ok;
    }

    rt_db_handle_t* external_ = nullptr;   // 借用（推荐）
    mutable rt_db_handle_t owned_{};       // 自持
    mutable bool owned_open_ = false;

    mutable double cache_[EMS_POINT_COUNT]      = {};
    mutable bool   quality_ok_[EMS_POINT_COUNT] = {};
    mutable int    stale_reads_ = 0;
    int            writes_      = 0;

    PowerCommand last_cmd_{};
    bool         has_last_cmd_ = false;
    DevicePump   pump_{};
};

} // namespace ems




