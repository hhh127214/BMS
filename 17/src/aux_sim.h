// =====================================================================
// 17/ — 选配模块模拟器（消防 / 温控散热 / 门禁安防 / 配电开关）
//
// 为什么四个类放在**同一个文件**：
//   它们共同对应"柜型选配"这件事 —— 装配与否取决于柜型（`docs/规划/部件测点映射表.csv`
//   里那 17 个曾经是缺口的部件）。拆成四个文件会让人以为它们是四件独立的事，
//   合在一起才能一眼看出：**这是一层可选的柜体配置，不是点表的固有部分**。
//
// ---------------------------------------------------------------------
// ★ 与既有三个模拟器的**唯一关键差异**：点名从哪来
//
//   bms_sim / pcs_sim / meter_sim 都用 `dpp(devpt::DP_XXX)` —— 那是**内置点表
//   的索引常量**，编译期就点死了。而本文件的 17 个点**没有内置索引**（它们走
//   `data/mod_*.json` 运行期加载），所以只能按**点名字符串**发布。
//
//   这不是降级，而是选配性的直接后果：既然"有没有这个部件"要在运行期决定，
//   那它发出的点名当然也不可能编译期确定。代价是**拼错点名不再有编译期拦截**，
//   因此本文件把点名集中放到一处常量里，并由测试拿「加载后的整张表」反查 ——
//   拼错一行形如 `"ALM.FIRE.SMOKE"` 的名字，测试会红，而不是静默丢一个点。
//
// ★ 为什么不顺便把这四个类做成"整个 17/ 通用的放点方式"：
//   内置三层仍需编译期索引（self_check 靠它），两种机制并存是有意的 ——
//   编译期索引管"必有之物"，名字 Load out 管"可无之物"。
//
// 编译：纯头文件（inline）。
// =====================================================================

#pragma once

#include "point_sink.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace ems {
namespace devsim {

// =====================================================================
// 点名常量：集中一处，便于测试对着加载后的点表反查
// =====================================================================
namespace auxpt {

inline const char* fire_state()    { return "STA.FIRE.HOST_STATE"; }
inline const char* fire_smoke()    { return "ALM.FIRE.SMOKE"; }
inline const char* fire_heat()     { return "ALM.FIRE.HEAT"; }
inline const char* fire_released() { return "STA.FIRE.RELEASED"; }
inline const char* fire_press()    { return "MEAS.FIRE.PRESSURE"; }

inline const char* hvac_t_amb()    { return "MEAS.HVAC.T_AMB"; }
inline const char* hvac_t_return() { return "MEAS.HVAC.T_RETURN"; }
inline const char* hvac_t_water()  { return "MEAS.HVAC.T_WATER"; }
inline const char* hvac_flow()     { return "MEAS.HVAC.FLOW"; }
inline const char* hvac_pump()     { return "STA.HVAC.PUMP_ON"; }
inline const char* hvac_onoff()    { return "CMD.HVAC.ONOFF"; }
inline const char* hvac_t_set()    { return "CMD.HVAC.T_SET"; }

inline const char* sec_door()      { return "STA.SEC.DOOR_OPEN"; }
inline const char* sec_water()     { return "ALM.SEC.WATER"; }

// ★ 段名与 `device_kind_name()` **逐字一致**（SWITCH 不是 SW）：
//   测试 T11 正是拿这一点反查点名，缩写会让"发布对了"被判成"发布错了"。
inline const char* sw_breaker()    { return "STA.SWITCH.AC_BREAKER"; }
inline const char* sw_trip()       { return "STA.SWITCH.TRIP_REASON"; }
inline const char* sw_estop()      { return "STA.SWITCH.ESTOP"; }

} // namespace auxpt

// =====================================================================
// 消防主机模拟器（DK_FIRE，5 点）
//
// 状态机（对应 `STA.FIRE.HOST_STATE`）：0 正常 / 1 火警 / 2 故障
//   · 烟感或温感动作 → 火警，并启动喷洒延时；
//   · 延时到 → 喷洒释放（**锁存**，不随探测器复位而撤），压力随之下降；
//   · 通信中断 → 故障/**优先级最高**（看不见的火比误报危险，宁可报故障）。
//
// ★ EMS 侧职责边界（`链路协议清单.csv` 已写明）：**只读监视 + 联锁停机**，
//   消防不参与调度决策。因此本模拟器只暴露 `trip_required()`，
//   不提供任何"降功率目标"之类的输出 —— 那是 EMS 上层的事。
// =====================================================================
struct FireSimConfig {
    bool   smoke_detected      = false;   // 可注入：烟感动作
    bool   heat_detected       = false;   // 可注入：温感动作
    bool   comm_ok             = true;    // 可注入：通信中断 → 故障
    double release_delay_s     = 5.0;     // 报警到喷洒的延时
    double pressure_init_mpa   = 1.20;    // 初始压力
    double pressure_empty_mpa  = 0.20;    // 喷完后残余
    double release_rate_mpa_s  = 0.030;   // 喷洒压降速率
    double drift_amp_mpa       = 0.004;   // 正常时的微小漂移（让"在动"可判定）
};

class FireSim {
public:
    explicit FireSim(const FireSimConfig& c = FireSimConfig()) : cfg_(c) {}

