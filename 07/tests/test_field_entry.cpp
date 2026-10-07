// =====================================================================
// 07/ 单元测试 —— 现场进程入口（P0.5 设备接入闭环）
//
//   T-F1: 默认值 —— 不传参数时必须是 device=sim 且 control=false
//         （这是「别把仿真顶掉」与「默认只读」两条约束的落点）
//   T-F2: --device 的三种取值 / 非法值 / 缺取值
//   T-F3: --device modbus 的目标地址校验（解析层不再管，见 validate_target）
//         ★ 配置通道②之后：地址可来自命令行或接入参数文件，所以要分两步验
//   T-F4: 数值参数的越界与**宽松解析陷阱**（"502abc" / "5O2" / " 502"）
//   T-F5: --control 与 --force 的交叉校验
//   T-F6: 不认识的参数
//   T-F7: --help（且帮助文本必须写出两条安全约束）
//   T-F8: steps_or 的默认拍数
//   T-F9: --record（实录 CSV 路径；只在 --control 下有意义）
//
//   ★ 「输出等价性」不在本文件里测：那要跑两个可执行文件。
//     由 scripts/build_test_field.bat 的 P-F1 用 findstr + fc 完成 ——
//     main_field.exe（不传 --device）与 loop_demo.exe 的输出必须逐字节相同
//     （唯一允许的差异是 mean_cycle，它是每拍耗时的计时噪声）。
//
// 编译：见 scripts/build_test_field.bat
// =====================================================================

#include "field_args.h"

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

// ---------------------------------------------------------------------
// 把 parse_args 包一层：argv 构造 + 结果放到全局，避免宏参数里的逗号问题。
// ---------------------------------------------------------------------
static field::Args g_a;
static std::string g_err;
static bool        g_ok = false;

static void P(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("main_field.exe"));
    for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
    g_ok = field::parse_args(static_cast<int>(argv.size()), argv.data(), g_a, g_err);
}

static bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// =====================================================================
// T-F1  默认值
// =====================================================================
static void test_f1_defaults() {
    std::cout << "\n[T-F1] 默认值：不传参数时必须落在「仿真 + 只读」\n";

    P({});
    EXPECT(g_ok);
    EXPECT(g_a.device == field::DeviceKind::kSim);   // ★ 默认仿真
    EXPECT(g_a.control == false);                    // ★ 默认只读
    EXPECT(g_a.force == false);
    EXPECT(g_a.port == 502);
    EXPECT(g_a.unit == 1);
    EXPECT(g_a.dt == 0.1);
    EXPECT(g_a.steps == 0);
    EXPECT(g_a.map_path.empty());
    EXPECT(g_a.host.empty());
    EXPECT(!g_a.is_field());
    EXPECT(std::string(field::device_kind_name(g_a.device)) == "sim");

    // 不传参数 与 显式 --device sim 必须等价（现场不该因为多打几个字而变行为）
    const field::Args bare = g_a;
    P({"--device", "sim"});
    EXPECT(g_ok);
    EXPECT(g_a.device == bare.device);
    EXPECT(g_a.control == bare.control);
    EXPECT(g_a.port == bare.port);
    EXPECT(g_a.unit == bare.unit);
    EXPECT(g_a.dt == bare.dt);
}

// =====================================================================
// T-F2  --device
// =====================================================================
static void test_f2_device_kind() {
    std::cout << "\n[T-F2] --device 的三种取值与非法值\n";

    P({"--device", "rtdb"});
    EXPECT(g_ok);
    EXPECT(g_a.device == field::DeviceKind::kRtdb);
    EXPECT(g_a.is_field());
    EXPECT(std::string(field::device_kind_name(g_a.device)) == "rtdb");

    P({"--device", "modbus", "--host", "10.0.0.1"});
    EXPECT(g_ok);
    EXPECT(g_a.device == field::DeviceKind::kModbus);
    EXPECT(g_a.host == "10.0.0.1");
    EXPECT(std::string(field::device_kind_name(g_a.device)) == "modbus");

    // 非法值：必须逐个拒掉，不能"近似匹配"
    P({"--device", "bogus"});
    EXPECT(!g_ok);
    EXPECT(has(g_err, "sim / rtdb / modbus"));
    P({"--device", "MODBUS"});
    EXPECT(!g_ok);                       // 大小写敏感：现场要打对
    P({"--device", "simx"});
    EXPECT(!g_ok);
    P({"--device", ""});
    EXPECT(!g_ok);
    P({"--device"});
    EXPECT(!g_ok);
    EXPECT(has(g_err, "缺少取值"));
}

