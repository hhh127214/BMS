// =====================================================================
// 07/ —— 现场进程入口 main_field.cpp
//
//   定位：把「点表填好」这件事接到真实设备上。这是设备接入闭环的最后一块——
//   在这之前，平台能保存点表、13/ 能读到点表，但**控制进程起来仍然是仿真**。
//
//   为什么另起一个 main（而不是往 main.cpp 里加开关）：
//     本项目「一个进程一个 main」是既有惯例（13/main_probe.cpp、
//     P3/main_gateway.cpp 都如此）。main.cpp 是**演示程序**，加 --device 进去
//     会让"演示"与"现场运行"两种语义搅在一起，且演示程序的输出会被迫改变——
//     而"不传 --device 时行为与改造前一致"是本任务最硬的一条约束。
//
//   ★ 默认路径零变更（可执行判据，不是声称）：
//     不传 --device（或 --device sim）时，本程序调用 demo7::run_scenario7_demo()
//     —— 与 main.cpp / loop_demo.exe **同一份**代码（demo_scenario7.h）。
//     判据见 scripts/build_test_field.bat 的 T-F3：两程序输出直接 diff，
//     只允许 mean_cycle（每拍耗时，计时噪声）不同。
//
//   ★ 安全设计（现场第一次接进来的默认姿势必须是"只看不动"）：
//     · 默认 mode = 只读巡检：**不创建 EmsRuntime**，因此代码里根本不存在
//       能下发指令的路径。想下发必须显式 --control。
//     · 即使 --control，也先做一次「预读体检」：读不到可信数据就拒绝进入闭环
//       （除非 --force）。理由：连不上还把指令发出去，比连不上更糟。
//
//   ⚠ 已知缺口（诚实记录，勿当作已覆盖）：
//     · EmsRuntime 内部的 t_ 仍是**理想时间轴** i*dt，与真实墙钟会累积漂移。
//       --control 下本程序按墙钟 pacing（sleep_until），但 t_ 未换成墙钟基准。
//       长时间在线运行前必须先解决这个口径问题（已记入 #106 实时入库）。
//     · 现场策略参数（需量目标、变压器容量等）目前取代码默认值，未从配置读。
//       配置持久化属 19/ 的范围。
//
//   编译：见 scripts/build_field.bat
//     （需要 -I ../13/src 与 07/src/rtdb 的 ems_point_table.o + -lws2_32）
// =====================================================================

#include "field_args.h"          // 07/  命令行参数
#include "demo_scenario7.h"      // 07/  与 main.cpp 共用的演示体
#include "record_csv.h"          // 07/  现场实录 CSV 写出器（--record）

#include "device_io.h"           // 04/  IDeviceIO（算法只认这个接口）
#include "realtime_loop.h"       // 07/  控制闭环

#include "modbus_device_io.h"    // 13/  Modbus TCP 主站适配器
#include "modbus_point_map.h"    // 13/  点表加载与「来源」如实区分
#include "device_conn_conf.h"    // 13/  接入参数（配置通道②）

#include "meter645_device_io.h"  // 21/  DL/T645 主站适配器（只读关口电表）
#include "dlt645_tcp_link.h"     // 21/  TCP 透传链路

#include "rtdb_device_io.h"      // 07/src/rtdb  共享内存实时库适配器
#include "rtdb_mirror.h"         // 07/src/rtdb  采集值 → 实时库镜像（上端 104 的数据源）

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

using namespace ems;

