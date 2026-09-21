// =====================================================================
// P3 / IEC104 从站（服务器）封装
//
// 把 lib60870-C 的 CS104_Slave 包成一个可用的对象，业务侧只需要：
//     Iec104Server srv(cfg, &source, &sink);
//     srv.start();
//     while (running) { srv.tick(now_ms); sleep(10ms); }
//
// ---------------------------------------------------------------------
// 角色定位：EMS 是**从站**，不是主站
// ---------------------------------------------------------------------
// 工商业储能对上接的是电网调度 / 虚拟电厂平台，那一边才有总召唤与下发遥控的权力。
// 所以 EMS 侧实现 CS104_Slave（监听 2404），调度做主站连过来。
// 主站 API（CS104_Connection）本项目只在**自环测试**里用（见 tests/test_iec104.cpp）——
// 用同一个库的主站打自己的从站，是最省事也最可信的验证方式。
//
// ---------------------------------------------------------------------
// 三件事的时序纪律（写在代码里，因为很容易做错）
// ---------------------------------------------------------------------
//   1) 总召唤（GI）
//      interrogationHandler **只登记 + 回 ACT_CON**，数据由 tick() 异步分批推，
//      最后发 ACT_TERM。绝不能在回调里发完所有数据 ——
//      库的协议栈线程会被阻塞，k/w 计数不推进，主站侧 t1 超时断链。
//      → 这是手册 §11 明确点名的坑，也是现场「总召唤卡住」的头号原因。
//
//   2) 周期 / 变化上报
//      · 变化（SPONTANEOUS）：只在值真的变了（超死区）时发，省带宽
//      · 周期（PERIODIC）：兜底全量，防止主站漏掉一次变化就永久失准
//      两者都由 tick() 驱动，与 EMS 的控制周期（100 ms）解耦。
//
//   3) 遥控 / 遥调
//      收到命令 → 交给 ICommandSink → **按 sink 的返回值决定肯定/否定确认**。
//      不能无条件回 ACT_CON：那样调度会以为命令生效了，而实际上被安全层挡掉了。
//      命令的「是否被接受」必须是**端到端可追溯**的，这是安全相关的要求。
//
// ---------------------------------------------------------------------
// 内存纪律（库的不透明句柄 + 手工生命周期，错了就是泄漏或野指针）
// ---------------------------------------------------------------------
//   · CS101_ASDU_getElement 取出的 IO **必须** InformationObject_destroy()
//   · 我们自己 create 的 ASDU，enqueue/send 之后才 CS101_ASDU_destroy()
//   · 回调里的 asdu 句柄归库所有，**不要** destroy
//   · 不要在回调里 destroy slave/connection
// =====================================================================

#ifndef EMS_P3_IEC104_SERVER_H
#define EMS_P3_IEC104_SERVER_H

#include "cs104_slave.h"
#include "cs101_information_objects.h"
#include "iec60870_common.h"
#include "iec60870_slave.h"

#include "iec104_point_map.h"
#include "iec104_time.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace iec104 {

// =====================================================================
// 数据源：把「值从哪来」与「协议怎么发」解耦
//
// 对齐 04/ 的 IDeviceIO 思路 —— 同一份网关代码可以喂：
//   · RtDbSource    读共享内存实时库（生产）
//   · MemorySource  内存表（单元测试 / 自环）
// 这样的好处是**协议层可以脱离共享内存单独测**，反之亦然。
// =====================================================================
// ---------------------------------------------------------------------
// 品质三态
// ---------------------------------------------------------------------
// 为什么不是 bool：
//   IEC104 的品质字节里 IV（0x80）与 NT（0x40）是**两件不同的事**：
//     IV = 值不可用（读不到、源明确标坏、通信断了）
//     NT = 值可读但已经**过时** —— 主站应显示旧值但不得据此决策
//   如果源侧只回答 bool，NT 就只能退化成 IV 或 GOOD 二者之一：
//   退化成 IV → 主站把"5 秒前的真实值"当成"没数据"，画面闪断；
//   退化成 GOOD → 主站拿陈旧值当成实时值，是最坏的一种错。
//   RT_DB 的 data_quality_t 本身就带 QUALITY_UNCERTAIN，所以这条信息
//   在源头是存在的，不该在网关这一层被丢掉。
// ---------------------------------------------------------------------
enum class Quality {
    kGood      = 0,   // 值可信 → 品质 0x00
    kNotTopical= 1,   // 值可读但陈旧/不确定 → 品质 0x40（NT）
    kInvalid   = 2    // 读不到 / 源标坏 → 品质 0x80（IV）
};

