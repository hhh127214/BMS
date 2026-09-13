// =====================================================================
// P1/ — 产品化 P1 配置化 · 演示程序
//
// 依据：docs/产品化/P0-架构分层.md §「P1 配置化」
//   目标：**现场部署不改源码，只改配置文件。**
//
// 本程序要证明的三件事（对应 P1 的三条验收线）：
//   ① 配置文件 → 运行时：`--apply` 一条命令完成装配（顺序由 P1 负责）
//   ② 改配置 → 行为改变：`--demo` 做 A/B 对照，用数据证明参数真的驱动行为
//   ③ 存取往返不漂移：`--roundtrip` 校验"读一份 / 写一份"没有字段丢失
//
// 用法：
//   ems-config --demo                       内置场景 A/B 对照（默认）
//   ems-config --apply <file>               加载配置 → 校验 → 装配 → 跑 24h
//   ems-config --check <file>               只做语法 + 语义校验
//   ems-config --roundtrip <file>           存取往返一致性校验
//   ems-config --dump-template [> file]     生成带注释的配置模板
//   ems-config --dump-doc [> file]          生成配置说明（Markdown）
//   ems-config --dump-schema [> file]       生成字段清单（机器可读 JSON）
//   ems-config --capture <out.json>         导出运行中的当前配置
//   ems-config --save-default <out.json>    导出内置默认配置
//   选项：--hours N（默认 24）--fast（dt=2s）--quiet
//
// 退出码：0 正常 / 1 配置错误 / 2 行为验证失败
// =====================================================================

#include "config_doc.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace ems;