namespace {

// ---------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------
double now_seconds() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Unix 墙钟秒（区别于 steady_clock 的单调秒）。
// --record 的 t_s 必须用这个：14/ live 导入按真实日期时间展示，
// 若用 steady_clock（从开机算起），时间戳会在每次重启后从 0 重新数，
// 与真实日期完全对不上。
double unix_now() {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

void banner(const std::string& s) {
    std::printf("\n============================================================\n");
    std::printf("  %s\n", s.c_str());
    std::printf("============================================================\n");
}

// 状态位压成一行。顺序刻意按「严重度」排：数据无效/离线在最前。
std::string status_brief(const DeviceStatus& s) {
    if (!s.data_valid)    return "数据无效";
    if (s.device_offline) return "设备离线";
    std::string r;
    if (s.pcs_fault)      r += "PCS故障;";
    if (!s.bms_comm_ok)   r += "BMS通信断;";
    if (!s.pcs_comm_ok)   r += "PCS通信断;";
    if (!s.meter_comm_ok) r += "电表通信断;";
    return r.empty() ? "正常" : r;
}

void print_limits(const DeviceLimits& l) {
    std::printf("  限值明细 : PCS 充/放 %.1f/%.1f kW | BMS %.1f/%.1f kW"
                " | 禁充=%s 禁放=%s | 变压器 %.1f kW | 需量 %.1f kW\n",
                l.pcs_rated_chg_kw, l.pcs_rated_dis_kw,
                l.bms_chg_limit_kw, l.bms_dis_limit_kw,
                l.bms_chg_forbidden ? "是" : "否",
                l.bms_dis_forbidden ? "是" : "否",
                l.transformer_capacity_kw, l.d_target_kw);
}

// ---------------------------------------------------------------------
// 只读巡检
//
//   ★ 本函数**不创建 EmsRuntime** —— 这是"不下发指令"的结构性保证，
//     不是靠 if 判断拦住的。runtime 不存在，就没有任何写设备的调用点。
// ---------------------------------------------------------------------
int drive_inspect(IDeviceIO& io, const field::Args& a, RtdbMirror* mir = nullptr) {
    const int steps = a.steps_or(10);
    std::printf("\n---- 只读巡检：%d 拍 × %.3f s ----\n", steps, a.dt);
    std::printf("  适配器   : %s\n", io.name());
    std::printf("  ★ 本模式不创建 EmsRuntime，代码里不存在能下发指令的路径。\n\n");

    // ---- 实录（--record）：只读巡检也写 ----
    //   ★ 2026-10-18 起才支持。此前 --record 只在 --control 分支里，
    //     于是"接上真实设备只看不动"跑得通、却**一行数据都不入库** ——
    //     平台侧页面全空，而现场看到的是一切的正常（读到了、数值对）。
    //     「接入后其他页面就该正常运行」这句话，卡的就是这一处。
    //   record 里的 state 记 INIT + reason 写明"只读巡检"：
    //     不能记成 RUNNING —— 那会让下游以为闭环在跑，而 p_cmd 恒为 0。
    record::RecordWriter rec;
    const bool recording = !a.record_path.empty();
    if (recording) {
        if (!rec.open(a.record_path)) {
            std::printf("\n[FAIL] 打不开实录文件 %s（检查目录是否存在、是否可写）\n",
                        a.record_path.c_str());
            return 3;
        }
        std::printf("  实录文件 : %s（20 列，与 10/ timeseries.csv 同构）\n",
                    a.record_path.c_str());
    }

    std::printf("  %-4s %10s %10s %10s %10s %8s  %s\n",
                "拍", "P_grid", "P_pv", "P_load", "P_bat", "SOC", "状态");

    RealtimeSnapshot snap;
    // 镜像用的限值缓存：read_limits() 会**真的发一次 Modbus 请求**，
    // 不能每拍调两次。每 10 拍刷一次（10 Hz 下 1 s）—— BMS 动态降功率
    // 是秒级事件，这个滞后足够，而通信开销只多 10%。
    DeviceLimits lim_cache;
    int ok_cnt = 0, invalid_cnt = 0, fail_streak = 0;
    const auto t_start = std::chrono::steady_clock::now();

    for (int i = 0; i < steps; ++i) {
        const bool r = io.read_snapshot(now_seconds(), snap);
        if (r) ++ok_cnt;
        const DeviceStatus st = io.read_status();
        if (!st.data_valid) ++invalid_cnt;

        // ★ 读失败就**不写实录**，连续 3 拍还没读到就退出。
        //   为什么：实录是平台的唯一数据来源。读失败照样写一行，
        //   写出来的是一行 0 值 —— 平台页面会显示"一切正常、负荷 0"，
        //   而现场其实一台设备都没连上。假数据比缺数据贵得多。
        fail_streak = r ? 0 : (fail_streak + 1);
        if (fail_streak >= 3) {
            std::printf("\n[FAIL] 连续 %d 拍没读到数据，退出（不写这些空拍）。\n", fail_streak);
            std::printf("  处置：① 设备是否上电、网线通不通（ping / 13\\build\\modbus_probe.exe）；\n");
            std::printf("        ② IP / 端口 / 从站号是否正确；\n");
            std::printf("        ③ 点表的表类型与地址是否与厂家手册一致。\n");
            rec.close();
            return 4;
        }

        std::printf("  %-4d %10.2f %10.2f %10.2f %10.2f %8.3f  %s\n",
                    i + 1, snap.p_grid_kw, snap.p_pv_kw, snap.p_load_kw,
                    snap.p_bat_actual_kw, snap.soc, status_brief(st).c_str());

        if (recording && r) {
            StepRecord row;
            row.t        = snap.timestamp > 0.0 ? snap.timestamp : now_seconds();
            row.state    = EmsState::kInit;   // 未进闭环
            row.p_load   = snap.p_load_kw;
            row.p_pv     = snap.p_pv_kw;
            row.p_grid   = snap.p_grid_kw;
            row.p_cmd    = 0.0;               // 只读：不存在下发值
            row.p_actual = snap.p_bat_actual_kw;
            row.soc      = snap.soc;
            row.temp     = snap.temperature_c;
            row.p_lower  = 0.0;
            row.p_upper  = 0.0;
            row.plan_target = 0.0;
            row.correction  = 0.0;
            row.clamped = row.safety_clip = row.state_gated = row.hold_last = false;
            int bits = 0;
            if (!st.bms_comm_ok)   bits |= 1;
            if (!st.pcs_comm_ok)   bits |= 2;
            if (!st.meter_comm_ok) bits |= 4;
            if (st.pcs_fault)      bits |= 8;
            if (st.device_offline) bits |= 16;
            row.fault_bits = bits;
            row.reason = "只读巡检（不下发指令）";
            rec.write(row, unix_now());
        }

        // ---- 实时库镜像（上端 IEC104 的数据源）----
        //   ★ 只在 r == true 时写：读失败还写一行 0，正是本文件一直在防的假数据。
        //   ★ cmd 传 nullptr：只读巡检不下发指令。写 CMD.P_BAT=0 会被调度
        //     读成"要求电池 0 kW"，与"没有指令"完全不是一回事。
        if (mir != nullptr && mir->is_open() && r) {
            if (i % 10 == 0) io.read_limits(lim_cache);
            RtdbMirror::Sample ms;
            ms.p_load = snap.p_load_kw;
            ms.p_pv   = snap.p_pv_kw;
            ms.p_bat  = snap.p_bat_actual_kw;
            ms.p_grid = snap.p_grid_kw;
            ms.soc    = snap.soc;
            ms.t_c    = snap.temperature_c;
            ms.soh    = snap.soh;
            mir->publish(ms, nullptr, st, lim_cache);
        }

        if (i == 0) {
            DeviceLimits lim;
            io.read_limits(lim);
            if (a.verbose) print_limits(lim);
            std::printf("  限值刷新 : %s\n",
                        io.limits_are_live() ? "每拍读设备（live 适配器）"
                                             : "否（限值来自装配配置）");
        }
        if (i + 1 < steps) {
            std::this_thread::sleep_until(
                t_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>((i + 1) * a.dt)));
        }
    }

    rec.close();

    std::printf("\n  采集成功 : %d/%d 拍；品质无效 %d 拍\n", ok_cnt, steps, invalid_cnt);
    if (mir != nullptr) {
        const RtdbMirror::Stats& ms = mir->stats();
        std::printf("  实时库镜像: %s —— %ld 拍 · 写入 %ld 点 · 失败 %ld\n",
                    mir->is_open() ? "已写入 MEAS/STA/CFG（不写 CMD）" : "未接通",
                    ms.steps, ms.wrote, ms.failed);
    }
    if (a.verbose) {
        const DeviceActuals ac = io.read_actuals();
        std::printf("  实际值   : P_bat %.2f kW | P_grid %.2f kW | SOC %.4f | 温度 %.2f ℃\n",
                    ac.p_bat_kw, ac.p_grid_kw, ac.soc, ac.temperature_c);
    }
    if (ok_cnt == 0) {
        std::printf("\n[FAIL] 一拍都没读到。检查设备是否在线、IP/端口/从站号是否正确，\n");
        std::printf("       以及点表的表类型与地址是否与厂家手册一致。\n");
        return 4;
    }
    std::printf("\n  结论：只读核对通过。确认上面的数值与现场实际一致后再加 --control。\n");
    return 0;
}

// ---------------------------------------------------------------------
// 闭环控制（会真的下发指令）
// ---------------------------------------------------------------------
int drive_control(IDeviceIO& io, const field::Args& a, RtdbMirror* mir = nullptr) {
    const int steps = a.steps_or(6000);

    // ---- 预读体检：读不到就不许下发 ----
    if (!a.force) {
        RealtimeSnapshot pre;
        const bool r = io.read_snapshot(now_seconds(), pre);
        const DeviceStatus st = io.read_status();
        std::printf("\n---- 预读体检 ----\n");
        std::printf("  读快照 : %s\n", r ? "成功" : "失败");
        std::printf("  品质位 : %s\n", st.data_valid ? "有效" : "无效");
        std::printf("  状态   : %s\n", status_brief(st).c_str());
        if (!r || !st.data_valid) {
            std::printf("\n[FAIL] 预读没拿到可信数据，**拒绝进入闭环**（一个指令都不会下发）。\n");
            std::printf("  为什么挡在这里：连不上还把指令发出去，是比连不上更糟的事——\n");
            std::printf("  指令一旦写进设备，执行机构就会动，而你还以为它在等数据。\n");
            std::printf("  处置：① 先去掉 --control 用只读模式确认能读到数据；\n");
            std::printf("        ② 确认 IP / 端口 / 从站号 / 点表 都正确；\n");
            std::printf("        ③ 确实要在无数据下空跑台架，再加 --force。\n");
            return 4;
        }
    }

    std::printf("\n---- 闭环控制：%d 拍 × %.3f s（按墙钟节拍）----\n", steps, a.dt);
    std::printf("  ★★ 本模式会向真实设备下发功率指令。适配器：%s\n", io.name());

    EmsRuntime rt;
    rt.attach_device(&io);            // ← 算法层不知道底下是 Modbus 还是 RT_DB
    rt.config().dt_s = a.dt;
    rt.apply_configs();

    if (a.verbose) {
        DeviceLimits lim;
        io.read_limits(lim);
        print_limits(lim);
        std::printf("  限值刷新 : %s\n",
                    io.limits_are_live() ? "每拍读设备（真机限值会变，如 BMS 动态降功率）"
                                         : "否（限值来自装配配置）");
    }

    // SOH 是慢变量（日变化 < 0.1%），StepRecord 里没有它 —— 启动时读一次。
    // 不每拍读：read_snapshot() 会真的发 Modbus 请求，为一天变不到 0.1% 的
    // 量付 10 Hz 的代价不值。
    double soh0 = 1.0;
    if (mir != nullptr && mir->is_open()) {
        RealtimeSnapshot pre_soh;
        if (io.read_snapshot(now_seconds(), pre_soh)) soh0 = pre_soh.soh;
        mir->set_soh(soh0);
    }

    const auto t_start = std::chrono::steady_clock::now();

    // ---- 现场实录（--record）：边跑边把每拍追加写 CSV ----
    //   列格式与 10/ 的 timeseries.csv 同构（20 列），但 t_s = Unix 墙钟秒、
    //   time = 完整日期时间（口径差异见 record_csv.h 顶部）。
    record::RecordWriter rec;
    if (!a.record_path.empty()) {
        if (!rec.open(a.record_path)) {
            std::printf("\n[FAIL] 打不开实录文件 %s（检查目录是否存在、是否可写）\n",
                        a.record_path.c_str());
            return 3;
        }
        std::printf("  实录文件 : %s（20 列，与 10/ timeseries.csv 同构）\n",
                    a.record_path.c_str());
    }

    // ---- 显式单循环（原来分 rt.run / --record 两条）----
    //   为什么合并：镜像与实录都取"这一拍的 StepRecord"，而 rt.run 的 hook
    //   在 step() **之前**调用，拿不到本拍结果。两条路径本来就用同一套
    //   sleep_until pacing，合并后连"第一拍前要不要空等一个 dt"也统一了
    //   （原来 run 路径会先空等一个 dt，record 路径不会）。
    for (int i = 0; i < steps; ++i) {
        const StepRecord r = rt.step(a.dt);
        if (!a.record_path.empty()) rec.write(r, unix_now());

        // ---- 实时库镜像：MEAS/STA/CFG + CMD ----
        //   这里写 CMD 不算越区：本进程同时兼任采集与控制，
        //   r.p_cmd / p_upper / p_lower 就是 EMS 这一拍的决定。
        if (mir != nullptr && mir->is_open()) {
            RtdbMirror::Sample ms;
            ms.p_load = r.p_load;
            ms.p_pv   = r.p_pv;
            ms.p_bat  = r.p_actual;
            ms.p_grid = r.p_grid;
            ms.soc    = r.soc;
            ms.t_c    = r.temp;
            ms.soh    = soh0;

            RtdbMirror::Command mc;
            mc.p_bat   = r.p_cmd;
            mc.p_upper = r.p_upper;
            mc.p_lower = r.p_lower;

            mir->publish(ms, &mc, io.read_status(), rt.device_limits());
        }

        if (i + 1 < steps) {
            std::this_thread::sleep_until(
                t_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>((i + 1) * a.dt)));
        }
    }
    rec.close();

