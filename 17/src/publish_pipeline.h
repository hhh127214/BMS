// =====================================================================
// 17/ — 规模化发布流水线：变化上传（C2）· 分级节拍（C3）· 批量写（C1）
//
// 缺口原文（§10.3 C 类）：
//   C1 | **批量写 API 存在但零调用** | `rt_db_set_multiple_values()` … 本项目中无人调用
//      → 几千点 = 几千次 seqlock 往返
//   C2 | **无变化上传（死区）**     | `publish_all()` 每拍**全量重发全表**
//   C3 | **无分级节拍**             | 单体电压不需要 100 ms，SOC 需要。现在只有一个节拍
//
// 三者是**一条链**，不是三个独立特性：
//
//     分级节拍            死区                批量写
//   （只跑该跑的） → （只发变了够多的） → （一次调用发 N 点）
//
// 所以本文件把它们做成**一条流水线**，并给出"全量重发 vs 优化后"的
// **写入次数对比** —— 这才是 C 类的量化结论：不是"做了优化"，是"从 X 次降到 Y 次"。
//
// ---------------------------------------------------------------------
// ★ 三条纪律（各自都有会红的断言守着，见 tests/test_scale_rtdb.cpp）：
//
//   1. **死区绝不能把安全位吃掉**（DeadbandPublisher）
//      禁充放位/故障位是"关键安全点"，配置成死区 0 = **永远发布**。
//      如果它们被死区抑制，"BMS 禁止放电"这条 L0 最底层的锁就永远传不上去 ——
//      而这不是"少发了一帧"，是**安全链断了**。见 §10.1 A1。
//
//   2. **冷启动必须全部先跑一次**（TieredScheduler）
//      慢档点（单体电压）若"等到第一个周期才发"，那么从上电到第一个慢档周期，
//      EMS 看到的这些点是**从未收到过**（值 = RT_DB 默认值）。
//      在点表的语义里"没被写过"与"写过 0"不可区分 —— 于是上电头几秒所有
//      单体电压都是 0 V，越限判据会集体误报。
//
//   3. **批量写要么整批成功，要么整批拒绝**（IValueWriter::write_batch）
//      点名先全部解析成索引，再发一次调用。解析到一半失败就**不写** ——
//      半批写入会让"这一拍的点集"处于一个既不是旧值也不是新值的中间态。
//
// 编译：纯头文件（inline）。真正对接 RT_DB 的写在 rtdb_batch_writer.h。
// =====================================================================

#pragma once

#include "point_sink.h"

#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace ems {
namespace devsim {

// =====================================================================
// C2 — 变化上传（死区）
// =====================================================================
class DeadbandPublisher {
public:
    struct Stats {
        long published  = 0;
        long suppressed = 0;
        double suppress_rate() const {
            const long n = published + suppressed;
            return (n > 0) ? static_cast<double>(suppressed) / static_cast<double>(n) : 0.0;
        }
    };

    // deadband <= 0 → **永远发布**（关键安全点用这个）
    void configure(const std::string& name, double deadband, bool always = false) {
        Entry& e = m_[name];
        e.db     = deadband;
        e.always = always;
    }

    bool   configured(const std::string& name) const {
        return m_.find(name) != m_.end();
    }
    double deadband_of(const std::string& name) const {
        auto it = m_.find(name);
        return it == m_.end() ? 0.0 : it->second.db;
    }
    // "永远发布" = 显式 always，或死区 <= 0
    bool always_publish(const std::string& name) const {
        auto it = m_.find(name);
        if (it == m_.end()) return true;             // 未配置 → 不抑制（保守）
        return it->second.always || it->second.db <= 0.0;
    }

    // 只记 last，不计入统计（用于"初始值已经写进段"之后的第一次推进）
    void seed(const std::string& name, double value) {
        Entry& e = m_[name];
        e.last = value;
        e.has_last = true;
    }

    // 返回 true = 本拍需要发布
    bool should_publish(const std::string& name, double value) {
        Entry& e = m_[name];                        // 未配置时按 db=0 → 永远发布
        const bool always = (e.always || e.db <= 0.0);
        bool pub;
        if (always) {
            pub = true;
        } else if (!e.has_last) {
            pub = true;                             // 没有历史值 → 必须发
        } else {
            pub = std::fabs(value - e.last) > e.db; // 严格大于：等于死区 → 抑制
        }
        if (pub) {
            e.last = value;
            e.has_last = true;
            ++st_.published;
        } else {
            ++st_.suppressed;
        }
        return pub;
    }

