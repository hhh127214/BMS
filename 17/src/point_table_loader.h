// =====================================================================
// 17/ — 点表配置化加载器（C5）
//
// 缺口原文（docs/规划/模拟器与设备接入梳理.md §10.3 C5）：
//
//   > P1 号称"现场改配置不改源码"，但**只有算法参数进去了，点表没进去** ——
//   > 而现场换站时，点表恰恰是**最容易变**的那一样。
//
// 本文件的职责：把"全点表"从**编译期常量**变成**运行时可加载的数据**。
//
// ---------------------------------------------------------------------
// 为什么选 JSON，而不是行式 CSV / INI：
//
//   ① **有嵌套**。一个点是一组结构化的八元组，而"量程"本身是一对 (min,max)。
//      CSV/INI 要么把量程拆成两列（range_min/range_max，可以接受），要么
//      把整点塞进一行用分隔符硬拼 —— 后者在点表里全是 "MEAS.BMS.CELL_V_MAX"
//      这种含点的名字时，分隔符选什么都别扭。
//   ② **类型可信**。JSON 里 `"default": 0` 与 `"default": "0"` 是两种东西，
//      加载器能把"字段类型写错"和"字段缺失"分开报错。INI 里全是字符串，
//      "把单位写成数字"这类错误要到业务层才炸。
//   ③ 项目**已有先例**（P1/src/json_lite.h），团队对 JSON 配置的心智模型已建立；
//      再引入一种新格式，现场运维要记两套。
//   ④ 17/ 不引第三方库 —— 自带 17/src/json_min.h（约 500 行，够用例）。
//
//   代价：JSON 比 CSV 啰嗦（点表 107 点 ≈ 1200 行），且手写容易漏逗号。
//   因此 json_min.h 有意放宽了**注释 + 尾逗号 + 裸键**（见其文件头）——
//   现场想临时注释掉一个点位定义，不该导致整张表加载失败。
//
// ---------------------------------------------------------------------
// 判据（对应 C5 的四条）：
//   ① 内置默认点表 → to_json() → load_json() → **逐点逐字段完全一致**（往返幂等）
//   ② 改 JSON 里的量程/单位 → 加载后生效
//   ③ 非法 JSON / 缺字段 / 点名重复 / 量程矛盾
//      → 显式报错，并指出**第几个点、哪个字段**（绝不静默用默认值）
//   ④ 支持**追加点**而不破坏既有索引
//
// 编译：纯头文件（inline）。
// =====================================================================

#pragma once

#include "device_point_table.h"
#include "json_min.h"

#include <cstdio>
#include <string>
#include <vector>

namespace ems {
namespace devpt {

// =====================================================================
// 运行时的点（与 DevicePointDef 同字段，但用 std::string —— 可加载）
// =====================================================================
struct DevicePoint {
    std::string name;
    std::string unit;
    int         type      = PT_ANALOG;
    double      range_min = 0.0;
    double      range_max = 0.0;
    double      def       = 0.0;
    int         device    = DK_BMS;
    int         zone      = PZ_MEAS;
};

// =====================================================================
// 加载/校验结果
//
// ★ 为什么要带 point_index + field：C5 判据③ 明确要求"指出第几个点、哪个字段"。
//   只回一个 bool 的加载器等于**静默**：现场拿到的是"点表没生效"，
//   而排查的第一现场是"哪一行"，不是"失败了没有"。
// =====================================================================
struct LoadError {
    bool        ok          = true;
    int         point_index = -1;   // 在**结果表**里的下标；-1 = 全局/整篇
    std::string point_name;         // 出错点名（能取到时）
    std::string field;              // 出错字段
    std::string message;

    std::string to_string() const {
        if (ok) return "ok";
        char buf[64];
        if (point_index >= 0) {
            std::snprintf(buf, sizeof(buf), "点[%d]", point_index);
        } else {
            std::snprintf(buf, sizeof(buf), "点[全局]");
        }
        std::string s = buf;
        if (!point_name.empty()) s += " '" + point_name + "'";
        if (!field.empty())      s += " 字段 '" + field + "'";
        s += " —— " + message;
        return s;
    }
};

// =====================================================================
// 可加载的点表
// =====================================================================
class DevicePointTable {
public:
    std::vector<DevicePoint> points;

