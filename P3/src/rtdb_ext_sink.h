// =====================================================================
// 外部设定落点的 RT_DB 实现（A3.1）
//
// ---------------------------------------------------------------------
// 角色边界：这是**第三个写入者**
// ---------------------------------------------------------------------
// 本项目在 RT_DB 上有三个写入者，点区互不重叠：
//
//   RtDbPointWriter   设备侧   → MEAS / STA / CFG
//   RtDbDeviceIO      EMS 侧   → CMD.*
//   RtDbExtWriter     网关     → EXT.*         ← 本类
//
// ★ 这条边界**不靠注释**：本类的每个写入都过 `ems_is_ext_point()`，
//   并且 `self_check()` 可以逐点核对"我要写的索引是不是真的在 EXT 区"。
//   写错区 = self_check() 返回非 0 = 测试判红。
//
// ---------------------------------------------------------------------
// 发布协议（为什么需要 EXT.SEQ 与 EXT.TS）
// ---------------------------------------------------------------------
// `rt_db_set_value` 每次只保证**单点**不撕裂，而"六个设定 + 时标"是一组
// 应当同时生效的量。读者若逐点读，可能读到"新的上界 + 旧的下界"的中间态 ——
// 对安全区间来说，中间态可能比两端都宽。
//
// 所以用 SEQ 当**区级序号**（seqlock 的思想，粒度是整区）：
//
//   写者：SEQ = ++n（**开始**，此刻起读者拿不到一致快照）
//         写 6 个载荷
//         写 TS
//         SEQ = ++n（**提交**，这一组值齐了）
//   读者：s0 = 读 SEQ → 读载荷与 TS → s1 = 读 SEQ
//         s0 == s1 **且 s0 为偶数** → 这组值属于同一次发布
//
// ★★ 为什么**开始**那一次递增不能省（这是 A3.1 写测试时发现的真缺陷）：
//   只在末尾递增的话，下面这条时序会让读者接受一个**半更新的快照** ——
//       读者读 s0 = 7（写者还没开始）
//       写者开始写 6 个载荷（一次只写一个点，中间态是"新旧混合"）
//       读者在写者提交**之前**读 s1 = 7        ← 等于 s0
//       → 判定"一致"，但载荷是混合的。
//   也就是说：末次递增只挡住了"读完之后才变"，挡不住"读的过程中在变"。
//   （单点 setter 只有一次载荷写入，看不出问题；`clear_all()` 一次写 6 个点，
//     那里才是真正会撕裂的地方 —— 所以这条纪律现在就要立。）
//
// ★ 往返递增后**稳定态的 SEQ 必为偶数**，写入进行中为奇数。
//   读者因此还要看奇偶：如果写者很慢，读者的 s0 与 s1 可能都落在同一个
//   **奇数**窗口里（s0 == s1 == 2k+1）—— 相等但正在改，必须拒绝。
//
// ★ SEQ 必须**只增不减**。本类在构造时读一次段里的当前 SEQ 并接着往上数 ——
//   否则网关重启后 SEQ 从 1 重新开始，读者会看到"序号倒退"，而"倒退"与
//   "没变化"在"要不要重新处理"这个判据上不可区分。
//
// ---------------------------------------------------------------------
// 为什么本类**不**在析构时清空外部设定
// ---------------------------------------------------------------------
// 网关正常退出时清空是合理的，但"网关进程被 kill"才是更需要考虑的情形 ——
// 那时析构根本不会跑。所以陈旧处置**不能只靠网关**：
//   网关侧：主站断开 / 空闲超时 → clear_all()（快路径）
//   EMS 侧：`now - EXT.TS > stale_s` → 整区视为无效（**不依赖网关活着**）
// 后一条才是兜底，前一条只是让状态早点变对。
// =====================================================================

#ifndef EMS_P3_RTDB_EXT_SINK_H
#define EMS_P3_RTDB_EXT_SINK_H

// ★ 必须排在所有 C++ 标准库头之前（rt_db_structs.h 的函数式原子宏会破坏
//   libstdc++ 里 std::atomic_* 的**声明**）。理由见 rtdb_quality.h 顶部。
#include "rtdb_quality.h"

#include "ext_setpoint_sink.h"

