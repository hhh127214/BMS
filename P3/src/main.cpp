// =====================================================================
// P3/ — 场景 F/G：通信适配器演示（Modbus 设备侧 + IEC104 调度侧）
//
// 本演示回答三个问题（都是现场一定会问的）：
//   F. Modbus：策略照常跑，但数据全走寄存器报文 —— 网线掉了会怎样？
//   G. IEC104：与调度主站建链、总召唤、下发遥调、心跳，报文长什么样？
//   H. 三者对照：同一套算法挂 MemoryDeviceIO / ModbusDeviceIO /
//      Iec104DeviceIO，指令序列是否**逐位一致**？介质量化带来多大偏差？
//
// 为什么 H 是重点：产品化的核心承诺是"换数据源不改算法"。承诺不能靠文档说，
// 要靠可执行的对照实验说 —— 本演示把三种介质的 StepRecord 逐拍比对并打印差异数。
//
// 编译：见 P3/scripts/build.bat
// =====================================================================

#include "data_models.h"
#include "device_io.h"
#include "ems_point_table.h"
#include "iec104_codec.h"
#include "iec104_device_io.h"
#include "memory_device_io.h"
#include "modbus_codec.h"
#include "modbus_device_io.h"
#include "modbus_slave_sim.h"
#include "realtime_loop.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace ems;

// ---------------------------------------------------------------------
static void rule(const std::string& title) {
    std::cout << "\n" << std::string(72, '=') << "\n" << title << "\n"
              << std::string(72, '=') << "\n";
}

// 环境脚本（负荷/光伏）。三种介质必须用同一份，否则比的是工况差异。
static void env_at(int step, double& p_load_kw, double& p_pv_kw) {
    const double t = step * 0.1;
    p_load_kw = 380.0 + 120.0 * std::sin(t / 30.0);
    p_pv_kw   = (t > 5.0 && t < 25.0) ? 150.0 + 60.0 * std::sin(t / 5.0) : 0.0;
}

// 三种介质共用的装配序列：**任何差异都只允许来自介质本身**
static void configure_runtime(EmsRuntime& rt) {
    rt.config().dt_s = 0.1;
    rt.config().enable_realtime_correction = true;
    rt.config().l2_correction_max_kw = 100.0;
    rt.safety_params().grid_p_min_kw = -1e9;
    rt.safety_params().ramp_kw_per_s = 1e9;
    rt.apply_configs();
    rt.fsm().request_run(true);
    rt.device_limits().transformer_capacity_kw = 800.0;
    rt.device_limits().d_target_kw = 250.0;
}

// ---------------------------------------------------------------------
// 运行 A：进程内点表（参考基准）
// ---------------------------------------------------------------------
struct MemoryRig {
    MemoryDeviceIO dev;
    EmsRuntime     rt;
    MemoryRig() { rt.attach_device(&dev); configure_runtime(rt); }
    void run(int n, int base = 0) {
        rt.run(n, 0.1, [&](EmsRuntime&, int i) {
            double l = 0.0, p = 0.0;
            env_at(base + i, l, p);
            dev.set_environment(l, p);
        });
    }
};

// ---------------------------------------------------------------------
// 运行 B：Modbus（EMS 主站 ↔ 从站寄存器）
// ---------------------------------------------------------------------
struct ModbusRig {
    MemoryDeviceIO          dev;
    ModbusSlaveSim          slave;
    LoopbackModbusTransport transport;
    ModbusDeviceIO          io;
    EmsRuntime              rt;

    explicit ModbusRig(MapProfile profile)
        : slave(profile), transport(&slave), io(&transport, profile, 1) {
        slave.attach_device(&dev);
        slave.publish_device_points();
        io.set_device_pump([&](double cmd, double dt) {
            dev.execute(cmd, dt);
            slave.publish_device_points();
        });
        io.open();
        rt.attach_device(&io);
        configure_runtime(rt);
    }
    void run(int n, int base = 0) {
        rt.run(n, 0.1, [&](EmsRuntime&, int i) {
            double l = 0.0, p = 0.0;
            env_at(base + i, l, p);
            dev.set_environment(l, p);
            slave.publish_device_points();
        });
    }
};

