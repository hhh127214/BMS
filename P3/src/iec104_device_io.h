// =====================================================================
// P3/ — 产品化 P3（通信）：IEC 60870-5-104 适配器 + 被控站仿真
//
// 定位：IDeviceIO 的**对外通信**适配器。与 ModbusDeviceIO（EMS ↔ 设备侧
//       PCS/BMS/电表）**方向相反**：Iec104DeviceIO 是 EMS 作为**可控站**
//       与调度/云端主站对话。
//
// ★ 一个容易被忽略的事实：Iec104DeviceIO 仍然只需要实现 IDeviceIO。
//   对本项目的算法（05/06/07/08）而言，"从调度主站拿计划/拿限值"与
//   "从 Modbus 拿设备量测"是同一件事 —— 都是"读量测 / 读限值 / 写指令"。
//   这正是 P0 分层的价值：四个适配器（Sim / Memory / RtDb / Modbus/Iec104）
//   可以任意替换，算法一行不改。
//
// ★ 与 Modbus 适配器共用的三条纪律（跨介质不变）：
//   1. 点名 / IOA / 类型标识**只允许出现在本文件与从站仿真内**；
//   2. execute() 的返回值不可信，闭环走下一拍 read_snapshot()；
//   3. 采集失败填最近一次有效值，可信性由 read_status().data_valid 表达。
//
// ★ MPI 方向与设备归属（谁写哪个信息体，现场就是靠这个划分职责）：
//   从站（调度侧）→ EMS：MEAS 遥测 / CFG 参数与限值 / STA 状态     COT=1 / 20
//   EMS → 从站（调度侧）：CMD 遥调（功率设定值，C_SE_NC_1，带 S/E）  COT=6
//
// ★ 关于"宽精度私有类型"（Iec104Profile::kWidePrivate）：
//   标准集里没有 64 bit 浮点，M_ME_NC_1 只有 24 位有效位。逐位等价验收
//   需要无损通道，因此本文件提供 type 200（IOA + IEEE754 double + QDS）。
//   现场部署一律用 kStandard（type 13），宽精度通道只用于验收 ——
//   这样"代码写错了"与"精度不够"才能分开定位（同 Modbus 的 T34 思路）。
//
// 编译：纯头文件。include 路径需含 04/src、07/src、07/src/rtdb。
// =====================================================================

#pragma once

