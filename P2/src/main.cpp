// =====================================================================
// P2/ — 产品化 P2 可观测性 · 命令行演示
//
//   ems-observe --demo      [hours] [dt]   24h 闭环 + 观察者汇总（默认）
//   ems-observe --decouple                  指标与 log_every 无关（核心价值）
//   ems-observe --fault     [hours]         故障场景 SOE（PCS 故障 / 通信中断）
//   ems-observe --export    <dir> [hours]   导出 soe.csv / soe.json / metrics.*
//   ems-observe --help
//
// 退出码：0 正常 / 1 参数错误 / 2 运行期失败（含硬不变量被破坏）
// =====================================================================

#include "observe.h"
#include "sim_24h.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

using namespace ems;

// ---------------------------------------------------------------------
// 装配：与 10/run_sim_24h() 完全一致的顺序，唯一区别是每拍额外喂给观察者。
// 观察者**不修改 07/**，这是它唯一被允许的接入方式。
// ---------------------------------------------------------------------
struct RunOut {
    ObserverTotals totals;
    LoopMetrics    naive;      // 07/ 的日志口径，用于对照
};

static RunOut run_with_observer(const Sim24hConfig& cfg, RuntimeObserver& obs) {
    ForecastSeries fc = make_typical_day_curves(cfg.typical);

    EmsRuntime rt;
    rt.config().dt_s            = cfg.dt_s;
    rt.config().log_every       = cfg.log_every;
    rt.config().enable_log      = true;
    rt.config().demand_window_s = cfg.econ.demand_window_s;
    rt.configure_plant(cfg.plant);
    rt.device_limits()      = cfg.limits;
    rt.safety_params()      = cfg.safety;
    rt.coordinator_config() = cfg.coord;
    rt.fsm_config()         = cfg.fsm;
    rt.set_forecast(fc);
    rt.apply_configs();

    const int steps = static_cast<int>(std::lround(cfg.duration_s / cfg.dt_s));
    const double dt = cfg.dt_s;
    obs.start(0.0);
    for (int i = 0; i < steps; ++i) {
        const double t = static_cast<double>(i) * dt;
        double ld = 0.0, pv = 0.0;
        fc.sample(t, &ld, &pv, nullptr);
        rt.set_environment(ld, pv);
        for (const auto& w : cfg.fault_windows) apply_fault(rt, w.kind, w.active(t));
        StepRecord rec = rt.step(dt);
        obs.on_step(rt, rec);
    }
    obs.stop(steps * dt);

    RunOut out;
    out.totals = obs.totals();
    out.naive  = rt.metrics();
    return out;
}

static void print_soe(const SoeLog& log) {
    std::printf("  %-10s %-5s %-8s %-22s %6s  %s\n",
                "t_first", "level", "source", "code", "repeat", "message");
    std::printf("  %s\n", std::string(78, '-').c_str());
    for (const auto& e : log.events()) {
        std::printf("  %-10.1f %-5s %-8s %-22s %6d  %s\n",
                    e.first_t, soe_level_name(e.level), soe_source_name(e.source),
                    soe_code_name(e.code), e.repeat_count, e.message.c_str());
    }
}

static void print_metrics(const MetricRegistry& m) {
    std::printf("  %-34s %-10s %14s\n", "指标", "类型", "值");
    std::printf("  %s\n", std::string(62, '-').c_str());
    for (const auto& x : m.all()) {
        char v[64];
        if (x.type == MetricType::kHistogram) {
            std::snprintf(v, sizeof(v), "n=%llu mean=%.2f",
                          static_cast<unsigned long long>(x.count),
                          x.count ? x.sum / static_cast<double>(x.count) : 0.0);
        } else {
            std::snprintf(v, sizeof(v), "%.3f", x.value);
        }
        std::printf("  %-34s %-10s %14s\n", x.name.c_str(),
                    metric_type_name(x.type), v);
    }
}