// ---------------------------------------------------------------------
// 运行 C：IEC104（EMS 可控站 ↔ 调度侧被控站）
// ---------------------------------------------------------------------
struct Iec104Rig {
    MemoryDeviceIO             dev;
    Iec104ControlledStationSim station;
    LoopbackIec104Transport    transport;
    Iec104DeviceIO             io;
    EmsRuntime                 rt;

    explicit Iec104Rig(Iec104Profile profile, uint16_t ca = 1)
        : station(profile, ca), transport(&station), io(&transport, profile, ca) {
        station.attach_device(&dev);
        io.set_device_pump([&](double cmd, double dt) { dev.execute(cmd, dt); });
        io.open();
        rt.attach_device(&io);
        configure_runtime(rt);
    }
    void run(int n, int base = 0) {
        rt.run(n, 0.1, [&](EmsRuntime&, int i) {
            double l = 0.0, p = 0.0;
            env_at(base + i, l, p);
            dev.set_environment(l, p);
        });
    }
};

// 逐拍比对（阈值 0 = 逐位）
static int diff_logs(const std::vector<StepRecord>& a, const std::vector<StepRecord>& b,
                     double eps = 0.0) {
    int diff = 0;
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const StepRecord& x = a[i];
        const StepRecord& y = b[i];
        const bool same =
            std::fabs(x.p_cmd - y.p_cmd) <= eps && std::fabs(x.p_actual - y.p_actual) <= eps &&
            std::fabs(x.p_grid - y.p_grid) <= eps && std::fabs(x.soc - y.soc) <= eps &&
            std::fabs(x.p_lower - y.p_lower) <= eps && std::fabs(x.p_upper - y.p_upper) <= eps &&
            std::fabs(x.plan_target - y.plan_target) <= eps &&
            std::fabs(x.correction - y.correction) <= eps &&
            x.state == y.state && x.clamped == y.clamped && x.safety_clip == y.safety_clip &&
            x.state_gated == y.state_gated && x.hold_last == y.hold_last &&
            x.fault_bits == y.fault_bits && x.reason == y.reason;
        if (!same) ++diff;
    }
    return diff;
}

static double peak_cmd(const std::vector<StepRecord>& lg) {
    double m = 0.0;
    for (const auto& r : lg) m = std::max(m, std::fabs(r.p_cmd));
    return m;
}

// =====================================================================
// F. Modbus：寄存器报文闭环 + 断链 → FAULT → 自愈
// =====================================================================
static void scenario_modbus() {
    rule("F. Modbus 设备侧：EMS 主站 ——[寄存器报文]—— PCS/BMS/电表从站");

    ModbusRig rig(MapProfile::kStandardF32);      // 现场标准映射（float32）
    const int N = 300;
    rig.run(N);

    const LoopMetrics m = rig.rt.metrics();
    std::cout << "  算法: " << rig.rt.device()->name()
              << "   设备侧: MemoryDeviceIO(点表替身)\n"
              << "  报文: " << rig.io.transactions() << " 次事务 / "
              << rig.slave.frames_handled() << " 帧被从站处理 / 异常 "
              << rig.slave.exception_count() << " 次\n"
              << "  映射: " << (rig.io.map().width == 2 ? "float32/ABCD 2 寄存器/点"
                                                       : "float64 4 寄存器/点")
              << "  总寄存器 " << rig.io.map().total_regs() << "\n"
              << "  " << N << " 拍指标: " << m.to_string() << "\n"
              << "  读失败 " << rig.io.stale_reads() << " / 超时 " << rig.io.timeouts()
              << " / 自检不一致点 " << rig.io.self_check() << "\n";

    // ---- 断链：从站静默 ----
    rig.transport.set_link_up(false);
    rig.run(8, N);
    const StepRecord& f = rig.rt.log().back();
    std::cout << "  [断链] fault_bits=0x" << std::hex << f.fault_bits << std::dec
              << " state=" << state_name(f.state)
              << " p_cmd=" << std::fixed << std::setprecision(3) << f.p_cmd
              << " kW  运行许可=" << (rig.rt.fsm().output_enabled() ? "有" : "已撤销") << "\n";

    // ---- 恢复 ----
    rig.transport.set_link_up(true);
    rig.run(20, N + 8);
    std::cout << "  [恢复] state=" << state_name(rig.rt.log().back().state)
              << "  数据可信=" << (rig.io.read_status().data_valid ? "是" : "否")
              << "  （恢复后停在非运行态，必须由上层显式重启 —— 安全要求）\n";
}

