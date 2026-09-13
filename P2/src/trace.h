// =====================================================================
// P2/ — 产品化 P2 可观测性 · 跟踪等级控制
//
// ---------------------------------------------------------------------
// 为什么需要这个文件
// ---------------------------------------------------------------------
// P2 之前，"记多少日志"只有一个旋钮：`LoopConfig::log_every`。
// 这一个旋钮同时承担了两个互相冲突的职责：
//
//   · **保真**：现场要能回放故障，所以关键事件一条都不能丢
//   · **省资源**：86400 拍全记就是 8.6 MB/天，长跑受不了
//
// 结果就是周期 10 踩的那个坑：默认 `log_every=10` 时，一次只持续 1 拍的
// 关口倒送被完全掩盖（显示 0 违规），必须手工改成 1 才暴露。
// **为了省资源而调大 log_every，等于同时关掉了故障可见性。**
//
// ---------------------------------------------------------------------
// 本文件的设计：把"记多少"拆成三个独立的维度
// ---------------------------------------------------------------------
//   ① **关键事件（SOE）** —— 永远记，不受任何降采样影响。
//      状态迁移、故障、越限、恢复…… 这些是"事件"，不是"时序"，
//      频率天然很低（一次 838 拍的限幅只产生 2 条事件）。
//      由 SoeLog 负责，本文件不参与。
//
//   ② **跟踪等级（本文件）** —— 按子系统决定"要不要输出细节"。
//      现场默认 kInfo（只看事件）；排查某子系统时把它调到 kDebug，
//      其他子系统不受影响。运行期热更新，不用重启。
//
//   ③ **采样间隔（本文件）** —— 高频跟踪（每拍一条）按子系统采样。
//      例如只关心安全层每 10 拍一次，就 `set_sample_every(kSafety, 10)`。
//      与 log_every 的区别是：**按子系统独立**，且**不影响 SOE**。
//
// 另外，本文件会统计"被过滤掉的量"—— 可观测性组件自己也要可观测，
// 否则"什么都没输出"到底是"真的没事"还是"等级设错了"就分不清。
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "soe.h"

#include <array>
#include <sstream>
#include <string>

namespace ems {

inline constexpr int kSoeSourceCount = 9;

inline int soe_source_index(SoeSource s) {
    const int i = static_cast<int>(s);
    return (i >= 0 && i < kSoeSourceCount) ? i : 0;
}

// =====================================================================
// 跟踪配置
// =====================================================================
struct TraceConfig {
    // 全局等级：低于此等级的事件不输出
    SoeLevel global = SoeLevel::kInfo;

    // 按子系统覆盖（未设置的用 global）
    std::array<bool,     kSoeSourceCount> has_level{};
    std::array<SoeLevel, kSoeSourceCount> level{};

    // 高频跟踪采样：每 N 拍输出一次（0 或 1 表示不采样，即每拍）
    std::array<int, kSoeSourceCount> sample_every{};
};

// =====================================================================
// 跟踪控制器
// =====================================================================
class TraceControl {
public:
    TraceControl() {
        cfg_.sample_every.fill(0);
        cfg_.has_level.fill(false);
        tick_.fill(0);
        filtered_.fill(0);
        passed_.fill(0);
    }

    void reset() {
        cfg_ = TraceConfig();
        cfg_.sample_every.fill(0);
        cfg_.has_level.fill(false);
        tick_.fill(0);
        filtered_.fill(0);
        passed_.fill(0);
    }

    // ---- 等级 ----
    void set_global(SoeLevel lv) { cfg_.global = lv; }
    SoeLevel global() const { return cfg_.global; }

    void set_level(SoeSource s, SoeLevel lv) {
        const int i = soe_source_index(s);
        cfg_.has_level[i] = true;
        cfg_.level[i] = lv;
    }
    void clear_level(SoeSource s) { cfg_.has_level[soe_source_index(s)] = false; }

    SoeLevel level(SoeSource s) const {
        const int i = soe_source_index(s);
        return cfg_.has_level[i] ? cfg_.level[i] : cfg_.global;
    }

