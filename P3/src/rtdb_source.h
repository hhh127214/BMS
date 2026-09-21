// =====================================================================
// P3 / RT_DB 只读数据源
//
// 网关从这里取实时数据，再按 iec104_point_map.h 的映射转成 IEC104 ASDU。
//
// ---------------------------------------------------------------------
// ★ 角色边界：网关是**第五个角色**，只读
// ---------------------------------------------------------------------
// RT_DB 共享内存段上现在有四个写者/读者，各自的区是固定的：
//
//   角色          写                读
//   ----------    --------------    --------------------------
//   设备进程      MEAS/STA/CFG      CMD
//   EMS 进程      CMD               MEAS/STA/CFG
//   初始化器      （建段 + 注册点表）
//   本网关        **不写任何东西**   MEAS/STA/CFG/CMD（全段只读）
//
// 为什么网关不能写：调度的遥控/遥调如果直接写 CMD.*，就绕过了 EMS 的安全约束层
// （05/ SafetyEngine），等于让远端能直接驱动 PCS —— 这是安全设计上不能接受的。
// 命令必须经 ICommandSink 交给装配层，由 EMS 决定接不接受。
//
// 为了让这条纪律**在类型上可见**：本类只有 read()，没有 write()。
// 不要为了省事给它加写方法；需要写就说明设计错了。
//
// ---------------------------------------------------------------------
// 为什么读失败要单独计数，而不是返回 0
// ---------------------------------------------------------------------
// 「段没建起来」与「值是 0」在数值上完全一样，但在现场是两种事故：
// 前者说明初始化器没跑，后者说明设备真的没出力。
// 所以 read() 返回 false 时上送的品质位会被置 INVALID —— 调度一眼能看出是坏点。
//
// 三种结局各不相同，别合并：
//   值 + GOOD  正常
//   值 + NT    值是真的，但陈旧 / 源头自己标了 UNCERTAIN —— 只显示不参与决策
//   无值 + IV  读不到 —— 主站必须是"未知"，不是"0"
// =====================================================================

#ifndef EMS_P3_RTDB_SOURCE_H
#define EMS_P3_RTDB_SOURCE_H

// ★ 必须排在第一位：rtdb_quality.h 要在任何 C++ 标准库头之前 include
//   （原因见 rtdb_quality.h 顶部：rt_db_structs.h 的 C11 原子宏会破坏
//    libstdc++ 里 std::atomic_* 的声明）
#include "rtdb_quality.h"       // RT_DB 的 data_quality_t + 编译期对账

#include "rt_db_api.h"          // 07/vendor/rt_db
#include "iec104_server.h"      // IDataSource
#include "iec104_point_map.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>

namespace iec104 {


class RtDbReadOnlySource : public IDataSource {
public:
    RtDbReadOnlySource() = default;
    explicit RtDbReadOnlySource(rt_db_handle_t* borrowed) : h_(borrowed) { resolve(); }
    ~RtDbReadOnlySource() override { close_owned(); }

    RtDbReadOnlySource(const RtDbReadOnlySource&) = delete;
    RtDbReadOnlySource& operator=(const RtDbReadOnlySource&) = delete;

    // 借用装配层持有的连接（一个进程一条连接，生命周期由装配层管）
    void attach(rt_db_handle_t* borrowed) { h_ = borrowed; resolve(); }

    // 自持连接：独立网关进程用这个。返回 false 表示共享内存段不存在
    // —— 现场含义：rtdb_initializer 没跑（Windows 上段是页面文件映射对象，
    //    最后一个句柄关闭即销毁，所以初始化器必须常驻保活）。
    bool open_owned(const char* config_path = nullptr) {
        close_owned();
        std::memset(&owned_, 0, sizeof(owned_));
        if (!rt_db_init(&owned_, config_path)) return false;
        owned_open_ = true;
        h_ = &owned_;
        resolve();
        return true;
    }

    void close_owned() {
        if (owned_open_) {
            rt_db_cleanup(&owned_);
            owned_open_ = false;
        }
        h_ = nullptr;
    }