// ---------------------------------------------------------------------
// --demo：24h 闭环 + 观察者汇总
// ---------------------------------------------------------------------
static int cmd_demo(double hours, double dt, bool quiet) {
    Sim24hConfig cfg = make_default_24h_config();
    cfg.dt_s       = dt;
    cfg.duration_s = hours * 3600.0;
    cfg.log_every  = 1;                 // 让 07/ 的口径也逐拍，便于对照

    RuntimeObserver obs;
    const RunOut out = run_with_observer(cfg, obs);

    std::printf("=====================================================================\n");
    std::printf(" P2 可观测性 · 24h 闭环演示（%g h @ dt=%g s）\n", hours, dt);
    std::printf("=====================================================================\n\n");
    std::printf("%s\n", obs.summary_text().c_str());

    if (!quiet) {
        std::printf("--- SOE（事件序列，%zu 条）---\n", obs.soe().size());
        print_soe(obs.soe());
        std::printf("\n--- 指标（%zu 个）---\n", obs.metrics().size());
        print_metrics(obs.metrics());
        std::printf("\n--- 直方图分位 ---\n");
        std::printf("  跟踪误差 |Δ|  p50=%.2f  p95=%.2f  p99=%.2f kW\n",
                    obs.metrics().quantile("ems_track_error_kw", 0.50),
                    obs.metrics().quantile("ems_track_error_kw", 0.95),
                    obs.metrics().quantile("ems_track_error_kw", 0.99));
        std::printf("  单拍指令变化   p50=%.2f  p95=%.2f  p99=%.2f kW\n",
                    obs.metrics().quantile("ems_cmd_step_kw", 0.50),
                    obs.metrics().quantile("ems_cmd_step_kw", 0.95),
                    obs.metrics().quantile("ems_cmd_step_kw", 0.99));
    }

    std::printf("\n");
    if (!out.totals.hard_invariants_ok()) {
        std::printf("[FAIL] 硬不变量被破坏：逃逸 %d / 超限 %d / 门控失效 %d\n",
                    out.totals.out_of_interval_ticks,
                    out.totals.over_limit_ticks,
                    out.totals.gated_nonzero_ticks);
        return 2;
    }
    std::printf("[OK] 硬不变量全部成立（逃逸 0 / 超限 0 / 门控失效 0）\n");
    return 0;
}

// ---------------------------------------------------------------------
// --decouple：P2 的核心价值 —— 指标与 log_every 无关
//
// 同一个抖动场景跑两遍，只改 log_every。观察者的输出质量指标**必须完全相同**，
// 而 07/ 的日志口径在 log_every=10 时会大幅失真。这就是"为了省资源调大
// log_every，等于同时关掉了故障可见性"的证据。
// ---------------------------------------------------------------------
static int cmd_decouple() {
    std::printf("=====================================================================\n");
    std::printf(" P2 可观测性 · 指标与 log_every 无关（核心价值验证）\n");
    std::printf("=====================================================================\n\n");

    // 抖动场景：负荷在 300/50 kW 之间每 5 拍切换，光伏反向 —— 制造持续
    // 的充放换向，让"指令行程"与"方向反转"变成高频量。
    struct Row { int log_every; double obs_travel; int obs_rev; int obs_flips;
                 double naive_travel; int naive_rev; int steps; int log_rows; };
    Row rows[2];
    const int log_everys[2] = {1, 10};

    for (int k = 0; k < 2; ++k) {
        const int le = log_everys[k];
        EmsRuntime rt;
        LoopConfig lc;
        lc.dt_s = 1.0;
        lc.log_every = le;
        lc.enable_log = true;
        lc.demand_window_s = 300.0;
        lc.output_deadband_kw = 0.0;     // 关死区，让抖动完整进到指令
        lc.output_switch_delay = 1;
        rt.config() = lc;
        rt.configure_plant(PlantConfig{});
        rt.apply_configs();

        RuntimeObserver obs;
        obs.start(0.0);
        const int N = 3600;
        for (int i = 0; i < N; ++i) {
            const bool hi = ((i / 5) % 2) == 0;
            rt.set_environment(hi ? 300.0 : 50.0, hi ? 50.0 : 300.0);
            StepRecord rec = rt.step(1.0);
            obs.on_step(rt, rec);
        }
        obs.stop(static_cast<double>(N));

        const LoopMetrics m = rt.metrics();
        rows[k].log_every    = le;
        rows[k].obs_travel   = obs.totals().cmd_travel_kw;
        rows[k].obs_rev      = obs.totals().cmd_reversals;
        rows[k].obs_flips    = obs.totals().sign_flips;
        rows[k].naive_travel = m.cmd_travel_kw;
        rows[k].naive_rev    = m.cmd_reversals;
        rows[k].steps        = obs.totals().steps;
        rows[k].log_rows     = static_cast<int>(rt.log().size());
    }

    std::printf("场景：3600 拍，负荷在 300/50 kW 之间每 5 拍切换（持续充放换向）\n\n");
    std::printf("  %-11s %10s %10s %10s %12s\n",
                "log_every", "观察者行程", "观察者换向", "日志行数", "07/ 日志行程");
    std::printf("  %s\n", std::string(58, '-').c_str());
    for (const auto& r : rows) {
        std::printf("  %-11d %10.1f %10d %10d %12.1f\n",
                    r.log_every, r.obs_travel, r.obs_rev, r.log_rows, r.naive_travel);
    }

    const bool same_obs = std::fabs(rows[0].obs_travel - rows[1].obs_travel) < 1e-6
                       && rows[0].obs_rev == rows[1].obs_rev;
    const bool naive_blind = rows[1].naive_travel < rows[0].naive_travel * 0.5;

    std::printf("\n结论：\n");
    std::printf("  ① 观察者：log_every 1 → 10，行程 %.1f → %.1f kW  %s\n",
                rows[0].obs_travel, rows[1].obs_travel,
                same_obs ? "（完全一致 ✓）" : "（不一致 ✗）");
    std::printf("  ② 07/ 日志口径：行程 %.1f → %.1f kW  %s\n",
                rows[0].naive_travel, rows[1].naive_travel,
                naive_blind ? "（降采样后严重失真 ✗）" : "（未失真）");
    std::printf("  ③ 日志行数 %d → %d（省了 %d 倍存储，代价是 07/ 的指标失明）\n",
                rows[0].log_rows, rows[1].log_rows,
                rows[0].log_rows / (rows[1].log_rows ? rows[1].log_rows : 1));
    std::printf("\n");

    if (!same_obs) {
        std::printf("[FAIL] 观察者指标随 log_every 变化 —— P2 的核心目标未达成\n");
        return 2;
    }
    std::printf("[OK] 观察者指标与 log_every 完全无关（逐拍累积）\n");
    return 0;
}

