// =====================================================================
// P2/ — 产品化 P2 可观测性 · 统一事件记录（SOE: Sequence of Events）
//
// ---------------------------------------------------------------------
// 为什么需要这个文件（P2 之前的状态）
// ---------------------------------------------------------------------
// 事件记录在 P2 之前散在三个模块，三种表示：
//
//   06/state_machine.h   StateEvent{ts, from, to, reason}   ← 只有状态迁移
//   10/src/sim_24h.h     AlarmEntry{t, level, source, msg}  ← 在**仿真装配层**组装
//   07/realtime_loop.h   StepRecord 向量                     ← 时序，不是事件
//
// 三个问题，都是生产环境会出事的：
//
// ① **告警组装寄生在仿真装配层**。`collect_alarms()` 的入参是 `Sim24hConfig`
//    （仿真专属结构体），现场没有它 —— 也就是说**现场部署后系统没有告警能力**。
//    可观测性必须是运行时的一等公民，不能挂在仿真装配层上。
//
// ② **事件风暴没有框架级抑制**。周期 10 实测：收紧配置下 `safety_clip` 连续
//    838 拍。逐拍记事件就是 838 条。10/ 里手工实现了"记首次+恢复"，
//    但**每个指标都要手写一遍**，漏一个就退化回风暴。
//
// ③ **level / source 是字符串**。无法排序、无法按等级过滤、无法统计，
//    而且 10/ 的 message 用 `to_string((long long)v)` **丢掉了小数**。
//
// ---------------------------------------------------------------------
// 本文件的设计
// ---------------------------------------------------------------------
//   · 等级 / 来源 / 事件码都是 **enum**（可比较、可统计、编译期检查）
//   · **时间窗抑制**是框架能力：同 (source, code) 在窗口内合并为一条，
//     带 `repeat_count` / `first_t` / `last_t` —— 事件风暴自动收敛
//   · **有界内存**：条目数超过容量即淘汰最旧，并如实上报 `dropped()`
//     （可观测性组件必须能报告自己的数据丢失，否则比没有更危险）
//   · 结构化数据：每条事件带最多 4 个 (key, value) 数值字段
//
// 条目用 `std::list` 存储 —— 需要**稳定迭代器**：抑制时要在原地更新条目，
// 淘汰时要从头部删除。SOE 是"事件级"频率（每秒几条），不是"拍级"，
// 所以链表较差的局部性在这里不是问题。
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iterator>
#include <list>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace ems {

// =====================================================================
// 等级
// =====================================================================
enum class SoeLevel : int {
    kDebug = 0,   // 调试细节（默认关）
    kInfo  = 1,   // 正常事件（状态迁移 / 恢复）
    kWarn  = 2,   // 预警（降功率 / 接近限值）
    kError = 3,   // 故障（门控 / 通信中断）
    kFatal = 4,   // 致命（不可继续运行）
};

inline const char* soe_level_name(SoeLevel l) {
    switch (l) {
        case SoeLevel::kDebug: return "DEBUG";
        case SoeLevel::kInfo:  return "INFO";
        case SoeLevel::kWarn:  return "WARN";
        case SoeLevel::kError: return "ERROR";
        case SoeLevel::kFatal: return "FATAL";
    }
    return "?";
}

inline bool soe_level_from_name(const std::string& s, SoeLevel* out) {
    if (!out) return false;
    if (s == "DEBUG") { *out = SoeLevel::kDebug; return true; }
    if (s == "INFO")  { *out = SoeLevel::kInfo;  return true; }
    if (s == "WARN")  { *out = SoeLevel::kWarn;  return true; }
    if (s == "ERROR") { *out = SoeLevel::kError; return true; }
    if (s == "FATAL") { *out = SoeLevel::kFatal; return true; }
    return false;
}

// =====================================================================
// 来源（子系统）
// =====================================================================
enum class SoeSource : int {
    kSystem      = 0,   // 观察者自身 / 框架
    kFsm         = 1,   // 06/ 状态机
    kSafety      = 2,   // 05/ 安全约束引擎
    kCoordinator = 3,   // 08/ 优化调度协同
    kStrategy    = 4,   // 04/ 策略层
    kDevice      = 5,   // 设备（PCS/BMS）
    kComm        = 6,   // 通信 / 采集
    kConfig      = 7,   // 配置（P1）
    kEcon        = 8,   // 经济性
};