class IDataSource {
public:
    virtual ~IDataSource() = default;

    // 读一个内部点名（如 "MEAS.P_GRID"）。
    // 返回 false 表示**这一拍没有值**（此时 *q 应为 kInvalid，out 无意义）。
    // 返回 true 但 *q = kNotTopical 表示值拿到了、但不可据以决策。
    // 标 const：读操作不该改数据源状态（统计计数用 mutable）。
    virtual bool read(const char* point, double* out, Quality* q = nullptr) const = 0;
};

// =====================================================================
// 命令出口：遥控/遥调的落点
//
// 网关**不写 RT_DB**（角色边界：设备侧写 MEAS/STA/CFG、EMS 写 CMD、
// 网关是第五个角色，只读）。命令交给 sink，由装配层决定往哪放。
// =====================================================================
class ICommandSink {
public:
    virtual ~ICommandSink() = default;

    // 返回 true → 回肯定确认（ACT_CON, negative=false）
    // 返回 false → 回否定确认（ACT_CON, negative=true），err 写明原因
    //
    // select=true 是选择-执行（SBO）流程的**选择**步。本项目默认走直接控制
    // （调度发来的都是 select=false），但仍按 SBO 语义处理，见下方 select 状态机。
    virtual bool on_command(int ioa, Kind kind, double value, bool select,
                            int ca, std::string* err) = 0;

    // 可选：连接开/关时通知（现场用来记「调度什么时候连上/断开的」）
    virtual void on_connection(bool opened, const char* peer) { (void)opened; (void)peer; }
};

// =====================================================================
// 配置
// =====================================================================
struct ServerConfig {
    std::string bind_addr = "0.0.0.0";
    int         port      = 2404;
    int         ca        = 1;      // 公共地址：调度转发表里的站地址
    int         oa        = 0;      // 源发地址
    int         max_conn  = 4;      // 最大并发主站连接

    // ---- 上报节拍 ----
    double periodic_s      = 5.0;   // 周期全量上报间隔（秒）
    double change_deadband = 0.002; // 变化死区：相对量程的比例（0.2%）

    // ---- 品质 ----
    bool use_timestamp_on_spontaneous = true;  // 自发/周期用带时标类型；GI 一律不带

    // ---- 调试 ----
    bool dump_raw = false;          // 打印每个原始报文帧（现场排障用，很吵）
};

// =====================================================================
// 从站
// =====================================================================
class Iec104Server {
public:
    struct Stats {
        // 连接
        std::uint64_t connects          = 0;
        std::uint64_t disconnects       = 0;
        std::uint64_t startdt           = 0;
        // 总召唤
        std::uint64_t gi_requests       = 0;
        std::uint64_t gi_asdu_sent      = 0;
        std::uint64_t gi_completed      = 0;
        std::uint64_t gi_rejected       = 0;   // 非 CA / 非站召唤
        std::uint64_t gi_act_term_sent  = 0;   // 发出 ACT_TERM 的次数（应为 gi_completed）
        // 对钟
        std::uint64_t clock_syncs       = 0;
        // 上报
        std::uint64_t periodic_asdu     = 0;
        std::uint64_t spontaneous_asdu  = 0;
        std::uint64_t points_uploaded   = 0;
        std::uint64_t points_invalid    = 0;   // 读不到 → 品质 INVALID (0x80)
        std::uint64_t points_not_topical= 0;   // 值陈旧/不确定 → 品质 NT (0x40)
        // 下行
        std::uint64_t commands_rx       = 0;
        std::uint64_t commands_accepted = 0;
        std::uint64_t commands_rejected = 0;
        // 异常
        std::uint64_t unknown_ioa       = 0;
        std::uint64_t unknown_ca        = 0;
        std::uint64_t unknown_cot       = 0;
        std::uint64_t illegal_type      = 0;
    };

    Iec104Server(const ServerConfig& cfg, IDataSource* source, ICommandSink* sink)
        : cfg_(cfg), src_(source), sink_(sink) {
        std::size_t n = 0;
        up_ = upload_table(&n);
        up_n_ = n;
        dn_ = download_table(&n);
        dn_n_ = n;
        prev_.assign(up_n_, 0.0);
        prev_valid_.assign(up_n_, false);
        prev_ok_.assign(up_n_, 0);
    }

    ~Iec104Server() { stop(); }

    Iec104Server(const Iec104Server&) = delete;
    Iec104Server& operator=(const Iec104Server&) = delete;

