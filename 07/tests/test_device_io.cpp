// =====================================================================
// 07/ 单元测试 —— 产品化 P0 / P0.5：设备 I/O 抽象与适配器可换性
//
//   T21: 适配器等价性 —— 同一套算法，SimDeviceIO 与 MemoryDeviceIO
//        的指令/实际/SOC/关口序列必须**逐位一致**（P0.5 的核心证明）
//   T22: 点名 ↔ 业务结构体映射（写指令入点表 / 从点表装配快照）
//   T23: attach_device() 真正生效（换了适配器，限值随之改变）
//   T24: 点表自检（点齐全、无缺失点读取）
//
// 编译：g++ -std=c++17 -Wall -O2 -I src -I ../04/src -I ../05/src
//           -I ../06/src -I ../08/src tests/test_device_io.cpp
//           -o build/test_device_io.exe
// =====================================================================

#include "data_models.h"
#include "device_io.h"
#include "memory_device_io.h"
#include "plant_model.h"
#include "sim_device_io.h"
#include "realtime_loop.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace ems;

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT(cond)                                                      \
    do {                                                                  \
        if (cond) {                                                       \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #cond << std::endl;                     \
        }                                                                 \
    } while (0)

#define EXPECT_NEAR(a, b, eps)                                            \
    do {                                                                  \
        double va = (a), vb = (b);                                        \
        if (std::fabs(va - vb) <= (eps)) {                                \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va << " vs " << #b << "="  \
                      << vb << " (eps " << (eps) << ")" << std::endl;     \
        }                                                                 \
    } while (0)

// =====================================================================
// 公共：理想被控对象配置（与 MemoryDeviceIO 默认点值一一对应）
//
// 只有把仿真对象也配成"理想"（无死区、无惯性、无噪声、无温升），
// 两个适配器才具备可比性 —— 这样 T21 的"逐位一致"才有意义。
// =====================================================================
static PlantConfig ideal_plant_config() {
    PlantConfig pc;
    pc.battery_capacity_kwh = 1000.0;   // ←→ CFG.BAT_CAP_KWH
    pc.soc_init             = 0.50;     // ←→ MEAS.SOC
    pc.soc_phys_min         = 0.05;     // ←→ CFG.SOC_PHYS_MIN
    pc.soc_phys_max         = 0.95;     // ←→ CFG.SOC_PHYS_MAX
    pc.eta_chg              = 1.00;     // ←→ CFG.ETA_CHG
    pc.eta_dis              = 1.00;     // ←→ CFG.ETA_DIS
    pc.soh                  = 1.00;     // ←→ MEAS.SOH
    pc.pcs_max_chg_kw       = 200.0;    // ←→ CFG.PCS_MAX_CHG
    pc.pcs_max_dis_kw       = 200.0;    // ←→ CFG.PCS_MAX_DIS
    pc.pcs_ramp_kw_per_s    = 1e9;      // ←→ CFG.PCS_RAMP_KW_PER_S（等效无限）
    pc.pcs_tau_s            = 0.0;      // ←→ CFG.TAU_S（无惯性）
    pc.pcs_deadtime_s       = 0.0;      //    MemoryDeviceIO 无死区
    pc.pcs_standby_kw       = 2.0;      // ←→ CFG.PCS_STANDBY
    pc.temp_ambient_c       = 25.0;     // ←→ MEAS.T_C
    pc.temp_heat_coef       = 0.0;      //    无温升 → 恒 25°C
    pc.noise_kw             = 0.0;
    pc.noise_soc            = 0.0;
    return pc;
}

// EmsRuntime 层配置（与适配器无关，两侧必须完全一致）
static void setup_runtime(EmsRuntime& rt) {
    rt.config().dt_s = 0.1;
    rt.config().l2_correction_max_kw = 100.0;
    rt.device_limits().transformer_capacity_kw = 800.0;
    rt.device_limits().d_target_kw = 250.0;
    rt.safety_params().ramp_kw_per_s = 200.0;
    rt.safety_params().grid_p_min_kw = 0.0;
    rt.apply_configs();
}

