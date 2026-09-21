// =====================================================================
// 11/ 单元测试 —— 周期 11：系统级联调（T41~T49）
//
//   T41: 通信建立与角色边界 —— 初始化器语义 / 双连接 / 设备侧只写
//        MEAS/STA/CFG、EMS 只写 CMD，互不越界
//   T42: 数据采集稳定性 —— 六类数据源 → 全点表，长跑零 stale、
//        零点表失配，所有量测点确实在变化（防"恒 0 假联调"）
//   T43: 控制指令与状态同步 —— CMD 三点全链路下发 / 设备侧观测指令
//        全部落在 EMS 声明的权限区间内 / 实际功率跟随 / 状态机迁移链
//   T44: 故障触发与异常恢复 —— 六类故障源逐个注入：触发（FAULT/
//        HOLD_LAST 路径正确）→ 恢复（停在 READY，不自动带载）
//   T45: 长稳（soak）—— 24 h = 86400 拍跨共享内存闭环，硬不变量逐拍
//        成立、墙钟有界、内存有界
//   T46: 日志记录全流程 —— P2 SOE 事件成对出现 / 迁移链不被抑制 /
//        观察者步数与 log_every 解耦
//   T47: 并发采集抗碰撞 —— 真读写并发下 seqlock 碰撞不得计成采集失败
//        （单进程顺序读写的测试永远暴露不了这一类缺陷）
//   T48: BMS 禁充放位 —— 设备侧经共享内存上报 → 安全层收紧上界 →
//        下发指令不放电。防的是"适配器里硬写 false"这一类**路上没有来源**
//        的缺口（仿真直接注入 DeviceLimits，绕开点表就照不出来）
//
// 与 07/test_rtdb_device_io.cpp（T25~T29）的差别：
//   T25~T29 验证的是**适配器本身**（点表契约 / 等价性 / 双连接 / 单故障 / 关口功率单一数据源）；
//   本文件验证的是**联调剧本** —— 六类数据源全链路、六类故障源逐个
//   触发恢复、24 h 长稳、日志全流程。设备侧逻辑与多进程演示
//   （device_side.exe）是**同一份 DeviceSideSim::step()**。
//
// 编译（见 11/scripts/build_test.bat）：
//   gcc: rt_db_api.c / ems_point_table.c / ems_rt_db_setup.c
//   g++: -I src -I ../04/src -I ../05/src -I ../06/src -I ../07/src
//        -I ../08/src -I ../P2/src -I ../07/src/rtdb -I ../07/vendor/rt_db
// =====================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "integration_runner.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <atomic>
#include <thread>
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
// 公共装置
// =====================================================================
struct RtdbEnv {
    rt_db_handle_t handle{};
    bool ok = false;
    RtdbEnv() {
        bool created = false;
        ok = ems_rt_db_setup(/*reset=*/true, &created) && rt_db_init(&handle, nullptr);
    }
    ~RtdbEnv() {
        if (ok) rt_db_cleanup(&handle);
    }
};

static void reset_segment() {
    bool created = false;
    ems_rt_db_setup(/*reset=*/true, &created);
}

// 单进程联调装置：设备角色 + EMS 角色 + 共享内存段。
// 设备侧持**独立连接**（h_dev）—— 与多进程形态同构：设备进程/EMS 进程
// 各自 rt_db_init，看到同一段。设备推进由 EMS 的 device_pump 驱动
// （T26 已证 pump 模式与进程内逐位等价，本模块沿用该机制跑联调剧本）。
struct IntegrationRig {
    RtdbEnv        env;
    rt_db_handle_t h_dev{};
    bool           dev_connected = false;

    std::unique_ptr<DeviceSideSim> device;
    std::unique_ptr<EmsSideApp>    app;
    double dev_t = 0.0;

    explicit IntegrationRig(const std::vector<DevFaultWindow>& script,
                            const EmsSideApp::Config& cfg = EmsSideApp::Config(),
                            const DeviceSimConfig& plant = DeviceSimConfig()) {
        reset_segment();
        dev_connected = rt_db_init(&h_dev, nullptr);
        device = std::make_unique<DeviceSideSim>(&h_dev, script, plant);
        app    = std::make_unique<EmsSideApp>(&env.handle, cfg);
        app->io().set_device_pump([this](double, double dt) {
            device->step(dev_t, dt);
            dev_t += dt;
        });
    }
    ~IntegrationRig() {
        if (dev_connected) rt_db_cleanup(&h_dev);
    }

    void run(int n, double dt) { app->run(n, dt); }
};