    // ---- 内置默认点表（点表真相源的运行时镜像）----
    static DevicePointTable builtin() {
        DevicePointTable t;
        t.points.reserve(static_cast<std::size_t>(DP_POINT_COUNT));
        for (int i = 0; i < DP_POINT_COUNT; ++i) {
            const DevicePointDef& d = kBuiltinPoints[i];
            DevicePoint p;
            p.name      = d.name;
            p.unit      = d.unit;
            p.type      = d.type;
            p.range_min = d.range_min;
            p.range_max = d.range_max;
            p.def       = d.def;
            p.device    = d.device;
            p.zone      = d.zone;
            t.points.push_back(std::move(p));
        }
        return t;
    }

    std::size_t size() const { return points.size(); }
    bool empty() const { return points.empty(); }

    const DevicePoint* find(const std::string& name) const {
        for (const auto& p : points) if (p.name == name) return &p;
        return nullptr;
    }
    const DevicePoint* find(const char* name) const {
        if (name == nullptr) return nullptr;
        return find(std::string(name));
    }
    // 按名查索引。找不到返回 -1 —— **不退化成 0**。
    int index_of(const std::string& name) const {
        for (std::size_t i = 0; i < points.size(); ++i) {
            if (points[i].name == name) return static_cast<int>(i);
        }
        return -1;
    }
    int index_of(const char* name) const {
        if (name == nullptr) return -1;
        return index_of(std::string(name));
    }