    FireSimConfig& mutable_cfg() { return cfg_; }

    // 是否需要联锁停机（EMS 上层消费；本模拟器不自行决定怎么停）
    bool trip_required() const { return released_ || cfg_.smoke_detected || cfg_.heat_detected; }

    void reset() {
        alarm_s_ = 0.0; released_ = false; press_ = cfg_.pressure_init_mpa; t_ = 0.0;
    }

    // 现场复位：对应真机主机上的复位按钮 + 换瓶/复压。
    //
    // ★ 为什么必须有这个接口，而不是让 released_ 随探测器复位自动清：
    //   喷洒一旦发生，现场就有灭火剂残留、钢瓶已泄压 —— "探测器不再报警"
    //   推不出"现场可以恢复运行"。真机上这一步必须人到现场确认后按下，
    //   所以这里也必须是**显式调用**。
    //
    //   缺了它，"喷洒"就成了一道单向门：模拟器再也回不到正常态，
    //   现场复电流程在联调里根本没法验证（见 T12 的教训）。
    void reset_alarm() {
        alarm_s_ = 0.0;
        released_ = false;
        press_    = cfg_.pressure_init_mpa;
    }

    void publish_static(PointSink& sink) const {
        sink.set(auxpt::fire_press(), cfg_.pressure_init_mpa);
    }

    void step(double t, double dt, PointSink& sink) {
        t_ = t;
        const bool alarm = cfg_.smoke_detected || cfg_.heat_detected;

        // ① 状态：故障时"看不见"优先 —— 通信断了就不知道现场怎么样
        int state = 2;                                   // 故障
        if (cfg_.comm_ok) {
            if (alarm)         state = 1;                // 火警
            else if (released_) state = 1;               // 已喷洒，仍属火警态
            else               state = 0;                // 正常
        }

        // ② 喷洒延时 + 锁存
        if (alarm) alarm_s_ += dt;
        else       alarm_s_ = 0.0;
        if (alarm && alarm_s_ >= cfg_.release_delay_s) released_ = true;

        // ③ 压力：喷洒时下降，否则缓慢漂移（温度导致的日常波动）
        double target = press_;
        if (released_) {
            press_ -= cfg_.release_rate_mpa_s * dt;
            if (press_ < cfg_.pressure_empty_mpa) press_ = cfg_.pressure_empty_mpa;
        } else {
            target = cfg_.pressure_init_mpa
                   + cfg_.drift_amp_mpa * std::sin(2.0 * kPi * t / 900.0);
            press_ = target;
        }

        sink.set(auxpt::fire_state(),    static_cast<double>(state));
        sink.set(auxpt::fire_smoke(),    cfg_.smoke_detected ? 1.0 : 0.0);
        sink.set(auxpt::fire_heat(),     cfg_.heat_detected  ? 1.0 : 0.0);
        sink.set(auxpt::fire_released(), released_ ? 1.0 : 0.0);
        sink.set(auxpt::fire_press(),    press_);
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    FireSimConfig cfg_;
    double alarm_s_  = 0.0;
    bool   released_ = false;
    double press_    = 1.20;
    double t_        = 0.0;
};

// =====================================================================
// 温控 / 散热模拟器（DK_HVAC，7 点）
//
// 一阶热模型（与大面积被冷却舱体同口径）：
//     C·dT/dt = Q_loss − k·(T − T_out) − COP·P_cool·u(t)
//   即：热源把舱内顶起来，舱体自然散热往环境掉，空调（u∈{0,1}）按额定制冷量往下压。
//
// 为什么用**带回差的温控器**（±hyst/2）而不是连续调节：
//   真机是压缩机启停（ON/OFF 控制），不是变频无级调节。若用连续控制，
//   `STA.HVAC.PUMP_ON` 就永远不会翻转 —— 而这条恰恰是最该被观测到的state
//   （"水泵到底开了没有"）。回差同时防止边界抖动导致的频繁启停。
// =====================================================================
struct HvacSimConfig {
    double c_th_kj_per_k     = 800.0;   // 舱体热容
    double k_loss_kw_per_k   = 0.35;    // 自然散热系数
    double t_out_c           = 30.0;    // 柜外环境温度
    double t_init_c          = 28.0;    // 柜内初始温度
    double cool_kw           = 6.0;     // 额定制冷量（带走的热功率）
    double hyst_k            = 1.0;     // 温控回差
    double rated_flow_lpm    = 40.0;    // 额定流量
    double flow_ramp_lpm_s   = 20.0;    // 流量建立速率（泵启后不是立刻满流量）
    double water_lag_s       = 20.0;    // 水温滞后时间常数
    double return_lag_s      = 8.0;     // 回风温度滞后
    bool   faultdisabled     = false;   // 可注入：设备故障（不制冷）
};

class HvacSim {
public:
    explicit HvacSim(const HvacSimConfig& c = HvacSimConfig()) : cfg_(c) { reset(); }
    HvacSimConfig& mutable_cfg() { return cfg_; }