// =====================================================================
// T41: 通信建立与角色边界
// =====================================================================
static void test_41_comm_and_role_boundary(RtdbEnv& env) {
    std::cerr << "[T41] 通信建立与角色边界（初始化器语义 / 双连接 / 点区不越界）...\n";
    EXPECT(env.ok);
    if (!env.ok) return;

    reset_segment();

    // ---- 部署顺序约束（联调检查 1）：初始化器（reset）必须在所有连接之前。
    //      reset 之后段级连接计数清零 —— 若顺序错了，现场表现为"计数与真实
    //      连接数脱节"，这里直接断言清零行为，防止后人把顺序"优化"掉。
    EXPECT(rt_db_get_connected_clients(&env.handle) == 0);

    // ---- 设备侧进程连接（联调检查 2：通信建立）
    rt_db_handle_t h_dev{};
    EXPECT(rt_db_init(&h_dev, nullptr));
    EXPECT(rt_db_get_connected_clients(&env.handle) == 1);
    {
        DeviceSideSim device(&h_dev);   // 构造即发布全表初值
        EXPECT(device.stats().steps == 0);

        // ---- EMS 侧进程连接（联调检查 3）
        RtDbDeviceIO io(&env.handle);
        EXPECT(io.is_open());
        EXPECT(io.self_check() == 0);            // 全点表点名/单位/索引对得上
        RealtimeSnapshot snap;
        EXPECT(io.read_snapshot(0.0, snap));     // data_valid（设备侧已发布 STA.VALID）
        EXPECT(io.read_status().data_valid);
        EXPECT(io.stale_reads() == 0);
        EXPECT_NEAR(snap.p_load_kw, 2.0, 1e-9);  // 初值 P_LOAD=0 + 站用电 2 kW

        // ---- 角色边界：EMS 写 CMD 三点
        // 符号约定（项目统一）：P_bat 放电为正、充电为负 → 允许下界是**负数**。
        // 这里刻意用 -80 覆盖"负值经共享内存往返不变号"这条契约。
        PowerCommand cmd;
        cmd.p_bat_cmd_kw = 123.0;
        cmd.p_upper      = 150.0;
        cmd.p_lower      = -80.0;
        EXPECT(io.write_command(cmd));

        // ---- 设备侧跑 3 拍：只写 MEAS/STA/CFG，CMD 区必须原封不动
        for (int i = 0; i < 3; ++i) device.step(i * 0.1, 0.1);

        RtDbPointWriter w(&h_dev);
        double v = 0.0;
        long   q = 0;
        EXPECT(w.read(EMS_CMD_P_BAT, &v, &q) && std::fabs(v - 123.0) < 1e-9);
        EXPECT(w.read(EMS_CMD_P_UPPER, &v, &q) && std::fabs(v - 150.0) < 1e-9);
        EXPECT(w.read(EMS_CMD_P_LOWER, &v, &q) && std::fabs(v - (-80.0)) < 1e-9);

        // ---- EXT 区（A3.2 起）：设备侧 publish_all() 必须跳过 —— 网关才写这里。
        //      判据：EXT.SEQ 仍为 0（从未被发布）。若设备侧碰了 EXT，这里会被改写。
        EXPECT(w.read(EMS_EXT_SEQ, &v, &q) && v == 0.0);

        // ---- 反向边界：设备侧观测到 EMS 指令（下行链路通）
        EXPECT_NEAR(device.observed_cmd_kw(), 123.0, 1e-9);
        EXPECT(device.stats().cmd_out_of_interval == 0);

        // ---- 设备侧量测变化经段回传 EMS（上行链路通）
        device.dev().force_soc(0.61);
        device.publish_all(0.3);
        EXPECT(io.read_snapshot(0.3, snap));
        EXPECT_NEAR(snap.soc, 0.61, 1e-9);
    }
    rt_db_cleanup(&h_dev);
}

// =====================================================================
// T42: 数据采集稳定性（六类数据源 → 全点表）
// =====================================================================
static void test_42_data_acquisition_stability(RtdbEnv& env) {
    std::cerr << "[T42] 数据采集稳定性（20000 拍跨进程采集）...\n";
    EXPECT(env.ok);
    if (!env.ok) return;

    EmsSideApp::Config cfg;
    cfg.dt_s = 0.1;
    IntegrationRig rig({}, cfg);   // 无故障剧本

    const int N = 20000;           // 2000 s
    rig.run(N, 0.1);

    // ---- 零失败采集（联调"全流程稳定性"的第一条）
    EXPECT(rig.app->io().stale_reads() == 0);
    EXPECT(rig.app->io().self_check() == 0);

    // ---- 全程无故障位（无剧本 → 不应有任何故障/保持）
    int fault_ticks = 0, hold_ticks = 0;
    for (const auto& r : rig.app->rt().log()) {
        if (r.fault_bits != 0) ++fault_ticks;
        if (r.hold_last)       ++hold_ticks;
    }
    EXPECT(fault_ticks == 0);
    EXPECT(hold_ticks == 0);

    // ---- 六类数据源全部"活着"（防恒 0 假联调：每个量测点都得真的在动）
    double load_min = 1e9, load_max = -1e9;
    double pv_min = 1e9,   pv_max = -1e9;
    double bat_min = 1e9,  bat_max = -1e9;
    double grid_min = 1e9, grid_max = -1e9;
    double soc_min = 1.0,  soc_max = 0.0;
    double t_min = 1e9,    t_max = -1e9;
    for (const auto& r : rig.app->rt().log()) {
        load_min = std::min(load_min, r.p_load);  load_max = std::max(load_max, r.p_load);
        pv_min   = std::min(pv_min, r.p_pv);      pv_max   = std::max(pv_max, r.p_pv);
        bat_min  = std::min(bat_min, r.p_actual); bat_max  = std::max(bat_max, r.p_actual);
        grid_min = std::min(grid_min, r.p_grid);  grid_max = std::max(grid_max, r.p_grid);
        soc_min  = std::min(soc_min, r.soc);      soc_max  = std::max(soc_max, r.soc);
        t_min    = std::min(t_min, r.temp);       t_max    = std::max(t_max, r.temp);
    }
    EXPECT(load_max - load_min > 100.0);   // 负荷（电表数据源）
    EXPECT(pv_max - pv_min > 50.0);        // 光伏（逆变器数据源）
    EXPECT(bat_max - bat_min > 10.0);      // 电池（PCS/BMS 数据源，EMS 在出力）
    EXPECT(grid_max - grid_min > 100.0);   // 关口（电表派生）
    EXPECT(soc_max - soc_min > 1e-3);      // SOC（BMS 数据源）
    EXPECT(t_max - t_min > 1e-4);          // 温度（BMS 热模型数据源）

    EXPECT(static_cast<int>(rig.app->rt().log().size()) == N);
    std::cerr << "       采集 " << N << " 拍 / stale=0 / 负荷 [" << load_min << ","
              << load_max << "] / SOC [" << soc_min << "," << soc_max << "]\n";
}