inline const char* soe_source_name(SoeSource s) {
    switch (s) {
        case SoeSource::kSystem:      return "SYSTEM";
        case SoeSource::kFsm:         return "FSM";
        case SoeSource::kSafety:      return "SAFETY";
        case SoeSource::kCoordinator: return "COORD";
        case SoeSource::kStrategy:    return "STRATEGY";
        case SoeSource::kDevice:      return "DEVICE";
        case SoeSource::kComm:        return "COMM";
        case SoeSource::kConfig:      return "CONFIG";
        case SoeSource::kEcon:        return "ECON";
    }
    return "?";
}

// =====================================================================
// 事件码
//
// 命名约定：`Start` / `End` 成对出现 —— 这是"边沿检测 + 抑制"的产物：
// 一次持续 838 拍的限幅只产生 1 条 Start（repeat_count=838）+ 1 条 End。
// =====================================================================
enum class SoeCode : int {
    kNone = 0,

    // ---- 状态机 ----
    kFsmTransition   = 100,
    kFsmEmergencyStop= 101,
    kFsmRunPermit    = 102,

    // ---- 安全约束 ----
    kSafetyClipStart = 200,
    kSafetyClipEnd   = 201,
    kGridReverseStart= 202,
    kGridReverseEnd  = 203,
    kGridOverStart   = 204,
    kGridOverEnd     = 205,
    kTrOverloadStart = 206,
    kTrOverloadEnd   = 207,
    kSocLowStart     = 208,
    kSocLowEnd       = 209,
    kSocHighStart    = 210,
    kSocHighEnd      = 211,
    kTempHighStart   = 212,
    kTempHighEnd     = 213,
    kRampLimitedStart= 214,
    kRampLimitedEnd  = 215,

    // ---- 设备 / 通信 ----
    kCommLostBms     = 300,
    kCommLostMeter   = 301,
    kCommLostPcs     = 302,
    kCommRestored    = 303,
    kDataStaleStart  = 304,
    kDataStaleEnd    = 305,
    kPcsFaultSet     = 306,
    kPcsFaultClear   = 307,
    kDeviceOffline   = 308,
    kDeviceOnline    = 309,
    kHoldLastStart   = 310,
    kHoldLastEnd     = 311,

    // ---- 协同层 ----
    kReoptFired      = 400,
    kPlanInvalidStart= 401,
    kPlanValidEnd    = 402,

    // ---- 经济性 ----
    kDemandBreachStart = 500,
    kDemandBreachEnd   = 501,

    // ---- 系统 ----
    kObserverStart   = 900,
    kObserverStop    = 901,
    kBufferOverflow  = 902,
    kInvariantBroken = 903,
};

inline const char* soe_code_name(SoeCode c) {
    switch (c) {
        case SoeCode::kNone:             return "NONE";
        case SoeCode::kFsmTransition:    return "FSM_TRANSITION";
        case SoeCode::kFsmEmergencyStop: return "FSM_EMERGENCY_STOP";
        case SoeCode::kFsmRunPermit:     return "FSM_RUN_PERMIT";
        case SoeCode::kSafetyClipStart:  return "SAFETY_CLIP_START";
        case SoeCode::kSafetyClipEnd:    return "SAFETY_CLIP_END";
        case SoeCode::kGridReverseStart: return "GRID_REVERSE_START";
        case SoeCode::kGridReverseEnd:   return "GRID_REVERSE_END";
        case SoeCode::kGridOverStart:    return "GRID_OVER_START";
        case SoeCode::kGridOverEnd:      return "GRID_OVER_END";
        case SoeCode::kTrOverloadStart:  return "TR_OVERLOAD_START";
        case SoeCode::kTrOverloadEnd:    return "TR_OVERLOAD_END";
        case SoeCode::kSocLowStart:      return "SOC_LOW_START";
        case SoeCode::kSocLowEnd:        return "SOC_LOW_END";
        case SoeCode::kSocHighStart:     return "SOC_HIGH_START";
        case SoeCode::kSocHighEnd:       return "SOC_HIGH_END";
        case SoeCode::kTempHighStart:    return "TEMP_HIGH_START";
        case SoeCode::kTempHighEnd:      return "TEMP_HIGH_END";
        case SoeCode::kRampLimitedStart: return "RAMP_LIMITED_START";
        case SoeCode::kRampLimitedEnd:   return "RAMP_LIMITED_END";
        case SoeCode::kCommLostBms:      return "COMM_LOST_BMS";
        case SoeCode::kCommLostMeter:    return "COMM_LOST_METER";
        case SoeCode::kCommLostPcs:      return "COMM_LOST_PCS";
        case SoeCode::kCommRestored:     return "COMM_RESTORED";
        case SoeCode::kDataStaleStart:   return "DATA_STALE_START";
        case SoeCode::kDataStaleEnd:     return "DATA_STALE_END";
        case SoeCode::kPcsFaultSet:      return "PCS_FAULT_SET";
        case SoeCode::kPcsFaultClear:    return "PCS_FAULT_CLEAR";
        case SoeCode::kDeviceOffline:    return "DEVICE_OFFLINE";
        case SoeCode::kDeviceOnline:     return "DEVICE_ONLINE";
        case SoeCode::kHoldLastStart:    return "HOLD_LAST_START";
        case SoeCode::kHoldLastEnd:      return "HOLD_LAST_END";
        case SoeCode::kReoptFired:       return "REOPT_FIRED";
        case SoeCode::kPlanInvalidStart: return "PLAN_INVALID_START";
        case SoeCode::kPlanValidEnd:     return "PLAN_VALID_END";
        case SoeCode::kDemandBreachStart:return "DEMAND_BREACH_START";
        case SoeCode::kDemandBreachEnd:  return "DEMAND_BREACH_END";
        case SoeCode::kObserverStart:    return "OBSERVER_START";
        case SoeCode::kObserverStop:     return "OBSERVER_STOP";
        case SoeCode::kBufferOverflow:   return "BUFFER_OVERFLOW";
        case SoeCode::kInvariantBroken:  return "INVARIANT_BROKEN";
    }
    return "?";
}

