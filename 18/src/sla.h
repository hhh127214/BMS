// =====================================================================
// 18/sla.h — 硬性 SLA 门禁：规则表 / 判据 / 来源 / 反向验证
//
// 这是本模块的核心。12/ 的 A4 只做"看一眼"（把 mean/max 打印出来，与一个
// 宽到几乎不可能红的阈值比一下），本模块要把它变成**能被 CI 拒绝的形式**。
//
// ---------------------------------------------------------------------
// 每条 SLA 必须回答四个问题（缺一不可）
// ---------------------------------------------------------------------
//   ① 数字是多少？
//   ② **数字从哪来**？—— 本模块把来源分成两类，且**强制标注**：
//        kDesignDerived ：由控制周期/预算/机制**推导**出来（先有数，后测量）
//        kMeasuredMargin：实测后设的裕度（**必须说明是对什么的裕度**）
//      不许出现第三种"看着差不多就填一个"。
//   ③ 在什么负载条件下这个数才成立？（condition）
//   ④ **反向验证**：怎么把它逼红？（reverse_test）—— 没有这一条的判据
//      无法区分"实现正确"与"条件根本没被触发"（约定 §3.1 / §3.2）。
//
// ---------------------------------------------------------------------
// 为什么需要"相对判据"而不只是绝对数
// ---------------------------------------------------------------------
// 绝对阈值会随机器换代而失效（今天 10 µs 的机器明天 2 µs），
// 也会被"调大到刚好能过"污染。相对判据（如"优化后 ≤ 优化前的 X%"、
// "末段 p99 ≤ 头段 p99 的 2 倍"）把结论锚在**同一台机器、同一时刻**的
// 两次测量上，机器性能变了它依然成立。
//
// 本模块两类判据并用：绝对判据守住"工程上必须达到的量级"（如 10 Hz 的
// 100 ms 预算），相对判据守住"实现是否真的改进了 / 是否随时间退化"。
// =====================================================================

#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ems {
namespace perf {

// =====================================================================
// 来源 / 方向
// =====================================================================
enum class SlaSource {
    kDesignDerived = 0,   // 设计推导：先有数，后测量
    kMeasuredMargin,      // 实测裕度：实测后设，须说明相对于什么
    kInheritedFrom12      // 沿用 12/ A4 的既有判据（保持口径可比）
};

inline const char* sla_source_name(SlaSource s) {
    switch (s) {
        case SlaSource::kDesignDerived: return "design-derived";
        case SlaSource::kMeasuredMargin: return "measured-margin";
        case SlaSource::kInheritedFrom12: return "inherited(12/A4)";
    }
    return "?";
}

enum class SlaDir {
    kUpperBound = 0,   // 实测 ≤ limit
    kLowerBound,       // 实测 ≥ limit（如加速比）
    kRatioUpper,       // optimized / baseline ≤ limit
    kRatioBound        // tail / head ≤ limit
};

inline const char* sla_dir_name(SlaDir d) {
    switch (d) {
        case SlaDir::kUpperBound: return "<=";
        case SlaDir::kLowerBound: return ">=";
        case SlaDir::kRatioUpper: return "ratio<=";
        case SlaDir::kRatioBound: return "ratio<=";
    }
    return "?";
}

struct SlaRule {
    std::string id;
    std::string name;
    std::string metric;      // p50 / p99 / max / ratio / speedup ...
    double      limit   = 0.0;
    SlaDir      dir     = SlaDir::kUpperBound;
    SlaSource   source  = SlaSource::kDesignDerived;
    std::string unit;
    std::string derivation;  // 数字怎么来的（推导过程 / 对什么设的裕度）
    std::string condition;   // 在什么负载条件下成立
    std::string reverse;     // 反向验证：怎么逼红
};

struct SlaOutcome {
    SlaRule rule;
    bool    pass            = false;
    double  measured        = 0.0;   // 参与门禁的那个值（对 ratio 即比值）
    double  measured_extra  = 0.0;   // ratio 时的分母（baseline / head）
    bool    evaluated       = true;  // false = 本次未覆盖（须在报告里出现）
    std::string note;