    // 该 (source, level) 是否应输出
    bool enabled(SoeSource s, SoeLevel lv) const {
        if (lv >= SoeLevel::kError) return true;   // 故障级别**永不受等级限制**
        return lv >= level(s);
    }

    // ---- 采样 ----
    void set_sample_every(SoeSource s, int n) {
        cfg_.sample_every[soe_source_index(s)] = (n < 0 ? 0 : n);
    }
    int sample_every(SoeSource s) const {
        return cfg_.sample_every[soe_source_index(s)];
    }

    // 高频跟踪的采样判定：每 N 拍返回一次 true。
    // N <= 1 表示不采样（每拍都 true）。
    bool sample(SoeSource s) {
        const int n = cfg_.sample_every[soe_source_index(s)];
        if (n <= 1) return true;
        const int i = soe_source_index(s);
        if (++tick_[i] >= n) { tick_[i] = 0; return true; }
        return false;
    }

    // ---- 统计（可观测性自己也要可观测）----
    void note_filtered(SoeSource s) { ++filtered_[soe_source_index(s)]; }
    void note_passed(SoeSource s)   { ++passed_[soe_source_index(s)]; }

    size_t filtered(SoeSource s) const { return filtered_[soe_source_index(s)]; }
    size_t passed(SoeSource s)   const { return passed_[soe_source_index(s)]; }
    size_t filtered_total() const {
        size_t n = 0;
        for (size_t i = 0; i < filtered_.size(); ++i) n += filtered_[i];
        return n;
    }
    size_t passed_total() const {
        size_t n = 0;
        for (size_t i = 0; i < passed_.size(); ++i) n += passed_[i];
        return n;
    }

    // 便捷：判定 + 统计 + 采样，一次完成。
    // 用法：if (trace.allow(SoeSource::kSafety, SoeLevel::kDebug)) { ... }
    bool allow(SoeSource s, SoeLevel lv) {
        if (!enabled(s, lv)) { note_filtered(s); return false; }
        // kDebug 级才走采样（事件级不受采样影响）
        if (lv == SoeLevel::kDebug && !sample(s)) { note_filtered(s); return false; }
        note_passed(s);
        return true;
    }

    // ---- 配置 ----
    void set_config(const TraceConfig& c) { cfg_ = c; }
    const TraceConfig& config() const { return cfg_; }

    std::string summary_text() const {
        std::ostringstream os;
        os << "TRACE: 全局 " << soe_level_name(cfg_.global)
           << " / 输出 " << passed_total() << " 条 / 过滤 " << filtered_total() << " 条\n";
        for (int i = 0; i < kSoeSourceCount; ++i) {
            if (!cfg_.has_level[i] && cfg_.sample_every[i] <= 0) continue;
            const SoeSource s = static_cast<SoeSource>(i);
            os << "  " << soe_source_name(s) << ": ";
            if (cfg_.has_level[i]) os << soe_level_name(cfg_.level[i]);
            else                   os << "(全局)";
            if (cfg_.sample_every[i] > 0) os << " 采样 1/" << cfg_.sample_every[i];
            os << "  输出 " << passed_[i] << " / 过滤 " << filtered_[i] << "\n";
        }
        return os.str();
    }

    std::string to_json() const {
        std::ostringstream os;
        os << "{\"global\": \"" << soe_level_name(cfg_.global) << "\""
           << ", \"per_source\": {";
        bool first = true;
        for (int i = 0; i < kSoeSourceCount; ++i) {
            if (!cfg_.has_level[i]) continue;
            if (!first) os << ", ";
            os << "\"" << soe_source_name(static_cast<SoeSource>(i)) << "\": \""
               << soe_level_name(cfg_.level[i]) << "\"";
            first = false;
        }
        os << "}, \"passed\": " << passed_total()
           << ", \"filtered\": " << filtered_total() << "}";
        return os.str();
    }

private:
    TraceConfig cfg_;
    std::array<int, kSoeSourceCount>    tick_{};
    std::array<size_t, kSoeSourceCount> filtered_{};
    std::array<size_t, kSoeSourceCount> passed_{};
};

} // namespace ems
