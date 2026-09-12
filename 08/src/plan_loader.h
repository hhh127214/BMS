// =====================================================================
// 08/ — 01/ 优化层计划加载器（周期 8 的"优化层产出"入口）
//
// 01/ 的 battery_server.exe 以 HTTP 返回 JSON 计划（见 01/docs/api.md §4）：
//   { "status":"ok", "strategy":"arbitrage", "exact":1, "horizon":24,
//     "time_step_hours":1.0, "final_soc":0.5, "total_profit_yuan":74.7,
//     "plan":[ {"t":0,"charge_kw":0.0,"discharge_kw":0.0,"soc":0.5}, ... ] }
//
// 本文件做两件事：
//   1. 内置一个极简 JSON 解析器（不引第三方依赖，与项目"零外部依赖"约定一致）
//   2. 把响应体归一化成 DayPlan（统一到 P_bat 符号约定：放电为正）
//
// 另外提供 build_plan_greedy()：当 01/ 服务未启动时的**降级兜底**计划生成器
// （贪心启发式，非最优），保证 05/ 的演示与测试可独立运行。
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ems {
namespace mini_json {

// =====================================================================
// 极简 JSON 值
// =====================================================================
struct Value {
    enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
    Type        type = Type::kNull;
    bool        b    = false;
    double      num  = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    bool is_null()   const { return type == Type::kNull; }
    bool is_bool()   const { return type == Type::kBool; }
    bool is_number() const { return type == Type::kNumber; }
    bool is_string() const { return type == Type::kString; }
    bool is_array()  const { return type == Type::kArray; }
    bool is_object() const { return type == Type::kObject; }

    double      as_number(double d = 0.0) const { return is_number() ? num : d; }
    bool        as_bool(bool d = false)   const { return is_bool() ? b : d; }
    std::string as_string(const std::string& d = "") const {
        return is_string() ? str : d;
    }
    size_t size() const {
        if (is_array())  return arr.size();
        if (is_object()) return obj.size();
        return 0;
    }
    const Value* find(const std::string& key) const {
        if (!is_object()) return nullptr;
        for (const auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    const Value& at(size_t i) const { return arr[i]; }
};

// =====================================================================
// 递归下降解析器
// =====================================================================
class Parser {
public:
    static bool parse(const std::string& text, Value& out, std::string* err) {
        Parser p(text);
        p.skip_ws();
        if (!p.parse_value(out)) {
            if (err) *err = p.err_.empty() ? "parse_error" : p.err_;
            return false;
        }
        p.skip_ws();
        if (p.i_ != p.s_.size()) {
            if (err) *err = "trailing_characters";
            return false;
        }
        return true;
    }

private:
    explicit Parser(const std::string& s) : s_(s) {}

    void skip_ws() {
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i_; continue; }
            break;
        }
    }
    bool fail(const std::string& m) { if (err_.empty()) err_ = m; return false; }

    bool parse_value(Value& v) {
        skip_ws();
        if (i_ >= s_.size()) return fail("unexpected_eof");
        char c = s_[i_];
        switch (c) {
            case '{': return parse_object(v);
            case '[': return parse_array(v);
            case '"': {
                v.type = Value::Type::kString;
                return parse_string(v.str);
            }
            case 't': case 'f': {
                if (literal("true"))  { v.type = Value::Type::kBool; v.b = true;  return true; }
                if (literal("false")) { v.type = Value::Type::kBool; v.b = false; return true; }
                return fail("bad_literal");
            }
            case 'n': {
                if (literal("null")) { v.type = Value::Type::kNull; return true; }
                return fail("bad_literal");
            }
            default: return parse_number(v);
        }
    }

    bool parse_object(Value& v) {
        v.type = Value::Type::kObject;
        ++i_;   // '{'
        skip_ws();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
        while (true) {
            skip_ws();
            std::string key;
            if (i_ >= s_.size() || s_[i_] != '"') return fail("expected_key");
            if (!parse_string(key)) return false;
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != ':') return fail("expected_colon");
            ++i_;
            Value child;
            if (!parse_value(child)) return false;
            v.obj.emplace_back(std::move(key), std::move(child));
            skip_ws();
            if (i_ >= s_.size()) return fail("unterminated_object");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; return true; }
            return fail("expected_comma_or_brace");
        }
    }

    bool parse_array(Value& v) {
        v.type = Value::Type::kArray;
        ++i_;   // '['
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
        while (true) {
            Value child;
            if (!parse_value(child)) return false;
            v.arr.push_back(std::move(child));
            skip_ws();
            if (i_ >= s_.size()) return fail("unterminated_array");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') { ++i_; return true; }
            return fail("expected_comma_or_bracket");
        }
    }

    bool parse_string(std::string& out) {
        if (s_[i_] != '"') return fail("expected_quote");
        ++i_;
        out.clear();
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (i_ >= s_.size()) return fail("bad_escape");
                char e = s_[i_++];
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        // 只保留 ASCII 部分，足够本用途（中文不出现于键名）
                        if (i_ + 4 > s_.size()) return fail("bad_unicode");
                        i_ += 4;
                        out += '?';
                        break;
                    }
                    default: return fail("bad_escape");
                }
            } else {
                out += c;
            }
        }
        return fail("unterminated_string");
    }

    bool parse_number(Value& v) {
        size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        bool any = false;
        while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') { ++i_; any = true; }
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') { ++i_; any = true; }
        }
        if (any && i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        }
        if (!any) return fail("bad_number");
        v.type = Value::Type::kNumber;
        v.num  = std::atof(s_.substr(start, i_ - start).c_str());
        return true;
    }

    bool literal(const char* lit) {
        size_t n = std::strlen(lit);
        if (s_.compare(i_, n, lit) != 0) return false;
        i_ += n;
        return true;
    }

    const std::string& s_;
    size_t i_ = 0;
    std::string err_;
};

} // namespace mini_json