    // =================================================================
    // 生命周期
    // =================================================================
    bool start() {
        slave_ = CS104_Slave_create(100 /*低优先级队列*/, 50 /*高优先级队列*/);
        if (slave_ == nullptr) return false;

        CS104_Slave_setLocalAddress(slave_, cfg_.bind_addr.c_str());
        CS104_Slave_setLocalPort(slave_, cfg_.port);
        // 单冗余组：只允许 1 个活动主站，其余热备。国内调度最常用。
        CS104_Slave_setServerMode(slave_, CS104_MODE_SINGLE_REDUNDANCY_GROUP);
        CS104_Slave_setMaxOpenConnections(slave_, cfg_.max_conn);

        CS104_Slave_setConnectionRequestHandler(slave_, &Iec104Server::on_conn_request, this);
        CS104_Slave_setConnectionEventHandler(slave_, &Iec104Server::on_conn_event, this);
        CS104_Slave_setInterrogationHandler(slave_, &Iec104Server::on_interrogation, this);
        CS104_Slave_setClockSyncHandler(slave_, &Iec104Server::on_clock_sync, this);
        CS104_Slave_setASDUHandler(slave_, &Iec104Server::on_asdu, this);
        if (cfg_.dump_raw)
            CS104_Slave_setRawMessageHandler(slave_, &Iec104Server::on_raw, this);

        CS104_Slave_start(slave_);
        if (!CS104_Slave_isRunning(slave_)) {
            CS104_Slave_destroy(slave_);
            slave_ = nullptr;
            return false;
        }
        running_ = true;
        return true;
    }

    void stop() {
        if (slave_ != nullptr) {
            CS104_Slave_stop(slave_);
            CS104_Slave_destroy(slave_);
            slave_ = nullptr;
        }
        running_ = false;
    }

    bool running() const { return running_; }
    const Stats& stats() const { return st_; }
    Stats& stats() { return st_; }

    // =================================================================
    // 主循环驱动：跑 GI 迭代 + 变化上报 + 周期上报
    // 建议 10~50 ms 调一次。**不要**在别处直接调协议栈。
    // =================================================================
    void tick(std::uint64_t now_ms) {
        if (!running_) return;
        serve_gi(now_ms);
        report_changes(now_ms);
        report_periodic(now_ms);
    }

    // 手动触发一次全量上送（现场排障用：`--push-now`）
    void force_periodic() { last_periodic_ms_ = 0; }

private:
    // =================================================================
    // 回调：连接请求（IP 白名单钩子；这里先全放行，列出对端供日志）
    // =================================================================
    static bool on_conn_request(void* param, const char* ip) {
        Iec104Server* self = static_cast<Iec104Server*>(param);
        std::snprintf(self->peer_, sizeof(self->peer_), "%s", ip ? ip : "?");
        return true;   // TODO 现场：按调度主站 IP 白名单收紧（见 docs/README.md §6）
    }

    static void on_conn_event(void* param, IMasterConnection con, CS104_PeerConnectionEvent e) {
        Iec104Server* self = static_cast<Iec104Server*>(param);
        switch (e) {
        case CS104_CON_EVENT_CONNECTION_OPENED:
            ++self->st_.connects;
            if (self->sink_) self->sink_->on_connection(true, self->peer_);
            break;
        case CS104_CON_EVENT_CONNECTION_CLOSED:
            ++self->st_.disconnects;
            if (self->sink_) self->sink_->on_connection(false, self->peer_);
            // 连接断了要把会话状态清掉，否则下一个主站接上来会看到上一个的半截状态
            self->gi_state_ = GiState::kIdle;
            self->gi_conn_ = nullptr;
            self->select_ioa_ = -1;
            break;
        case CS104_CON_EVENT_ACTIVATED:
            ++self->st_.startdt;
            break;
        case CS104_CON_EVENT_DEACTIVATED:
            break;
        }
        (void)con;
    }

