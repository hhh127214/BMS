// =====================================================================
// 07/rtdb — 现场采集 → 实时库 的镜像写入器（RtdbMirror）
//
// 为什么需要它（2026-10-19 实测出来的缺口）：
//
//   `--device modbus` 走 ModbusDeviceIO，每拍的结果只写进实录 CSV；
//   RT_DB 里的 `MEAS.*` 没有任何进程写，一直是 EMS_POINT_DEFAULTS 的初值。
//   上端 IEC104 网关读的正是 `MEAS.*` —— 于是主站收到的是
//   「通道正常、24 点齐全、值全是 0 / SOC 50% / 25℃」，而且品质 GOOD。
//
//   这不是点表填错，是数据源根本没接上。本类补的就是这一段。
//   实测依据：同一时刻 CSV 是 P_load=380 / SOC=0.545，
//   而 104 上送是 0.000 / 50.000，且 gateway.log 报「坏读 0」——
//   读到了、读到的是初值。
//
// 写哪一区（纪律见 ems_point_table.h 顶部）：
//
//     RtDbPointWriter（设备侧）→ MEAS / STA / CFG   ← 本类
//     RtDbDeviceIO    （EMS 侧）→ CMD
//     RtDbExtWriter   （网关）  → EXT
//
//   ★ 一个进程同时兼任采集与控制时（07 --device modbus --control），
//     它算出的 CMD 也是"EMS 自己的决定"，可以发布；
//     但**只读巡检不许写 CMD**。
//     `CMD.P_BAT = 0` 不是"没有指令"，是"要求电池 0 kW"——调度会当停机命令。
//     所以本类的 publish() 用 `const Command* cmd`：只读路径传 nullptr，
//     **结构上没有**可写的东西，不是靠一个 if 记住别写。
//
// ★ 打不开段不阻断采集，但必须喊出来。
//   现场"先起设备采集、后起实时库初始化器"是常有的顺序。此时静默降级
//   等于把"上端发空数"藏起来 —— 所以 open() 失败要把原因交给调用方打印，
//   运行期每 600 拍再提醒一次写入失败数。
//
// ★ CFG 只写"设备侧真能给"的那几个点。
//   容量 / τ / ramp / 待机自耗 / 两个效率 / SOC 物理上下限，Modbus 点表里
//   没有来源 —— **不写**，保留初始化器写进去的默认值（与 PlantConfig 默认
//   对齐）。凭空造一个值比留初值更糟：初值至少是"没人给过"，造出来的值
//   看起来像"设备说过"。
// =====================================================================

#pragma once

#include "ems_point_table.h"  // 点表唯一真相源（索引常量 + 点区归属）
#include "rtdb_device_io.h"   // ems::RtDbDeviceIO / ems::RtDbPointWriter
#include "data_models.h"      // RealtimeSnapshot / DeviceLimits / DeviceStatus

#include <cstdio>
#include <string>

namespace ems {

class RtdbMirror {
public:
    // 本拍要发布的量测（与 RealtimeSnapshot 同口径）
    struct Sample {
        double p_load = 0.0;   // kW 负荷
        double p_pv   = 0.0;   // kW 光伏
        double p_bat  = 0.0;   // kW 电池实际功率（放电为正）
        double p_grid = 0.0;   // kW 关口（进口为正）
        double soc    = 0.0;   // 0..1
        double t_c    = 0.0;   // ℃
        double soh    = 1.0;   // 0..1
    };

    // 本拍下发过的指令（只读巡检不构造这个结构）
    struct Command {
        double p_bat   = 0.0;
        double p_upper = 0.0;
        double p_lower = 0.0;
    };

    struct Stats {
        long steps  = 0;   // 提交过多少拍
        long wrote  = 0;   // 成功写入的点次
        long failed = 0;   // 失败的点次
    };

    RtdbMirror() = default;
    ~RtdbMirror() { io_.close_owned(); }

