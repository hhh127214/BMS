// =====================================================================
// 20/ — SOE 持久化 SoeStore（落盘先行，内存有界）
//
// ---------------------------------------------------------------------
// 要解决的两件事（交接文档 §3.6「⑥」+ §3.3 第 3 条）
// ---------------------------------------------------------------------
//  ① `06/state_machine.h` 的 `history()` **只在内存**，长跑会丢 ——
//     进程一重启，状态迁移链就没了（P2 的观察者是运行期对象，同样只在内存）。
//  ② P2 的 `SoeLog` **有界 4096 条 ≈ 600 KB**，超出即淘汰最旧并计 `dropped()`。
//     它**如实上报**丢失（这点是对的），但上报不等于不丢 —— 现场要的是"不丢"。
//
// 本文件的做法：
//   · **文件是唯一真相源**（append-only，一事件一行），内存只是缓存 + 索引；
//   · `append()` **先写盘并 flush，再入内存** —— 掉电最坏丢一条，且该条尚未返回成功；
//   · 内存容量 `memory_capacity` 只约束**热缓存**，超出后淘汰缓存但**索引与文件都在**
//     —— 检索仍然能取回全部条目。这就是"超出容量上限时不丢"。
//
// ---------------------------------------------------------------------
// 文件格式（**稳定契约**，外部工具可直接读）
// ---------------------------------------------------------------------
//   纯文本、LF 行尾、UTF-8；第 1 行是版本行，其后每行一条事件：
//
//     seq,t,level,source,code,kind,value,message
//
//   字段含义：
//     seq      单调递增序号（重开文件后接着上一个）
//     t        事件时刻（秒，%.6f）
//     level    SoeLevel：DEBUG/INFO/WARN/ERROR/FATAL
//     source   SoeSource：SYSTEM/FSM/SAFETY/COORD/STRATEGY/DEVICE/COMM/CONFIG/ECON
//     code     SoeCode 名（如 COMM_LOST_BMS）
//     kind     SET / CLEAR
//     value    %.10g
//     message  双引号包裹；内部 `"` → `""`，`\` → `\\`，换行/回车 → 字面 `\n` / `\r`
//              （**一条事件必须一行** —— 否则运维用 wc -l / grep 统计会错位）
//
//   示例行：
//     1,0.500000,ERROR,COMM,COMM_LOST_BMS,SET,0,"bms comm lost"
//     2,20.500000,ERROR,COMM,COMM_LOST_BMS,CLEAR,0,"cleared: COMM_LOST_BMS"
//
//   `--dump` 例子（见 20/src/soe_dump.cpp）：
//     soe_dump.exe --file 20\build\soe.log --min-level WARN
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "alarm_model.h"   // AlarmEvent / SoeLevel / SoeSource / SoeCode 名称

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <ostream>
#include <string>
#include <vector>

namespace ems {

// =====================================================================
// 存储记录（SOE 事件）
// =====================================================================
struct SoeStoreRecord {
    long long   seq     = 0;
    double      t       = 0.0;
    SoeLevel    level   = SoeLevel::kInfo;
    SoeSource   source  = SoeSource::kSystem;
    SoeCode     code    = SoeCode::kNone;
    bool        is_set  = true;
    double      value   = 0.0;
    std::string message;

    const char* kind_name() const { return is_set ? "SET" : "CLEAR"; }

    // 文件行（不含行尾）
    std::string to_line() const {
        char head[256];
        std::snprintf(head, sizeof(head), "%lld,%.6f,%s,%s,%s,%s,%.10g,",
                      seq, t, soe_level_name(level), soe_source_name(source),
                      soe_code_name(code), kind_name(), value);
        std::string s(head);
        s += '"';
        s += escape(message);
        s += '"';
        return s;
    }

    static std::string escape(const std::string& in) {
        std::string o;
        o.reserve(in.size() + 8);
        for (char c : in) {
            if (c == '"')       o += "\"\"";
            else if (c == '\\') o += "\\\\";
            else if (c == '\n') o += "\\n";
            else if (c == '\r') o += "\\r";
            else                o.push_back(c);
        }
        return o;
    }
    static std::string unescape(const std::string& in) {
        std::string o;
        o.reserve(in.size());
        for (size_t i = 0; i < in.size(); ++i) {
            if (in[i] == '"' && i + 1 < in.size() && in[i + 1] == '"') { o += '"'; ++i; }
            else if (in[i] == '\\' && i + 1 < in.size()) {
                const char n = in[i + 1];
                if (n == '\\')      { o += '\\'; ++i; }
                else if (n == 'n')  { o += '\n'; ++i; }
                else if (n == 'r')  { o += '\r'; ++i; }
                else                { o += '\\'; }        // 未知转义：保留反斜杠
            }
            else o.push_back(in[i]);
        }
        return o;
    }