    // =================================================================
    // 回调：总召唤
    //   只做两件事：登记任务 + 回 ACT_CON。数据在 tick() 里发。
    // =================================================================
    static bool on_interrogation(void* param, IMasterConnection con,
                                 CS101_ASDU asdu, uint8_t qoi) {
        Iec104Server* self = static_cast<Iec104Server*>(param);
        const int ca = CS101_ASDU_getCA(asdu);

        if (ca != self->cfg_.ca) {
            // 未知 CA：按规范回 UNKNOWN_CA + 否定
            ++self->st_.unknown_ca;
            CS101_ASDU_setCOT(asdu, CS101_COT_UNKNOWN_CA);
            CS101_ASDU_setNegative(asdu, true);
            IMasterConnection_sendASDU(con, asdu);
            return true;
        }
        if (qoi != IEC60870_QOI_STATION) {
            // 本项目不上送分组召唤（group 字段留着，将来按组切）
            ++self->st_.gi_rejected;
            IMasterConnection_sendACT_CON(con, asdu, true /*negative*/);
            return true;
        }

        ++self->st_.gi_requests;
        self->gi_state_   = GiState::kRunning;
        self->gi_conn_    = con;
        self->gi_oa_      = static_cast<uint8_t>(CS101_ASDU_getOA(asdu));
        self->gi_qoi_     = qoi;
        self->gi_group_   = 1;
        self->gi_mark_    = now_utc_ms();
        // 只回确认，不发数据 —— 见文件头「时序纪律 1」
        IMasterConnection_sendACT_CON(con, asdu, false);
        return true;
    }

    // =================================================================
    // 回调：对钟。库会在返回 true 后自动回 ACT_CON（手册 §6.7）
    // =================================================================
    static bool on_clock_sync(void* param, IMasterConnection con,
                              CS101_ASDU asdu, CP56Time2a new_time) {
        Iec104Server* self = static_cast<Iec104Server*>(param);
        // 调度送来的也是**北京时间**，按同一口径减偏移得到 UTC
        self->last_clock_sync_utc_ms_ = cp56time2a_local_to_utc_ms(new_time);
        char buf[40];
        format_cp56time2a_local(new_time, buf, sizeof(buf));
        self->last_clock_sync_text_ = buf;

        // ★ 顺序：**先写载荷，再递增计数**。
        //   这个回调跑在库自己的线程上，而观察者（单元测试、网管打印）
        //   跑在应用线程上，惯用写法是 `while (stats().clock_syncs < 1);`
        //   再读 last_clock_sync_text()。若计数先自增，观察者就有机会
        //   在载荷写进去之前通过检查，读到**半成品** —— 实测过，T66 会
        //   偶发断言失败，而且看起来像随机抖动，最难查的一类。
        //   所以把计数当成「我前面写的东西都好了」的发布信号来用。
        std::atomic_thread_fence(std::memory_order_release);
        ++self->st_.clock_syncs;
        (void)con; (void)asdu;
        return true;
    }

    static void on_raw(void* param, IMasterConnection con, uint8_t* msg, int size, bool send) {
        Iec104Server* self = static_cast<Iec104Server*>(param);
        // 只打前 kRawDumpLimit 帧，避免刷屏。
        // ★ 计数必须是 int，不能是 char：char 是 signed 的，到 127 就回绕成负数，
        //   于是 `> 200` 永远不成立 —— `--dump-raw` 本想"只打前 200 帧"，
        //   结果会把整场联调的所有帧全部刷出来，正好和意图相反。
        if (self->raw_dumped_++ > kRawDumpLimit) return;
        std::printf("  [104 %s] ", send ? "TX" : "RX");
        for (int i = 0; i < size && i < 32; ++i) std::printf("%02X ", msg[i]);
        if (size > 32) std::printf("... (%d B)", size);
        std::printf("\n");
        (void)con;
    }

