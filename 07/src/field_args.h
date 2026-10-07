// =====================================================================
// 07/ —— 现场进程入口的命令行参数
//
//   为什么单独成一个头文件：main_field.cpp 里 main() 的参数解析无法被
//   单元测试直接调用。抽出来之后，tests/test_field_entry.cpp 可以逐条断言
//   「默认值 / 缺必填 / 越界 / 不认识」这四类边界。
//
//   ★ 两条默认值是本文件的**核心约束**，改动前请先读：
//     ① device 默认 kSim —— 这是「别把仿真顶掉」的落点。不传 --device 时，
//        现场入口与演示程序走**同一份**场景代码（demo_scenario7.h），
//        输出逐字节相同（判据见 scripts/build_test_field.bat 的 T-F3）。
//     ② control 默认 false（只读）—— 现场第一次接进来时，默认姿势必须是
//        "只看不动"。真正下发功率指令需要显式 --control。
//        理由：本程序连的是真实 PCS，写错一个寄存器就是真的充放电。
// =====================================================================

#ifndef EMS_07_FIELD_ARGS_H
#define EMS_07_FIELD_ARGS_H

#include <cstdlib>
#include <string>

namespace ems {
namespace field {

enum class DeviceKind { kSim, kRtdb, kModbus };

inline const char* device_kind_name(DeviceKind k) {
    switch (k) {
    case DeviceKind::kSim:    return "sim";
    case DeviceKind::kRtdb:   return "rtdb";
    case DeviceKind::kModbus: return "modbus";
    }
    return "?";
}

struct Args {
    DeviceKind  device  = DeviceKind::kSim;   // ★ 默认仿真
    std::string host;                         // modbus：命令行给，或由接入参数文件补
    int         port    = 502;
    int         unit    = 1;
    int         timeout_ms = 1000;            // 单次请求超时（可由接入参数文件补）
    std::string map_path;                     // 空 = 按 13/ 的约定路径找
    std::string record_path;                  // 空 = 不写实录（--record 才写）
    bool        control = false;              // ★ 默认只读
    bool        force   = false;              // 跳过闭环前的预读体检
    int         steps   = 0;                  // 0 = 用该模式的默认拍数
    double      dt      = 0.1;
    bool        help    = false;
    bool        verbose = false;

