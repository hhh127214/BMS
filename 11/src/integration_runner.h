// =====================================================================
// 11/ — 周期 11：系统级联调（EMS ↔ BMS/PCS/电表/光伏/变压器/负荷 全链路）
//
// 依据：工商业储能EMS调控策略设计方案.md §7 周期 11
//   「完成 EMS、BMS、PCS、电表、光伏、变压器、负荷全链路联调，测试通信、
//     数据采集、控制指令、状态同步、故障触发、异常恢复、日志记录全流程稳定性。
//     周期目标：打通真实设备闭环控制链路。」
//
// 本文件把联调的**两个角色**各封装成一个类，供两种执行形态复用**同一份代码**：
//
//   · DeviceSideSim —— 设备侧进程（BMS/PCS/电表/光伏/变压器/负荷六类数据源）
//   · EmsSideApp    —— EMS 侧进程（RtDbDeviceIO + EmsRuntime + P2 观察者）
//
// 执行形态：
//   形态 1（tests/，自动化）：单进程内两个角色共用一个共享内存段，
//     设备推进由 RtDbDeviceIO 的 device_pump 回调驱动（与 07/T26 同机制）。
//   形态 2（scripts/run_integration.bat，演示）：**真·三进程** ——
//     rtdb_initializer.exe（建段保活）+ device_side.exe + ems_side.exe，
//     两个 worker 各自独立节拍，仅靠共享内存段交换数据。
//
//     "单进程测试验证的就是设备进程的真实逻辑" —— 两种形态跑的是
//     DeviceSideSim::step() / EmsSideApp::step() 同一段代码，这是本模块
//     测试结论能外推到多进程演示的根据。
//
// 角色边界（与 07/RT_DB 接入的纪律一致，联调期进一步收紧）：
//   · 设备侧：只写 MEAS/STA/CFG（29 点），只读 CMD（3 点）
//   · EXT 区（A3.2 起）归网关（RtDbExtWriter）写，设备侧 publish_all() 一并跳过
//   · EMS 侧：只写 CMD（3 点），只读 MEAS/STA/CFG
//   · 点名只允许出现在本文件（算法层看到的是结构体）
//
// 编译：纯头文件，实现全部 inline。
//   头文件搜索路径：src ../04/src ../05/src ../06/src ../07/src ../08/src
//                    ../P2/src + 07 的 src/rtdb、vendor/rt_db
//   链接：rt_db_api.c / ems_point_table.c / ems_rt_db_setup.c（gcc 编 C）
// =====================================================================

#pragma once

#include "data_models.h"
#include "memory_device_io.h"
#include "realtime_loop.h"
#include "rtdb_device_io.h"

// 段的建立与点表注册（初始化器进程与测试装置共用）
#include "ems_rt_db_setup.h"

// P2 可观测性（周期 11 的"日志记录"验收载体）
#include "observe.h"

#include "rt_db_api.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ems {

// =====================================================================
// 进程节拍器：把"模拟时间"映射到"墙钟时间"
//
// 为什么多进程演示必须节拍：两个 worker 进程只有**同时活着**才谈得上联调
// 闭环。设备进程若全速跑完 3000 拍（实测几毫秒），段里的量测就冻住了，EMS
// 侧的所谓"闭环"实际是开环 —— p_actual 永远不变，跟踪误差恒为 0，测试反而
// "更好看"。真实部署里两个进程各有自己的 100 ms 节拍，这里用 sleep 复现。
//
// speed > 1 = 加速（demo 用）：sleep(dt/speed)，模拟时间仍是 dt。
// =====================================================================
class Pacing {
public:
    Pacing(double dt_s, double speed) : dt_(dt_s), speed_(speed > 0.0 ? speed : 1.0) {}

    // 每拍调用一次：按累计目标时刻补偿，避免 sleep 抖动累积成漂移
    void wait() {
        ++n_;
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
        const double want = static_cast<double>(n_) * dt_ / speed_;
        const double s = want - elapsed;
        if (s > 0.0)
            std::this_thread::sleep_for(std::chrono::duration<double>(s));
    }

private:
    std::chrono::steady_clock::time_point t0_ = std::chrono::steady_clock::now();
    double      dt_    = 0.1;
    double      speed_ = 1.0;
    long long   n_     = 0;
};