    // =================================================================
    // 回调：兜底 —— 控制命令走这里
    // =================================================================
    static bool on_asdu(void* param, IMasterConnection con, CS101_ASDU asdu) {
        Iec104Server* self = static_cast<Iec104Server*>(param);
        const int ca      = CS101_ASDU_getCA(asdu);
        const TypeID type = CS101_ASDU_getTypeID(asdu);
        const CS101_CauseOfTransmission cot = CS101_ASDU_getCOT(asdu);

        if (ca != self->cfg_.ca) {
            ++self->st_.unknown_ca;
            CS101_ASDU_setCOT(asdu, CS101_COT_UNKNOWN_CA);
            CS101_ASDU_setNegative(asdu, true);
            IMasterConnection_sendASDU(con, asdu);
            return true;
        }
        if (cot != CS101_COT_ACTIVATION) {
            ++self->st_.unknown_cot;
            CS101_ASDU_setCOT(asdu, CS101_COT_UNKNOWN_COT);
            CS101_ASDU_setNegative(asdu, true);
            IMasterConnection_sendASDU(con, asdu);
            return true;
        }

        double value = 0.0;
        bool   select = false;
        Kind   kind;
        if (type == C_SE_NC_1) {              // 49/50 系列里我们只用浮点设点(50)
            kind = Kind::kSetpointFloat;
        } else if (type == C_SC_NA_1) {
            kind = Kind::kCommandSingle;
        } else if (type == C_DC_NA_1) {
            kind = Kind::kCommandDouble;
        } else {
            ++self->st_.illegal_type;
            return false;   // 交回库的默认处理
        }

        InformationObject io = CS101_ASDU_getElement(asdu, 0);
        if (io == nullptr) return true;

        const int ioa = InformationObject_getObjectAddress(io);
        switch (kind) {
        case Kind::kSetpointFloat:
            value  = static_cast<double>(SetpointCommandShort_getValue(
                         reinterpret_cast<SetpointCommandShort>(io)));
            select = SetpointCommandShort_isSelect(reinterpret_cast<SetpointCommandShort>(io));
            break;
        case Kind::kCommandSingle:
            value  = SingleCommand_getState(reinterpret_cast<SingleCommand>(io)) ? 1.0 : 0.0;
            select = SingleCommand_isSelect(reinterpret_cast<SingleCommand>(io));
            break;
        case Kind::kCommandDouble:
            value  = static_cast<double>(DoubleCommand_getState(
                         reinterpret_cast<DoubleCommand>(io)));
            select = DoubleCommand_isSelect(reinterpret_cast<DoubleCommand>(io));
            break;
        default: break;
        }
        InformationObject_destroy(reinterpret_cast<InformationObject>(io));

        ++self->st_.commands_rx;

        const PointDef* def = find_by_ioa(self->dn_, self->dn_n_, ioa);
        if (def == nullptr) {
            ++self->st_.unknown_ioa;
            ++self->st_.commands_rejected;
            CS101_ASDU_setCOT(asdu, CS101_COT_UNKNOWN_IOA);
            CS101_ASDU_setNegative(asdu, true);
            IMasterConnection_sendASDU(con, asdu);
            return true;
        }
        if (def->kind != kind) {
            // 类型不匹配：调度用错了 TypeID（把遥控点当遥调点发之类）
            ++self->st_.commands_rejected;
            CS101_ASDU_setCOT(asdu, CS101_COT_UNKNOWN_TYPE_ID);
            CS101_ASDU_setNegative(asdu, true);
            IMasterConnection_sendASDU(con, asdu);
            return true;
        }

        // ---- 选择/执行（SBO）状态机：选择只是「占位」，不落到 sink ----
        if (select) {
            self->select_ioa_ = ioa;
            self->select_ok_  = true;
            CS101_ASDU_setCOT(asdu, CS101_COT_ACTIVATION_CON);
            CS101_ASDU_setNegative(asdu, false);
            IMasterConnection_sendASDU(con, asdu);
            return true;
        }

        // ---- 执行：交给 sink 裁决 ----
        std::string err;
        bool accepted = (self->sink_ != nullptr)
            ? self->sink_->on_command(ioa, kind, from_protocol(*def, value), false, ca, &err)
            : false;
        if (self->sink_ == nullptr) err = "网关未装配命令出口（ICommandSink 为空）";

        if (accepted) {
            self->last_cmd_text_ = std::string(def->name) + " = " +
                                   std::to_string(value) + " " + def->unit + " 已受理";
            std::atomic_thread_fence(std::memory_order_release);
            ++self->st_.commands_accepted;   // 先载荷后计数，理由见 on_clock_sync
            CS101_ASDU_setCOT(asdu, CS101_COT_ACTIVATION_CON);
            CS101_ASDU_setNegative(asdu, false);
        } else {
            self->last_cmd_text_ = std::string(def->name) + " 被拒绝：" + err;
            std::atomic_thread_fence(std::memory_order_release);
            ++self->st_.commands_rejected;
            CS101_ASDU_setCOT(asdu, CS101_COT_ACTIVATION_CON);
            CS101_ASDU_setNegative(asdu, true);   // ★ 否定确认：调度必须能看出没生效
        }
        IMasterConnection_sendASDU(con, asdu);
        return true;
    }