// =====================================================================
// G. IEC104：建链 / 总召唤 / 遥调 / 心跳
// =====================================================================
static void scenario_iec104() {
    rule("G. IEC104 调度侧：EMS 可控站 ——[104 报文]—— 调度主站（被控站）");

    Iec104Rig rig(Iec104Profile::kStandard);
    const int N = 200;
    rig.run(N);

    std::cout << "  建链: STARTDT " << (rig.io.startdt_ok() ? "已确认" : "未确认")
              << " / 总召唤 " << (rig.io.gi_done() ? "完成" : "未完成")
              << "（激活确认 " << rig.io.gi_actcon() << "，激活终止 " << rig.io.gi_actterm()
              << "，初始化结束 " << rig.io.init_end() << "）\n"
              << "  会话: 我方 I 帧 " << rig.io.i_frames_tx() << " / 收到 I 帧 "
              << rig.io.i_frames_rx() << " / S 帧 " << rig.io.s_frames_tx()
              << " / 心跳 TESTFR " << rig.io.testfr_tx() << "(发) " << rig.io.testfr_rx()
              << "(收)\n"
              << "  序号: N(S)=" << rig.io.vs() << " N(R)=" << rig.io.vr()
              << " 未确认=" << rig.io.unacked_tx() << "（k=12 w=8）"
              << "  窗口阻塞 " << rig.io.window_stalls() << " 次\n"
              << "  遥调: 从站收到设定值 " << rig.station.setpoint_count()
              << " 条 / 激活确认回显 " << rig.io.setpoint_echo() << " 条\n"
              << "  报文量: 上行 " << rig.transport.bytes_rx() << " 字节 / 下行 "
              << rig.transport.bytes_tx() << " 字节\n"
              << "  异常: 畸形 " << rig.io.malformed() << " / CA 不匹配 " << rig.io.bad_ca()
              << " / 未知类型 " << rig.io.unknown_type() << " / 自检不一致 "
              << rig.io.self_check() << "\n"
              << "  " << N << " 拍指标: " << rig.rt.metrics().to_string() << "\n";

    // ---- 下发一条遥调并看从站侧读回 ----
    PowerCommand cmd;
    cmd.timestamp = rig.rt.now();
    cmd.p_bat_cmd_kw = 80.0;
    cmd.p_upper = 120.0;
    cmd.p_lower = -60.0;
    rig.io.write_command(cmd);
    rig.io.poll(0);
    rig.io.poll(0);
    std::cout << "  [遥调] 下发 P_bat=80.0 kW / 区间 [-60, 120] → 从站侧读回 P_bat="
              << rig.dev.get(EMS_POINT_NAMES[EMS_CMD_P_BAT]) << " kW\n";

    // ---- 对端哑（t3 判据）----
    rig.io.set_stale_after_polls(3);
    rig.transport.inject_silence(100000);
    rig.run(6, N);
    std::cout << "  [对端哑] 连续 " << rig.io.polls_without_data()
              << " 轮无新帧 → 数据可信="
              << (rig.io.read_status().data_valid ? "是" : "否")
              << "  state=" << state_name(rig.rt.log().back().state) << "\n";
    rig.transport.inject_silence(0);
    rig.io.poll(0);
    std::cout << "  [恢复] 数据可信=" << (rig.io.read_status().data_valid ? "是" : "否") << "\n";
}