// =====================================================================
// 计划数据结构（统一到 P_bat 符号：放电为正）
// =====================================================================
struct PlanPoint {
    double t_s       = 0.0;   // 时段起始时刻（相对计划起点，秒）
    double p_plan_kw = 0.0;   // 计划 P_bat（放电为正，充电为负）
    double soc_ref   = 0.5;   // 该时段结束时的 SOC 参考
    double grid_kw   = 0.0;   // 计划网购功率（forecast / demand_response 才有）
    bool   has_grid  = false;
};

struct DayPlan {
    std::string strategy;              // arbitrage / forecast / demand_response
    double      step_s = 900.0;        // 时段长度（秒）
    double      time_step_hours = 0.25;
    int         horizon = 0;           // 时段数
    double      final_soc = 0.5;
    double      total_profit_yuan = 0.0;
    double      total_cost_yuan   = 0.0;
    std::vector<PlanPoint> points;
    bool        loaded = false;
    std::string source;                // 来源说明（文件路径 / "greedy_fallback"）

    double duration_s() const {
        return points.empty() ? 0.0 : step_s * static_cast<double>(points.size());
    }

    // 阶梯保持采样（按**时间**查找，支持任意起点/部分时段的计划）
    // 若 t_s 落在计划区间之外，按计划跨度循环折回（24h 计划可跨天复用）
    bool sample(double t_s, double* p_plan_kw, double* soc_ref) const {
        if (points.empty()) return false;
        const double begin = points.front().t_s;
        const double end   = points.back().t_s + step_s;
        const double span  = end - begin;
        double tt = t_s;
        if (span > 0.0) {
            if (tt < begin || tt >= end) {
                tt = begin + std::fmod(tt - begin, span);
                if (tt < begin) tt += span;
            }
        }
        size_t idx = 0;
        for (size_t i = 0; i < points.size(); ++i) {
            if (points[i].t_s <= tt + 1e-9) idx = i;
            else break;
        }
        if (p_plan_kw) *p_plan_kw = points[idx].p_plan_kw;
        if (soc_ref)   *soc_ref   = points[idx].soc_ref;
        return true;
    }

    // 按绝对时刻定位时段序号（用于对齐预测序列）
    size_t slot_of(double t_s) const {
        if (step_s <= 0.0) return 0;
        double tt = t_s - points.front().t_s;
        if (tt < 0.0) tt = 0.0;
        return static_cast<size_t>(tt / step_s);
    }

    double planned_charge_kwh() const {
        double e = 0.0;
        for (const auto& p : points)
            if (p.p_plan_kw < 0) e += -p.p_plan_kw * step_s / 3600.0;
        return e;
    }
    double planned_discharge_kwh() const {
        double e = 0.0;
        for (const auto& p : points)
            if (p.p_plan_kw > 0) e += p.p_plan_kw * step_s / 3600.0;
        return e;
    }
    double soc_swing() const {
        if (points.empty()) return 0.0;
        double lo = 1e18, hi = -1e18;
        for (const auto& p : points) { lo = std::min(lo, p.soc_ref); hi = std::max(hi, p.soc_ref); }
        return hi - lo;
    }
};