// =====================================================================
// 故障剧本时间窗（设备侧注入）
//
// 与 10/sim_24h.h 的 FaultWindow 语义相同，但作用对象不同：
//   10/ 的 apply_fault 作用于 EMS 进程内的 rt.plant()（离线仿真）；
//   本文件的剧本由**设备侧**写进共享内存 —— 故障必须"像真的一样"
//   从设备方向传来，EMS 侧只能通过 RtDbDeviceIO 的 DeviceStatus 看到。
//   这是联调与离线仿真的本质差别。
// =====================================================================
struct DevFaultWindow {
    double t_begin_s = 0.0;
    double t_end_s   = 0.0;
    // 1=comm_bms_off 2=comm_meter_off 3=comm_pcs_off
    // 4=pcs_fault    5=device_offline  6=data_quality_bad（P_PV 品质位劣化）
    // 7=bms_forbid_chg 8=bms_forbid_dis
    //
    // ★ 7/8 与 1~6 性质不同：前六种是**故障**（设备坏了 / 通信断了），
    //   7/8 是 BMS **正常上报的运行限值**（"我现在不许充 / 不许放"）。
    //   它们走的是另一条链路：STA.BMS_*_FORBID → read_limits() →
    //   DeviceLimits → 04/S01(kBmsForbid, L0) → 05/ 折进 (p_lower, p_upper)。
    //   放进同一个"剧本"结构，是因为对 EMS 而言两者都"由设备侧说了算"，
    //   都必须经共享内存传过来 —— 这正是联调与离线仿真的本质差别。
    //   代价是**不能**把它计进 comm_fault_ticks（它不是故障，见 apply_script）。
    int    kind      = 0;
    std::string note;

    bool active(double t) const { return t >= t_begin_s && t < t_end_s; }
};

inline const char* dev_fault_kind_name(int kind) {
    switch (kind) {
        case 1: return "BMS_COMM_LOST";
        case 2: return "METER_COMM_LOST";
        case 3: return "PCS_COMM_LOST";
        case 4: return "PCS_FAULT";
        case 5: return "DEVICE_OFFLINE";
        case 6: return "DATA_QUALITY_BAD";
        case 7: return "BMS_FORBID_CHARGE";
        case 8: return "BMS_FORBID_DISCHARGE";
        default: return "NONE";
    }
}

// =====================================================================
// 环境脚本（电表 / 光伏逆变器数据源）
// 两进程（device_side.exe / ems_side 的对照参考）必须用**同一份**函数 ——
// 这里的"同一份"指编译期同源：两个 main 都 include 本头文件。
// =====================================================================
inline void integration_env(double t, double* p_load_kw, double* p_pv_kw) {
    // 300 s 演示窗：负荷 380±120 kW 缓变 + 白天光伏 150±60 kW 快变
    *p_load_kw = 380.0 + 120.0 * std::sin(t / 30.0);
    *p_pv_kw   = (t > 5.0 && t < 250.0) ? 150.0 + 60.0 * std::sin(t / 5.0) : 0.0;
}

