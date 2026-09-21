// =====================================================================
// EXT 外部设定的**读取与失效判定**（A3.1）
//
// 本文件解决的是"怎么读一组应当同时生效的量"，**不涉及**"读到的值怎么用"
// —— 后者是 A3.2（`narrow_interval_by_ext` 只是把已定的语义先钉下来，
// 见文件末尾）。
//
// ---------------------------------------------------------------------
// 为什么不能逐点读
// ---------------------------------------------------------------------
// `rt_db_get_value` 保证的是**单点**不撕裂。而"上界 / 下界 / 设定值 / 时标"
// 是一组应当同时生效的量：逐点读可能读到「**新的上界 + 旧的下界**」——
// 对安全区间来说，中间态可能比两端都**宽**，那正是最不能容忍的一瞬间。
//
// 所以用 `EXT.SEQ` 当区级序号（与写者的发布协议配对，见 P3/src/rtdb_ext_sink.h）：
//
//   s0 = 读 SEQ  →  读 6 个载荷 + TS  →  s1 = 读 SEQ
//   s0 == s1 **且 s0 为偶数**  →  这组值属于同一次发布，可用
//   否则  →  读的过程中区被改过（或正处于改写中），**重试**
//
// ★★ 为什么还要求 s0 是**偶数**（A3.1 修协议时补的）：
//   写者在载荷写入的**前后各递增一次**，所以"稳定态 = 偶数、改写中 = 奇数"。
//   只比相等是不够的：写者慢的时候，读者的 s0 与 s1 可能都落在**同一个奇数
//   窗口**里（s0 == s1 == 2k+1）—— 相等，但载荷正在被改，必须拒绝。
//   反过来，写者快的时候靠相等就够了（整个发布夹在两次读 SEQ 之间时，
//   序号会从 2k 变成 2k+2，不相等）。两条判据各挡一半，缺一不可。
//
// ★ 重试耗尽 ≠ "数据不可信"，而是"本拍没有一致快照" —— 处置是**本拍不施加
//   外部设定**（回到"无外部设定"），而不是沿用一半新一半旧的值。
//   这与 RT_DB 的 seqlock 语义同源：碰撞是"请重试"，不是"值坏了"。
//
// ---------------------------------------------------------------------
// ★★ 失效（fail-safe）不能只靠写者
// ---------------------------------------------------------------------
// 网关正常运行时会在"主站断开 / 空闲超时"时清空 EXT 区。但**网关进程自己
// 死掉**时没有任何人会去清 —— 那时最后一个设定会**永远留在段里继续生效**，
// 而调度已经连不上了。所以陈旧的兜底判据必须在这里：
//
//     now - EXT.TS > stale_s   →   整区视为无效（usable = false）
//
// 这条判据**不依赖任何进程活着**，是真正的最后一道。
//
// ★ 本文件**不引入 RT_DB 类型**：读取通过一个 `read(int index) -> double`
//   的回调注入。这样全部判据（序号重试、陈旧、NaN、上下界倒挂）都能用
//   几行假数据单测穷举，不需要起共享内存段。
// =====================================================================

#ifndef EMS_EXT_SETPOINTS_H
#define EMS_EXT_SETPOINTS_H

#include "ems_point_table.h"   // 07/src/rtdb  点表真相源

#include <cmath>
#include <cstddef>

namespace ems {

// ---------------------------------------------------------------------
// "无外部设定"的哨兵值 —— 与 EMS_POINT_DEFAULTS 的 EXT 段**必须一致**
//
// 为什么用哨兵而不是另加一个 `EXT.VALID` 布尔点：
//   ① 哨兵值同时是**默认值**，所以"从未收到过"与"收到过又清空"两条路径
//      落到同一个可判定的状态，不需要额外一点去表达；
//   ② 少一个点 = 少一处"忘记维护"的地方（A1 的教训：新点的默认值本身
//      就是一处容易取反的语义）。
//
// ★ 0 作为 P_SETPOINT 的"无设定"哨兵是否安全：0 kW 对储能不是有意义的
//   操作指令（"停机"另有 5001 遥控），所以 0 可以安全地兼职"没设定"。
//   这条假设有 T41 断言钉住。
// ---------------------------------------------------------------------
struct ExtSetpoints {
    // ---- 有效性（三个**独立**的事实，不要合成一个 bool）----
    bool present    = false;   // 段里收到过任何一次外部设定（EXT.SEQ > 0）
    bool stale      = false;   // 收到过，但 now - EXT.TS 超过 stale_s
    bool consistent = true;    // 拿到了一致的快照（区序号在读取前后相同）
    bool finite     = true;    // 全部数值有限（NaN/Inf 会污染区间求交）
    bool band_ok    = true;    // p_lower_set <= p_upper_set（倒挂 = 调度配错）

    // 综合判据：功率设定（6001~6004）**只有 usable 为真时才允许施加**。
    // ★ 例外：停机 / 闭锁（5001/5002）在 stale 时仍粘住（见 narrow_interval_by_ext），
    //   它只要求 present && consistent && finite，不要求新鲜。
    bool usable() const { return present && !stale && consistent && finite && band_ok; }

