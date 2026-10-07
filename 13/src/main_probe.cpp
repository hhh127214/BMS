// =====================================================================
// 13/ — Modbus 点表核对工具 modbus_probe.exe（**现场工具，不是测试**）
//
// 用途：接真机时第一件要做的事 —— 把 32 个点的**实际值**读回来，
//       与设备说明书逐点核对。现场点表与我们的映射表不一致是常态
//       （厂家改了固件、点表版本不对、地址段挪了），
//       而这件事**只能靠读到实际值来判断**，看代码看不出来。
//
// 用法：
//   modbus_probe.exe                       # 连 127.0.0.1:502 unit=1，读一遍
//   modbus_probe.exe --host 192.168.1.10 --port 502 --unit 3
//   modbus_probe.exe --repeat 10 --interval 1.0     # 每 1 s 读一遍，共 10 遍
//   modbus_probe.exe --plan                # 只打印分块读计划（不发报文）
//   modbus_probe.exe --set-power -50       # 下发指令（会先读回限值）
//   modbus_probe.exe --verbose             # 打印每次请求的耗时
//
// 输出里 `ok` 列的含义（**本工具最重要的一列**）：
//   ok   = 该点本拍可信（所在分块读成功 **且** 该点解码成功）
//   --   = 不可信。记住 Modbus 没有品质位，这一列是**主站合成**的结论；
//          值为 0 且 ok=false 与"真的是 0"，在协议上完全一样。
//
// 编译：见 13/scripts/build.bat
// =====================================================================

#include "modbus_device_io.h"
#include "modbus_point_map.h"
#include "device_conn_conf.h"     // 13/  接入参数（配置通道②）

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using namespace ems;
using namespace ems::modbus;

namespace {

void usage() {
    std::printf(
        "用法: modbus_probe.exe [选项]\n"
        "  --host H          设备地址（不给时读约定路径的接入参数文件）\n"
        "  --port N          TCP 端口（默认 502）\n"
        "  --unit N          从站地址（默认 1）\n"
        "  --timeout MS      单次请求超时（默认 1000）\n"
        "  --repeat N        重复读 N 遍（默认 1）\n"
        "  --interval S      两遍之间间隔秒（默认 0.5）\n"
        "  --plan            只打印分块读计划后退出\n"
        "  --load-map FILE   从 CSV 加载点表（覆盖内置默认表；现场点表走这个）\n"
        "  --dump-map        把当前生效的点表按 CSV 打印后退出（可直接当模板）\n"
        "  --conn FILE       显式指定接入参数文件（默认按约定路径找 active.conn）\n"
        "  --dump-conn       打印当前生效的接入参数后退出（不发报文）\n"
        "  --set-power KW    下发电池功率指令（先读限值，再写 [p, upper, lower]）\n"
        "\n"
        "连接目标的来源（优先级从高到低）：\n"
        "  1. 命令行 --host / --port / --unit\n"
        "  2. 约定路径的接入参数文件 active.conn（配置通道②）：\n"
        "       依次找 $EMS_POINT_MAP_DIR\\active.conn，\n"
        "           或 <exe 上级两级>\\config\\point-map\\active.conn\n"
        "       —— 这个文件由 14/ 平台在界面上保存接入参数时原子写出\n"
        "  3. 都没有 → 内置默认 127.0.0.1:502 从站 1（接本地仿真从站用）\n"
        "  文件存在但非法 → **直接失败**，不退回默认（静默回退会让\n"
        "  「平台写错了参数」与「平台没写参数」表现一致）。\n"
        "  --verbose         打印请求耗时与诊断计数\n");
}

// 打印一个点的值。用点的**编码方式**决定小数位，避免把 u16 也印成浮点。
void print_point(const ModbusDeviceIO& io, std::size_t i) {
    const PointBinding& b = binding_for_index(i);
    const double v = io.cached(i);
    const bool ok = io.point_valid(i);
    char num[32];
    if (b.encoding == Encoding::kF32) {
        std::snprintf(num, sizeof(num), "%12.4f", v);
    } else if (b.encoding == Encoding::kI16) {
        std::snprintf(num, sizeof(num), "%12.4f", v);
    } else if (b.encoding == Encoding::kU16) {
        std::snprintf(num, sizeof(num), "%12.6f", v);
    } else {
        std::snprintf(num, sizeof(num), "%12s", (v > 0.5) ? "1 (true)" : "0 (false)");
    }
    std::printf("  [%2zu] %-24s %s[%2u] %-3s  %s  %s\n",
                i, b.name, (b.table == Table::kInputReg) ? "IR"
                          : (b.table == Table::kHoldingReg) ? "HR"
                          : (b.table == Table::kDiscreteInput) ? "DI" : "CO",
                static_cast<unsigned>(b.address),
                (b.encoding == Encoding::kF32) ? "f32"
                        : (b.encoding == Encoding::kU16) ? "u16"
                        : (b.encoding == Encoding::kI16) ? "i16" : "bit",
                num, ok ? "ok" : "--");
}

void print_plan() {
    const ReadBlock*  blocks = active_blocks();
    const std::size_t nBlk   = active_block_count();
    std::printf("分块读计划（%zu 次请求读全 %zu 点）：\n",
                full_scan_request_count(), active_binding_count());
    for (std::size_t k = 0; k < nBlk; ++k) {
        const ReadBlock& b = blocks[k];
        const char* t = (b.table == Table::kInputReg) ? "输入寄存器 IR (FC04)"
                      : (b.table == Table::kHoldingReg) ? "保持寄存器 HR (FC03)"
                      : (b.table == Table::kDiscreteInput) ? "离散输入 DI (FC02)" : "线圈 CO (FC01)";
        std::printf("  #%zu  %-22s 地址 %u .. %u  （%u 个）\n",
                    k + 1, t, static_cast<unsigned>(b.start),
                    static_cast<unsigned>(b.start + b.count - 1),
                    static_cast<unsigned>(b.count));
    }
    std::printf("\n★ 这个请求次数是**契约**不是实现细节：\n"
                "  一次 Modbus 事务只能读一段连续地址；点表一散，\n"
                "  采一帧的耗时就会随点数线性增长（现场\"采一帧要 3 秒\"多半是这个）。\n"
                "  内置默认表是 3 次；加载现场点表后，这个数字由地址排布决定。\n");
}

} // namespace