    const Stats& stats() const { return st_; }
    void reset_stats() { st_ = Stats(); }
    std::size_t point_count() const { return m_.size(); }

private:
    struct Entry {
        double db = 0.0;
        bool   always = false;
        double last = 0.0;
        bool   has_last = false;
    };
    std::map<std::string, Entry> m_;
    Stats st_;
};

// =====================================================================
// C3 — 分级节拍
//
// 三档：快（保护/状态，100 ms 级）/ 中（功率类，1 s 级）/ 慢（单体/电表电量，5 s 级）
// =====================================================================
class TieredScheduler {
public:
    enum Tier { kFast = 0, kMedium = 1, kSlow = 2, kTierCount = 3 };

    TieredScheduler(double fast_s = 0.1, double medium_s = 1.0, double slow_s = 5.0) {
        period_[kFast]   = fast_s;
        period_[kMedium] = medium_s;
        period_[kSlow]   = slow_s;
        for (int i = 0; i < kTierCount; ++i) acc_[i] = 0.0;
    }

    void set_period(Tier t, double s) { period_[t] = (s > 0.0) ? s : 0.0; }
    double period(Tier t) const { return period_[t]; }

    void assign(const std::string& name, Tier t) { names_[t].push_back(name); }
    // 未显式指定的点按**快档**处理（保守：宁可多发，不可漏发）
    Tier tier_of(const std::string& name) const {
        for (int t = 0; t < kTierCount; ++t) {
            for (const auto& n : names_[t]) if (n == name) return static_cast<Tier>(t);
        }
        return kFast;
    }

    // 每拍推进；返回本拍到期的点名（按 快 → 中 → 慢 的顺序，档内保持登记顺序）
    std::vector<std::string> tick(double dt) {
        std::vector<std::string> out;
        ++st_.ticks;
        for (int t = 0; t < kTierCount; ++t) {
            acc_[t] += dt;
            // ★ 冷启动：第一拍**所有档都到期**。否则上电到第一个慢档周期之间，
            //   慢档点是"从未收到过"，而 RT_DB 里"没写过"与"写过 0"不可区分。
            const bool due = cold_ || (period_[t] <= 0.0) ||
                             (acc_[t] >= period_[t] - 1e-9);
            if (!due) continue;
            if (cold_) acc_[t] = 0.0;
            else       acc_[t] -= period_[t];
            ++st_.fires[t];
            for (const auto& n : names_[t]) out.push_back(n);
        }
        cold_ = false;
        return out;
    }

    struct Stats {
        long fires[kTierCount] = {0, 0, 0};
        long ticks = 0;
    };
    const Stats& stats() const { return st_; }
    void reset_stats() {
        st_ = Stats();
        for (int i = 0; i < kTierCount; ++i) acc_[i] = 0.0;
        cold_ = true;
    }
    bool cold() const { return cold_; }
    std::size_t point_count() const {
        return names_[0].size() + names_[1].size() + names_[2].size();
    }
    const std::vector<std::string>& names_of(Tier t) const { return names_[t]; }

private:
    double period_[kTierCount] = {0.1, 1.0, 5.0};
    double acc_[kTierCount]    = {0.0, 0.0, 0.0};
    bool   cold_ = true;
    std::vector<std::string> names_[kTierCount];
    Stats st_;
};

// =====================================================================
// C1 — 可注入的写接口
// =====================================================================
struct WriteStats {
    long calls        = 0;   // **API 调用次数**（批量写按 1 计 —— 这是 C1 的核心指标）
    long point_writes = 0;   // 点值写入次数
};

class IValueWriter {
public:
    virtual ~IValueWriter() = default;
    virtual bool write_one(const std::string& name, double v, int q) = 0;
    virtual bool write_batch(const std::vector<std::string>& names,
                             const std::vector<double>& vals,
                             const std::vector<int>& qs) = 0;
    virtual const WriteStats& stats() const = 0;
};

// 记录型 writer：把值存进内存表，并统计调用次数。
// 用途：① 证明"N 点批量写的调用次数 == 1"；② 证明"批量写与 N 次单点写逐点等价"。
class CountingWriter : public IValueWriter {
public:
    bool write_one(const std::string& name, double v, int q) override {
        (void)q;
        ++st_.calls;
        ++st_.point_writes;
        store_[name] = v;
        return true;
    }

    bool write_batch(const std::vector<std::string>& names,
                     const std::vector<double>& vals,
                     const std::vector<int>& qs) override {
        if (names.size() != vals.size() || vals.size() != qs.size()) {
            ++bad_calls_;
            return false;                    // 长度不一致 → 拒绝（**不半写**）
        }
        // ★ 先把所有点位解析/校验完，再"提交" —— 与 RtDbBatchWriter 同一纪律
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (names[i].empty()) { ++bad_calls_; return false; }
        }
        ++st_.calls;                          // ← **一次调用**
        st_.point_writes += static_cast<long>(names.size());
        for (std::size_t i = 0; i < names.size(); ++i) store_[names[i]] = vals[i];
        return true;
    }

