// =====================================================================
// 07/ — 仿真适配器 SimDeviceIO（把 PlantModel 接到 IDeviceIO 上）
//
// 作用：产品化 P0 的「保真通道」。它把周期 7 的 PlantModel（电池 + PCS +
//       电表 + 热模型 + 故障注入）包装成 IDeviceIO 实现，**行为与重构前
//       逐位一致** —— 这是 P0 能安全落地的前提：先证明"抽象不改变行为"，
//       再谈"换数据源"。
//
// 为什么不把 PlantModel 直接改造成 IDeviceIO：
//   PlantModel 是**被控对象**（物理模型），它天然还要提供 set_environment、
//   force_soc、set_pcs_fault 这类"上帝视角"的注入接口，这些**不属于**
//   IDeviceIO 契约（真实设备不接受环境注入）。混在一起会让接口被仿真
//   细节污染。因此采用适配器模式：PlantModel 保持纯净，由本文件做桥接。
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "device_io.h"
#include "plant_model.h"

namespace ems {

class SimDeviceIO : public IDeviceIO {
public:
    SimDeviceIO() = default;
    explicit SimDeviceIO(const PlantConfig& cfg) : plant_(cfg) {}

    // -----------------------------------------------------------------
    // IDeviceIO 实现
    // -----------------------------------------------------------------
    bool read_snapshot(Timestamp now, RealtimeSnapshot& out) override {
        out = plant_.sample(now);
        return plant_.config().data_valid;
    }

    bool read_limits(DeviceLimits& out) override {
        const PlantConfig& pc = plant_.config();
        // 与重构前 EmsRuntime::refresh_device_limits() 完全等价：
        // 先整体复位到默认值（保留 transformer_capacity_kw / d_target_kw 默认），
        // 再由 PCS 额定派生 PCS/BMS 四路限值。
        out = DeviceLimits{};
        out.pcs_rated_chg_kw = pc.pcs_max_chg_kw;
        out.pcs_rated_dis_kw = pc.pcs_max_dis_kw;
        out.bms_chg_limit_kw = pc.pcs_max_chg_kw;
        out.bms_dis_limit_kw = pc.pcs_max_dis_kw;
        out.updated_at       = 0.0;
        return true;
    }

    DeviceStatus read_status() const override {
        const PlantConfig& pc = plant_.config();
        DeviceStatus s;
        s.bms_comm_ok    = pc.comm_ok_bms;
        s.pcs_comm_ok    = pc.comm_ok_pcs;
        s.meter_comm_ok  = pc.comm_ok_meter;
        s.pcs_fault      = pc.pcs_fault;
        s.device_offline = pc.device_offline;
        s.data_valid     = pc.data_valid;
        return s;
    }

    DeviceActuals read_actuals() const override {
        DeviceActuals a;
        a.p_bat_kw      = plant_.p_bat_actual();
        a.p_grid_kw     = plant_.p_grid_actual();
        a.p_load_kw     = plant_.p_load();
        a.p_pv_kw       = plant_.p_pv();
        a.soc           = plant_.soc();
        a.temperature_c = plant_.temperature_c();
        return a;
    }

    // 仿真里"下发"不产生副作用：指令由 execute() 消费（真实 PCS 亦如此，
    // 只是真实系统的 execute() 是一次网络写）。
    bool write_command(const PowerCommand& /*cmd*/) override { return true; }

    double execute(double p_cmd_kw, double dt_s) override {
        return plant_.step(p_cmd_kw, dt_s);
    }

    double battery_capacity_kwh() const override {
        return plant_.config().battery_capacity_kwh;
    }

    const char* name() const override { return "SimDeviceIO(PlantModel)"; }

    // -----------------------------------------------------------------
    // 仿真专属接口（**不属于** IDeviceIO 契约）
    //
    // 仅供测试/演示做环境注入与故障注入。真实适配器不提供这些方法 ——
    // 若算法代码调用了它们，就说明算法没有被真正解耦，编译会失败。
    // 这正是本文件刻意把注入接口与接口契约分开的原因：让"越界"变成
    // 编译错误而不是运行期惊喜。
    // -----------------------------------------------------------------
    PlantModel&       plant()       { return plant_; }
    const PlantModel& plant() const { return plant_; }

    void set_config(const PlantConfig& pc) { plant_.set_config(pc); }
    void set_environment(double p_load_kw, double p_pv_kw) {
        plant_.set_environment(p_load_kw, p_pv_kw);
    }

private:
    PlantModel plant_{};
};

} // namespace ems