    std::printf("\n  闭环指标：%s\n", rt.metrics().to_string().c_str());
    if (mir != nullptr) {
        const RtdbMirror::Stats& ms = mir->stats();
        std::printf("  实时库镜像: %s —— %ld 拍 · 写入 %ld 点 · 失败 %ld\n",
                    mir->is_open() ? "已写入 MEAS/STA/CFG + CMD" : "未接通",
                    ms.steps, ms.wrote, ms.failed);
    }
    return 0;
}

// ---------------------------------------------------------------------
// 分支：Modbus TCP（EMS 为主站）
// ---------------------------------------------------------------------
int run_modbus(const field::Args& a) {
    // ---- 点表：显式 --map > 约定路径 > 内置默认表 ----
    std::string rep, used;
    modbus::MapSource src = modbus::MapSource::kBuiltin;

    if (!a.map_path.empty()) {
        if (!modbus::load_point_map_csv(a.map_path.c_str(), &rep)) {
            std::printf("\n[FAIL] 点表加载失败（--map %s）\n  原因：%s\n",
                        a.map_path.c_str(), rep.c_str());
            return 3;
        }
        src  = modbus::MapSource::kExplicit;
        used = a.map_path;
    } else if (!modbus::load_point_map_default(&rep, &src, &used)) {
        std::printf("\n[FAIL] 约定路径下的点表**非法**，拒绝启动。\n");
        std::printf("  原因：%s\n", rep.c_str());
        std::printf("  ★ 这里故意不退回内置表：否则「平台把表写错了」与「平台还没写表」\n");
        std::printf("    在现场表现完全一样，而前者会让人误以为配置已经生效。\n");
        std::printf("  处置：在 15/ 界面上重新保存一次点表，或用 --map 显式指定正确的文件。\n");
        return 3;
    }

    std::printf("  点表来源 : %s\n", modbus::map_source_name(src));
    if (!used.empty()) std::printf("  点表文件 : %s\n", used.c_str());
    std::printf("  点表点数 : %llu\n",
                static_cast<unsigned long long>(modbus::active_map().count));
    std::printf("  设备地址 : %s:%d  从站号 %d\n", a.host.c_str(), a.port, a.unit);

    ModbusDeviceIO::Config cfg;
    cfg.host       = a.host;
    cfg.port       = static_cast<std::uint16_t>(a.port);
    cfg.unit_id    = static_cast<std::uint8_t>(a.unit);
    cfg.timeout_ms = a.timeout_ms;   // 来自 --timeout（或接入参数文件的 timeout_ms）
    ModbusDeviceIO io(cfg);

    // ---- 实时库镜像（--mirror-rtdb）----
    //   为什么放在这里、而不是在 simproc 里再起一个写入进程：
    //   RT_DB 的纪律是「一个进程一条连接」，同一个值也只该有一个写入者。
    //   让采集进程顺手发布，就不存在「两个进程写同一批点、谁后写谁赢」。
    RtdbMirror mir;
    RtdbMirror* m = nullptr;
    if (a.mirror_rtdb) {
        std::string why;
        if (mir.open(&why)) {
            m = &mir;
            std::printf("  实时库镜像: 已接通（写 MEAS/STA/CFG%s）\n",
                        a.control ? " + CMD" : "，不写 CMD");
        } else {
            // 不阻断采集：现场"先起采集、后起实时库初始化器"是常见顺序。
            // 但必须喊出来 —— 否则上端 104 一直发初值，而链路和点表都看着正常。
            std::printf("\n[WARN] 实时库镜像未接通：%s\n", why.c_str());
            std::printf("  影响：上端 IEC104 的 MEAS.* 会停在初值（0 / 50%% / 25℃），\n");
            std::printf("        而链路与点表看起来都正常。\n");
            std::printf("  处置：确认 rtdb_initializer.exe 已常驻，再重启本进程。\n\n");
        }
    }

    return a.control ? drive_control(io, a, m) : drive_inspect(io, a, m);
}