    // =================================================================
    // 校验（不含"两层映射"那部分；映射的校验见 point_mapping.h）
    //
    // 覆盖 C4 的 ①④⑤：
    //   ① 点名唯一且非空
    //   ④ 量程合理（min<max，默认值在量程内）
    //   ⑤ 索引与数组下标一致（下标 i 的点，按名反查必须回到 i）
    //
    // first：从第几个点开始校验（追加场景下，旧点已验过可跳过）
    // =================================================================
    LoadError validate(std::size_t first = 0) const {
        LoadError e;
        // ① 点名非空
        for (std::size_t i = first; i < points.size(); ++i) {
            if (points[i].name.empty()) {
                e.ok = false; e.point_index = static_cast<int>(i); e.field = "name";
                e.message = "点名为空（空点名在 RT_DB 里等价于 UNUSED，永远读不到）";
                return e;
            }
        }
        // ① 点名唯一 —— O(n²)，107 点无妨；换站到几千点时该换 hash，见 README 已知边界
        for (std::size_t i = first; i < points.size(); ++i) {
            for (std::size_t j = i + 1; j < points.size(); ++j) {
                if (points[i].name == points[j].name) {
                    e.ok = false; e.point_index = static_cast<int>(j);
                    e.point_name = points[j].name; e.field = "name";
                    e.message = "点名重复（与点[" + std::to_string(i) + "] '" +
                                points[i].name + "' 同名）—— 按名查索引会永远命中第一个，"
                                "第二个点静默收不到值";
                    return e;
                }
            }
        }
        // ④ 量程 + 默认值 + 枚举合法性
        for (std::size_t i = first; i < points.size(); ++i) {
            const DevicePoint& p = points[i];
            if (!(p.range_min < p.range_max)) {
                e.ok = false; e.point_index = static_cast<int>(i); e.point_name = p.name;
                e.field = "range";
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "量程矛盾：要求 min < max，实为 [%g, %g]", p.range_min, p.range_max);
                e.message = buf;
                return e;
            }
            if (p.def < p.range_min || p.def > p.range_max) {
                e.ok = false; e.point_index = static_cast<int>(i); e.point_name = p.name;
                e.field = "default";
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                              "默认值越量程：default=%g 不在 [%g, %g] 内",
                              p.def, p.range_min, p.range_max);
                e.message = buf;
                return e;
            }
            if (p.type != PT_ANALOG && p.type != PT_DIGITAL) {
                e.ok = false; e.point_index = static_cast<int>(i); e.point_name = p.name;
                e.field = "type"; e.message = "类型非法";
                return e;
            }
            if (p.device < DK_BMS || p.device > DK_PCS) {
                e.ok = false; e.point_index = static_cast<int>(i); e.point_name = p.name;
                e.field = "device"; e.message = "所属设备非法";
                return e;
            }
            if (p.zone < PZ_MEAS || p.zone > PZ_CMD) {
                e.ok = false; e.point_index = static_cast<int>(i); e.point_name = p.name;
                e.field = "zone"; e.message = "点区非法";
                return e;
            }
        }
        // ⑤ 索引与下标一致
        for (std::size_t i = first; i < points.size(); ++i) {
            const int back = index_of(points[i].name);
            if (back != static_cast<int>(i)) {
                e.ok = false; e.point_index = static_cast<int>(i); e.point_name = points[i].name;
                e.field = "index";
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "索引与数组下标不一致：下标 %zu 反查得到 %d"
                              "（同名点在前，或表被外部改坏）", i, back);
                e.message = buf;
                return e;
            }
        }
        return e;
    }

    // =================================================================
    // 导出成 JSON（判据①的"导出"侧）
    // =================================================================
    std::string to_json(int indent = 2) const {
        jmin::Value root = jmin::Value::make_object();
        root.set("version", jmin::Value(1.0));
        root.set("count",   jmin::Value(static_cast<double>(points.size())));
        jmin::Value arr = jmin::Value::make_array();
        for (const auto& p : points) {
            jmin::Value o = jmin::Value::make_object();
            o.set("name", jmin::Value(p.name));
            o.set("unit", jmin::Value(p.unit));
            o.set("type", jmin::Value(std::string(point_type_name(p.type))));
            jmin::Value r = jmin::Value::make_array();
            r.push(jmin::Value(p.range_min));
            r.push(jmin::Value(p.range_max));
            o.set("range", std::move(r));
            o.set("default", jmin::Value(p.def));
            o.set("device",  jmin::Value(std::string(device_kind_name(p.device))));
            o.set("zone",    jmin::Value(std::string(point_zone_name(p.zone))));
            arr.push(std::move(o));
        }
        root.set("points", std::move(arr));
        return jmin::dump(root, indent);
    }

    // =================================================================
    // 从 JSON 加载
    //
    //   append = false → **全量替换**（换站：整张点表重写）
    //   append = true  → **追加到末尾**（增容：只加新点，旧点索引不动）
    //
    // ★ 原子性：任何错误都**不改动**本对象。半加载的表比加载失败更危险 ——
    //   它看起来"能跑"，但索引与现场不一致。
    // =================================================================
    LoadError load_json(const std::string& text, bool append = false) {
        LoadError e;
        jmin::ParseError perr;
        jmin::Value root = jmin::parse(text, &perr);
        if (!perr.ok) {
            e.ok = false;
            e.point_index = -1;
            e.field = "<json>";
            e.message = perr.to_string();
            return e;
        }
        if (!root.is_object()) {
            e.ok = false; e.field = "<root>";
            e.message = std::string("顶层必须是对象，实为 ") + root.type_name();
            return e;
        }
        const jmin::Value* pts = root.find("points");
        if (pts == nullptr) {
            e.ok = false; e.field = "points";
            e.message = "缺少 'points' 数组（点表定义必须放在 points 下）";
            return e;
        }
        if (!pts->is_array()) {
            e.ok = false; e.field = "points";
            e.message = std::string("'points' 必须是数组，实为 ") + pts->type_name();
            return e;
        }

        // 结果表：append 时从现有表起，否则空表起
        std::vector<DevicePoint> out = append ? points : std::vector<DevicePoint>();
        const std::size_t base = out.size();

        for (std::size_t k = 0; k < pts->arr.size(); ++k) {
            const jmin::Value& jp = pts->arr[k];
            // 出错时上报的是**结果表里的全局下标**，不是数组内下标 ——
            // 追加场景下"第几个点"必须按整张表数，否则现场对不上。
            const int gi = static_cast<int>(base + k);
            if (!jp.is_object()) {
                e.ok = false; e.point_index = gi; e.field = "<point>";
                e.message = std::string("第 ") + std::to_string(k) +
                            " 个点表项必须是对象，实为 " + jp.type_name();
                return e;
            }

            DevicePoint p;
            // ---- name ----
            if (!read_str(jp, "name", gi, "", "name", p.name, &e)) return e;
            if (p.name.empty()) {
                e.ok = false; e.point_index = gi; e.field = "name";
                e.message = "点名为空字符串";
                return e;
            }
            // ---- unit ----
            if (!read_str(jp, "unit", gi, p.name, "unit", p.unit, &e)) return e;
            // ---- type ----
            std::string s;
            if (!read_str(jp, "type", gi, p.name, "type", s, &e)) return e;
            p.type = point_type_from_name(s);
            if (p.type < 0) {
                e.ok = false; e.point_index = gi; e.point_name = p.name; e.field = "type";
                e.message = "类型必须是 analog / digital，实为 '" + s + "'";
                return e;
            }
            // ---- range ----
            const jmin::Value* r = jp.find("range");
            if (r == nullptr) {
                e.ok = false; e.point_index = gi; e.point_name = p.name; e.field = "range";
                e.message = "缺字段 'range'（必须是 [min, max] 两元素数组）";
                return e;
            }
            if (!r->is_array() || r->arr.size() != 2 ||
                !r->arr[0].is_number() || !r->arr[1].is_number()) {
                e.ok = false; e.point_index = gi; e.point_name = p.name; e.field = "range";
                e.message = "range 必须是两个数字组成的数组";
                return e;
            }
            p.range_min = r->arr[0].num;
            p.range_max = r->arr[1].num;
            // ---- default ----
            const jmin::Value* d = jp.find("default");
            if (d == nullptr) {
                e.ok = false; e.point_index = gi; e.point_name = p.name; e.field = "default";
                e.message = "缺字段 'default'";
                return e;
            }
            if (!d->is_number()) {
                e.ok = false; e.point_index = gi; e.point_name = p.name; e.field = "default";
                e.message = std::string("default 必须是数字，实为 ") + d->type_name();
                return e;
            }
            p.def = d->num;
            // ---- device ----
            if (!read_str(jp, "device", gi, p.name, "device", s, &e)) return e;
            p.device = device_kind_from_name(s);
            if (p.device < 0) {
                e.ok = false; e.point_index = gi; e.point_name = p.name; e.field = "device";
                e.message = "device 必须是 bms / meter / pcs，实为 '" + s + "'";
                return e;
            }
            // ---- zone ----
            if (!read_str(jp, "zone", gi, p.name, "zone", s, &e)) return e;
            p.zone = point_zone_from_name(s);
            if (p.zone < 0) {
                e.ok = false; e.point_index = gi; e.point_name = p.name; e.field = "zone";
                e.message = "zone 必须是 meas / sta / cfg / alm / cmd，实为 '" + s + "'";
                return e;
            }

            out.push_back(std::move(p));
        }

        // 全表校验（含与既有部分的重复名检查）
        DevicePointTable tmp;
        tmp.points = out;
        LoadError ve = tmp.validate(append ? 0 : 0);
        if (!ve.ok) {
            ve.point_index = ve.point_index;   // 已是全局下标
            return ve;
        }

        points = std::move(out);      // 只有全部通过才提交
        return e;                     // ok == true
    }

private:
    // 读一个必填字符串字段；缺失/类型不对都报错（**不静默用默认值**）
    static bool read_str(const jmin::Value& obj, const char* key, int gi,
                         const std::string& pname, const char* field_name,
                         std::string& out, LoadError* e) {
        const jmin::Value* v = obj.find(key);
        if (v == nullptr) {
            e->ok = false; e->point_index = gi; e->point_name = pname; e->field = field_name;
            e->message = std::string("缺字段 '") + key + "'";
            return false;
        }
        if (!v->is_string()) {
            e->ok = false; e->point_index = gi; e->point_name = pname; e->field = field_name;
            e->message = std::string("'") + key + "' 必须是字符串，实为 " + v->type_name();
            return false;
        }
        out = v->str;
        return true;
    }
};

} // namespace devpt
} // namespace ems