// 环境脚本（负荷/光伏随时间变化，激励出 L2 三大实时控制器的动作）
static void env_at(double t, double* load, double* pv) {
    *load = 200.0; *pv = 150.0;
    if (t >= 50.0 && t < 150.0)  { *load = 450.0; *pv = 100.0; }
    else if (t >= 150.0)         { *load = 200.0; *pv = 150.0; }
}

// =====================================================================
// T21: 适配器等价性 —— P0.5 的核心证明
//
// "换数据源不改算法" 不能只写在文档里。这里用同一条 300 s 环境脚本，
// 分别驱动 SimDeviceIO（物理模型）与 MemoryDeviceIO（纯点表），
// 要求两条轨迹**逐位一致**（容差 1e-9）。
// =====================================================================
static void test_21_adapter_equivalence() {
    std::cerr << "[T21] 适配器等价性（SimDeviceIO vs MemoryDeviceIO） ...\n";
    const int    steps = 3000;
    const double dt    = 0.1;

    // ---- A 路：仿真适配器 ----
    EmsRuntime rta;
    rta.configure_plant(ideal_plant_config());
    setup_runtime(rta);
    rta.run(steps, dt, [dt](EmsRuntime& r, int i) {
        double l = 0.0, pv = 0.0;
        env_at(i * dt, &l, &pv);
        r.set_environment(l, pv);
    });

    // ---- B 路：进程内点表适配器 ----
    MemoryDeviceIO mem;
    EmsRuntime rtb;
    rtb.attach_device(&mem);
    setup_runtime(rtb);
    rtb.run(steps, dt, [&mem, dt](EmsRuntime& /*r*/, int i) {
        double l = 0.0, pv = 0.0;
        env_at(i * dt, &l, &pv);
        mem.set_environment(l, pv);
    });

    const auto& la = rta.log();
    const auto& lb = rtb.log();

    EXPECT(static_cast<int>(la.size()) == steps);
    EXPECT(static_cast<int>(lb.size()) == steps);
    EXPECT(la.size() == lb.size());

    const size_t n = std::min(la.size(), lb.size());
    double d_cmd = 0.0, d_actual = 0.0, d_soc = 0.0, d_grid = 0.0, d_temp = 0.0;
    int    d_state = 0;
    for (size_t i = 0; i < n; ++i) {
        d_cmd    = std::max(d_cmd,    std::fabs(la[i].p_cmd    - lb[i].p_cmd));
        d_actual = std::max(d_actual, std::fabs(la[i].p_actual - lb[i].p_actual));
        d_soc    = std::max(d_soc,    std::fabs(la[i].soc      - lb[i].soc));
        d_grid   = std::max(d_grid,   std::fabs(la[i].p_grid   - lb[i].p_grid));
        d_temp   = std::max(d_temp,   std::fabs(la[i].temp     - lb[i].temp));
        if (la[i].state != lb[i].state) ++d_state;
    }
    EXPECT_NEAR(d_cmd,    0.0, 1e-9);
    EXPECT_NEAR(d_actual, 0.0, 1e-9);
    EXPECT_NEAR(d_soc,    0.0, 1e-9);
    EXPECT_NEAR(d_grid,   0.0, 1e-9);
    EXPECT_NEAR(d_temp,   0.0, 1e-9);
    EXPECT(d_state == 0);

    // 两条轨迹都不是"恒 0 的假一致" —— 确实在出力、确实在动
    double max_abs_cmd = 0.0;
    for (const auto& r : la) max_abs_cmd = std::max(max_abs_cmd, std::fabs(r.p_cmd));
    EXPECT(max_abs_cmd > 10.0);
    EXPECT(std::fabs(la.back().soc - 0.50) > 1e-4);   // SOC 确实演化过

    std::cerr << "        等价性：maxΔcmd=" << d_cmd
              << " maxΔsoc=" << d_soc << " maxΔgrid=" << d_grid << "\n";
}