    RtdbMirror(const RtdbMirror&) = delete;
    RtdbMirror& operator=(const RtdbMirror&) = delete;

    // =================================================================
    // 打开：RT_DB 由初始化器建段，本类只 attach
    // =================================================================
    bool open(std::string* err = nullptr) {
        if (io_.is_open()) return true;

        if (!io_.open_owned(nullptr)) {
            if (err) *err = "共享内存段打不开（实时库初始化器没起，或不在同一登录会话）";
            return false;
        }

        // 点表对不上就一个点都不写。
        // 为什么宁可不写：索引整体错位时，写入会把 A 的值写进 B —— 主站
        // 看到的是"点名对得上、数全是隔壁量的"，比空值更难查。
        const int bad = io_.self_check();
        if (bad != 0) {
            if (err) {
                *err = "实时库点表与编译期点表不一致（" + std::to_string(bad) +
                       " 点）—— 索引可能整体错位，拒绝镜像写入";
            }
            io_.close_owned();
            return false;
        }

        // 写入者边界自检：本类只碰 MEAS / STA / CFG。
        // ★ 必须逐个点查，不能扫 0..EMS_STA_BMS_DIS_FORBID 这段连续区间 ——
        //   CMD 三点（7..9）正好夹在 MEAS 与 CFG 中间，扫区间会把它们当成
        //   "越界"（第一版就是这么误报的）。这里查的是"我要写的这些点"。
        const int owned[] = {
            EMS_P_LOAD, EMS_P_PV, EMS_P_BAT, EMS_P_GRID, EMS_SOC, EMS_T_C, EMS_SOH,
            EMS_STA_BMS, EMS_STA_PCS, EMS_STA_METER, EMS_STA_FAULT, EMS_STA_OFFLINE,
            EMS_STA_VALID, EMS_STA_BMS_CHG_FORBID, EMS_STA_BMS_DIS_FORBID,
            EMS_CFG_MAX_CHG, EMS_CFG_MAX_DIS, EMS_CFG_BMS_CHG_LIM,
            EMS_CFG_BMS_DIS_LIM, EMS_CFG_TR_KVA, EMS_CFG_D_TARGET,
        };
        for (int i : owned) {
            const ems_point_zone_t z = ems_point_zone_of(i);
            if (z != EMS_ZONE_MEAS && z != EMS_ZONE_CFG && z != EMS_ZONE_STA) {
                if (err) *err = "点区分界异常（索引 " + std::to_string(i) +
                                " 落在 " + ems_zone_name(z) + " 区）";
                io_.close_owned();
                return false;
            }
        }

        w_.attach(io_.handle());
        return true;
    }

    bool is_open() const { return w_.ok(); }
    const Stats& stats() const { return st_; }

    // SOH 是慢变量（日变化 < 0.1%），闭环路径每拍拿不到它 ——
    // 启动时由调用方从一次 read_snapshot 取回，之后原样发布。
    double soh() const { return soh_; }
    void   set_soh(double v) { soh_ = v; }

    // =================================================================
    // 每拍发布
    // =================================================================
    // ★ 调用方必须保证「这一拍真的读到了」（read_snapshot 返回 true）。
    //   读失败还写一行 0，就是 07/ 一直在防的假数据。
    // ★ cmd 传 nullptr = 只读巡检：不写 CMD。
    void publish(const Sample& s, const Command* cmd, const DeviceStatus& st,
                 const DeviceLimits& lim) {
        if (!is_open()) return;

        // 品质：这一拍可信 → GOOD；读到的是缓存且已失效 → UNCERTAIN（NT）。
        // 用 NT 而不是 IV：值仍然是真的，只是"别据以决策"。
        const long q = st.data_valid ? rtdb_quality::kGood : rtdb_quality::kUncertain;

        put(EMS_P_LOAD, s.p_load, q);
        put(EMS_P_PV,   s.p_pv, q);
        put(EMS_P_BAT,  s.p_bat, q);
        put(EMS_P_GRID, s.p_grid, q);
        put(EMS_SOC,    s.soc, q);
        put(EMS_T_C,    s.t_c, q);
        put(EMS_SOH,    s.soh, q);

        if (cmd != nullptr) {
            put(EMS_CMD_P_BAT,   cmd->p_bat);
            put(EMS_CMD_P_UPPER, cmd->p_upper);
            put(EMS_CMD_P_LOWER, cmd->p_lower);
        }

        publish_sta(st, lim);
        publish_cfg(lim);
        end_step();
    }