// =====================================================================
// 设备侧仿真器：六类数据源 → 共享内存
//
//   数据源            点                                     说明
//   ─────────────────────────────────────────────────────────────
//   关口电表          MEAS.P_LOAD / P_PV / P_GRID            环境 + 派生
//   BMS               MEAS.SOC / T_C / SOH + STA.BMS + CFG.BMS_*  限值
//   PCS               MEAS.P_BAT + STA.PCS / FAULT + CFG.PCS_*    执行
//   光伏逆变器        MEAS.P_PV（品质位）                      kind6 劣化
//   变压器            CFG.TR_KVA（静态容量）                  联调不注入
//   负荷              环境脚本驱动 P_LOAD                     不可控输入
//
// 物理积分复用 MemoryDeviceIO（PCS 惯性 / 变化率 / 效率 / fail-safe 全部继承
// 07/ 的实现；热模型用 07/ 新增的 set_thermal_model() 打开 —— 它的式子与
// PlantModel::update_thermal() 逐字相同，所以联调期的温度曲线与离线仿真一致），
// 跨进程发布只做"点值搬运"。
//
// 电厂一次参数（容量 / 额定 / 变压器 / 需量契约 / 惯性 / 热模型）由
// DeviceSimConfig 显式给出 —— **不能在 EMS 侧改**。原因：这些量在真实系统里
// 是设备/一次侧的属性，EMS 只能通过 CFG.* 读，读不到就按点表默认值保守运行。
// 早期版本把这些参数放在 EmsSideApp::Config 里且从未生效，直接导致联调现场
// 出现"EMS 拿点表默认的 250 kVA 去核 500 kW 负荷 → 变压器过载约束常驻 →
// 状态机在 NORMAL/DERATED 之间抖动"的假故障（详见 11/docs/联调报告.md）。
//
// ★ 需量契约必须选在**可行工况**上：d_target 落在负荷带（260~500 kW）之内时，
//   需量策略的预测均值会被"本拍指令本身"推动，而 merge_realtime_correction()
//   的投入/退出判据没有滞环 → 形成 1 拍极限环（实测：d_target=450 时末段
//   平均跟踪误差 40.7 kW、200 拍里 163 次指令跳变）。这是 07/ 的 L2 合并
//   逻辑的真实缺陷，已记入 11/docs/联调报告.md「遗留问题」，不在本模块修
//   （会动到 05/08/09 的既有基线）。联调基准取 d_target 高于负荷峰值 →
//   策略评估但不动，基准工况干净。
// =====================================================================
struct DeviceSimConfig {
    double battery_capacity_kwh = 1000.0;
    double pcs_max_chg_kw       = 200.0;
    double pcs_max_dis_kw       = 200.0;
    double bms_chg_limit_kw     = 200.0;
    double bms_dis_limit_kw     = 200.0;
    // 一次侧变压器容量（kVA）。1000 kVA 箱变 —— 负荷峰值（500 kW + 站用 2 kW）
    // 与并网点功率都留在额定之内，变压器过载约束不参与联调基准（变压器专项
    // 在 09/ 的场景 S2）。
    double transformer_kva      = 1000.0;
    // 契约需量（kW）。取 600 kW —— **高于负荷峰值**，即"设计工况下需量可控"。
    // 这样需量策略每拍都被评估（覆盖该策略的调用链），但不产生纠偏动作，
    // 基准工况不会被 L2 极限环污染（见文件头说明）。
    double demand_target_kw     = 600.0;
    double tau_s                = 0.5;     // PCS 一阶惯性
    double ramp_kw_per_s        = 400.0;   // 满幅 200 kW 需 0.5 s
    double standby_kw           = 2.0;     // PCS 空载损耗（计入负荷侧）
    double eta_chg              = 0.95;
    double eta_dis              = 0.95;
    double soc_phys_min         = 0.05;
    double soc_phys_max         = 0.95;
    // 热模型（BMS 温度量测的数据源）
    double temp_ambient_c       = 25.0;
    double temp_heat_coef       = 0.00060;
    double temp_cool_coef       = 0.00200;
    // 关口电表的**系统偏差**（kW）。默认 0 = 理想电表。
    //
    // 用途（缺口 A2）：让"电表读数 ≠ 功率平衡"这件事在联调里可被表达。
    // 没有它，设备侧发布的 MEAS.P_GRID 恰好等于 P_load − P_pv − P_bat，
    // 于是"EMS 到底读没读电表"在夹具上不可区分 —— 断言恒真（见 11/T49）。
    double meter_bias_kw        = 0.0;
};

class DeviceSideSim {
public:
    struct Stats {
        int    steps               = 0;
        int    cmd_observed        = 0;   // 观测到指令变化的拍数
        int    failsafe_ticks      = 0;   // fail-safe 零出力拍数
        int    comm_fault_ticks    = 0;   // 处于任一故障窗内的拍数
        int    bms_forbid_ticks    = 0;   // BMS 禁充/禁放窗内的拍数（**非故障**）
        int    quality_bad_ticks   = 0;   // 品质位劣化拍数
        int    cmd_out_of_interval = 0;   // 观测到 EMS 指令越其声明区间
        double max_abs_cmd_kw      = 0.0;
        double last_cmd_kw         = 0.0;
        double last_p_bat_kw       = 0.0;
        double soc_end             = 0.5;
    };

    DeviceSideSim(rt_db_handle_t* h, std::vector<DevFaultWindow> script = {},
                  const DeviceSimConfig& cfg = DeviceSimConfig())
        : w_(h), script_(std::move(script)) {
        apply_plant(cfg);
        publish_all(0.0);   // 连接后第一件事：把全表初值发布进段
    }