#include "device_io.h"        // 04/  IDeviceIO
#include "ems_point_table.h"  // 07/src/rtdb  30 点真相源
#include "iec104_codec.h"     // 同目录：APCI/ASDU 编解码（纯函数）
#include "memory_device_io.h" // 07/src  设备替身（仅**从站仿真**使用）

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace ems {

using iec104::AsduHeader;
using iec104::AsduView;
using iec104::ApciHeader;
using iec104::InfoFloat;
using iec104::InfoSingle;

// =====================================================================
// 信息体地址（IOA）分配 + 点映射表
//
// 为什么按"段"分配（0x4x01 起）：104 的 IOA 是 3 字节，现场习惯按信息体
// 类别分段便于用抓包工具阅读。本项目的四段与 Modbus 的四个块一一对应，
// 于是"同一份点表、两种介质"是可逐点对照的。
// =====================================================================
static constexpr uint32_t kIoaMeasBase = 0x004001;   // 遥测（MEAS）
static constexpr uint32_t kIoaCfgBase  = 0x004101;   // 参数/限值（CFG）
static constexpr uint32_t kIoaStaBase  = 0x004201;   // 状态（STA）
static constexpr uint32_t kIoaCmdBase  = 0x004301;   // 遥调（CMD）

enum class Iec104Profile {
    kStandard,     // 现场：M_ME_NC_1(13) 短浮点
    kWidePrivate,  // 验收：M_ME_WIDE(200) 双精度（本项目私有）
};

struct Iec104Point {
    const char* point  = nullptr;
    uint32_t    ioa    = 0;
    uint8_t     type_id = iec104::kM_ME_NC_1;
    int         group  = 0;   // 0=MEAS 1=CFG 2=STA 3=CMD
};

// 点后缀固定归属某个段 —— 与 Modbus 的块划分是同一套语义
inline int iec104_point_group(int index) {
    if (index <= EMS_SOH)         return 0;
    if (index <= EMS_CMD_P_LOWER) return 3;
    if (index <= EMS_CFG_SOC_MAX) return 1;
    return 2;
}

struct Iec104PointMap {
    static constexpr int kPointCount = 30;

    Iec104Point pts[kPointCount] = {};
    uint8_t     meas_type_id = iec104::kM_ME_NC_1;   // 量测用类型（随 profile）

    uint32_t base_of_group(int g) const {
        switch (g) {
            case 0: return kIoaMeasBase;
            case 1: return kIoaCfgBase;
            case 2: return kIoaStaBase;
            default: return kIoaCmdBase;
        }
    }
    int count_of_group(int g) const {
        int n = 0;
        for (int i = 0; i < kPointCount; ++i) {
            if (pts[i].group == g) ++n;
        }
        return n;
    }
    int first_index_of_group(int g) const {
        for (int i = 0; i < kPointCount; ++i) {
            if (pts[i].group == g) return i;
        }
        return -1;
    }
    int index_of_point(const char* name) const {
        if (name == nullptr) return -1;
        for (int i = 0; i < kPointCount; ++i) {
            if (pts[i].point != nullptr && std::strcmp(pts[i].point, name) == 0) return i;
        }
        return -1;
    }
    const Iec104Point* find_point(const char* name) const {
        const int i = index_of_point(name);
        return (i < 0) ? nullptr : &pts[i];
    }
    int index_of_ioa(uint32_t ioa) const {
        for (int i = 0; i < kPointCount; ++i) {
            if (pts[i].ioa == ioa) return i;
        }
        return -1;
    }
};

inline Iec104PointMap build_iec104_map(Iec104Profile profile) {
    Iec104PointMap m;
    m.meas_type_id = (profile == Iec104Profile::kWidePrivate) ? iec104::kM_ME_WIDE
                                                              : iec104::kM_ME_NC_1;
    uint32_t next[4] = {kIoaMeasBase, kIoaCfgBase, kIoaStaBase, kIoaCmdBase};
    for (int i = 0; i < Iec104PointMap::kPointCount; ++i) {
        const int g = iec104_point_group(i);
        m.pts[i].point = EMS_POINT_NAMES[i];
        m.pts[i].ioa   = next[g]++;
        m.pts[i].group = g;
        m.pts[i].type_id = (g == 0 || g == 1) ? m.meas_type_id      // 遥测/参数 → 浮点
                          : (g == 2)           ? iec104::kM_SP_NA_1 // 状态 → 单点
                                               : iec104::kC_SE_NC_1;// 遥调 → 设定值
    }
    return m;
}

static_assert(Iec104PointMap::kPointCount == EMS_POINT_COUNT,
              "IEC104 点映射表点数必须与 ems_point_table.h 的 EMS_POINT_COUNT 一致");

// =====================================================================
// 传输层抽象：只负责"把字节发出去、把字节收回来"
//
// 与 Modbus 的关键差别：104 是**字节流 + 由从站主动上送**，没有
// "请求-响应"配对。因此接口是 send / receive 两个方向，而不是 transact。
// =====================================================================
class IIec104Transport {
public:
    virtual ~IIec104Transport() = default;

    virtual bool connect() = 0;
    virtual void close()    = 0;
    virtual bool connected() const = 0;
    virtual bool send(const uint8_t* buf, size_t len) = 0;
    // 接收。返回 false = 链路错误；返回 true 且 *len == 0 = "这一轮没有报文"。
    virtual bool receive(uint8_t* buf, size_t cap, size_t* len, int timeout_ms) = 0;
};

// =====================================================================
// 被控站仿真 —— 真实系统里这是**调度主站/网关**（另一台机器）
//
// 它自己有一台"设备"（MemoryDeviceIO）。信息对象在**上送时刻**由设备当前值
// 编码而成 —— 104 没有 Modbus 那种寄存器层，遥测是"编码出来"的。
// =====================================================================
class Iec104ControlledStationSim {
public:
    static constexpr size_t kOutCap = 8192;
    static constexpr size_t kInCap  = 4096;

    Iec104ControlledStationSim() : map_(build_iec104_map(Iec104Profile::kStandard)) {}

    explicit Iec104ControlledStationSim(Iec104Profile profile, uint16_t ca = 1)
        : map_(build_iec104_map(profile)), ca_(ca) {}

    void set_profile(Iec104Profile p) { map_ = build_iec104_map(p); }
    void set_common_address(uint16_t ca) { ca_ = ca; }
    uint16_t common_address() const { return ca_; }
    const Iec104PointMap& map() const { return map_; }

    void attach_device(MemoryDeviceIO* dev) { dev_ = dev; }
    MemoryDeviceIO* device() const { return dev_; }

    // 周期上送开关（现场由主站的"循环传送"参数控制）
    void set_cyclic(bool on) { cyclic_ = on; }
    bool cyclic() const { return cyclic_; }

    // -----------------------------------------------------------------
    // 字节入口 / 出口（传输层调用）
    // -----------------------------------------------------------------
    void feed(const uint8_t* buf, size_t len) {
        if (buf == nullptr) return;
        for (size_t i = 0; i < len && in_len_ < kInCap; ++i) in_[in_len_++] = buf[i];
        process();
    }
    size_t take(uint8_t* out, size_t cap) {
        const size_t n = std::min(cap, out_len_);
        if (n > 0) std::memcpy(out, out_, n);
        if (n < out_len_) std::memmove(out_, out_ + n, out_len_ - n);
        out_len_ -= n;
        return n;
    }

    // -----------------------------------------------------------------
    // 诊断
    // -----------------------------------------------------------------
    bool     started()      const { return started_; }
    uint16_t vs()           const { return vs_; }        // 本站下一个 I 帧的 N(S)
    uint16_t vr()           const { return vr_; }        // 期望对方下一个 N(S)
    int      i_frames_rx()  const { return i_rx_; }
    int      i_frames_tx()  const { return i_tx_; }
    int      u_frames_rx()  const { return u_rx_; }
    int      s_frames_rx()  const { return s_rx_; }
    int      gi_count()     const { return gi_count_; }
    int      setpoint_count() const { return sp_rx_; }
    int      bad_ca()       const { return bad_ca_; }
    int      unknown_type() const { return unknown_type_; }
    int      malformed()    const { return malformed_; }
    int      cyclic_bursts() const { return cyclic_bursts_; }
    int      queue_overflows() const { return queue_overflows_; }

private:
    // ---------------- 出方向 ----------------
    // 空间不足时**整帧丢弃**，绝不半帧入队 —— 否则接收侧会把"半个旧帧 + 新帧"
    // 拼成一个语法成立但内容错乱的帧，这种错最难查（长度字段自洽）。
    void queue(const uint8_t* buf, size_t len) {
        if (out_len_ + len > kOutCap) { ++queue_overflows_; return; }
        std::memcpy(out_ + out_len_, buf, len);
        out_len_ += len;
    }
    void queue_u(uint8_t ufunc) {
        uint8_t f[8];
        const size_t n = iec104::wrap_u_frame(f, sizeof(f), ufunc);
        if (n > 0) queue(f, n);
    }
    void queue_i(const uint8_t* asdu, size_t asdu_len) {
        uint8_t f[512];
        const size_t n = iec104::wrap_i_frame(f, sizeof(f), vs_, vr_, asdu, asdu_len);
        if (n == 0) return;
        queue(f, n);
        vs_ = static_cast<uint16_t>((vs_ + 1) & 0x7FFF);
        ++i_tx_;
    }

    // 上送一段遥测（MEAS / CFG）：SQ=1，IOA 连续
    void queue_measure_group(int group, uint8_t cot) {
        if (dev_ == nullptr) return;
        const uint32_t base = map_.base_of_group(group);
        const int n = map_.count_of_group(group);
        if (n <= 0 || n > 16) return;
        InfoFloat items[16];
        for (int k = 0; k < n; ++k) {
            items[k].ioa   = base + static_cast<uint32_t>(k);
            const int idx  = map_.index_of_ioa(items[k].ioa);
            items[k].value = (idx >= 0) ? dev_->get(EMS_POINT_NAMES[idx]) : 0.0;
            items[k].qds   = iec104::kQdsGood;
        }
        uint8_t asdu[512];
        const size_t an = (map_.meas_type_id == iec104::kM_ME_WIDE)
                              ? iec104::make_asdu_wide(asdu, sizeof(asdu), cot, ca_, items, n)
                              : iec104::make_asdu_measurements(asdu, sizeof(asdu), cot, ca_, items, n);
        if (an > 0) queue_i(asdu, an);
    }

    // 上送状态段（STA）：M_SP_NA_1，SQ=1
    void queue_status_group(uint8_t cot) {
        if (dev_ == nullptr) return;
        const uint32_t base = map_.base_of_group(2);
        const int n = map_.count_of_group(2);
        if (n <= 0 || n > 16) return;
        InfoSingle items[16];
        for (int k = 0; k < n; ++k) {
            items[k].ioa   = base + static_cast<uint32_t>(k);
            const int idx  = map_.index_of_ioa(items[k].ioa);
            items[k].value = (idx >= 0) && (dev_->get(EMS_POINT_NAMES[idx]) > 0.5);
            items[k].qds   = iec104::kQdsGood;
        }
        uint8_t asdu[256];
        const size_t an = iec104::make_asdu_single_points(asdu, sizeof(asdu), cot, ca_, items, n);
        if (an > 0) queue_i(asdu, an);
    }

    // 一次周期上送：遥测 + 参数 + 状态（COT = 周期）
    void queue_cyclic_burst(uint8_t cot) {
        queue_measure_group(0, cot);
        queue_measure_group(1, cot);
        queue_status_group(cot);
        ++cyclic_bursts_;
    }

    // ---------------- 入方向 ----------------
    void process() {
        size_t off = 0;
        bool gi_this_round = false;
        while (off < in_len_) {
            ApciHeader h;
            const size_t n = iec104::parse_apdu(in_ + off, in_len_ - off, h);
            if (n == 0) break;                     // 半个帧：留着等下一批字节
            gi_this_round = handle_frame(in_ + off, n, h) || gi_this_round;
            off += n;
        }
        if (off > 0) {
            std::memmove(in_, in_ + off, in_len_ - off);
            in_len_ -= off;
        }
        // 处理完本轮输入后按周期上送（总召那一轮已经用 COT=20 全量上送过，不重复）
        if (started_ && cyclic_ && !gi_this_round) queue_cyclic_burst(iec104::kCOT_Per);
    }

    // 返回 true 表示本轮处理了总召唤
    bool handle_frame(const uint8_t* apdu, size_t len, const ApciHeader& h) {
        switch (h.kind) {
            case iec104::kFrameU:
                ++u_rx_;
                if (h.ufunc == iec104::kStartDtAct) {
                    queue_u(iec104::kStartDtCon);
                    started_ = true;
                    // 初始化结束（类型 70）：现场 104 链路建立的标志之一
                    uint8_t asdu[32];
                    const size_t an = iec104::make_asdu_init_end(asdu, sizeof(asdu), ca_);
                    if (an > 0) queue_i(asdu, an);
                } else if (h.ufunc == iec104::kStopDtAct) {
                    queue_u(iec104::kStopDtCon);
                    started_ = false;
                } else if (h.ufunc == iec104::kTestFrAct) {
                    queue_u(iec104::kTestFrCon);
                }
                return false;

            case iec104::kFrameS:
                ++s_rx_;
                return false;

            case iec104::kFrameI: {
                ++i_rx_;
                vr_ = static_cast<uint16_t>((h.ns + 1) & 0x7FFF);

                AsduView v;
                if (!iec104::parse_asdu(apdu, len, h, v)) { ++malformed_; return false; }
                if (v.h.ca != ca_) { ++bad_ca_; return false; }
                if (!iec104::asdu_length_ok(v)) { ++malformed_; return false; }

                switch (v.h.type_id) {
                    case iec104::kC_IC_NA_1: {
                        uint8_t qoi = 0;
                        if (!iec104::decode_general_interrogation(v, &qoi)) { ++malformed_; return false; }
                        ++gi_count_;
                        // ① 激活确认 → ② 全量数据（COT=响应站召唤） → ③ 激活终止
                        uint8_t asdu[32];
                        size_t an = iec104::make_asdu_general_interrogation(
                            asdu, sizeof(asdu), iec104::kCOT_ActCon, ca_, qoi);
                        if (an > 0) queue_i(asdu, an);
                        queue_cyclic_burst(iec104::kCOT_Introgen);
                        an = iec104::make_asdu_general_interrogation(
                            asdu, sizeof(asdu), iec104::kCOT_ActTerm, ca_, qoi);
                        if (an > 0) queue_i(asdu, an);
                        return true;
                    }
                    case iec104::kC_SE_NC_1:
                    case iec104::kC_SC_NA_1:
                    case iec104::kC_DC_NA_1: {
                        uint32_t ioa = 0;
                        double   val = 0.0;
                        uint8_t  qos = 0;
                        if (!iec104::decode_setpoint_float(v, &ioa, &val, &qos)) { ++malformed_; return false; }
                        ++sp_rx_;
                        // 设备侧读回指令：只有 CMD 段的**设定值**才落到设备点表
                        const int idx = map_.index_of_ioa(ioa);
                        if (v.h.type_id == iec104::kC_SE_NC_1 && idx >= 0 &&
                            map_.pts[idx].group == 3 && dev_ != nullptr) {
                            dev_->set(EMS_POINT_NAMES[idx], val);
                        }
                        // 激活确认（回同一 IOA / 同一类型）
                        uint8_t asdu[64];
                        size_t an = 0;
                        if (v.h.type_id == iec104::kC_SE_NC_1) {
                            an = iec104::make_asdu_setpoint_float(asdu, sizeof(asdu),
                                                                  iec104::kCOT_ActCon, ca_, ioa, val, qos);
                        } else if (v.h.type_id == iec104::kC_SC_NA_1) {
                            an = iec104::make_asdu_single_command(asdu, sizeof(asdu),
                                                                  iec104::kCOT_ActCon, ca_, ioa,
                                                                  (qos & 0x01) != 0, false);
                        } else {
                            an = iec104::make_asdu_double_command(asdu, sizeof(asdu),
                                                                  iec104::kCOT_ActCon, ca_, ioa,
                                                                  static_cast<uint8_t>(qos & 0x03), false);
                        }
                        if (an > 0) queue_i(asdu, an);
                        return false;
                    }
                    default:
                        ++unknown_type_;
                        return false;
                }
            }
        }
        return false;
    }

    Iec104PointMap   map_;
    uint16_t         ca_ = 1;
    MemoryDeviceIO*  dev_ = nullptr;
    bool             started_ = false;
    bool             cyclic_  = true;

    uint16_t vs_ = 0;      // 本站 N(S)
    uint16_t vr_ = 0;      // 本站 N(R)（= 期望对方下一个 N(S)）

    uint8_t  in_[kInCap]   = {};
    size_t   in_len_       = 0;
    uint8_t  out_[kOutCap] = {};
    size_t   out_len_      = 0;

    int i_rx_ = 0, i_tx_ = 0, u_rx_ = 0, s_rx_ = 0;
    int gi_count_ = 0, sp_rx_ = 0, bad_ca_ = 0, unknown_type_ = 0, malformed_ = 0;
    int cyclic_bursts_ = 0;
    int queue_overflows_ = 0;
};

// =====================================================================
// 环回传输：把"EMS ↔ 调度主站"接在本进程内
// =====================================================================
class LoopbackIec104Transport : public IIec104Transport {
public:
    LoopbackIec104Transport() = default;
    explicit LoopbackIec104Transport(Iec104ControlledStationSim* station) : station_(station) {}

    void attach(Iec104ControlledStationSim* station) { station_ = station; }
    Iec104ControlledStationSim* station() const { return station_; }

    bool connect() override { up_ = (station_ != nullptr); return up_; }
    void close()   override { up_ = false; }
    bool connected() const override { return up_; }

    void set_link_up(bool u) { up_ = u; }
    // 故障注入：接下来 n 次 receive 收不到任何字节（对端"哑了"）
    void inject_silence(int n) { silence_budget_ = n; }

    int send_failures() const { return send_fail_; }
    int link_errors() const { return link_errors_; }
    int bytes_tx() const { return bytes_tx_; }
    int bytes_rx() const { return bytes_rx_; }
    int frames_tx() const { return frames_tx_; }
    int frames_rx() const { return frames_rx_; }
    int silent_reads() const { return silent_reads_; }

    bool send(const uint8_t* buf, size_t len) override {
        if (!up_ || station_ == nullptr) { ++send_fail_; return false; }
        station_->feed(buf, len);
        ++frames_tx_;
        bytes_tx_ += static_cast<int>(len);
        return true;
    }

    bool receive(uint8_t* buf, size_t cap, size_t* len, int /*timeout_ms*/) override {
        if (len != nullptr) *len = 0;
        if (!up_ || station_ == nullptr) { ++link_errors_; return false; }
        if (silence_budget_ > 0) { --silence_budget_; ++silent_reads_; return true; }
        const size_t n = station_->take(buf, cap);
        if (len != nullptr) *len = n;
        if (n > 0) { ++frames_rx_; bytes_rx_ += static_cast<int>(n); }
        return true;
    }

private:
    Iec104ControlledStationSim* station_ = nullptr;
    bool up_ = false;
    int  silence_budget_ = 0;
    int  send_fail_ = 0;
    int  link_errors_ = 0;
    int  bytes_tx_ = 0;
    int  bytes_rx_ = 0;
    int  frames_tx_ = 0;
    int  frames_rx_ = 0;
    int  silent_reads_ = 0;
};

// =====================================================================
// 可控站适配器：IEC 104 → IDeviceIO
// =====================================================================
class Iec104DeviceIO : public IDeviceIO {
public:
    using DevicePump = std::function<void(double p_cmd_kw, double dt_s)>;

    static constexpr size_t kTxCap = 512;
    static constexpr size_t kRxCap = 8192;

    Iec104DeviceIO()
        : map_(build_iec104_map(Iec104Profile::kStandard)) { reset_cache(); }

    Iec104DeviceIO(IIec104Transport* t, Iec104Profile profile = Iec104Profile::kStandard,
                   uint16_t common_address = 1)
        : transport_(t), map_(build_iec104_map(profile)), ca_(common_address) {
        reset_cache();
    }

    ~Iec104DeviceIO() override = default;

    // -----------------------------------------------------------------
    // 装配
    // -----------------------------------------------------------------
    void attach_transport(IIec104Transport* t) { transport_ = t; }
    void set_profile(Iec104Profile p) { map_ = build_iec104_map(p); reset_cache(); }
    void set_map(const Iec104PointMap& m) { map_ = m; reset_cache(); }
    void set_common_address(uint16_t ca) { ca_ = ca; }
    const Iec104PointMap& map() const { return map_; }
    uint16_t common_address() const { return ca_; }
    void set_device_pump(DevicePump p) { pump_ = std::move(p); }

    // 周期上送由从站自己的定时器负责时，把保活关掉（poll 只做接收）
    void set_poll_keepalive(bool on) { poll_keepalive_ = on; }
    bool poll_keepalive() const { return poll_keepalive_; }
    void set_heartbeat_polls(int n) { heartbeat_polls_ = (n < 1) ? 1 : n; }
    // 数据陈旧看门狗：连续 n 轮 poll 收不到新的 I 帧就把量测判为不可信。
    // 现场对应 104 的 t3（长时间无数据）判据 —— 链路还在、对端"哑了"时，
    // 光看 connected() 是发现不了的。0 = 关闭（默认，与其它适配器一致）。
    void set_stale_after_polls(int n) { stale_after_polls_ = (n < 0) ? 0 : n; }
    int  polls_without_data() const { return polls_without_data_; }

    // -----------------------------------------------------------------
    // 会话
    // -----------------------------------------------------------------
    // 建链：connect → STARTDT → 初始化结束 → 总召唤（拿全量遥测/参数/状态）
    bool open() {
        if (transport_ == nullptr) return false;
        reset_session();
        if (!transport_->connect()) return false;

        uint8_t f[16];
        size_t n = iec104::wrap_u_frame(f, sizeof(f), iec104::kStartDtAct);
        if (n == 0 || !send_bytes(f, n)) return false;
        ++u_tx_;
        drain_rx(0);
        if (!startdt_ok_) return false;                 // 从站没确认启动 → 链路不可用

        uint8_t asdu[32];
        const size_t an = iec104::make_asdu_general_interrogation(
            asdu, sizeof(asdu), iec104::kCOT_Act, ca_);
        if (an == 0 || !send_i_frame(asdu, an)) return false;
        drain_rx(0);
        return gi_done_;
    }

    void close() { if (transport_ != nullptr) transport_->close(); }
    bool is_open() const { return transport_ != nullptr && transport_->connected(); }

    bool startdt_ok() const { return startdt_ok_; }
    bool gi_done() const { return gi_done_; }
    uint16_t vs() const { return vs_; }
    uint16_t vr() const { return vr_; }
    int unacked_tx() const { return (vs_ - peer_ack_) & 0x7FFF; }

    // 自检：映射表 ↔ 点名真相源 ↔ IOA 区间三者一致。返回不一致点数，0 = 全对。
    int self_check() const {
        int bad = 0;
        for (int i = 0; i < Iec104PointMap::kPointCount; ++i) {
            const Iec104Point& p = map_.pts[i];
            if (p.point == nullptr || std::strcmp(p.point, EMS_POINT_NAMES[i]) != 0) { ++bad; continue; }
            const int expect_group = iec104_point_group(i);
            if (p.group != expect_group) { ++bad; continue; }
            const uint32_t expect_ioa = map_.base_of_group(expect_group) +
                                        static_cast<uint32_t>(i - map_.first_index_of_group(expect_group));
            if (p.ioa != expect_ioa) { ++bad; continue; }
            const uint8_t expect_type =
                (expect_group == 0 || expect_group == 1) ? map_.meas_type_id
                : (expect_group == 2)                    ? iec104::kM_SP_NA_1
                                                         : iec104::kC_SE_NC_1;
            if (p.type_id != expect_type) { ++bad; continue; }
            if (map_.index_of_ioa(p.ioa) != i) ++bad;
        }
        return bad;
    }

    // -----------------------------------------------------------------
    // 诊断
    // -----------------------------------------------------------------
    int stale_reads() const { return stale_reads_; }
    int i_frames_rx() const { return i_rx_; }
    int i_frames_tx() const { return i_tx_; }
    int s_frames_tx() const { return s_tx_; }
    int s_frames_rx() const { return s_rx_; }
    int u_frames_tx() const { return u_tx_; }
    int u_frames_rx() const { return u_rx_; }
    int testfr_tx() const { return testfr_tx_; }
    int testfr_rx() const { return testfr_rx_; }
    int gi_actcon() const { return gi_actcon_; }
    int gi_actterm() const { return gi_actterm_; }
    int init_end() const { return init_end_; }
    int setpoint_actcon() const { return sp_actcon_; }
    int setpoint_echo() const { return sp_echo_; }
    int malformed() const { return malformed_; }
    int bad_ca() const { return bad_ca_; }
    int unknown_type() const { return unknown_type_; }
    int window_stalls() const { return window_stalls_; }
    int poll_ticks() const { return poll_ticks_; }
    int link_errors() const { return link_errors_; }
    int send_errors() const { return send_errors_; }

    double cached_value(int index) const {
        return (index >= 0 && index < Iec104PointMap::kPointCount) ? cache_[index] : 0.0;
    }

    // -----------------------------------------------------------------
    // 会话驱动：发送保活/确认 → 接收 → 按 w 窗口确认
    //
    // 返回 true 表示这一轮**收到了新的 I 帧**（有新数据）。
    // 为什么 poll 里要发东西：104 的从站通常按自己的定时器上送；在没有定时器
    // 的环回装置里，用一个合法的 S 帧 / TESTFR 当"拍点"来换取这一轮的上送。
    // 现场对接自定时从站时 set_poll_keepalive(false) 即可（poll 只做接收）。
    // -----------------------------------------------------------------
    bool poll(int timeout_ms = 0) const {
        if (transport_ == nullptr || !transport_->connected()) {
            for (int i = 0; i <= static_cast<int>(EMS_SOH); ++i) quality_[i] = false;
            ++stale_reads_;
            return false;
        }
        if (poll_keepalive_) send_keepalive();
        const int before = i_rx_;
        drain_rx(timeout_ms);
        const bool got = (i_rx_ > before);

        // 数据陈旧看门狗（t3 判据）：链路在但长期没有新数据，同样不可信
        if (got) polls_without_data_ = 0;
        else     ++polls_without_data_;
        if (stale_after_polls_ > 0 && polls_without_data_ > stale_after_polls_) {
            for (int i = 0; i <= static_cast<int>(EMS_SOH); ++i) quality_[i] = false;
            ++stale_reads_;
        }

        if (rx_since_ack_ >= iec104::kWindowW) send_s_frame();   // w 侧：收够 w 帧必须确认
        return got;
    }

    // -----------------------------------------------------------------
    // IDeviceIO 实现
    // -----------------------------------------------------------------
    bool read_snapshot(Timestamp now, RealtimeSnapshot& out) override {
        poll(0);

        const double p_bat = cache_[EMS_P_BAT];
        const double p_pv  = cache_[EMS_P_PV];
        const double p_load = std::max(0.0, cache_[EMS_P_LOAD] + cache_[EMS_CFG_STANDBY]);

        out.timestamp       = now;
        out.p_bat_actual_kw = p_bat;
        out.p_pv_kw         = p_pv;
        out.p_load_kw       = p_load;
        out.p_grid_kw       = p_load - p_pv - p_bat;
        out.soc             = clamp01(cache_[EMS_SOC]);
        out.temperature_c   = cache_[EMS_T_C];
        out.soh             = cache_[EMS_SOH];
        out.has_lookahead   = false;   // 与其它三个适配器保持一致

        const bool offline = cache_[EMS_STA_OFFLINE] > 0.5;
        out.meters_alive["BMS"]   = cache_[EMS_STA_BMS]   > 0.5 && !offline;
        out.meters_alive["METER"] = cache_[EMS_STA_METER] > 0.5 && !offline;
        out.meters_alive["PCS"]   = cache_[EMS_STA_PCS]   > 0.5 && !offline;

        const double h = std::fmod(now, 86400.0) / 3600.0;
        const bool valley = (h < 8.0 || h >= 22.0);
        out.pricing.cur_tou_type  = valley ? TouType::kValley : TouType::kPeak;
        out.pricing.cur_tou_price = valley ? 0.30 : 0.90;

        return data_valid();
    }

    bool read_limits(DeviceLimits& out) override {
        poll(0);
        out = DeviceLimits{};
        out.pcs_rated_chg_kw        = cache_[EMS_CFG_MAX_CHG];
        out.pcs_rated_dis_kw        = cache_[EMS_CFG_MAX_DIS];
        out.bms_chg_limit_kw        = cache_[EMS_CFG_BMS_CHG_LIM];
        out.bms_dis_limit_kw        = cache_[EMS_CFG_BMS_DIS_LIM];
        out.transformer_capacity_kw = cache_[EMS_CFG_TR_KVA];
        out.d_target_kw             = cache_[EMS_CFG_D_TARGET];
        out.bms_chg_forbidden       = false;
        out.bms_dis_forbidden       = false;
        return true;
    }

    DeviceStatus read_status() const override {
        poll(0);
        DeviceStatus s;
        s.bms_comm_ok    = cache_[EMS_STA_BMS]     > 0.5;
        s.pcs_comm_ok    = cache_[EMS_STA_PCS]     > 0.5;
        s.meter_comm_ok  = cache_[EMS_STA_METER]   > 0.5;
        s.pcs_fault      = cache_[EMS_STA_FAULT]   > 0.5;
        s.device_offline = cache_[EMS_STA_OFFLINE] > 0.5;
        s.data_valid     = data_valid();
        return s;
    }

    DeviceActuals read_actuals() const override {
        poll(0);
        DeviceActuals a;
        a.p_bat_kw      = cache_[EMS_P_BAT];
        a.p_grid_kw     = cache_[EMS_P_GRID];
        a.p_load_kw     = cache_[EMS_P_LOAD];
        a.p_pv_kw       = cache_[EMS_P_PV];
        a.soc           = cache_[EMS_SOC];
        a.temperature_c = cache_[EMS_T_C];
        return a;
    }

    // 写指令：CMD 段三个信息体各发一个 C_SE_NC_1（执行，不带选择）
    bool write_command(const PowerCommand& cmd) override {
        last_cmd_     = cmd;
        has_last_cmd_ = true;
        bool ok = send_setpoint(EMS_CMD_P_BAT,   cmd.p_bat_cmd_kw);
        ok = send_setpoint(EMS_CMD_P_UPPER, cmd.p_upper) && ok;
        ok = send_setpoint(EMS_CMD_P_LOWER, cmd.p_lower) && ok;
        return ok;
    }

    double execute(double p_cmd_kw, double dt_s) override {
        if (has_last_cmd_) {
            send_setpoint(EMS_CMD_P_BAT,   p_cmd_kw);
            send_setpoint(EMS_CMD_P_UPPER, last_cmd_.p_upper);
            send_setpoint(EMS_CMD_P_LOWER, last_cmd_.p_lower);
        } else {
            // 没有权限区间时**只发 CMD.P_BAT 一个信息体**。多发一个 0/0 的
            // 上下限，对端若按权限区间执行会理解成"禁止动作" —— 危险下发。
            // 与 ModbusDeviceIO::execute() 是同一条纪律。
            send_setpoint(EMS_CMD_P_BAT, p_cmd_kw);
        }

        if (pump_) pump_(p_cmd_kw, dt_s);

        poll(0);                       // 取回上送（含实测 P_BAT）
        return cache_[EMS_P_BAT];
    }

    double battery_capacity_kwh() const override {
        if (!cfg_loaded_) poll(0);
        return cache_[EMS_CFG_CAP_KWH];
    }

    const char* name() const override { return "Iec104DeviceIO(104)"; }

private:
    static double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

    void reset_cache() const {
        for (int i = 0; i < Iec104PointMap::kPointCount; ++i) {
            cache_[i]   = EMS_POINT_DEFAULTS[i];
            quality_[i] = true;
        }
        cfg_loaded_ = false;
    }

    void reset_session() {
        rx_len_ = 0;
        vs_ = vr_ = peer_ack_ = 0;
        startdt_ok_ = gi_done_ = false;
        ack_pending_ = false;
        rx_since_ack_ = 0;
        poll_ticks_ = 0;
        last_cmd_ = PowerCommand{};
        has_last_cmd_ = false;
    }

    // ---- 发送 ----
    bool send_bytes(const uint8_t* buf, size_t len) const {
        if (transport_ == nullptr || !transport_->connected()) { ++send_errors_; return false; }
        if (!transport_->send(buf, len)) { ++send_errors_; return false; }
        ++frames_tx_;
        return true;
    }
    bool send_s_frame() const {
        uint8_t f[8];
        const size_t n = iec104::wrap_s_frame(f, sizeof(f), vr_);
        if (n == 0 || !send_bytes(f, n)) return false;
        ++s_tx_;
        ack_pending_   = false;
        rx_since_ack_  = 0;
        return true;
    }
    void send_keepalive() const {
        ++poll_ticks_;
        // 每 heartbeat_polls 轮发一次 TESTFR（心跳），其余轮发 S 帧（确认）。
        // 两者都是合法帧，也都能让从站"这一轮有活干" —— 环回装置里正好借此
        // 换取一次周期上送。
        if (heartbeat_polls_ > 0 && (poll_ticks_ % heartbeat_polls_) == 0) {
            uint8_t f[8];
            const size_t n = iec104::wrap_u_frame(f, sizeof(f), iec104::kTestFrAct);
            if (n > 0 && send_bytes(f, n)) ++testfr_tx_;
            return;
        }
        send_s_frame();
    }
    // 发一个 I 帧。k 窗口满时**不发**（改为先确认再等），并计数以便现场排障。
    bool send_i_frame(const uint8_t* asdu, size_t asdu_len) const {
        if (unacked_tx() >= iec104::kWindowK) {
            ++window_stalls_;
            send_s_frame();
            return false;
        }
        uint8_t f[kTxCap];
        const size_t n = iec104::wrap_i_frame(f, sizeof(f), vs_, vr_, asdu, asdu_len);
        if (n == 0 || !send_bytes(f, n)) return false;
        vs_ = static_cast<uint16_t>((vs_ + 1) & 0x7FFF);
        ++i_tx_;
        return true;
    }
    bool send_setpoint(int index, double value) const {
        const int idx = (index >= 0 && index < Iec104PointMap::kPointCount) ? index : -1;
        if (idx < 0) return false;
        uint8_t asdu[64];
        const size_t an = iec104::make_asdu_setpoint_float(
            asdu, sizeof(asdu), iec104::kCOT_Act, ca_, map_.pts[idx].ioa, value,
            iec104::make_qoc(/*select=*/false));
        if (an == 0) return false;
        return send_i_frame(asdu, an);
    }

    // ---- 接收 ----
    void drain_rx(int timeout_ms) const {
        for (;;) {
            if (rx_len_ >= sizeof(rx_)) rx_len_ = 0;      // 畸形流保护：不留死循环
            size_t got = 0;
            if (!transport_->receive(rx_ + rx_len_, sizeof(rx_) - rx_len_, &got, timeout_ms)) {
                ++link_errors_;
                return;
            }
            if (got == 0) break;
            rx_len_ += got;
            if (timeout_ms != 0) break;                   // 阻塞取一轮即可
        }
        parse_rx();
    }
    void parse_rx() const {
        size_t off = 0;
        while (off < rx_len_) {
            ApciHeader h;
            const size_t n = iec104::parse_apdu(rx_ + off, rx_len_ - off, h);
            if (n == 0) break;                             // 半个帧：留到下一批
            handle_frame(rx_ + off, n, h);
            off += n;
        }
        if (off > 0) {
            std::memmove(rx_, rx_ + off, rx_len_ - off);
            rx_len_ -= off;
        }
    }
    void handle_frame(const uint8_t* apdu, size_t len, const ApciHeader& h) const {
        ++frames_rx_;
        switch (h.kind) {
            case iec104::kFrameU:
                ++u_rx_;
                if (h.ufunc == iec104::kStartDtCon)      startdt_ok_ = true;
                else if (h.ufunc == iec104::kStopDtCon)  startdt_ok_ = false;
                else if (h.ufunc == iec104::kTestFrCon)  ++testfr_rx_;
                break;
            case iec104::kFrameS:
                ++s_rx_;
                peer_ack_    = h.nr;
                ack_pending_ = false;
                rx_since_ack_ = 0;
                break;
            case iec104::kFrameI:
                ++i_rx_;
                vr_          = static_cast<uint16_t>((h.ns + 1) & 0x7FFF);
                peer_ack_    = h.nr;
                ack_pending_ = true;     // 收到 I 帧必须确认（w 窗口的 w 侧）
                ++rx_since_ack_;
                apply_asdu(apdu, len, h);
                break;
        }
    }
    void apply_asdu(const uint8_t* apdu, size_t len, const ApciHeader& h) const {
        AsduView v;
        if (!iec104::parse_asdu(apdu, len, h, v)) { ++malformed_; return; }
        if (v.h.ca != ca_) { ++bad_ca_; return; }
        if (!iec104::asdu_length_ok(v)) { ++malformed_; return; }

        switch (v.h.type_id) {
            case iec104::kM_ME_NC_1:
            case iec104::kM_ME_WIDE: {
                std::vector<InfoFloat> fs;
                const bool ok = (v.h.type_id == iec104::kM_ME_NC_1)
                                    ? iec104::decode_measurements(v, fs)
                                    : iec104::decode_wide(v, fs);
                if (!ok) { ++malformed_; return; }
                for (std::size_t k = 0; k < fs.size(); ++k) {
                    const int idx = map_.index_of_ioa(fs[k].ioa);
                    if (idx < 0) continue;
                    cache_[idx]   = fs[k].value;
                    quality_[idx] = iec104::qds_valid(fs[k].qds);
                }
                cfg_loaded_ = true;      // CFG 段随每次上送刷新
                break;
            }
            case iec104::kM_SP_NA_1: {
                std::vector<InfoSingle> ss;
                if (!iec104::decode_single_points(v, ss)) { ++malformed_; return; }
                for (std::size_t k = 0; k < ss.size(); ++k) {
                    const int idx = map_.index_of_ioa(ss[k].ioa);
                    if (idx < 0) continue;
                    cache_[idx]   = ss[k].value ? 1.0 : 0.0;
                    quality_[idx] = iec104::qds_valid(ss[k].qds);
                }
                break;
            }
            case iec104::kC_SE_NC_1: {
                uint32_t ioa = 0;
                double   val = 0.0;
                uint8_t  qos = 0;
                if (!iec104::decode_setpoint_float(v, &ioa, &val, &qos)) { ++malformed_; return; }
                if (v.h.cot == iec104::kCOT_ActCon) {
                    ++sp_actcon_;
                    // 命令确认里带 QOS 回显；只有 CMD 段才是本适配器发过的
                    if (ioa >= kIoaCmdBase && ioa < kIoaCmdBase + 16) ++sp_echo_;
                }
                break;
            }
            case iec104::kC_IC_NA_1:
                if (v.h.cot == iec104::kCOT_ActCon)       ++gi_actcon_;
                else if (v.h.cot == iec104::kCOT_ActTerm) { ++gi_actterm_; gi_done_ = true; }
                break;
            case iec104::kM_EI_NA_1:
                ++init_end_;
                break;
            default:
                ++unknown_type_;
                break;
        }
    }

    bool data_valid() const {
        bool ok = cache_[EMS_STA_VALID] > 0.5;
        for (int i = 0; i <= static_cast<int>(EMS_SOH); ++i) ok = ok && quality_[i];
        return ok;
    }

    IIec104Transport* transport_ = nullptr;
    Iec104PointMap    map_;
    uint16_t          ca_ = 1;

    bool              poll_keepalive_ = true;
    int               heartbeat_polls_ = 3;
    int               stale_after_polls_ = 0;

    mutable double cache_[Iec104PointMap::kPointCount]   = {};
    mutable bool   quality_[Iec104PointMap::kPointCount] = {};
    mutable bool   cfg_loaded_ = false;

    mutable uint16_t vs_       = 0;    // 我方 N(S)
    mutable uint16_t vr_       = 0;    // 我方 N(R)（= 期望对端下一个 N(S)）
    mutable uint16_t peer_ack_ = 0;    // 对端最近一次给的 N(R)
    mutable bool     startdt_ok_ = false;
    mutable bool     gi_done_    = false;
    mutable int      ack_pending_ = false;
    mutable int      rx_since_ack_ = 0;
    mutable int      poll_ticks_   = 0;
    mutable int      polls_without_data_ = 0;

    mutable uint8_t rx_[kRxCap] = {};
    mutable size_t  rx_len_     = 0;

    mutable int frames_tx_ = 0, frames_rx_ = 0;
    mutable int i_tx_ = 0, i_rx_ = 0, s_tx_ = 0, s_rx_ = 0, u_tx_ = 0, u_rx_ = 0;
    mutable int testfr_tx_ = 0, testfr_rx_ = 0;
    mutable int gi_actcon_ = 0, gi_actterm_ = 0, init_end_ = 0;
    mutable int sp_actcon_ = 0, sp_echo_ = 0;
    mutable int malformed_ = 0, bad_ca_ = 0, unknown_type_ = 0;
    mutable int window_stalls_ = 0;
    mutable int stale_reads_ = 0;
    mutable int send_errors_ = 0;
    mutable int link_errors_ = 0;

    PowerCommand last_cmd_{};
    bool         has_last_cmd_ = false;
    DevicePump   pump_{};
};

} // namespace ems