// =====================================================================
// T43: 控制指令与状态同步
// =====================================================================
static void test_43_command_and_state_sync(RtdbEnv& env) {
    std::cerr << "[T43] 控制指令全链路与状态同步...\n";
    EXPECT(env.ok);
    if (!env.ok) return;

    EmsSideApp::Config cfg;
    cfg.dt_s = 0.1;
    IntegrationRig rig({}, cfg);

    const int N = 3000;
    rig.run(N, 0.1);

    // ---- 状态同步：迁移链完整、终态 NORMAL
    EXPECT(rig.app->rt().fsm().state() == EmsState::kNormal);
    EXPECT(rig.app->rt().fsm().history().size() >= 3);
    EXPECT(rig.app->rt().fsm().output_enabled());

    // ---- 控制指令：EMS 真的在出力，且设备侧全链路观测
    const auto& log = rig.app->rt().log();
    double max_cmd = 0.0;
    for (const auto& r : log) max_cmd = std::max(max_cmd, std::fabs(r.p_cmd));
    EXPECT(max_cmd > 50.0);
    EXPECT(rig.device->stats().max_abs_cmd_kw > 50.0);

    // ---- 设备侧硬契约：观测到的指令 100% 落在 EMS 声明的权限区间内
    EXPECT(rig.device->stats().cmd_out_of_interval == 0);
    EXPECT(rig.device->stats().cmd_observed > 100);   // 指令确实在持续下发

    // ---- 末拍一致性：设备侧读到的 CMD 三点 == EMS 末拍记录（全链路同步）
    RtDbPointWriter w(&rig.h_dev);
    double v = 0.0;
    const StepRecord& last = log.back();
    EXPECT(w.read(EMS_CMD_P_BAT, &v) && std::fabs(v - last.p_cmd) < 1e-6);
    EXPECT(w.read(EMS_CMD_P_UPPER, &v) && std::fabs(v - last.p_upper) < 1e-6);
    EXPECT(w.read(EMS_CMD_P_LOWER, &v) && std::fabs(v - last.p_lower) < 1e-6);

    // ---- 实际功率跟随（稳态段跟踪误差有界；PCS 一阶惯性 tau=0.5 s）
    double sum_err = 0.0;
    int    n_err = 0;
    for (int i = N - 200; i < N; ++i) {
        sum_err += std::fabs(log[i].p_cmd - log[i].p_actual);
        ++n_err;
    }
    const double mean_err = n_err ? sum_err / n_err : 1e9;
    EXPECT(mean_err < 30.0);

    std::cerr << "       峰值指令 " << max_cmd << " kW / 末段平均跟踪误差 "
              << mean_err << " kW / 迁移 " << rig.app->rt().fsm().history().size()
              << " 次\n";
}

// =====================================================================
// T44: 故障触发与异常恢复（六类故障源逐个）
// =====================================================================
static void test_44_fault_trigger_and_recovery(RtdbEnv& env) {
    std::cerr << "[T44] 故障触发与异常恢复（六类故障源逐个注入）...\n";
    EXPECT(env.ok);
    if (!env.ok) return;

    // 每类故障源的期望行为（联调口径，从 06/ 状态机与接口规范 §6 推导）：
    //   kind 1 BMS 通信断   → FAULT + 门控归零
    //   kind 2 电表通信断   → 不进 FAULT，采集超时走 HOLD_LAST（接口规范 §6）
    //   kind 3 PCS 通信断   → FAULT + 门控归零
    //   kind 4 PCS 故障     → FAULT + 门控归零
    //   kind 5 设备离线     → FAULT + 门控归零
    //   kind 6 品质位劣化   → data_invalid → FAULT + 门控归零
    struct Expect {
        int  kind;
        bool to_fault;
        int  fault_bit;      // 期望置位的 fault_bits 位
    };
    const Expect expects[] = {
        {1, true,  0}, {2, false, 2}, {3, true, 1},
        {4, true, 3},  {5, true, 5},  {6, true, 4},
    };

    const double dt = 0.1;
    const int pre = 300;      // 正常段 30 s
    const int win = 100;      // 故障窗 10 s（30~40 s）
    const int post = 200;     // 恢复段 20 s

    for (const auto& e : expects) {
        reset_segment();

        std::vector<DevFaultWindow> script;
        DevFaultWindow fw;
        fw.kind = e.kind;
        fw.t_begin_s = 30.0;
        fw.t_end_s   = 40.0;
        fw.note      = dev_fault_kind_name(e.kind);
        script.push_back(fw);

        EmsSideApp::Config cfg;
        cfg.dt_s = dt;
        IntegrationRig rig(script, cfg);

        rig.run(pre, dt);

        // ---- 触发前：正常运行
        EXPECT(rig.app->rt().fsm().state() == EmsState::kNormal);
        EXPECT(rig.app->rt().log().back().fault_bits == 0);

        // ---- 故障窗内
        rig.run(win, dt);
        const auto& inwin = rig.app->rt().log();
        bool saw_bit = false, saw_gate = false, saw_fault_state = false;
        for (int i = pre; i < pre + win; ++i) {
            const auto& r = inwin[i];
            if (r.fault_bits & (1 << e.fault_bit)) saw_bit = true;
            if (r.state_gated && std::fabs(r.p_cmd) < 1e-9) saw_gate = true;
            if (r.state == EmsState::kFault) saw_fault_state = true;
        }
        EXPECT(saw_bit);                 // 故障位经共享内存传到 EMS
        if (e.to_fault) {
            EXPECT(saw_fault_state);     // 进 FAULT
            EXPECT(saw_gate);            // 门控归零
        } else {
            // 电表断：不进 FAULT；持续 10 s > comm_stale_threshold 5 s → HOLD_LAST
            bool saw_hold = false;
            for (int i = pre; i < pre + win; ++i)
                if (inwin[i].hold_last) saw_hold = true;
            EXPECT(saw_hold);
        }

        // ---- 恢复段：故障清除
        rig.run(post, dt);
        const auto& fin = rig.app->rt().log().back();
        EXPECT(fin.fault_bits == 0);
        if (e.to_fault) {
            EXPECT(rig.app->rt().fsm().state() != EmsState::kFault);
            EXPECT(!rig.app->rt().fsm().output_enabled());   // 停在 READY，不自动带载
            EXPECT(std::fabs(fin.p_cmd) < 1e-9);
        }

        // ---- 设备侧 fail-safe 统计（联调口径：**设备自保**只在设备自己看得见
        //      的故障源上触发，即 PCS 故障 / 设备离线 / PCS 通信断 —— 这三条在
        //      DeviceSideSim 的物理模型里都能独立判定。
        //        · kind 2 电表通信断：PCS 无感知，EMS 走 HOLD_LAST（不算 fail-safe）
        //        · kind 1 BMS 通信断  ：断的是"BMS→EMS"这条链路，PCS 仍在正常
        //          执行；现场的保护动作由 EMS 门控完成（与 06/ 的 FAULT 路径一致），
        //          **不能**要求 PCS 自己跳 —— 那等于把 EMS 的职责塞进设备。
        //        · kind 6 品质位劣化  ：值仍然是值，只是不可信；EMS 据此进 FAULT，
        //          设备侧照常执行指令。
        //      这张"谁该动"的表就是联调要固化下来的角色边界。
        const bool dev_self_protect = (e.kind == 3 || e.kind == 4 || e.kind == 5);
        if (dev_self_protect) {
            EXPECT(rig.device->stats().failsafe_ticks > 0);
        } else {
            EXPECT(rig.device->stats().failsafe_ticks == 0);
        }

        std::cerr << "       kind " << e.kind << " ("
                  << dev_fault_kind_name(e.kind) << ") : bit" << e.fault_bit
                  << " 置位 / " << (e.to_fault ? "FAULT+门控" : "HOLD_LAST")
                  << " / 恢复不自动带载 / 设备自保="
                  << (dev_self_protect ? "是" : "否（EMS 侧动作）") << "\n";
    }
}