    // ---- 载荷 ----
    double p_setpoint  = 0.0;    // 0 = 无设定
    double p_upper_set = 1e9;    // 1e9 = 不限制（与 CFG.PCS_RAMP_KW_PER_S 同款约定）
    double p_lower_set = -1e9;   // -1e9 = 不限制
    double d_target    = 0.0;    // 0 = 不覆盖 CFG.D_TARGET
    bool   pcs_onoff   = true;
    bool   ems_enable  = true;

    // 中间量：这两个点在段里是 double（0/1），阈值转换后才进 bool 字段。
    // 单独留出来是为了让"读到的原始值"也能参与有限性检查 ——
    // 一个 NaN 转 bool 会变成 true，看起来完全正常。
    double pcs_onoff_raw  = 1.0;
    double ems_enable_raw = 1.0;

    // ---- 诊断 ----
    double    ts_s     = 0.0;    // 最后一次发布的墙钟秒
    long long seq      = 0;
    int       retries  = 0;      // 区序号重试次数（持续增长 = 调度在猛发命令）
    double    age_s    = 0.0;    // now - ts_s（<0 表示时标来自未来，见下）

    // 时标来自未来（now < ts_s）：只可能出现在**改过系统时间**或
    // 写者与自己不同机器的情况下。不当作"新鲜"，而是当作不可用 ——
    // 否则一个未来的时标会让"陈旧判定"**永远不成立**，等于把兜底关掉。
    bool from_future = false;
};

// ---------------------------------------------------------------------
// 按点索引读一个 double。返回 false = 该点当前不可读（品质坏 / 索引不存在）。
// 注入这个回调而不是直接吃 RtDbDeviceIO：本文件因此可以纯单测。
// ---------------------------------------------------------------------

// 稳定态判据：写者前后各递增一次，所以提交完成的 SEQ 必为偶数。
// 抽成具名函数是为了让"奇偶"这条规则**只写一处** —— 它同时被本文件与
// P3/rtdb_ext_sink.h 的协议引用，散在表达式里迟早会有一处被改掉。
inline bool is_committed_seq(double s0) {
    const long long n = static_cast<long long>(s0);
    return n > 0 && (n % 2 == 0);
}

template <class ReadFn>
ExtSetpoints load_ext_setpoints(ReadFn read, double now_s, double stale_s,
                                int max_retries = 8) {
    ExtSetpoints out;
    if (!(max_retries >= 1)) max_retries = 1;

    // ---- ① 区序号（一致快照）----
    bool got = false;
    bool seq_readable = false;   // 见下：present 的含义是"真的读到过 SEQ>0"
    for (int attempt = 0; attempt < max_retries; ++attempt) {
        double s0 = 0.0;
        if (!read(EMS_EXT_SEQ, s0)) break;   // SEQ 都读不到 → seq_readable 保持 false
        seq_readable = true;
        if (!(s0 > 0.0)) {            // SEQ == 0 → 从未收到过任何外部设定
            out.present = false;
            return out;               // 保持默认（= 无约束），usable() 为 false
        }

        if (!(read(EMS_EXT_P_SETPOINT,  out.p_setpoint)  &&
              read(EMS_EXT_P_UPPER_SET, out.p_upper_set) &&
              read(EMS_EXT_P_LOWER_SET, out.p_lower_set) &&
              read(EMS_EXT_D_TARGET,    out.d_target)    &&
              read(EMS_EXT_PCS_ONOFF,   out.pcs_onoff_raw) &&
              read(EMS_EXT_EMS_ENABLE,  out.ems_enable_raw) &&
              read(EMS_EXT_TS,          out.ts_s))) {
            break;                    // 有一个点读不到 → 本拍没有一致快照
        }

        double s1 = 0.0;
        if (!read(EMS_EXT_SEQ, s1)) break;
        // 一致 = 前后读到同一个值，**且**这个值落在稳定态（偶数）。
        // 只看相等会放过"整个读都落在同一个奇数窗口里"的半更新快照。
        if (s1 == s0 && is_committed_seq(s0)) {
            got = true;
            out.seq = static_cast<long long>(s0);
            break;
        }

        ++out.retries;
    }

    // ★ `present` 的字面含义是"段里收到过外部设定"，**不是**"本拍读成功"。
    //   所以只有真的读到过 SEQ>0 才能置 true。
    //   踩过的坑：原先在循环后面无条件写 `present = true` —— 于是"SEQ 这个点
    //   根本读不到"也会报 present=true，与它自己的注释互相矛盾，
    //   而且会让现场看到"收到过设定 / 快照不一致"这种指向完全错误的方向的提示。
    //   （usable() 两种情况下都是 false，所以**行为**没变 —— 这正是它难被发现的原因。）
    out.present    = seq_readable;
    out.consistent = got;
    if (!got) {
        // ★ 重试耗尽：**不要**返回"一半新一半旧"的载荷。
        //   统一退回哨兵值（无约束），并让 usable() 为 false。
        out.p_setpoint  = 0.0;
        out.p_upper_set = 1e9;
        out.p_lower_set = -1e9;
        out.d_target    = 0.0;
        out.pcs_onoff   = true;       // 无约束时 PCS 保持在线，不因读失败而停机
        out.ems_enable  = true;       // ★ 同上：读失败**不能**导致 EMS 被闭锁
        return out;
    }

    // ---- ② 数值合法性 ----
    out.pcs_onoff  = out.pcs_onoff_raw > 0.5;
    out.ems_enable = out.ems_enable_raw > 0.5;
    const double vals[] = {out.p_setpoint, out.p_upper_set, out.p_lower_set,
                           out.d_target, out.pcs_onoff_raw, out.ems_enable_raw,
                           out.ts_s};
    for (double v : vals) {
        if (!std::isfinite(v)) { out.finite = false; break; }
    }
    out.band_ok = out.p_lower_set <= out.p_upper_set;

    // ---- ③ 陈旧 ----
    out.age_s = now_s - out.ts_s;
    if (out.age_s < 0.0) out.from_future = true;
    // ★ stale_s <= 0 的含义定为"**禁用外部设定**"（视为总是陈旧），
    //   而不是"禁用超时判定"。理由：前者的失效方向是安全的，后者不是 ——
    //   一个配置成 0 的超时如果被解释成"永不超时"，兜底就没了。
    out.stale = out.from_future || !(stale_s > 0.0) || (out.age_s > stale_s);

    return out;
}

// =====================================================================
// A3.2 的入口：把外部设定折成对权限区间的**收窄**
//
// ★ A3.1 **不调用**本函数（本步只做通路）。放在这里是为了把已定的语义
//   连同它的取舍一起钉下来，避免 A3.2 时凭记忆重推一遍。
//   它**有测试覆盖**（T72），不是"写完没人用"的死代码。
//   A3.2（2026-09-20）起，本函数由 05/ SafetyEngine 作为第 10 条约束调用。
//
// 语义（2026-09-20 与用户确认）：**方向感知的单侧收紧**
//   正设定 S  →  只收紧上界：p_upper = min(p_upper, S)
//   负设定 S  →  只收紧下界：p_lower = max(p_lower, S)
//
// 为什么不是对称幅值带 [-|S|, +|S|]（字面上更贴"我希望在 100 kW 以内"）：
//   对称带会把**反方向**也锁死。光伏大发 / 谷时充电时，调度一个 +100 会把
//   充电压到 -100 → 可能制造 `interval_contradiction` → DERATED。
//   那等于**用安全层造出一个假降额**（本项目已把"区间矛盾 ≠ 紧急"列为纪律）。
//   反方向该由 6004（P_LOWER_SET）显式表达，而不是被 6001 顺手带走。
//
// 为什么不是"直接写 p_desired"：
//   P_desired 是**期望**，不构成安全约束；写成 desired 等于调度的设定
//   可以绕过 L0（SOC 上下限 / BMS 限值 / 变压器容量），违反设计方案
//   §十一「安全优先级最高」。
//
// ★ 三个"只收紧"的来源合起来用 min/max，所以**结构上不可能放宽**：
//   即便调度发来一个比现有区间更宽的设定，min/max 也会把它吃掉。
// =====================================================================
inline void narrow_interval_by_ext(const ExtSetpoints& ext, double* p_lower,
                                   double* p_upper) {
    // ★ 停机 / 闭锁（5001 / 5002）：**stale 也粘住**（2026-09-20 拍板）。
    //   "调度最后说停"是状态意图，通信断了不该自动复机 —— 只要拿到过一致的
    //   快照、数值合法，"旧"不是"坏"。
    //   守卫刻意**不含 band_ok**（那是功率设定带的倒挂，与停机这条独立命令
    //   无关）；但**必须含 finite** —— 一个 NaN 的 raw 值会经 `> 0.5` 判成
    //   false，被误当成"停机"，必须挡住。
    const bool can_hold_stop = ext.present && ext.consistent && ext.finite;
    if (can_hold_stop && (!ext.ems_enable || !ext.pcs_onoff)) {
        *p_lower = 0.0;
        *p_upper = 0.0;
        return;
    }

    // 功率设定（6001~6004）：只有 usable()（含新鲜）才施加 ——
    // 无效 / 陈旧 / 快照不一致 / 数值非法 → 不施加任何约束
    if (!ext.usable()) return;

    if (ext.p_setpoint > 0.0) {
        if (ext.p_setpoint < *p_upper) *p_upper = ext.p_setpoint;
    } else if (ext.p_setpoint < 0.0) {
        if (ext.p_setpoint > *p_lower) *p_lower = ext.p_setpoint;
    }
    // 6003 / 6004：显式收紧。方向固定 —— 上界设定只可能压上界。
    if (ext.p_upper_set < *p_upper) *p_upper = ext.p_upper_set;
    if (ext.p_lower_set > *p_lower) *p_lower = ext.p_lower_set;
}

} // namespace ems

#endif // EMS_EXT_SETPOINTS_H