// =====================================================================
// T22: 点名 ↔ 业务结构体映射
// =====================================================================
static void test_22_point_mapping() {
    std::cerr << "[T22] 点名 ↔ 结构体映射 ...\n";
    MemoryDeviceIO io;

    // ---- 写指令 → 落点表 ----
    PowerCommand cmd;
    cmd.p_bat_cmd_kw = 123.4;
    cmd.p_upper      = 200.0;
    cmd.p_lower      = -150.0;
    EXPECT(io.write_command(cmd));
    EXPECT_NEAR(io.get(mem_point::kCmdPBat), 123.4, 1e-9);
    EXPECT_NEAR(io.get(mem_point::kCmdUp),   200.0, 1e-9);
    EXPECT_NEAR(io.get(mem_point::kCmdLo),  -150.0, 1e-9);

    // ---- 点表 → 装配快照 ----
    io.set_environment(300.0, 80.0);
    io.set(mem_point::kPBat, -50.0);
    io.force_soc(0.62);
    RealtimeSnapshot rt;
    EXPECT(io.read_snapshot(12.5, rt));
    EXPECT_NEAR(rt.timestamp,       12.5,   1e-9);
    EXPECT_NEAR(rt.p_bat_actual_kw, -50.0,  1e-9);
    EXPECT_NEAR(rt.p_pv_kw,          80.0,  1e-9);
    EXPECT_NEAR(rt.p_load_kw,       302.0,  1e-9);   // 300 + 站用电 2
    EXPECT_NEAR(rt.p_grid_kw,       272.0,  1e-9);   // 302 - 80 - (-50)
    EXPECT_NEAR(rt.soc,              0.62,  1e-9);
    EXPECT(rt.meters_alive["BMS"]);
    EXPECT(rt.meters_alive["METER"]);
    EXPECT(rt.meters_alive["PCS"]);

    // ---- 通信断开 → 状态位与快照同步反映 ----
    io.set_comm_meter(false);
    DeviceStatus st = io.read_status();
    EXPECT(!st.meter_comm_ok);
    EXPECT(st.bms_comm_ok);
    EXPECT(io.read_snapshot(13.0, rt));
    EXPECT(!rt.meters_alive["METER"]);

    // ---- 配置点 → read_limits ----
    io.set(mem_point::kMaxChg, 77.0);
    io.set(mem_point::kMaxDis, 88.0);
    io.set(mem_point::kBmsChgLim, 66.0);
    io.set(mem_point::kBmsDisLim, 55.0);
    io.set(mem_point::kTrKva, 999.0);
    io.set(mem_point::kDTarget, 321.0);
    DeviceLimits lim;
    EXPECT(io.read_limits(lim));
    EXPECT_NEAR(lim.pcs_rated_chg_kw,        77.0, 1e-9);
    EXPECT_NEAR(lim.pcs_rated_dis_kw,        88.0, 1e-9);
    EXPECT_NEAR(lim.bms_chg_limit_kw,        66.0, 1e-9);
    EXPECT_NEAR(lim.bms_dis_limit_kw,        55.0, 1e-9);
    EXPECT_NEAR(lim.transformer_capacity_kw, 999.0, 1e-9);
    EXPECT_NEAR(lim.d_target_kw,             321.0, 1e-9);

    // ---- 执行：一阶惯性 + SOC 积分 ----
    MemoryDeviceIO e;
    e.set_environment(0.0, 0.0);
    e.set(mem_point::kTauS, 1.0);           // τ=1s → α = 1−e^−0.1
    double got = e.execute(50.0, 0.1);
    EXPECT_NEAR(got, 50.0 * (1.0 - std::exp(-0.1 / 1.0)), 1e-9);
    // 满出力 50kW 跑 10 拍 → SOC 下降 50*1.0/3600/1000
    e.set(mem_point::kPBat, 0.0);
    e.force_soc(0.50);
    e.set(mem_point::kTauS, 0.0);
    for (int i = 0; i < 10; ++i) e.execute(50.0, 0.1);
    EXPECT_NEAR(e.get(mem_point::kSoc), 0.50 - 50.0 * 1.0 / 3600.0 / 1000.0, 1e-12);

    // ---- 故障注入 → fail-safe 零出力 ----
    e.set_pcs_fault(true);
    EXPECT_NEAR(e.execute(50.0, 0.1), 0.0, 1e-9);
    e.set_pcs_fault(false);
    e.set_comm_pcs(false);
    EXPECT_NEAR(e.execute(50.0, 0.1), 0.0, 1e-9);
}

