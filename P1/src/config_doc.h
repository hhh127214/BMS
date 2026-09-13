// =====================================================================
// P1/ — 产品化 P1 配置化 · 配置模板 / 文档生成
//
// 配置文档最大的风险是「**和代码漂移**」：加了个参数忘了改文档，
// 现场按文档配了半天不生效。本文件的解法是**不手写文档** ——
// 文档由 `Binder` 字段表（ems_config.h）直接渲染：
//
//   字段表 = 唯一事实来源
//      ├─► load / save（config_loader.h）
//      ├─► 配置模板（带注释的 JSON，可直接改完就用）
//      └─► Markdown 文档（键 / 单位 / 默认值 / 必填 / 说明）
//
// 改了 bind_fields()，模板和文档**自动**跟着变。这是 P1 的"顺手收益"。
//
// 用法（见 P1/src/main.cpp）：
//     ems-config --dump-template > ems_config.json   # 生成可用的配置模板
//     ems-config --dump-doc > CONFIG.md              # 生成配置说明文档
//     ems-config --dump-schema                       # 只输出字段清单（JSON）
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "config_loader.h"

#include <sstream>
#include <string>

namespace ems {

// =====================================================================
// 模板：带注释的 JSON
//
// 为什么用带注释的 JSON 而不是"纯 JSON + 单独文档"：
//   现场工程师改配置时只打开一个文件。注释就在字段上方，
//   单位、范围、注意事项一眼可见，不用在两份文件之间来回翻。
//   本模块的 json_lite.h 支持注释，所以这个文件能被原样读回。
// =====================================================================
namespace doc_detail {

inline void comment_line(std::ostringstream& os, const std::string& pad,
                         const Binder::Field& f) {
    std::string c;
    if (!f.desc.empty()) c += f.desc;
    if (!f.unit.empty() && f.unit != "1") {
        if (!c.empty()) c += "  ";
        c += "[" + f.unit + "]";
    }
    if (f.required) {
        if (!c.empty()) c += "  ";
        c += "★关键项";
    }
    if (!c.empty()) os << pad << "// " << c << "\n";
}

// 单个标量字段 → `"key": value,`
inline void field_line(std::ostringstream& os, const std::string& pad,
                       const Binder::Field& f) {
    comment_line(os, pad, f);
    os << pad << json::detail::escape(f.key) << ": ";
    const json::Value v = f.get();
    if (v.is_bool())        os << (v.b ? "true" : "false");
    else if (v.is_number()) os << json::detail::num_to_string(v.num);
    else if (v.is_string()) os << json::detail::escape(v.str);
    else                    os << "null";
    os << ",\n";
}

template <class T>
inline void section(std::ostringstream& os, const std::string& key, T& obj,
                    bool trailing_comma = true) {
    Binder b;
    bind_fields(b, obj);
    os << "  " << json::detail::escape(key) << ": {\n";
    for (const auto& f : b.fields()) field_line(os, "    ", f);
    os << "  }" << (trailing_comma ? "," : "") << "\n";
}

} // namespace doc_detail

// base 提供当前（默认）值；rt 提供已注册策略 id 列表（可为 nullptr）
inline std::string render_config_template(EmsRuntime* rt, const EmsConfig& base_in) {
    EmsConfig base = base_in;
    std::ostringstream os;
    os << "// =====================================================================\n"
       << "// EMS 现场配置模板 —— 由 `ems-config --dump-template` 自动生成\n"
       << "//\n"
       << "// 本文件支持注释（// 与 #）与尾逗号，可直接编辑后交给现场部署使用：\n"
       << "//     ems-config --apply <本文件> --check\n"
       << "//\n"
       << "// 标 ★ 的是关键项：缺省不会报错，但会给出提示并使用默认值。\n"
       << "// 单位标注在方括号里；单位 1 表示无量纲（比例 / 系数）。\n"
       << "// =====================================================================\n"
       << "{\n";

    os << "  // ---- 元信息（不参与控制，用于归档 / 审计 / 交接）----\n";
    os << "  \"name\": "        << json::detail::escape(base.name)        << ",\n";
    os << "  \"description\": " << json::detail::escape(base.description) << ",\n";
    os << "  \"site\": "        << json::detail::escape(base.site)        << ",\n";
    os << "  \"version\": "     << json::detail::escape(base.version)     << ",\n\n";

    doc_detail::section(os, config_keys::kLoop,   base.loop);
    doc_detail::section(os, config_keys::kPlant,  base.plant);
    doc_detail::section(os, config_keys::kLimits, base.limits);
    doc_detail::section(os, config_keys::kSafety, base.safety);
    doc_detail::section(os, config_keys::kCoord,  base.coord);
    doc_detail::section(os, config_keys::kFsm,    base.fsm);

    os << "\n  // ---- 策略：id → {enabled, note, params} ----\n"
       << "  // 参数名由各策略自己解释（P1 不校验参数名，只校验 id 是否存在）。\n"
       << "  // 下面按已注册策略列出骨架，params 留空表示使用策略内置默认值。\n";
    os << "  \"strategies\": {\n";
    std::vector<std::string> ids;
    if (rt != nullptr) ids = rt->manager().list_ids();
    std::sort(ids.begin(), ids.end());
    for (size_t i = 0; i < ids.size(); ++i) {
        os << "    " << json::detail::escape(ids[i]) << ": {\n"
           << "      \"enabled\": true,\n"
           << "      \"note\": \"\",\n"
           << "      \"params\": {}\n"
           << "    }" << (i + 1 < ids.size() ? "," : "") << "\n";
    }
    os << "  }\n";
    os << "}\n";
    return os.str();
}

// =====================================================================
// Markdown 文档
// =====================================================================
namespace doc_detail {

template <class T>
inline void md_section(std::ostringstream& os, const std::string& title,
                       const std::string& key, T& obj) {
    Binder b;
    bind_fields(b, obj);
    os << "### `" << key << "` —— " << title << "\n\n";
    os << "| 键 | 单位 | 默认值 | 关键 | 说明 |\n";
    os << "|---|---|---|---|---|\n";
    for (const auto& f : b.fields()) {
        const json::Value v = f.get();
        std::string def;
        if (v.is_bool())        def = v.b ? "true" : "false";
        else if (v.is_number()) def = json::detail::num_to_string(v.num);
        else if (v.is_string()) def = "`" + v.str + "`";
        else                    def = "-";
        os << "| `" << f.key << "` | "
           << (f.unit.empty() ? "-" : f.unit) << " | "
           << def << " | "
           << (f.required ? "★" : "") << " | "
           << (f.desc.empty() ? "-" : f.desc) << " |\n";
    }
    os << "\n";
}

} // namespace doc_detail

inline std::string render_config_markdown(EmsRuntime* rt, const EmsConfig& base_in) {
    EmsConfig base = base_in;
    std::ostringstream os;
    os << "# EMS 配置说明\n\n"
       << "> 本文档由 `ems-config --dump-doc` 从字段绑定表自动生成，**请勿手工编辑**。\n"
       << "> 修改参数请改 `P1/src/ems_config.h` 的 `bind_fields()`，然后重新生成。\n\n";

    os << "## 顶层结构\n\n"
       << "| 节 | 含义 |\n|---|---|\n"
       << "| `name` / `description` / `site` / `version` | 元信息，不参与控制 |\n"
       << "| `loop` | 实时循环（周期、使能开关、日志） |\n"
       << "| `plant` | 被控对象参数（仿真装配用；现场由真实设备提供） |\n"
       << "| `limits` | 设备限制（PCS 额定 / BMS 允许 / 变压器 / 需量） |\n"
       << "| `safety` | 安全约束（SOC / 温度 / 关口 / 变压器 / 变化率） |\n"
       << "| `coordinator` | 优化调度与实时协同（滚动优化、纠偏增益） |\n"
       << "| `state_machine` | 状态机（自检拍数、故障恢复拍数、READY 是否允许输出） |\n"
       << "| `strategies` | 策略开关与参数（id → enabled / note / params） |\n\n";

    os << "---\n\n## 参数明细\n\n";
    doc_detail::md_section(os, "实时循环", config_keys::kLoop,   base.loop);
    doc_detail::md_section(os, "被控对象", config_keys::kPlant,  base.plant);
    doc_detail::md_section(os, "设备限制", config_keys::kLimits, base.limits);
    doc_detail::md_section(os, "安全约束", config_keys::kSafety, base.safety);
    doc_detail::md_section(os, "优化协同", config_keys::kCoord,  base.coord);
    doc_detail::md_section(os, "状态机",   config_keys::kFsm,    base.fsm);

    os << "### `strategies` —— 策略开关与参数\n\n"
       << "策略参数是一张 `参数名 → 数值` 的表，**参数名由各策略自己解释**。\n"
       << "P1 只校验策略 `id` 是否存在（打错会报错），参数名交给策略校验。\n\n";
    if (rt != nullptr) {
        os << "当前已注册策略：\n\n| id |\n|---|\n";
        std::vector<std::string> ids = rt->manager().list_ids();
        std::sort(ids.begin(), ids.end());
        for (const auto& id : ids) os << "| `" << id << "` |\n";
        os << "\n";
    }
    os << "示例：\n\n```json\n\"strategies\": {\n"
       << "  \"anti_reverse\": { \"enabled\": true, \"params\": { \"margin_kw\": 5.0 } }\n"
       << "}\n```\n";
    return os.str();
}

// =====================================================================
// 机器可读字段清单（供外部工具/CI 消费：键名、单位、默认值、是否关键）
// =====================================================================
namespace doc_detail {

template <class T>
inline json::Value schema_section(T& obj) {
    Binder b;
    bind_fields(b, obj);
    json::Value arr = json::Value::make_array();
    for (const auto& f : b.fields()) {
        json::Value e = json::Value::make_object();
        e.set("key",      json::Value(f.key));
        e.set("unit",     json::Value(f.unit));
        e.set("desc",     json::Value(f.desc));
        e.set("required", json::Value(f.required));
        const json::Value v = f.get();
        e.set("default", v);
        arr.push(std::move(e));
    }
    return arr;
}

} // namespace doc_detail

inline json::Value render_config_schema(EmsRuntime* rt) {
    EmsConfig base;
    json::Value root = json::Value::make_object();
    root.set("version", json::Value(1));
    root.set(config_keys::kLoop,   doc_detail::schema_section(base.loop));
    root.set(config_keys::kPlant,  doc_detail::schema_section(base.plant));
    root.set(config_keys::kLimits, doc_detail::schema_section(base.limits));
    root.set(config_keys::kSafety, doc_detail::schema_section(base.safety));
    root.set(config_keys::kCoord,  doc_detail::schema_section(base.coord));
    root.set(config_keys::kFsm,    doc_detail::schema_section(base.fsm));

    json::Value ids = json::Value::make_array();
    if (rt != nullptr) {
        std::vector<std::string> v = rt->manager().list_ids();
        std::sort(v.begin(), v.end());
        for (const auto& id : v) ids.push(json::Value(id));
    }
    root.set("strategy_ids", std::move(ids));
    return root;
}

} // namespace ems