    bool is_open() const { return h_ != nullptr && h_->shm_addr != nullptr; }

    // -----------------------------------------------------------------
    // ★ 借出连接句柄，给**同样借用**约定的适配器用（A3.1 起：
    //   RtDbExtWriter 需要同一个句柄来写 EXT 区）。
    //
    // 为什么是"借出"而不是"自己再开一条"：RT_DB 的纪律是**一个进程一条连接**，
    // 适配器一律**借用**装配层的 handle。本类自己开了连接（open_owned）时，
    // 这个句柄的所有权仍在本类手里 —— 借用方**不得**对它调 rt_db_cleanup。
    //
    // ★ 注意命名：本访问器返回的是"可写句柄"，但本类**没有**写方法。
    //   "谁来写"由**类型**决定（IExtSetpointSink 只有六个具名 setter），
    //   不由"拿到了句柄"决定 —— 拿到句柄本身不构成写权限。
    // -----------------------------------------------------------------
    rt_db_handle_t* borrowed_handle() { return h_; }
    const rt_db_handle_t* borrowed_handle() const { return h_; }

    // =================================================================
    // 解析点名 → 索引（一次性，运行时不做字符串比较）
    // =================================================================
    void resolve() {
        index_.clear();
        if (!is_open()) return;
        std::size_t n = 0;
        const PointDef* t = upload_table(&n);
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t idx = rt_db_find_index_by_id(h_, t[i].src);
            if (idx != static_cast<std::size_t>(-1)) index_[t[i].src] = idx;
        }
    }

    // 上送表里有多少个点在段里找不到（0 = 全部对得上）
    int missing_points() const {
        std::size_t n = 0;
        const PointDef* t = upload_table(&n);
        int miss = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (index_.find(t[i].src) == index_.end()) ++miss;
        return miss;
    }

    // =================================================================
    // 数据陈旧判据（可选，默认关闭）
    //
    // 现场为什么要它：BMS/PCS 通信断了以后，设备进程**不再写点**，
    // 于是段里的值永远是最后一次的好值 —— 品质位还是 GOOD，
    // 值看着也合理。调度拿到的是"几分钟前的负荷"，而且**看不出来**。
    // 所以要额外比一次时间戳的年龄，超限就置 NT（而非 IV）：
    // 值本身仍然是真的，只是不新鲜。
    //
    // 取 0 = 关闭（自环测试用；没有设备在写点，开了会全部变 NT）。
    // 现场建议取 3 ~ 5 倍设备进程的写周期。
    // =================================================================
    void set_stale_limit_ms(long long ms) { stale_limit_ms_ = ms; }
    long long stale_limit_ms() const { return stale_limit_ms_; }

    // =================================================================
    // IDataSource 实现（**只读**）
    // =================================================================
    bool read(const char* point, double* out, Quality* q = nullptr) const override {
        auto set_q = [q](Quality v) { if (q != nullptr) *q = v; };

        if (out == nullptr) { set_q(Quality::kInvalid); return false; }

        auto it = index_.find(point);
        if (it == index_.end()) {              // 点名在段里不存在
            ++misses_;
            set_q(Quality::kInvalid);
            return false;
        }

        long quality = rtdb_q::kBad;
        struct timespec ts{};
        bool got = false;

        // seqlock 碰撞的语义是「请重试」，不是「数据不可信」。
        // 只读一次会把偶发碰撞当成坏点 → 触发一次假的「好 → 坏」品质跳变上报。
        // 11/ 联调里实测 600 拍撞 51 次，全是假告警（见 07/ .../rtdb_device_io.h）。
        for (int attempt = 0; attempt < kReadRetries; ++attempt) {
            if (rt_db_get_value(h_, it->second, out, &quality, &ts)) { got = true; break; }
            // 写者临界区里含 update_timestamp()（Windows 下一次系统调用），
            // 可能比"连读几次"还长，纯忙等会次次都撞上 —— 必须让出 CPU。
            if (attempt + 1 < kReadRetries) std::this_thread::yield();
        }

        if (!got) {                            // 重试用尽 → 真的读不到
            ++misses_;
            set_q(Quality::kInvalid);
            return false;
        }

        // ① 先看源头标的品质
        Quality v = translate_quality(quality);

        // ② 再看数据新不新鲜（只可能把 GOOD 降级成 NT，不会把坏点救回来）
        if (v == Quality::kGood && stale_limit_ms_ > 0) {
            const long long age = now_ms_real() - ts_to_ms(ts);
            if (age > stale_limit_ms_) { ++stale_; v = Quality::kNotTopical; }
        }

        set_q(v);
        return true;
    }

