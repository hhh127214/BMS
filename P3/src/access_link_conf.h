// =====================================================================
// P3/ — 上端链路参数（**配置通道②**：约定文件路径）
//
// 定位：IEC104 从站的「监听口 / 公共地址 / 谁连进来」此前**只存在命令行里**。
//   14/ 平台在界面上写 2404，现场敲 `--port 2405` 也没人拦 —— 调度收不到数，
//   平台上一切正常。这与 13/ 当初引入 active.conn 的处境完全相同，处置也相同：
//
//     15/ 表单 ──PUT──▶ 14/ 写 upstream_link 表
//                        └─▶ 原子写 config/point-map/active.upstream
//                                     └─▶ 本文件读它（不给 --port 时）
//
// ★ 与 active.csv / active.conn **同目录**（`..\..\config\point-map`）：
//   一次现场装配 = 上端一条链路 + 下端一组接入参数 + 一份点表，三者同生共死。
//   目录约定只允许有一处（环境变量 EMS_POINT_MAP_DIR 同时覆盖三者）。
//
// 三条纪律（与 13/src/device_conn_conf.h 逐条一致，现场行为才可预期）：
//   ① 文件**不存在** ≠ 错误：现场还没配，调用方按自己的方式提示。
//      **不编一个默认端口出来** —— 编出来就会去监听它。
//   ② 文件**存在但非法** ≠ 回退：硬失败。静默回退会让「平台写错了」与
//      「平台还没写」表现得一模一样。
//   ③ 未知键**硬失败**：拼错一个键名（`listen_prot=`）若被忽略，从站会拿
//      默认 2404 去监听，而调度连的是别的口 —— 屏幕上一切正常。
//
// 格式：`key=value`，`#` 注释，UTF-8（可带 BOM）。
//   与 14/src/uplinkconf.py 的 KEYS / REQUIRED / 取值范围**逐条对齐**。
//
// 编译：纯头文件，C++17。
// =====================================================================

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>            // GetModuleFileNameA（目录约定：与点表同一份规则）
#endif

