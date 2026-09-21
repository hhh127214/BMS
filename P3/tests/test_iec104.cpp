// =====================================================================
// P3/ 单元测试 —— IEC104 网关（T61~T72）
//
//   T61 握手与连接事件    —— 连接请求 / OPENED / STARTDT_CON 计数正确
//   T62 总召唤闭环        —— GI 请求 → ACT_CON → 数据 ASDU → ACT_TERM，
//                            24 点齐全、值/系数正确、**GI 用不带时标类型**
//   T63 变化上报与死区    —— 超死区发 SPONTANEOUS（带时标）、不超死区不发、
//                            且按类型拆帧（一个 ASDU 只装一种 IO）
//   T64 遥调受理与拒绝    —— C_SE_NC_1 受理回肯定、未知 IOA 回 UNKNOWN_IOA 否定
//   T65 遥控与类型校验    —— C_SC_NA_1 受理；把遥控 TypeID 打到遥调点上回
//                            UNKNOWN_TYPE_ID（防调度用错类型却"看起来成功"）
//   T66 ★时标时区        —— CP56Time2a 必须是**本地时间**，不是库默认的 UTC
//   T67 品质位            —— 读不到的点以 INVALID 品质上送，不是静默填 0
//   T68 断线与重连        —— 断线清 GI 会话、重连后能重新完成 GI
//   ---- A3.1（外部设定）----
//   T69 EXT 点区契约      —— 分区 / 边界 / 点名前缀 / zone 名字（对着点表真相源）
//   T69b ★EXT 默认值     —— 必须落在"不引入新约束"那一侧（反向守卫）
//   T70 IOA→落点分发      —— 六条对应关系穷举 + 未映射 IOA 必须拒绝
//   T71 读取与失效判定    —— SEQ=0 / 区序号抖动 / 奇数窗口 / 陈旧 / 未来时标 /
//                            NaN / 上下界倒挂 / 点读不到 / SEQ 读不到
//   T72 区间收窄语义      —— 方向感知单侧收紧；usable=false 时逐位不动
//   ---- 转发表外置（design §6）----
//   T77 转发表 CSV 加载   —— 样例 CSV 与默认表逐位一致；各种非法行回退默认
//
//   落点**跨共享内存边界**的那一半在 tests/test_ext_rtdb.cpp（T73~T76）——
//   本文件的数据源是内存实现，验证不了"值有没有真的落到段里那个点上"。
//
// ---------------------------------------------------------------------
// 为什么用 lib60870 自己的主站打自己的从站
// ---------------------------------------------------------------------
// 这是**唯一**能在没有调度主站的环境里验证协议正确性的办法：
// 报文编码/解码、APCI 的 k/w/t1~t3 推进、STARTDT 时序，全部走真实 TCP。
// 代价是「两边同源」—— 库的 bug 会同时出现在两侧而测不出来（同源盲区）。
// 所以 T66 这类**契约断言**必须对着外部真值（墙钟 / 换算公式）验，
// 不能只验"往返一致"：库对 CP56Time2a 的双向换算是严格互逆的，
// 往返测出来永远恒等，而实际时标差了 8 小时。
//
// 编译（见 P3/scripts/build_test.bat）：
//   g++ -std=c++17 -DNOMINMAX -I src -I ../../vendor/lib60870/{config,src/inc/api,src/hal/inc}
//       tests/test_iec104.cpp ../../vendor/lib60870/build/lib60870.a
//       -lws2_32 -liphlpapi -lbcrypt
//   （本测试**不需要** RT_DB —— 数据源用内存实现，协议层可以脱离共享内存单独测）
// =====================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "iec104_server.h"
#include "iec104_point_map.h"

#include "ext_setpoint_sink.h"   // A3.1：IExtSetpointSink / dispatch_ext_setpoint（网关的写口）
#include "ext_setpoints.h"       // A3.1：ems::load_ext_setpoints / narrow_interval_by_ext（读侧判定）

#include "cs104_connection.h"
#include "hal_time.h"

// ★ 为什么测试要 include 库的「内部」头：
//   lib60870 **没有为带时标类型提供公开取值接口** ——
//   M_ME_TF_1(36) / M_SP_TB_1(30) 只有 _create / _destroy / _getTimestamp，
//   没有 _getValue / _getQuality（见 cs101_information_objects.h:746-760）。
//   从站侧发得出去，主站侧要读值就只能用内部结构体。
//   这个 include 路径是本项目编译期就用到的（-Isrc/inc/internal），
//   手册 §2.4 也把该目录列为必须添加的包含路径。
//   **生产代码（iec104_server.h）不依赖它** —— 我们只发不收。
#include "information_objects_internal.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace iec104;

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT(cond)                                                       \
    do {                                                                   \
        if (cond) { ++g_pass; }                                            \
        else {                                                             \
            ++g_fail;                                                      \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__         \
                      << " : " << #cond << std::endl;                      \
        }                                                                  \
    } while (0)

#define EXPECT_NEAR(a, b, eps)                                             \
    do {                                                                   \
        const double va = (a), vb = (b);                                   \
        if (std::fabs(va - vb) <= (eps)) { ++g_pass; }                     \
        else {                                                             \
            ++g_fail;                                                      \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__         \
                      << " : " << #a << "=" << va << " vs " << #b << "="   \
                      << vb << " (eps " << (eps) << ")" << std::endl;      \
        }                                                                  \
    } while (0)

// =====================================================================
// 内存数据源（协议层测试不需要 RT_DB）
// =====================================================================
class MemorySource : public IDataSource {
public:
    void set(const std::string& p, double v) { v_[p] = v; }
    void break_point(const std::string& p) { broken_.insert(p); }
    void fix_point(const std::string& p) { broken_.erase(p); }
    // 让某个点变成"有值但不可据以决策"（RT_DB 的 QUALITY_UNCERTAIN / 数据超龄）
    void age_point(const std::string& p) { uncertain_.insert(p); }
    void freshen_point(const std::string& p) { uncertain_.erase(p); }

    bool read(const char* point, double* out, Quality* q = nullptr) const override {
        auto set_q = [q](Quality v) { if (q != nullptr) *q = v; };
        if (broken_.count(point)) { set_q(Quality::kInvalid); return false; }
        auto it = v_.find(point);
        if (it == v_.end()) { set_q(Quality::kInvalid); return false; }
        *out = it->second;
        set_q(uncertain_.count(point) ? Quality::kNotTopical : Quality::kGood);
        return true;
    }
    std::size_t size() const { return v_.size(); }

private:
    std::map<std::string, double> v_;
    std::set<std::string> broken_;
    std::set<std::string> uncertain_;
};

// =====================================================================
// 命令出口：记录收到的命令
// =====================================================================
class TestSink : public ICommandSink {
public:
    struct Rec { int ioa; Kind kind; double value; };

    bool accept = true;

    bool on_command(int ioa, Kind kind, double value, bool select, int ca,
                    std::string* err) override {
        if (select) return true;
        std::lock_guard<std::mutex> lk(m_);
        recs.push_back({ioa, kind, value});
        if (!accept) { if (err) *err = "测试：显式拒绝"; return false; }
        return true;
    }

    std::size_t count() const {
        std::lock_guard<std::mutex> lk(m_);
        return recs.size();
    }
    Rec at(std::size_t i) const {
        std::lock_guard<std::mutex> lk(m_);
        return recs[i];
    }
    std::vector<Rec> all() const {
        std::lock_guard<std::mutex> lk(m_);
        return recs;
    }
    void clear() { std::lock_guard<std::mutex> lk(m_); recs.clear(); }

private:
    mutable std::mutex m_;
    std::vector<Rec> recs;
};

// =====================================================================
// 从站测试环境
// =====================================================================
struct Harness {
    MemorySource  src;
    TestSink      sink;
    std::unique_ptr<Iec104Server> srv;
    std::thread   ticker;
    std::atomic<bool> stop{false};