    // 一次侧参数落地到进程内物理模型（点表 CFG.* + 热模型开关）。
    // 必须在 publish_all() 之前调用 —— EMS 建连后第一次 read_limits() 就要读到。
    void apply_plant(const DeviceSimConfig& c) {
        auto& d = dev_;
        d.set(mem_point::kCapKwh,    c.battery_capacity_kwh);
        d.set(mem_point::kMaxChg,    c.pcs_max_chg_kw);
        d.set(mem_point::kMaxDis,    c.pcs_max_dis_kw);
        d.set(mem_point::kBmsChgLim, c.bms_chg_limit_kw);
        d.set(mem_point::kBmsDisLim, c.bms_dis_limit_kw);
        d.set(mem_point::kTrKva,     c.transformer_kva);
        d.set(mem_point::kDTarget,   c.demand_target_kw);
        d.set(mem_point::kTauS,      c.tau_s);
        d.set(mem_point::kRampKwS,   c.ramp_kw_per_s);
        d.set(mem_point::kStandby,   c.standby_kw);
        d.set(mem_point::kEtaChg,    c.eta_chg);
        d.set(mem_point::kEtaDis,    c.eta_dis);
        d.set(mem_point::kSocMin,    c.soc_phys_min);
        d.set(mem_point::kSocMax,    c.soc_phys_max);
        d.set_thermal_model(c.temp_ambient_c, c.temp_heat_coef, c.temp_cool_coef);
        d.set_meter_bias_kw(c.meter_bias_kw);
    }

    // 一拍。t 为模拟时刻，dt 为控制周期。
    // 顺序即契约：环境刷新 → 剧本 → 读指令 → 物理执行 → 发布。
    void step(double t, double dt) {
        // ① 环境刷新（电表 / 光伏数据源）
        double load = 0.0, pv = 0.0;
        integration_env(t, &load, &pv);
        dev_.set_environment(load, pv);

        // ② 故障剧本（每拍全量重放，幂等）
        apply_script(t);

        // ③ 读 EMS 指令（CMD 区，设备侧只读不写）
        double cmd = 0.0, lo = 0.0, hi = 0.0;
        long   q   = 0;
        w_.read(EMS_CMD_P_BAT,   &cmd, &q);
        w_.read(EMS_CMD_P_LOWER, &lo,  &q);
        w_.read(EMS_CMD_P_UPPER, &hi,  &q);

        if (std::fabs(cmd - st_.last_cmd_kw) > 1e-9) ++st_.cmd_observed;
        st_.last_cmd_kw    = cmd;
        st_.max_abs_cmd_kw = std::max(st_.max_abs_cmd_kw, std::fabs(cmd));
        // 设备侧对 EMS 的硬契约：指令必须落在 EMS 自己声明的权限区间内。
        // 越界说明 EMS 内部（门控/安全层）出了问题，设备侧如实计数。
        if (cmd < lo - 1e-6 || cmd > hi + 1e-6) ++st_.cmd_out_of_interval;

        // ④ 物理执行（PCS / BMS 进程的物理积分 + fail-safe）
        const double p_bat = dev_.execute(cmd, dt);
        st_.last_p_bat_kw = p_bat;
        if (std::fabs(p_bat) < 1e-9 && std::fabs(cmd) > 1e-9) ++st_.failsafe_ticks;

        // ⑤ 发布全部设备点（MEAS / STA / CFG；品质位按剧本劣化）
        publish_all(t);

        ++st_.steps;
        st_.soc_end = dev_.get(EMS_POINT_NAMES[EMS_SOC]);
    }

    // 把进程内设备模型的全部设备侧点发布进共享内存。
    // 角色边界：CMD.* 三点属于 EMS 的下行区；EXT.* 属于网关（A3.1 起）—— 都不触碰。
    void publish_all(double t) {
        quality_bad_ = false;
        for (const auto& fw : script_) {
            if (fw.kind == 6 && fw.active(t)) quality_bad_ = true;
        }
        for (std::size_t i = 0; i < EMS_POINT_COUNT; ++i) {
            if (i >= static_cast<std::size_t>(EMS_CMD_P_BAT) &&
                i <= static_cast<std::size_t>(EMS_CMD_P_LOWER)) {
                continue;   // 设备侧不写 CMD 区
            }
            if (i >= static_cast<std::size_t>(EMS_EXT_BEGIN)) {
                continue;   // 设备侧不写 EXT 区（A3.2 起：EXT 归网关 RtDbExtWriter）
            }
            const long q = (quality_bad_ && i == static_cast<std::size_t>(EMS_P_PV))
                               ? static_cast<long>(rtdb_quality::kBad)
                               : static_cast<long>(rtdb_quality::kGood);
            w_.write(i, dev_.get(EMS_POINT_NAMES[i]), q);
        }
    }