    // =================================================================
    // GI 迭代器：每次 tick 推一个分组，推完发 ACT_TERM
    // =================================================================
    void serve_gi(std::uint64_t now_ms) {
        if (gi_state_ != GiState::kRunning || gi_conn_ == nullptr) return;
        // 防御：连接可能已经没了（库还没回调 CLOSED）
        if (!IMasterConnection_isReady(gi_conn_)) {
            gi_state_ = GiState::kIdle; gi_conn_ = nullptr; return;
        }
        if (gi_group_ == 1) {
            st_.gi_asdu_sent += static_cast<std::uint64_t>(
                emit(gi_conn_, CS101_COT_INTERROGATED_BY_STATION, gi_oa_,
                     false /*不带时标：规范要求 GI 用基础类型*/, all_indices(), now_ms));
            gi_group_ = 2;
            return;
        }
        // 结束：ACT_TERM，否则主站会一直等
        CS101_ASDU a = CS101_ASDU_create(al_params(), false,
                                         CS101_COT_ACTIVATION_TERMINATION,
                                         gi_oa_, cfg_.ca, false, false);
        // ★★★ 必须给这个 ASDU 装一个 C_IC_NA_1 的信息对象。
        //   ASDU 的 TypeID 由**第一个加入的信息对象**决定；空 ASDU 的
        //   TypeID 是 0 —— 主站侧 CS101_ASDU_createFromBufferEx 校验失败，
        //   整帧被丢弃 → 主站永远等不到 ACT_TERM → 总召唤卡死。
        //   官方 examples/cs104_server/simple_server.c:185 就是这么写的，
        //   但**手册 §6.4 / §6.9 的示例漏了这一步**，照抄手册会卡在总召唤。
        if (InformationObject io = reinterpret_cast<InformationObject>(
                InterrogationCommand_create(nullptr, 0, gi_qoi_))) {
            CS101_ASDU_addInformationObject(a, io);
            InformationObject_destroy(io);
            ++st_.gi_act_term_sent;
        }
        IMasterConnection_sendACT_TERM(gi_conn_, a);
        CS101_ASDU_destroy(a);
        ++st_.gi_completed;
        gi_state_ = GiState::kIdle;
        gi_conn_  = nullptr;
        (void)now_ms;
    }

    // =================================================================
    // 变化上报：值变了 **或品质跳变了** 才发
    //
    // 为什么品质跳变也要发：一个点从"好"变"坏"（比如 BMS 通信断了，
    // BMS_CHG_LIM 读不出来）本身就是最重要的信息之一。
    // 如果只在"值变化"时发，调度会一直拿着**上一次的好值**，
    // 而且因为读失败被跳过，连 INVALID 都收不到 —— 这是最危险的失联方式。
    // =================================================================
    void report_changes(std::uint64_t now_ms) {
        if (!has_connection()) return;
        std::vector<int> changed;
        for (std::size_t i = 0; i < up_n_; ++i) {
            double v = 0.0; Quality q = Quality::kInvalid;
            const bool got = src_->read(up_[i].src, &v, &q);
            const bool qgood = (q == Quality::kGood);
            const double wire = got ? to_protocol(up_[i], v) : 0.0;

            bool report = false;
            if (!got || !qgood) {
                report = (prev_ok_[i] != 0);                 // 好 → 坏 / 好 → 陈旧
            } else {
                const double span = std::fabs(wire) > 1e-9 ? std::fabs(wire) : 1.0;
                report = (prev_ok_[i] == 0) ||          // 坏 → 好
                         !prev_valid_[i] ||             // 首拍：没有基线
                         std::fabs(wire - prev_[i]) > cfg_.change_deadband * span;
            }
            if (report) changed.push_back(static_cast<int>(i));

            prev_[i]       = wire;                      // 无论发不发都记住最新值
            prev_valid_[i] = true;
            prev_ok_[i]    = (got && qgood) ? 1 : 0;
        }
        if (changed.empty()) return;
        st_.spontaneous_asdu += static_cast<std::uint64_t>(
            emit(nullptr /*走 enqueue：面向所有连接*/, CS101_COT_SPONTANEOUS, 0,
                 cfg_.use_timestamp_on_spontaneous, changed, now_ms));
    }

    // =================================================================
    // 周期全量：兜底，防止主站漏一次变化就永久失准
    // =================================================================
    void report_periodic(std::uint64_t now_ms) {
        if (!has_connection()) return;
        if (last_periodic_ms_ != 0 &&
            now_ms < last_periodic_ms_ +
                     static_cast<std::uint64_t>(cfg_.periodic_s * 1000.0)) return;
        last_periodic_ms_ = now_ms;
        st_.periodic_asdu += static_cast<std::uint64_t>(
            emit(nullptr, CS101_COT_PERIODIC, 0,
                 cfg_.use_timestamp_on_spontaneous, all_indices(), now_ms));
    }