// =====================================================================
// T45: 长稳（soak）—— 24 h 跨共享内存闭环
// =====================================================================
static void test_45_soak_24h(RtdbEnv& env) {
    std::cerr << "[T45] 长稳 soak：24 h = 86400 拍跨共享内存闭环...\n";
    EXPECT(env.ok);
    if (!env.ok) return;

    // 三个故障窗：清晨 BMS 通信闪断 60 s / 午间 PCS 故障 120 s / 傍晚品质劣化 300 s
    std::vector<DevFaultWindow> script;
    DevFaultWindow w1; w1.kind = 1; w1.t_begin_s = 6.0 * 3600.0;
    w1.t_end_s = 6.0 * 3600.0 + 60.0;  w1.note = "morning_bms";
    DevFaultWindow w4; w4.kind = 4; w4.t_begin_s = 12.0 * 3600.0;
    w4.t_end_s = 12.0 * 3600.0 + 120.0; w4.note = "noon_pcs";
    DevFaultWindow w6; w6.kind = 6; w6.t_begin_s = 18.0 * 3600.0;
    w6.t_end_s = 18.0 * 3600.0 + 300.0; w6.note = "dusk_quality";
    script.push_back(w1);
    script.push_back(w4);
    script.push_back(w6);

    EmsSideApp::Config cfg;
    cfg.dt_s      = 1.0;    // 24 h @ 1 s
    cfg.log_every = 10;     // 长稳默认降采样
    IntegrationRig rig(script, cfg);

    const int N = 86400;
    const auto t0 = std::chrono::steady_clock::now();
    rig.run(N, 1.0);
    const auto t1 = std::chrono::steady_clock::now();
    const double wall_s = std::chrono::duration<double>(t1 - t0).count();

    // ---- 硬不变量（观察者逐拍判定，不受 log_every 影响）
    EXPECT(rig.app->obs().totals().out_of_interval_ticks == 0);

    // ---- 门控不变量：非运行态不允许非零指令（从日志检查，降采样下仍应零违例）
    int gated_nonzero = 0;
    for (const auto& r : rig.app->rt().log())
        if (r.state_gated && std::fabs(r.p_cmd) > 1e-9) ++gated_nonzero;
    EXPECT(gated_nonzero == 0);

    // ---- 跨进程采集品质：24 h 零失败（品质劣化窗只影响 data_valid，不算 stale）
    EXPECT(rig.app->io().stale_reads() == 0);

    // ---- SOC 物理边界
    EXPECT(rig.app->obs().totals().soc_min >= 0.0);
    EXPECT(rig.app->obs().totals().soc_max <= 1.0);

    // ---- 内存有界：日志条数 = N/log_every（确定值即有界）；SOE 远小于容量
    EXPECT(static_cast<int>(rig.app->rt().log().size()) == N / 10);
    EXPECT(rig.app->obs().soe().size() < 200);
    EXPECT(rig.app->obs().soe().dropped() == 0);

    // ---- 墙钟预算：24 h 模拟必须远小于 5 分钟（联调可重复性）
    EXPECT(wall_s < 300.0);

    // ---- 故障剧本确实发生过（防"剧本没生效"的假通过）
    EXPECT(rig.device->stats().comm_fault_ticks > 0);
    EXPECT(rig.device->stats().quality_bad_ticks > 0);

    std::cerr << "       24h/86400 拍完成，墙钟 " << std::fixed
              << std::setprecision(1) << wall_s << " s（"
              << 86400.0 / wall_s << "x 加速）/ log "
              << rig.app->rt().log().size() << " 行 / SOE "
              << rig.app->obs().soe().size() << " 条\n";
}

