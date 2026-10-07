// =====================================================================
// 20/ — soe_dump：SOE 落盘文件的读取 / 过滤 / 导出工具
//
// 用途（也是"文件格式稳定、能被外部工具读"的证据之一）：
//   现场排障时不必写代码，用本工具按时间/等级/来源过滤 SOE 文件。
//
// 用法：
//   soe_dump.exe --file 20\build\soe.log
//   soe_dump.exe --file 20\build\soe.log --min-level WARN
//   soe_dump.exe --file 20\build\soe.log --source COMM --t0 10 --t1 60
//   soe_dump.exe --file 20\build\soe.log --out 20\build\soe.filtered.log
//
// 输出：规范化后的同一行格式（与 SoeStore::header_line() 一致）+ 末行统计。
// 退出码：0 = 成功（哪怕过滤后 0 条），1 = 文件打不开 / 解析失败。
//
// 编译：见 20/scripts/build.bat（`-I src`，本文件只依赖 soe_store.h）。
// =====================================================================

#include "soe_store.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

using namespace ems;

static void usage() {
    std::printf(
        "usage: soe_dump --file <path> [--min-level DEBUG|INFO|WARN|ERROR|FATAL]\n"
        "                [--source SYSTEM|FSM|SAFETY|COORD|STRATEGY|DEVICE|COMM|CONFIG|ECON]\n"
        "                [--t0 <sec>] [--t1 <sec>] [--out <path>]\n");
}

int main(int argc, char** argv) {
    std::string file, out;
    std::string min_level_str = "DEBUG";
    std::string source_str;
    double t0 = -1e300, t1 = 1e300;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& dst) -> bool {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        if (a == "--file") { if (!next(file)) { usage(); return 1; } }
        else if (a == "--out") { if (!next(out)) { usage(); return 1; } }
        else if (a == "--min-level") { if (!next(min_level_str)) { usage(); return 1; } }
        else if (a == "--source") { if (!next(source_str)) { usage(); return 1; } }
        else if (a == "--t0") { std::string v; if (!next(v)) { usage(); return 1; } t0 = std::strtod(v.c_str(), nullptr); }
        else if (a == "--t1") { std::string v; if (!next(v)) { usage(); return 1; } t1 = std::strtod(v.c_str(), nullptr); }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); usage(); return 1; }
    }
    if (file.empty()) { usage(); return 1; }

    SoeLevel min_level = SoeLevel::kDebug;
    if (!soe_level_from_name(min_level_str, &min_level)) {
        std::fprintf(stderr, "bad --min-level: %s\n", min_level_str.c_str());
        return 1;
    }
    const bool has_source = !source_str.empty();

    // 用 SoeStore 的检索能力（索引 + 回文件取正文），不自己再写一遍解析
    SoeStore store;
    std::string err;
    if (!store.open(file, &err)) {
        std::fprintf(stderr, "[FAIL] %s\n", err.c_str());
        return 1;
    }

    std::vector<SoeStoreRecord> rows;
    if (has_source) {
        const SoeSource src = SoeStoreRecord::parse_source(source_str);
        rows = store.query(t0, t1, min_level, src);
    } else {
        // 无 source 过滤：先按时间，再按等级
        for (auto& r : store.query_range(t0, t1))
            if (r.level >= min_level) rows.push_back(r);
    }

    std::ofstream fout;
    std::ostream* os = &std::cout;
    if (!out.empty()) {
        fout.open(out.c_str(), std::ios::out | std::ios::binary);
        if (!fout.is_open()) {
            std::fprintf(stderr, "[FAIL] cannot open --out: %s\n", out.c_str());
            return 1;
        }
        os = &fout;
    }

    *os << SoeStore::header_line() << "\n";
    for (const auto& r : rows) *os << r.to_line() << "\n";

    std::printf("file=%s total=%lu matched=%lu min_level=%s\n",
                file.c_str(), (unsigned long)store.size(),
                (unsigned long)rows.size(), min_level_str.c_str());
    if (!out.empty()) std::printf("wrote %s\n", out.c_str());
    return 0;
}