    void apply_script(double t) {
        bool bms_off = false, meter_off = false, pcs_off = false;
        bool pcs_fault = false, offline = false;
        bool bms_forbid_chg = false, bms_forbid_dis = false;
        for (const auto& fw : script_) {
            if (!fw.active(t)) continue;
            switch (fw.kind) {
                case 1: bms_off   = true; break;
                case 2: meter_off = true; break;
                case 3: pcs_off   = true; break;
                case 4: pcs_fault = true; break;
                case 5: offline   = true; break;
                case 7: bms_forbid_chg = true; break;   // BMS 禁充（正常上报）
                case 8: bms_forbid_dis = true; break;   // BMS 禁放（正常上报）
                default: break;   // kind 6 在 publish_all 里处理（品质位）
            }
        }
        dev_.set_comm_bms(!bms_off);
        dev_.set_comm_meter(!meter_off);
        dev_.set_comm_pcs(!pcs_off);
        dev_.set_pcs_fault(pcs_fault);
        dev_.set_device_offline(offline);
        // BMS 禁充放：每拍**全量重放**（与其它剧本字段一致）→ 窗口一结束
        // 自动回到"允许"。这一点很要紧：它让"限值可动态变化、且可恢复"
        // 成为被测行为，而不是一次性开关。
        dev_.set_bms_forbid(bms_forbid_chg, bms_forbid_dis);

        if (bms_off || meter_off || pcs_off || pcs_fault || offline)
            ++st_.comm_fault_ticks;
        // 禁充放**不计** comm_fault_ticks —— 它不是故障（见 DevFaultWindow 注释）。
        if (bms_forbid_chg || bms_forbid_dis) ++st_.bms_forbid_ticks;
        if (quality_bad_) ++st_.quality_bad_ticks;
    }

    const Stats& stats() const { return st_; }
    MemoryDeviceIO& dev() { return dev_; }

    // 观测到的 EMS 指令（联调报告用）
    double observed_cmd_kw() const { return st_.last_cmd_kw; }

    std::string report_text() const {
        std::ostringstream os;
        os << std::fixed << std::setprecision(2);
        os << "设备侧联调报告（六类数据源 → 共享内存）\n";
        os << "  拍数            " << st_.steps << "\n";
        os << "  指令变化拍数    " << st_.cmd_observed << "\n";
        os << "  指令峰值        " << st_.max_abs_cmd_kw << " kW\n";
        os << "  指令越 EMS 区间 " << st_.cmd_out_of_interval << " 拍（硬契约，必须 0）\n";
        os << "  fail-safe 拍数  " << st_.failsafe_ticks << "\n";
        os << "  故障窗内拍数    " << st_.comm_fault_ticks << "\n";
        os << "  BMS禁充放拍数   " << st_.bms_forbid_ticks
           << "（正常上报，不计故障）\n";
        os << "  品质位劣化拍数  " << st_.quality_bad_ticks << "\n";
        os << "  末拍实际功率    " << st_.last_p_bat_kw << " kW\n";
        os << "  末拍 SOC        " << st_.soc_end << "\n";
        os << "  角色边界        设备侧只写 MEAS/STA/CFG（CMD/EXT 不碰）✔\n";
        return os.str();
    }

private:
    MemoryDeviceIO   dev_;     // 进程内物理模型（PCS/BMS/电表仿真）
    RtDbPointWriter  w_;       // 设备侧 → 共享内存
    std::vector<DevFaultWindow> script_;
    bool             quality_bad_ = false;
    Stats            st_;
};