    // 解析一行；失败返回 false 并填 err
    static bool parse_line(const std::string& line, SoeStoreRecord& out, std::string* err) {
        // 定位前 7 个逗号
        size_t pos[7];
        int n = 0;
        for (size_t i = 0; i < line.size() && n < 7; ++i) {
            if (line[i] == ',') pos[n++] = i;
        }
        if (n < 7) { if (err) *err = "too few fields"; return false; }

        auto field = [&](int i) -> std::string {
            const size_t b = (i == 0) ? 0 : pos[i - 1] + 1;
            const size_t e = (i < 7) ? pos[i] : line.size();
            return line.substr(b, e - b);
        };

        out.seq    = std::strtoll(field(0).c_str(), nullptr, 10);
        out.t      = std::strtod(field(1).c_str(), nullptr);
        if (!soe_level_from_name(field(2), &out.level)) { if (err) *err = "bad level"; return false; }
        out.source = parse_source(field(3));
        out.code   = parse_code(field(4));
        const std::string kind = field(5);
        out.is_set = (kind != "CLEAR");
        out.value  = std::strtod(field(6).c_str(), nullptr);

        std::string m = field(7);
        if (m.size() >= 2 && m.front() == '"' && m.back() == '"')
            m = m.substr(1, m.size() - 2);
        out.message = unescape(m);
        return true;
    }

    static SoeSource parse_source(const std::string& s) {
        for (int i = 0; i <= 8; ++i) {
            const SoeSource v = static_cast<SoeSource>(i);
            if (s == soe_source_name(v)) return v;
        }
        return SoeSource::kSystem;
    }
    static SoeCode parse_code(const std::string& s) {
        // 覆盖 soe_code_name() 的全部取值；未知码退化为 kNone 但**保留可读性**
        // 由 message 承载（不静默丢行）。
        static const SoeCode kAll[] = {
            SoeCode::kNone, SoeCode::kFsmTransition, SoeCode::kFsmEmergencyStop,
            SoeCode::kFsmRunPermit, SoeCode::kSafetyClipStart, SoeCode::kSafetyClipEnd,
            SoeCode::kGridReverseStart, SoeCode::kGridReverseEnd, SoeCode::kGridOverStart,
            SoeCode::kGridOverEnd, SoeCode::kTrOverloadStart, SoeCode::kTrOverloadEnd,
            SoeCode::kSocLowStart, SoeCode::kSocLowEnd, SoeCode::kSocHighStart,
            SoeCode::kSocHighEnd, SoeCode::kTempHighStart, SoeCode::kTempHighEnd,
            SoeCode::kRampLimitedStart, SoeCode::kRampLimitedEnd, SoeCode::kCommLostBms,
            SoeCode::kCommLostMeter, SoeCode::kCommLostPcs, SoeCode::kCommRestored,
            SoeCode::kDataStaleStart, SoeCode::kDataStaleEnd, SoeCode::kPcsFaultSet,
            SoeCode::kPcsFaultClear, SoeCode::kDeviceOffline, SoeCode::kDeviceOnline,
            SoeCode::kHoldLastStart, SoeCode::kHoldLastEnd, SoeCode::kReoptFired,
            SoeCode::kPlanInvalidStart, SoeCode::kPlanValidEnd, SoeCode::kDemandBreachStart,
            SoeCode::kDemandBreachEnd, SoeCode::kObserverStart, SoeCode::kObserverStop,
            SoeCode::kBufferOverflow, SoeCode::kInvariantBroken,
        };
        for (SoeCode c : kAll) if (s == soe_code_name(c)) return c;
        return SoeCode::kNone;
    }

    // 与 AlarmEvent 的桥（告警装配器 → SOE 存储）
    static SoeStoreRecord from_event(const AlarmEvent& ev) {
        SoeStoreRecord r;
        r.t       = ev.t;
        r.level   = soe_level_of(ev.severity);
        r.source  = ev.source;
        r.code    = ev.code;
        r.is_set  = ev.is_set;
        r.value   = ev.value;
        r.message = ev.message.empty()
                        ? std::string(soe_code_name(ev.code))
                        : ev.message;
        return r;
    }
};

// =====================================================================
// SoeStore
// =====================================================================
class SoeStore {
public:
    struct Config {
        // 内存热缓存条数。**不是**文件容量 —— 文件无上限增长（由运维轮转）。
        // 超出后淘汰最旧**缓存**，索引与文件都保留 → 检索仍能取回全部。
        size_t memory_capacity = 4096;
        bool   flush_each = true;   // 每条 append 后 flush（掉电最多丢当前未返回的一条）
    };