    // 失败提示：每 600 拍（10 Hz 下约 1 分钟）说一次，别刷屏
    void end_step() {
        ++st_.steps;
        if (st_.failed > 0 && (st_.steps == 1 || st_.steps % 600 == 0)) {
            std::printf("  [WARN] 实时库镜像第 %ld 拍：本段累计写入失败 %ld 次"
                        " —— 上端 104 会有旧值或空值\n",
                        st_.steps, st_.failed);
        }
    }

private:
    // =================================================================
    // 状态位（STA）—— 设备侧的真值，品质恒 GOOD：
    //   "通信断了"本身就是真实值，不能因为通信断了把它标成不可信。
    // =================================================================
    void publish_sta(const DeviceStatus& st, const DeviceLimits& lim) {
        put(EMS_STA_BMS,   st.bms_comm_ok ? 1.0 : 0.0);
        put(EMS_STA_PCS,   st.pcs_comm_ok ? 1.0 : 0.0);
        put(EMS_STA_METER, st.meter_comm_ok ? 1.0 : 0.0);
        put(EMS_STA_FAULT, st.pcs_fault ? 1.0 : 0.0);
        put(EMS_STA_OFFLINE, st.device_offline ? 1.0 : 0.0);
        put(EMS_STA_VALID,   st.data_valid ? 1.0 : 0.0);

        // BMS 安全位：每拍显式写（点表注释里的现场硬要求）。
        // "不支持就不写"在这里不成立 —— 不写与写 0 不可区分，而写 0 是"允许"。
        put(EMS_STA_BMS_CHG_FORBID, lim.bms_chg_forbidden ? 1.0 : 0.0);
        put(EMS_STA_BMS_DIS_FORBID, lim.bms_dis_forbidden ? 1.0 : 0.0);
    }

    // =================================================================
    // 配置（CFG）—— 只写设备侧能给的那 6 点，其余保留初始化器初值
    // =================================================================
    void publish_cfg(const DeviceLimits& lim) {
        put(EMS_CFG_MAX_CHG,     lim.pcs_rated_chg_kw);
        put(EMS_CFG_MAX_DIS,     lim.pcs_rated_dis_kw);
        put(EMS_CFG_BMS_CHG_LIM, lim.bms_chg_limit_kw);
        put(EMS_CFG_BMS_DIS_LIM, lim.bms_dis_limit_kw);
        put(EMS_CFG_TR_KVA,      lim.transformer_capacity_kw);
        put(EMS_CFG_D_TARGET,    lim.d_target_kw);
        // 不写：CAP_KWH / TAU_S / RAMP / STANDBY / ETA_CHG / ETA_DIS / SOC_MIN / SOC_MAX
        // —— Modbus 点表里没有这些量，凭空写就是发明数据。
    }

    void put(std::size_t index, double value,
             long quality = rtdb_quality::kGood) {
        if (w_.write(index, value, quality)) {
            ++st_.wrote;
        } else {
            ++st_.failed;
        }
    }

    RtDbDeviceIO    io_;   // 持有连接（一个进程一条连接）
    RtDbPointWriter w_;    // 只写 MEAS / STA / CFG
    Stats           st_;
    double          soh_ = 1.0;
};

}  // namespace ems
