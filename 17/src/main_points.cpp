// =====================================================================
// 17/src/main_points.cpp —— 点表加载与核对工具（dev_points.exe）
//
// 为什么要有这个 exe：
//   站点追加点（换站 / 换厂家才有的点）走运行时 JSON 加载，但「谁来加载、
//   加载了没有」在 17/ 里原本**没有任何地方可查** —— 只有测试里 load 过一次。
//   本工具把它变成现场可核对的一步：加载 → 校验 → 两层自检 → 导出。
//
// ★ 默认追加文件不存在是**设计内的降级**，不是故障：
//   不是每个站都有追加点。但它**必须被打印**（[SKIP]），否则"没加载"与
//   "加载了但为空"在日志上不可分辨 —— 与 P2「SKIP 必须打印」同一条纪律。
//   而**显式** `--append <file>` 指定的文件缺失 = 缺陷，报错并以 1 退出。
//
// 为什么不做成"加载失败就自动跳过"：
//   站点追加点悄悄不生效，是这类配置最容易出现的失效方式 —— 界面上什么都
//   看不出来，只有点表里少了几行。宁可让进程起不来，也不能让它悄悄跑错。
//
// 编译：见 17/scripts/build.bat（build_all 第 37 步会带上）
// =====================================================================

#include "device_point_table.h"
#include "point_table_loader.h"
#include "point_mapping.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using ems::devpt::DevicePointTable;
using ems::devpt::LoadError;

const char* kDefaultAppend = "data/append_points.json";

void usage() {
    std::printf(
        "用法: dev_points.exe [选项]\n"
        "\n"
        "  --mod <name>      加载选配模块点表 data/mod_<name>.json\n"
        "                    可取 fire / hvac / sec / switch，可重复给多个\n"
        "                    ★ 恒排在站点追加点之后（见下方顺序说明）\n"
        "  --append <file>   追加点表 JSON（默认 %s），可重复给多个\n"
        "                    显式指定时，文件缺失 = 缺陷，报错退出 1\n"
        "  --no-append       不加载任何追加表，只用内置点表\n"
        "  --out <file>      导出整张点表 JSON 到文件\n"
        "  --list [dev]      列出点名（可按 bms / meter / pcs / fire / hvac / sec / switch 过滤）\n"
        "  --help            显示本帮助\n"
        "\n"
        "  ★ 追加顺序决定索引：内置 107 → 站点追加表（按命令行顺序）→ 选配模块。\n"
        "    `--mod` 与 `--append` 分成两段加载，故命令行里谁先写都不影响索引 ——\n"
        "    否则同一份配置换个写法索引就全变，这是现场最不能发生的事。\n"
        "\n"
        "退出码: 0 正常 / 1 加载失败、校验不通过或两层不一致\n",
        kDefaultAppend);
}

bool file_exists(const char* p) {
    std::FILE* f = std::fopen(p, "rb");
    if (f) { std::fclose(f); return true; }
    return false;
}

