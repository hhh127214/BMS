// =====================================================================
// 17/ — 批量写的 **RT_DB 真实调用路径**（C1）
//
// 缺口原文（§10.3 C1）：
//   > `rt_db_set_multiple_values()` 在 `07/vendor/rt_db/rt_db_api.h:56`，
//   > 本项目中无人调用（只有上游 `test/test_rt_db.c:135` 用过）
//   > → 几千点 = 几千次 seqlock 往返
//
// 本文件把那个"存在但零调用"的 API 接上，并给出**可被断言的调用次数**。
//
// ★ 与 publish_pipeline.h 的 CountingWriter 的分工：
//   · CountingWriter 证明"我们的流水线一次调用发 N 点"（逻辑侧，可注入）
//   · RtDbBatchWriter 证明"这个调用真的落到 rt_db_set_multiple_values 上"
//     （集成侧，需要真实共享内存段）
//   两者**都要**有：只有前者是"自己和自己对答案"，只有后者则在没有段的环境里测不了。
//   见 tests/test_scale_rtdb.cpp：有段就跑真集成，没段就显式 SKIP（不是 FAIL）。
//
// ★ 一个必须诚实说出来的事实（见 docs/README.md 已知边界）：
//   vendor 的 `rt_db_set_multiple_values()` 内部是 `for(i) rt_db_set_value(...)`
//   —— 它把 **API 调用次数** 从 N 降到 1，但**内部仍然是 N 次 seqlock 往返**，
//   `header.write_count` 照样 +N。所以本模块量化的是"调用次数"，不是"内存往返次数"。
//   要真正降往返，得改 vendor（不在 17/ 的权限内）。
//
// 编译：需要 -I 到 07/vendor/rt_db，并链接 rt_db_api.c（gcc 编 C）。
// =====================================================================

#pragma once

#include "publish_pipeline.h"

#include "rt_db_api.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ems {
namespace devsim {

// =====================================================================
// 把 N 个点值一次性写进 RT_DB
//
// 返回 false 的两种情形**必须分开**对待（调用方要能区分）：
//   · 有点名解析不出来 → **整批拒绝**（一个都不写）。半批写入会让这一拍的
//     点集处于既非旧值也非新值的中间态，而下游无法察觉。
//   · 底层调用失败 → 同样返回 false。
// =====================================================================
inline bool publish_batch(rt_db_handle_t* h,
                          const std::vector<std::string>& names,
                          const std::vector<double>& values,
                          const std::vector<int>& qualities,
                          std::size_t* out_resolved_count = nullptr) {
    if (h == nullptr) return false;
    if (names.size() != values.size() || values.size() != qualities.size()) return false;
    if (names.empty()) return false;

    std::vector<std::size_t> idx(names.size());
    std::vector<long>        qs(names.size());
    std::size_t resolved = 0;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::size_t k = rt_db_find_index_by_id(h, names[i].c_str());
        if (k == static_cast<std::size_t>(-1)) {
            if (out_resolved_count) *out_resolved_count = resolved;
            return false;                       // 整批拒绝
        }
        idx[i] = k;
        qs[i]  = static_cast<long>(qualities[i]);
        ++resolved;
    }
    if (out_resolved_count) *out_resolved_count = resolved;
    return rt_db_set_multiple_values(h, idx.data(), idx.size(), values.data(), qs.data());
}

// =====================================================================
// IValueWriter 的 RT_DB 实现
// =====================================================================
class RtDbBatchWriter : public IValueWriter {
public:
    explicit RtDbBatchWriter(rt_db_handle_t* h = nullptr) : h_(h) {}

    bool ready() const { return h_ != nullptr; }
    void set_handle(rt_db_handle_t* h) { h_ = h; }

    // 句柄为空 → **干净地拒绝**（返回 false），不崩、不半写
    bool write_one(const std::string& name, double v, int q) override {
        if (h_ == nullptr) { ++rejected_; return false; }
        const std::size_t idx = rt_db_find_index_by_id(h_, name.c_str());
        if (idx == static_cast<std::size_t>(-1)) { ++rejected_; return false; }
        ++st_.calls;
        ++st_.point_writes;
        return rt_db_set_value(h_, idx, v, static_cast<long>(q));
    }

    bool write_batch(const std::vector<std::string>& names,
                     const std::vector<double>& vals,
                     const std::vector<int>& qs) override {
        if (h_ == nullptr)                    { ++rejected_; return false; }
        if (names.size() != vals.size() ||
            vals.size()   != qs.size())       { ++rejected_; return false; }
        if (names.empty())                    { ++rejected_; return false; }
        const bool ok = publish_batch(h_, names, vals, qs, nullptr);
        if (!ok) { ++rejected_; return false; }
        ++st_.calls;                                   // ← 一次调用
        st_.point_writes += static_cast<long>(names.size());
        return true;
    }

    const WriteStats& stats() const override { return st_; }
    void reset_stats() { st_ = WriteStats(); rejected_ = 0; }
    long rejected() const { return rejected_; }

private:
    rt_db_handle_t* h_ = nullptr;
    WriteStats      st_;
    long            rejected_ = 0;
};

} // namespace devsim
} // namespace ems