// ---------------------------------------------------------------------
// 分支：DL/T645-2007 电表（EMS 为主站，经串口服务器 TCP 透传）
//
//   ★ 只读通道：645 电表没有"接收功率指令"的语义，--control 在
//     field_args.h 的解析层已被拒，这里**必然**走 drive_inspect。
//     这一点不靠"运行到 drive_control 里才写失败"来拦 —— 到那时
//     现场看到的是"写失败"，像通信故障，其实是装错了通道。
//
//   ★ 点表不是 CSV：645 的 EMS 点 ↔ DI 绑定是代码常量
//     （21/src/meter645_point_map.h，默认只有 EMS_P_GRID ← 0x02030000），
//     因为一块电表能答出什么由协议附录 A.2 决定，不是现场能"配"出来的。
//     启动时先跑 self_check_map，把"绑定表被改坏"拦在连接之前。
// ---------------------------------------------------------------------
int run_dlt645(const field::Args& a) {
    const meter645::MapCheck mc = meter645::self_check_map();
    std::printf("  绑定自检 : %s（检查了 %d 行）\n",
                mc.total == 0 ? "通过" : "**不一致**", mc.checked);
    if (mc.total != 0) {
        std::printf("\n[FAIL] 645 点表绑定自检未通过：索引越界=%d 点名不符=%d "
                    "DI未知=%d 重复绑定=%d\n"
                    "  绑定表是代码常量（meter645_point_map.h），它坏了说明 21/ 或\n"
                    "  07/ 的点表被改过而映射没跟上 —— 先修表再连现场。\n",
                    mc.index_out_of_range, mc.name_mismatch,
                    mc.di_unknown, mc.dup_index);
        return 3;
    }

    dlt645::Address meter;
    if (!meter.from_string(a.meter_addr)) {
        // 解析层已校验"12 位数字"，走到这里说明校验被绕过 —— 按编程错误处理。
        std::printf("\n[FAIL] 电表地址「%s」无法解析（应为 12 位十进制数字）\n",
                    a.meter_addr.c_str());
        return 3;
    }

    std::printf("  透传网关 : %s:%d\n", a.host.c_str(), a.port);
    std::printf("  电表地址 : %s\n", meter.to_string().c_str());
    std::printf("  绑定关系 : EMS_P_GRID ← DI 0x02030000（瞬时总有功功率）\n");
    std::printf("  ★ 本通道只有关口功率一路量测；快照里 SOC/P_BAT 恒 0\n");
    std::printf("    （= 没有值，不是读到 0），有效性见状态行的「电表通信」位。\n");

    dlt645::TcpLink link(a.host, static_cast<unsigned short>(a.port));
    if (!link.open()) {
        std::printf("\n[FAIL] 连不上透传网关 %s:%d。\n", a.host.c_str(), a.port);
        std::printf("  处置：① 网关是否上电、IP/端口是否正确；\n");
        std::printf("        ② 想先验证程序本身，用 21\\build\\sim_tcp_server.exe 起一台仿真表。\n");
        return 4;
    }

    dlt645::MasterOptions opt;
    opt.timeout_ms = a.timeout_ms;
    Meter645DeviceIO io(&link, meter, opt);

    // 只读巡检（结构保证：control 在解析层已被拒）
    return drive_inspect(io, a);
}

