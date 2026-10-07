// =====================================================================
// 20/ — 配置持久化 ConfigStore
//
// ---------------------------------------------------------------------
// 要解决的事（交接文档 §3.6「④」）
// ---------------------------------------------------------------------
// `04/` 的配置只有 **in-memory + set_param**：
//     `StrategyManager::set_param(id, key, value)`（04/src/strategy_manager.h:125）
//     `IStrategy::set_param()` 把值写进 `params_`（04/src/strategy_base.h:69）
// 进程一退，现场调的全部参数就没了 —— 只能重启后再调一遍。
//
// 本文件做三件事：
//   ① **落盘 + 回读**：字段级往返，逐字段一致；
//   ② **版本号 + 变更台账**：每个字段"谁在什么时候改成什么"可回溯；
//   ③ **损坏 / 缺字段显式报错，且指出是哪个字段** ——
//      不允许静默用默认值。这一条与 A1 的教训同型：
//      `11/docs/README.md` §5.5 记着"设备侧发布的 MEAS.P_GRID 恰好等于相减式，
//      改成正确实现后在所有夹具下逐位恒等"，即**静默降级会掩盖缺陷**。
//      配置缺字段若静默取默认值，现场表现就是"EMS 用错误参数在跑"。
//
// ---------------------------------------------------------------------
// 文件格式（稳定契约；UTF-8，LF）
// ---------------------------------------------------------------------
//     # EMS_CONFIG v1
//     VERSION|1
//     FIELD|<group>|<name>|<value>
//     CHANGE|<t>|<who>|<group>.<name>|<old>|<new>|<created 0/1>
//
//   · group 用策略 id（如 S07_PEAK_VALLEY），name 用参数名（如 P_discharge）
//     —— 与 04/ 的 `set_param(id, key, value)` 一一对应；
//   · group / name / who 中**不允许出现 `|`**（解析器按 `|` 切分）；
//   · value 用 `%.10g`，可往返。
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace ems {

struct ConfigField {
    std::string group;
    std::string name;
    double      value = 0.0;
    std::string full() const { return group + "." + name; }
};

struct ConfigChange {
    double      t = 0.0;
    std::string who;
    std::string field;        // group.name
    double      old_value = 0.0;
    double      new_value = 0.0;
    bool        created = false;   // true = 该字段此前不存在
};

struct ConfigLoadResult {
    bool        ok = false;
    std::string error;                             // 第一条错误（已含字段/行号）
    std::vector<std::string> missing_fields;       // 缺了哪些必填字段
    std::vector<std::string> unknown_fields;       // 文件里有、必填清单里没有
    std::string version;
    int         corrupt_line = 0;                  // 损坏行号（0 = 无）
};

class ConfigStore {
public:
    static const char* current_version() { return "1"; }

    explicit ConfigStore(std::string path) : path_(std::move(path)) {}

    // ---- 必填字段清单（load 时缺一即失败）----
    void require(const std::string& group, const std::string& name) {
        required_.push_back(group + "." + name);
    }
    void require_field(const std::string& full) { required_.push_back(full); }
    const std::vector<std::string>& required() const { return required_; }

    // ---- 写入 ----
    // 记一条变更台账：字段 + 旧值 + 新值 + 时间 + 谁。
    // 值未变则不记（避免噪声）。
    bool set(const std::string& group, const std::string& name, double value,
             const std::string& who, double t, std::string* err = nullptr) {
        if (group.empty() || name.empty()) {
            if (err) *err = "empty group/name";
            return false;
        }
        if (group.find('|') != std::string::npos || name.find('|') != std::string::npos ||
            who.find('|') != std::string::npos) {
            if (err) *err = "group/name/who must not contain '|'";
            return false;
        }
        ConfigField* f = find(group, name);
        if (f == nullptr) {
            ConfigField nf;
            nf.group = group; nf.name = name; nf.value = value;
            fields_.push_back(nf);
            ConfigChange c;
            c.t = t; c.who = who; c.field = group + "." + name;
            c.old_value = 0.0; c.new_value = value; c.created = true;
            changes_.push_back(c);
            return true;
        }
        if (f->value == value) return true;
        ConfigChange c;
        c.t = t; c.who = who; c.field = f->full();
        c.old_value = f->value; c.new_value = value; c.created = false;
        changes_.push_back(c);
        f->value = value;
        return true;
    }

    // 不记台账的批量赋值（load 内部 / 初始化模板用）
    void put_silent(const std::string& group, const std::string& name, double value) {
        ConfigField* f = find(group, name);
        if (f) { f->value = value; return; }
        ConfigField nf; nf.group = group; nf.name = name; nf.value = value;
        fields_.push_back(nf);
    }

    // ---- 读取 ----
    // ★ 没有默认值：字段不存在 → 返回 false。**不允许**静默用默认值。
    bool get(const std::string& group, const std::string& name, double* out) const {
        for (const auto& f : fields_) {
            if (f.group == group && f.name == name) { if (out) *out = f.value; return true; }
        }
        return false;
    }
    bool has(const std::string& group, const std::string& name) const {
        return find(group, name) != nullptr;
    }
    size_t size() const { return fields_.size(); }
    const std::vector<ConfigField>&  fields()  const { return fields_; }
    const std::vector<ConfigChange>& changes() const { return changes_; }
    const std::string& path() const { return path_; }
    const std::string& file_version() const { return version_; }

    // ---- 落盘 ----
    bool save(std::string* err = nullptr) const {
        std::ofstream f(path_.c_str(), std::ios::out | std::ios::binary);
        if (!f.is_open()) { if (err) *err = "cannot write: " + path_; return false; }
        f << "# EMS_CONFIG v" << current_version() << "\n";
        f << "VERSION|" << current_version() << "\n";
        for (const auto& e : fields_) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.10g", e.value);
            f << "FIELD|" << e.group << "|" << e.name << "|" << buf << "\n";
        }
        for (const auto& c : changes_) {
            char o[64], n[64], t[64];
            std::snprintf(t, sizeof(t), "%.6f", c.t);
            std::snprintf(o, sizeof(o), "%.10g", c.old_value);
            std::snprintf(n, sizeof(n), "%.10g", c.new_value);
            f << "CHANGE|" << t << "|" << c.who << "|" << c.field << "|"
              << o << "|" << n << "|" << (c.created ? 1 : 0) << "\n";
        }
        if (!f.good()) { if (err) *err = "write failed: " + path_; return false; }
        return true;
    }

    // ---- 回读 ----
    //
    // 失败时 result.error **必须指出是哪个字段 / 哪一行**。
    // 成功时用文件内容**替换**内存字段（含台账）。
    ConfigLoadResult load() {
        ConfigLoadResult r;
        std::ifstream f(path_.c_str(), std::ios::in | std::ios::binary);
        if (!f.is_open()) {
            r.error = "file not found: " + path_;
            return r;
        }

        std::vector<ConfigField>  fields;
        std::vector<ConfigChange> changes;
        std::map<std::string, bool> seen;
        std::string line;
        int lineno = 0;
        bool header_ok = false;
        while (std::getline(f, line)) {
            ++lineno;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (line[0] == '#') { header_ok = true; continue; }

            std::vector<std::string> parts = split(line, '|');
            if (parts.empty()) continue;

            if (parts[0] == "VERSION") {
                if (parts.size() < 2) { r.error = "line " + std::to_string(lineno) + ": bad VERSION"; r.corrupt_line = lineno; return r; }
                r.version = parts[1];
                if (parts[1] != current_version()) {
                    r.error = "version mismatch: file='" + parts[1] +
                              "' expected='" + current_version() + "'";
                    r.corrupt_line = lineno;
                    return r;
                }
                continue;
            }

            if (parts[0] == "FIELD") {
                if (parts.size() < 4) {
                    r.error = "line " + std::to_string(lineno) + ": FIELD needs 4 fields";
                    r.corrupt_line = lineno;
                    return r;
                }
                ConfigField cf;
                cf.group = parts[1]; cf.name = parts[2];
                cf.value = std::strtod(parts[3].c_str(), nullptr);
                const std::string key = cf.full();
                if (seen.count(key)) {
                    r.error = "line " + std::to_string(lineno) +
                              ": duplicate field '" + key + "'";
                    r.corrupt_line = lineno;
                    return r;
                }
                seen[key] = true;
                fields.push_back(cf);
                continue;
            }

            if (parts[0] == "CHANGE") {
                if (parts.size() < 7) {
                    r.error = "line " + std::to_string(lineno) + ": CHANGE needs 7 fields";
                    r.corrupt_line = lineno;
                    return r;
                }
                ConfigChange c;
                c.t         = std::strtod(parts[1].c_str(), nullptr);
                c.who       = parts[2];
                c.field     = parts[3];
                c.old_value = std::strtod(parts[4].c_str(), nullptr);
                c.new_value = std::strtod(parts[5].c_str(), nullptr);
                c.created   = (parts[6] == "1");
                changes.push_back(c);
                continue;
            }

            r.error = "line " + std::to_string(lineno) + ": unknown record type '" +
                      parts[0] + "'";
            r.corrupt_line = lineno;
            return r;
        }

        if (!header_ok) {
            r.error = "missing header comment (# EMS_CONFIG v...)";
            return r;
        }

        // 必填字段校验 —— 缺哪个就点名哪个，**不取默认值**
        for (const auto& req : required_) {
            if (!seen.count(req)) {
                r.missing_fields.push_back(req);
            }
        }
        // 文件里有、但不在必填清单里的字段（当清单非空时）
        if (!required_.empty()) {
            for (const auto& fld : fields) {
                bool known = false;
                for (const auto& req : required_) if (req == fld.full()) { known = true; break; }
                if (!known) r.unknown_fields.push_back(fld.full());
            }
        }

        if (!r.missing_fields.empty()) {
            r.error = "missing required field: " + r.missing_fields[0];
            if (r.missing_fields.size() > 1)
                r.error += " (and " + std::to_string(r.missing_fields.size() - 1) + " more)";
            return r;
        }

        // 成功：以文件为准替换内存
        fields_  = fields;
        changes_ = changes;
        version_ = r.version;
        r.ok = true;
        return r;
    }

private:
    ConfigField* find(const std::string& group, const std::string& name) {
        for (auto& f : fields_)
            if (f.group == group && f.name == name) return &f;
        return nullptr;
    }
    const ConfigField* find(const std::string& group, const std::string& name) const {
        for (const auto& f : fields_)
            if (f.group == group && f.name == name) return &f;
        return nullptr;
    }
    static std::vector<std::string> split(const std::string& s, char d) {
        std::vector<std::string> v;
        size_t b = 0;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == d) { v.push_back(s.substr(b, i - b)); b = i + 1; }
        }
        return v;
    }

    std::string path_;
    std::vector<ConfigField>  fields_;
    std::vector<ConfigChange> changes_;
    std::vector<std::string>  required_;
    std::string version_ = current_version();
};

} // namespace ems
