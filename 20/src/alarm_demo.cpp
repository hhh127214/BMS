// =====================================================================
// 20/ — alarm_demo：告警能力 + 持久化 的端到端演示
//
// 做什么：
//   ① 跑一段 24h 仿真的前 600 s（含故障时间窗）
//   ② 把 `StepRecord` 经**仿真入口适配器**喂给 AlarmAssembler
//   ③ 把告警事件落盘成 SOE 文件（soe_store.h）
//   ④ 把一小组参数落盘成配置台账（config_store.h）
//   ⑤ 打印 SOE 汇总，并提示用 soe_dump.exe 复查
//
// 用法：
//   alarm_demo.exe [out_dir]     缺省 out_dir = build
//
// ★ 本演示故意**只**用仿真入口。生产入口（alarm_input_from_production）
//   的等价性由 tests/test_alarm_model.cpp 的 T10/T11 钉住 —— 那里没有
//   Sim24hConfig。两边用的是同一个 AlarmAssembler。
//
// 编译：见 20/scripts/build.bat
// =====================================================================

#include "alarm_model.h"
#include "alarm_assembler.h"
#include "alarm_input.h"
#include "soe_store.h"
#include "config_store.h"

#include "sim_24h.h"        // 演示用仿真；生产入口不需要它

#include <cstdio>
#include <string>
#include <vector>

using namespace ems;

int main(int argc, char** argv) {
    const std::string out_dir = (argc > 1) ? argv[1] : "build";
    const std::string soe_path = out_dir + "/soe.log";
    const std::string cfg_path = out_dir + "/ems_config.ini";

    // 演示每次都从干净的产物开始（SoeStore 本身是**追加**语义：
    // 同路径重开 = 延续上一个文件，这正是"重启不丢"的能力，见 tests/S02）。
    // 想看追加效果：把下面两行去掉再跑一次。
    std::remove(soe_path.c_str());
    std::remove(cfg_path.c_str());

    // ---------------- ① 仿真（600 s，含三个故障窗）----------------
    Sim24hConfig cfg = make_default_24h_config();
    cfg.dt_s       = 1.0;
    cfg.log_every  = 1;
    cfg.duration_s = 600.0;
    cfg.fault_windows.clear();
    cfg.fault_windows.push_back(FaultWindow{100.0, 200.0, 1, "bms_comm_off"});
    cfg.fault_windows.push_back(FaultWindow{300.0, 360.0, 4, "pcs_fault"});
    cfg.fault_windows.push_back(FaultWindow{420.0, 480.0, 2, "meter_off"});

    const Sim24hResult res = run_sim_24h(cfg);
    if (!res.ok) {
        std::printf("[FAIL] sim: %s\n", res.error.c_str());
        return 1;
    }
    std::printf("sim ok: steps=%d log_rows=%d\n", res.steps, res.log_rows);

    // ---------------- ② 告警装配（阈值来自 SafetyParams/DeviceLimits）----------------
    const AlarmContext ctx = alarm_context_from(
        cfg.safety, cfg.limits, cfg.grid_min_required > -1e8);
    AlarmAssembler asm_(ctx);

    // ---------------- ③ 落盘 SOE ----------------
    SoeStore::Config sc;
    sc.memory_capacity = 256;          // 故意设小，证明"内存有界但文件不丢"
    sc.flush_each = true;
    SoeStore soe(sc);
    std::string err;
    if (!soe.open(soe_path, &err)) {
        std::printf("[FAIL] soe open: %s\n", err.c_str());
        return 1;
    }

    int sets = 0, clears = 0;
    for (size_t i = 0; i < res.log.size(); ++i) {
        AlarmInput in;
        alarm_input_from_step_record(res.log[i], i == 0 ? nullptr : &res.log[i - 1], in);
        for (const auto& ev : asm_.update(in)) {
            if (!soe.append(ev, &err)) {
                std::printf("[FAIL] soe append: %s\n", err.c_str());
                return 1;
            }
            if (ev.is_set) ++sets; else ++clears;
        }
    }

    std::printf("alarms: records=%d sets=%d clears=%d\n",
                (int)asm_.size(), sets, clears);
    std::printf("alarm severity histogram (per level):\n");
    for (int i = 4; i >= 0; --i) {
        const AlarmSeverity s = (AlarmSeverity)i;
        int n = 0;
        for (const auto& r : asm_.records()) if (r.severity == s) ++n;
        std::printf("  %-10s %d\n", alarm_severity_name(s), n);
    }
    std::printf("alarm action histogram:\n");
    for (int i = 0; i <= 4; ++i) {
        const AlarmAction act = (AlarmAction)i;
        int n = 0;
        for (const auto& r : asm_.records()) if (r.action == act) ++n;
        std::printf("  %-10s %d\n", alarm_action_name(act), n);
    }
    std::printf("soe: size=%lu memory=%lu dropped_from_memory=%lu\n",
                (unsigned long)soe.size(), (unsigned long)soe.memory_size(),
                (unsigned long)soe.dropped_from_memory());

    // ---------------- ④ 配置落盘（带变更台账）----------------
    ConfigStore cfgst(cfg_path);
    cfgst.require("S04_DEMAND_MGMT", "d_target_kw");
    cfgst.require("S07_PEAK_VALLEY", "P_discharge");
    std::string cerr_;
    cfgst.set("S04_DEMAND_MGMT", "d_target_kw", cfg.limits.d_target_kw, "installer", 0.0, &cerr_);
    cfgst.set("S07_PEAK_VALLEY", "P_discharge", 80.0, "operator", 100.0, &cerr_);
    cfgst.set("S07_PEAK_VALLEY", "P_discharge", 120.0, "operator", 200.0, &cerr_);
    if (!cfgst.save(&cerr_)) {
        std::printf("[FAIL] config save: %s\n", cerr_.c_str());
        return 1;
    }
    std::printf("config: fields=%d changes=%d -> %s\n",
                (int)cfgst.size(), (int)cfgst.changes().size(), cfg_path.c_str());

    // ---------------- ⑤ 回读自证 ----------------
    ConfigStore back(cfg_path);
    back.require("S04_DEMAND_MGMT", "d_target_kw");
    back.require("S07_PEAK_VALLEY", "P_discharge");
    const ConfigLoadResult lr = back.load();
    if (!lr.ok) {
        std::printf("[FAIL] config reload: %s\n", lr.error.c_str());
        return 1;
    }
    double v = 0;
    back.get("S07_PEAK_VALLEY", "P_discharge", &v);
    std::printf("config reload ok: P_discharge=%.0f\n", v);

    std::printf("\nnext: soe_dump.exe --file %s --min-level WARN\n", soe_path.c_str());
    return 0;
}
