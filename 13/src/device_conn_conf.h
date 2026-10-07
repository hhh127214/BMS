// =====================================================================
// 13/ — 设备接入参数（**配置通道②**：约定文件路径）
//
// 定位：点表有「配置通道①」（active.csv），本文件补上它的另一半 ——
//   接入参数（IP / 端口 / 从站号）也落成端侧能直接读的文件。
//
//     15/ 表单 ──PUT──▶ 14/ 写 device_conn 表
//                        └─▶ 原子写 config/point-map/active.conn
//                                     └─▶ 本文件读它（不给 --host 时）
//
// 为什么需要（这是接真机时最隐蔽的一类不一致）：
//   接入参数此前**只存在平台数据库里**。用户在两界面上填完 IP，13/ 07/ 却
//   仍然必须靠命令行 `--host 192.168.1.10 --unit 3` 才连得上 —— 于是
//   界面上写着 .10、命令行敲的是 .1，两边都不报错，现场拿到的数据来自
//   一台**没人以为在连**的设备。多敲一次、换个人接手，都会踩。
//
//   ★ 本文件与 active.csv **同目录**（`modbus::default_point_map_dir()`），
//     因为一次装配 = 一组接入参数 + 一份点表（见 14/schema.sql 的
//     device_conn 注释），两者必须来自同一次保存。目录约定只能有一处，
//     多一处就会分叉 —— 所以这里直接复用点表那份，不另立一套。
//
// 三条纪律（与 load_point_map_csv 完全对齐，现场行为才可预期）：
//   ① 文件**不存在** ≠ 错误：这是"现场还没配"的正常状态，调用方按自己的
//      方式提示（probe 报"没有接入参数"，07 报"要么给 --host 要么先在
//      界面上配置"）。**不编一个默认 IP 出来** —— 编出来就会去连它。
//   ② 文件**存在但非法** ≠ 回退：硬失败。静默回退会让「平台写错了参数」与
//      「平台还没写参数」表现得一模一样，而前者会让人以为配置已生效。
//   ③ 未知键**硬失败**：拼错一个键名（hots= / prot=）若被忽略，程序会拿
//      默认值（502 / 从站 1）去连 —— 而屏幕上一切正常。
//
// 格式：`key=value`，`#` 注释，UTF-8（可带 BOM）。
//   与 14/src/connconf.py 的 KEYS / REQUIRED / 取值范围**逐条对齐**。
//
// 编译：纯头文件，C++17。
// =====================================================================

#pragma once