    // =================================================================
    // 统一发送器：**按类型分桶 + 按 ASDU 容量分包**
    //
    // 两道约束缺一不可：
    //   ① 同一个 ASDU 只能装同一种 TypeID（库会拒收混装，返回 false）
    //   ② ASDU 最大 249 字节 —— 带 CP56Time2a 的短浮点每个 IO 占
    //      IOA(3)+值(4)+品质(1)+时标(7)=15 字节，头 6 字节，
    //      所以**一帧最多 16 个点**（不带时标时是 30 个）。
    //      18 个遥测点不分包就会静默丢 2 个 —— 现象是"有点没上去"。
    // -----------------------------------------------------------------
    int emit(IMasterConnection con, CS101_CauseOfTransmission cot, int oa,
             bool with_ts, const std::vector<int>& idx, std::uint64_t now_ms) {
        int frames = 0;
        for (int k = 0; k < kKindCount; ++k) {
            const Kind kk = static_cast<Kind>(k);
            if (is_control(kk)) continue;

            std::vector<int> bucket;
            for (int i : idx)
                if (up_[i].kind == kk) bucket.push_back(i);
            if (bucket.empty()) continue;

            const int per = max_ios_per_asdu(kk, with_ts);
            std::size_t pos = 0;
            while (pos < bucket.size()) {
                CS101_ASDU a = CS101_ASDU_create(al_params(), false, cot,
                                                 oa, cfg_.ca, false, false);
                int n = 0;
                for (int c = 0; c < per && pos < bucket.size(); ++c, ++pos) {
                    if (add_point(a, up_[bucket[pos]], now_ms, with_ts)) ++n;
                }
                if (n > 0) {
                    if (con != nullptr) {
                        // GI 响应绕过普通队列立即发（GI 数据不该排在自发事件后面）
                        IMasterConnection_sendASDUEx(con, a, true);
                    } else {
                        CS104_Slave_enqueueASDU(slave_, a);
                    }
                    ++frames;
                }
                CS101_ASDU_destroy(a);
            }
        }
        return frames;
    }

    // 一个 ASDU 里最多能塞几个这种类型的 IO（按当前应用层参数算）
    int max_ios_per_asdu(Kind k, bool with_ts) {
        CS101_AppLayerParameters ap = al_params();
        const int header  = 2 + ap->sizeOfCOT + ap->sizeOfCA;
        const int maxAsdu = (ap->maxSizeOfASDU > 0) ? ap->maxSizeOfASDU : 249;
        int val = 4;
        switch (k) {
        case Kind::kMeasFloat:      val = 4; break;
        case Kind::kMeasNormalized: val = 2; break;
        case Kind::kMeasScaled:     val = 2; break;
        case Kind::kSinglePoint:    val = 1; break;
        case Kind::kDoublePoint:    val = 1; break;
        case Kind::kCounter:        val = 5; break;
        default:                    val = 4; break;
        }
        const int per_io = ap->sizeOfIOA + val + 1 + (with_ts ? 7 : 0);
        const int n = (maxAsdu - header) / per_io;
        return n < 1 ? 1 : n;
    }

    std::vector<int> all_indices() const {
        std::vector<int> v(up_n_);
        for (std::size_t i = 0; i < up_n_; ++i) v[i] = static_cast<int>(i);
        return v;
    }