    void reset() {
        t_amb_ = cfg_.t_init_c; t_ret_ = cfg_.t_init_c; t_wat_ = cfg_.t_init_c;
        flow_ = 0.0; pump_on_ = false; t_ = 0.0; on_ = false; t_set_ = 25.0;
    }

    // cmd_onoff / cmd_t_set 是 EMS 下行量；同时**回读**当前下发值。
    //
    // ★ 为什么这里把 CMD 区也发布出来（`CMD.HVAC.ONOFF` / `CMD.HVAC.T_SET`）：
    //   PCS 那边的规矩是"CMD 区不由 simulator 发布"（那是 EMS 下行）。但 HVAC 走在
    //   RS485 保持寄存器上，现场查故障的第一句话就是"设下去的值回读是多少" ——
    //   寄存器回读与下行值不一致本身就是一类真实事故。所以这里发布的是
    //   **回读值**，不是"模拟器替 EMS 下令"。测试会断言两者一致。
    void step(double t, double dt, PointSink& sink, bool cmd_onoff, double cmd_t_set,
              double heat_kw) {
        t_ = t; on_ = cmd_onoff; t_set_ = cmd_t_set;

        // ① 温控器（带回差）
        if (on_) {
            if (!pump_on_ && t_amb_ > t_set_ + cfg_.hyst_k * 0.5) pump_on_ = true;
            if ( pump_on_ && t_amb_ < t_set_ - cfg_.hyst_k * 0.5) pump_on_ = false;
        } else {
            pump_on_ = false;
        }
        const double cooling = (pump_on_ && !cfg_.faultdisabled) ? cfg_.cool_kw : 0.0;

        // ② 一阶热模型
        const double dT = (heat_kw - cfg_.k_loss_kw_per_k * (t_amb_ - cfg_.t_out_c) - cooling)
                        / cfg_.c_th_kj_per_k * dt;
        t_amb_ += dT;

        // ③ 回风温度：跟随舱温，带滞后 + 局部温升（回风比舱内均值略高）
        const double a_ret = 1.0 - std::exp(-dt / cfg_.return_lag_s);
        t_ret_ += (t_amb_ + 0.8 * (cooling > 0.0 ? 1.0 : 0.0) - t_ret_) * a_ret;

        // ④ 水温：泵运行时趋近"设定值下方"（供水要更冷），停泵后向舱温漂移
        const double t_wat_target = pump_on_ ? (t_set_ - 5.0) : t_amb_;
        const double a_wat = 1.0 - std::exp(-dt / cfg_.water_lag_s);
        t_wat_ += (t_wat_target - t_wat_) * a_wat;

        // ⑤ 流量：泵启后有建立过程，不是阶跃到满量程
        const double flow_target = pump_on_ ? cfg_.rated_flow_lpm : 0.0;
        if (flow_ < flow_target) flow_ = std::min(flow_target, flow_ + cfg_.flow_ramp_lpm_s * dt);
        else                     flow_ = std::max(flow_target, flow_ - cfg_.flow_ramp_lpm_s * dt);

        sink.set(auxpt::hvac_t_amb(),    t_amb_);
        sink.set(auxpt::hvac_t_return(), t_ret_);
        sink.set(auxpt::hvac_t_water(),  t_wat_);
        sink.set(auxpt::hvac_flow(),     flow_);
        sink.set(auxpt::hvac_pump(),     pump_on_ ? 1.0 : 0.0);
        sink.set(auxpt::hvac_onoff(),    on_      ? 1.0 : 0.0);
        sink.set(auxpt::hvac_t_set(),    t_set_);
    }

private:
    HvacSimConfig cfg_;
    double t_amb_ = 28.0, t_ret_ = 28.0, t_wat_ = 28.0;
    double flow_  = 0.0;
    bool   pump_on_ = false, on_ = false;
    double t_set_ = 25.0, t_ = 0.0;
};

// =====================================================================
// 门禁 / 安防模拟器（DK_SEC，2 点）
//
// 只有两个点，但它们的值是**干接点 DI**，特点是：
//   · 门磁：随时可能开合，`door_open` 必须能随时翻转；
//   · 水浸：**锁存**（一旦泡过水，必须由人现场复位，不能自动恢复 ——
//     这也是为什么它放在 ALM 区而不是 STA 区）。
// =====================================================================
struct SecSimConfig {
    bool door_open  = false;   // 可注入
    bool water_hit  = false;   // 可注入（true 后锁存）
    double door_auto_s = 0.0;  // >0 时自动周期开合（演示/联调用）
};

class SecSim {
public:
    explicit SecSim(const SecSimConfig& c = SecSimConfig()) : cfg_(c) {}
    SecSimConfig& mutable_cfg() { return cfg_; }