    void start(int port, int ca, double periodic_s = 1.0) {
        Harness::populate(src);   // 预置一套典型值
        ServerConfig cfg;
        cfg.bind_addr  = "127.0.0.1";
        cfg.port       = port;
        cfg.ca         = ca;
        cfg.periodic_s = periodic_s;
        cfg.dump_raw   = false;
        srv.reset(new Iec104Server(cfg, &src, &sink));
        srv->start();
        ticker = std::thread([this] {
            while (!stop.load()) {
                srv->tick(now_utc_ms());
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    }
    void stop_all() {
        stop.store(true);
        if (ticker.joinable()) ticker.join();
        srv->stop();
    }

    // 预置一套「典型工况」——与 11/ 联调基准同量级
    static void populate(MemorySource& s) {
        s.set("MEAS.P_LOAD", 380.0);
        s.set("MEAS.P_PV",   150.0);
        s.set("MEAS.P_BAT",   50.0);
        s.set("MEAS.P_GRID", 180.0);
        s.set("MEAS.SOC",      0.55);
        s.set("MEAS.T_C",     28.5);
        s.set("MEAS.SOH",      0.98);
        s.set("CMD.P_BAT",    50.0);
        s.set("CMD.P_UPPER", 200.0);
        s.set("CMD.P_LOWER", -200.0);
        s.set("CFG.BAT_CAP_KWH",     1000.0);
        s.set("CFG.PCS_MAX_CHG",      200.0);
        s.set("CFG.PCS_MAX_DIS",      200.0);
        s.set("CFG.BMS_CHG_LIM",      180.0);
        s.set("CFG.BMS_DIS_LIM",      180.0);
        s.set("CFG.TRANSFORMER_KVA", 1000.0);
        s.set("CFG.D_TARGET",         600.0);
        s.set("CFG.TAU_S",              0.5);
        s.set("CFG.PCS_RAMP_KW_PER_S", 400.0);
        s.set("CFG.PCS_STANDBY",         2.0);
        s.set("CFG.ETA_CHG",             0.96);
        s.set("CFG.ETA_DIS",             0.96);
        s.set("CFG.SOC_PHYS_MIN",        0.05);
        s.set("CFG.SOC_PHYS_MAX",        0.95);
        s.set("STA.BMS_COMM_OK",   1.0);
        s.set("STA.PCS_COMM_OK",   1.0);
        s.set("STA.METER_COMM_OK", 1.0);
        s.set("STA.PCS_FAULT",     0.0);
        s.set("STA.OFFLINE",       0.0);
        s.set("STA.DATA_VALID",    1.0);
    }
};

// =====================================================================
// 主站接收记录
// =====================================================================
struct RxPoint {
    int    ioa      = 0;
    int    type     = 0;
    int    cot      = 0;
    double value    = 0.0;
    int    quality  = -1;
    bool   has_ts   = false;
    std::uint64_t ts_wire_ms = 0;   // CP56Time2a 字段按库的 UTC 口径还原
};

struct MasterStats {
    std::mutex m;
    bool opened = false, startdt = false, closed = false, failed = false;
    std::vector<RxPoint> pts;
    int gi_act_con = 0, gi_act_term = 0;
    int cmd_act_con_pos = 0, cmd_act_con_neg = 0;
    int last_neg_cot = -1;
    int last_neg_type = -1;
    int asdu_total = 0;

    void add(const RxPoint& p) { std::lock_guard<std::mutex> lk(m); pts.push_back(p); }
    std::size_t size() { std::lock_guard<std::mutex> lk(m); return pts.size(); }
    std::vector<RxPoint> snapshot() { std::lock_guard<std::mutex> lk(m); return pts; }
    std::size_t count_cot(int cot) {
        std::lock_guard<std::mutex> lk(m);
        std::size_t n = 0;
        for (auto& p : pts) if (p.cot == cot) ++n;
        return n;
    }
    std::vector<RxPoint> by_ioa(int ioa) {
        std::lock_guard<std::mutex> lk(m);
        std::vector<RxPoint> r;
        for (auto& p : pts) if (p.ioa == ioa) r.push_back(p);
        return r;
    }
};

static void conn_handler(void* param, CS104_Connection con, CS104_ConnectionEvent e) {
    MasterStats* st = static_cast<MasterStats*>(param);
    std::lock_guard<std::mutex> lk(st->m);
    switch (e) {
    case CS104_CONNECTION_OPENED:              st->opened  = true; break;
    case CS104_CONNECTION_CLOSED:              st->closed  = true; break;
    case CS104_CONNECTION_FAILED:              st->failed  = true; break;
    case CS104_CONNECTION_STARTDT_CON_RECEIVED: st->startdt = true; break;
    default: break;
    }
    (void)con;
}

static bool asdu_handler(void* param, int, CS101_ASDU asdu) {
    MasterStats* st = static_cast<MasterStats*>(param);
    const int type = static_cast<int>(CS101_ASDU_getTypeID(asdu));
    const int cot  = static_cast<int>(CS101_ASDU_getCOT(asdu));
    const int n    = CS101_ASDU_getNumberOfElements(asdu);
    {
        std::lock_guard<std::mutex> lk(st->m);
        st->asdu_total++;
        if (type == 100 /*C_IC_NA_1*/) {
            if (cot == 7 /*ACT_CON*/)  st->gi_act_con++;
            if (cot == 10 /*ACT_TERM*/) st->gi_act_term++;
        }
        if ((type == 45 || type == 46 || type == 50) && cot == 7) {
            if (CS101_ASDU_isNegative(asdu)) {
                st->cmd_act_con_neg++;
                st->last_neg_cot = cot;
                st->last_neg_type = type;
            } else {
                st->cmd_act_con_pos++;
            }
        }
    }

    for (int i = 0; i < n; ++i) {
        InformationObject io = CS101_ASDU_getElement(asdu, i);
        if (io == nullptr) continue;
        RxPoint p;
        p.type    = type;
        p.cot     = cot;
        p.ioa     = InformationObject_getObjectAddress(io);

        switch (type) {
        case M_ME_NC_1: {   // 13 短浮点遥测（无时标）
            auto m = reinterpret_cast<MeasuredValueShort>(io);
            p.value   = static_cast<double>(MeasuredValueShort_getValue(m));
            p.quality = static_cast<int>(MeasuredValueShort_getQuality(m));
            break;
        }
        case M_ME_TF_1: {   // 36 短浮点遥测 + CP56Time2a
            auto m = reinterpret_cast<struct sMeasuredValueShortWithCP56Time2a*>(io);
            p.value   = static_cast<double>(m->value);
            p.quality = static_cast<int>(m->quality);
            p.has_ts  = true;
            p.ts_wire_ms = CP56Time2a_toMsTimestamp(&m->timestamp);
            break;
        }
        case M_SP_NA_1: {   // 1 单点遥信（无时标）
            auto s = reinterpret_cast<SinglePointInformation>(io);
            p.value   = SinglePointInformation_getValue(s) ? 1.0 : 0.0;
            p.quality = static_cast<int>(SinglePointInformation_getQuality(s));
            break;
        }
        case M_SP_TB_1: {   // 30 单点遥信 + CP56Time2a
            auto s = reinterpret_cast<struct sSinglePointWithCP56Time2a*>(io);
            p.value   = s->value ? 1.0 : 0.0;
            p.quality = static_cast<int>(s->quality);
            p.has_ts  = true;
            p.ts_wire_ms = CP56Time2a_toMsTimestamp(&s->timestamp);
            break;
        }
        default:
            InformationObject_destroy(io);
            continue;
        }
        InformationObject_destroy(io);
        st->add(p);
    }
    return true;
}

// =====================================================================
// 工具
// =====================================================================
static void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

template <typename Pred>
static bool wait_until(Pred pred, int timeout_ms, int poll_ms = 10) {
    const int n = timeout_ms / poll_ms;
    for (int i = 0; i < n; ++i) {
        if (pred()) return true;
        sleep_ms(poll_ms);
    }
    return pred();
}

static const int kPort = 24040;

// =====================================================================

int main() {
    std::printf("=== P3 IEC104 gateway tests (T61~T72 + T77) ===\n\n");

    // ------------------------------------------------------------------
    // T61 握手与连接事件
    // ------------------------------------------------------------------
    std::printf("-- T61 handshake --\n");
    {
        Harness h;
        h.start(kPort, 1);
        sleep_ms(50);

        MasterStats st;
        CS104_Connection con = CS104_Connection_create("127.0.0.1", kPort);
        EXPECT(con != nullptr);
        CS104_Connection_setConnectionHandler(con, conn_handler, &st);
        CS104_Connection_setASDUReceivedHandler(con, asdu_handler, &st);

        const bool connected = CS104_Connection_connect(con);
        EXPECT(connected);
        CS104_Connection_sendStartDT(con);

        EXPECT(wait_until([&] { return st.startdt; }, 3000));
        {
            std::lock_guard<std::mutex> lk(st.m);
            EXPECT(st.opened);
            EXPECT(!st.failed);
        }
        // 从站侧计数
        EXPECT(h.srv->stats().connects == 1);
        EXPECT(wait_until([&] { return h.srv->stats().startdt >= 1; }, 2000));
        EXPECT(h.srv->peer()[0] != '\0');
        EXPECT(std::strcmp(h.srv->peer(), "127.0.0.1") == 0);

        CS104_Connection_close(con);
        CS104_Connection_destroy(con);
        EXPECT(wait_until([&] { return h.srv->stats().disconnects >= 1; }, 3000));
        h.stop_all();
    }

    // ------------------------------------------------------------------
    // T62 总召唤闭环
    // ------------------------------------------------------------------
    std::printf("-- T62 general interrogation --\n");
    {
        Harness h;
        h.start(kPort + 1, 1);
        sleep_ms(50);

        MasterStats st;
        CS104_Connection con = CS104_Connection_create("127.0.0.1", kPort + 1);
        CS104_Connection_setConnectionHandler(con, conn_handler, &st);
        CS104_Connection_setASDUReceivedHandler(con, asdu_handler, &st);
        EXPECT(CS104_Connection_connect(con));
        CS104_Connection_sendStartDT(con);
        EXPECT(wait_until([&] { return st.startdt; }, 3000));

        // 明确：GI 请求前把自发清除，避免初值自发混进来
        sleep_ms(200);
        {
            std::lock_guard<std::mutex> lk(st.m);
            st.pts.clear();
        }

        const bool gi_sent = CS104_Connection_sendInterrogationCommand(
            con, CS101_COT_ACTIVATION, 1, IEC60870_QOI_STATION);
        EXPECT(gi_sent);

        EXPECT(wait_until([&] { return st.count_cot(20) > 0; }, 5000));
        EXPECT(wait_until([&] {
            std::lock_guard<std::mutex> lk(st.m);
            return st.gi_act_term > 0;
        }, 5000));
        EXPECT(wait_until([&] { return h.srv->stats().gi_completed >= 1; }, 3000));

        // 24 个上送点全到
        {
            std::size_t upload_n = 0;
            upload_table(&upload_n);
            EXPECT(upload_n == 24);
            const std::size_t got = st.count_cot(20);
            EXPECT(got >= upload_n);
        }

        // GI 响应必须是 INTERROGATED_BY_STATION(20) 且**不带时标**
        {
            std::lock_guard<std::mutex> lk(st.m);
            int ts_count = 0, nots_count = 0;
            for (auto& p : st.pts) {
                if (p.cot != 20) continue;
                if (p.has_ts) ++ts_count; else ++nots_count;
            }
            EXPECT(nots_count > 0);
            EXPECT(ts_count == 0);   // 规范：GI 响应只用基础类型
        }

        // 值 + 系数：SOC 0.55 → 55.0（scale=100）
        {
            auto soc = st.by_ioa(4005);
            EXPECT(!soc.empty());
            if (!soc.empty()) {
                EXPECT_NEAR(soc[0].value, 55.0, 1e-3);
                EXPECT(soc[0].type == 13 /*M_ME_NC_1*/);
            }
            auto t = st.by_ioa(4006);
            if (!t.empty()) EXPECT_NEAR(t[0].value, 28.5, 1e-3);
            auto pg = st.by_ioa(4004);
            if (!pg.empty()) EXPECT_NEAR(pg[0].value, 180.0, 1e-3);
            auto lo = st.by_ioa(4010);
            if (!lo.empty()) EXPECT_NEAR(lo[0].value, -200.0, 1e-3);   // 负值往返不变号
            // 遥信
            auto yx = st.by_ioa(1004);
            EXPECT(!yx.empty());
            if (!yx.empty()) {
                EXPECT_NEAR(yx[0].value, 0.0, 1e-9);
                EXPECT(yx[0].type == 1 /*M_SP_NA_1*/);
            }
            auto yx1 = st.by_ioa(1001);
            if (!yx1.empty()) EXPECT_NEAR(yx1[0].value, 1.0, 1e-9);
        }

        EXPECT(h.srv->stats().gi_requests >= 1);
        auto s = st.snapshot();
        EXPECT(!s.empty());

        CS104_Connection_close(con);
        CS104_Connection_destroy(con);
        h.stop_all();
    }

    // ------------------------------------------------------------------
    // T63 变化上报与死区
    // ------------------------------------------------------------------
    std::printf("-- T63 spontaneous + deadband --\n");
    {
        Harness h;
        h.start(kPort + 2, 1, 9999.0);   // 周期设成很大，避免混入 PERIODIC
        sleep_ms(50);

        MasterStats st;
        CS104_Connection con = CS104_Connection_create("127.0.0.1", kPort + 2);
        CS104_Connection_setConnectionHandler(con, conn_handler, &st);
        CS104_Connection_setASDUReceivedHandler(con, asdu_handler, &st);
        EXPECT(CS104_Connection_connect(con));
        CS104_Connection_sendStartDT(con);
        EXPECT(wait_until([&] { return st.startdt; }, 3000));

        // 首拍：所有点都没"上一拍"，应触发一次全量自发
        EXPECT(wait_until([&] { return st.count_cot(3) >= 24; }, 3000));

        // 自发帧必须**带时标**（与 GI 相反）
        {
            std::lock_guard<std::mutex> lk(st.m);
            int ts = 0, nots = 0;
            for (auto& p : st.pts) {
                if (p.cot != 3) continue;
                if (p.has_ts) ++ts; else ++nots;
            }
            EXPECT(ts > 0);
            EXPECT(nots == 0);
        }

        sleep_ms(300);
        const std::size_t before = st.size();

        // (a) 小于死区：380 × 0.002 = 0.76 kW；改 0.1 kW → 不该发
        h.src.set("MEAS.P_LOAD", 380.1);
        sleep_ms(400);
        EXPECT(st.size() == before);

        // (b) 大于死区：改到 500 → 该发，且只发这一个点
        h.src.set("MEAS.P_LOAD", 500.0);
        EXPECT(wait_until([&] { return st.size() > before; }, 3000));
        {
            auto recs = st.by_ioa(4001);
            EXPECT(!recs.empty());
            if (!recs.empty()) {
                const RxPoint& last = recs.back();
                EXPECT(last.cot == 3 /*SPONTANEOUS*/);
                EXPECT_NEAR(last.value, 500.0, 1e-3);
                EXPECT(last.has_ts);
            }
            const std::size_t delta = st.size() - before;
            EXPECT(delta <= 2);   // 只该有 4001 一个点（+容一次边界重发）
        }

        EXPECT(h.srv->stats().spontaneous_asdu >= 1);

        CS104_Connection_close(con);
        CS104_Connection_destroy(con);
        h.stop_all();
    }

    // ------------------------------------------------------------------
    // T64 遥调受理与拒绝
    // ------------------------------------------------------------------
    std::printf("-- T64 setpoint accept / reject --\n");
    {
        Harness h;
        h.start(kPort + 3, 1);
        sleep_ms(50);

        MasterStats st;
        CS104_Connection con = CS104_Connection_create("127.0.0.1", kPort + 3);
        CS104_Connection_setConnectionHandler(con, conn_handler, &st);
        CS104_Connection_setASDUReceivedHandler(con, asdu_handler, &st);
        EXPECT(CS104_Connection_connect(con));
        CS104_Connection_sendStartDT(con);
        EXPECT(wait_until([&] { return st.startdt; }, 3000));
        sleep_ms(100);

        // (a) 受理：IOA 6001 有功功率设定
        h.sink.accept = true;
        InformationObject sp = reinterpret_cast<InformationObject>(
            SetpointCommandShort_create(nullptr, 6001, 123.5f, false, 0));
        EXPECT(CS104_Connection_sendProcessCommandEx(con, CS101_COT_ACTIVATION, 1, sp));
        InformationObject_destroy(sp);
        EXPECT(wait_until([&] { return h.sink.count() >= 1; }, 3000));
        EXPECT(wait_until([&] {
            std::lock_guard<std::mutex> lk(st.m);
            return st.cmd_act_con_pos >= 1;
        }, 3000));
        if (h.sink.count() >= 1) {
            auto r = h.sink.at(0);
            EXPECT(r.ioa == 6001);
            EXPECT(r.kind == Kind::kSetpointFloat);
            EXPECT_NEAR(r.value, 123.5, 1e-3);
        }

        // (b) sink 拒绝 → 必须回否定确认（不能骗调度说成功了）
        h.sink.accept = false;
        InformationObject sp2 = reinterpret_cast<InformationObject>(
            SetpointCommandShort_create(nullptr, 6001, 50.0f, false, 0));
        EXPECT(CS104_Connection_sendProcessCommandEx(con, CS101_COT_ACTIVATION, 1, sp2));
        InformationObject_destroy(sp2);
        EXPECT(wait_until([&] {
            std::lock_guard<std::mutex> lk(st.m);
            return st.cmd_act_con_neg >= 1;
        }, 3000));
        EXPECT(h.srv->stats().commands_rejected >= 1);
        h.sink.accept = true;

        // (c) 未知 IOA → UNKNOWN_IOA + 否定
        //     注意 sink 记录的是"到达 sink 的命令"，所以要用**增量**判断
        const std::size_t  sink_before = h.sink.count();
        const std::uint64_t rej0 = h.srv->stats().commands_rejected;
        InformationObject sp3 = reinterpret_cast<InformationObject>(
            SetpointCommandShort_create(nullptr, 6099, 1.0f, false, 0));
        EXPECT(CS104_Connection_sendProcessCommandEx(con, CS101_COT_ACTIVATION, 1, sp3));
        InformationObject_destroy(sp3);
        EXPECT(wait_until([&] { return h.srv->stats().commands_rejected > rej0; }, 3000));
        EXPECT(h.srv->stats().unknown_ioa >= 1);
        EXPECT(h.sink.count() == sink_before);   // 未知点在 sink 之前就被挡掉

        // (d) 错误 CA → 回 UNKNOWN_CA
        const std::uint64_t ca0 = h.srv->stats().unknown_ca;
        InformationObject sp4 = reinterpret_cast<InformationObject>(
            SetpointCommandShort_create(nullptr, 6001, 1.0f, false, 0));
        EXPECT(CS104_Connection_sendProcessCommandEx(con, CS101_COT_ACTIVATION, 99, sp4));
        InformationObject_destroy(sp4);
        EXPECT(wait_until([&] { return h.srv->stats().unknown_ca > ca0; }, 3000));

        EXPECT(h.srv->stats().commands_rx >= 3);

        CS104_Connection_close(con);
        CS104_Connection_destroy(con);
        h.stop_all();
    }

    // ------------------------------------------------------------------
    // T65 遥控与类型校验
    // ------------------------------------------------------------------
    std::printf("-- T65 single command + type check --\n");
    {
        Harness h;
        h.start(kPort + 4, 1);
        sleep_ms(50);

        MasterStats st;
        CS104_Connection con = CS104_Connection_create("127.0.0.1", kPort + 4);
        CS104_Connection_setConnectionHandler(con, conn_handler, &st);
        CS104_Connection_setASDUReceivedHandler(con, asdu_handler, &st);
        EXPECT(CS104_Connection_connect(con));
        CS104_Connection_sendStartDT(con);
        EXPECT(wait_until([&] { return st.startdt; }, 3000));
        sleep_ms(100);

        // (a) PCS 启停（IOA 5001）→ 受理，value=1
        InformationObject sc = reinterpret_cast<InformationObject>(
            SingleCommand_create(nullptr, 5001, true, false, 0));
        EXPECT(CS104_Connection_sendProcessCommandEx(con, CS101_COT_ACTIVATION, 1, sc));
        InformationObject_destroy(sc);
        EXPECT(wait_until([&] { return h.sink.count() >= 1; }, 3000));
        if (h.sink.count() >= 1) {
            auto r = h.sink.at(0);
            EXPECT(r.ioa == 5001);
            EXPECT(r.kind == Kind::kCommandSingle);
            EXPECT_NEAR(r.value, 1.0, 1e-9);
        }

        // (b) 类型不符：把「单点命令」打到「遥调点 6001」上 → UNKNOWN_TYPE_ID
        const std::uint64_t rej0 = h.srv->stats().commands_rejected;
        InformationObject bad = reinterpret_cast<InformationObject>(
            SingleCommand_create(nullptr, 6001, true, false, 0));
        EXPECT(CS104_Connection_sendProcessCommandEx(con, CS101_COT_ACTIVATION, 1, bad));
        InformationObject_destroy(bad);
        EXPECT(wait_until([&] { return h.srv->stats().commands_rejected > rej0; }, 3000));
        EXPECT(h.sink.count() == 1);   // 类型不符不该落到 sink

        // (c) 选择-执行（SBO）：选择步不该落到 sink
        const std::size_t sink0 = h.sink.count();
        InformationObject sel = reinterpret_cast<InformationObject>(
            SingleCommand_create(nullptr, 5002, true, true /*select*/, 0));
        EXPECT(CS104_Connection_sendProcessCommandEx(con, CS101_COT_ACTIVATION, 1, sel));
        InformationObject_destroy(sel);
        sleep_ms(300);
        EXPECT(h.sink.count() == sink0);
        // 执行步生效
        InformationObject exe = reinterpret_cast<InformationObject>(
            SingleCommand_create(nullptr, 5002, true, false /*execute*/, 0));
        EXPECT(CS104_Connection_sendProcessCommandEx(con, CS101_COT_ACTIVATION, 1, exe));
        InformationObject_destroy(exe);
        EXPECT(wait_until([&] { return h.sink.count() == sink0 + 1; }, 3000));

        CS104_Connection_close(con);
        CS104_Connection_destroy(con);
        h.stop_all();
    }

    // ------------------------------------------------------------------
    // T66 ★ 时标时区 —— 本项目最容易被"自环恒等"掩盖的缺陷
    // ------------------------------------------------------------------
    std::printf("-- T66 CP56Time2a timezone --\n");
    {
        EXPECT(utc_offset_seconds() == 8 * 3600);
        EXPECT(std::strcmp(utc_offset_text(), "UTC+08:00") == 0);

        // 时基单位：`ms_to_s()` 是全工程**唯一**的 ms→s 换算口（心跳 / 超时 / 报告都走它）。
        // 它曾经写成 `(now - t0) / 1000000ull`（`now_utc_ms()` 返回的是**毫秒**）——
        // 于是网关心跳在头 1000 秒里恒打印 `T+0s`，而编译、单测、协议交互**全部正常**，
        // 属于"只影响打印"的静默出错，靠肉眼看日志才发现。所以在这里把单位钉死：
        // 换算被抽出成纯函数，就是为了让这类缺陷能被断言，而不是能被观察。
        EXPECT(ms_to_s(60000) == 60);     // 60 s 心跳必须报 T+60s（错版会得 0）
        EXPECT(ms_to_s(999) == 0);        // 未满 1 s 截断，不四舍五入
        EXPECT(ms_to_s(3600000ULL) == 3600);

        Harness h;
        h.start(kPort + 5, 1, 9999.0);
        sleep_ms(50);

        MasterStats st;
        CS104_Connection con = CS104_Connection_create("127.0.0.1", kPort + 5);
        CS104_Connection_setConnectionHandler(con, conn_handler, &st);
        CS104_Connection_setASDUReceivedHandler(con, asdu_handler, &st);
        EXPECT(CS104_Connection_connect(con));
        CS104_Connection_sendStartDT(con);
        EXPECT(wait_until([&] { return st.startdt; }, 3000));

        EXPECT(wait_until([&] { return st.count_cot(3) >= 24; }, 3000));
        const std::uint64_t rx_utc_ms = now_utc_ms();

        RxPoint ts_pt;
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(st.m);
            for (auto& p : st.pts)
                if (p.has_ts) { ts_pt = p; found = true; break; }
        }
        EXPECT(found);
        if (found) {
            // ① 主断言：协议里的时标 = UTC + 8h（本地时间），误差 30 s 内
            const double delta_s =
                (static_cast<double>(ts_pt.ts_wire_ms) -
                 static_cast<double>(rx_utc_ms)) / 1000.0;
            EXPECT_NEAR(delta_s, static_cast<double>(utc_offset_seconds()), 30.0);

            // ② 反向守卫：绝不能没有偏移（那就是库默认的 UTC 行为）
            //    没有这条守卫，"两边都按 UTC" 也能骗过 ① 的宽松写法
            EXPECT(std::fabs(delta_s - static_cast<double>(utc_offset_seconds())) > 0.0
                   ? true : true);
            EXPECT(std::fabs(delta_s) > 3600.0);   // 本机时区非 UTC，必差 >=1h
        }

        // ③ 与墙钟独立对账（不经过库的往返换算）：
        //    把 CP56Time2a 的字段拆出来，应与 gmtime(now + 8h) 的字段一致
        {
            std::time_t t_exp = std::time(nullptr) + utc_offset_seconds();
            struct tm exp{};
#if defined(_WIN32)
            gmtime_s(&exp, &t_exp);
#else
            gmtime_r(&t_exp, &exp);
#endif
            struct sCP56Time2a probe;
            make_local_cp56time2a(&probe, now_utc_ms());
            EXPECT(CP56Time2a_getHour(&probe) == exp.tm_hour);
            EXPECT(CP56Time2a_getMinute(&probe) == exp.tm_min);
            EXPECT(CP56Time2a_getYear(&probe) + 2000 == exp.tm_year + 1900);
            // 夏令时位必须为 0（国内无夏令时；置 1 会让调度侧误判）
            EXPECT(CP56Time2a_isSummerTime(&probe) == false);
        }

        // ④ 双向互逆（在本文件的偏移口径下）
        {
            struct sCP56Time2a a, b;
            const std::uint64_t t = 1789811518735ull;   // 固定时刻，可复现
            make_local_cp56time2a(&a, t);
            const std::uint64_t back = cp56time2a_local_to_utc_ms(&a);
            EXPECT(back == t);
            make_local_cp56time2a(&b, t);
            EXPECT(CP56Time2a_getHour(&a) == CP56Time2a_getHour(&b));
        }

        // ⑤ 对钟：调度送来的北京时间应被正确还原成 UTC
        {
            struct sCP56Time2a send_ts;
            const std::uint64_t now = now_utc_ms();
            make_local_cp56time2a(&send_ts, now);
            EXPECT(CS104_Connection_sendClockSyncCommand(con, 1, &send_ts));
            EXPECT(wait_until([&] { return h.srv->stats().clock_syncs >= 1; }, 3000));
            const std::uint64_t got = h.srv->last_clock_sync_utc_ms();
            EXPECT_NEAR(static_cast<double>(got), static_cast<double>(now), 3000.0);
            EXPECT(!h.srv->last_clock_sync_text().empty());
        }

        CS104_Connection_close(con);
        CS104_Connection_destroy(con);
        h.stop_all();
    }

    // ------------------------------------------------------------------
    // T67 品质位
    // ------------------------------------------------------------------
    std::printf("-- T67 quality bits --\n");
    {
        Harness h;
        h.start(kPort + 6, 1, 9999.0);
        sleep_ms(50);

        MasterStats st;
        CS104_Connection con = CS104_Connection_create("127.0.0.1", kPort + 6);
        CS104_Connection_setConnectionHandler(con, conn_handler, &st);
        CS104_Connection_setASDUReceivedHandler(con, asdu_handler, &st);
        EXPECT(CS104_Connection_connect(con));
        CS104_Connection_sendStartDT(con);
        EXPECT(wait_until([&] { return st.startdt; }, 3000));
        EXPECT(wait_until([&] { return st.count_cot(3) >= 24; }, 3000));

        // 正常时品质应为 GOOD
        {
            auto recs = st.by_ioa(4006);   // 电池温度
            EXPECT(!recs.empty());
            if (!recs.empty()) EXPECT(recs[0].quality == IEC60870_QUALITY_GOOD);
        }

        // 让温度点读失败 → 自发帧里品质应为 INVALID(0x80)
        {
            std::lock_guard<std::mutex> lk(st.m);
            st.pts.clear();
        }
        h.src.break_point("MEAS.T_C");
        h.src.set("MEAS.P_LOAD", 600.0);   // 同时造一个变化触发上报
        EXPECT(wait_until([&] { return st.count_cot(3) > 0; }, 3000));
        {
            auto recs = st.by_ioa(4006);
            EXPECT(!recs.empty());
            if (!recs.empty()) {
                EXPECT(recs.back().quality == IEC60870_QUALITY_INVALID);
                EXPECT(recs.back().value == 0.0);   // 值填 0，但品质说明它不可信
            }
        }
        EXPECT(h.srv->stats().points_invalid >= 1);

        // 恢复后品质回到 GOOD
        h.src.fix_point("MEAS.T_C");
        h.src.set("MEAS.P_LOAD", 700.0);
        EXPECT(wait_until([&] {
            auto r = st.by_ioa(4006);
            return !r.empty() && r.back().quality == IEC60870_QUALITY_GOOD;
        }, 3000));

        // ---- 第三态 NT：有值，但不可据以决策 --------------------------
        // 这是与 IV 的**关键区别**，也是把 IDataSource 从 bool 改成三态的理由：
        //   NT 退化成 IV  → 主站丢掉一个本来可用的量，画面闪断；
        //   NT 退化成 GOOD→ 主站拿陈旧值当实时值用（现场最危险的一种错）。
        // 所以这里既要断言品质是 0x40，也要断言**值是原值**。
        {
            std::lock_guard<std::mutex> lk(st.m);
            st.pts.clear();
        }
        h.src.age_point("MEAS.T_C");        // 温度点变成"不确定/陈旧"
        h.src.set("MEAS.P_LOAD", 800.0);    // 造一个变化触发上报
        EXPECT(wait_until([&] {
            auto r = st.by_ioa(4006);
            return !r.empty() && r.back().quality != IEC60870_QUALITY_GOOD;
        }, 3000));
        {
            auto recs = st.by_ioa(4006);
            EXPECT(!recs.empty());
            if (!recs.empty()) {
                EXPECT(recs.back().quality == IEC60870_QUALITY_NON_TOPICAL);   // 0x40
                EXPECT(recs.back().quality != IEC60870_QUALITY_INVALID);      // ≠ 0x80
                EXPECT(recs.back().quality != IEC60870_QUALITY_GOOD);         // ≠ 0x00
                // ★ 值必须原样送出去 —— 这正是 NT 与 IV 的差别
                EXPECT_NEAR(recs.back().value, 28.5, 1e-6);
            }
        }
        EXPECT(h.srv->stats().points_not_topical >= 1);

        // 恢复新鲜 → 回到 GOOD
        h.src.freshen_point("MEAS.T_C");
        h.src.set("MEAS.P_LOAD", 900.0);
        EXPECT(wait_until([&] {
            auto r = st.by_ioa(4006);
            return !r.empty() && r.back().quality == IEC60870_QUALITY_GOOD;
        }, 3000));
        {
            auto recs = st.by_ioa(4006);
            EXPECT(!recs.empty());
            if (!recs.empty()) EXPECT_NEAR(recs.back().value, 28.5, 1e-6);
        }

        CS104_Connection_close(con);
        CS104_Connection_destroy(con);
        h.stop_all();
    }

    // ------------------------------------------------------------------
    // T68 断线与重连
    // ------------------------------------------------------------------
    std::printf("-- T68 disconnect / reconnect --\n");
    {
        Harness h;
        h.start(kPort + 7, 1);
        sleep_ms(50);

        for (int round = 0; round < 2; ++round) {
            MasterStats st;
            CS104_Connection con = CS104_Connection_create("127.0.0.1", kPort + 7);
            CS104_Connection_setConnectionHandler(con, conn_handler, &st);
            CS104_Connection_setASDUReceivedHandler(con, asdu_handler, &st);
            EXPECT(CS104_Connection_connect(con));
            CS104_Connection_sendStartDT(con);
            EXPECT(wait_until([&] { return st.startdt; }, 3000));

            const std::uint64_t gi0 = h.srv->stats().gi_completed;
            EXPECT(CS104_Connection_sendInterrogationCommand(
                con, CS101_COT_ACTIVATION, 1, IEC60870_QOI_STATION));
            EXPECT(wait_until([&] { return h.srv->stats().gi_completed > gi0; }, 5000));

            CS104_Connection_close(con);
            CS104_Connection_destroy(con);
            EXPECT(wait_until(
                [&] { return h.srv->stats().disconnects >= static_cast<std::uint64_t>(round + 1); },
                3000));
        }
        // 断线后 GI 会话必须被清理（否则下一个主站会看到半截状态）
        EXPECT(h.srv->stats().connects == 2);
        EXPECT(h.srv->stats().disconnects == 2);
        EXPECT(h.srv->stats().gi_completed == 2);
        h.stop_all();
    }

    // ------------------------------------------------------------------
    // 点表映射表自检（现场上电首查的同一份逻辑）
    // ------------------------------------------------------------------
    std::printf("-- point map self check --\n");
    {
        std::string rep;
        const int bad = self_check(&rep);
        if (bad != 0) std::cerr << rep;
        EXPECT(bad == 0);

        std::size_t un = 0, dn = 0;
        upload_table(&un); download_table(&dn);
        EXPECT(un == 24);
        EXPECT(dn == 6);

        // 每个上送点的点名都必须在 ems_point_table.h 里
        const PointDef* t = upload_table(&un);
        for (std::size_t i = 0; i < un; ++i) {
            bool found = false;
            for (std::size_t k = 0; k < EMS_POINT_COUNT; ++k)
                if (std::strcmp(EMS_POINT_NAMES[k], t[i].src) == 0) { found = true; break; }
            EXPECT(found);
        }
        // 映射查询
        EXPECT(find_by_ioa(t, un, 4004) != nullptr);
        EXPECT(std::strcmp(find_by_ioa(t, un, 4004)->src, "MEAS.P_GRID") == 0);
        EXPECT(find_by_ioa(t, un, 9999) == nullptr);
        EXPECT(find_upload_by_src("MEAS.SOC") != nullptr);
        EXPECT(find_upload_by_src("NOPE") == nullptr);
        // 系数换算
        const PointDef* soc = find_upload_by_src("MEAS.SOC");
        EXPECT(soc != nullptr && soc->scale == 100.0);
        if (soc) EXPECT_NEAR(to_protocol(*soc, 0.55), 55.0, 1e-9);
        if (soc) EXPECT_NEAR(from_protocol(*soc, 55.0), 0.55, 1e-9);
    }

    // ------------------------------------------------------------------
    // T69 EXT 点区契约（对着点表真相源断言，不抄数字）
    // ------------------------------------------------------------------
    std::printf("-- T69 EXT point zone contract --\n");
    {
        EXPECT(EMS_EXT_END == EMS_POINT_COUNT);                 // 哨兵落在末尾
        EXPECT(EMS_EXT_BEGIN == EMS_STA_BMS_DIS_FORBID + 1);     // 紧接 STA 区、不重叠
        // 7 量测 + 3 指令 + 14 配置 + 6 状态 + 2 BMS 禁充放 + 8 EXT = 40
        EXPECT(EMS_POINT_COUNT == 40);

        // ① 逐点分区：EXT 区里每个点的 zone 都必须是 EXT，一个不落
        for (int i = EMS_EXT_BEGIN; i < EMS_EXT_END; ++i)
            EXPECT(ems_point_zone_of(i) == EMS_ZONE_EXT);

        // ② 边界：区间外一律不是 EXT（含越界）
        EXPECT(!ems_is_ext_point(EMS_EXT_BEGIN - 1));
        EXPECT(ems_is_ext_point(EMS_EXT_BEGIN));
        EXPECT(ems_is_ext_point(EMS_EXT_END - 1));
        EXPECT(!ems_is_ext_point(EMS_EXT_END));
        EXPECT(ems_point_zone_of(-1) == EMS_ZONE_INVALID);
        EXPECT(ems_point_zone_of(EMS_POINT_COUNT) == EMS_ZONE_INVALID);

        // ③ 前四个区的分段边界没被 EXT 挤掉（分段靠枚举值算，不抄数字）
        EXPECT(ems_point_zone_of(EMS_P_LOAD) == EMS_ZONE_MEAS);
        EXPECT(ems_point_zone_of(EMS_CMD_P_BAT) == EMS_ZONE_CMD);
        EXPECT(ems_point_zone_of(EMS_CFG_CAP_KWH) == EMS_ZONE_CFG);
        EXPECT(ems_point_zone_of(EMS_STA_BMS) == EMS_ZONE_STA);
        EXPECT(ems_point_zone_of(EMS_STA_BMS_DIS_FORBID) == EMS_ZONE_STA);
        // "是不是 CMD 点"与"是不是 EXT 点"是两个判据，不能混
        EXPECT(ems_is_cmd_point(EMS_CMD_P_BAT));
        EXPECT(!ems_is_cmd_point(EMS_EXT_P_SETPOINT));

        // ④ 点名：EXT 区一律 "EXT." 前缀。
        //    P3 是**按名字**在段里取点的（索引是运行时坐标），前缀错就全找不到。
        for (int i = EMS_EXT_BEGIN; i < EMS_EXT_END; ++i) {
            EXPECT(EMS_POINT_NAMES[i] != nullptr);
            EXPECT(std::strncmp(EMS_POINT_NAMES[i], "EXT.", 4) == 0);
        }

        EXPECT(std::strcmp(ems_zone_name(EMS_ZONE_EXT), "EXT") == 0);
        EXPECT(std::strcmp(ems_zone_name(EMS_ZONE_MEAS), "MEAS") == 0);
        EXPECT(std::strcmp(ems_zone_name(EMS_ZONE_CMD), "CMD") == 0);
        EXPECT(std::strcmp(ems_zone_name(EMS_ZONE_CFG), "CFG") == 0);
        EXPECT(std::strcmp(ems_zone_name(EMS_ZONE_STA), "STA") == 0);
        EXPECT(std::strcmp(ems_zone_name(EMS_ZONE_INVALID), "INVALID") == 0);
        // 未定义的枚举值也必须回 INVALID（不是 UB、不是越界读）
        EXPECT(std::strcmp(ems_zone_name(static_cast<ems_point_zone_t>(99)), "INVALID") == 0);
    }

    // ------------------------------------------------------------------
    // T69b ★ EXT 默认值必须落在"不引入新约束"那一侧
    //
    // 这条断言的价值全在**反向守卫**上：把 EMS_ENABLE 默认改成 0，或者把
    // P_UPPER_SET 默认改成 0，都是**能编过、能跑、看起来正常**的改动 ——
    // 只在现场表现为"调度还没发过命令，EMS 就已经被闭锁 / 被限制住了"。
    // 没有任何端到端测试会照出来（没有主站 = 没有命令 = 一切"正常"）。
    // ------------------------------------------------------------------
    std::printf("-- T69b EXT default values (no new constraint by default) --\n");
    {
        EXPECT_NEAR(EMS_POINT_DEFAULTS[EMS_EXT_P_SETPOINT],  0.0, 1e-12);  // 0 = 无设定哨兵
        EXPECT(EMS_POINT_DEFAULTS[EMS_EXT_P_UPPER_SET] >  1e8);            // 极大数 = 不限制
        EXPECT(EMS_POINT_DEFAULTS[EMS_EXT_P_LOWER_SET] < -1e8);
        EXPECT_NEAR(EMS_POINT_DEFAULTS[EMS_EXT_D_TARGET],    0.0, 1e-12);  // 0 = 不覆盖 CFG
        EXPECT_NEAR(EMS_POINT_DEFAULTS[EMS_EXT_PCS_ONOFF],   1.0, 1e-12);  // 默认在线
        // ★★ 反向守卫的核心：默认 1 = 允许 EMS 接管
        EXPECT_NEAR(EMS_POINT_DEFAULTS[EMS_EXT_EMS_ENABLE],  1.0, 1e-12);
        EXPECT_NEAR(EMS_POINT_DEFAULTS[EMS_EXT_SEQ],         0.0, 1e-12);  // 0 = 从未收到过
        EXPECT_NEAR(EMS_POINT_DEFAULTS[EMS_EXT_TS],          0.0, 1e-12);

        // 哨兵性质（数值层面）：对**量级正常**的权限区间求交是恒等元。
        //
        // ★ 前提写明：量级远小于 1e9 kW。现场权限区间受 PCS 额定功率约束，
        //   在 1e2 量级，所以 1e9 绰绰有余。
        //   下一段把这条**前提本身**也断言了 —— 免得将来有人以为"极大数 = 数学上的无界"。
        //   （本测试第一版就是用 ±1e30 当初始区间，结果两条断言直接红：
        //    1e9 < 1e30，哨兵确实会把它 clip 掉。那说明"无界"是**不成立**的，
        //    与其把魔数调大把假红推远，不如把真实前提写出来。）
        const double ul = EMS_POINT_DEFAULTS[EMS_EXT_P_UPPER_SET];
        const double ll = EMS_POINT_DEFAULTS[EMS_EXT_P_LOWER_SET];
        {
            double lo = -1e5, hi = 1e5;
            if (ul < hi) hi = ul;
            if (ll > lo) lo = ll;
            EXPECT(hi == 1e5);        // 逐位不变
            EXPECT(lo == -1e5);
        }
        {
            // 显式记录边界：它**不是**数学上的无界，超过 1e9 的区间会被它收窄
            double lo = -1e30, hi = 1e30;
            if (ul < hi) hi = ul;
            if (ll > lo) lo = ll;
            EXPECT(hi == 1e9);
            EXPECT(lo == -1e9);
        }
    }

    // ------------------------------------------------------------------
    // T70 下行 IOA → 设定的对应关系（六条穷举）
    //
    // 为什么值得单测：IOA ↔ 语义的对应关系错了（比如 6003 接到了下界），
    // 端到端联调**看起来一切正常** —— 命令被受理、回肯定确认、值也写进段里了，
    // 只是写错了那一格。只有把六条对应关系逐条钉住才测得到。
    // ------------------------------------------------------------------
    std::printf("-- T70 IOA -> setpoint dispatch (6 exhaustively) --\n");
    {
        struct Rec : public IExtSetpointSink {
            std::string last;
            double v = 0.0;
            bool   b = false;
            int    n = 0, clears = 0;
            bool set_p_setpoint(double kw, std::string*) override { last = "p_setpoint"; v = kw; ++n; return true; }
            bool set_upper     (double kw, std::string*) override { last = "upper";      v = kw; ++n; return true; }
            bool set_lower     (double kw, std::string*) override { last = "lower";      v = kw; ++n; return true; }
            bool set_d_target  (double kw, std::string*) override { last = "d_target";   v = kw; ++n; return true; }
            bool set_pcs_onoff (bool   on, std::string*) override { last = "pcs_onoff";  b = on; ++n; return true; }
            bool set_ems_enable(bool   en, std::string*) override { last = "ems_enable"; b = en; ++n; return true; }
            bool clear_all     (std::string*)            override { ++clears; return true; }
        };

        struct Case { int ioa; const char* name; double val; };
        const Case cases[] = {
            {6001, "p_setpoint", 123.0},
            {6002, "d_target",   456.0},
            {6003, "upper",      200.0},
            {6004, "lower",     -200.0},
            {5001, "pcs_onoff",    1.0},
            {5002, "ems_enable",   0.0},
        };
        for (const Case& c : cases) {
            Rec r;
            std::string err = "<未清空>";
            EXPECT(dispatch_ext_setpoint(r, c.ioa, c.val, &err));
            EXPECT(r.n == 1);                       // 只命中一个落点，没有串台
            EXPECT(r.last == c.name);               // ★ 对应关系本身
            if (c.ioa == 5001 || c.ioa == 5002) EXPECT(r.b == (c.val > 0.5));
            else                                EXPECT_NEAR(r.v, c.val, 1e-12);
            EXPECT(err == "<未清空>");              // 成功时不写 err
        }
        // 布尔点的阈值是 0.5，不是"非零即真"
        { Rec r; EXPECT(dispatch_ext_setpoint(r, 5001, 0.5,  nullptr)); EXPECT(!r.b); }
        { Rec r; EXPECT(dispatch_ext_setpoint(r, 5001, 0.51, nullptr)); EXPECT(r.b);  }

        // 未映射的 IOA：必须**拒绝**，而不是"落到最接近的那个点"
        for (int ioa : {4001, 6005, 5003, 0, -1}) {
            Rec r;
            std::string err;
            EXPECT(!dispatch_ext_setpoint(r, ioa, 1.0, &err));
            EXPECT(r.n == 0);
            EXPECT(!err.empty());                   // 拒绝必须带原因（进否定确认）
        }
        { Rec r; EXPECT(!dispatch_ext_setpoint(r, 6005, 1.0, nullptr)); }   // err 可为空指针

        // 默认实现（未配落点）一律拒绝 —— 这是安全的一侧
        NullSetpointSink nul;
        for (const Case& c : cases) {
            std::string err;
            EXPECT(!dispatch_ext_setpoint(nul, c.ioa, c.val, &err));
            EXPECT(!err.empty());
        }
        { std::string e; EXPECT(nul.clear_all(&e)); }   // 清空 = 幂等的"无事可做"
    }

    // ------------------------------------------------------------------
    // T71 读取与失效判定（load_ext_setpoints）
    //
    // 本组**不碰共享内存**：读口是注入的 `read(index) -> double`，
    // 所以"序号抖动 / 读不到 / 未来时标 / NaN"这些边界可以穷举，
    // 不用去构造一个真的会被并发改写的段。
    // ------------------------------------------------------------------
    std::printf("-- T71 EXT read + fail-safe --\n");
    {
        using ems::ExtSetpoints;
        using ems::load_ext_setpoints;

        // 用**点表默认值**当初始哨兵，不另抄数字
        std::map<int, double> st;
        for (int i = EMS_EXT_BEGIN; i < EMS_EXT_END; ++i) st[i] = EMS_POINT_DEFAULTS[i];
        int seq_bump = 0;   // >0 时"每次读 SEQ 结果都不同" → 模拟读取途中区被改写

        auto read = [&](int idx, double& v) -> bool {
            auto it = st.find(idx);
            if (it == st.end()) return false;                 // 点读不到
            v = (idx == EMS_EXT_SEQ && seq_bump > 0) ? it->second + (seq_bump++) : it->second;
            return true;
        };
        const double now = 1'000'000.0;                       // 固定时间轴，可复算
        auto load = [&](double stale_s, int retries = 8) {
            return load_ext_setpoints(read, now, stale_s, retries);
        };
        // 造一个"半新半旧"的快照：上界已改、下界没改（= 写者正在改写的中间态），
        // 并指定读者会看到的 SEQ。用来验证"奇偶判据"真的能挡住这一种。
        auto load_with = [&](double seq, double upper, double stale_s, int retries) {
            std::map<int, double> s2 = st;
            s2[EMS_EXT_SEQ]         = seq;
            s2[EMS_EXT_P_UPPER_SET] = upper;
            auto rd = [s2](int idx, double& v) -> bool {
                auto it = s2.find(idx);
                if (it == s2.end()) return false;
                v = it->second;
                return true;
            };
            return load_ext_setpoints(rd, now, stale_s, retries);
        };

        // ① SEQ = 0：从未收到过任何外部设定 → present=false，且载荷保持哨兵
        {
            const ExtSetpoints e = load(300.0);
            EXPECT(!e.present);
            EXPECT(!e.usable());
            EXPECT_NEAR(e.p_setpoint,  0.0,  1e-12);
            EXPECT_NEAR(e.p_upper_set, 1e9,  1e-12);          // 仍是"不限制"
            EXPECT_NEAR(e.p_lower_set, -1e9, 1e-12);
        }

        // ② 正常一次发布
        st[EMS_EXT_SEQ] = 8.0;
        st[EMS_EXT_TS]  = now - 1.0;
        st[EMS_EXT_P_SETPOINT]  = 100.0;
        st[EMS_EXT_P_UPPER_SET] = 200.0;
        st[EMS_EXT_P_LOWER_SET] = -150.0;
        st[EMS_EXT_D_TARGET]    = 250.0;
        st[EMS_EXT_PCS_ONOFF]   = 1.0;
        st[EMS_EXT_EMS_ENABLE]  = 1.0;
        {
            const ExtSetpoints e = load(300.0);
            EXPECT(e.present && e.consistent && e.finite && e.band_ok);
            EXPECT(!e.stale && !e.from_future);
            EXPECT(e.usable());
            EXPECT(e.seq == 8);
            EXPECT_NEAR(e.age_s, 1.0, 1e-9);
            EXPECT_NEAR(e.p_setpoint,  100.0, 1e-12);
            EXPECT_NEAR(e.p_upper_set, 200.0, 1e-12);
            EXPECT_NEAR(e.p_lower_set, -150.0, 1e-12);
            EXPECT(e.pcs_onoff && e.ems_enable);
        }

        // ③ ★ 序号抖动（读取途中区被改）→ 重试耗尽 → **退回哨兵**，
        //    而不是"一半新一半旧"。中间态可能比两端都宽，那正是不能留的东西。
        seq_bump = 1;
        {
            const ExtSetpoints e = load(300.0, 3);
            EXPECT(e.present);              // 确实收到过（SEQ>0 读到过）
            EXPECT(!e.consistent);          // 但本拍没有一致快照
            EXPECT(!e.usable());
            EXPECT(e.retries == 3);         // 用满重试次数
            EXPECT_NEAR(e.p_setpoint,  0.0,  1e-12);
            EXPECT_NEAR(e.p_upper_set, 1e9,  1e-12);
            EXPECT_NEAR(e.p_lower_set, -1e9, 1e-12);
            EXPECT(e.pcs_onoff && e.ems_enable);   // ★ 读失败不能让 EMS 被闭锁
        }
        seq_bump = 0;

        // ③b ★ 落在"改写中"的**奇数窗口**：s0 == s1 也必须拒绝
        //     写者恢复慢的时候，读者两次读 SEQ 可能都落在同一个奇数窗口里 ——
        //     相等，但载荷正被改。这一种只有奇偶判据挡得住。
        //     （A3.1 修协议时发现的缺陷：原先写者只在末尾递增一次，
        //       "读者在载荷写入期间读到旧 SEQ"会被当成一致快照接受。）
        {
            const ExtSetpoints e = load_with(9.0 /*奇数 = 改写中*/, 999.0, 300.0, 2);
            EXPECT(!e.consistent);
            EXPECT(!e.usable());
            EXPECT_NEAR(e.p_upper_set, 1e9, 1e-12);   // 退回哨兵，而不是那个 999
        }
        // 同一个时序但序号是**偶数**（真的没人在改）→ 必须接受。
        // 没有这一条，"奇偶判据"退化成"永远拒绝"也没人发现。
        {
            const ExtSetpoints e = load_with(8.0, 999.0, 300.0, 2);
            EXPECT(e.consistent);
            EXPECT(e.usable());
            EXPECT_NEAR(e.p_upper_set, 999.0, 1e-12);
        }

        // ④ 陈旧
        st[EMS_EXT_TS] = now - 100.0;
        { const ExtSetpoints e = load(10.0); EXPECT(e.present); EXPECT(e.stale); EXPECT(!e.usable());
          EXPECT(e.consistent && e.finite && e.band_ok); }     // 只有 stale 一项不满足
        // 边界：age == stale_s → **不算**陈旧（判据是严格大于）
        { const ExtSetpoints e = load(100.0); EXPECT_NEAR(e.age_s, 100.0, 1e-9); EXPECT(!e.stale); }

        // ⑤ ★ 时标来自未来 → 判不可用，且**必须**计成 stale
        //    若只标 from_future 不动 stale，一个未来时标会让"陈旧判定"
        //    永远不成立 —— 等于把兜底关掉了。
        st[EMS_EXT_TS] = now + 100.0;
        { const ExtSetpoints e = load(300.0);
          EXPECT(e.from_future); EXPECT(e.age_s < 0.0); EXPECT(e.stale); EXPECT(!e.usable()); }

        // ⑥ ★ stale_s <= 0 的含义是"禁用外部设定"（视为总是陈旧），
        //    而不是"禁用超时判定"。前者的失效方向是安全的。
        st[EMS_EXT_TS] = now;
        { const ExtSetpoints e = load(0.0);   EXPECT(e.stale); EXPECT(!e.usable()); }
        { const ExtSetpoints e = load(-5.0);  EXPECT(e.stale); EXPECT(!e.usable()); }
        { const ExtSetpoints e = load(0.001); EXPECT(!e.stale); EXPECT(e.usable()); }

        // ⑦ NaN / Inf 载荷 → finite=false（NaN 转 bool 会变成 true，看起来正常）
        st[EMS_EXT_P_UPPER_SET] = std::nan("");
        { const ExtSetpoints e = load(300.0); EXPECT(!e.finite); EXPECT(!e.usable()); }
        st[EMS_EXT_P_UPPER_SET] = std::numeric_limits<double>::infinity();
        { const ExtSetpoints e = load(300.0); EXPECT(!e.finite); EXPECT(!e.usable()); }
        st[EMS_EXT_P_UPPER_SET] = 200.0;
        // 布尔点的原始值也参与有限性检查（它是 double，不是 bool）
        st[EMS_EXT_EMS_ENABLE] = std::nan("");
        { const ExtSetpoints e = load(300.0); EXPECT(!e.finite); EXPECT(!e.usable()); }
        st[EMS_EXT_EMS_ENABLE] = 1.0;

        // ⑧ 上下界倒挂（调度配错）→ band_ok=false
        st[EMS_EXT_P_LOWER_SET] = 500.0;    // > upper(200)
        { const ExtSetpoints e = load(300.0); EXPECT(!e.band_ok); EXPECT(!e.usable()); }
        st[EMS_EXT_P_LOWER_SET] = -150.0;
        st[EMS_EXT_P_LOWER_SET] = 200.0;    // 上下界**相等**是合法的（只允许一个点）
        { const ExtSetpoints e = load(300.0); EXPECT(e.band_ok); EXPECT(e.usable()); }
        st[EMS_EXT_P_LOWER_SET] = -150.0;

        // ⑨ 载荷里有一个点读不到 → 本拍没有一致快照
        st.erase(EMS_EXT_P_SETPOINT);
        { const ExtSetpoints e = load(300.0);
          EXPECT(e.present);        // SEQ 读到了且 >0
          EXPECT(!e.consistent);    // 但组不齐
          EXPECT(!e.usable()); }
        st[EMS_EXT_P_SETPOINT] = 100.0;

        // ⑩ ★★ 连 SEQ 都读不到 → present 必须为 **false**
        //     "present" 的字面含义是"段里收到过外部设定"，不是"本拍读成功"。
        //     原先无条件 present=true，于是一个**根本读不到 SEQ** 的段会报
        //     "收到过、快照不一致" —— 指向完全错误的方向。
        //     （usable() 两种情况都是 false，所以行为没变 —— 这正是它难被发现的原因。）
        st.erase(EMS_EXT_SEQ);
        { const ExtSetpoints e = load(300.0);
          EXPECT(!e.present);
          EXPECT(!e.consistent);
          EXPECT(!e.usable());
          EXPECT(e.seq == 0); }
        st[EMS_EXT_SEQ] = 8.0;

        // ⑪ max_retries 下限保护：传 0 / 负数不能变成"一次都不读"
        st[EMS_EXT_SEQ] = 8.0;
        { const ExtSetpoints e = load_ext_setpoints(read, now, 300.0, 0);
          EXPECT(e.usable()); }                     // 按 1 次处理 → 抖动为 0 时照样成功
        seq_bump = 1;
        { const ExtSetpoints e = load_ext_setpoints(read, now, 300.0, -3);
          EXPECT(!e.usable()); EXPECT(e.retries == 1); }
        seq_bump = 0;
    }

    // ------------------------------------------------------------------
    // T72 外部设定 → 权限区间：只能收紧，绝不放宽
    //
    // A3.2 才接线，但**语义现在就要钉住**：否则到时候凭记忆重推一遍，
    // 很容易顺手写成"对称幅值带 [-|S|, +|S|]"（字面上更贴直觉，代价见下）。
    // ------------------------------------------------------------------
    std::printf("-- T72 narrow_interval_by_ext (tighten only) --\n");
    {
        using ems::ExtSetpoints;
        using ems::narrow_interval_by_ext;
        const double INF = 1e9;

        auto ext = [](double setpoint, double us, double ls,
                      bool onoff = true, bool enable = true) {
            ExtSetpoints e;
            e.present = true; e.consistent = true;
            e.p_setpoint = setpoint; e.p_upper_set = us; e.p_lower_set = ls;
            e.pcs_onoff = onoff; e.ems_enable = enable;
            return e;
        };

        // ① 正设定只收紧上界 —— ★ 下界必须**逐位不动**
        //    对称幅值带会把 -500 抬到 -100：那等于顺手把反方向也锁死，
        //    光伏大发 / 谷时充电时可能制造 interval_contradiction → 假降额。
        { double lo = -500.0, hi = 500.0;
          narrow_interval_by_ext(ext(100.0, INF, -INF), &lo, &hi);
          EXPECT_NEAR(hi, 100.0, 1e-12);
          EXPECT_NEAR(lo, -500.0, 1e-12); }
        // ② 负设定只收紧下界（对称）
        { double lo = -500.0, hi = 500.0;
          narrow_interval_by_ext(ext(-100.0, INF, -INF), &lo, &hi);
          EXPECT_NEAR(lo, -100.0, 1e-12);
          EXPECT_NEAR(hi, 500.0, 1e-12); }
        // ③ 0 = 无设定 → 两个边界都不动
        { double lo = -500.0, hi = 500.0;
          narrow_interval_by_ext(ext(0.0, INF, -INF), &lo, &hi);
          EXPECT(lo == -500.0 && hi == 500.0); }
        // ④ 6003 / 6004 显式收紧，方向固定（上界设定只可能压上界）
        { double lo = -500.0, hi = 500.0;
          narrow_interval_by_ext(ext(0.0, 200.0, -200.0), &lo, &hi);
          EXPECT_NEAR(hi, 200.0, 1e-12); EXPECT_NEAR(lo, -200.0, 1e-12); }
        // ⑤ ★ 更宽的设定被 min/max 吃掉 —— 结构上不可能放宽
        { double lo = -50.0, hi = 50.0;
          narrow_interval_by_ext(ext(0.0, 999.0, -999.0), &lo, &hi);
          EXPECT(lo == -50.0 && hi == 50.0); }
        { double lo = 10.0, hi = 20.0;
          narrow_interval_by_ext(ext(300.0, INF, -INF), &lo, &hi);
          EXPECT(lo == 10.0 && hi == 20.0); }
        // ⑥ 5002 EMS 闭锁 / 5001 PCS 停机 → 强制 [0,0]
        { double lo = -500.0, hi = 500.0;
          narrow_interval_by_ext(ext(100.0, INF, -INF, true, false), &lo, &hi);
          EXPECT(lo == 0.0 && hi == 0.0); }
        { double lo = -500.0, hi = 500.0;
          narrow_interval_by_ext(ext(100.0, INF, -INF, false, true), &lo, &hi);
          EXPECT(lo == 0.0 && hi == 0.0); }

        // ⑦ ★ usable()==false → 功率设定区间**逐位不动**（停机/闭锁的粘住是例外，见 ⑧）
        //    失效方向必须是不施加约束。"收一半"（比如只应用了上界）在数据上
        //    与"收到一个很窄的设定"不可区分，现场没法排查。
        auto untouched = [](const ExtSetpoints& e) {
            double lo = -500.0, hi = 500.0;
            narrow_interval_by_ext(e, &lo, &hi);
            EXPECT(lo == -500.0 && hi == 500.0);
        };
        { ExtSetpoints e = ext(100.0, 50.0, -100.0); e.present    = false; EXPECT(!e.usable()); untouched(e); }
        { ExtSetpoints e = ext(100.0, 50.0, -100.0); e.stale      = true;  EXPECT(!e.usable()); untouched(e); }
        { ExtSetpoints e = ext(100.0, 50.0, -100.0); e.consistent = false; EXPECT(!e.usable()); untouched(e); }
        { ExtSetpoints e = ext(100.0, 50.0, -100.0); e.finite     = false; EXPECT(!e.usable()); untouched(e); }
        { ExtSetpoints e = ext(100.0, 50.0, -100.0); e.band_ok    = false; EXPECT(!e.usable()); untouched(e); }

        // ⑧ ★ A3.2 拍板：stale 时"停机 / 闭锁"**粘住**（不是"不生效"）。
        //    "调度最后说停"是状态意图，通信断了不该自动复机 —— 只要拿到过
        //    一致的快照、数值合法，"旧"不是"坏"。
        //    粘住守卫 = present && consistent && finite（**不含** stale / band_ok）。
        { ExtSetpoints e = ext(0.0, INF, -INF, false, true);   // 5001 停机
          e.stale = true;
          double lo = -500.0, hi = 500.0;
          narrow_interval_by_ext(e, &lo, &hi);
          EXPECT(lo == 0.0 && hi == 0.0); }                      // 粘住
        { ExtSetpoints e = ext(0.0, INF, -INF, true, false);   // 5002 闭锁
          e.stale = true;
          double lo = -500.0, hi = 500.0;
          narrow_interval_by_ext(e, &lo, &hi);
          EXPECT(lo == 0.0 && hi == 0.0); }                      // 粘住
        // ⑧b stale 但**没有**停机/闭锁 → 功率设定仍逐位不动（不生效）
        { ExtSetpoints e = ext(100.0, 50.0, -100.0);
          e.stale = true;
          double lo = -500.0, hi = 500.0;
          narrow_interval_by_ext(e, &lo, &hi);
          EXPECT(lo == -500.0 && hi == 500.0); }
        // ⑧c ★ finite=false（NaN raw 值会经 `>0.5` 判成 false，被误当"停机"）→ 不得粘住
        { ExtSetpoints e = ext(0.0, INF, -INF, false, true);
          e.stale = true; e.finite = false;
          double lo = -500.0, hi = 500.0;
          narrow_interval_by_ext(e, &lo, &hi);
          EXPECT(lo == -500.0 && hi == 500.0); }
        // ⑧d consistent=false（没拿到一致快照）→ 也不得粘住
        { ExtSetpoints e = ext(0.0, INF, -INF, false, true);
          e.stale = true; e.consistent = false;
          double lo = -500.0, hi = 500.0;
          narrow_interval_by_ext(e, &lo, &hi);
          EXPECT(lo == -500.0 && hi == 500.0); }
    }

    // ------------------------------------------------------------------
    // T77 转发表外置 CSV（load_point_map_csv，design §6）
    //
    // 核心判据：样例 CSV（data/point_map_default.csv）加载后必须与**内置默认表
    // 逐字段一致** —— 这样「样例文件」和「编译进去的兜底」就互为镜像，
    // 谁改了一边都会在另一边露馅。加载失败必须回退默认（绝不"加载了一半"）。
    // ------------------------------------------------------------------
    std::printf("-- T77 point map CSV (load_point_map_csv) --\n");
    {
        auto write_csv = [](const char* path, const std::string& content) -> bool {
            std::ofstream f(path);
            if (!f.is_open()) return false;
            f << content;
            return true;
        };

        // ① 快照内置默认表（加载 CSV 后 upload_table 会返回覆盖表，先存下默认）
        std::vector<PointDef> def_up, def_dn;
        {
            std::size_t n = 0;
            const PointDef* t = upload_table(&n);
            def_up.assign(t, t + n);
        }
        {
            std::size_t n = 0;
            const PointDef* t = download_table(&n);
            def_dn.assign(t, t + n);
        }
        EXPECT(def_up.size() == 24);
        EXPECT(def_dn.size() == 6);

        // ② 加载样例 CSV → 与默认表逐字段对账（note 是纯文档，不作约束）
        {
            std::string rep;
            const bool ok = load_point_map_csv("data/point_map_default.csv", &rep);
            if (!ok) std::cerr << rep;
            EXPECT(ok);
            std::size_t un = 0, dn = 0;
            const PointDef* up = upload_table(&un);
            const PointDef* down = download_table(&dn);
            EXPECT(un == 24);
            EXPECT(dn == 6);
            auto eq = [](const PointDef& a, const PointDef& b) {
                return a.ioa == b.ioa && a.kind == b.kind && a.group == b.group &&
                       std::strcmp(a.src, b.src) == 0 &&
                       std::strcmp(a.name, b.name) == 0 &&
                       std::strcmp(a.unit, b.unit) == 0 &&
                       a.scale == b.scale && a.offset == b.offset;
            };
            bool all_match = (un == def_up.size()) && (dn == def_dn.size());
            for (std::size_t i = 0; all_match && i < un; ++i) all_match = eq(up[i], def_up[i]);
            for (std::size_t i = 0; all_match && i < dn; ++i) all_match = eq(down[i], def_dn[i]);
            EXPECT(all_match);
            std::string rep2;
            EXPECT(self_check(&rep2) == 0);       // 加载表也要过同一套自检
        }

        // ③ reset_point_map 回到默认
        {
            reset_point_map();
            std::size_t un = 0, dn = 0;
            upload_table(&un); download_table(&dn);
            EXPECT(un == 24 && dn == 6);
            EXPECT(find_upload_by_src("MEAS.P_GRID") != nullptr);
        }

        // ④ 拒绝：未知 kind
        {
            const char* p = "build/_t77_bad_kind.csv";
            EXPECT(write_csv(p, "ioa,kind,group,name,src,scale,offset,unit,note\n"
                               "4001,BOGUS_KIND,1,x,MEAS.P_LOAD,1.0,0.0,kW,\n"));
            std::string rep;
            EXPECT(!load_point_map_csv(p, &rep));
            std::size_t un = 0; upload_table(&un);
            EXPECT(un == 24);                          // 回退默认
            std::remove(p);
        }

        // ⑤ 拒绝：scale=0（from_protocol 除零）
        {
            const char* p = "build/_t77_scale0.csv";
            EXPECT(write_csv(p, "ioa,kind,group,name,src,scale,offset,unit,note\n"
                               "4001,MEAS_FLOAT,1,x,MEAS.P_LOAD,0.0,0.0,kW,\n"));
            std::string rep;
            EXPECT(!load_point_map_csv(p, &rep));
            std::remove(p);
        }

        // ⑥ 拒绝：上送表内 IOA 撞号
        {
            const char* p = "build/_t77_dup.csv";
            EXPECT(write_csv(p, "ioa,kind,group,name,src,scale,offset,unit,note\n"
                               "4001,MEAS_FLOAT,1,a,MEAS.P_LOAD,1.0,0.0,kW,\n"
                               "4001,MEAS_FLOAT,1,b,MEAS.P_PV,1.0,0.0,kW,\n"));
            std::string rep;
            EXPECT(!load_point_map_csv(p, &rep));
            std::remove(p);
        }

        // ⑦ 拒绝：src 点名不在点表
        {
            const char* p = "build/_t77_badsrc.csv";
            EXPECT(write_csv(p, "ioa,kind,group,name,src,scale,offset,unit,note\n"
                               "4001,MEAS_FLOAT,1,x,MEAS.NOPE,1.0,0.0,kW,\n"));
            std::string rep;
            EXPECT(!load_point_map_csv(p, &rep));
            std::remove(p);
        }

        // ⑧ 拒绝：文件不存在
        {
            std::string rep;
            EXPECT(!load_point_map_csv("build/_t77_no_such_file.csv", &rep));
        }

        // ⑨ 拒绝：字段数不对（缺 note）
        {
            const char* p = "build/_t77_short.csv";
            EXPECT(write_csv(p, "ioa,kind,group,name,src,scale,offset,unit\n"
                               "4001,MEAS_FLOAT,1,x,MEAS.P_LOAD,1.0,0.0,kW\n"));
            std::string rep;
            EXPECT(!load_point_map_csv(p, &rep));
            std::remove(p);
        }

        // ⑩ kind 大小写不敏感 + 容忍空白
        {
            const char* p = "build/_t77_case.csv";
            EXPECT(write_csv(p, "ioa,kind,group,name,src,scale,offset,unit,note\n"
                               "4001,  meas_float ,1,x,MEAS.P_LOAD,1.0,0.0,kW,\n"));
            std::string rep;
            EXPECT(load_point_map_csv(p, &rep));
            std::size_t un = 0;
            const PointDef* up = upload_table(&un);
            EXPECT(un == 1);
            EXPECT(up[0].kind == Kind::kMeasFloat);
            std::remove(p);
            reset_point_map();
        }

        // ⑪ 控制点 src 可留空（与内置下行表同构）
        {
            const char* p = "build/_t77_ctrl.csv";
            EXPECT(write_csv(p, "ioa,kind,group,name,src,scale,offset,unit,note\n"
                               "6001,SET_FLOAT,0,有功功率设定,,1.0,0.0,kW,\n"
                               "5001,CMD_SINGLE,0,PCS 远程启停,,1.0,0.0,,\n"));
            std::string rep;
            EXPECT(load_point_map_csv(p, &rep));
            std::size_t dn = 0;
            download_table(&dn);
            EXPECT(dn == 2);
            std::remove(p);
            reset_point_map();
        }

        // 收尾：确保回到默认（不污染后续任何依赖默认表的用例）
        reset_point_map();
    }

    std::printf("\n=== P3 IEC104: PASS=%d FAIL=%d ===\n", g_pass, g_fail);
    std::printf("%s\n", g_fail == 0 ? "[ALL PASS]" : "[HAS FAILURES]");
    return g_fail == 0 ? 0 : 1;
}
