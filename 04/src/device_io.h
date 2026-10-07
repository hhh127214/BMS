// =====================================================================
// 04/ — 设备 I/O 抽象接口 IDeviceIO（产品化 P0：架构分层）
//
// 目标（产品化 P0）：
//   把「算法」与「设备数据来源」解耦。算法层（05/06/07/08）只依赖本接口，
//   **不依赖**任何具体实现。本接口的实现者共 4 个（路径经核实）：
//     · SimDeviceIO    07/src/sim_device_io.h     仿真适配器（周期 7 的 PlantModel）
//     · MemoryDeviceIO 07/src/memory_device_io.h  进程内点表适配器（P0.5，验证接口真的可换）
//     · RtDbDeviceIO   07/src/rtdb/rtdb_device_io.h 共享内存实时库适配器
//     · ModbusDeviceIO 13/src/modbus_device_io.h  现场设备适配器，EMS 为主站
//       13/ 的主体是**头文件**（modbus_tcp_client.h / modbus_point_map.h /
//       modbus_device_io.h），由 07/ 装配层 include 使用；modbus_probe.exe 只是它的
//       一个消费者（见 13/scripts/build.bat 开头）。故 Modbus 适配器不另立进程。
//       ★ 现场装配点：07/src/main_field.cpp（`--device modbus` 时 attach 本实现，
//         默认 sim 时 attach SimDeviceIO，见 attach_device() 传参）。
//
// 【更正 · 本注释早期版本的错误】原文把 ModbusDeviceIO 指到 P3/src/modbus_device_io.h、
//   把一个 "Iec104DeviceIO" 指到 P3/src/iec104_device_io.h。核实结论：
//     · 这两个文件都不存在；`Iec104DeviceIO` 这个类**全仓库无定义、无引用**；
//     · IEC104 方向落成了 P3/ 的**独立网关进程**，既不实现本接口、也不 include 本头文件
//       （P3/src/ 下无任何对 04/ 的依赖）。真实的类是：
//         Iec104Server  调度从站（EMS 为受控站）  P3/src/iec104_server.h:149
//       它按同样的「接口对实现解耦」思路，另立了两个更贴合协议语义的口：
//         IDataSource   读点     iec104_server.h:92   品质是**三态**而非 bool
//                       （kGood / kNotTopical 值可读但陈旧 / kInvalid）
//         ICommandSink  收遥控遥调 iec104_server.h:109  返回值决定回肯定/否定确认
//   为什么不硬塞进 IDeviceIO：本接口是按「每拍读一次快照、写一次指令」的闭环控制
//   语义设计的，而 IEC104 是「按点上报 + 遥控确认」，粒度不同；品质也不是一个
//   data_valid 布尔位能装下的（见上面 IDataSource 的三态说明）。
//   强行统一只会让 execute()/read_snapshot() 退化成空壳，并把品质位的语义丢掉。
//
//   另一处**已知缺口**（诚实记录，勿当作已覆盖）：上述 4 个实现里，只有 Sim/Mem/RtDb
//   三者做过数值等价比对（07/tests/test_device_io.cpp 的 T23；12/src/acceptance_runner.h
//   的 A1-05，max|Δ| ≤ 1e-9）。ModbusDeviceIO **不在**该等价比对内，它另由
//   13/tests/ 的适配器契约测试（T21~T30）覆盖，且 T28 断言 execute() 不做物理积分
//   「与 RtDbDeviceIO 同契约」。
//
// 为什么必须做这一步（不做会怎样）：
//   现状 EmsRuntime 直接持有 PlantModel，算法与仿真对象编译期绑死。后果：
//     ① 现场调试时只能改源码把 PlantModel 换成真实驱动 —— 无法交付；
//     ② 单元测试与现场运行走的是两套代码路径，测过的不是要跑的；
//     ③ 接入 RT_DB 要改动 05/06/07/08 四个模块，回归面不可控。
//
// 关键设计约束（与 RT_DB / 点表对接的前提，务必遵守）：
//   1. 接口参数一律是**业务语义结构体**（RealtimeSnapshot / DeviceLimits /
//      PowerCommand / DeviceStatus），**绝不出现点名**（如 "BMS_01.SOC"）。
//      点名是基础设施细节，只允许出现在适配器内部。若让算法看见点名，
//      就等于把「点表定义」变成了算法的编译期依赖，换一个站就得改算法。
//   2. 接口只表达五件事：读量测 / 读限制 / 读状态 / 写指令 / 推进一拍。
//   3. 算法**不得假设 execute() 的返回值是可信实际功率**。仿真里 execute()
//      立即返回本拍实际功率；真实系统里它只是「把指令发出去」，实际值要等
//      下一拍 read_snapshot() 的 p_bat_actual_kw。闭环必须走量测。
//      因此本接口把 execute() 的返回值定义为「适配器尽力提供的即时反馈，
//      仅用于仿真/记录」，闭环回路以 read_snapshot() 为准。
//
// 编译：纯头文件，无需单独编译。
// =====================================================================