    static const char* header_line() {
        return "# EMS_SOE v1  fields: seq,t,level,source,code,kind,value,message";
    }

    SoeStore() = default;
    explicit SoeStore(const Config& c) : cfg_(c) {}

    ~SoeStore() { close(); }

    void set_config(const Config& c) { cfg_ = c; }

    // -----------------------------------------------------------------
    // 打开 / 新建。文件已存在 → **重建索引并接着写**（这就是"重启"路径）。
    // -----------------------------------------------------------------
    bool open(const std::string& path, std::string* err = nullptr) {
        close();
        path_ = path;
        err_.clear();

        // 先扫描已有文件（若存在）建立索引
        bool any_line = false;
        {
            std::ifstream in(path_.c_str(), std::ios::in | std::ios::binary);
            if (in.is_open()) {
                std::string line;
                std::streamoff off = 0;
                bool first = true;
                while (std::getline(in, line)) {
                    any_line = true;
                    const std::streamoff this_off = off;
                    off += static_cast<std::streamoff>(line.size()) + 1;  // +\n
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (first) { first = false; if (!line.empty() && line[0] == '#') continue; }
                    if (line.empty() || line[0] == '#') continue;
                    SoeStoreRecord r;
                    std::string perr;
                    if (!SoeStoreRecord::parse_line(line, r, &perr)) {
                        // 损坏行：显式报错，指出行号 —— 不静默跳过
                        if (err) *err = "corrupt SOE line at offset " +
                                        std::to_string((long long)this_off) + ": " + perr;
                        err_ = *err;
                        // 失败即回到干净状态：不允许调用方拿到一个"半加载"的 store
                        index_.clear();
                        memory_.clear();
                        last_seq_ = 0;
                        mem_dropped_ = 0;
                        return false;
                    }
                    index_.push_back(IndexEntry{r.seq, this_off, r.t, r.level, r.source, r.code});
                    push_memory(r);
                    last_seq_ = r.seq;
                }
            }
        }

        // 打开写入流（追加，二进制 → 偏移可预测、LF 不被改写）
        out_.open(path_.c_str(), std::ios::out | std::ios::binary | std::ios::app);
        if (!out_.is_open()) {
            if (err) *err = "cannot open for append: " + path_;
            err_ = *err;
            return false;
        }
        out_.seekp(0, std::ios::end);
        write_off_ = static_cast<std::streamoff>(out_.tellp());
        // 新建（或空文件）时先落版本头 —— 外部工具靠它识别格式与版本
        if (!any_line) {
            const std::string h = header_line();
            out_ << h << '\n';
            out_.flush();
            write_off_ += static_cast<std::streamoff>(h.size()) + 1;
        }
        return true;
    }

    void close() {
        if (out_.is_open()) out_.close();
        index_.clear();
        memory_.clear();
        last_seq_ = 0;
        write_off_ = 0;
        mem_dropped_ = 0;
    }

    // -----------------------------------------------------------------
    // 追加一条：**先落盘 + flush，后入内存**。
    // 返回 false 时该条未持久化 —— 调用方必须当作"没记上"处理。
    // -----------------------------------------------------------------
    bool append(SoeStoreRecord rec, std::string* err = nullptr) {
        if (!out_.is_open()) {
            if (err) *err = "store not open";
            return false;
        }
        rec.seq = ++last_seq_;
        const std::string line = rec.to_line();
        out_ << line << '\n';
        if (cfg_.flush_each) out_.flush();
        if (!out_.good()) {
            // 写失败（磁盘满 / 卷只读）：文件里可能留下**半行**。
            // 此时必须把 store 关掉，否则后续 append 会追加到半行之后，
            // 生成一条永远解析不了的损坏行 —— 那是最坏的一种静默损坏。
            --last_seq_;
            out_.close();
            if (err) *err = "write failed (disk full?) - store closed";
            err_ = *err;
            return false;
        }
        index_.push_back(IndexEntry{rec.seq, write_off_, rec.t, rec.level, rec.source, rec.code});
        write_off_ += static_cast<std::streamoff>(line.size()) + 1;
        push_memory(rec);
        return true;
    }