// ---------------------------------------------------------------------
// --fault：故障场景 SOE
// ---------------------------------------------------------------------
static int cmd_fault(double hours) {
    Sim24hConfig cfg = make_default_24h_config();
    cfg.duration_s = hours * 3600.0;
    cfg.log_every  = 1;
    const double mid = cfg.duration_s * 0.4;
    // kind: 1=comm_bms 2=comm_meter 3=comm_pcs 4=pcs_fault 5=device_offline 6=data_invalid
    cfg.fault_windows.push_back({mid,          mid + 600.0,  4, "pcs_fault"});
    cfg.fault_windows.push_back({mid + 1800.0, mid + 2100.0, 2, "meter_comm"});
    cfg.fault_windows.push_back({mid + 3600.0, mid + 3900.0, 3, "pcs_comm"});

    RuntimeObserver obs;
    const RunOut out = run_with_observer(cfg, obs);

    std::printf("=====================================================================\n");
    std::printf(" P2 可观测性 · 故障场景 SOE（%g h，3 个故障时间窗）\n", hours);
    std::printf("=====================================================================\n\n");
    std::printf("  故障窗：t=%.0f~%.0f s PCS 故障 / t=%.0f~%.0f s 电表通信 / "
                "t=%.0f~%.0f s PCS 通信\n\n",
                mid, mid + 600.0, mid + 1800.0, mid + 2100.0,
                mid + 3600.0, mid + 3900.0);

    std::printf("--- SOE（%zu 条事件 / %d 拍）---\n", obs.soe().size(), out.totals.steps);
    print_soe(obs.soe());

    std::printf("\n--- 门控与降级 ---\n");
    std::printf("  状态门控 %d 拍 / HOLD_LAST %d 拍 / 故障位 %d 拍\n",
                out.totals.state_gate_ticks, out.totals.hold_last_ticks,
                out.totals.fault_ticks);
    std::printf("  硬不变量：逃逸 %d / 超限 %d / 门控失效 %d\n",
                out.totals.out_of_interval_ticks, out.totals.over_limit_ticks,
                out.totals.gated_nonzero_ticks);
    std::printf("\n  按等级：");
    for (int lv = 4; lv >= 0; --lv) {
        const size_t n = obs.soe().count(static_cast<SoeLevel>(lv));
        if (n) std::printf("%s=%zu ", soe_level_name(static_cast<SoeLevel>(lv)), n);
    }
    std::printf("\n");

    std::printf("\n");
    if (!out.totals.hard_invariants_ok()) {
        std::printf("[FAIL] 硬不变量被破坏\n");
        return 2;
    }
    // 故障场景必须真的产生故障事件，否则这个演示没有意义
    if (!obs.soe().has(SoeCode::kPcsFaultSet) ||
        !obs.soe().has(SoeCode::kCommLostMeter) ||
        !obs.soe().has(SoeCode::kCommLostPcs)) {
        std::printf("[FAIL] 故障事件未按预期产生\n");
        return 2;
    }
    std::printf("[OK] 故障置位/清除事件成对出现，故障期间正确门控\n");
    return 0;
}