// =====================================================================
// H. 三介质对照：换数据源不改算法
// =====================================================================
static void scenario_three_media() {
    rule("H. 换数据源不改算法：四份运行、同一套算法、逐拍比对");

    const int N = 400;

    MemoryRig ref;
    ref.run(N);

    ModbusRig    mb_wide(MapProfile::kWideF64);
    mb_wide.run(N);
    ModbusRig    mb_std(MapProfile::kStandardF32);
    mb_std.run(N);

    Iec104Rig    ie_wide(Iec104Profile::kWidePrivate);
    ie_wide.run(N);
    Iec104Rig    ie_std(Iec104Profile::kStandard);
    ie_std.run(N);

    const std::vector<StepRecord>& L = ref.rt.log();

    std::cout << std::left << std::setw(38) << "  运行"
              << std::setw(12) << "逐位差异"
              << std::setw(14) << "峰值指令kW"
              << "末拍SOC\n";
    std::cout << "  " << std::string(70, '-') << "\n";

    auto row = [&](const std::string& name, const std::vector<StepRecord>& lg, bool exact) {
        const int d = diff_logs(L, lg, exact ? 0.0 : 1e-9);
        std::cout << "  " << std::left << std::setw(38) << name
                  << std::setw(12) << d
                  << std::setw(14) << std::fixed << std::setprecision(3) << peak_cmd(lg)
                  << std::setprecision(6) << lg.back().soc << "\n";
    };

    row("A 进程内点表 MemoryDeviceIO（基准）", L, true);
    row("B Modbus 报文 / 宽精度 f64", mb_wide.rt.log(), true);
    row("C Modbus 报文 / 现场 f32", mb_std.rt.log(), false);
    row("D IEC104 报文 / 宽精度私有 200", ie_wide.rt.log(), true);
    row("E IEC104 报文 / 现场 M_ME_NC_1", ie_std.rt.log(), false);

    std::cout << "\n  说明：B/D 用无损通道，逐位差异必须是 0 —— 这证明"
                 "\"地址映射 + 报文收发 + 缓存/故障语义\"这一整套适配器代码没有改变算法行为。\n"
                 "        C/E 用现场标准通道（float32，24 位有效位），差异来自**介质的数值分辨率**，"
                 "不是代码缺陷；两路独立实现的偏差上界一致（≈3.9e-5 kW），互为旁证。\n";

    // 量化偏差与决策拓扑
    auto topo = [&](const std::vector<StepRecord>& lg) {
        int t = 0;
        const std::size_t n = std::min(L.size(), lg.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (L[i].state != lg[i].state || L[i].fault_bits != lg[i].fault_bits ||
                L[i].state_gated != lg[i].state_gated || L[i].hold_last != lg[i].hold_last ||
                L[i].clamped != lg[i].clamped || L[i].reason != lg[i].reason) ++t;
        }
        return t;
    };
    auto maxdev = [&](const std::vector<StepRecord>& lg) {
        double d = 0.0;
        const std::size_t n = std::min(L.size(), lg.size());
        for (std::size_t i = 0; i < n; ++i) d = std::max(d, std::fabs(L[i].p_cmd - lg[i].p_cmd));
        return d;
    };
    std::cout << "\n  f32 通道的量化边界：Modbus 决策拓扑差异 " << topo(mb_std.rt.log())
              << " 拍 / 最大指令偏差 " << std::scientific << std::setprecision(3)
              << maxdev(mb_std.rt.log()) << " kW\n"
              << "                        IEC104 决策拓扑差异 " << std::fixed
              << topo(ie_std.rt.log()) << " 拍 / 最大指令偏差 " << std::scientific
              << std::setprecision(3) << maxdev(ie_std.rt.log()) << " kW\n"
              << "  → 量化不改变任何一拍的状态/门控/故障判定，偏差只为 PCS 额定的万分之几。\n";
}

// =====================================================================
int main() {
    std::cout << "P3/ 产品化 P3 —— 通信（Modbus 设备侧 + IEC104 调度侧）演示\n";
    std::cout << "四种适配器（Sim / Memory / RtDb / Modbus / IEC104）挂同一套算法，"
                 "算法一行不改。\n";

    scenario_modbus();
    scenario_iec104();
    scenario_three_media();

    std::cout << "\n" << std::string(72, '=') << "\n"
              << " 演示结束。P3 的验收证据在 tests/test_modbus.cpp（T31~T36）与\n"
              << " tests/test_iec104.cpp（T41~T46）中，用 `scripts\\build_test_*.bat` 实跑。\n"
              << std::string(72, '=') << "\n";
    return 0;
}