namespace iec104 {

// 文件名与格式版本 —— 与 14/src/uplinkconf.py 的 LINK_NAME / FORMAT_TAG 逐字一致
inline const char* link_file_name() { return "active.upstream"; }
inline const char* link_format_tag() { return "ems-upstream-link/1"; }

inline int link_port_default() { return 2404; }   // IEC 60870-5-104 标准端口
inline int link_ca_min()       { return 1; }
inline int link_ca_max()       { return 65534; }  // 0 与 65535 保留

// =====================================================================
// 结构
// =====================================================================
struct UpstreamLink {
    std::string link_id     = "dispatch";
    std::string kind        = "real";        // real = 上级调度；sim = 模拟调度主站
    std::string protocol    = "iec104";
    std::string bind_host   = "0.0.0.0";
    int         listen_port = 2404;
    int         common_addr = 1;             // ASDU 公共地址 CA
    std::string master_host = "127.0.0.1";   // kind=sim 时谁当模拟主站
    bool        enabled     = true;
    std::string saved_at;                    // 元信息（只给人看）
    std::string username;
};

// 三态**刻意分开**：调用方要能区分"没配"与"配错了"（处置建议完全不同）
enum class LinkLoad {
    kOk,
    kMissing,
    kInvalid,
};

inline const char* link_load_name(LinkLoad s) {
    switch (s) {
    case LinkLoad::kOk:      return "已加载";
    case LinkLoad::kMissing: return "不存在";
    case LinkLoad::kInvalid: return "非法";
    }
    return "?";
}

// =====================================================================
// 路径
// =====================================================================
inline std::string link_path_in_dir(const std::string& dir) {
#ifdef _WIN32
    return dir + "\\" + link_file_name();
#else
    return dir + "/" + link_file_name();
#endif
}

// 与 active.csv / active.conn 同目录。规则与 13/src/modbus_point_map.h 的
// default_point_map_dir() 一致：环境变量优先，否则 <exe 上级两级>\config\point-map。
inline std::string default_link_dir() {
    const char* env = std::getenv("EMS_POINT_MAP_DIR");
    if (env != nullptr && *env != '\0') return std::string(env);
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    const DWORD n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string exe(buf, static_cast<std::size_t>(n));
    const std::size_t slash = exe.find_last_of("\\/");
    const std::string dir =
        (slash == std::string::npos) ? std::string(".") : exe.substr(0, slash);
    return dir + "\\..\\..\\config\\point-map";
#else
    return std::string("config/point-map");
#endif
}

inline std::string default_link_path() {
    return link_path_in_dir(default_link_dir());
}

// =====================================================================
// 转发表（IOA ↔ 点名）的约定路径 —— 2026-10-18 补，见 CHANGES.md §84
//
// 为什么以前只有链路、没有转发表的约定路径：
//   链路参数（`active.upstream`）早就能由平台维护，唯独转发表只能靠命令行
//   `--point-map` 给 —— 于是"这一次该用哪份 IOA 表"仍然是启动网关那个人
//   脑子里的东西。2026-09-20 把转发表从代码里外置出来，改掉了"改一个 IOA
//   就要重编 C++"，但没改掉"还得有人记得敲那串参数"这一半。
//
//   补齐的办法与另外三条完全同构：平台保存到约定文件，端侧启动时读。
//   当初拒绝给写入能力的理由是怕出现第二个真相源 —— 那个担心是对的，
//   但正确答案是**把真相源收敛成一个文件**，不是让界面永远只读：
//   有了本约定之后，界面写的就是网关读的那一份，不再是两份。
//
// ★ 与 active.csv / active.conn / active.upstream **同一个目录**：
//   上端完整的一次装配 = 一条链路（连得上）+ 一张转发表（读得对）。
//   两者不在同一处就可能出现"按新链路连上、按旧表在发"的混合态。
//
// 加载优先级（见 main_gateway.cpp）：
//   ① 命令行 `--point-map F`（临时核对用，优先）
//   ② 本文件返回的约定路径（平台写的那份）
//   ③ 都没有 → 内置默认表（编译进去的 24 上送 + 6 下行）
// =====================================================================
inline const char* point_map_file_name() { return "active.pointmap.csv"; }

inline std::string point_map_path_in_dir(const std::string& dir) {
#ifdef _WIN32
    return dir + "\\" + point_map_file_name();
#else
    return dir + "/" + point_map_file_name();
#endif
}

inline std::string default_point_map_path() {
    return point_map_path_in_dir(default_link_dir());
}

// 文件在不在 —— 端侧只做这一个判断，不做"半存在"的猜测。
// 判据是"能不能真正打开"，不是"stat 到元数据"：前者与端侧进程实际的
// 可读性一致（Windows 上被独占打开时会直接失败）。
inline bool file_exists(const char* path) {
    if (path == nullptr || *path == '\0') return false;
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    std::fclose(f);
    return true;
}

// =====================================================================
// 严格解析
// =====================================================================
namespace link_detail {

inline std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' ||
                     s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

inline std::string upper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

inline bool valid_host(const std::string& s) {
    if (s.empty() || s.size() > 63) return false;
    const char c0 = s[0];
    const bool alnum0 = (c0 >= '0' && c0 <= '9') || (c0 >= 'A' && c0 <= 'Z') ||
                        (c0 >= 'a' && c0 <= 'z');
    if (!alnum0) return false;
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

inline bool parse_int(const std::string& s, int lo, int hi, int* out,
                      std::string* err, const char* label) {
    if (s.empty()) { *err = std::string(label) + " 取值为空"; return false; }
    for (char c : s) {
        if (c < '0' || c > '9') {
            *err = std::string(label) + " 应为非负整数，实际是「" + s + "」";
            return false;
        }
    }
    const long v = std::strtol(s.c_str(), nullptr, 10);
    if (v < static_cast<long>(lo) || v > static_cast<long>(hi)) {
        *err = std::string(label) + " 应在 " + std::to_string(lo) + ".." +
               std::to_string(hi) + " 之间，实际是 " + s;
        return false;
    }
    *out = static_cast<int>(v);
    return true;
}

inline bool parse_flag(const std::string& s, bool* out, std::string* err,
                       const char* label) {
    const std::string t = upper(trim(s));
    if (t == "1" || t == "TRUE" || t == "YES" || t == "ON")  { *out = true;  return true; }
    if (t == "0" || t == "FALSE" || t == "NO" || t == "OFF") { *out = false; return true; }
    *err = std::string(label) + " 应为 0/1，实际是「" + s + "」";
    return false;
}

inline bool known_key(const std::string& k) {
    static const char* kKeys[] = {
        "format", "link_id", "kind", "protocol", "bind_host",
        "listen_port", "common_addr", "master_host", "enabled",
        "saved_at", "username",
    };
    for (const char* x : kKeys) if (k == x) return true;
    return false;
}

inline std::string known_keys_text() {
    return "format/link_id/kind/protocol/bind_host/listen_port/common_addr/"
           "master_host/enabled/saved_at/username";
}

}  // namespace link_detail

// =====================================================================
// 加载
//
// kMissing / kInvalid 时 *out **不被改动** —— 调用方不能假定里面有可用值。
// =====================================================================
inline LinkLoad load_upstream_link_path(const std::string& path, UpstreamLink* out,
                                        std::string* report) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        if (report) {
            *report = "没有上端链路参数文件（" + path +
                      "）。它由 14/ 平台在 15/ 界面保存上端接入时原子写出。";
        }
        return LinkLoad::kMissing;
    }