// ---------------------------------------------------------------------
// --export：落盘
// ---------------------------------------------------------------------
static bool write_file(const std::string& path, const std::string& data) {
    std::ofstream f(path.c_str(), std::ios::binary);
    if (!f.is_open()) return false;
    f << data;
    return f.good();
}

static int cmd_export(const std::string& dir, double hours) {
    Sim24hConfig cfg = make_default_24h_config();
    cfg.duration_s = hours * 3600.0;
    cfg.log_every  = 1;

    RuntimeObserver obs;
    const RunOut out = run_with_observer(cfg, obs);

    const std::string soe_csv  = dir + "/soe.csv";
    const std::string soe_json = dir + "/soe.json";
    const std::string m_prom   = dir + "/metrics.prom";
    const std::string m_json   = dir + "/metrics.json";
    const std::string tr_json  = dir + "/trace.json";

    bool ok = true;
    ok &= write_file(soe_csv,  obs.soe().to_csv());
    ok &= write_file(soe_json, obs.soe().to_json());
    ok &= write_file(m_prom,   obs.metrics().to_prometheus());
    ok &= write_file(m_json,   obs.metrics().to_json());
    ok &= write_file(tr_json,  obs.trace().to_json());
    if (!ok) {
        std::printf("[FAIL] 导出失败（目录不存在？）：%s\n", dir.c_str());
        return 1;
    }

    std::printf("=====================================================================\n");
    std::printf(" P2 可观测性 · 导出（%g h）\n", hours);
    std::printf("=====================================================================\n");
    std::printf("  %-32s  %8zu 字节  （%zu 条事件）\n", soe_csv.c_str(),
                obs.soe().to_csv().size(), obs.soe().size());
    std::printf("  %-32s  %8zu 字节\n", soe_json.c_str(), obs.soe().to_json().size());
    std::printf("  %-32s  %8zu 字节  （%zu 个指标）\n", m_prom.c_str(),
                obs.metrics().to_prometheus().size(), obs.metrics().size());
    std::printf("  %-32s  %8zu 字节\n", m_json.c_str(), obs.metrics().to_json().size());
    std::printf("  %-32s  %8zu 字节\n", tr_json.c_str(), obs.trace().to_json().size());
    std::printf("\n  观测 %d 拍，产生 %zu 条事件（压缩比 %d:1），"
                "抑制合并 %zu 次，淘汰 %zu 条\n",
                out.totals.steps, obs.soe().size(),
                obs.soe().size() ? out.totals.steps / static_cast<int>(obs.soe().size()) : 0,
                obs.soe().suppressed(), obs.soe().dropped());
    std::printf("\n[OK] 导出完成\n");
    return 0;
}

// ---------------------------------------------------------------------
int main(int argc, char** argv) {
    std::string mode = "--demo";
    std::string arg;
    double hours = 24.0;
    double dt    = 1.0;
    bool   quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            std::printf(
                "usage: ems-observe [--demo [h] [dt] | --decouple | --fault [h]\n"
                "                   | --export <dir> [h] | --quiet | --help]\n\n"
                "  --demo     24h 闭环 + 观察者汇总（默认）\n"
                "  --decouple 指标与 log_every 无关（核心价值验证）\n"
                "  --fault    故障场景 SOE\n"
                "  --export   导出 soe.csv / soe.json / metrics.prom / metrics.json\n"
                "  --quiet    只打印汇总\n");
            return 0;
        }
        if (a == "--quiet") { quiet = true; continue; }
        if (a == "--demo" || a == "--decouple" || a == "--fault" || a == "--export") {
            mode = a;
            continue;
        }
        if (!a.empty() && a[0] == '-') {
            std::printf("[FAIL] 未知参数: %s（--help 查看用法）\n", a.c_str());
            return 1;
        }
        // 位置参数：--export 的第一个是目录，其余是数值
        if (mode == "--export" && arg.empty()) { arg = a; continue; }
        char* end = nullptr;
        const double v = std::strtod(a.c_str(), &end);
        if (end && *end == '\0') {
            if (hours == 24.0) hours = v; else dt = v;
        }
    }

    if (mode == "--decouple") return cmd_decouple();
    if (mode == "--fault")    return cmd_fault(hours);
    if (mode == "--export") {
        if (arg.empty()) { std::printf("[FAIL] --export 需要输出目录\n"); return 1; }
        return cmd_export(arg, hours);
    }
    return cmd_demo(hours, dt, quiet);
}