// 加载单份追加表。
//   tag       打印时的类别标签（"追加表" / "选配模块"）
//   hard_fail 文件缺失是否算缺陷：
//               显式 --append 与 --mod 都算（人指明了要它）；
//               默认的 append_points.json 不算（不是每站都有追加点）。
bool load_one(DevicePointTable& t, const std::string& p, const char* tag, bool hard_fail) {
    const std::size_t before = t.size();
    if (!file_exists(p.c_str())) {
        if (hard_fail) {
            std::printf("[FAIL] 指定的点表不存在: %s\n", p.c_str());
            std::printf("       显式指定的表缺失是缺陷 —— 不静默跳过\n");
            return false;
        }
        std::printf("[SKIP] 默认追加表不存在: %s（不是缺陷，但必须打印）\n", p.c_str());
        return true;
    }
    const LoadError e = t.load_file(p, true);
    if (!e.ok) {
        std::printf("[FAIL] 点表加载失败: %s\n", e.to_string().c_str());
        return false;
    }
    std::printf("%s: %s → +%d 点\n", tag, p.c_str(),
                static_cast<int>(t.size() - before));
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> append_list;   // ★ 可多个：站点追加点
    std::vector<std::string> mod_list;      // ★ 柜型选配模块
    bool        use_append       = true;
    bool        explicit_append  = false;
    std::string out_path;
    std::string list_dev;
    bool        do_list = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help") {
            usage();
            return 0;
        } else if (a == "--append" && i + 1 < argc) {
            append_list.push_back(argv[++i]);
            explicit_append = true;
        } else if (a == "--mod" && i + 1 < argc) {
            // ★ 不置 explicit_append：选配模块是**叠加**在站点追加点之上的，
            //   它不该把默认的 append_points.json 顶掉 —— 那会让站点点静默丢失。
            mod_list.push_back(std::string("data/mod_") + argv[++i] + ".json");
        } else if (a == "--no-append") {
            use_append = false;
        } else if (a == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (a == "--list") {
            do_list = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') list_dev = argv[++i];
        } else {
            std::printf("[FAIL] 未知参数: %s\n", a.c_str());
            usage();
            return 1;
        }
    }

    using namespace ems::devpt;

    DevicePointTable t = DevicePointTable::builtin();
    const int builtin_n = static_cast<int>(t.size());
    std::printf("内置点表      : %d 点\n", builtin_n);

    if (use_append) {
        // ① 站点追加点：显式给了就按命令行顺序，没给就用默认那一份
        if (append_list.empty()) append_list.push_back(kDefaultAppend);
        for (const std::string& p : append_list) {
            if (!load_one(t, p, "追加表        ", explicit_append)) return 1;
        }
        // ② 柜型选配模块：恒在站点追加点之后 ⇒ 索引不随命令行书写顺序改变
        for (const std::string& p : mod_list) {
            if (!load_one(t, p, "选配模块      ", true)) return 1;
        }
    } else {
        std::printf("[SKIP] --no-append：只用内置表\n");
    }

    std::printf("合计          : %d 点\n", static_cast<int>(t.size()));

    // ---- 校验 ----
    const LoadError v = t.validate();
    if (!v.ok) {
        std::printf("[FAIL] 点表校验不通过: %s\n", v.to_string().c_str());
        return 1;
    }
    std::printf("点表校验      : 通过\n");

    // ---- 两层自检（40 点抽象表 ↔ 设备点表）----
    const int bad = self_check(t);
    std::printf("两层自检      : 不一致 %d 项\n", bad);
    if (bad != 0) {
        std::printf("[FAIL] 两层点表不一致 —— 新增点必须同时扩 point_mapping.h 的映射\n");
        return 1;
    }

    // ---- 追加区清单 ----
    if (static_cast<int>(t.size()) > builtin_n) {
        std::printf("\n追加区点位（索引 %d..%d）:\n", builtin_n, static_cast<int>(t.size()) - 1);
        for (std::size_t i = static_cast<std::size_t>(builtin_n); i < t.size(); ++i) {
            std::printf("  %3d  %-26s %-6s %-6s %-5s\n",
                        static_cast<int>(i),
                        t.points[i].name.c_str(),
                        t.points[i].unit.c_str(),
                        device_kind_name(t.points[i].device),
                        point_zone_name(t.points[i].zone));
        }
    }

    // ---- 导出 ----
    if (!out_path.empty()) {
        std::FILE* f = std::fopen(out_path.c_str(), "wb");
        if (!f) {
            std::printf("[FAIL] 写不出: %s\n", out_path.c_str());
            return 1;
        }
        const std::string s = t.to_json(2);
        std::fwrite(s.data(), 1, s.size(), f);
        std::fclose(f);
        std::printf("\n[OK] 已导出 %d 点 → %s\n", static_cast<int>(t.size()), out_path.c_str());
    }

    // ---- 列出 ----
    if (do_list) {
        std::printf("\n点位清单%s:\n", list_dev.empty() ? "" : ("（" + list_dev + "）").c_str());
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (!list_dev.empty() && list_dev != device_kind_name(t.points[i].device)) continue;
            std::printf("  %3d  %s\n", static_cast<int>(i), t.points[i].name.c_str());
        }
    }

    std::printf("\n[DEV POINTS OK] %d points\n", static_cast<int>(t.size()));
    return 0;
}