// =====================================================================
// T23: attach_device() 真正生效
// =====================================================================
static void test_23_attach_device_takes_effect() {
    std::cerr << "[T23] attach_device 生效 ...\n";
    MemoryDeviceIO mem;
    // 把 PCS 能力降到 50kW（仿真默认 200kW）—— 若算法仍在用 PlantModel，
    // 指令就会突破 50kW，本测试即失败。
    mem.set(mem_point::kMaxChg,    50.0);
    mem.set(mem_point::kMaxDis,    50.0);
    mem.set(mem_point::kBmsChgLim, 50.0);
    mem.set(mem_point::kBmsDisLim, 50.0);

    EmsRuntime rt;
    rt.attach_device(&mem);
    EXPECT(rt.device() == static_cast<IDeviceIO*>(&mem));
    EXPECT(std::string(rt.device()->name()) == "MemoryDeviceIO(PointTable)");
    EXPECT_NEAR(rt.device_limits().pcs_rated_dis_kw, 50.0, 1e-9);

    rt.config().dt_s = 0.1;
    rt.config().enable_realtime_correction = true;
    rt.config().l2_correction_max_kw = 100.0;   // L2 纠偏有界
    rt.device_limits().transformer_capacity_kw = 800.0;
    rt.device_limits().d_target_kw = 250.0;
    rt.safety_params().grid_p_min_kw = -1e9;
    rt.safety_params().ramp_kw_per_s = 1e9;
    rt.apply_configs();

    // 大负荷 → 需量管理想把 P_bat 推到 250kW 放电，但设备只有 50kW
    mem.set_environment(500.0, 0.0);
    rt.run(500, 0.1, nullptr);

    double max_abs = 0.0;
    for (const auto& r : rt.log()) max_abs = std::max(max_abs, std::fabs(r.p_cmd));
    EXPECT(max_abs <= 50.0 + 1e-6);       // 被新适配器的限值卡住
    EXPECT(max_abs > 1.0);                // 确实在出力（不是被门控成恒 0）

    // 换回默认仿真适配器 → 限值恢复 200kW
    rt.attach_device(nullptr);
    EXPECT(rt.device() != nullptr);
    EXPECT_NEAR(rt.device_limits().pcs_rated_dis_kw, 200.0, 1e-9);
}

// =====================================================================
// T24: 点表自检
// =====================================================================
static void test_24_point_table_selfcheck() {
    std::cerr << "[T24] 点表自检 ...\n";
    MemoryDeviceIO io;
    const char* names[] = {
        mem_point::kPLoad, mem_point::kPPv, mem_point::kPBat, mem_point::kPGrid,
        mem_point::kSoc, mem_point::kTC, mem_point::kSoh,
        mem_point::kCmdPBat, mem_point::kCmdUp, mem_point::kCmdLo,
        mem_point::kCapKwh, mem_point::kMaxChg, mem_point::kMaxDis,
        mem_point::kBmsChgLim, mem_point::kBmsDisLim, mem_point::kTrKva,
        mem_point::kDTarget, mem_point::kTauS, mem_point::kRampKwS,
        mem_point::kStandby, mem_point::kEtaChg, mem_point::kEtaDis,
        mem_point::kSocMin, mem_point::kSocMax,
        mem_point::kStaBms, mem_point::kStaPcs, mem_point::kStaMeter,
        mem_point::kStaFault, mem_point::kStaOff, mem_point::kStaValid,
    };
    for (const char* n : names) EXPECT(io.has_point(n));

    // 跑一轮完整闭环，过程中不得出现"读到不存在的点"
    EmsRuntime rt;
    rt.attach_device(&io);
    rt.config().dt_s = 0.1;
    rt.device_limits().transformer_capacity_kw = 800.0;
    rt.apply_configs();
    io.set_environment(300.0, 100.0);
    rt.run(200, 0.1, nullptr);
    EXPECT(io.miss_count() == 0);
    EXPECT(rt.log().size() == 200u);
}

// =====================================================================
int main() {
    std::cerr << "=========================================\n"
              << " 07/ P0+P0.5 设备 I/O 抽象与适配器可换性 单元测试\n"
              << "=========================================\n";

    test_21_adapter_equivalence();
    test_22_point_mapping();
    test_23_attach_device_takes_effect();
    test_24_point_table_selfcheck();

    std::cerr << "=========================================\n"
              << " PASS=" << g_pass << "  FAIL=" << g_fail << "\n"
              << "=========================================\n";
    return (g_fail == 0) ? 0 : 1;
}