    bool append(const AlarmEvent& ev, std::string* err = nullptr) {
        return append(SoeStoreRecord::from_event(ev), err);
    }

    // -----------------------------------------------------------------
    // 检索。索引筛候选，再从**文件**取正文（内存命中则直接用缓存）。
    // -----------------------------------------------------------------
    std::vector<SoeStoreRecord> all() const { return query_range(-1e300, 1e300); }

    std::vector<SoeStoreRecord> query_range(double t0, double t1) const {
        return collect([&](const IndexEntry& e) { return e.t >= t0 && e.t <= t1; });
    }
    std::vector<SoeStoreRecord> query_level(SoeLevel min_level) const {
        return collect([&](const IndexEntry& e) { return e.level >= min_level; });
    }
    std::vector<SoeStoreRecord> query_source(SoeSource src) const {
        return collect([&](const IndexEntry& e) { return e.source == src; });
    }
    std::vector<SoeStoreRecord> query(double t0, double t1, SoeLevel min_level,
                                     SoeSource src) const {
        return collect([&](const IndexEntry& e) {
            return e.t >= t0 && e.t <= t1 && e.level >= min_level && e.source == src;
        });
    }

    // ---- 规模 / 丢失上报（可观测性组件必须能报告自己的数据丢失）----
    size_t size()              const { return index_.size(); }        // 文件里的总条数
    size_t memory_size()       const { return memory_.size(); }       // 热缓存条数
    size_t dropped_from_memory() const { return mem_dropped_; }       // 被缓存淘汰的条数
    long long last_seq()       const { return last_seq_; }
    const std::string& last_error() const { return err_; }

    // ---- dump：把全部条目重新规范化输出（给 `--dump` 用）----
    bool dump(std::ostream& os, SoeLevel min_level = SoeLevel::kDebug) const {
        os << header_line() << "\n";
        for (const auto& e : index_) {
            if (e.level < min_level) continue;
            SoeStoreRecord r;
            if (!read_at(e, r)) return false;
            os << r.to_line() << "\n";
        }
        return true;
    }

    // 单独解析一个文件（不需要 open 写流）—— 给外部工具 / 测试用
    static bool parse_file(const std::string& path,
                           std::vector<SoeStoreRecord>& out,
                           std::string* err = nullptr) {
        std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
        if (!in.is_open()) { if (err) *err = "cannot open: " + path; return false; }
        std::string line;
        int lineno = 0;
        while (std::getline(in, line)) {
            ++lineno;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            SoeStoreRecord r;
            std::string perr;
            if (!SoeStoreRecord::parse_line(line, r, &perr)) {
                if (err) *err = "line " + std::to_string(lineno) + ": " + perr;
                return false;
            }
            out.push_back(r);
        }
        return true;
    }

private:
    struct IndexEntry {
        long long  seq;
        std::streamoff off;
        double     t;
        SoeLevel   level;
        SoeSource  source;
        SoeCode    code;
    };

    template <typename Pred>
    std::vector<SoeStoreRecord> collect(Pred pred) const {
        std::vector<SoeStoreRecord> v;
        for (const auto& e : index_) {
            if (!pred(e)) continue;
            SoeStoreRecord r;
            if (read_at(e, r)) v.push_back(r);
        }
        return v;
    }

    // 先看内存缓存；未命中再回到文件按偏移读
    bool read_at(const IndexEntry& e, SoeStoreRecord& out) const {
        for (auto it = memory_.rbegin(); it != memory_.rend(); ++it) {
            if (it->seq == e.seq) { out = *it; return true; }
        }
        std::ifstream in(path_.c_str(), std::ios::in | std::ios::binary);
        if (!in.is_open()) return false;
        in.seekg(e.off);
        std::string line;
        if (!std::getline(in, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string perr;
        return SoeStoreRecord::parse_line(line, out, &perr);
    }

    void push_memory(const SoeStoreRecord& r) {
        memory_.push_back(r);
        while (memory_.size() > cfg_.memory_capacity) {
            memory_.pop_front();
            ++mem_dropped_;
        }
    }

    Config cfg_{};
    std::string path_;
    std::string err_;
    std::ofstream out_;
    std::vector<IndexEntry> index_;
    std::deque<SoeStoreRecord> memory_;   // 热缓存（有界）
    long long  last_seq_ = 0;
    std::streamoff write_off_ = 0;
    size_t mem_dropped_ = 0;
};

} // namespace ems