// =====================================================================
// T46: 日志记录全流程（P2 SOE / 指标 / 与 log_every 解耦）
// =====================================================================
static void test_46_logging_end_to_end(RtdbEnv& env) {
    std::cerr << "[T46] 日志记录全流程（SOE 成对 / 迁移链 / 解耦）...\n";
    EXPECT(env.ok);
    if (!env.ok) return;

    // 剧本：PCS 故障窗 + 品质劣化窗（两个独立窗，验证事件各自成对）
    std::vector<DevFaultWindow> script;
    DevFaultWindow f4; f4.kind = 4; f4.t_begin_s = 20.0; f4.t_end_s = 30.0;
    f4.note = "pcs_fault_win";
    DevFaultWindow f6; f6.kind = 6; f6.t_begin_s = 50.0; f6.t_end_s = 60.0;
    f6.note = "quality_win";
    script.push_back(f4);
    script.push_back(f6);

    EmsSideApp::Config cfg;
    cfg.dt_s = 0.1;
    IntegrationRig rig(script, cfg);
    const int N = 1200;
    rig.run(N, 0.1);

    const auto& soe = rig.app->obs().soe();

    // ---- PCS 故障事件成对且有序（SET 在前，CLEAR 在后）
    EXPECT(soe.count(SoeCode::kPcsFaultSet) >= 1);
    EXPECT(soe.count(SoeCode::kPcsFaultClear) >= 1);
    const SoeEvent* set = soe.find(SoeCode::kPcsFaultSet);
    const SoeEvent* clr = soe.find(SoeCode::kPcsFaultClear);
    EXPECT(set != nullptr && clr != nullptr && set->first_t < clr->first_t);

    // ---- 状态迁移链完整（迁移事件不被抑制合并）
    EXPECT(soe.count(SoeCode::kFsmTransition) >= 3);

    // ---- 观察者生命周期事件存在
    EXPECT(soe.count(SoeCode::kObserverStart) == 1);

    // ---- SOE 有界 + 零丢失
    EXPECT(soe.size() < 100);
    EXPECT(soe.dropped() == 0);

    // ---- 指标注册表非空（O(1) 增量指标在联调期持续更新）
    EXPECT(rig.app->obs().metrics().size() > 0);

    // ---- 观察者步数 == 总拍数（与 log_every 无关；本遍 log_every=1）
    EXPECT(rig.app->obs().totals().steps == N);

    // ---- 第二遍：log_every=10，观察者步数不变（解耦验证，P2 核心价值）
    reset_segment();
    EmsSideApp::Config cfg10 = cfg;
    cfg10.log_every = 10;
    IntegrationRig rig2(script, cfg10);
    rig2.run(N, 0.1);
    EXPECT(rig2.app->obs().totals().steps == N);       // 观察者不受降采样影响
    EXPECT(rig2.app->rt().log().size() == static_cast<std::size_t>(N / 10));
    EXPECT(rig2.app->obs().soe().count(SoeCode::kPcsFaultSet) >= 1);  // SOE 不受影响

    std::cerr << "       SOE " << soe.size() << " 条（PCS_FAULT 成对 / 迁移链 "
              << soe.count(SoeCode::kFsmTransition) << " 条）/ 观察者步数 "
              << rig.app->obs().totals().steps << " == " << N
              << "（log_every=1 与 10 两遍一致）\n";
}