    bool is_field() const { return device != DeviceKind::kSim; }
    int  steps_or(int dflt) const { return steps > 0 ? steps : dflt; }
};

inline std::string usage_text(const char* exe) {
    const std::string e = (exe && *exe) ? exe : "main_field.exe";
    return
        "07/ 现场进程入口 —— 设备接入与实时控制闭环\n"
        "\n"
        "用法：\n"
        "  " + e + " [--device sim|rtdb|modbus] [选项]\n"
        "\n"
        "设备来源（--device）：\n"
        "  sim     仿真（默认）。跑周期 7 演示场景，输出与 loop_demo.exe 逐字相同。\n"
        "  rtdb    共享内存实时库（RtDbDeviceIO）。要求设备侧另有进程往同一段里写。\n"
        "  modbus  Modbus TCP 主站（ModbusDeviceIO），直连 PCS / BMS / 电表。\n"
        "\n"
        "选项：\n"
        "  --host <ip>     设备地址。**不给时自动读接入参数文件**（见下）\n"
        "  --port <n>      端口，默认 502\n"
        "  --unit <n>      从站号，默认 1（范围 1..247）\n"
        "  --map <path>    点表 CSV。不给则按约定路径找：\n"
        "                     $EMS_POINT_MAP_DIR\\active.csv\n"
        "                     或 <exe 上级两级>\\config\\point-map\\active.csv\n"
        "                  两者都没有 → 内置默认表（32 点），进程照样能起来\n"
        "  --steps <n>     跑多少拍。默认：只读 10 拍 / 闭环 6000 拍\n"
        "  --dt <s>        控制周期，默认 0.1\n"
        "  --record <path> 闭环时把每拍追加写成 CSV（20 列，与 10/ timeseries.csv\n"
        "                  同构；t_s 是 Unix 墙钟秒，time 是完整日期时间）。\n"
        "                  供 14/ 平台做实时入库（scenario.time_base='wall'）。\n"
        "                  只在 --control 下有意义。\n"
        "  --force         跳过闭环前的预读体检（默认会先读一拍，读不到就拒绝下发）\n"
        "  -v, --verbose   打印限值与状态位明细\n"
        "  -h, --help      显示本帮助\n"
        "\n"
        "★ 接入参数的来源（--device modbus 时）：\n"
        "   1. 命令行 --host / --port / --unit（给了就用它）\n"
        "   2. 否则读约定路径的接入参数文件 active.conn（与点表同目录）：\n"
        "        $EMS_POINT_MAP_DIR\\active.conn\n"
        "        或 <exe 上级两级>\\config\\point-map\\active.conn\n"
        "      这个文件由 14/ 平台在 **15/ 界面「设备接入配置」里保存**时原子写出。\n"
        "      平台上把该设备标为「未启用」(enabled=0) 时，本程序**拒绝启动** ——\n"
        "      「未启用」是一个明确的意图，不该被「命令行没给参数」悄悄改写。\n"
        "   3. 两处都没有 → 报错（**不编一个默认 IP**：编出来就会去连它）\n"
        "   文件存在但非法 → 硬失败，不退回默认\n"
        "\n"
        "★★ 安全：现场接入默认是「只读」\n"
        "  不加 --control 时，本程序只读量测、**绝不下发任何指令** ——\n"
        "  这是现场第一次接进来时的默认姿势，用来核对点表、看真实数据。\n"
        "  确认无误后加 --control 才会真正向 PCS 写功率指令：\n"
        "    " + e + " --device modbus --host 192.168.1.10 --unit 3\n"
        "        → 只读核对（看数据对不对）\n"
        "    " + e + " --device modbus --host 192.168.1.10 --unit 3 --control\n"
        "        → 真下发（确认无误后再用）\n"
        "\n"
        "例：\n"
        "  " + e + "                                      仿真（与演示一致）\n"
        "  " + e + " --device rtdb --steps 20 -v            看实时库里的真实数据\n"
        "  " + e + " --device modbus --host 10.0.0.7        现场只读核对\n";
}

namespace detail {

// 刻意不用 std::stoi / std::strtol 的宽松解析：现场参数是手敲的，
// "502 " / "5O2" / "502abc" 这类输入必须当场拒掉，不能悄悄解析成 5 或 502。
inline bool parse_int(const std::string& s, int lo, int hi, int& out,
                      std::string& err, const char* label) {
    if (s.empty()) { err = std::string(label) + " 的取值为空"; return false; }
    for (char c : s) {
        if (c < '0' || c > '9') {
            err = std::string(label) + " 应为整数，实际是「" + s + "」";
            return false;
        }
    }
    const long v = std::strtol(s.c_str(), nullptr, 10);
    if (v < static_cast<long>(lo) || v > static_cast<long>(hi)) {
        err = std::string(label) + " 应在 " + std::to_string(lo) + ".." +
              std::to_string(hi) + " 之间，实际是 " + s;
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

inline bool parse_double(const std::string& s, double lo, double hi, double& out,
                         std::string& err, const char* label) {
    if (s.empty()) { err = std::string(label) + " 的取值为空"; return false; }
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || end == nullptr || *end != '\0') {
        err = std::string(label) + " 应为数值，实际是「" + s + "」";
        return false;
    }
    if (!(v >= lo && v <= hi)) {   // 写成 !(a&&b) 以挡住 NaN
        err = std::string(label) + " 应在 " + std::to_string(lo) + ".." +
              std::to_string(hi) + " 之间，实际是 " + s;
        return false;
    }
    out = v;
    return true;
}

}  // namespace detail

// 解析成功返回 true；失败返回 false 且 err 里是**给现场人看**的原因 + 处置建议。
inline bool parse_args(int argc, char** argv, Args& out, std::string& err) {
    out = Args{};
    err.clear();

    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];

        if (k == "-h" || k == "--help") { out.help = true; return true; }

        std::string v;
        auto take = [&]() -> bool {
            if (i + 1 >= argc) {
                err = "参数 " + k + " 后面缺少取值（例：--port 502）";
                return false;
            }
            v = argv[++i];
            return true;
        };