// =====================================================================
// 解析 01/ 的响应 JSON → DayPlan
// =====================================================================
inline bool parse_plan_json(const std::string& text, DayPlan& plan, std::string* err) {
    mini_json::Value root;
    if (!mini_json::Parser::parse(text, root, err)) return false;
    if (!root.is_object()) { if (err) *err = "root_not_object"; return false; }

    const auto* st = root.find("status");
    if (st && st->is_string() && st->as_string() != "ok") {
        const auto* msg = root.find("message");
        if (err) *err = "server_error:" + (msg ? msg->as_string("unknown") : "unknown");
        return false;
    }
    const auto* plan_arr = root.find("plan");
    if (!plan_arr || !plan_arr->is_array() || plan_arr->size() == 0) {
        if (err) *err = "missing_plan_array";
        return false;
    }

    DayPlan p;
    if (const auto* v = root.find("strategy"))          p.strategy = v->as_string();
    if (const auto* v = root.find("horizon"))           p.horizon  = static_cast<int>(v->as_number());
    if (const auto* v = root.find("time_step_hours"))   p.time_step_hours = v->as_number(0.25);
    if (const auto* v = root.find("final_soc"))         p.final_soc = v->as_number(0.5);
    if (const auto* v = root.find("total_profit_yuan")) p.total_profit_yuan = v->as_number();
    if (const auto* v = root.find("total_cost_yuan"))   p.total_cost_yuan   = v->as_number();
    p.step_s = p.time_step_hours * 3600.0;
    if (p.step_s <= 0.0) p.step_s = 900.0;

    for (size_t i = 0; i < plan_arr->size(); ++i) {
        const auto& e = plan_arr->at(i);
        if (!e.is_object()) continue;
        PlanPoint pp;
        double t_idx = static_cast<double>(i);
        if (const auto* v = e.find("t"))            t_idx = v->as_number(t_idx);
        double chg = 0.0, dis = 0.0;
        if (const auto* v = e.find("charge_kw"))    chg = v->as_number();
        if (const auto* v = e.find("discharge_kw")) dis = v->as_number();
        // 01/ 的 plan 里 charge_kw / discharge_kw 都是非负幅度
        pp.p_plan_kw = dis - chg;
        pp.t_s = t_idx * p.step_s;
        if (const auto* v = e.find("soc"))          pp.soc_ref = v->as_number(0.5);
        if (const auto* v = e.find("grid_kw")) { pp.grid_kw = v->as_number(); pp.has_grid = true; }
        p.points.push_back(pp);
    }
    if (p.horizon <= 0) p.horizon = static_cast<int>(p.points.size());
    p.loaded = true;
    p.source = "01_plan_json";
    plan = p;
    return true;
}

inline bool load_plan_file(const std::string& path, DayPlan& plan, std::string* err) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f.is_open()) { if (err) *err = "cannot_open:" + path; return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_plan_json(ss.str(), plan, err);
}

