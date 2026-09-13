// =====================================================================
// 10/ — 周期 10：EMS 24h 离线仿真测试 · 演示程序
//
// 依据：工商业储能EMS调控策略设计方案.md §7 周期 10
//   「搭建离线仿真环境，导入负荷曲线、光伏曲线、电价时序数据、
//     SOC 初始参数、BMS/PCS/变压器设备参数，完成 24 小时全场景仿真测试，
//     输出功率曲线、SOC 曲线、策略状态、经济收益、告警日志」
//
// 用法：
//   sim_demo.exe                         内置典型日，输出到 build/
//   sim_demo.exe --csv <path>            从 CSV 导入日曲线
//   sim_demo.exe --csv <path> --out <dir>
//   sim_demo.exe --fast                  加速模式（dt=2s，仅演示，不用于验收）
//   sim_demo.exe --fault                 附加故障注入场景（PCS 故障 + 通信中断）
//
// 产物：timeseries.csv / alarms.csv / summary.json / report.html
// =====================================================================

#include "sim_report.h"
#include "sim_24h.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

using namespace ems;

namespace {

// ---- 内置场景：正常典型日 ----
Sim24hConfig scenario_normal() {
    Sim24hConfig c = make_default_24h_config();
    c.title = "EMS 24h 离线仿真 · 典型日（正常）";
    return c;
}

// ---- 内置场景：故障注入日 ----
// 目的：验证"设备异常"下的闭环**安全响应**（而非经济性）。
//   08:00–08:30  PCS 故障        → 应门控到 0，且状态机记录 SOE
//   14:00–14:10  BMS 通信中断    → 数据不可信 → 应降级/门控
//   20:00–20:05  关口电表通信中断 → 并网约束失去量测，应保守
// 每个窗口都是"发生 → 持续 → 恢复"，用于验证**恢复后能重新并网**（不锁死）。
Sim24hConfig scenario_fault() {
    Sim24hConfig c = make_default_24h_config();
    c.title = "EMS 24h 离线仿真 · 故障注入日";
    c.fault_windows = {
        {8.0 * 3600.0,  8.5 * 3600.0,  4, "PCS 故障（停止出力）"},
        {14.0 * 3600.0, 14.1667 * 3600.0, 1, "BMS 通信中断"},
        {20.0 * 3600.0, 20.0833 * 3600.0, 2, "关口电表通信中断"},
    };
    // 演示"上层保持运行许可"：故障恢复后重新带载，展示完整恢复路径。
    // 现场若要求人工确认，把这里置 false 即可（恢复后停在 READY 待机）。
    c.auto_restart_after_fault = true;
    return c;
}

void print_kv(const char* k, double v, const char* u, int prec = 2) {
    std::printf("  %-26s %12.*f %s\n", k, prec, v, u);
}

void print_result(const Sim24hResult& r, const Sim24hConfig& cfg) {
    const auto& e = r.econ;
    const auto& c = r.curves;

    std::printf("\n===== 仿真概要 =====\n");
    std::printf("  步数 %d（%.1f s 步长）/ 日志 %d 行 / 墙钟 %.2f s\n",
                r.steps, cfg.dt_s, r.log_rows, r.wall_s);

    std::printf("\n===== 曲线 =====\n");
    print_kv("曲线点数", c.points, "点", 0);
    print_kv("日用电量", c.e_load_kwh, "kWh", 1);
    print_kv("日发电量(光伏)", c.e_pv_kwh, "kWh", 1);
    print_kv("负荷 min/avg/max", c.load_min_kw, "kW", 1);
    std::printf("  %-26s %12.1f / %.1f kW\n", "             ", c.load_avg_kw, c.load_max_kw);
    print_kv("光伏峰值", c.pv_max_kw, "kW", 1);
    print_kv("电价 min/avg/max", c.price_min, "元/kWh", 3);
    std::printf("  %-26s %12.3f / %.3f 元/kWh\n", "             ", c.price_avg, c.price_max);

    std::printf("\n===== 运行状态 =====\n");
    print_kv("SOC min/max/avg/end", e.soc_min, "", 4);
    std::printf("  %-26s %12.4f / %.4f / %.4f\n", "             ",
                e.soc_max, e.soc_avg, e.soc_end);
    print_kv("SOC 越界拍数", e.soc_violation, "拍", 0);
    print_kv("关口 min/max", r.min_grid_kw, "kW", 1);
    std::printf("  %-26s %12.1f kW\n", "             ", r.max_grid_kw);
    print_kv("最大 |指令|", e.max_abs_cmd_kw, "kW", 1);
    print_kv("最高温度", e.max_temp_c, "°C", 1);
    print_kv("安全限幅拍数", e.safety_clip_ticks, "拍", 0);
    print_kv("门控拍数", e.gated_ticks, "拍", 0);
    print_kv("倒送拍数", e.grid_reverse_ticks, "拍", 0);

    std::printf("\n===== 跟踪质量（LoopMetrics）=====\n");
    print_kv("跟踪 RMSE", r.metrics.rmse_track_kw, "kW", 3);
    print_kv("平均绝对误差", r.metrics.mean_abs_track_err_kw, "kW", 3);
    print_kv("最大绝对误差", r.metrics.max_abs_track_err_kw, "kW", 3);
    print_kv("符号翻转率", r.metrics.flip_rate_per_s, "/s", 4);
    print_kv("方向反转率", r.metrics.reversal_rate_per_s, "/s", 4);
    print_kv("指令总行程", r.metrics.cmd_travel_kw, "kW", 1);
    print_kv("状态迁移次数", static_cast<double>(r.metrics.state_changes), "次", 0);

    std::printf("\n===== 经济性（两部制）=====\n");
    print_kv("购电量", e.e_import_kwh, "kWh", 1);
    print_kv("上网电量", e.e_export_kwh, "kWh", 1);
    print_kv("光伏自用", e.pv_self_use_kwh, "kWh", 1);
    print_kv("储能充电", e.e_charge_kwh, "kWh", 1);
    print_kv("储能放电", e.e_discharge_kwh, "kWh", 1);
    print_kv("等效循环", e.equiv_cycles, "次/日", 3);
    print_kv("最大需量(含储能)", e.peak_grid_kw, "kW", 1);
    print_kv("最大需量(无储能)", e.peak_grid_base_kw, "kW", 1);
    std::printf("  ---- 费用 ----\n");
    print_kv("电度电费(含储能)", e.cost_energy_cny, "元", 1);
    print_kv("需量电费(含储能)", e.cost_demand_cny, "元", 1);
    print_kv("总费用(含储能)", e.cost_total_cny, "元", 1);
    print_kv("总费用(无储能)", e.cost_total_base_cny, "元", 1);
    print_kv("节省-电度", e.saving_energy_cny, "元", 1);
    print_kv("节省-需量", e.saving_demand_cny, "元", 1);
    print_kv("节省合计", e.saving_total_cny, "元", 1);
    print_kv("电池衰减成本", e.cost_degradation_cny, "元", 1);
    print_kv("净收益", e.net_benefit_cny, "元/日", 1);
    print_kv("节省比例", e.saving_pct(), "%", 2);
    {
        const double capex = cfg.econ.battery_capex_cny_per_kwh * cfg.plant.battery_capacity_kwh;
        print_kv("静态回收期(外推)", e.payback_years(capex), "年", 2);
    }

    std::printf("\n===== 不变量校验 =====\n");
    auto inv = [](const char* n, int v) {
        std::printf("  %-34s %8d  %s\n", n, v, v == 0 ? "[PASS]" : "[FAIL]");
    };
    std::printf("  -- 硬不变量（任何场景、任何时刻不得违反）--\n");
    inv("指令逃逸 p_cmd ∉ [p_lower,p_upper]", r.out_of_interval);
    inv("功率超限 |p_cmd| > min(PCS,BMS)", r.over_limit);
    inv("门控失效（非运行态非零指令）", r.gated_nonzero);
    std::printf("  -- 安全不变量（物理量，受能量预算影响）--\n");
    inv("关口越界（稳态窗口）", r.grid_breach);
    if (r.grid_breach_soc_limited > 0)
        std::printf("      其中 %d 拍发生在 SOC 触底/触顶（储能无可用容量，属能量预算结果）\n",
                    r.grid_breach_soc_limited);
    inv("变压器越限（稳态窗口）", r.tr_breach);
    inv("SOC 越界", e.soc_violation);

    std::printf("\n  => 硬不变量：%s\n", r.hard_invariants_ok() ? "全部通过" : "存在违规");
    std::printf("  => 安全不变量：%s\n", r.safety_invariants_ok() && e.soc_violation == 0
                                          ? "全部通过" : "存在越限（见上，属场景结论）");

    if (!cfg.fault_windows.empty()) {
        std::printf("\n===== 故障注入（%d 拍处于故障窗）=====\n", r.fault_ticks);
        for (const auto& w : cfg.fault_windows) {
            std::printf("  %5.2f h – %5.2f h  kind=%d  %s\n",
                        w.t_begin_s / 3600.0, w.t_end_s / 3600.0, w.kind, w.note.c_str());
        }
        std::printf("  上层运行许可保持：%s\n",
                    cfg.auto_restart_after_fault ? "是（恢复后自动带载）"
                                                 : "否（恢复后停在 READY 待机）");
    }

    std::printf("\n===== 告警 =====\n");
    print_kv("告警总数", static_cast<double>(r.alarm_count), "条", 0);
    print_kv("其中 FAULT", static_cast<double>(r.fault_count), "条", 0);
    const size_t show = r.alarms.size() < 12 ? r.alarms.size() : 12;
    for (size_t i = 0; i < show; ++i) {
        const auto& a = r.alarms[i];
        std::printf("  [%7.0fs] %-6s %-12s %s\n",
                    a.t, a.level.c_str(), a.source.c_str(), a.message.c_str());
    }
    if (r.alarms.size() > show)
        std::printf("  ... 其余 %zu 条见 alarms.csv\n", r.alarms.size() - show);
}

} // namespace