// =====================================================================
// 结构化字段（最多 4 个，避免每条事件都堆分配）
// =====================================================================
struct SoeField {
    std::string key;
    double      value = 0.0;
    SoeField() = default;
    SoeField(const char* k, double v) : key(k), value(v) {}
    SoeField(std::string k, double v) : key(std::move(k)), value(v) {}
};

// =====================================================================
// 事件
// =====================================================================
struct SoeEvent {
    double    t          = 0.0;    // 首次发生时刻
    double    first_t    = 0.0;    // 与 t 同义（显式命名，便于阅读）
    double    last_t     = 0.0;    // 最后一次发生时刻
    int       repeat_count = 1;    // 被抑制合并的次数（含首次）
    SoeLevel  level      = SoeLevel::kInfo;
    SoeSource source     = SoeSource::kSystem;
    SoeCode   code       = SoeCode::kNone;
    std::string message;
    std::vector<SoeField> data;

    // 是否参与时间窗抑制。默认 true。
    // 置 false 用于"每次都独立成条"的事件 —— 典型是状态迁移与硬不变量违例：
    // 合并它们会丢掉迁移链（A→B→C 变成 A 重复 2 次），而迁移链正是要留的。
    bool suppressible = true;

    bool   suppressed()  const { return repeat_count > 1; }
    double duration_s()  const { return last_t - first_t; }

    double get(const std::string& key, double def = 0.0) const {
        for (const auto& f : data) if (f.key == key) return f.value;
        return def;
    }

    std::string key() const {
        return std::string(soe_source_name(source)) + ":" + soe_code_name(code);
    }
};

// =====================================================================
// 配置
// =====================================================================
struct SoeConfig {
    // 条目上限。超出即淘汰最旧并计入 dropped()。
    // 4096 条 × 约 150 字节 ≈ 600 KB —— 现场可常驻。
    size_t capacity = 4096;

    // 同 (source, code) 在此窗口内重复发生 → 合并为一条（抑制事件风暴）。
    // 窗口从**上一次发生时刻**起算（滑动窗口），所以持续数小时的风暴
    // 会一直合并成一条，`duration_s()` 就是风暴时长。
    double suppress_window_s = 5.0;
    bool   suppress_enabled  = true;
};

// =====================================================================
// SOE 日志
// =====================================================================
class SoeLog {
public:
    explicit SoeLog(const SoeConfig& c = SoeConfig()) : cfg_(c) {}

    void set_config(const SoeConfig& c) { cfg_ = c; }
    const SoeConfig& config() const { return cfg_; }

    void reset() {
        entries_.clear();
        index_.clear();
        dropped_ = 0;
        total_ = 0;
        suppressed_ = 0;
    }

    // ---- 写入 ----