    const WriteStats& stats() const override { return st_; }
    void reset_stats() { st_ = WriteStats(); }

    bool   has(const std::string& n) const { return store_.find(n) != store_.end(); }
    double at(const std::string& n) const {
        auto it = store_.find(n);
        return it == store_.end() ? 0.0 : it->second;
    }
    const std::map<std::string, double>& store() const { return store_; }
    long bad_calls() const { return bad_calls_; }

private:
    WriteStats st_;
    std::map<std::string, double> store_;
    long bad_calls_ = 0;
};

// =====================================================================
// 流水线：分级节拍 → 死区 → 批量写
// =====================================================================
class PublishPipeline {
public:
    explicit PublishPipeline(IValueWriter* w = nullptr) : w_(w) {}
    void set_writer(IValueWriter* w) { w_ = w; }

    DeadbandPublisher& deadband()  { return db_; }
    TieredScheduler&   scheduler() { return sch_; }
    IValueWriter*      writer()    { return w_; }

    // 采集侧：每拍把**全部**点的当前值喂进来（不管发不发）
    void set_value(const std::string& name, double v) { values_[name] = v; }
    double value_of(const std::string& name) const {
        auto it = values_.find(name);
        return it == values_.end() ? 0.0 : it->second;
    }
    const std::map<std::string, double>& values() const { return values_; }
    std::size_t point_count() const { return values_.size(); }

    struct TickResult {
        long due        = 0;   // 本拍到期（过了分级节拍）
        long published  = 0;   // 本拍真正写出去的点数
        long suppressed = 0;   // 被死区抑制的点数
        long batch_calls = 0;  // 本拍的 API 调用次数（0 或 1）
    };

    TickResult tick(double dt) {
        TickResult r;
        const std::vector<std::string> due = sch_.tick(dt);
        r.due = static_cast<long>(due.size());
        std::vector<std::string> names;
        std::vector<double>      vals;
        std::vector<int>         qs;
        names.reserve(due.size());
        vals.reserve(due.size());
        qs.reserve(due.size());
        for (const auto& n : due) {
            const double v = value_of(n);
            if (db_.should_publish(n, v)) {
                names.push_back(n);
                vals.push_back(v);
                qs.push_back(0);
            } else {
                ++r.suppressed;
            }
        }
        if (!names.empty() && w_ != nullptr) {
            // ★ 一次调用写 N 点 —— 这就是 C1
            if (w_->write_batch(names, vals, qs)) ++r.batch_calls;
        }
        r.published = static_cast<long>(names.size());
        tot_.ticks       += 1;
        tot_.due         += r.due;
        tot_.published   += r.published;
        tot_.suppressed  += r.suppressed;
        tot_.batch_calls += r.batch_calls;
        tot_.point_writes += r.published;
        return r;
    }

    struct Totals {
        long ticks        = 0;
        long due          = 0;
        long published    = 0;
        long suppressed   = 0;
        long batch_calls  = 0;
        long point_writes = 0;
    };
    const Totals& totals() const { return tot_; }
    void reset_totals() { tot_ = Totals(); }

    // -----------------------------------------------------------------
    // 全量重发对照：**每拍写全部点、每个点一次单点写**（= 修复前的 publish_all）
    //
    // 刻意用**另一个 writer** 跑：不然它会把优化路径的值覆盖掉，
    // 于是"逐点值等价"那条断言就永远测不出东西。
    // -----------------------------------------------------------------
    static long baseline_tick_with(IValueWriter& w,
                                   const std::map<std::string, double>& values) {
        for (const auto& kv : values) w.write_one(kv.first, kv.second, 0);
        return static_cast<long>(values.size());
    }

private:
    DeadbandPublisher db_;
    TieredScheduler   sch_;
    IValueWriter*     w_ = nullptr;
    std::map<std::string, double> values_;
    Totals tot_;
};

// =====================================================================
// 把模拟器的输出直接接到流水线的采集侧
// =====================================================================
class PipelineSink : public PointSink {
public:
    explicit PipelineSink(PublishPipeline& p) : p_(&p) {}
    void set(const std::string& name, double value, int quality = 0) override {
        (void)quality;
        p_->set_value(name, value);
    }
private:
    PublishPipeline* p_;
};

} // namespace devsim
} // namespace ems