// ---------------------------------------------------------------------
// 分支：共享内存实时库
// ---------------------------------------------------------------------
int run_rtdb(const field::Args& a) {
    RtDbDeviceIO io;
    if (!io.open_owned(nullptr)) {
        std::printf("\n[FAIL] 打不开共享内存实时库（RT_DB）。\n");
        std::printf("  可能原因：① 段还没建 —— 先跑一次 07\\build\\ems_rt_db_init.exe\n");
        std::printf("           ② 设备侧采集进程没在跑（段由先创建者建立）\n");
        std::printf("           ③ 权限不足（Windows 下需同一登录会话）\n");
        return 3;
    }

    const int bad = io.self_check();
    std::printf("  点表自检 : 共享内存 vs 编译期点表 —— %s\n",
                bad == 0 ? "完全一致"
                         : ("**不一致 " + std::to_string(bad) + " 点**").c_str());
    if (bad != 0) {
        std::printf("  ★ 不一致意味着：设备侧写的点与算法读的点**错位**，读到的值会整体偏。\n");
        std::printf("    先别急着控制 —— 核对两边的点表版本（07/src/rtdb/ems_point_table.h）。\n");
    }

    return a.control ? drive_control(io, a) : drive_inspect(io, a);
}

}  // namespace

// ---------------------------------------------------------------------
// 接入参数（配置通道②）：命令行没给目标时，读约定路径的 active.conn
//
// 为什么要有这一步：客户在 15/ 界面上填完 IP，如果还要人工把同一串参数
// 再敲一遍到命令行，那么"界面写的"和"实际连的"就是两份可以互相不一致的
// 真相 —— 而两边都不会报错。让端侧直接读平台写下的那份，两边就只有一个来源。
//
// 返回 0 = 可以继续；非 0 = 已打印原因，直接以该值退出。
// ---------------------------------------------------------------------
int resolve_modbus_target(field::Args& a) {
    if (a.device != field::DeviceKind::kModbus) return 0;

    if (!a.host.empty()) {
        std::printf("  接入参数 : 命令行指定（%s:%d 从站 %d）\n",
                    a.host.c_str(), a.port, a.unit);
        return 0;
    }

    modbus::DeviceConn c;
    std::string rep, used;
    const modbus::ConnLoad s = modbus::load_device_conn_default(&c, &rep, &used);

    if (s == modbus::ConnLoad::kInvalid) {
        std::printf("\n[FAIL] 接入参数文件**非法**，拒绝启动：\n"
                    "  %s\n"
                    "  ★ 这里**故意不退回默认地址** —— 静默回退会让\n"
                    "    「平台把参数写错了」与「平台还没写参数」表现一致，\n"
                    "    而前者会让人以为配置生效了、实际连的是另一台设备。\n"
                    "  处置：修正 %s，或在 15/ 界面重新保存接入参数，\n"
                    "        或用 --host 显式指定目标。\n",
                    rep.c_str(), used.c_str());
        return 3;
    }

    if (s == modbus::ConnLoad::kMissing) {
        std::printf("\n[FAIL] --device modbus 需要一个目标地址，但两处都没有给：\n"
                    "  ① 命令行 --host <设备IP>\n"
                    "  ② 接入参数文件（%s —— 当前不存在）\n"
                    "  处置：在 15/ 界面「设备接入配置」里填好 IP / 端口 / 从站号并保存，\n"
                    "        平台会原子写出上面那个文件；或用 --host 显式指定。\n"
                    "  想先探一下端口通不通：\n"
                    "        13\\build\\modbus_probe.exe --host <ip> --port <n>\n",
                    used.c_str());
        return 3;
    }

    if (!c.enabled) {
        std::printf("\n[FAIL] 平台把该设备标记为**未启用**（enabled=0），拒绝启动：\n"
                    "  %s\n  %s\n"
                    "  ★ 不自动降级去连它 —— 「未启用」是一个明确的意图，\n"
                    "    不该被「命令行没给参数」悄悄改写。\n"
                    "  处置：在 15/ 界面启用它，或用 --host 显式指定目标。\n",
                    used.c_str(), modbus::format_device_conn(c).c_str());
        return 3;
    }

    // ★ 用 conn_target_host/port()，**不要**用 c.host / c.port。
    //   v1.5 给接入参数加了「形态」（real/sim），形态决定实际连哪个地址：
    //     real → host:port（现场设备）   sim → sim_host:sim_port（设备替身）
    //   这里原来直接取 c.host —— 于是平台把形态切成 sim 之后，
    //   `13/` 的探针连的是替身、`07/` 的主站却还在连真机地址：
    //   两处各解析一次，静默漂移，而两边都不报错。
    a.host       = modbus::conn_target_host(c);
    a.port       = modbus::conn_target_port(c);
    a.unit       = c.unit_id;
    a.timeout_ms = c.timeout_ms;
    std::printf("  接入参数 : 约定路径 ← %s\n", used.c_str());
    std::printf("             %s\n", modbus::format_device_conn(c).c_str());
    if (modbus::conn_is_sim(c)) {
        std::printf("             ★ 形态是模拟源：本进程连的是**设备替身**，"
                    "不是现场装置（已登记的真实地址 %s:%d 本次不用）\n",
                    c.host.c_str(), c.port);
    }
    // 平台配的 poll_period_ms 是**采样节奏**，本进程的 --dt 是**控制周期** ——
    // 两者语义不同（多久采一次 vs 多久算一次），所以这里只报出来，不拿它改 --dt。
    std::printf("             （平台配的采样节奏 %d ms 仅作参考，控制周期仍由 --dt 决定）\n",
                c.poll_period_ms);
    return 0;
}