    // 返回 true 表示新建了一条，false 表示被合并进已有条目
    bool push(const SoeEvent& ev_in) {
        SoeEvent ev = ev_in;
        ev.first_t = ev.t;
        ev.last_t  = ev.t;
        ++total_;

        const std::string k = ev.key();

        if (cfg_.suppress_enabled && ev.repeat_count == 1 && ev.suppressible) {
            auto it = index_.find(k);
            if (it != index_.end()) {
                SoeEvent& prev = *(it->second);
                if (ev.t - prev.last_t <= cfg_.suppress_window_s) {
                    // 合并：更新尾部时刻与计数，data 取"较大幅度"的那个值，
                    // 保证抑制后不丢失峰值信息（例如最大越限幅度）。
                    prev.repeat_count += 1;
                    prev.last_t = ev.t;
                    merge_data(prev, ev);
                    ++suppressed_;
                    return false;
                }
            }
        }

        entries_.push_back(std::move(ev));
        index_[k] = std::prev(entries_.end());

        // 有界内存：淘汰最旧
        while (entries_.size() > cfg_.capacity) {
            const SoeEvent& oldest = entries_.front();
            index_.erase(oldest.key());
            entries_.pop_front();
            ++dropped_;
        }
        return true;
    }

    void add(double t, SoeLevel lv, SoeSource src, SoeCode code,
             const std::string& msg) {
        SoeEvent ev;
        ev.t = t; ev.level = lv; ev.source = src; ev.code = code; ev.message = msg;
        push(ev);
    }

    void add(double t, SoeLevel lv, SoeSource src, SoeCode code,
             const std::string& msg, std::initializer_list<SoeField> fields) {
        SoeEvent ev;
        ev.t = t; ev.level = lv; ev.source = src; ev.code = code; ev.message = msg;
        for (const auto& f : fields) {
            if (ev.data.size() >= 4) break;
            ev.data.push_back(f);
        }
        push(ev);
    }

    // ---- 查询 ----

    size_t size()      const { return entries_.size(); }
    size_t dropped()   const { return dropped_; }        // 被容量淘汰的条数
    size_t total()     const { return total_; }          // 累计 push 次数（含被抑制）
    size_t suppressed()const { return suppressed_; }     // 被抑制合并的次数
    bool   overflowed()const { return dropped_ > 0; }

    std::vector<SoeEvent> events() const {
        return std::vector<SoeEvent>(entries_.begin(), entries_.end());
    }

    size_t count(SoeLevel lv) const {
        size_t n = 0;
        for (const auto& e : entries_) if (e.level == lv) ++n;
        return n;
    }
    size_t count_at_least(SoeLevel lv) const {
        size_t n = 0;
        for (const auto& e : entries_) if (e.level >= lv) ++n;
        return n;
    }
    size_t count(SoeSource src) const {
        size_t n = 0;
        for (const auto& e : entries_) if (e.source == src) ++n;
        return n;
    }
    size_t count(SoeCode code) const {
        size_t n = 0;
        for (const auto& e : entries_) if (e.code == code) ++n;
        return n;
    }

    bool has(SoeCode code) const { return count(code) > 0; }

    // 该事件码累计发生的"次数"（把抑制的展开算）—— 衡量严重程度用这个
    int total_occurrences(SoeCode code) const {
        int n = 0;
        for (const auto& e : entries_) if (e.code == code) n += e.repeat_count;
        return n;
    }

    const SoeEvent* find(SoeCode code) const {
        for (const auto& e : entries_) if (e.code == code) return &e;
        return nullptr;
    }
    const SoeEvent* last(SoeCode code) const {
        const SoeEvent* r = nullptr;
        for (const auto& e : entries_) if (e.code == code) r = &e;
        return r;
    }

    std::vector<SoeEvent> filter_level(SoeLevel min_lv) const {
        std::vector<SoeEvent> v;
        for (const auto& e : entries_) if (e.level >= min_lv) v.push_back(e);
        return v;
    }
    std::vector<SoeEvent> filter_source(SoeSource src) const {
        std::vector<SoeEvent> v;
        for (const auto& e : entries_) if (e.source == src) v.push_back(e);
        return v;
    }
    std::vector<SoeEvent> filter_range(double t0, double t1) const {
        std::vector<SoeEvent> v;
        for (const auto& e : entries_) if (e.last_t >= t0 && e.first_t <= t1) v.push_back(e);
        return v;
    }

    // ---- 导出 ----

    static std::string csv_header() {
        return "t_first,t_last,duration_s,repeat,level,source,code,message,data";
    }

    std::string to_csv() const {
        std::ostringstream os;
        os << csv_header() << "\n";
        for (const auto& e : entries_) {
            os << e.first_t << "," << e.last_t << "," << e.duration_s() << ","
               << e.repeat_count << "," << soe_level_name(e.level) << ","
               << soe_source_name(e.source) << "," << soe_code_name(e.code) << ","
               << "\"" << escape_csv(e.message) << "\",";
            os << "\"";
            for (size_t i = 0; i < e.data.size(); ++i) {
                if (i) os << "; ";
                os << e.data[i].key << "=" << e.data[i].value;
            }
            os << "\"\n";
        }
        return os.str();
    }