    std::string seen[11];
    std::string seen_val[11];
    std::size_t seen_n = 0;

    char buf[512];
    int  lineNo = 0;
    bool bad = false;
    std::string why;

    while (!bad && std::fgets(buf, sizeof(buf), f) != nullptr) {
        ++lineNo;
        std::string line = link_detail::trim(std::string(buf));
        if (line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line = line.substr(3);
        }
        if (line.empty() || line[0] == '#') continue;

        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            bad = true;
            why = "第 " + std::to_string(lineNo) + " 行不是 key=value 形式："
                  + line.substr(0, 60);
            break;
        }
        const std::string k = link_detail::trim(line.substr(0, eq));
        const std::string v = link_detail::trim(line.substr(eq + 1));
        if (k.empty()) {
            bad = true;
            why = "第 " + std::to_string(lineNo) + " 行的键为空";
            break;
        }
        if (!link_detail::known_key(k)) {
            bad = true;
            why = "第 " + std::to_string(lineNo) + " 行出现不认识的键「" + k +
                  "」（认识的键：" + link_detail::known_keys_text() + "）。"
                  "拼错的键若被忽略，从站会拿默认端口去监听 —— 所以直接拒绝。";
            break;
        }
        bool dup = false;
        for (std::size_t i = 0; i < seen_n; ++i) if (seen[i] == k) dup = true;
        if (dup) {
            bad = true;
            why = "第 " + std::to_string(lineNo) + " 行键「" + k + "」重复出现";
            break;
        }
        if (seen_n < 11) { seen[seen_n] = k; seen_val[seen_n] = v; ++seen_n; }
    }
    std::fclose(f);

    if (bad) {
        if (report) *report = why;
        return LinkLoad::kInvalid;
    }

    auto get = [&](const char* k, bool* found) -> std::string {
        for (std::size_t i = 0; i < seen_n; ++i) {
            if (seen[i] == k) { *found = true; return seen_val[i]; }
        }
        *found = false;
        return std::string();
    };

    // ---- 必填键（与 14/src/uplinkconf.py 的 REQUIRED 逐条对齐）----
    static const char* kRequired[] = {
        "format", "link_id", "kind", "protocol",
        "listen_port", "common_addr", "enabled",
    };
    std::string missing;
    for (const char* k : kRequired) {
        bool found = false;
        get(k, &found);
        if (!found) {
            if (!missing.empty()) missing += "/";
            missing += k;
        }
    }
    if (!missing.empty()) {
        if (report) {
            *report = "上端链路参数缺少必填键：" + missing +
                      "（文件可能被手工截断，或来自不同版本的端侧）";
        }
        return LinkLoad::kInvalid;
    }

    bool found = false;
    UpstreamLink c;

    const std::string fmt = get("format", &found);
    if (fmt != link_format_tag()) {
        if (report) {
            *report = std::string("文件格式版本不匹配：期望「") + link_format_tag() +
                      "」，实际是「" + fmt + "」";
        }
        return LinkLoad::kInvalid;
    }

    c.link_id = get("link_id", &found);
    if (c.link_id != "dispatch") {
        if (report) {
            *report = "link_id 只能是 dispatch（一个从站只有一条上端链路），实际是「" +
                      c.link_id + "」";
        }
        return LinkLoad::kInvalid;
    }

    c.kind = get("kind", &found);
    if (c.kind != "real" && c.kind != "sim") {
        if (report) *report = "kind 只支持 real/sim，实际是「" + c.kind + "」";
        return LinkLoad::kInvalid;
    }

    c.protocol = get("protocol", &found);
    if (c.protocol != "iec104") {
        if (report) *report = "protocol 只支持 iec104，实际是「" + c.protocol + "」";
        return LinkLoad::kInvalid;
    }

    c.bind_host = get("bind_host", &found);
    if (c.bind_host.empty()) c.bind_host = "0.0.0.0";
    if (!link_detail::valid_host(c.bind_host)) {
        if (report) {
            *report = "bind_host 应是 IPv4 或 0.0.0.0，实际是「" + c.bind_host + "」";
        }
        return LinkLoad::kInvalid;
    }

    if (!link_detail::parse_int(get("listen_port", &found), 1, 65535,
                                &c.listen_port, report, "监听端口(listen_port)")) {
        return LinkLoad::kInvalid;
    }
    if (!link_detail::parse_int(get("common_addr", &found), link_ca_min(),
                                link_ca_max(), &c.common_addr, report,
                                "公共地址(common_addr)")) {
        return LinkLoad::kInvalid;
    }

    c.master_host = get("master_host", &found);
    if (c.master_host.empty()) c.master_host = "127.0.0.1";
    if (!link_detail::valid_host(c.master_host)) {
        if (report) {
            *report = "master_host 应是 IPv4 或主机名，实际是「" + c.master_host + "」";
        }
        return LinkLoad::kInvalid;
    }

    if (!link_detail::parse_flag(get("enabled", &found), &c.enabled,
                                 report, "启用(enabled)")) {
        return LinkLoad::kInvalid;
    }

    c.saved_at = get("saved_at", &found);    // 元信息，缺了不影响
    c.username = get("username", &found);

    if (out) *out = c;
    if (report) *report = "ok";
    return LinkLoad::kOk;
}

inline LinkLoad load_upstream_link_default(UpstreamLink* out, std::string* report,
                                           std::string* used_path) {
    const std::string path = default_link_path();
    if (used_path) *used_path = path;
    return load_upstream_link_path(path, out, report);
}

// 这一端到底「跟谁对接」—— 给启动横幅用，别让现场从 kind 字段自己推
inline bool link_is_sim(const UpstreamLink& c) { return c.kind == "sim"; }

inline const char* link_peer_text(const UpstreamLink& c) {
    return link_is_sim(c) ? "本地模拟调度主站（iec104_master.exe）"
                          : "上级调度主站（对方发起连接）";
}

// 打印用（--dump-link / 启动横幅）
inline std::string format_upstream_link(const UpstreamLink& c) {
    return std::string(link_is_sim(c) ? "形态 模拟调度(★ 不是上级调度)" : "形态 上级调度") +
           "  监听 " + c.bind_host + ":" + std::to_string(c.listen_port) +
           "  CA " + std::to_string(c.common_addr) +
           "  对端 " + link_peer_text(c) +
           "  启用 " + (c.enabled ? "是" : "否");
}

}  // namespace iec104