int main(int argc, char** argv) {
    Sim24hConfig cfg = scenario_normal();
    std::string out_dir = "build";
    bool fault_mode = false;
    bool fast = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--csv" && i + 1 < argc) {
            cfg.use_csv = true;
            cfg.curves_path = argv[++i];
        } else if (a == "--out" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (a == "--fast") {
            fast = true;
        } else if (a == "--log-every" && i + 1 < argc) {
            cfg.log_every = std::atoi(argv[++i]);
        } else if (a == "--fault") {
            fault_mode = true;
        } else if (a == "--help" || a == "-h") {
            std::printf("usage: sim_demo [--csv <path>] [--out <dir>] [--fast] "
                        "[--log-every N] [--fault]\n");
            return 0;
        }
    }

    if (fault_mode) cfg = scenario_fault();
    if (fast) {
        cfg.dt_s = 2.0;
        cfg.log_every = 5;      // 10 s 日志
        cfg.title += " · 加速模式";
    }
    cfg.out_dir = out_dir;

    std::printf("=== 周期 10 · EMS 24h 离线仿真 ===\n");
    std::printf("曲线来源：%s\n", cfg.use_csv ? cfg.curves_path.c_str() : "内置典型日（96 点 / 15 min）");
    std::printf("设备：电池 %.0f kWh / PCS %.0f kW / 变压器 %.0f kVA / 契约需量 %.0f kW\n",
                cfg.plant.battery_capacity_kwh, cfg.limits.pcs_rated_dis_kw,
                cfg.limits.transformer_capacity_kw, cfg.limits.d_target_kw);
    std::printf("时序：dt=%.1f s / log_every=%d（日志粒度 %.0f s）/ 时长 %.0f h\n",
                cfg.dt_s, cfg.log_every, cfg.dt_s * cfg.log_every, cfg.duration_s / 3600.0);

    Sim24hResult r = run_sim_24h(cfg);
    if (!r.ok) {
        std::printf("[FAIL] 仿真失败：%s\n", r.error.c_str());
        return 1;
    }

    print_result(r, cfg);

    const int n = write_all_reports(out_dir, r, cfg, &r.forecast);
    std::printf("\n===== 产物 =====\n");
    std::printf("  写出 %d/4 个文件到 %s/\n", n, out_dir.c_str());
    std::printf("    timeseries.csv  时序明细\n");
    std::printf("    alarms.csv      告警日志\n");
    std::printf("    summary.json    机器可读汇总\n");
    std::printf("    report.html     单文件可视化报告\n");

    const bool hard_ok = r.hard_invariants_ok();
    std::printf("\n%s\n", hard_ok ? "[SIM OK] 硬不变量全部通过"
                                  : "[SIM FAIL] 硬不变量存在违规（架构级问题）");
    return hard_ok ? 0 : 2;
}