    std::string to_json() const {
        std::ostringstream os;
        os << "{\n  \"summary\": " << summary_json() << ",\n  \"events\": [\n";
        size_t i = 0;
        for (const auto& e : entries_) {
            os << "    {\"t\": " << e.first_t << ", \"t_last\": " << e.last_t
               << ", \"duration_s\": " << e.duration_s()
               << ", \"repeat\": " << e.repeat_count
               << ", \"level\": \"" << soe_level_name(e.level) << "\""
               << ", \"source\": \"" << soe_source_name(e.source) << "\""
               << ", \"code\": \"" << soe_code_name(e.code) << "\""
               << ", \"message\": \"" << escape_json(e.message) << "\"";
            if (!e.data.empty()) {
                os << ", \"data\": {";
                for (size_t k = 0; k < e.data.size(); ++k) {
                    if (k) os << ", ";
                    os << "\"" << escape_json(e.data[k].key) << "\": " << e.data[k].value;
                }
                os << "}";
            }
            os << "}" << (++i < entries_.size() ? "," : "") << "\n";
        }
        os << "  ]\n}\n";
        return os.str();
    }

    std::string summary_json() const {
        std::ostringstream os;
        os << "{\"total_pushed\": " << total_
           << ", \"stored\": " << entries_.size()
           << ", \"suppressed\": " << suppressed_
           << ", \"dropped\": " << dropped_
           << ", \"by_level\": {";
        bool first = true;
        for (int lv = 0; lv <= 4; ++lv) {
            const SoeLevel L = static_cast<SoeLevel>(lv);
            const size_t n = count(L);
            if (n == 0) continue;
            if (!first) os << ", ";
            os << "\"" << soe_level_name(L) << "\": " << n;
            first = false;
        }
        os << "}, \"by_source\": {";
        first = true;
        for (int s = 0; s <= 8; ++s) {
            const SoeSource S = static_cast<SoeSource>(s);
            const size_t n = count(S);
            if (n == 0) continue;
            if (!first) os << ", ";
            os << "\"" << soe_source_name(S) << "\": " << n;
            first = false;
        }
        os << "}}";
        return os.str();
    }

    std::string summary_text() const {
        std::ostringstream os;
        os << "SOE: 存储 " << entries_.size() << " 条"
           << " / 累计 " << total_ << " 次"
           << "（抑制合并 " << suppressed_ << " 次）";
        if (dropped_) os << " [已淘汰 " << dropped_ << " 条]";
        os << "\n";
        for (int lv = 4; lv >= 0; --lv) {
            const SoeLevel L = static_cast<SoeLevel>(lv);
            const size_t n = count(L);
            if (n == 0) continue;
            os << "  " << soe_level_name(L) << ": " << n << "\n";
        }
        return os.str();
    }

private:
    static void merge_data(SoeEvent& into, const SoeEvent& from) {
        // 抑制后仍要保住"最严重"的信息：同名字段取绝对值更大者
        for (const auto& f : from.data) {
            bool merged = false;
            for (auto& g : into.data) {
                if (g.key == f.key) {
                    if (std::fabs(f.value) > std::fabs(g.value)) g.value = f.value;
                    merged = true;
                    break;
                }
            }
            if (!merged && into.data.size() < 4) into.data.push_back(f);
        }
    }

    static std::string escape_csv(const std::string& s) {
        std::string o;
        o.reserve(s.size());
        for (char c : s) {
            // 换行必须转成字面量 \n —— CSV 允许引号内嵌换行，但那样一条事件
            // 会跨多行，运维用 wc -l / grep 统计时全线错位。SOE 导出必须是
            // **一事件一行**。
            if (c == '"')       o += "\"\"";
            else if (c == '\n') o += "\\n";
            else if (c == '\r') o += "\\r";
            else                o.push_back(c);
        }
        return o;
    }

    static std::string escape_json(const std::string& s) {
        std::string o;
        o.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '"':  o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n";  break;
                case '\r': o += "\\r";  break;
                case '\t': o += "\\t";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        o += buf;
                    } else {
                        o.push_back(c);
                    }
            }
        }
        return o;
    }

    SoeConfig cfg_;
    std::list<SoeEvent> entries_;                                   // 插入序 = 时间序
    std::unordered_map<std::string, std::list<SoeEvent>::iterator> index_;
    size_t dropped_    = 0;
    size_t total_      = 0;
    size_t suppressed_ = 0;
};

} // namespace ems