        if (k == "--device") {
            if (!take()) return false;
            if      (v == "sim")    out.device = DeviceKind::kSim;
            else if (v == "rtdb")   out.device = DeviceKind::kRtdb;
            else if (v == "modbus") out.device = DeviceKind::kModbus;
            else {
                err = "不认识的设备类型「" + v +
                      "」。只接受 sim / rtdb / modbus 三者之一";
                return false;
            }
        } else if (k == "--host") {
            if (!take()) return false;
            if (v.empty()) { err = "--host 的取值为空"; return false; }
            out.host = v;
        } else if (k == "--port") {
            if (!take()) return false;
            if (!detail::parse_int(v, 1, 65535, out.port, err, "--port")) return false;
        } else if (k == "--unit") {
            if (!take()) return false;
            if (!detail::parse_int(v, 1, 247, out.unit, err, "--unit (从站号)")) return false;
        } else if (k == "--map") {
            if (!take()) return false;
            if (v.empty()) { err = "--map 的取值为空"; return false; }
            out.map_path = v;
        } else if (k == "--record") {
            if (!take()) return false;
            if (v.empty()) { err = "--record 的取值为空"; return false; }
            out.record_path = v;
        } else if (k == "--steps") {
            if (!take()) return false;
            if (!detail::parse_int(v, 1, 100000000, out.steps, err, "--steps")) return false;
        } else if (k == "--dt") {
            if (!take()) return false;
            if (!detail::parse_double(v, 0.001, 60.0, out.dt, err, "--dt (秒)")) return false;
        } else if (k == "--control") {
            out.control = true;
        } else if (k == "--force") {
            out.force = true;
        } else if (k == "-v" || k == "--verbose") {
            out.verbose = true;
        } else {
            err = "不认识的参数「" + k + "」。用 --help 看用法";
            return false;
        }
    }

    // ---- 交叉校验（只做不依赖目标地址的那些）----
    //
    // ★ 「--device modbus 必须给 --host」**刻意不在这里**：
    //   地址还可能来自接入参数文件（配置通道②，config/point-map/active.conn），
    //   而本文件是**纯解析**（不碰文件系统，测试要能逐条断言边界）。
    //   所以那一项挪到 validate_target() —— 由 main_field.cpp 在
    //   parse_args 之后、尝试读接入参数文件之后再调。顺序不能颠倒：
    //   先解析（知道用户想要什么）→ 再补配置 → 最后校验目标齐不齐。
    if (!out.control && out.force) {
        err = "--force 只在 --control 下有意义（它跳过的正是闭环前的预读体检）";
        return false;
    }
    if (!out.control && !out.record_path.empty()) {
        err = "--record 只在 --control 下有意义：只读模式不创建 EmsRuntime，"
              "没有闭环记录可写。想边控制边实录，请加 --control。";
        return false;
    }
    if (out.device == DeviceKind::kSim && out.control) {
        err = "--device sim 下 --control 没有意义：仿真本来就在跑闭环，"
              "不存在「只读」一说（这也是它不接真实设备的证明）。"
              "真要向设备下发，请用 --device modbus --host <设备IP> --control";
        return false;
    }
    return true;
}

// 目标地址的最终校验。与 parse_args 分开的原因见上面那段注释。
//
// 调用时机：parse_args 成功 → （modbus 分支）尝试从接入参数文件补 host/port/unit
//           → 本函数。所以它看到的是**补完之后**的 Args。
inline bool validate_target(const Args& a, std::string& err) {
    if (a.device == DeviceKind::kModbus && a.host.empty()) {
        err = "--device modbus 需要一个目标地址，但两处都没有给：\n"
              "        ① 命令行 --host <设备IP>\n"
              "        ② 接入参数文件（在 15/ 界面上配置接入参数并保存，\n"
              "           平台会落盘到 config/point-map/active.conn，本程序启动时读它）\n"
              "      设备地址在设备铭牌或交换机上；想先探一下端口通不通，可用\n"
              "      13\\build\\modbus_probe.exe --host <ip> --port <n>";
        return false;
    }
    return true;
}

}  // namespace field
}  // namespace ems

#endif  // EMS_07_FIELD_ARGS_H