// =====================================================================
// T-F3  modbus 的目标地址：解析层不管，交给 validate_target
//
// ★ 这条在「配置通道②」之后**改了分工**，不是放宽了要求：
//   地址还可能来自接入参数文件（config/point-map/active.conn），而
//   parse_args 是**纯解析**（不碰文件系统，否则边界测试没法逐条断言）。
//   所以"必须有目标"这一项挪到 validate_target()，由 main_field.cpp 在
//   「解析 → 尝试读接入参数文件」之后再调。要求本身一点没松：
//   两处都没有地址时，仍然必须拒绝启动，且要给出具体处置办法。
// =====================================================================
static void test_f3_modbus_needs_host() {
    std::cout << "\n[T-F3] --device modbus 的目标地址校验（解析 + validate_target）\n";

    // 解析层：不再因为缺 host 就失败（地址可能来自配置文件）
    P({"--device", "modbus"});
    EXPECT(g_ok);
    EXPECT(g_a.host.empty());
    // 但目标校验必须挡住它，而且要说清两处来源 + 具体工具名
    EXPECT(!field::validate_target(g_a, g_err));
    EXPECT(has(g_err, "--host"));
    EXPECT(has(g_err, "active.conn"));        // 第二条来源要写清楚
    EXPECT(has(g_err, "modbus_probe.exe"));   // 处置建议要具体到工具名

    // 显式写了 --host 但取值是空串 → 解析层当场拒掉。
    // （与"根本没写 --host"不同：后者交给 validate_target / 接入参数文件处理，
    //   前者是敲错了参数，必须当场发现。）
    P({"--device", "modbus", "--host", ""});
    EXPECT(!g_ok);
    EXPECT(has(g_err, "取值为空"));

    // 给了地址就过（IPv4 与主机名都认）
    P({"--device", "modbus", "--host", "192.168.1.10"});
    EXPECT(g_ok);
    EXPECT(field::validate_target(g_a, g_err));
    P({"--device", "modbus", "--host", "pcs-01.local"});
    EXPECT(g_ok);
    EXPECT(field::validate_target(g_a, g_err));

    // rtdb / sim 不需要目标地址
    P({"--device", "rtdb"});
    EXPECT(g_ok);
    EXPECT(field::validate_target(g_a, g_err));
    P({"--device", "rtdb", "--host", "ignored"});
    EXPECT(g_ok);
    EXPECT(field::validate_target(g_a, g_err));
    P({});
    EXPECT(g_ok);
    EXPECT(field::validate_target(g_a, g_err));

    // 接入参数文件补上地址之后（模拟 main_field.cpp 的那一步），校验就该过
    P({"--device", "modbus"});
    EXPECT(g_ok);
    g_a.host = "10.0.0.7";        // ← resolve_modbus_target() 的等价效果
    g_a.unit = 5;
    EXPECT(field::validate_target(g_a, g_err));
    EXPECT(g_a.unit == 5);
}

// =====================================================================
// T-F4  越界与宽松解析陷阱
// =====================================================================
static void test_f4_ranges() {
    std::cout << "\n[T-F4] 数值参数的越界与宽松解析陷阱\n";

    const std::vector<std::string> base = {"--device", "modbus", "--host", "h"};

    auto with = [&](const char* k, const char* v) {
        std::vector<std::string> a = base;
        a.push_back(k);
        a.push_back(v);
        P(a);
    };

    with("--port", "0");          EXPECT(!g_ok);
    with("--port", "65536");      EXPECT(!g_ok);
    with("--port", "65535");      EXPECT(g_ok);
    with("--port", "502");        EXPECT(g_ok);

    with("--unit", "0");          EXPECT(!g_ok);
    with("--unit", "248");        EXPECT(!g_ok);
    with("--unit", "247");        EXPECT(g_ok);
    with("--unit", "1");          EXPECT(g_ok);

    // ★ 陷阱：strtol / stoi 会把这些「解析成功」，但现场手敲的这类输入
    //   必须当场拒掉 —— 否则 --port 502abc 会悄悄变成 502，连到错误的端口。
    with("--port", "502abc");     EXPECT(!g_ok);
    with("--port", "5O2");        EXPECT(!g_ok);   // 字母 O
    with("--port", " 502");       EXPECT(!g_ok);
    with("--port", "502 ");       EXPECT(!g_ok);
    with("--port", "-1");         EXPECT(!g_ok);
    with("--port", "5.02");       EXPECT(!g_ok);
    with("--port", "");           EXPECT(!g_ok);

    with("--dt", "0");            EXPECT(!g_ok);
    with("--dt", "abc");          EXPECT(!g_ok);
    with("--dt", "1e9");          EXPECT(!g_ok);
    with("--dt", "0.05");         EXPECT(g_ok);
    with("--dt", "0.1");          EXPECT(g_ok);

    with("--steps", "0");         EXPECT(!g_ok);
    with("--steps", "1");         EXPECT(g_ok);
    with("--steps", "1000");      EXPECT(g_ok);
}