#include "modbus_point_map.h"   // trim_copy / default_point_map_dir（目录约定只有一处）

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ems {
namespace modbus {

// 文件名与格式版本 —— 与 14/src/connconf.py 的 CONN_NAME / FORMAT_TAG
// **逐字一致**。不一致 = 两侧读不到同一份配置。
inline const char* conn_file_name() { return "active.conn"; }
inline const char* conn_format_tag() { return "ems-device-conn/1"; }

// 取值范围 —— 与 14/src/connconf.py 同名同值，改一处必须两边都改
inline int conn_port_min()    { return 1; }
inline int conn_port_max()    { return 65535; }
inline int conn_unit_min()    { return 1; }      // 0 是广播地址，不能作正常从站
inline int conn_unit_max()    { return 247; }
inline int conn_poll_min_ms() { return 10; }
inline int conn_poll_max_ms() { return 60000; }
inline int conn_tmo_min_ms()  { return 10; }
inline int conn_tmo_max_ms()  { return 60000; }

// =====================================================================
// 结构
// =====================================================================
struct DeviceConn {
    std::string device_id;                 // 设备号（与 15/ 界面一致）
    std::string protocol = "modbus_tcp";
    std::string host;                      // 设备 IP 或主机名
    int  port            = 502;
    int  unit_id         = 1;              // 从站地址
    int  poll_period_ms  = 100;            // 轮询周期（与控制周期对齐）
    int  timeout_ms      = 1000;           // 单次请求超时
    bool auto_reconnect  = true;
    bool enabled         = false;          // 0 = 该设备未启用
    std::string saved_at;                  // 元信息（只给人看）
    std::string username;                  // 元信息（只给人看）
};

// 加载结果。三态**刻意分开**：调用方要能区分"没配"与"配错了"，
// 因为给现场的处置建议完全不同（去界面配 / 去修文件）。
enum class ConnLoad {
    kOk,        // 读到了且合法
    kMissing,   // 约定路径下没有这个文件（正常状态，不是错误）
    kInvalid,   // 文件在，但不合法 —— 硬失败
};

inline const char* conn_load_name(ConnLoad s) {
    switch (s) {
    case ConnLoad::kOk:      return "已加载";
    case ConnLoad::kMissing: return "不存在";
    case ConnLoad::kInvalid: return "非法";
    }
    return "?";
}

// =====================================================================
// 路径
// =====================================================================
inline std::string path_in_dir_conn(const std::string& dir) {
#ifdef _WIN32
    return dir + "\\" + conn_file_name();
#else
    return dir + "/" + conn_file_name();
#endif
}

// 与 active.csv 同目录。复用点表那份目录推算（环境变量 EMS_POINT_MAP_DIR
// 优先，否则 <exe 上级两级>/config/point-map）。
inline std::string default_device_conn_path() {
    return path_in_dir_conn(default_point_map_dir());
}

// =====================================================================
// 严格解析（现场配置是手写/Excel 出来的，必须当场拒掉"看起来像"的值）
// =====================================================================
namespace conn_detail {

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
    const std::string t = upper_copy(trim_copy(s));
    if (t == "1" || t == "TRUE" || t == "YES" || t == "ON")  { *out = true;  return true; }
    if (t == "0" || t == "FALSE" || t == "NO" || t == "OFF") { *out = false; return true; }
    *err = std::string(label) + " 应为 0/1，实际是「" + s + "」";
    return false;
}

// 主机名 / IPv4 白名单 —— 与 14/src/connconf.py 的 _RE_HOST 同规则。
// 刻意不做"严格 IPv4 校验"：现场会写设备主机名（pcs-01.local）。
inline bool valid_host(const std::string& s) {
    if (s.empty() || s.size() > 63) return false;
    const char c0 = s[0];
    const bool alnum0 = (c0 >= '0' && c0 <= '9') ||
                        (c0 >= 'A' && c0 <= 'Z') ||
                        (c0 >= 'a' && c0 <= 'z');
    if (!alnum0) return false;
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// 认识的键（顺序 = 落盘顺序，与 14/ 侧一致）
inline bool known_key(const std::string& k) {
    static const char* kKeys[] = {
        "format", "device_id", "protocol", "host", "port", "unit_id",
        "poll_period_ms", "timeout_ms", "auto_reconnect", "enabled",
        "saved_at", "username",
    };
    for (const char* x : kKeys) if (k == x) return true;
    return false;
}

inline std::string known_keys_text() {
    return "format/device_id/protocol/host/port/unit_id/poll_period_ms/"
           "timeout_ms/auto_reconnect/enabled/saved_at/username";
}

}  // namespace conn_detail

// =====================================================================
// 加载
//
// 成功返回 kOk 并填 *out；kMissing / kInvalid 时 *out 不被改动 ——
// 调用方**不能**假定 *out 里有什么可用值（这正是"不编默认值"的落点）。
// report 里是给现场人看的原因 + 处置建议。
//
// ★ 分两层：load_device_conn_path() 收**完整文件路径**；
//   load_device_conn_at() 只收**目录**（拼上约定的 active.conn）。
//   为什么必须分开：`--conn <路径>` 要能指定任意文件名，而早期实现把
//   路径当目录用、又拼回 active.conn —— 结果"文件明明在，程序说找不到"。
// =====================================================================
inline ConnLoad load_device_conn_path(const std::string& path, DeviceConn* out,
                                      std::string* report) {
    if (!file_exists(path.c_str())) {
        if (report) {
            *report = "没有接入参数文件（" + path +
                      "）。配置文件由 14/ 平台在 15/ 界面保存接入参数时原子写出。";
        }
        return ConnLoad::kMissing;
    }

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        if (report) *report = "接入参数文件打不开（权限？）：" + path;
        return ConnLoad::kInvalid;
    }

    std::string seen[12];
    std::string seen_val[12];
    std::size_t seen_n = 0;

    char buf[512];
    int  lineNo = 0;
    bool bad = false;
    std::string why;

    while (!bad && std::fgets(buf, sizeof(buf), f) != nullptr) {
        ++lineNo;
        std::string line = trim_copy(std::string(buf));
        // Excel 另存会带 BOM —— 剥掉，而不是跳过整行
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
        const std::string k = trim_copy(line.substr(0, eq));
        const std::string v = trim_copy(line.substr(eq + 1));

        if (k.empty()) {
            bad = true;
            why = "第 " + std::to_string(lineNo) + " 行的键为空";
            break;
        }
        if (!conn_detail::known_key(k)) {
            bad = true;
            why = "第 " + std::to_string(lineNo) + " 行出现不认识的键「" + k +
                  "」（认识的键：" + conn_detail::known_keys_text() + "）。"
                  "拼错的键若被忽略，程序会拿默认值去连另一台设备 —— 所以直接拒绝。";
            break;
        }
        bool dup = false;
        for (std::size_t i = 0; i < seen_n; ++i) if (seen[i] == k) dup = true;
        if (dup) {
            bad = true;
            why = "第 " + std::to_string(lineNo) + " 行键「" + k + "」重复出现";
            break;
        }
        if (seen_n < 12) { seen[seen_n] = k; seen_val[seen_n] = v; ++seen_n; }
    }
    std::fclose(f);

    if (bad) {
        if (report) *report = why;
        return ConnLoad::kInvalid;
    }

    auto get = [&](const char* k, bool* found) -> std::string {
        for (std::size_t i = 0; i < seen_n; ++i) {
            if (seen[i] == k) { *found = true; return seen_val[i]; }
        }
        *found = false;
        return std::string();
    };

    // ---- 必填键（与 14/ 侧 REQUIRED 逐条对齐）----
    static const char* kRequired[] = {
        "format", "device_id", "protocol", "host", "port", "unit_id",
        "poll_period_ms", "timeout_ms", "auto_reconnect", "enabled",
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
            *report = "接入参数缺少必填键：" + missing +
                      "（文件可能被手工截断，或来自不同版本的端侧）";
        }
        return ConnLoad::kInvalid;
    }

    bool found = false;
    DeviceConn c;

    const std::string fmt = get("format", &found);
    if (fmt != conn_format_tag()) {
        if (report) {
            *report = std::string("文件格式版本不匹配：期望「") + conn_format_tag() +
                      "」，实际是「" + fmt + "」";
        }
        return ConnLoad::kInvalid;
    }

    c.device_id = get("device_id", &found);
    if (c.device_id.empty()) {
        if (report) *report = "device_id 为空";
        return ConnLoad::kInvalid;
    }

    c.protocol = get("protocol", &found);
    if (c.protocol != "modbus_tcp") {
        if (report) {
            *report = "协议只支持 modbus_tcp，实际是「" + c.protocol + "」";
        }
        return ConnLoad::kInvalid;
    }

    c.host = get("host", &found);
    if (!conn_detail::valid_host(c.host)) {
        if (report) {
            *report = "host 应是 IPv4 或主机名（只允许字母数字与 . _ -），实际是「"
                      + c.host + "」";
        }
        return ConnLoad::kInvalid;
    }

    struct Num { const char* key; int lo; int hi; int* dst; const char* label; };
    const Num nums[] = {
        {"port",           conn_port_min(),  conn_port_max(),  &c.port,           "端口(port)"},
        {"unit_id",        conn_unit_min(),  conn_unit_max(),  &c.unit_id,        "从站号(unit_id)"},
        {"poll_period_ms", conn_poll_min_ms(), conn_poll_max_ms(), &c.poll_period_ms, "轮询周期(poll_period_ms)"},
        {"timeout_ms",     conn_tmo_min_ms(), conn_tmo_max_ms(), &c.timeout_ms,    "超时(timeout_ms)"},
    };
    for (const Num& n : nums) {
        if (!conn_detail::parse_int(get(n.key, &found), n.lo, n.hi, n.dst,
                                    report, n.label)) {
            return ConnLoad::kInvalid;
        }
    }
    if (!conn_detail::parse_flag(get("auto_reconnect", &found), &c.auto_reconnect,
                                 report, "自动重连(auto_reconnect)")) {
        return ConnLoad::kInvalid;
    }
    if (!conn_detail::parse_flag(get("enabled", &found), &c.enabled,
                                 report, "启用(enabled)")) {
        return ConnLoad::kInvalid;
    }

    c.saved_at = get("saved_at", &found);   // 元信息，缺了不影响
    c.username = get("username", &found);

    if (out) *out = c;
    if (report) *report = "ok";
    return ConnLoad::kOk;
}