    int misses() const { return misses_; }        // 点名找不到 / 重试后仍读不到
    int stale()  const { return stale_; }         // 因超龄被判 NT 的次数

private:
    // 品质常量已上移到文件头 rtdb_q 命名空间（那里有 static_assert 对账）

    // seqlock 重试上限，与 07/ 保持一致
    static constexpr int kReadRetries = 64;

    static Quality translate_quality(long q) {
        switch (q) {
        case rtdb_q::kGood:      return Quality::kGood;
        case rtdb_q::kUncertain: return Quality::kNotTopical;   // 值可用，但别据以决策
        case rtdb_q::kBad:       return Quality::kInvalid;
        default:
            // 未定义的编码。为什么判 NT 而不是 IV：
            // 值确实读到了，丢成 IV 会让主站失去这个量；判 NT 则既保住值、
            // 又明确告诉调度"别信"。宁可保守，不要信息丢失。
            return Quality::kNotTopical;
        }
    }

    static long long ts_to_ms(const struct timespec& t) {
        return static_cast<long long>(t.tv_sec) * 1000LL
             + static_cast<long long>(t.tv_nsec) / 1000000LL;
    }

    // RT_DB 的 update_timestamp() 用的是 CLOCK_REALTIME 口径
    // （Windows 下 GetSystemTimeAsFileTime），所以这里也用 system_clock 对齐。
    static long long now_ms_real() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    rt_db_handle_t  owned_{};
    bool            owned_open_ = false;
    rt_db_handle_t* h_ = nullptr;
    std::map<std::string, std::size_t> index_;
    long long       stale_limit_ms_ = 0;              // 0 = 关闭
    mutable int     misses_ = 0;   // read() 是 const，统计计数用 mutable
    mutable int     stale_  = 0;
};

// =====================================================================
// 自检：现场上电第一件事
//
// 查两件事，都是「不查就会静默出错」的：
//   1. 点表映射表自身没问题（IOA 唯一、点名对得上、区间正确）
//   2. 段里的点名能全部解析到（少一个点 = 有一个上送量永远是坏点）
// =====================================================================
inline int gateway_self_check(const RtDbReadOnlySource& src, std::string* report) {
    std::string out;
    char line[512];

    std::string pm;
    const int map_bad = self_check(&pm);
    if (map_bad > 0) out += "[点表映射表]\n" + pm;

    int rtdb_bad = 0;
    if (!src.is_open()) {
        rtdb_bad = 1;
        out += "  [RT_DB] 共享内存段未连接（先跑 rtdb_initializer）\n";
    } else {
        rtdb_bad = src.missing_points();
        if (rtdb_bad > 0) {
            std::size_t n = 0;
            const PointDef* t = upload_table(&n);
            for (std::size_t i = 0; i < n; ++i) {
                // 复读一次，找出具体是哪几个
                double v = 0.0; Quality q = Quality::kInvalid;
                if (!src.read(t[i].src, &v, &q)) {
                    std::snprintf(line, sizeof(line),
                                  "  [IOA %d] %s: 点名 '%s' 在 RT_DB 段里找不到\n",
                                  t[i].ioa, t[i].name, t[i].src);
                    out += line;
                }
            }
        }
    }

    if (report) *report = out;
    return map_bad + rtdb_bad;
}

} // namespace iec104

#endif // EMS_P3_RTDB_SOURCE_H