// =====================================================================
// T-F5  --control / --force
// =====================================================================
static void test_f5_control_force() {
    std::cout << "\n[T-F5] --control / --force 的交叉校验\n";

    P({"--device", "modbus", "--host", "h"});
    EXPECT(g_ok);
    EXPECT(g_a.control == false);          // ★ 默认只读

    P({"--device", "modbus", "--host", "h", "--control"});
    EXPECT(g_ok);
    EXPECT(g_a.control == true);
    EXPECT(g_a.force == false);

    // --force 只在 --control 下有意义
    P({"--device", "modbus", "--host", "h", "--force"});
    EXPECT(!g_ok);
    EXPECT(has(g_err, "--force"));

    P({"--device", "modbus", "--host", "h", "--control", "--force"});
    EXPECT(g_ok);
    EXPECT(g_a.control == true);
    EXPECT(g_a.force == true);

    // 仿真下 --control 无意义（仿真本来就在跑闭环）
    P({"--device", "sim", "--control"});
    EXPECT(!g_ok);
    P({"--control"});                       // 默认就是 sim，同样被拒
    EXPECT(!g_ok);
}

// =====================================================================
// T-F6  不认识的参数
// =====================================================================
static void test_f6_unknown() {
    std::cout << "\n[T-F6] 不认识的参数必须报错，不能默默忽略\n";

    P({"--devic", "modbus"});               // 拼错
    EXPECT(!g_ok);
    EXPECT(has(g_err, "--help"));

    P({"-x"});
    EXPECT(!g_ok);

    P({"--device", "modbus", "--host", "h", "extra"});
    EXPECT(!g_ok);

    P({"--device", "modbus", "--host", "h", "--contorl"});  // 拼错
    EXPECT(!g_ok);
}

// =====================================================================
// T-F7  --help
// =====================================================================
static void test_f7_help() {
    std::cout << "\n[T-F7] --help 与帮助文本的内容约束\n";

    P({"--help"});
    EXPECT(g_ok);
    EXPECT(g_a.help);
    P({"-h"});
    EXPECT(g_ok);
    EXPECT(g_a.help);
    // help 优先：后面即使有非法参数也应直接返回帮助
    P({"--help", "--device", "bogus"});
    EXPECT(g_ok);
    EXPECT(g_a.help);

    const std::string u = field::usage_text("main_field.exe");
    EXPECT(has(u, "sim"));
    EXPECT(has(u, "rtdb"));
    EXPECT(has(u, "modbus"));
    EXPECT(has(u, "--control"));
    EXPECT(has(u, "只读"));                   // 安全约束必须写在帮助里
    EXPECT(has(u, "EMS_POINT_MAP_DIR"));      // 约定路径的来源
    EXPECT(has(u, "config\\point-map"));
    EXPECT(has(u, "--map"));
}

// =====================================================================
// T-F8  steps_or
// =====================================================================
static void test_f8_steps_or() {
    std::cout << "\n[T-F8] steps_or 的默认拍数\n";

    field::Args a;
    EXPECT(a.steps_or(6000) == 6000);
    EXPECT(a.steps_or(10) == 10);
    a.steps = 50;
    EXPECT(a.steps_or(6000) == 50);
    EXPECT(a.steps_or(10) == 50);
    a.steps = 1;
    EXPECT(a.steps_or(6000) == 1);
}

// =====================================================================
// T-F9  --record
// =====================================================================
static void test_f9_record() {
    std::cout << "\n[T-F9] --record 实录 CSV 路径\n";

    // 默认不写实录
    P({"--device", "modbus", "--host", "h"});
    EXPECT(g_ok);
    EXPECT(g_a.record_path.empty());

    // 只有 --control 下 --record 才有意义（只读不建 EmsRuntime，没记录可写）
    P({"--device", "modbus", "--host", "h", "--record", "out.csv"});
    EXPECT(!g_ok);
    EXPECT(has(g_err, "--record"));
    EXPECT(has(g_err, "--control"));

    // --control + --record 合法
    P({"--device", "modbus", "--host", "h", "--control", "--record", "out.csv"});
    EXPECT(g_ok);
    EXPECT(g_a.record_path == "out.csv");

    // 缺取值 / 空取值
    P({"--device", "modbus", "--host", "h", "--control", "--record"});
    EXPECT(!g_ok);
    EXPECT(has(g_err, "缺少取值"));
    P({"--device", "modbus", "--host", "h", "--control", "--record", ""});
    EXPECT(!g_ok);
    EXPECT(has(g_err, "取值为空"));
}

// =====================================================================
int main() {
    std::cout << "=========================================================\n"
              << " 07/ 现场进程入口单元测试（T-F1 ~ T-F9）\n"
              << "=========================================================\n";

    test_f1_defaults();
    test_f2_device_kind();
    test_f3_modbus_needs_host();
    test_f4_ranges();
    test_f5_control_force();
    test_f6_unknown();
    test_f7_help();
    test_f8_steps_or();
    test_f9_record();

    std::cout << "\n=========================================\n"
              << " PASS=" << g_pass << "  FAIL=" << g_fail << "\n"
              << "=========================================\n";
    return (g_fail == 0) ? 0 : 1;
}