int main(int argc, char** argv) {
    ModbusDeviceIO::Config cfg;
    int repeat = 1;
    double interval_s = 0.5;
    bool plan_only = false;
    bool verbose = false;
    bool set_power = false;
    bool dump_map = false;
    bool dump_conn = false;
    double power_kw = 0.0;
    std::string load_map_path;
    std::string conn_path_arg;
    // 命令行**显式**给过的连接参数。没给的才允许由接入参数文件补 ——
    // 命令行优先是刻意的：现场临时核对一台设备时不该被平台配置挡住。
    bool host_given = false, port_given = false, unit_given = false,
         timeout_given = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* def) -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : std::string(def);
        };
        if (a == "--host")          { cfg.host = next("127.0.0.1"); host_given = true; }
        else if (a == "--port")     { cfg.port = static_cast<std::uint16_t>(std::atoi(next("502").c_str())); port_given = true; }
        else if (a == "--unit")     { cfg.unit_id = static_cast<std::uint8_t>(std::atoi(next("1").c_str())); unit_given = true; }
        else if (a == "--timeout")  { cfg.timeout_ms = std::atoi(next("1000").c_str()); timeout_given = true; }
        else if (a == "--repeat")   repeat = std::atoi(next("1").c_str());
        else if (a == "--interval") interval_s = std::atof(next("0.5").c_str());
        else if (a == "--plan")     plan_only = true;
        else if (a == "--load-map") load_map_path = next("");
        else if (a == "--dump-map") dump_map = true;
        else if (a == "--conn")     conn_path_arg = next("");
        else if (a == "--dump-conn") dump_conn = true;
        else if (a == "--verbose")  verbose = true;
        else if (a == "--set-power") { set_power = true; power_kw = std::atof(next("0").c_str()); }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::printf("未知参数: %s\n\n", a.c_str()); usage(); return 2; }
    }

    std::printf("=== 13/ Modbus 点表核对工具 ===\n\n");

    // ★ 点表加载必须发生在**任何**自检与打印之前：加载失败时活动表仍是默认表，
    //   那之后所有输出都是"内置表的结论"，与现场设备无关 —— 而屏幕上看起来
    //   一切正常。这正是本项目最忌讳的静默错误，所以失败时直接停下。
    MapSource    src = MapSource::kBuiltin;
    std::string  used_path;
    if (!load_map_path.empty()) {
        std::string rep;
        if (load_point_map_csv(load_map_path.c_str(), &rep)) {
            src       = MapSource::kExplicit;
            used_path = load_map_path;
            std::printf("点表加载   : 成功 ← %s\n", load_map_path.c_str());
        } else {
            std::printf("[FAIL] 点表加载失败（活动表仍为内置默认表，未被改动）：\n"
                        "       %s\n"
                        "       修正 CSV 后重跑；不加 --load-map 时改为读约定路径。\n",
                        rep.c_str());
            return 2;
        }
    } else {
        // 没给 --load-map：按约定路径找平台下发的点表（见 modbus_point_map.h）
        std::string rep;
        if (!load_point_map_default(&rep, &src, &used_path)) {
            std::printf("[FAIL] %s\n"
                        "       ★ 这里**故意不退回内置表**。约定路径上有一份非法\n"
                        "         点表时静默回退，会让「平台写错了表」和「平台没写表」\n"
                        "         在现场表现完全一样 —— 而前者会让人以为配置已生效，\n"
                        "         实际跑在另一张表上。\n"
                        "       处置：修正该文件，或在平台上重新保存，\n"
                        "             或显式用 --load-map 指定另一份表。\n",
                        rep.c_str());
            return 2;
        }
        if (src == MapSource::kDefaultPath) {
            std::printf("点表加载   : 成功 ← %s\n", used_path.c_str());
        }
    }

    std::printf("点表来源   : %s\n", map_source_name(src));
    if (src != MapSource::kBuiltin) {
        std::printf("点表文件   : %s\n", used_path.c_str());
    }

    // 自检先跑：映射表 ↔ 点表逐字比对。配置错误在这里报，
    // 而不是等读到一堆 0 之后再去猜。
    const int sc = self_check();
    std::printf("映射表自检 : %s（%zu 点，分块 %zu 次）\n",
                sc == 0 ? "通过" : "**失败**", mapped_point_count(),
                full_scan_request_count());

    if (dump_map) {
        std::printf("\n");
        std::printf("%s", export_point_map_csv(active_point_map()).c_str());
        return sc == 0 ? 0 : 1;
    }

    if (plan_only) {
        std::printf("\n");
        print_plan();
        return sc == 0 ? 0 : 1;
    }

    // =================================================================
    // 接入参数（配置通道②）
    //
    // 优先级：命令行 --host > 约定路径的 active.conn > 内置默认 127.0.0.1。
    // 三段**分别报出来**（就像点表报「内置默认表 / 约定路径 / 命令行指定」）——
    // 现场第一件事就是确认"我现在到底在跟谁说话"，而这恰恰是命令行参数
    // 最容易记错的一环。
    // =================================================================
    DeviceConn sc_conn;
    std::string  conn_used_path;
    std::string  conn_why;
    ConnLoad     conn_load = ConnLoad::kMissing;

    if (!conn_path_arg.empty()) {
        // --conn 收的是**完整文件路径**（可以是任意文件名，不限于 active.conn）
        conn_used_path = conn_path_arg;
        conn_load = load_device_conn_path(conn_path_arg, &sc_conn, &conn_why);
        if (conn_load == ConnLoad::kMissing) {
            // --conn 是显式要求，文件不在就是错误（不静默退回默认）
            std::printf("[FAIL] --conn 指定的接入参数文件不存在：%s\n",
                        conn_path_arg.c_str());
            return 2;
        }
    } else {
        conn_load = load_device_conn_default(&sc_conn, &conn_why, &conn_used_path);
    }

    // ★ kInvalid 对两条来源**一律硬失败**（不能只挡约定路径那条）。
    //   漏掉 --conn 这条的后果是一样的：非法文件被静默忽略、程序拿内置默认
    //   127.0.0.1 继续连 —— 屏幕上一切正常，实际连的是另一台设备。
    if (conn_load == ConnLoad::kInvalid) {
        std::printf("[FAIL] 接入参数文件**非法**，拒绝启动：\n"
                    "       %s\n"
                    "       ★ 这里**故意不退回默认地址**。静默回退会让\n"
                    "         「平台把参数写错了」与「平台还没写参数」在现场表现一致——\n"
                    "         而前者会让人以为配置已生效，实际连的是另一台设备。\n"
                    "       处置：修正 %s，或在 15/ 界面上重新保存接入参数，\n"
                    "             或用 --host 显式指定目标。\n",
                    conn_why.c_str(), conn_used_path.c_str());
        return 2;
    }

    if (conn_load == ConnLoad::kOk && host_given) {
        // 命令行优先是刻意的：现场临时核对一台设备，不该被平台配置挡住
        std::printf("接入参数   : 命令行指定（接入参数文件已读取但未采用）\n"
                    "             %s\n"
                    "             %s\n",
                    conn_used_path.c_str(),
                    format_device_conn(sc_conn).c_str());
    } else if (conn_load == ConnLoad::kOk && !sc_conn.enabled) {
        std::printf("[FAIL] 平台把该设备标记为**未启用**（enabled=0）：\n"
                    "             %s\n"
                    "             %s\n"
                    "       处置：在 15/ 界面上启用它，或用 --host 显式指定目标。\n"
                    "       ★ 不自动降级去连它 —— 「未启用」是一个明确的意图，\n"
                    "         不该被「命令行没给参数」悄悄改写。\n",
                    conn_used_path.c_str(), format_device_conn(sc_conn).c_str());
        return 2;
    } else if (conn_load == ConnLoad::kOk) {
        if (!port_given)    cfg.port       = static_cast<std::uint16_t>(sc_conn.port);
        if (!unit_given)    cfg.unit_id    = static_cast<std::uint8_t>(sc_conn.unit_id);
        if (!timeout_given) cfg.timeout_ms = sc_conn.timeout_ms;
        cfg.host           = sc_conn.host;
        cfg.auto_reconnect = sc_conn.auto_reconnect;
        std::printf("接入参数   : 约定路径 ← %s\n"
                    "             %s\n",
                    conn_used_path.c_str(), format_device_conn(sc_conn).c_str());
    } else if (host_given) {
        std::printf("接入参数   : 约定路径下没有 active.conn（用命令行参数）\n");
    } else {
        std::printf("接入参数   : 约定路径下没有 active.conn —— 用内置默认 %s:%u\n"
                    "             从站 %u（接本地仿真从站用）\n"
                    "             想在平台配置的设备上核对，先在 15/ 界面保存接入参数，\n"
                    "             或用 --host 显式指定。\n",
                    cfg.host.c_str(), static_cast<unsigned>(cfg.port),
                    static_cast<unsigned>(cfg.unit_id));
    }

    if (dump_conn) {
        std::printf("\n");
        if (conn_load == ConnLoad::kOk) {
            std::printf("接入参数文件：%s\n  %s\n",
                        conn_used_path.c_str(), format_device_conn(sc_conn).c_str());
        } else {
            std::printf("没有接入参数文件。当前生效目标来自命令行/内置默认：\n"
                        "  地址 %s:%u 从站 %u 超时 %d ms\n",
                        cfg.host.c_str(), static_cast<unsigned>(cfg.port),
                        static_cast<unsigned>(cfg.unit_id), cfg.timeout_ms);
        }
        return sc == 0 ? 0 : 1;
    }

    std::printf("目标       : %s:%u unit=%u 超时=%d ms\n\n",
                cfg.host.c_str(), static_cast<unsigned>(cfg.port),
                static_cast<unsigned>(cfg.unit_id), cfg.timeout_ms);

    ModbusDeviceIO io(cfg);
    const auto t0 = std::chrono::steady_clock::now();
    const bool connected = io.connect();
    const auto connect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    if (!connected) {
        std::printf("[FAIL] 连接失败（%lld ms）。\n"
                    "       检查：设备是否上电、网线、IP/端口、从站地址；\n"
                    "       本工具用**非阻塞 connect**，所以离线时会在这里快速返回，\n"
                    "       不会卡在 OS 的 SYN 重传超时上（那要 ~20 s）。\n",
                    static_cast<long long>(connect_ms));
        return 1;
    }
    std::printf("[ OK ] 连接成功（%lld ms）\n\n", static_cast<long long>(connect_ms));

    for (int k = 0; k < repeat; ++k) {
        if (repeat > 1) std::printf("---------- 第 %d/%d 遍 ----------\n", k + 1, repeat);

        const auto s0 = std::chrono::steady_clock::now();
        RealtimeSnapshot snap;
        const bool ok = io.read_snapshot(static_cast<double>(k), snap);
        const auto scan_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - s0).count();

        std::printf("量测（%s，耗时 %lld ms）：\n", ok ? "ok" : "**不可信**",
                    static_cast<long long>(scan_ms));
        for (std::size_t i = 0; i <= EMS_SOH; ++i) print_point(io, i);

        std::printf("\n配置：\n");
        for (std::size_t i = EMS_CFG_CAP_KWH; i <= EMS_CFG_SOC_MAX; ++i) print_point(io, i);

        std::printf("\n状态：\n");
        for (std::size_t i = EMS_STA_BMS; i < modbus::active_binding_count(); ++i) print_point(io, i);

        DeviceLimits lim;
        io.read_limits(lim);
        std::printf("\n限值（装配给 05/ 安全引擎的输入）：\n"
                    "  PCS 额定充/放      : %.2f / %.2f kW\n"
                    "  BMS 限充/限放      : %.2f / %.2f kW\n"
                    "  变压器容量         : %.2f kVA\n"
                    "  契约需量           : %.2f kW\n"
                    "  ★ BMS 禁充 / 禁放  : %s / %s\n"
                    "  电池额定容量       : %.2f kWh\n",
                    lim.pcs_rated_chg_kw, lim.pcs_rated_dis_kw,
                    lim.bms_chg_limit_kw, lim.bms_dis_limit_kw,
                    lim.transformer_capacity_kw, lim.d_target_kw,
                    lim.bms_chg_forbidden ? "**禁止**" : "允许",
                    lim.bms_dis_forbidden ? "**禁止**" : "允许",
                    io.battery_capacity_kwh());

        if (set_power) {
            DeviceLimits l2;
            io.read_limits(l2);
            // ★ 权限区间取设备能力的 ±，**不是**拿指令值当区间 ——
            //   本工具是核对工具，不该替 05/ 安全引擎做决策。
            //   现场真要下发，权限区间必须由策略/安全层给出。
            PowerCommand cmd;
            cmd.p_bat_cmd_kw = power_kw;
            cmd.p_upper = l2.pcs_rated_dis_kw;
            cmd.p_lower = -l2.pcs_rated_chg_kw;
            const bool w = io.write_command(cmd);
            std::printf("\n下发指令 : %s  P_bat=%.2f  区间[%.2f, %.2f]（一次 FC16 原子下发）\n",
                        w ? "成功" : "**失败**", cmd.p_bat_cmd_kw, cmd.p_lower, cmd.p_upper);
        }

        std::printf("\n诊断     : 扫描 %d 次（部分成功 %d）  分块失败 %d  解码失败 %d  "
                    "请求 %d  超时 %d  从站异常 %d  迟到包 %d  不可信点 %d\n",
                    io.scans(), io.partial_scans(), io.block_fails(), io.decode_fails(),
                    io.client().requests(), io.client().timeouts(),
                    io.client().exceptions(), io.client().stale_responses(),
                    io.stale_points());
        // ★ 两个失败计数**分开报**：block_fails 高是通信/地址问题，
        //   decode_fails 高是**字序/编码**问题 —— 现场排查路径完全不同。
        if (io.decode_fails() > 0) {
            std::printf("  ★ decode_fails > 0：通信是好的，**映射表配错了**"
                        "（常见原因：32 位字序）。\n");
        }

        if (k + 1 < repeat) std::this_thread::sleep_for(
            std::chrono::milliseconds(static_cast<long long>(interval_s * 1000)));
        else if (repeat > 1) std::printf("\n");
    }

    if (verbose) {
        std::printf("重连次数 : %d  未连接扫描 : %d\n", io.reconnects(), io.unconnected_scans());
    }
    return 0;
}