// =====================================================================
// EMS 侧应用：RtDbDeviceIO + EmsRuntime + P2 观察者
//
// 注意：**不设置 device_pump**（多进程形态天然没有泵）。
// 单进程测试形态由测试代码注入泵 —— EmsSideApp 本身对"设备在哪个进程"
// 完全无知，这正是 P0 抽象在联调期的最终形态。
// =====================================================================
class EmsSideApp {
public:
    struct Config {
        double dt_s       = 0.1;
        int    log_every  = 1;
        // 联调基准工况：并网运行、无并网点功率硬约束（防逆流/需量窗口这些
        // 场景约束由 09/ 的专项场景负责，联调期不叠加，避免把"场景约束动作"
        // 误当成"链路异常"）。一次侧参数（变压器/额定/需量/惯性）**不在这里**，
        // 见 DeviceSimConfig。
        double grid_p_min_kw  = -1e9;
        double grid_p_max_kw  = 1e9;
        double ramp_kw_per_s  = 1e9;
        bool   use_forecast   = true;
    };

    EmsSideApp(rt_db_handle_t* h) : io_(h) { setup(); }
    EmsSideApp(rt_db_handle_t* h, const Config& c) : io_(h), cfg_(c) { setup(); }

    RtDbDeviceIO& io() { return io_; }
    EmsRuntime& rt() { return rt_; }
    RuntimeObserver& obs() { return obs_; }