// =====================================================================
int main(int argc, char** argv) {
    field::Args a;
    std::string err;
    const char* exe = (argc > 0) ? argv[0] : nullptr;

    if (!field::parse_args(argc, argv, a, err)) {
        std::printf("[FAIL] 参数错误：%s\n\n", err.c_str());
        std::printf("%s", field::usage_text(exe).c_str());
        return 2;
    }
    if (a.help) {
        std::printf("%s", field::usage_text(exe).c_str());
        return 0;
    }

    // ★★ 默认路径：与演示程序**同一份代码**，输出逐字节一致。
    //    这是「不传 --device 时行为零变更」的落点，改动前请先看 demo_scenario7.h 顶部。
    if (a.device == field::DeviceKind::kSim) {
        return demo7::run_scenario7_demo();
    }

    banner("07/ 现场进程 —— 设备接入");
    std::printf("  设备来源 : %s\n", field::device_kind_name(a.device));
    std::printf("  运行模式 : %s\n", a.control
                    ? "闭环控制（★ 会向设备下发功率指令）"
                    : "只读巡检（不下发任何指令）");
    std::printf("  控制周期 : %.3f s   拍数：%d\n", a.dt, a.steps_or(a.control ? 6000 : 10));

    // 先补接入参数（可能填上 host/port/unit），再校验目标齐不齐
    const int rc = resolve_modbus_target(a);
    if (rc != 0) return rc;
    if (!field::validate_target(a, err)) {
        std::printf("\n[FAIL] %s\n", err.c_str());
        return 2;
    }

    if (a.device == field::DeviceKind::kModbus) return run_modbus(a);
    if (a.device == field::DeviceKind::kDlt645) return run_dlt645(a);
    return run_rtdb(a);
}