#pragma once

#include "data_models.h"

namespace ems {

// ---------------------------------------------------------------------
// 设备通信/故障状态（业务语义，非点名）
//
// 对应真实系统里的通信心跳、装置告警、数据品质位。
// ---------------------------------------------------------------------
struct DeviceStatus {
    bool      bms_comm_ok    = true;   // BMS 通信正常
    bool      pcs_comm_ok    = true;   // PCS 通信正常
    bool      meter_comm_ok  = true;   // 关口电表通信正常
    bool      pcs_fault      = false;  // PCS 故障（停止出力）
    bool      device_offline = false;  // 设备离线
    bool      data_valid     = true;   // 数据有效性（品质位）
    Timestamp last_update    = 0.0;    // 最近一次成功采集时刻
};

// ---------------------------------------------------------------------
// 设备实际运行值（记录 / 展示用）
//
// 真实系统里这些来自下一拍量测；仿真里直接取自物理模型。
// 注意：与 RealtimeSnapshot 的区别 —— 本结构是「真值」（无噪声、无站用电
// 折算），用于 CSV 与指标统计；RealtimeSnapshot 是「EMS 看到的量测」。
// ---------------------------------------------------------------------
struct DeviceActuals {
    double p_bat_kw      = 0.0;    // 电池实际功率（放电为正）
    double p_grid_kw     = 0.0;    // 关口实际功率（进口为正）
    double p_load_kw     = 0.0;    // 负荷实际值（不含站用电折算）
    double p_pv_kw       = 0.0;    // 光伏实际出力
    double soc           = 0.5;    // 实际 SOC
    double temperature_c = 25.0;   // 实际温度
};

// ---------------------------------------------------------------------
// 设备 I/O 抽象接口
//
// 生命周期：适配器由装配层（main / 进程入口）创建并注入 EmsRuntime，
//           EmsRuntime 只持有裸指针，**不拥有**适配器。
// ---------------------------------------------------------------------
class IDeviceIO {
public:
    virtual ~IDeviceIO() = default;

    // ① 读量测快照（冻结语义，接口规范 §5）。
    //    约定：**无论返回什么，适配器都必须填充 out** —— 采集失败时填最近一次
    //    有效值，无历史值时填保守零值。返回值仅作诊断提示；数据是否可信以
    //    read_status().data_valid 为准。这样算法层无需为"采集失败"写分支，
    //    故障语义统一收敛到 ② 的故障判定里。
    virtual bool read_snapshot(Timestamp now, RealtimeSnapshot& out) = 0;

    // ② 读设备运行时限制（PCS 额定、BMS 动态降功率、变压器容量、契约需量）。
    //    每个控制周期刷新 —— 这是 BMS 动态降功率能生效的唯一入口。
    virtual bool read_limits(DeviceLimits& out) = 0;

    // ③ 读通信/故障状态
    virtual DeviceStatus read_status() const = 0;

    // ④ 读实际运行值（记录/展示；非闭环回路）
    virtual DeviceActuals read_actuals() const = 0;

    // ⑤ 写功率指令（下发 PCS）
    virtual bool write_command(const PowerCommand& cmd) = 0;

    // ⑥ 推进一拍。
    //    仿真：积分物理模型（死区/惯性/变化率/效率），返回本拍实际功率。
    //    真实：仅「发指令 + 等待」，返回值不可信，闭环请用 ① 的实测值。
    virtual double execute(double p_cmd_kw, double dt_s) = 0;

    // ⑦ 设备静态参数：电池额定容量（kWh）。用于 SOC 规划与优化层的能量约束。
    virtual double battery_capacity_kwh() const = 0;

    // ⑧ 适配器名称（日志 / 自检 / 现场排障用）
    virtual const char* name() const = 0;

    // ⑨ ② 的限值是否"活的"（运行期会变）。
    //
    // 为什么需要这个方法：`read_limits()` 是**每控制周期**该刷的（见 ② 的契约），
    //   但 dev_ 同时也是装配层/测试的注入点（rt.device_limits() = cfg.limits）。
    //   无条件每拍刷新会把注入覆盖掉，现有多处仿真注入（09/10/P1/P2）即刻失效。
    //   所以"要不要每拍刷"取决于限值到底会不会在脚下变：
    //     · 真实设备 / 共享内存适配器 → true：BMS 动态降功率、禁充放位、
    //       PCS 额定都可能在中途变，只在装配期读一次 = 当常量用。
    //     · 仿真适配器 → false：限值来自装配配置，同一进程内不存在
    //       "没人通知就变了"的情况，刷新只会覆盖调用方的注入。
    //   EmsRuntime::attach_device() 据此自动打开运行期刷新，现场装配不会忘。
    virtual bool limits_are_live() const { return false; }
};

} // namespace ems
