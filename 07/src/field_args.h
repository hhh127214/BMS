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

enum class DeviceKind { kSim, kRtdb, kModbus, kDlt645 };

inline const char* device_kind_name(DeviceKind k) {
    switch (k) {
    case DeviceKind::kSim:    return "sim";
    case DeviceKind::kRtdb:   return "rtdb";
    case DeviceKind::kModbus: return "modbus";
    case DeviceKind::kDlt645: return "dlt645";
    }
    return "?";
}

struct Args {
    DeviceKind  device  = DeviceKind::kSim;   // ★ 默认仿真
    std::string host;                         // modbus：命令行给，或由接入参数文件补
                                              // dlt645：命令行给（645 尚无接入参数文件通道）
    int         port    = 502;
    int         unit    = 1;
    int         timeout_ms = 1000;            // 单次请求超时（可由接入参数文件补）
    std::string meter_addr;                   // dlt645：12 位电表地址（--addr）
    std::string map_path;                     // 空 = 按 13/ 的约定路径找
    std::string record_path;                  // 空 = 不写实录（--record 才写）
    bool        control = false;              // ★ 默认只读
    bool        force   = false;              // 跳过闭环前的预读体检
    bool        mirror_rtdb = false;          // 每拍把采集值镜像写进 RT_DB（上端 104 的数据源）
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
        "  " + e + " [--device sim|rtdb|modbus|dlt645] [选项]\n"
        "\n"
        "设备来源（--device）：\n"
        "  sim     仿真（默认）。跑周期 7 演示场景，输出与 loop_demo.exe 逐字相同。\n"
        "  rtdb    共享内存实时库（RtDbDeviceIO）。要求设备侧另有进程往同一段里写。\n"
        "  modbus  Modbus TCP 主站（ModbusDeviceIO），直连 PCS / BMS / 电表。\n"
        "  dlt645  DL/T645-2007 主站（Meter645DeviceIO），经串口服务器透传读关口电表。\n"
        "          **只读**：电表没有接收功率指令的语义，--control 会被拒绝。\n"
        "\n"
        "选项：\n"
        "  --host <ip>     设备地址。modbus 不给时自动读接入参数文件（见下）；\n"
        "                  dlt645 只认命令行（645 尚无接入参数文件通道）\n"
        "  --port <n>      端口，默认 502（dlt645 透传网关常用 4001/23 等，按现场）\n"
        "  --unit <n>      从站号，默认 1（范围 1..247；仅 modbus 用）\n"
        "  --addr <12位>   电表地址（仅 dlt645 用；电表铭牌/条码上的 12 位表号）\n"
        "  --map <path>    点表 CSV。不给则按约定路径找：\n"
        "                     $EMS_POINT_MAP_DIR\\active.csv\n"
        "                     或 <exe 上级两级>\\config\\point-map\\active.csv\n"
        "                  两者都没有 → 内置默认表（32 点），进程照样能起来\n"
        "  --steps <n>     跑多少拍。默认：只读 10 拍 / 闭环 6000 拍\n"
        "  --dt <s>        控制周期，默认 0.1\n"
        "  --record <path> 把每拍追加写成 CSV（20 列，与 10/ timeseries.csv 同构；\n"
        "                  t_s 是 Unix 墙钟秒，time 是完整日期时间）。\n"
        "                  供 14/ 平台做实时入库（scenario.time_base='wall'）。\n"
        "                  ★ 只读巡检**也写**（2026-10-18 起）：不写的话，接上真实设备\n"
        "                    只看不动时，平台页面一行数据都没有 —— 而现场看到的是\n"
        "                    「读到了、数值对」，没有任何地方提示「没入库」。\n"
        "                    只读写的行 state 恒为 INIT、reason 写明只读，p_cmd 恒 0。\n"
        "  --force         跳过闭环前的预读体检（默认会先读一拍，读不到就拒绝下发）\n"
        "  --mirror-rtdb   每拍把读到的量测/状态/配置写进共享内存实时库（MEAS/STA/CFG），\n"
        "                  并把自己算出的指令写进 CMD。这是**上端 IEC104 的数据源**：\n"
        "                  不写的话，网关照常上送 24 点，但值全是实时库初值\n"
        "                  （0 kW / SOC 50% / 25℃），品质还是 GOOD，不报错。\n"
        "                  ★ 仅 --device modbus：rtdb 模式自己就是读写方（会双写），\n"
        "                    dlt645 只有关口功率一路量测（其余写 0 = 假数据）。\n"
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
        "  " + e + " --device modbus --host 10.0.0.7        现场只读核对\n"
        "  " + e + " --device dlt645 --host 10.0.0.8 --port 4001 --addr 000000000001\n"
        "                                                  经透传网关读关口电表（只读）\n";
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
            else if (v == "dlt645") out.device = DeviceKind::kDlt645;
            else {
                err = "不认识的设备类型「" + v +
                      "」。只接受 sim / rtdb / modbus / dlt645 四者之一";
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
        } else if (k == "--addr") {
            if (!take()) return false;
            // 645 电表地址：12 位十进制数字（6 字节 BCD，低位在前传输）。
            // 校验"全是数字且正好 12 位"放在这里做 —— 多一位少一位、
            // 混进字母都是现场手敲的高发错误，必须当场拒。
            if (v.size() != 12) {
                err = "--addr (电表地址) 应为 12 位十进制数字，实际是「" + v + "」";
                return false;
            }
            for (char c : v) {
                if (c < '0' || c > '9') {
                    err = "--addr (电表地址) 只能含数字 0-9，实际是「" + v + "」";
                    return false;
                }
            }
            out.meter_addr = v;
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
        } else if (k == "--mirror-rtdb") {
            out.mirror_rtdb = true;
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

    // --mirror-rtdb 只对 modbus 成立。
    //   rtdb：那条路径本身就是实时库的读方，再镜像就是自己写自己读；
    //   dlt645：只有关口功率一路量测，其余点写进去只能是 0 —— 假数据；
    //   sim：没有真实设备，镜像出来的还是模型值，且 sim 路径不需要上端。
    if (out.mirror_rtdb && out.device != DeviceKind::kModbus) {
        err = "--mirror-rtdb 只能与 --device modbus 同用"
              "（rtdb 模式会双写、dlt645 只有一路量测、sim 没有真实设备）";
        return false;
    }
    // ★ `--record` 在只读巡检下**是允许的**（2026-10-18 改）。
    //   原先这里挡着，理由是「只读不创建 EmsRuntime，没有闭环记录可写」——
    //   技术上成立，产品上是个坑：现场第一次接设备必然是只读核对，
    //   此时平台页面一行数据都没有，而现场看到的是「读到了、数值对」，
    //   没有任何地方提示「没入库」。所以只读路径自己也写实录，
    //   state 记 INIT、reason 写「只读巡检」、p_cmd 恒 0 —— 能看出来不是闭环数据。
    if (out.device == DeviceKind::kSim && out.control) {
        err = "--device sim 下 --control 没有意义：仿真本来就在跑闭环，"
              "不存在「只读」一说（这也是它不接真实设备的证明）。"
              "真要向设备下发，请用 --device modbus --host <设备IP> --control";
        return false;
    }
    // ★ 645 电表是**只读计量设备**：协议里没有"接收功率指令"的语义。
    //   所以 645 + --control 不是"暂不支持"，是语义错误 —— 在解析层就拒掉，
    //   不让它活到适配器那一层才报"写失败"（那时看起来像通信故障，其实是装错通道）。
    if (out.device == DeviceKind::kDlt645 && out.control) {
        err = "--device dlt645 下 --control 是语义错误：DL/T645 电表是只读计量设备，\n"
              "      没有接收功率指令的语义。645 通道只用于只读巡检（核对关口计量）；\n"
              "      要下发功率指令，请用 --device modbus（PCS 在那条通道上）。";
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
    // dlt645：串口服务器透传的 TCP 地址 + 12 位电表地址，缺一不可。
    // ★ 645 目前**没有**接入参数文件通道（那是 modbus 的配置通道②），
    //   所以这里只有命令行一个来源 —— 故意不编默认地址/默认表地址：
    //   编出来就会去连它，而连错表在界面上表现为"数据正常但全错"。
    if (a.device == DeviceKind::kDlt645) {
        if (a.host.empty()) {
            err = "--device dlt645 需要串口服务器（透传网关）的 TCP 地址：\n"
                  "        --host <网关IP> --port <端口>\n"
                  "      想先对着仿真表核对，可用：\n"
                  "        21\\build\\sim_tcp_server.exe 起一台仿真表，再连它";
            return false;
        }
        if (a.meter_addr.empty()) {
            err = "--device dlt645 需要电表地址：\n"
                  "        --addr <12 位数字>（电表铭牌/条码上的 12 位表号）\n"
                  "      多表共 485 总线时地址是唯一的寻址依据，不许猜。";
            return false;
        }
    }
    return true;
}

}  // namespace field
}  // namespace ems

#endif  // EMS_07_FIELD_ARGS_H