    // -----------------------------------------------------------------
    // 装配层：等设备侧就绪（多进程启动顺序约束）
    //
    // 为什么必须等：EMS 在 attach_device() 时经 read_limits() 把一次侧参数
    // （变压器容量 / PCS 额定 / 需量契约 / 惯性）读进 dev_，之后只在
    // attach_device() / configure_plant() 时刷新。设备进程若尚未发布 CFG.*，
    // EMS 读到的是**点表默认值**（变压器 250 kVA）—— 现场表现就是"EMS 拿错误
    // 的一次侧参数去算安全边界"，会凭空产生变压器过载约束。
    //
    // 判据用 MEAS.P_LOAD：点表默认 0.0，设备侧一发布就是工况值（≥ 260 kW）。
    // 就绪后重新 attach_device()，把 CFG.* 与协调层参数一起刷新。
    // -----------------------------------------------------------------
    bool wait_for_device(double timeout_s = 10.0) {
        const auto t0 = std::chrono::steady_clock::now();
        while (true) {
            io_.read_snapshot(0.0, dummy_);
            if (io_.cached_value(EMS_P_LOAD) > 0.5) {
                rt_.attach_device(&io_);
                return true;
            }
            if (std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - t0).count() > timeout_s) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // 状态迁移链（联调报告的"状态同步"证据）
    std::string history_text() {
        std::ostringstream os;
        os << std::fixed << std::setprecision(1);
        const auto& h = rt_.fsm().history();
        if (h.empty()) return "  （无迁移）\n";
        for (const auto& e : h) {
            os << "  t=" << std::setw(8) << e.ts << " s  " << state_name(e.from)
               << " → " << state_name(e.to) << "   (" << e.reason << ")\n";
        }
        return os.str();
    }

    // SOE 明细（联调报告的"日志记录"证据）。一事件一行，便于 grep / wc。
    std::string soe_text(std::size_t max_n = 40) {
        std::ostringstream os;
        os << std::fixed << std::setprecision(1);
        const auto& evs = obs_.soe().events();
        const std::size_t n = std::min(max_n, evs.size());
        for (std::size_t i = 0; i < n; ++i) {
            const auto& e = evs[i];
            os << "  t=" << std::setw(8) << e.t << " s  [" << soe_level_name(e.level)
               << "] " << soe_code_name(e.code) << "  " << e.message;
            if (e.repeat_count > 1) os << "  ×" << e.repeat_count;
            os << "\n";
        }
        if (evs.size() > n) os << "  ...（共 " << evs.size() << " 条）\n";
        return os.str();
    }

    // 单拍（多进程形态的主循环体；单进程测试同用此入口）
    StepRecord step(double dt) {
        StepRecord rec = rt_.step(dt);
        obs_.on_step(rt_, rec);
        return rec;
    }

    void run(int n_steps, double dt) {
        for (int i = 0; i < n_steps; ++i) step(dt);
    }

    // 联调报告：闭环指标 + SOE 事件 + 状态迁移链
    std::string report_text() {
        std::ostringstream os;
        os << std::fixed << std::setprecision(2);
        const auto& tot = obs_.totals();
        const auto& m   = rt_.metrics();
        os << "EMS 侧联调报告（RtDbDeviceIO → EmsRuntime 全栈）\n";
        os << "  拍数            " << tot.steps << "\n";
        os << "  指令峰值        " << tot.max_abs_cmd_kw << " kW\n";
        os << "  指令总行程      " << tot.cmd_travel_kw << " kW\n";
        os << "  指令逃逸拍数    " << tot.out_of_interval_ticks << "（硬不变量，必须 0）\n";
        os << "  SOC 区间        [" << tot.soc_min << ", " << tot.soc_max << "]\n";
        os << "  关口功率区间    [" << tot.min_grid_kw << ", " << tot.max_grid_kw << "] kW\n";
        os << "  充电量/放电量   " << tot.e_chg_kwh << " / " << tot.e_dis_kwh << " kWh\n";
        os << "  单拍均值/峰值   " << m.mean_cycle_us << " / " << m.max_cycle_us << " us\n";
        os << "  状态迁移        " << rt_.fsm().history().size() << " 次\n";
        os << "  SOE 事件        " << obs_.soe().size() << " 条 / dropped "
           << obs_.soe().dropped() << " / suppressed " << obs_.soe().suppressed() << "\n";
        os << "  观察者步数      " << tot.steps << "（与 log_every 无关）\n";
        os << "  读点降级/碰撞  " << io_.stale_reads() << " / " << io_.collisions()
           << "（碰撞被重试吸收；降级=保留上次有效值，非故障）\n";
        os << "\n状态迁移链：\n" << history_text();
        os << "\nSOE 明细：\n" << soe_text();
        return os.str();
    }

private:
    void setup() {
        rt_.attach_device(&io_);   // 读限值：来自段里的 CFG.*（设备侧已发布）

        rt_.config().dt_s      = cfg_.dt_s;
        rt_.config().log_every = cfg_.log_every;
        rt_.config().enable_log = true;
        rt_.config().enable_realtime_correction = true;
        rt_.config().l2_correction_max_kw = 100.0;

        rt_.safety_params().grid_p_min_kw = cfg_.grid_p_min_kw;
        rt_.safety_params().grid_p_max_kw = cfg_.grid_p_max_kw;
        rt_.safety_params().ramp_kw_per_s = cfg_.ramp_kw_per_s;

        if (cfg_.use_forecast) {
            ForecastSeries fc;
            fc.step_s = 12.5;
            for (int i = 0; i < 96; ++i) {
                const double h = i * 0.25;
                fc.price.push_back((h < 7.0) ? 0.30 : (h < 9.0 ? 0.60 : (h < 12.0 ? 1.00 :
                              (h < 14.0 ? 0.60 : (h < 17.0 ? 1.00 : (h < 21.0 ? 1.20 :
                              (h < 23.0 ? 0.60 : 0.30)))))));
                fc.p_load_kw.push_back(380.0 + 120.0 * std::sin(h / 3.0));
                fc.p_pv_kw.push_back((h >= 6.0 && h <= 18.0)
                                     ? 150.0 * std::sin(3.14159265358979 * (h - 6.0) / 12.0)
                                     : 0.0);
            }
            fc.loaded = true;
            rt_.set_forecast(fc);
        }

        rt_.apply_configs();
        rt_.fsm().request_run(true);   // 上层运行许可（联调 = 人工已确认启动）
        obs_.start(0.0);
    }

    RtDbDeviceIO    io_;
    EmsRuntime      rt_;
    RuntimeObserver obs_;
    Config          cfg_;
    RealtimeSnapshot dummy_;   // wait_for_device() 的探针落点
};

// =====================================================================
// 联调检查单：周期 11 的七项任务逐项落检查项（测试与演示共用口径）
// =====================================================================
struct IntegrationCheck {
    std::string phase;     // 通信建立 / 数据采集 / 控制指令 / 状态同步 /
                           // 故障触发 / 异常恢复 / 日志记录
    std::string name;
    bool        pass = false;
    std::string evidence;
};

inline std::string checks_text(const std::vector<IntegrationCheck>& checks) {
    std::ostringstream os;
    int pass = 0;
    for (const auto& c : checks) {
        if (c.pass) ++pass;
        os << "  [" << (c.pass ? "PASS" : "FAIL") << "] " << c.phase
           << " · " << c.name << "  —  " << c.evidence << "\n";
    }
    os << "  合计 " << pass << " / " << checks.size() << " 项通过\n";
    return os.str();
}

} // namespace ems