#include "rt_db_api.h"          // 07/vendor/rt_db
#include "ems_point_table.h"    // 07/src/rtdb

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace iec104 {

// 墙钟秒（Unix epoch）。
// ★ 抽成有名字的纯函数：本项目在 P3 网关心跳上吃过一次"毫秒当微秒除"的亏
//   （1000 秒恒 T+0s）。散落在表达式里的换算无法断言，抽出来才测得到。
inline double wall_clock_s() {
    using clock = std::chrono::system_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

class RtDbExtWriter : public IExtSetpointSink {
public:
    // 借用装配层持有的连接（一个进程一条连接，适配器**借用** handle —— 与
    // RtDbReadOnlySource / RtDbDeviceIO 同一约定）
    explicit RtDbExtWriter(rt_db_handle_t* borrowed) : h_(borrowed) { seed_seq(); }

    RtDbExtWriter(const RtDbExtWriter&) = delete;
    RtDbExtWriter& operator=(const RtDbExtWriter&) = delete;

    void attach(rt_db_handle_t* borrowed) { h_ = borrowed; seed_seq(); }

    bool is_open() const { return h_ != nullptr && h_->shm_addr != nullptr; }

    // -----------------------------------------------------------------
    // 自检：本类要写的每个索引必须落在 EXT 区。
    // 返回**违规点数**（0 = 通过）。与 RtDbDeviceIO::self_check() 同款约定：
    // 返回计数而不是 bool，现场能报出"坏了几个、是哪几个"。
    // -----------------------------------------------------------------
    // 逐点核对"点表里的索引 i 与我按名字期望的点是同一个"。
    // 为什么按名字反查：索引是运行时坐标，点表在中间插入时索引会整体偏移，
    // 而本类里的常量是编译期固定的 —— 只有名字能发现这件事。
    int self_check() const {
        int bad = 0;
        for (std::size_t k = 0; k < kExtCount; ++k) {
            const int idx = kExtIndex[k];
            // ① 分区：我写的点必须在 EXT 区
            if (!ems_is_ext_point(idx)) { ++bad; continue; }
            if (h_ == nullptr || h_->shm_addr == nullptr) continue;  // 未连接时只查分区
            // ② 身份：段里按**名字**找到的下标，必须就是我以为的那个下标。
            //    索引是运行时坐标，只有名字能发现"点表中间插了点"。
            const std::size_t found = rt_db_find_index_by_id(h_, EMS_POINT_NAMES[idx]);
            if (found != static_cast<std::size_t>(idx)) ++bad;
        }
        return bad;
    }

    // -----------------------------------------------------------------
    // 六个具名落点
    // -----------------------------------------------------------------
    bool set_p_setpoint(double kw,      std::string* err) override {
        return put(EMS_EXT_P_SETPOINT, kw, "EXT.P_SETPOINT", err);
    }
    bool set_upper(double kw,           std::string* err) override {
        return put(EMS_EXT_P_UPPER_SET, kw, "EXT.P_UPPER_SET", err);
    }
    bool set_lower(double kw,           std::string* err) override {
        return put(EMS_EXT_P_LOWER_SET, kw, "EXT.P_LOWER_SET", err);
    }
    bool set_d_target(double kw,        std::string* err) override {
        return put(EMS_EXT_D_TARGET, kw, "EXT.D_TARGET", err);
    }
    bool set_pcs_onoff(bool on,         std::string* err) override {
        return put(EMS_EXT_PCS_ONOFF, on ? 1.0 : 0.0, "EXT.PCS_ONOFF", err);
    }
    bool set_ems_enable(bool enabled,   std::string* err) override {
        return put(EMS_EXT_EMS_ENABLE, enabled ? 1.0 : 0.0, "EXT.EMS_ENABLE", err);
    }

    // -----------------------------------------------------------------
    // 清空：把六个设定写回**点表的默认值**（= "不施加约束"的哨兵）
    //
    // ★ 直接读 EMS_POINT_DEFAULTS，不另抄一份常量 ——
    //   "一处定义 + 两个消费者"。另抄一份的话，将来改了默认值
    //   会让"清空"回到一个不是"无约束"的值上，而这种错在运行期完全静默
    //   （值看起来总是个正常数字）。T40 有一条断言钉住默认值本身的性质。
    // -----------------------------------------------------------------
    bool clear_all(std::string* err) override {
        if (!is_open()) return fail(err, "RT_DB 未连接");

        const int idx[kExtCount] = {EMS_EXT_P_SETPOINT, EMS_EXT_P_UPPER_SET,
                                    EMS_EXT_P_LOWER_SET, EMS_EXT_D_TARGET,
                                    EMS_EXT_PCS_ONOFF, EMS_EXT_EMS_ENABLE};
        // ★ 这一组是**多点数**（六个），正是协议里"开始必须递增"要保护的对象：
        //   少了 begin_publish()，读者可能在六个点写一半时读到 s0 == s1 == 旧值，
        //   于是接受一个"半个新半个旧"的区间。
        begin_publish();
        for (int i = 0; i < kExtCount; ++i) {
            if (!write_raw(idx[i], EMS_POINT_DEFAULTS[idx[i]])) {
                commit_publish();      // 尽力提交，别把读者永远留在奇数窗口里
                return fail(err, std::string("清空写失败：") + EMS_POINT_NAMES[idx[i]]);
            }
        }
        commit_publish();
        ++clears_;
        return true;
    }

    // ---- 诊断 ----
    // seq() 返回**最后一次提交**的序号（恒为偶数，见文件头的发布协议）。
    // ★ 语义变了（A3.1 修协议时）：每次发布递增**两次**，所以
    //   "发布了几次" = seq() / 2，不再是 seq() 本身。报告里别直接当次数用。
    long long seq()     const { return seq_; }
    int  writes()       const { return writes_; }
    int  clears()       const { return clears_; }
    int  rejects()      const { return rejects_; }   // 被分区守卫挡下的写入次数

private:
    static constexpr int kExtCount = 6;
    // 自检用的索引表（与 clear_all 的 idx 表同源；两处都从常量算，不抄数字）
    static constexpr int kExtIndex[kExtCount] = {
        EMS_EXT_P_SETPOINT, EMS_EXT_P_UPPER_SET, EMS_EXT_P_LOWER_SET,
        EMS_EXT_D_TARGET, EMS_EXT_PCS_ONOFF, EMS_EXT_EMS_ENABLE};

    // 写一个载荷点，并提交一次发布
    bool put(int index, double v, const char* name, std::string* err) {
        if (!is_open()) return fail(err, "RT_DB 未连接");
        begin_publish();
        if (!write_raw(index, v)) {
            commit_publish();          // 同上：失败也要离开奇数窗口
            return fail(err, std::string("写失败：") + name);
        }
        commit_publish();
        return true;
    }

    // ★ 运行期分区守卫。
    //   编到这里的索引都是具名方法给的常量，理论上必在 EXT 区；
    //   但加这一道很便宜，而且能把"将来有人改错了常量"变成
    //   **返回 false + 计数**，而不是悄悄写脏 CMD.* 或 MEAS.*。
    bool write_raw(int index, double v) {
        if (!ems_is_ext_point(index)) { ++rejects_; return false; }
        if (h_ == nullptr) { ++rejects_; return false; }
        if (!rt_db_set_value(h_, static_cast<std::size_t>(index), v, rtdb_q::kGood)) {
            ++rejects_;
            return false;
        }
        ++writes_;
        return true;
    }

    // -----------------------------------------------------------------
    // 发布：**前后各递增一次**（见文件头的发布协议）
    //   begin  →  SEQ 变奇数 = "本区正在被改写，读者请重试"
    //   commit →  SEQ 变偶数 = "这一组值齐了"
    // ★ 顺序不能反、也不能省掉 begin —— 省掉 begin 会让"读者在载荷写入
    //   期间读到旧 SEQ"被误判成一致快照（A3.1 实测过这条时序）。
    // -----------------------------------------------------------------
    void begin_publish() {
        rt_db_set_value(h_, static_cast<std::size_t>(EMS_EXT_SEQ),
                        static_cast<double>(++seq_), rtdb_q::kGood);
    }
    void commit_publish() {
        rt_db_set_value(h_, static_cast<std::size_t>(EMS_EXT_TS),
                        wall_clock_s(), rtdb_q::kGood);
        rt_db_set_value(h_, static_cast<std::size_t>(EMS_EXT_SEQ),
                        static_cast<double>(++seq_), rtdb_q::kGood);
    }

    // 网关重启后接着数，别让 SEQ 倒退（见文件头）。
    // ★ 段里的值应当是偶数（提交态）。若读到奇数，说明上一个写者死在半途 ——
    //   接着往上数即可（下一次 ++ 就回到偶数），**不要**把它归零或向下取整：
    //   序号倒退与"没变化"不可区分，那正是本机制要避免的。
    void seed_seq() {
        seq_ = 0;
        if (h_ == nullptr || h_->shm_addr == nullptr) return;
        double cur = 0.0;
        long q = 0;
        struct timespec t{};
        if (rt_db_get_value(h_, static_cast<std::size_t>(EMS_EXT_SEQ), &cur, &q, &t)) {
            if (cur > 0.0) seq_ = static_cast<long long>(cur);
        }
    }

    static bool fail(std::string* err, const std::string& why) {
        if (err) *err = why;
        return false;
    }

    rt_db_handle_t* h_ = nullptr;
    long long seq_    = 0;
    int writes_       = 0;
    int clears_       = 0;
    int rejects_      = 0;
};

} // namespace iec104

#endif // EMS_P3_RTDB_EXT_SINK_H