// =====================================================================
// 降级兜底：贪心启发式计划生成器
//   —— 当 01/ 的 MILP 服务不可用时使用，保证系统仍能运行（功能降级，不是停机）
//   逻辑：按电价排序，最低价时段充电、最高价时段放电，受 SOC 与功率上限约束。
//
//   start_slot：滚动重优化的起始时段序号。
//   采用**滚动时域**：永远规划完整 24h（N 个时段），预测序列按日周期延拓，
//   points[k].t_s = (start_slot + k) * step_hours * 3600（绝对时刻）。
//   这样每次重优化都产出覆盖未来 24h 的完整计划，不会因接近日末而退化。
// =====================================================================
inline DayPlan build_plan_greedy(const std::vector<double>& price,
                                 const std::vector<double>& load_kw,
                                 const std::vector<double>& pv_kw,
                                 double soc_init,
                                 double soc_min,
                                 double soc_max,
                                 double capacity_kwh,
                                 double p_max_chg_kw,
                                 double p_max_dis_kw,
                                 double step_hours,
                                 size_t start_slot = 0) {
    DayPlan plan;
    const size_t n = price.size();
    plan.strategy = "greedy_fallback";
    plan.step_s = step_hours * 3600.0;
    plan.time_step_hours = step_hours;
    plan.horizon = static_cast<int>(n);
    plan.source = "greedy_fallback";

    if (n == 0) return plan;
    start_slot %= n;

    const size_t m = n;                  // 滚动时域长度 = 一个完整日周期
    plan.points.resize(m);

    // 绝对时段序号（按日周期延拓）
    std::vector<size_t> abs_slot(m);
    for (size_t k = 0; k < m; ++k) abs_slot[k] = start_slot + k;

    // 在延拓序列上按电价排序，取最便宜 1/4 充电、最贵 1/4 放电
    std::vector<size_t> order(m);
    for (size_t k = 0; k < m; ++k) order[k] = k;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return price[abs_slot[a] % n] < price[abs_slot[b] % n];
    });

    const size_t charge_slots    = std::max<size_t>(1, m / 4);
    const size_t discharge_slots = std::max<size_t>(1, m / 4);

    std::vector<int> action(m, 0);   // -1 充电 / +1 放电 / 0 待机

    // 辅助：某时段的净负荷（>0 表示从电网取电）
    auto net_of = [&](size_t k) {
        const size_t slot = abs_slot[k] % n;
        return load_kw[slot] - (slot < pv_kw.size() ? pv_kw[slot] : 0.0);
    };

    // ---- ① 光伏余电时段（净负荷 < 0）必须充电吸收，否则必然倒送 ----
    //     这与电价无关：不许倒送时，"把余电吃进去"是硬需求。
    for (size_t k = 0; k < m; ++k) {
        if (net_of(k) < 0.0) action[k] = -1;
    }
    // ---- ② 最贵 1/4 放电、最便宜 1/4 充电（不覆盖已定的余电吸收时段）----
    for (size_t k = 0; k < discharge_slots && k < m; ++k) {
        const size_t idx = order[m - 1 - k];
        if (action[idx] == 0) action[idx] = +1;
    }
    for (size_t k = 0; k < charge_slots && k < m; ++k) {
        const size_t idx = order[k];
        if (action[idx] == 0) action[idx] = -1;
    }

    // ---- ③ 光伏余电裕度预留 ----
    // 不许倒送时，白天光伏大发的余电**必须**由储能吸收；而余电是"免费电量"，
    // 比谷段 0.30 元/kWh 买电更便宜，所以应当**优先吸收余电、少买谷电**。
    // v1.x 的实现把每个谷段都充到 soc_max，凌晨 3 点就满充 → 白天余电无处可去，
    // 只能倒送（安全层仅能检出区间矛盾，无法物理避免）。
    // 做法：从后往前累加"该时段之后还有多少余电必须吸收"，据此预留 SOC 裕度。
    const double eta = 0.95;
    const double usable_kwh = std::max(1e-9, capacity_kwh * eta);
    std::vector<double> surplus_ahead(m + 1, 0.0);
    for (size_t k = m; k-- > 0; ) {
        const double net = net_of(k);
        const double e = (net < 0.0) ? std::min(p_max_chg_kw, -net) * step_hours : 0.0;
        surplus_ahead[k] = surplus_ahead[k + 1] + e;
    }
    // 时段 k 的充电 SOC 上限：为**其后**的余电留出裕度
    auto soc_cap_at = [&](size_t k) {
        const double reserve = std::min(soc_max - soc_min, surplus_ahead[k + 1] / usable_kwh);
        return std::max(soc_min, soc_max - reserve);
    };

    double soc = soc_init;
    for (size_t k = 0; k < m; ++k) {
        const double net = net_of(k);
        double p = 0.0;
        if (action[k] == -1) {
            // 充到"本时段上限"为止（该上限已为后续余电预留裕度）
            const double cap = soc_cap_at(k);
            const double room_kwh = std::max(0.0, (cap - soc) * capacity_kwh) / eta;
            const double p_want = (net < 0.0) ? std::min(p_max_chg_kw, -net)
                                              : p_max_chg_kw;
            p = -std::min(p_want, room_kwh / step_hours);
        } else if (action[k] == +1) {
            double avail_kwh = std::max(0.0, (soc - soc_min) * capacity_kwh) * eta;
            double p_by_soc = avail_kwh / step_hours;
            p = std::min(p_max_dis_kw, p_by_soc);
            // 不允许倒送：放电不超过当前净负荷
            p = std::min(p, std::max(0.0, net));
        }
        plan.points[k].t_s = static_cast<double>(abs_slot[k]) * plan.step_s;
        plan.points[k].p_plan_kw = p;
        if (p > 0.0)      soc -= p * step_hours / capacity_kwh / eta;
        else if (p < 0.0) soc += (-p) * step_hours * eta / capacity_kwh;
        soc = std::max(soc_min, std::min(soc_max, soc));
        plan.points[k].soc_ref = soc;
    }
    plan.final_soc = soc;
    plan.loaded = true;
    return plan;
}

// =====================================================================
// 导出计划为 CSV（便于画图 / 与 01/ 结果比对）
// =====================================================================
inline bool save_plan_csv(const DayPlan& plan, const std::string& path) {
    std::ofstream f(path.c_str());
    if (!f.is_open()) return false;
    f << "t_s,P_plan_kW,SOC_ref,grid_kW\n";
    for (const auto& p : plan.points) {
        f << p.t_s << "," << p.p_plan_kw << "," << p.soc_ref;
        if (p.has_grid) f << "," << p.grid_kw;
        f << "\n";
    }
    return true;
}

} // namespace ems