// 按约定目录加载（文件名固定 active.conn）
inline ConnLoad load_device_conn_at(const std::string& dir, DeviceConn* out,
                                    std::string* report, std::string* used_path) {
    const std::string path = path_in_dir_conn(dir);
    if (used_path) *used_path = path;
    if (!file_exists(path.c_str())) {
        if (report) {
            *report = "约定路径下没有接入参数文件（" + path +
                      "）。这不是错误 —— 现场还没在界面上配置接入参数。"
                      "配置后本文件会由 14/ 平台原子写出。";
        }
        return ConnLoad::kMissing;   // ① 不是错误
    }
    return load_device_conn_path(path, out, report);
}

inline ConnLoad load_device_conn_default(DeviceConn* out, std::string* report,
                                         std::string* used_path) {
    return load_device_conn_at(default_point_map_dir(), out, report, used_path);
}

// 打印用（probe 的 --dump-conn / 现场核对）
inline std::string format_device_conn(const DeviceConn& c) {
    return "设备号 " + c.device_id + "  协议 " + c.protocol +
           "  地址 " + c.host + ":" + std::to_string(c.port) +
           "  从站 " + std::to_string(c.unit_id) +
           "  轮询 " + std::to_string(c.poll_period_ms) + "ms" +
           "  超时 " + std::to_string(c.timeout_ms) + "ms" +
           "  自动重连 " + (c.auto_reconnect ? "是" : "否") +
           "  启用 " + (c.enabled ? "是" : "否");
}

}  // namespace modbus
}  // namespace ems