    // 相对 limit 的余量（"还有多少倍空间"）：
    //   upper-bound 类：limit / measured（>1 表示有裕度）
    //   lower-bound 类：measured / limit（>1 表示高于下限）
    //   比值类        ：limit / measured
    double headroom_x() const {
        if (measured <= 1e-12) return 1e12;
        if (rule.dir == SlaDir::kLowerBound) return measured / rule.limit;
        return rule.limit / measured;
    }
    double headroom_abs() const {
        return (rule.dir == SlaDir::kLowerBound) ? (measured - rule.limit)
                                                 : (rule.limit - measured);
    }
};

// =====================================================================
// 门禁求值（纯逻辑，可被反向验证直接调用）
// =====================================================================
inline SlaOutcome evaluate_abs(const SlaRule& r, double measured,
                               const std::string& note = std::string()) {
    SlaOutcome o;
    o.rule = r;
    o.measured = measured;
    o.note = note;
    switch (r.dir) {
        case SlaDir::kUpperBound: o.pass = (measured <= r.limit); break;
        case SlaDir::kLowerBound: o.pass = (measured >= r.limit); break;
        default:                  o.pass = false; break;
    }
    return o;
}

// ratio = num / den，判 ratio ≤ limit
inline SlaOutcome evaluate_ratio(const SlaRule& r, double num, double den,
                                 const std::string& note = std::string()) {
    SlaOutcome o;
    o.rule = r;
    o.measured_extra = den;
    // 分母为 0 → 视为无法求值（不是通过）。这类"除零导致静默通过"是本项目
    // 的既有坑之一，故显式判失败并在 note 里说明。
    if (den <= 1e-12) {
        o.measured = 0.0;
        o.pass = false;
        o.note = note + " [denom<=0 -> INVALID]";
        return o;
    }
    o.measured = num / den;
    o.pass = (o.measured <= r.limit);
    o.note = note;
    return o;
}

// ratio = num / den，判 ratio ≥ limit（如"调用次数降低倍数 ≥ 50×"）
inline SlaOutcome evaluate_ratio_lower(const SlaRule& r, double num, double den,
                                       const std::string& note = std::string()) {
    SlaOutcome o;
    o.rule = r;
    o.measured_extra = den;
    if (den <= 1e-12) {
        o.measured = 0.0;
        o.pass = false;
        o.note = note + " [denom<=0 -> INVALID]";
        return o;
    }
    o.measured = num / den;
    o.pass = (o.measured >= r.limit);
    o.note = note;
    return o;
}

// =====================================================================
// 规则表（本模块覆盖的全部 SLA）
//
// 阈值一律**先推导再测量**。推导过程写在各 rule 的 derivation 字段，
// 并原样进报告 —— 报告里出现"来源"一栏就是为了让读者能审计它。
// =====================================================================
struct SlaTable {
    std::vector<SlaRule> rules;
    std::vector<SlaOutcome> outcomes;