namespace {

// =====================================================================
// 合成日曲线（P1 自带，刻意不依赖 10/ —— P1 是产品化步骤，不应反向依赖周期产物）
// =====================================================================
double synth_load_kw(double h) {
    const double base = 170.0;                                       // 基础负荷
    const double day  = 150.0 * std::exp(-std::pow((h - 14.0) / 4.0, 2.0));
    const double eve  =  95.0 * std::exp(-std::pow((h - 19.5) / 1.8, 2.0));
    const double night=  40.0 * std::exp(-std::pow((h -  3.0) / 2.5, 2.0));
    return base + day + eve + night;
}

double synth_pv_kw(double h) {
    if (h < 6.0 || h > 18.5) return 0.0;
    return 230.0 * std::exp(-std::pow((h - 12.5) / 3.2, 2.0));
}

// TOU 电价（谷 / 平 / 峰），与负荷曲线同源，保证优化器有可用的日前计划
double synth_price(double h) {
    if (h < 8.0 || h >= 22.0)      return 0.30;   // 谷
    if (h >= 10.0 && h < 12.0)     return 1.10;   // 峰
    if (h >= 17.0 && h < 21.0)     return 1.10;   // 峰
    return 0.65;                                   // 平
}

// 合成日前预测（15 min 阶梯，96 点）—— 优化器据此生成计划
ForecastSeries make_forecast(double step_s = 900.0) {
    ForecastSeries fc;
    fc.step_s = step_s;
    const int n = static_cast<int>(std::lround(86400.0 / step_s));
    for (int i = 0; i < n; ++i) {
        const double h = (static_cast<double>(i) * step_s) / 3600.0;
        fc.p_load_kw.push_back(synth_load_kw(h));
        fc.p_pv_kw.push_back(synth_pv_kw(h));
        fc.price.push_back(synth_price(h));
    }
    fc.loaded = true;
    return fc;
}

// =====================================================================
// 内置基线配置：一个典型的 500 kWh / 250 kW 工商业储能站
//
// 注意这里**只写"站点相关"的值**；其余全部沿用各层结构体的默认值 ——
// 这正是配置化要达到的效果：一份配置只描述"这个站和默认有什么不同"。
// =====================================================================
EmsConfig make_base_config() {
    EmsConfig c;
    c.name        = "site-500kwh-demo";
    c.description = "P1 演示基线：500 kWh / 250 kW 工商业储能站";
    c.site        = "示例园区 1# 配电房";
    c.version     = "1.0";

    // ---- 实时循环 ----
    c.loop.dt_s              = 1.0;
    c.loop.enable_log        = true;
    c.loop.log_every         = 1;          // 演示要精确统计，不降采样
    c.loop.demand_window_s   = 900.0;      // 国内需量计量 15 min
    c.loop.l2_correction_max_kw = 80.0;    // 与协同层纠偏上限对齐（避免二次限幅）

    // ---- 被控对象（现场由真实设备提供；仿真装配用）----
    c.plant.battery_capacity_kwh = 500.0;
    c.plant.soc_init             = 0.50;
    c.plant.soc_phys_min         = 0.05;
    c.plant.soc_phys_max         = 0.98;
    c.plant.pcs_max_chg_kw       = 250.0;
    c.plant.pcs_max_dis_kw       = 250.0;
    c.plant.pcs_ramp_kw_per_s    = 250.0;
    c.plant.pcs_tau_s            = 0.8;
    c.plant.pcs_deadtime_s       = 0.4;
    c.plant.pcs_standby_kw       = 2.0;
    c.plant.noise_kw             = 0.0;    // 演示要可复现，关掉噪声
    c.plant.noise_soc            = 0.0;
    c.plant.seed                 = 20260912u;

    // ---- 设备限制 ----
    c.limits.pcs_rated_chg_kw      = 250.0;
    c.limits.pcs_rated_dis_kw      = 250.0;
    c.limits.bms_chg_limit_kw      = 250.0;
    c.limits.bms_dis_limit_kw      = 250.0;
    c.limits.transformer_capacity_kw = 630.0;
    c.limits.d_target_kw           = 320.0;

    // ---- 安全约束 ----
    c.safety.soc_min      = 0.10;
    c.safety.soc_max      = 0.95;
    c.safety.soc_warn_low = 0.15;
    c.safety.soc_warn_high= 0.90;
    c.safety.grid_p_min_kw = -30.0;        // 允许少量倒送（余电上网）
    c.safety.grid_p_max_kw = 630.0;        // 关口上限 = 变压器容量
    c.safety.ramp_kw_per_s = 250.0;
    c.safety.grid_lookahead_max_drop_kw = 60.0;   // 周期 10 引入的前瞻置信上限

    // ---- 优化协同 ----
    c.coord.reopt_period_s = 900.0;

    // ---- 状态机 ----
    c.fsm.allow_ready_output = false;

    return c;
}

// =====================================================================
// 运行统计（从 rt.log() 提取的可观测指标）
// =====================================================================
struct RunStats {
    int    steps      = 0;
    int    log_rows   = 0;
    double min_grid   =  1e18;
    double max_grid   = -1e18;
    double load_peak  = 0.0;
    double pv_peak    = 0.0;
    double e_import   = 0.0;   // kWh
    double e_export   = 0.0;   // kWh
    double e_chg      = 0.0;
    double e_dis      = 0.0;
    double soc_min    = 1e18;
    double soc_max    = -1e18;
    double soc_end    = 0.0;
    double max_abs_cmd= 0.0;
    int    gated      = 0;
    int    clipped    = 0;
    int    reversed   = 0;
    int    hold_last  = 0;
    int    out_of_interval = 0;
};

RunStats collect(const EmsRuntime& rt, double dt) {
    RunStats s;
    const auto& log = rt.log();
    s.log_rows = static_cast<int>(log.size());
    for (const auto& r : log) {
        s.min_grid = std::min(s.min_grid, r.p_grid);
        s.max_grid = std::max(s.max_grid, r.p_grid);
        s.load_peak = std::max(s.load_peak, r.p_load);
        s.pv_peak   = std::max(s.pv_peak,   r.p_pv);
        s.soc_min   = std::min(s.soc_min, r.soc);
        s.soc_max   = std::max(s.soc_max, r.soc);
        s.soc_end   = r.soc;
        s.max_abs_cmd = std::max(s.max_abs_cmd, std::fabs(r.p_cmd));
        if (r.p_grid > 0.0) s.e_import += r.p_grid * dt / 3600.0;
        else                s.e_export += -r.p_grid * dt / 3600.0;
        if (r.p_actual < 0.0) s.e_chg += -r.p_actual * dt / 3600.0;
        else                  s.e_dis +=  r.p_actual * dt / 3600.0;
        if (r.state_gated) s.gated++;
        if (r.safety_clip) s.clipped++;
        if (r.hold_last)   s.hold_last++;
        if (r.p_grid < 0.0) s.reversed++;
        if (r.p_cmd < r.p_lower - 1e-9 || r.p_cmd > r.p_upper + 1e-9) s.out_of_interval++;
    }
    if (s.log_rows == 0) { s.min_grid = s.max_grid = s.soc_min = s.soc_max = 0.0; }
    return s;
}

// 装配一个新运行时并跑指定时长
struct RunOutcome {
    bool                   ok = false;
    std::string            error;
    RunStats               stats;
    ConfigDiagnostics      diag;
    double                 wall_s = 0.0;
};

RunOutcome run_scenario(const EmsConfig& cfg, double hours, double dt) {
    RunOutcome out;

    EmsRuntime rt;
    rt.init();

    ApplyOptions opt;
    opt.inject_plant = true;
    out.diag = apply_config(rt, cfg, opt);
    if (!out.diag.ok()) {
        out.error = "配置装配失败";
        return out;
    }

    const int n = static_cast<int>(std::lround(hours * 3600.0 / dt));
    const auto t0 = std::chrono::steady_clock::now();

    // 注入日前预测：优化器据此排计划（无预测时优化层退化为"不动作"）
    ForecastSeries fc = make_forecast(900.0);
    rt.set_forecast(fc);

    rt.run(n, dt, [dt](EmsRuntime& rr, int i) {
        const double t = static_cast<double>(i) * dt;
        const double h = std::fmod(t, 86400.0) / 3600.0;
        rr.set_environment(synth_load_kw(h), synth_pv_kw(h));
    });
    const auto t1 = std::chrono::steady_clock::now();

    out.wall_s = std::chrono::duration<double>(t1 - t0).count();
    out.stats  = collect(rt, dt);
    out.stats.steps = n;
    out.ok = true;
    return out;
}

// =====================================================================
// 输出小工具
// =====================================================================
void kv(const char* k, double v, const char* u, int prec = 2) {
    std::printf("  %-30s %12.*f %s\n", k, prec, v, u);
}

void print_diag(const ConfigDiagnostics& d) {
    for (const auto& i : d.issues) std::printf("  %s\n", i.to_string().c_str());
    std::printf("  -> %zu 错误 / %zu 警告\n", d.error_count(), d.warning_count());
}

// =====================================================================
// 子命令：--demo（A/B 对照，证明"改配置 = 改行为"）
// =====================================================================
int cmd_demo(double hours, double dt, bool quiet) {
    std::printf("=== P1 配置化 · A/B 对照演示 ===\n");
    std::printf("目的：证明「只改配置、不改源码」就能改变系统行为。\n\n");

    EmsConfig a = make_base_config();
    EmsConfig b = a;                                  // 只改 2 个参数

    // 改动 1：关口功率上限 630 → 200 kW（硬约束收紧）
    b.limits.transformer_capacity_kw = 200.0;
    b.safety.grid_p_max_kw           = 200.0;
    // 改动 2：契约需量 320 → 200 kW（经济约束收紧）
    b.limits.d_target_kw             = 200.0;
    b.name = "site-500kwh-demo-tight";

    std::printf("--- 配置差异（仅这两处）---\n");
    std::printf("  limits.transformer_capacity_kw : %.0f  ->  %.0f\n",
                a.limits.transformer_capacity_kw, b.limits.transformer_capacity_kw);
    std::printf("  limits.d_target_kw             : %.0f  ->  %.0f\n",
                a.limits.d_target_kw, b.limits.d_target_kw);
    std::printf("  safety.grid_p_max_kw           : %.0f  ->  %.0f\n\n",
                a.safety.grid_p_max_kw, b.safety.grid_p_max_kw);

    RunOutcome ra = run_scenario(a, hours, dt);
    if (!ra.ok) { std::printf("[FAIL] 基线场景：%s\n", ra.error.c_str()); print_diag(ra.diag); return 2; }
    RunOutcome rb = run_scenario(b, hours, dt);
    if (!rb.ok) { std::printf("[FAIL] 收紧场景：%s\n", rb.error.c_str()); print_diag(rb.diag); return 2; }

    const auto& sa = ra.stats;
    const auto& sb = rb.stats;

    std::printf("--- 运行结果（各 %.0f h / dt=%.1f s）---\n", hours, dt);
    std::printf("  %-30s %14s %14s %10s\n", "指标", "基线(630)", "收紧(200)", "变化");
    auto row = [](const char* n, double x, double y, int prec, const char* u) {
        std::printf("  %-30s %14.*f %14.*f %+10.*f %s\n", n, prec, x, prec, y, prec, y - x, u);
    };
    row("关口峰值",      sa.max_grid,    sb.max_grid,    1, "kW");
    row("关口谷值",      sa.min_grid,    sb.min_grid,    1, "kW");
    row("购电量",        sa.e_import,    sb.e_import,    1, "kWh");
    row("上网电量",      sa.e_export,    sb.e_export,    1, "kWh");
    row("储能充电量",    sa.e_chg,       sb.e_chg,       1, "kWh");
    row("储能放电量",    sa.e_dis,       sb.e_dis,       1, "kWh");
    row("SOC 最低",      sa.soc_min,     sb.soc_min,     4, "");
    row("SOC 最高",      sa.soc_max,     sb.soc_max,     4, "");
    row("最大|指令|",    sa.max_abs_cmd, sb.max_abs_cmd, 1, "kW");
    std::printf("  %-30s %14d %14d %+10d 拍\n", "安全限幅拍数", sa.clipped, sb.clipped, sb.clipped - sa.clipped);
    std::printf("  %-30s %14d %14d %+10d 拍\n", "倒送拍数",     sa.reversed, sb.reversed, sb.reversed - sa.reversed);
    std::printf("  %-30s %14d %14d %+10d 拍\n", "指令逃逸拍数", sa.out_of_interval, sb.out_of_interval, sb.out_of_interval - sa.out_of_interval);

    // 判定：收紧配置必须真的收紧行为
    const bool grid_tightened = sb.max_grid < sa.max_grid - 1e-6;
    std::printf("\n--- 判定 ---\n");
    std::printf("  关口峰值是否随配置收紧而下降：%s（%.1f -> %.1f kW）\n",
                grid_tightened ? "是" : "否",
                sa.max_grid, sb.max_grid);
    std::printf("  硬不变量（指令逃逸）是否保持为 0：%s\n",
                (sa.out_of_interval == 0 && sb.out_of_interval == 0) ? "是" : "否");
    if (!quiet) {
        std::printf("  墙钟耗时：基线 %.2f s / 收紧 %.2f s\n", ra.wall_s, rb.wall_s);
    }

    const bool pass = grid_tightened && sa.out_of_interval == 0 && sb.out_of_interval == 0;
    std::printf("\n%s\n", pass ? "[DEMO OK] 配置驱动行为，且硬不变量未被破坏"
                              : "[DEMO FAIL] 配置未按预期驱动行为");
    return pass ? 0 : 2;
}

// =====================================================================
// 子命令：--apply
// =====================================================================
int cmd_apply(const std::string& path, double hours, double dt, bool quiet) {
    std::printf("=== P1 配置化 · 加载并装配 ===\n");
    std::printf("配置文件：%s\n\n", path.c_str());

    EmsConfig cfg;
    ConfigDiagnostics d;
    if (!load_config_file(path, &cfg, &d)) {
        std::printf("[FAIL] 加载失败\n");
        print_diag(d);
        return 1;
    }
    std::printf("--- 语法/类型检查 ---\n");
    print_diag(d);
    if (!d.ok()) return 1;

    ConfigDiagnostics sv = validate(cfg);
    std::printf("\n--- 语义校验 ---\n");
    print_diag(sv);
    if (!sv.ok()) return 1;

    std::printf("\n--- 配置概要 ---\n");
    std::printf("  name=%s  site=%s  version=%s\n",
                cfg.name.c_str(), cfg.site.c_str(), cfg.version.c_str());
    kv("电池容量", cfg.plant.battery_capacity_kwh, "kWh", 0);
    kv("PCS 额定充/放", cfg.limits.pcs_rated_chg_kw, "kW", 0);
    kv("变压器容量", cfg.limits.transformer_capacity_kw, "kVA", 0);
    kv("契约需量", cfg.limits.d_target_kw, "kW", 0);
    kv("SOC 下限/上限", cfg.safety.soc_min, "", 3);
    std::printf("  %-30s %12.3f\n", "              上限", cfg.safety.soc_max);
    kv("策略条目数", static_cast<double>(cfg.strategies.size()), "条", 0);

    RunOutcome r = run_scenario(cfg, hours, dt);
    if (!r.ok) {
        std::printf("\n[FAIL] %s\n", r.error.c_str());
        print_diag(r.diag);
        return 1;
    }
    if (!r.diag.issues.empty()) {
        std::printf("\n--- 装配期诊断 ---\n");
        print_diag(r.diag);
    }

    const auto& s = r.stats;
    std::printf("\n--- 运行结果（%.0f h / dt=%.1f s / %d 拍）---\n", hours, dt, s.steps);
    kv("关口峰值", s.max_grid, "kW", 1);
    kv("关口谷值", s.min_grid, "kW", 1);
    kv("购电量",   s.e_import, "kWh", 1);
    kv("上网电量", s.e_export, "kWh", 1);
    kv("储能充电量", s.e_chg, "kWh", 1);
    kv("储能放电量", s.e_dis, "kWh", 1);
    kv("SOC 最低", s.soc_min, "", 4);
    kv("SOC 最高", s.soc_max, "", 4);
    kv("SOC 结束", s.soc_end, "", 4);
    kv("最大|指令|", s.max_abs_cmd, "kW", 1);
    std::printf("  %-30s %12d 拍\n", "安全限幅拍数", s.clipped);
    std::printf("  %-30s %12d 拍\n", "倒送拍数", s.reversed);
    std::printf("  %-30s %12d 拍\n", "指令逃逸拍数", s.out_of_interval);
    if (!quiet) kv("墙钟耗时", r.wall_s, "s", 2);

    const bool pass = (s.out_of_interval == 0);
    std::printf("\n%s\n", pass ? "[APPLY OK] 配置装配成功，硬不变量保持"
                               : "[APPLY FAIL] 出现指令逃逸（架构级问题）");
    return pass ? 0 : 2;
}

// =====================================================================
// 子命令：--check
// =====================================================================
int cmd_check(const std::string& path) {
    std::printf("=== P1 配置化 · 校验 ===\n");
    std::printf("配置文件：%s\n\n", path.c_str());

    EmsConfig cfg;
    ConfigDiagnostics d;
    const bool loaded = load_config_file(path, &cfg, &d);
    std::printf("--- 语法/类型检查 ---\n");
    print_diag(d);
    if (!loaded) return 1;

    ConfigDiagnostics sv = validate(cfg);
    std::printf("\n--- 语义校验 ---\n");
    print_diag(sv);

    // 策略 id 是否存在（需要运行时注册表）
    EmsRuntime rt;
    rt.init();
    ConfigDiagnostics ad = apply_config(rt, cfg, ApplyOptions());
    std::printf("\n--- 装配检查（策略 id / 参数）---\n");
    print_diag(ad);

    const bool ok = d.ok() && sv.ok() && ad.ok();
    std::printf("\n%s\n", ok ? "[CHECK OK] 配置可用" : "[CHECK FAIL] 配置存在问题");
    return ok ? 0 : 1;
}

// =====================================================================
// 子命令：--roundtrip（存取往返一致性）
//
// 这是"一张绑定表驱动 load/save"这个设计的**验收测试**：
//   cfg → JSON → cfg' ，逐字段比较，任何字段丢失/漂移都会被抓出来。
// =====================================================================
int cmd_roundtrip(const std::string& path) {
    std::printf("=== P1 配置化 · 存取往返一致性 ===\n");
    std::printf("配置文件：%s\n\n", path.c_str());

    EmsConfig a;
    ConfigDiagnostics d;
    if (!load_config_file(path, &a, &d)) {
        std::printf("[FAIL] 加载失败\n");
        print_diag(d);
        return 1;
    }
    if (!d.ok()) { print_diag(d); return 1; }

    // 往返 1：结构体 → JSON → 结构体
    json::Value j1 = to_json(a);
    ConfigDiagnostics d1;
    EmsConfig b = from_json(j1, &d1);
    if (!d1.ok()) {
        std::printf("[FAIL] 二次解析失败\n");
        print_diag(d1);
        return 1;
    }

    // 往返 2：再走一遍，检查是否稳定（幂等）
    json::Value j2 = to_json(b);
    const std::string s1 = json::dump(j1, 2);
    const std::string s2 = json::dump(j2, 2);
    const bool idempotent = (s1 == s2);

    // 逐字段比较：复用 to_json 的结果做文本比对即可覆盖全部字段
    // （因为 to_json 遍历的就是绑定表里的每一个字段）
    std::printf("--- 结果 ---\n");
    std::printf("  字段文本长度：第一次 %zu / 第二次 %zu\n", s1.size(), s2.size());
    std::printf("  往返幂等（dump 两次一致）：%s\n", idempotent ? "是" : "否");

    bool same = idempotent;
    if (!idempotent) {
        // 定位第一处差异
        size_t i = 0;
        while (i < s1.size() && i < s2.size() && s1[i] == s2[i]) ++i;
        const size_t lo = i > 60 ? i - 60 : 0;
        std::printf("  首处差异 @%zu:\n", i);
        std::printf("    第一次: ...%s\n", s1.substr(lo, 120).c_str());
        std::printf("    第二次: ...%s\n", s2.substr(lo, 120).c_str());
        same = false;
    }

    // 覆盖度统计：绑定表字段总数
    size_t n_fields = 0;
    {
        EmsConfig t;
        Binder b1; bind_fields(b1, t.loop);   n_fields += b1.fields().size();
        Binder b2; bind_fields(b2, t.plant);  n_fields += b2.fields().size();
        Binder b3; bind_fields(b3, t.limits); n_fields += b3.fields().size();
        Binder b4; bind_fields(b4, t.safety); n_fields += b4.fields().size();
        Binder b5; bind_fields(b5, t.coord);  n_fields += b5.fields().size();
        Binder b6; bind_fields(b6, t.fsm);    n_fields += b6.fields().size();
        std::printf("  绑定表字段总数：%zu（全部参与往返）\n", n_fields);
    }
    std::printf("  策略条目：%zu\n", a.strategies.size());

    std::printf("\n%s\n", same ? "[ROUNDTRIP OK] 存取往返无漂移"
                              : "[ROUNDTRIP FAIL] 存在字段漂移");
    return same ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode;
    std::string arg;
    double hours = 24.0;
    double dt    = 1.0;
    bool   quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--demo" || a == "--check" || a == "--apply" || a == "--roundtrip"
            || a == "--dump-template" || a == "--dump-doc" || a == "--dump-schema"
            || a == "--capture" || a == "--save-default") {
            mode = a;
            if ((a == "--check" || a == "--apply" || a == "--roundtrip"
                 || a == "--capture" || a == "--save-default") && i + 1 < argc) {
                arg = argv[++i];
            }
        } else if (a == "--hours" && i + 1 < argc) {
            hours = std::atof(argv[++i]);
        } else if (a == "--fast") {
            dt = 2.0;
        } else if (a == "--quiet") {
            quiet = true;
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: ems-config [--demo | --apply <f> | --check <f> | --roundtrip <f>\n"
                "                   | --dump-template | --dump-doc | --dump-schema\n"
                "                   | --capture <out> | --save-default <out>]\n"
                "                   [--hours N] [--fast] [--quiet]\n");
            return 0;
        }
    }
    if (mode.empty()) mode = "--demo";

    // 需要运行时的模式：先建一个（用于枚举已注册策略）
    EmsRuntime rt;
    rt.init();

    if (mode == "--dump-template") {
        std::printf("%s", render_config_template(&rt, make_base_config()).c_str());
        return 0;
    }
    if (mode == "--dump-doc") {
        std::printf("%s", render_config_markdown(&rt, make_base_config()).c_str());
        return 0;
    }
    if (mode == "--dump-schema") {
        std::printf("%s", json::dump(render_config_schema(&rt), 2).c_str());
        return 0;
    }
    if (mode == "--capture") {
        if (arg.empty()) { std::printf("[FAIL] --capture 需要输出路径\n"); return 1; }
        EmsConfig cfg = make_base_config();
        ApplyOptions opt; opt.inject_plant = true;
        ConfigDiagnostics d = apply_config(rt, cfg, opt);
        if (!d.ok()) { print_diag(d); return 1; }
        EmsConfig got = capture_config(rt);
        if (!save_config_file(arg, got)) { std::printf("[FAIL] 写入失败: %s\n", arg.c_str()); return 1; }
        std::printf("[CAPTURE OK] 已导出运行中配置 → %s\n", arg.c_str());
        std::printf("  策略条目：%zu\n", got.strategies.size());
        for (const auto& kv2 : got.strategies) {
            std::printf("    %-24s enabled=%d params=%zu\n",
                        kv2.first.c_str(), kv2.second.enabled ? 1 : 0,
                        kv2.second.params.size());
        }
        return 0;
    }
    if (mode == "--save-default") {
        if (arg.empty()) { std::printf("[FAIL] --save-default 需要输出路径\n"); return 1; }
        if (!save_config_file(arg, make_base_config())) {
            std::printf("[FAIL] 写入失败: %s\n", arg.c_str());
            return 1;
        }
        std::printf("[SAVE OK] 已写出默认配置 → %s\n", arg.c_str());
        return 0;
    }
    if (mode == "--check") {
        if (arg.empty()) { std::printf("[FAIL] --check 需要配置文件路径\n"); return 1; }
        return cmd_check(arg);
    }
    if (mode == "--apply") {
        if (arg.empty()) { std::printf("[FAIL] --apply 需要配置文件路径\n"); return 1; }
        return cmd_apply(arg, hours, dt, quiet);
    }
    if (mode == "--roundtrip") {
        if (arg.empty()) { std::printf("[FAIL] --roundtrip 需要配置文件路径\n"); return 1; }
        return cmd_roundtrip(arg);
    }

    return cmd_demo(hours, dt, quiet);
}
