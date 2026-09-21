// =====================================================================
// 外部设定落点接口（A3.1）
//
// ---------------------------------------------------------------------
// 为什么必须有这个接口：网关原先"一律否定确认"
// ---------------------------------------------------------------------
// IEC104 网关是"第五个角色"，纪律是**它不能直接驱动设备** ——
// 调度遥控若直写 `CMD.*`，就绕过了 `05/` 安全引擎，等于远端能直接开 PCS。
// 这条纪律在 `rtdb_source.h` 里的表达是 `RtDbReadOnlySource` **没有 `write()`**。
//
// 于是 A3.1 之前，网关收到的 6 个下行点**没有落点**，只能一律回否定确认。
// 那是**诚实**的（比"回肯定确认但什么也没做"安全），但调度也就真的控不了。
//
// ---------------------------------------------------------------------
// 现在怎么让步，以及让步的边界在哪
// ---------------------------------------------------------------------
// ★ 让步的方式是：**另给一个类型，只暴露六个具名 setter。**
//
//   本接口里**不存在** "按索引写" / "按点名写" / "批量写" 这类入口。
//   所以"网关写 `CMD.*`"这件事不是"不允许"，而是**写不出来** ——
//   与 `RtDbReadOnlySource` 用"没有 write()"表意是同一手法，
//   只是这次要放开的恰好是 `EXT.*` 那一小块。
//
//   ★ 两个能力在类型上是**分开**的：读口的类型没有写方法，写口的类型只有
//     六个具名方法。将来维护者想把一条命令从读口"顺手"送出去，必须新增
//     一个方法 —— 而那是**显式动作**，会被 review 看见。
//
//   为什么不做成"允许写 EXT 区"的白名单式 `write(point, value)`：
//     白名单是**运行期**校验（错了只在运行期报），而具名方法是**编译期**的。
//     这个项目在 A1/A2 上反复吃过"写在注释里的约束没人守"的亏。
//
// ---------------------------------------------------------------------
// 与 IEC104 下行点的对应（表在 iec104_point_map.h::download_table）
// ---------------------------------------------------------------------
//   6001 有功功率设定  → set_p_setpoint
//   6002 契约需量设定  → set_d_target
//   6003 允许上界设定  → set_upper
//   6004 允许下界设定  → set_lower
//   5001 PCS 远程启停  → set_pcs_onoff
//   5002 EMS 投入退出  → set_ems_enable
//
// ★ 返回值是**确认的依据**：true → 回肯定确认（ACT_CON），false → 回否定确认。
//   "受理与否必须端到端可追溯"是安全相关要求，不能无条件回 ACT_CON。
// =====================================================================

#ifndef EMS_P3_EXT_SETPOINT_SINK_H
#define EMS_P3_EXT_SETPOINT_SINK_H

#include <string>

namespace iec104 {

class IExtSetpointSink {
public:
    virtual ~IExtSetpointSink() = default;

    // 落点失败必须返回 false 并**说明原因**（原因会进否定确认，供调度侧排查）。
    // 每个方法的 err 允许为 nullptr（调用方不关心时）。
    virtual bool set_p_setpoint(double kw,     std::string* err) = 0;
    virtual bool set_upper     (double kw,     std::string* err) = 0;
    virtual bool set_lower     (double kw,     std::string* err) = 0;
    virtual bool set_d_target  (double kw,     std::string* err) = 0;
    virtual bool set_pcs_onoff (bool   on,     std::string* err) = 0;
    virtual bool set_ems_enable(bool   enabled, std::string* err) = 0;

    // 清空全部外部设定（主站断开 / 空闲超时）→ 回到"无外部设定"。
    //
    // ★ 语义是"把六个设定写成**不施加约束**的哨兵值"，**不是**把 `EXT.SEQ` 归零：
    //   SEQ 表示"本区被改写过多少次"，读者用"SEQ 变了没有"决定要不要重新处理。
    //   归零 = 让它在读者眼里**倒退**，而"倒退"与"没变化"在判据上不可区分
    //   （见 rtdb_ext_sink.h 的发布协议）。
    virtual bool clear_all(std::string* err) = 0;
};

// ---------------------------------------------------------------------
// 默认实现：什么都不落，一律拒绝。
//
// 为什么要有它：`main_gateway.cpp` 在**没有配置 RT_DB 落点**时必须保持
// A3.1 之前的行为（一律否定确认）—— 那是安全的一侧。有了这个默认实现，
// "没配落点"就是一个**显式的对象**，而不是一段 `if (sink_ == nullptr)`。
// ---------------------------------------------------------------------
class NullSetpointSink : public IExtSetpointSink {
public:
    bool set_p_setpoint(double,      std::string* err) override { return deny(err); }
    bool set_upper     (double,      std::string* err) override { return deny(err); }
    bool set_lower     (double,      std::string* err) override { return deny(err); }
    bool set_d_target  (double,      std::string* err) override { return deny(err); }
    bool set_pcs_onoff (bool,        std::string* err) override { return deny(err); }
    bool set_ems_enable(bool,        std::string* err) override { return deny(err); }
    // 清空是幂等的"无事可做"，不是失败 —— 返回 true 更准确
    bool clear_all(std::string*) override { return true; }

private:
    static bool deny(std::string* err) {
        if (err) *err = "网关未配置外部设定落点（RT_DB EXT 区不可用）";
        return false;
    }
};

// ---------------------------------------------------------------------
// 按 IOA 分发。**不引入 lib60870 类型**，便于单测直接调用。
//
// 为什么把分发单独抽出来：IOA ↔ 语义的对应关系如果散在 `main_gateway.cpp`
// 的 if-else 里，"6003 被接到了上界还是下界"这种错只能靠端到端联调发现。
// 抽成这个函数后，六条对应关系可以用**六条单测**穷举掉。
//
// 返回 false 时 err 写明原因（未知 IOA / 落点失败）。
// ---------------------------------------------------------------------
inline bool dispatch_ext_setpoint(IExtSetpointSink& sink, int ioa, double value,
                                  std::string* err) {
    switch (ioa) {
        case 6001: return sink.set_p_setpoint(value, err);
        case 6002: return sink.set_d_target(value, err);
        case 6003: return sink.set_upper(value, err);
        case 6004: return sink.set_lower(value, err);
        case 5001: return sink.set_pcs_onoff(value > 0.5, err);
        case 5002: return sink.set_ems_enable(value > 0.5, err);
        default:
            if (err) {
                *err = "IOA " + std::to_string(ioa) +
                       " 没有落点（下行表里不是可受理的设定点）";
            }
            return false;
    }
}

} // namespace iec104

#endif // EMS_P3_EXT_SETPOINT_SINK_H