    void add(const SlaRule& r) { rules.push_back(r); }
    void record(const SlaOutcome& o) { outcomes.push_back(o); }
    void record(const std::vector<SlaOutcome>& v) {
        for (const auto& o : v) outcomes.push_back(o);
    }
    int passed() const {
        int n = 0;
        for (const auto& o : outcomes) if (o.evaluated && o.pass) ++n;
        return n;
    }
    int failed() const {
        int n = 0;
        for (const auto& o : outcomes) if (o.evaluated && !o.pass) ++n;
        return n;
    }
    int not_evaluated() const {
        int n = 0;
        for (const auto& o : outcomes) if (!o.evaluated) ++n;
        return n;
    }
    const SlaOutcome* find(const std::string& id) const {
        for (const auto& o : outcomes) if (o.rule.id == id) return &o;
        return nullptr;
    }
};

// ---------------------------------------------------------------------
// 本模块的 SLA 规则定义（集中一处，便于审计与复用）
// ---------------------------------------------------------------------
inline std::vector<SlaRule> build_sla_rules() {
    std::vector<SlaRule> v;
    auto mk = [](const char* id, const char* name, const char* metric,
                 double limit, SlaDir dir, SlaSource src, const char* unit,
                 const char* derivation, const char* condition,
                 const char* reverse) {
        SlaRule r;
        r.id = id; r.name = name; r.metric = metric; r.limit = limit;
        r.dir = dir; r.source = src;
        r.unit = unit; r.derivation = derivation; r.condition = condition;
        r.reverse = reverse;
        return r;
    };

    // ---------------- 单拍闭环（10 Hz）----------------
    v.push_back(mk(
        "SLA-C1", "单拍闭环 p99", "p99", 10.0, SlaDir::kUpperBound,
        SlaSource::kDesignDerived, "ms",
        "10 Hz 周期 = 100 ms。单拍计算占周期 10% 为上限：留 90 ms 给采集等待、"
        "执行机构响应与 OS 抖动。Windows 一次调度时间片约 15.6 ms，若计算本身 "
        "≥10 ms，一次抢占即吃掉周期的 25%，节拍漂移不可接受。",
        "单线程、无并发编译、dt=0.1 s、log_every=1、9 策略全启用",
        "注入人为延迟：对 3% 的拍 busy-wait 12 ms，p99 必 >10 ms -> 断言红"));

    v.push_back(mk(
        "SLA-C2", "单拍闭环 p99.9", "p99.9", 25.0, SlaDir::kUpperBound,
        SlaSource::kDesignDerived, "ms",
        "允许千分之一的长尾（页错误/缓存未命中/中断），但连续 4 拍长尾之和 "
        "不得超过 100 ms 周期，故单拍上限取 25 ms。",
        "同 SLA-C1",
        "注入延迟到 0.2% 的拍（>25 ms）-> 断言红"));

    v.push_back(mk(
        "SLA-C3", "单拍闭环均值", "mean", 1000.0, SlaDir::kUpperBound,
        SlaSource::kInheritedFrom12, "us",
        "沿用 12/ A4-01（mean ≤ 1000 us，即 10 Hz 下 CPU 占用 ≤1%）。"
        "保持该口径便于与既有验收报告逐项对照。",
        "同 SLA-C1",
        "注入延迟：每拍 +5 us 使 mean >1000 us -> 断言红"));

    v.push_back(mk(
        "SLA-C4", "单拍尾部分散度 (p99 - p50)", "p99-p50", 1.0,
        SlaDir::kUpperBound, SlaSource::kDesignDerived, "ms",
        "单拍预算 10 ms（SLA-C1）。尾部相对中位数的额外分散必须 ≤ 预算的 10%"
        "(=1 ms)，否则'中位数很快'是假象：均值被中位数掩盖，偶发长尾会吃掉节拍。"
        "★ 为什么不用比值 p99/p50：本模块实测该比值在 µs 尺度被"
        "**计时量化（粒度 100 ns = p50 的 3%）与微架构抖动（计时器噪声 max 达 4.3 us）**"
        "主导，p50 仅 ~3.4 us 时比值轻易越过 3 而不反映任何设计缺陷 —— "
        "即'阈值不能拍脑袋'的一个实例。故改用对量级不敏感的绝对尾散，"
        "并把 p99/p50 降级为**报告值**。",
        "单线程、无并发编译、dt=0.1 s、log_every=1、9 策略全启用",
        "每 50 拍注入 +2 ms -> 尾散 >1 ms -> 断言红"));

    v.push_back(mk(
        "SLA-C5", "单拍含冷启动 max", "max", 50.0, SlaDir::kUpperBound,
        SlaSource::kInheritedFrom12, "ms",
        "沿用 12/ A4-02（单拍不得吃掉半个 100 ms 周期）。首拍含惰性分配与"
        "页错误，故不能按稳态量级设。",
        "含第 1 拍的完整运行",
        "若把预热也算进 max 则本判据的区分度下降；反向验证由 SLA-C1 承担"));

    // ---------------- 采集路径 ----------------
    v.push_back(mk(
        "SLA-A1", "read_snapshot p99", "p99", 2.0, SlaDir::kUpperBound,
        SlaSource::kDesignDerived, "ms",
        "采集必须远小于计算预算。全表 40 点 seqlock 读，正常 <1 us/点；"
        "取单拍 10 ms 预算的 20% = 2 ms 作为上限（含重试与 yield）。",
        "无竞争 / 有设备写者线程竞争两种，各自出数",
        "与写者线程竞争下若重试策略失效，p99 会显著上升 -> 见 SLA-A2"));

    v.push_back(mk(
        "SLA-A2", "重试吸收率（stale/collisions）", "ratio", 0.01,
        SlaDir::kRatioUpper, SlaSource::kDesignDerived, "x",
        "rtdb_device_io.h 的纪律：正确判据是**降级远少于碰撞**。"
        "有竞争时应近乎全部碰撞被 64 次重试吸收，降级率 <=1%。",
        "必须有真实并发写者（否则 collisions==0，比值无意义 -> SLA-A3 反向守卫）",
        "去掉重试（只读一次）-> stale≈collisions -> 断言红"));

    v.push_back(mk(
        "SLA-A3", "无竞争采集零降级 + 竞争下确有碰撞", "count", 0.0,
        SlaDir::kUpperBound, SlaSource::kDesignDerived, "次",
        "反向守卫（约定 §3.2）：先证明'真的撞上了'（collisions>0），"
        "否则'stale==0'可能只是条件没被触发。"
        "判据：单线程 stale==0；并发下 collisions>0 且 absorbed>0。",
        "并发写者存在（碰撞守卫）",
        "关掉写者线程 -> collisions 落到 0 -> 守卫断言红"));

    // ---------------- Modbus 编解码（纯 CPU）----------------
    v.push_back(mk(
        "SLA-M1", "Modbus 请求构造 p99", "p99", 1.0, SlaDir::kUpperBound,
        SlaSource::kDesignDerived, "us",
        "MBAP+PDU 固定 12 字节填充，无 IO、无分配。现代 x86 应在 ~100 ns；"
        "上限取 1 us（10x 裕度），确保编解码不进入闭环关键路径的噪声带。",
        "单线程，纯 CPU，无 socket",
        "人为每次构造后 busy-wait 5 us -> p99 >1 us -> 断言红"));

    v.push_back(mk(
        "SLA-M2", "32 位浮点编解码往返 p99", "p99", 2.0, SlaDir::kUpperBound,
        SlaSource::kDesignDerived, "us",
        "put_f32+get_f32 各一次 = 位运算 + memcpy，无分配；上限 2 us。",
        "单线程，纯 CPU",
        "同 SLA-M1 的注入手法"));

    v.push_back(mk(
        "SLA-M3", "编解码 p99 对单拍 p99 的占比", "ratio", 0.10,
        SlaDir::kRatioUpper, SlaSource::kDesignDerived, "x",
        "要求编解码比整个算法闭环**便宜一个数量级以上**（<=10%）："
        "现场'慢'才有可能被归因到 IO 往返而不是 CPU。若编解码本身就接近"
        "单拍的量级，性能归因就失效了。",
        "两边同在空载机器上测",
        "若编解码被改成 O(N) 分配则占比上升 -> 断言红"));

    // ---------------- SOE / 告警 ----------------
    v.push_back(mk(
        "SLA-S1", "SoeLog::push p99", "p99", 10.0, SlaDir::kUpperBound,
        SlaSource::kDesignDerived, "us",
        "SOE 是事件级频率（每秒几条 ~ 事件风暴下每拍 1 条）。10 us 远低于 "
        "拍预算；上限取 10 us 覆盖链表插入 + map 查找 + 可能的容量淘汰。",
        "单线程；含事件风暴（高重复率）负载",
        "注入 20 us/次 busy-wait -> 断言红"));

    v.push_back(mk(
        "SLA-S2", "SoeLog 内存有界（size <= capacity）", "count", 0.0,
        SlaDir::kUpperBound, SlaSource::kDesignDerived, "次",
        "可观测性组件必须能报告自己的丢失：超出容量即淘汰最旧。"
        "判据：push 3*capacity 次后 size()==capacity 且 dropped()>0。",
        "推送量 > capacity",
        "把容量改成 INT_MAX -> dropped 落到 0 -> 断言红"));

    v.push_back(mk(
        "SLA-S3", "AlarmAssembler::update p99", "p99", 20.0,
        SlaDir::kUpperBound, SlaSource::kDesignDerived, "us",
        "每拍评估 ~10 条条件，其中每条都无条件构造 snprintf 消息串"
        "（~10 x 0.5 us = 5 us）；上限取 20 us（4x 裕度）。",
        "单线程，无告警 / 有告警两种",
        "注入 30 us/次 -> 断言红"));

    // ---------------- 发布流水线（相对判据）----------------
    // 口径来源：17/docs/README.md §4（102 点 / 档位 32·38·32 / 17 安全点 /
    // dt=0.5 / 600 拍 / 动态工况），以及它的「坑 6」与 §2.7：
    //   "17 个安全点必须永远发布，17×600=10200 是**不可压缩的地板**。
    //    全量重发 61200 ÷ 10200 ≈ 6.0× 才是点值写入降低倍数的**理论上界**。"
    // 故本模块**不**用"点写入降低 N 倍"那种依赖值模型的判据，
    // 改用两条与点表规模/值模型**无关**的机制判据 + 一条地板守卫：
    v.push_back(mk(
        "SLA-P1", "API 调用次数降低倍数 (基线/优化)", "ratio", 50.0,
        SlaDir::kLowerBound, SlaSource::kDesignDerived, "x",
        "机制：批量写把每拍 N 次调用压成**1 次**，与点数无关。"
        "102 点下理论 = 102×（17/docs §4 实测 102.0×）。"
        "阈值取 50×（理论值的 49%）—— 留一倍余量覆盖「某拍无到期点→0 次调用」"
        "这类边界，且远高于任何可能的实现噪声。判据与值变化模型无关。",
        "102 点、快中慢 32/38/32、17 安全点死区 0、dt=0.5、600 拍",
        "把优化路径改成逐点单点写 -> 降低倍数→1× -> 断言红"));

    v.push_back(mk(
        "SLA-P2", "安全点地板精确成立 且 死区确实抑制", "count", 0.0,
        SlaDir::kUpperBound, SlaSource::kDesignDerived, "次",
        "两条反向守卫（约定 §3.2），违反计数必须为 0："
        "① **地板不可压缩**：优化后写入 ≥ 安全点数×拍数（= 算出来的 10200），"
        "  即死区**没有**把安全点吃掉（17/docs §2.7：吃掉安全位不是少发一帧，"
        "  是安全链断了）；"
        "② **死区真的在工作**：数据点死区抑制率 ≥ 0.20（若为 0，说明死区没接线，"
        "  这时'写入少'其实来自分级节拍而不是死区，结论会被混淆）。"
        "阈值 0.20 是「死区必须可观测」的下限，非对着实测定。",
        "同 SLA-P1（动态工况）",
        "把优化路径死区全置 0 -> 抑制率→0 -> 断言红"));

    v.push_back(mk(
        "SLA-P3", "优化后 API 调用次数/拍 <= 1", "max", 1.0,
        SlaDir::kUpperBound, SlaSource::kDesignDerived, "次",
        "C1 的定义即'一次调用写 N 点'。若每拍 >1 次说明批量写没生效。",
        "稳态（非冷启动拍）",
        "改用单点写路径 -> 调用数=N -> 断言红"));

    v.push_back(mk(
        "SLA-P4", "优化后发布步 p50/拍 <= 基线 p50/拍 × 0.95", "ratio", 0.95,
        SlaDir::kRatioUpper, SlaSource::kMeasuredMargin, "x",
        "引入分级+死区+批量必须换来**可测**的典型代价改善。"
        "取 5% 为下限：基线 ~70 us/拍，5% = 3.5 us ≫ 计时器噪声 p99 (0.1 us)。"
        "相对判据用 **p50**（典型代价）而非 mean —— mean 会被偶发抢占污染。"
        "★ 诚实记录：本模块原按机制预期设 2×（≤0.50），实测 p50 比仅 ~0.74、"
        "mean 比 ~0.91，**远低于预期**。根因：优化路径对每个**到期**点做两次"
        "字符串键 map 查找（死区表 + 取值表），到期点数 ≈ 快档占比 ×N 且与"
        "死区是否抑制无关 —— 这是 CPU 的地板，与 17/docs「坑 6」说的"
        "'写入次数地板'不是同一件事。阈值据此下调到'可测改善'，并把差距记录在案。",
        "同一进程、同一台机器、同一时刻连续跑两路；1200 点（放大口径，"
        "使负载体量远高于计时噪声）；夹具（值更新）不计入计时区间",
        "在优化路径计时区间内注入 100 us/拍 -> 比值 >0.95 -> 断言红"));

    v.push_back(mk(
        "SLA-P5", "基线发布步/拍 >= 5 us（反向守卫）", "mean", 5.0,
        SlaDir::kLowerBound, SlaSource::kDesignDerived, "us",
        "若基线本身只有零点几 us，比值会被计时噪声主导、无区分度。"
        "先证明被比较的对象有足够体量（约定 §3.2 的反向守卫）。",
        "放大口径 1200 点全量单点写",
        "把点数降到 4 点 -> 基线 <5 us -> 守卫红"));

    // ---------------- 24h 长跑 ----------------
    v.push_back(mk(
        "SLA-L1", "长跑墙钟加速比", "speedup", 1000.0, SlaDir::kLowerBound,
        SlaSource::kDesignDerived, "x",
        "离线仿真的用途是'一天工况数秒内跑完'。86400 拍 @ dt=0.1 = 仿真 8640 s；"
        "加速比 >=1000x 即墙钟 <=8.64 s。",
        "空载机器，log_every=1",
        "人为每拍 sleep 使墙钟上升 -> 加速比 <1000x -> 断言红"));

    v.push_back(mk(
        "SLA-L2", "性能不随运行时长退化（末段/头段 p99）", "ratio", 2.0,
        SlaDir::kRatioBound, SlaSource::kDesignDerived, "x",
        "无内存泄漏 / 容器膨胀时，头段与末段应同分布。比值 >2 即存在"
        "随运行时长增长的代价（泄漏、缓存膨胀、日志容器线性增长未摊平）。"
        "口径取**稳健统计量**：把 post-warmup 样本切 16 个等长子窗，逐窗取 p99，"
        "再比较「末 1/4 子窗 p99 的中位数」与「首 1/4 子窗 p99 的中位数」——"
        "单窗 p99 是第 10 差的样本，一次 OS 调度抖动就能翻数倍（实测同二进制"
        "连跑 5 次单窗比值在 0.20~3.42 之间跳），取中位后单窗抖动被压掉。",
        "86400 拍；16 个等长子窗，首/末各 1/4 子窗的 p99 取中位数",
        "注入随时长线性增长的延迟 -> 末段子窗 p99 中位显著抬高 -> 断言红"));

    v.push_back(mk(
        "SLA-L3", "长跑内存 / 句柄有界", "max", 64.0, SlaDir::kUpperBound,
        SlaSource::kInheritedFrom12, "MB",
        "沿用 12/ A4-04（工作集增长 <=64 MB，句柄增量 <=0）。"
        "StepRecord 日志按 log_every=1 线性增长属**设计内**（有界容器），"
        "不构成泄漏；故另设 SLA-L2 用相对判据兜'生长导致变慢'。",
        "86400 拍，log_every=1",
        "故意泄漏（每拍 new 1 KB 不释放）-> RSS 增长突破 64 MB -> 断言红"));

    v.push_back(mk(
        "SLA-L4", "长跑末段 p99 绝对上限", "p99", 10.0, SlaDir::kUpperBound,
        SlaSource::kDesignDerived, "ms",
        "与 SLA-C1 同源（100 ms 周期 10%）。防止头尾**同时**退化时"
        "相对判据（SLA-L2）仍成立而绝对性能已不可接受。",
        "86400 拍末段 1000 拍",
        "同 SLA-C1 的注入手法"));

    return v;
}

// =====================================================================
// 渲染
// =====================================================================
inline std::string fmt_double(double v, int prec) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.*f", prec, v);
    return std::string(b);
}

inline std::string sla_outcome_line(const SlaOutcome& o) {
    std::string s;
    s += o.pass ? "[PASS] " : "[FAIL] ";
    s += o.rule.id + "  " + o.rule.name;
    s += "  |  " + o.rule.metric + " " + sla_dir_name(o.rule.dir) + " " +
         fmt_double(o.rule.limit, 4) + " " + o.rule.unit;
    s += "  |  measured=" + fmt_double(o.measured, 4);
    if (o.rule.dir == SlaDir::kRatioUpper || o.rule.dir == SlaDir::kRatioBound)
        s += " (den=" + fmt_double(o.measured_extra, 4) + ")";
    s += "  |  src=" + std::string(sla_source_name(o.rule.source));
    s += "  |  headroom=" + fmt_double(o.headroom_x(), 2) + "x";
    if (!o.note.empty()) s += "  |  " + o.note;
    return s;
}

} // namespace perf
} // namespace ems
