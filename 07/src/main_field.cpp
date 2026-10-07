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

#include "rtdb_device_io.h"      // 07/src/rtdb  共享内存实时库适配器

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
int drive_inspect(IDeviceIO& io, const field::Args& a) {
    const int steps = a.steps_or(10);
    std::printf("\n---- 只读巡检：%d 拍 × %.3f s ----\n", steps, a.dt);
    std::printf("  适配器   : %s\n", io.name());
    std::printf("  ★ 本模式不创建 EmsRuntime，代码里不存在能下发指令的路径。\n\n");

    std::printf("  %-4s %10s %10s %10s %10s %8s  %s\n",
                "拍", "P_grid", "P_pv", "P_load", "P_bat", "SOC", "状态");

    RealtimeSnapshot snap;
    int ok_cnt = 0, invalid_cnt = 0;
    const auto t_start = std::chrono::steady_clock::now();

    for (int i = 0; i < steps; ++i) {
        const bool r = io.read_snapshot(now_seconds(), snap);
        if (r) ++ok_cnt;
        const DeviceStatus st = io.read_status();
        if (!st.data_valid) ++invalid_cnt;

        std::printf("  %-4d %10.2f %10.2f %10.2f %10.2f %8.3f  %s\n",
                    i + 1, snap.p_grid_kw, snap.p_pv_kw, snap.p_load_kw,
                    snap.p_bat_actual_kw, snap.soc, status_brief(st).c_str());

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

    std::printf("\n  采集成功 : %d/%d 拍；品质无效 %d 拍\n", ok_cnt, steps, invalid_cnt);
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
int drive_control(IDeviceIO& io, const field::Args& a) {
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

    if (a.record_path.empty()) {
        // 原路径：rt.run + hook 只做墙钟 pacing。改动前请先看 demo_scenario7.h 顶部。
        rt.run(steps, a.dt, [&](EmsRuntime&, int i) {
            if (i + 1 < steps) {
                std::this_thread::sleep_until(
                    t_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double>((i + 1) * a.dt)));
            }
        });
    } else {
        // --record 路径：显式循环，拿到 step() 的 StepRecord 写盘。
        //   pacing 与上面完全一致（同一套 sleep_until），只是多了写文件。
        for (int i = 0; i < steps; ++i) {
            const StepRecord r = rt.step(a.dt);
            rec.write(r, unix_now());
            if (i + 1 < steps) {
                std::this_thread::sleep_until(
                    t_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double>((i + 1) * a.dt)));
            }
        }
    }
    rec.close();

    std::printf("\n  闭环指标：%s\n", rt.metrics().to_string().c_str());
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

    return a.control ? drive_control(io, a) : drive_inspect(io, a);
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

    a.host       = c.host;
    a.port       = c.port;
    a.unit       = c.unit_id;
    a.timeout_ms = c.timeout_ms;
    std::printf("  接入参数 : 约定路径 ← %s\n", used.c_str());
    std::printf("             %s\n", modbus::format_device_conn(c).c_str());
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
    return run_rtdb(a);
}