// =====================================================================
// T47: 并发采集抗碰撞（seqlock 重试）
//
// 这是**只有真·并发才能暴露**的一类缺陷：单进程测试里设备泵跑在
// EmsRuntime::step() 内部，读写是顺序的，seqlock 永远撞不上；只有设备进程
// 与 EMS 进程各自独立跑节拍时，读点才会嵌在写点中间。RT_DB 的
// rt_db_get_value() 是 seqlock 读，此时返回 false —— 那是"请重试"，不是
// "采集失败"。适配器若直接把单次 false 计成 stale，现场就会出现大量假告警
// （三进程演示实测：600 拍里 51 次，全部假报警）。
//
// 判据（四层，缺一层就会被"两边都恒 0"骗过）：
//   ① 反向守卫 collisions() > 0 —— 本轮真的撞上了，测试才测到了东西
//      （读循环会一直跑到撞上为止，上限 3 s）；
//   ② 碰撞不是降级 stale_reads() < collisions() —— 重试确实吸收了碰撞；
//      把重试循环删掉（单次 false 直接计 stale）→ stale==collisions → 立刻红；
//   ③ 降级率 stale_reads()*1000 < reads —— 绝对量级守卫（< 0.1%）；
//   ④ 正确性（硬判据）bad_soc == 0 —— 重试确实挡住了半写状态。
//
// 刻意**不**断言 stale_reads() == 0：那是把"重试预算"当硬保证。写者临界区含
// update_timestamp()，若写者恰在此刻被 OS 抢走（Windows 时间片 15.6 ms），
// yield 会立刻返回、64 次预算在微秒级烧完 → 读者必然降级（降级 = 保留上一次
// 有效值 + 品质置不可信，是**设计内的降级路径**，不是故障；下游安全层本来就
// 拒绝采信 quality!=GOOD 的点）。2026-09-19 全量构建满载实测：1068810 次
// read_snapshot 里 22 次降级（≈2e-5），而同一二进制单跑恒为 0 —— 也就是说
// "恒 0"这个判据测的是**机器闲不闲**，不是**代码对不对**。
// =====================================================================
static void test_47_concurrent_acquisition(RtdbEnv& env) {
    std::cerr << "[T47] 并发采集抗碰撞（seqlock 重试，真读写并发）...\n";
    EXPECT(env.ok);
    if (!env.ok) return;

    reset_segment();

    rt_db_handle_t h_dev{};
    rt_db_handle_t h_ems{};
    EXPECT(rt_db_init(&h_dev, nullptr));
    EXPECT(rt_db_init(&h_ems, nullptr));

    RtDbPointWriter w(&h_dev);
    RtDbDeviceIO    io(&h_ems);
    EXPECT(io.self_check() == 0);

    std::atomic<bool> stop{false};
    std::atomic<long> n_writes{0};

    // 写侧强度取**真实设备节拍量级**：一轮扫完 27 个设备侧点（与设备进程
    // publish_all() 同构），然后 sleep(1 ms)。Windows 下 sleep_for(1 ms) 实测
    // 约 17 ms 才返回，所以写频率约为真实部署（每 100 ms 一轮）的 6 倍。
    //
    // 刻意不取"零间隙猛写"：那样写者会 100% 占住 seqlock，读者的重试预算必然
    // 耗尽（重试次数再多也只是把概率往下压，不会归零），那衡量的是"写者让不
    // 让出"而不是"采集可不可靠"。真实设备进程每拍写一轮就让出，属于前者。
    const int burst = 1;
    std::thread writer([&] {
        double  v     = 0.0;
        long    sweep = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            for (std::size_t i = 0; i < EMS_POINT_COUNT; ++i) {
                // 角色边界：设备侧不写 CMD 区
                if (i >= static_cast<std::size_t>(EMS_CMD_P_BAT) &&
                    i <= static_cast<std::size_t>(EMS_CMD_P_LOWER)) {
                    continue;
                }
                // SOC 限定在 [0.1, 0.9]，便于断言"读到的值自洽"
                const double val = (i == static_cast<std::size_t>(EMS_SOC))
                                       ? 0.5 + 0.4 * std::sin(v)
                                       : 100.0 + 50.0 * std::sin(v);
                w.write(i, val);
                ++n_writes;
            }
            v += 0.01;
            if (++sweep % burst == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    int reads = 0, bad_soc = 0;
    const auto t0 = std::chrono::steady_clock::now();
    // 读循环至少跑 1.0 s，且**必须真的撞上过一次碰撞**才收工（硬上限 3 s）：
    // 否则这一轮压根没走到 seqlock 抗碰撞路径，"吸收/降级"类断言就退化成恒真。
    for (;;) {
        const double el = std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - t0).count();
        if ((el >= 1.0 && io.collisions() > 0) || el >= 3.0) break;
        RealtimeSnapshot s;
        io.read_snapshot(0.0, s);
        if (s.soc < 0.0 || s.soc > 1.0) ++bad_soc;
        ++reads;
    }
    stop.store(true);
    writer.join();

    // 判据四层 —— 前两层是**反向守卫**，缺了就会被"两边都恒 0"骗过：
    //   ① 真的撞上了：本测试才测到了东西（单跑实测 碰撞≈84 / 110 万次读循环）
    //   ② 碰撞不是降级：重试至少吸收了一部分（删掉重试循环 → stale==collisions → 红）
    //   ③ 降级率 < 0.1%（相对读循环）：绝对量级守卫，防"重试形同虚设"时失控
    //   ④ 硬判据 bad_soc == 0：半写状态一次都没漏出去
    // 刻意**不**断言 stale_reads() == 0（理由见文件头 T47 说明）。
    EXPECT(io.collisions() > 0);
    EXPECT(io.stale_reads() < io.collisions());
    EXPECT(io.stale_reads() * 1000 < reads);
    EXPECT(reads > 100);                  // 读侧确实在高频跑
    EXPECT(n_writes.load() > 1000);       // 写侧确实在跑（≥ 37 次全点扫描）
    EXPECT(bad_soc == 0);                 // 读到的值始终自洽（无半写状态）

    std::cerr << "       并发读写 " << n_writes.load() << " 点 / " << reads
              << " 次 read_snapshot / 碰撞=" << io.collisions()
              << " 吸收=" << io.absorbed() << " 降级=" << io.stale_reads() << "\n";

    rt_db_cleanup(&h_ems);
    rt_db_cleanup(&h_dev);
}

// =====================================================================
// T48: BMS 禁充放位 —— 跨进程安全输入（A1，2026-09-19 补）
//
// 为什么单独一条用例：这两个 bool 是 04/S01(kBmsForbid, **L0 最底层**) 与
// 05/check_bms_forbid() 的**唯一输入**。而 2026-09-19 之前
// RtDbDeviceIO::read_limits() 把它们**硬写成 false** —— 等于"BMS 永远允许
// 充放"，设备侧上报的禁充放被静默吞掉，而三层测试全绿也没人发现。
//
// 根因不是"没写"，是"写了、但这条路上没有来源"。这类缺口有个共同特征：
// **测试里观测不到**（仿真/单测直接注入 DeviceLimits，绕开了点表这条路），
// 只有"设备侧经共享内存上报"这条真实路径才照得出来。所以本用例刻意用
// 设备侧剧本 + RtDbDeviceIO，全链路不复用任何上层注入。
//
// 判据设计（7 层，缺一层就会被"两边都恒 0"骗过）：
//   ① 反向守卫 dis_ticks ≥ 90 —— 窗内 100 拍里设备侧真的每拍都在发禁放位；
//   ② 位不串 chg_ticks == 0；
//   ③ 窗外残留 ≤ 2 拍 —— 只允许边界那 1 拍的读取延迟，不允许持续漏出；
//   ④ 硬判据 窗内 p_upper ≤ 0 的拍数 ≥ 窗内拍数−3 —— 安全层把上界收死
//      （用**计数**而非 max：列首列尾各 1 拍的边界过渡不会污染判据）；
//   ⑤ **差分** 窗外 p_upper ≤ 0 的拍数 ≤ 2 —— 证明那个 0 是禁放造成的，
//      而不是"本来这台机器上界就是 0"（关键：不依赖"EMS 想放电"）；
//   ⑥ 硬判据 窗内下发指令 p_cmd ≤ 0 —— 约束必须一路走到指令上；
//   ⑦ 语义 窗内 p_lower < 0 —— 禁放**只压上界**，没被误写成双向锁死。
//
// ★ 本用例还照出了一处更深的东西（已一并修）：`EmsRuntime::step()` 原本
//   **从不调用** refresh_device_limits() —— 限值在运行期是冻结的。
//   于是不只是禁充放位，连 BMS 动态降功率、PCS 额定、变压器容量都退化成
//   装配期常量（而 04/IDeviceIO 的契约写的是"每个控制周期刷新"）。
//   修法见 LoopConfig::refresh_limits_each_step。
// =====================================================================
static void test_48_bms_forbid_cross_process(RtdbEnv& env) {
    std::cerr << "[T48] BMS 禁充放位：设备侧上报 → 安全层收紧 → 下发指令不越界 ...\n";
    EXPECT(env.ok);
    if (!env.ok) return;

    // 禁放窗 [10, 20) s，其余时间允许。dt = 0.1 → 拍 100~199 落在窗内。
    std::vector<DevFaultWindow> script;
    DevFaultWindow w8;
    w8.kind      = 8;                     // BMS_FORBID_DISCHARGE
    w8.t_begin_s = 10.0;
    w8.t_end_s   = 20.0;
    w8.note      = dev_fault_kind_name(w8.kind);
    script.push_back(w8);

    EmsSideApp::Config cfg;
    cfg.dt_s      = 0.1;
    cfg.log_every = 1;
    IntegrationRig rig(script, cfg);

    const double dt = 0.1;
    const int    N  = 300;                // 30 s：窗前 10 s / 窗内 10 s / 窗后 10 s

    int    dis_ticks = 0, chg_ticks = 0, dis_outside_ticks = 0;
    int    upper_dead_in = 0, upper_dead_out = 0;   // p_upper ≤ 0 的拍数
    int    in_ticks = 0, out_ticks = 0;
    int    gated_out = 0, hold_out = 0;
    double max_cmd_in   = -1e18, min_lower_in = 1e18;

    for (int i = 0; i < N; ++i) {
        const StepRecord rec = rig.app->step(dt);
        const DeviceLimits& dl = rig.app->rt().device_limits();
        const bool w = (i * dt >= 10.0 && i * dt < 20.0);

        if (dl.bms_chg_forbidden) ++chg_ticks;
        if (dl.bms_dis_forbidden) {
            ++dis_ticks;
            if (!w) ++dis_outside_ticks;
        }
        if (w) {
            ++in_ticks;
            if (rec.p_upper <= 1e-6) ++upper_dead_in;
            max_cmd_in   = std::max(max_cmd_in,   rec.p_cmd);
            min_lower_in = std::min(min_lower_in, rec.p_lower);
        } else {
            ++out_ticks;
            // 窗外"上界为 0"另有两条**设计内**路径，必须排除，否则差分判据没有
            // 区分度（它们与禁放位无关，是另外的机制）：
            //   · state_gated（非运行态）→ 区间收到 0，不许输出；
            //   · hold_last（采集超时保持）→ 区间**钉在上一拍指令**上（单点）。
            if (rec.state_gated)      ++gated_out;
            else if (rec.hold_last)   ++hold_out;
            else if (rec.p_upper <= 1e-6) ++upper_dead_out;
        }
    }

    // ① 反向守卫：设备侧真的每拍都在发（EMS 侧有 1 拍读取延迟，留容差）
    EXPECT(dis_ticks >= 90);
    // ② 位不串
    EXPECT(chg_ticks == 0);
    // ③ 窗外不残留（第 1 拍的边界过渡：设备侧在 t=19.9 写 1，EMS 在 t=20.0 才读到）
    EXPECT(dis_outside_ticks <= 2);
    // ④ 硬判据：禁放把**上界**收死 —— 窗内绝大多数拍 p_upper ≤ 0。
    //    用**计数**而不是 max/min：单拍边界过渡不会污染判据（列首/列尾各 1 拍）。
    EXPECT(upper_dead_in >= in_ticks - 3);
    // ⑤ 差分：窗外的**常规拍**上界没有被收死 —— 排除"本来上界就是 0"的假通过
    EXPECT(upper_dead_out <= 2);
    // ⑥ 硬判据：下发指令不放电
    EXPECT(max_cmd_in <= 1e-6);
    // ⑦ 语义：只压上界，充电方向仍开放（没被误写成双向锁死）
    EXPECT(min_lower_in < -1e-6);

    std::cerr << "       禁放拍数=" << dis_ticks << "（窗内 " << in_ticks
              << " 拍）/ 窗外残留=" << dis_outside_ticks
              << " / 窗内 p_upper≤0 拍数=" << upper_dead_in
              << " vs 窗外常规拍 " << upper_dead_out << "（共 " << out_ticks
              << " 拍，另有门控 " << gated_out << " / hold_last " << hold_out << " 拍）"
              << " / 窗内 p_cmd_max=" << max_cmd_in
              << " p_lower_min=" << min_lower_in << "\n";
}

// =====================================================================
// T49: 关口功率的**单一数据源** —— 电表（缺口 A2，2026-09-19）
//
// 契约：算法用的关口功率必须来自**电表读数**（`MEAS.P_GRID`），
//       而**不是** `P_load − P_pv − P_bat` 三路量测相减**推算**出来的。
//
// 为什么必须让夹具故意不一致：改前三个适配器都在 read_snapshot() 里推算，
//   而设备侧发布的 `MEAS.P_GRID` **恰好等于同一个式子** —— 两个口径逐位相同，
//   于是"到底读没读电表"在任何夹具上都**不可区分**，断言恒真（测不到东西）。
//   所以本用例给设备侧的电表加一个**系统偏差**（400 kW 负荷下 +40 kW ≈ 10%），
//   把两个口径拉开：
//     真读电表 → 算法看到的关口功率 = 三路平衡 + 40
//     继续推算 → 算法看到的关口功率 = 三路平衡（差整整 40 kW）
//   现场后果：防逆流（S05）与变压器过载约束拿 p_grid 当命门，
//   40 kW 的假偏差足以让它误动作或漏判倒送。
//
// 与 07/T25、07/T29 的差别：那两条在**适配器层**（单进程、夹具写点）；
//   本用例走**设备进程 → 共享内存 → EMS 每拍刷新**的真实路径。
// =====================================================================
static void test_49_grid_single_source(RtdbEnv& env) {
    std::cerr << "[T49] 关口功率单一数据源：设备侧电表读数（不用三路推算）...\n";
    EXPECT(env.ok);
    if (!env.ok) return;

    DeviceSimConfig plant;
    plant.meter_bias_kw = 40.0;      // 电表系统偏差：+40 kW（多报进口）

    EmsSideApp::Config cfg;
    cfg.dt_s      = 0.1;
    cfg.log_every = 1;
    IntegrationRig rig({}, cfg, plant);

    const double dt = 0.1;
    const int    N  = 200;           // 20 s

    int    ok_meter = 0, ok_algo = 0, ok_gap = 0;
    double min_gap = 1e18, max_gap = -1e18, max_err_algo = 0.0;

    for (int i = 0; i < N; ++i) {
        const StepRecord rec = rig.app->step(dt);

        RealtimeSnapshot snap;
        rig.app->io().read_snapshot(i * dt, snap);

        // 三路量测的"功率平衡" —— 正是**改前**算法拿到的那个数
        const double balance = snap.p_load_kw - snap.p_pv_kw - snap.p_bat_actual_kw;
        const double gap     = snap.p_grid_kw - balance;

        // ① 算法路径的关口功率 == 三路平衡 + 电表偏差 → 说明它读了电表
        if (std::fabs(snap.p_grid_kw - (balance + plant.meter_bias_kw)) < 1e-6) ++ok_meter;
        // ② 记录路径（read_actuals → StepRecord）与算法路径**同口径**。
        //    改前这两条路一条走推算、一条读电表 → 差整整一个 bias（本判据会红）。
        if (std::fabs(rec.p_grid - snap.p_grid_kw) < 1e-6) ++ok_algo;
        // ③ 两源确实被拉开了（不是"恰好相等所以怎么判都对"）
        if (std::fabs(gap - plant.meter_bias_kw) < 1e-6) ++ok_gap;

        min_gap = std::min(min_gap, gap);
        max_gap = std::max(max_gap, gap);
        max_err_algo = std::max(max_err_algo, std::fabs(rec.p_grid - snap.p_grid_kw));
    }

    // ① 反向守卫：偏差不为 0，这条用例才有区分度（否则恒真）
    EXPECT(std::fabs(plant.meter_bias_kw) > 10.0);
    // ② 硬判据：逐拍**全部**成立（不是"大部分成立"）
    EXPECT(ok_meter == N);      // 算法读的是电表
    EXPECT(ok_algo  == N);      // 两条路径同口径
    EXPECT(ok_gap   == N);      // 两源相差正好一个 bias
    // ③ 偏差是**恒定**的 40 kW，不是被平均掉的噪声
    EXPECT(std::fabs(min_gap - plant.meter_bias_kw) < 1e-6);
    EXPECT(std::fabs(max_gap - plant.meter_bias_kw) < 1e-6);
    // ④ 同口径的量化证据
    EXPECT(max_err_algo < 1e-9);

    std::cerr << "       电表偏差=" << plant.meter_bias_kw
              << " kW / 逐拍三路平衡差=[" << min_gap << ", " << max_gap << "]"
              << " / 两路径最大差=" << max_err_algo
              << " / 命中拍数=" << ok_meter << "/" << N << "\n";
}

// =====================================================================
int main() {
    std::cerr << "=========================================\n"
              << " 11/ 周期 11：系统级联调 单元测试（T41~T49）\n"
              << "=========================================\n";

    RtdbEnv env;
    if (!env.ok) {
        std::cerr << "  [环境] 共享内存初始化失败 —— RT_DB 段建不起来，"
                     "后续断言会整体失败\n";
    }

    test_41_comm_and_role_boundary(env);
    test_42_data_acquisition_stability(env);
    test_43_command_and_state_sync(env);
    test_44_fault_trigger_and_recovery(env);
    test_45_soak_24h(env);
    test_46_logging_end_to_end(env);
    test_47_concurrent_acquisition(env);
    test_48_bms_forbid_cross_process(env);
    test_49_grid_single_source(env);

    std::cerr << "=========================================\n"
              << " PASS=" << g_pass << "  FAIL=" << g_fail << "\n"
              << "=========================================\n";
    return (g_fail == 0) ? 0 : 1;
}