    // -----------------------------------------------------------------
    // 加一个信息对象。返回是否成功。
    // 内存：addInformationObject 内部**复制**，我们 create 出来的 io 要自己 destroy。
    // -----------------------------------------------------------------
    bool add_point(CS101_ASDU a, const PointDef& d, std::uint64_t now_ms, bool with_ts) {
        double raw = 0.0;
        Quality q = Quality::kInvalid;
        const bool got = src_->read(d.src, &raw, &q);
        if (!got) ++st_.points_invalid;
        else if (q == Quality::kNotTopical) ++st_.points_not_topical;

        const double wire = to_protocol(d, raw);

        // 三态 → IEC104 品质字节。GOOD=0x00 是**唯一**的"可用"，其余都是异常，
        // 所以判据写成 switch 而不是 if：新增一态时编译器会直接报出来。
        QualityDescriptor qd = IEC60870_QUALITY_GOOD;
        switch (q) {
        case Quality::kGood:       qd = IEC60870_QUALITY_GOOD;        break;
        case Quality::kNotTopical: qd = IEC60870_QUALITY_NON_TOPICAL; break;
        case Quality::kInvalid:    qd = IEC60870_QUALITY_INVALID;     break;
        }
        if (!got) qd = IEC60870_QUALITY_INVALID;   // 没值一定是 IV，压倒源侧的回答

        InformationObject io = nullptr;
        switch (d.kind) {
        case Kind::kMeasFloat: {
            const float f = static_cast<float>(wire);
            io = with_ts
                ? reinterpret_cast<InformationObject>(
                      MeasuredValueShortWithCP56Time2a_create(nullptr, d.ioa, f, qd, ts_of(now_ms)))
                : reinterpret_cast<InformationObject>(
                      MeasuredValueShort_create(nullptr, d.ioa, f, qd));
            break;
        }
        case Kind::kMeasScaled: {
            const int v = static_cast<int>(wire);
            io = with_ts
                ? reinterpret_cast<InformationObject>(
                      MeasuredValueScaledWithCP56Time2a_create(nullptr, d.ioa, v, qd, ts_of(now_ms)))
                : reinterpret_cast<InformationObject>(
                      MeasuredValueScaled_create(nullptr, d.ioa, v, qd));
            break;
        }
        case Kind::kSinglePoint: {
            const bool b = (wire > 0.5);
            io = with_ts
                ? reinterpret_cast<InformationObject>(
                      SinglePointWithCP56Time2a_create(nullptr, d.ioa, b, qd, ts_of(now_ms)))
                : reinterpret_cast<InformationObject>(
                      SinglePointInformation_create(nullptr, d.ioa, b, qd));
            break;
        }
        default:
            break;   // 其余类型本轮不用（累计量需要 BCR，见 docs/design.md §7）
        }
        if (io == nullptr) return false;

        const bool ok = CS101_ASDU_addInformationObject(a, io);
        InformationObject_destroy(io);
        if (ok) ++st_.points_uploaded;
        return ok;
    }

    // 时标是栈对象，生命周期只到本次 create 调用（库会复制进去）
    CP56Time2a ts_of(std::uint64_t now_ms) {
        make_local_cp56time2a(&ts_buf_, now_ms);
        return &ts_buf_;
    }

    CS101_AppLayerParameters al_params() {
        return CS104_Slave_getAppLayerParameters(slave_);
    }

    bool has_connection() const {
        return slave_ != nullptr && CS104_Slave_getOpenConnections(slave_) > 0;
    }

private:
    static constexpr int kKindCount = 9;    // Kind 枚举项数（见 iec104_point_map.h）
    static constexpr int kRawDumpLimit = 200;  // --dump-raw 最多打多少帧

    enum class GiState { kIdle, kRunning };

    ServerConfig   cfg_;
    IDataSource*   src_   = nullptr;
    ICommandSink*  sink_  = nullptr;
    CS104_Slave    slave_ = nullptr;
    bool           running_ = false;

    const PointDef* up_ = nullptr;   std::size_t up_n_ = 0;
    const PointDef* dn_ = nullptr;   std::size_t dn_n_ = 0;

    std::vector<double> prev_;
    std::vector<char>   prev_valid_;
    std::vector<char>   prev_ok_;      // 上一拍这个点是 GOOD 吗（值/品质跳变检测）

    // GI 会话状态：**单冗余组模式下只有一个活动连接**，所以用单变量是安全的。
    // 换成 CS104_MODE_MULTIPLE_REDUNDANCY_GROUPS 就必须按 connection 分桶
    // —— 手册 §11「多连接要点」点名的坑，见 docs/README.md §6。
    GiState         gi_state_ = GiState::kIdle;
    IMasterConnection gi_conn_ = nullptr;
    std::uint8_t    gi_oa_    = 0;
    std::uint8_t    gi_qoi_   = 20;    // 记住请求的 QOI，ACT_TERM 的 IO 要用
    int             gi_group_ = 0;
    std::uint64_t   gi_mark_  = 0;

    int  select_ioa_ = -1;
    bool select_ok_  = false;

    std::uint64_t last_periodic_ms_ = 0;
    struct sCP56Time2a ts_buf_;

    Stats st_;
    char  peer_[64] = {0};
    int   raw_dumped_ = 0;      // int，别用 char（见 on_raw 的说明）

    std::uint64_t last_clock_sync_utc_ms_ = 0;
    std::string   last_clock_sync_text_;
    std::string   last_cmd_text_;

public:
    // ---- 供外部（日志 / 报告 / 测试）只读观察 ----
    std::uint64_t last_clock_sync_utc_ms() const { return last_clock_sync_utc_ms_; }
    const std::string& last_clock_sync_text() const { return last_clock_sync_text_; }
    const std::string& last_command_text() const { return last_cmd_text_; }
    const char* peer() const { return peer_; }
    CS104_Slave handle() const { return slave_; }
};

} // namespace iec104

#endif // EMS_P3_IEC104_SERVER_H