    void reset() { water_latched_ = false; }

    void step(double t, double dt, PointSink& sink) {
        (void)dt;
        bool door = cfg_.door_open;
        if (cfg_.door_auto_s > 0.0) {
            // 每 door_auto_s 开一次，开 1/4 周期
            const double ph = std::fmod(t, cfg_.door_auto_s) / cfg_.door_auto_s;
            door = (ph < 0.25);
        }
        if (cfg_.water_hit) water_latched_ = true;   // 锁存，不自动恢复

        sink.set(auxpt::sec_door(),  door ? 1.0 : 0.0);
        sink.set(auxpt::sec_water(), water_latched_ ? 1.0 : 0.0);
    }

    bool water_latched() const { return water_latched_; }

private:
    SecSimConfig cfg_;
    bool water_latched_ = false;
};

// =====================================================================
// 配电开关模拟器（DK_SWITCH，3 点）
//
// 交流侧断路器 + 急停。这是**安全的一部分**，两条纪律：
//
//   ① 急停按下 → 断路器**必须**分闸（tripped），且 `trip_reason` 给出原因码；
//   ② 急停未复位时，**任何**合闸命令都必须被拒绝。
//      这两条如果没钉住，最常见的失效是"急停按下但程序照样下发合闸" ——
//      而这恰恰是我们上周标出的危险语义之最。
//
// 原因码：0 无 / 1 手动分闸 / 2 过流 / 3 短路 / 4 漏电 / 5 急停 / 6 外部联锁(消防)
// =====================================================================
struct SwitchSimConfig {
    bool  breaker_closed   = true;   // 初始合闸
    bool  estop_pressed    = false;  // 可注入
    bool  external_trip    = false;  // 可注入：外部联锁（如消防）
};

class SwitchSim {
public:
    static constexpr int kReasonNone     = 0;
    static constexpr int kReasonManual   = 1;
    static constexpr int kReasonOverload = 2;
    static constexpr int kReasonEstop    = 5;
    static constexpr int kReasonExtTrip  = 6;

    explicit SwitchSim(const SwitchSimConfig& c = SwitchSimConfig()) : cfg_(c) {}

    SwitchSimConfig& mutable_cfg() { return cfg_; }
    int  trip_reason() const { return reason_; }
    bool closed()       const { return closed_; }

    // 合闸请求。返回是否被接受（急停未复位 → 拒绝）
    bool request_close(double t, double dt, PointSink& sink, bool want_close) {
        step_begin(t, dt);

        // ① 安全优先：急停 / 外部联锁 → 强制分闸，且覆盖本次请求
        bool denied = false;
        if (cfg_.estop_pressed) {
            if (closed_) { closed_ = false; reason_ = kReasonEstop; }
            if (want_close) denied = true;
        } else if (cfg_.external_trip) {
            if (closed_) { closed_ = false; reason_ = kReasonExtTrip; }
            if (want_close) denied = true;
        } else if (!want_close) {
            if (closed_) { closed_ = false; reason_ = kReasonManual; }
        } else {
            closed_ = true;
            reason_ = kReasonNone;
        }

        publish(sink);
        return !denied;
    }

    // 不带命令的一拍（只刷新状态）
    void step(double t, double dt, PointSink& sink) {
        request_close(t, dt, sink, closed_ && !cfg_.estop_pressed && !cfg_.external_trip);
    }

private:
    void step_begin(double t, double dt) { t_ = t; last_dt_ = dt; }
    void publish(PointSink& sink) {
        sink.set(auxpt::sw_breaker(), closed_ ? 1.0 : 0.0);
        sink.set(auxpt::sw_trip(),    static_cast<double>(reason_));
        sink.set(auxpt::sw_estop(),   cfg_.estop_pressed ? 1.0 : 0.0);
    }

    SwitchSimConfig cfg_;
    bool closed_ = true;
    int  reason_ = kReasonNone;
    double t_ = 0.0, last_dt_ = 0.0;
};

} // namespace devsim
} // namespace ems
