// =====================================================================
// 10/ — 周期 10：仿真产物导出（时序 CSV / 告警 CSV / 汇总 JSON / HTML 报告）
//
// 依据：设计方案 §7 周期 10「输出功率曲线、SOC 曲线、策略状态、
//       经济收益、告警日志」
//
// 全部手写，不引入任何第三方依赖（项目是 header-only C++17，
// 图表用内联 SVG 而不是 JS 图表库 —— 报告单文件、离线可看、可直接打印）。
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "econ_metrics.h"
#include "sim_24h.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace ems {

// =====================================================================
// 内部工具
// =====================================================================
namespace sim_report_detail {

inline std::string fmt(double v, int prec = 2) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(prec) << v;
    return os.str();
}

inline std::string hhmm(double t_s) {
    const int total_min = static_cast<int>(t_s / 60.0);
    const int h = (total_min / 60) % 24;
    const int m = total_min % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
    return std::string(buf);
}

// ---------------------------------------------------------------------
// 多序列折线图（内联 SVG）
//   xs 单位：小时；ys[i] 与 xs 等长
// ---------------------------------------------------------------------
struct Series {
    std::string        label;
    std::string        color;
    std::vector<double> y;
    bool               dashed = false;
};

inline std::string svg_chart(const std::string& title,
                             const std::vector<Series>& series,
                             const std::vector<double>& xs,
                             const std::string& y_unit,
                             int W = 860, int H = 260) {
    const int ML = 58, MR = 14, MT = 30, MB = 30;
    const int pw = W - ML - MR;
    const int ph = H - MT - MB;

    if (xs.empty() || series.empty()) return std::string();

    double ymin = 1e18, ymax = -1e18;
    for (const auto& s : series)
        for (double v : s.y) { ymin = std::min(ymin, v); ymax = std::max(ymax, v); }
    if (ymin > ymax) { ymin = 0; ymax = 1; }
    // 上下留白
    const double span0 = ymax - ymin;
    const double pad = (span0 > 1e-9) ? span0 * 0.10 : std::max(1.0, std::fabs(ymax) * 0.1);
    ymin -= pad; ymax += pad;

    const double x0 = xs.front(), x1 = xs.back();
    const double xr = std::max(1e-9, x1 - x0);
    const double yr = std::max(1e-9, ymax - ymin);

    auto X = [&](double x) { return ML + (x - x0) / xr * pw; };
    auto Y = [&](double y) { return MT + (ymax - y) / yr * ph; };

    std::ostringstream o;
    o << "<svg viewBox=\"0 0 " << W << " " << H << "\" width=\"100%\" "
         "style=\"max-width:" << W << "px;font-family:system-ui,-apple-system,"
         "\'Segoe UI\',sans-serif\">\n";
    o << "<rect x=\"0\" y=\"0\" width=\"" << W << "\" height=\"" << H
      << "\" fill=\"#ffffff\"/>\n";
    o << "<text x=\"" << ML << "\" y=\"18\" font-size=\"13\" font-weight=\"600\" "
         "fill=\"#1f2937\">" << title << "</text>\n";

    // y 网格（5 条）
    for (int i = 0; i <= 4; ++i) {
        const double v = ymin + yr * i / 4.0;
        const double y = Y(v);
        o << "<line x1=\"" << ML << "\" y1=\"" << fmt(y, 1) << "\" x2=\"" << (ML + pw)
          << "\" y2=\"" << fmt(y, 1) << "\" stroke=\"#e5e7eb\" stroke-width=\"1\"/>\n";
        o << "<text x=\"" << (ML - 6) << "\" y=\"" << fmt(y + 4, 1)
          << "\" font-size=\"10\" fill=\"#6b7280\" text-anchor=\"end\">"
          << fmt(v, 0) << "</text>\n";
    }
    // x 刻度（每 4 h）
    for (int h = 0; h <= 24; h += 4) {
        const double x = X(static_cast<double>(h));
        if (x < ML - 1 || x > ML + pw + 1) continue;
        o << "<line x1=\"" << fmt(x, 1) << "\" y1=\"" << MT << "\" x2=\"" << fmt(x, 1)
          << "\" y2=\"" << (MT + ph) << "\" stroke=\"#f3f4f6\" stroke-width=\"1\"/>\n";
        o << "<text x=\"" << fmt(x, 1) << "\" y=\"" << (MT + ph + 14)
          << "\" font-size=\"10\" fill=\"#6b7280\" text-anchor=\"middle\">"
          << h << "h</text>\n";
    }
    // 零线
    if (ymin < 0.0 && ymax > 0.0) {
        const double y = Y(0.0);
        o << "<line x1=\"" << ML << "\" y1=\"" << fmt(y, 1) << "\" x2=\"" << (ML + pw)
          << "\" y2=\"" << fmt(y, 1) << "\" stroke=\"#9ca3af\" stroke-width=\"1\" "
             "stroke-dasharray=\"3,3\"/>\n";
    }

    // 折线
    for (const auto& s : series) {
        o << "<polyline fill=\"none\" stroke=\"" << s.color << "\" stroke-width=\"1.6\"";
        if (s.dashed) o << " stroke-dasharray=\"5,3\"";
        o << " points=\"";
        for (size_t i = 0; i < xs.size() && i < s.y.size(); ++i)
            o << fmt(X(xs[i]), 1) << "," << fmt(Y(s.y[i]), 1) << " ";
        o << "\"/>\n";
    }

    // 图例
    int lx = ML;
    for (const auto& s : series) {
        o << "<line x1=\"" << lx << "\" y1=\"" << (H - 8) << "\" x2=\"" << (lx + 16)
          << "\" y2=\"" << (H - 8) << "\" stroke=\"" << s.color << "\" stroke-width=\"2.4\"";
        if (s.dashed) o << " stroke-dasharray=\"5,3\"";
        o << "/>\n";
        o << "<text x=\"" << (lx + 21) << "\" y=\"" << (H - 4)
          << "\" font-size=\"10.5\" fill=\"#374151\">" << s.label << "</text>\n";
        lx += 26 + static_cast<int>(s.label.size()) * 7;
    }
    if (!y_unit.empty()) {
        o << "<text x=\"" << (W - MR) << "\" y=\"" << (MT - 8)
          << "\" font-size=\"10\" fill=\"#9ca3af\" text-anchor=\"end\">" << y_unit
          << "</text>\n";
    }
    o << "</svg>\n";
    return o.str();
}

} // namespace sim_report_detail

// =====================================================================
// 时序 CSV
// =====================================================================
inline bool write_timeseries_csv(const std::string& path,
                                 const std::vector<StepRecord>& log) {
    std::ofstream f(path.c_str());
    if (!f.is_open()) return false;
    f << "t_s,time,state,P_load_kW,P_pv_kW,P_grid_kW,P_cmd_kW,P_actual_kW,"
         "SOC,T_C,P_lower,P_upper,plan_target,correction,clamped,safety_clip,"
         "state_gated,hold_last,fault_bits,reason\n";
    f << std::fixed << std::setprecision(3);
    for (const auto& r : log) {
        f << r.t << "," << sim_report_detail::hhmm(r.t) << ","
          << state_name(r.state) << ","
          << r.p_load << "," << r.p_pv << "," << r.p_grid << ","
          << r.p_cmd << "," << r.p_actual << "," << r.soc << "," << r.temp << ","
          << r.p_lower << "," << r.p_upper << ","
          << r.plan_target << "," << r.correction << ","
          << (r.clamped ? 1 : 0) << "," << (r.safety_clip ? 1 : 0) << ","
          << (r.state_gated ? 1 : 0) << "," << (r.hold_last ? 1 : 0) << ","
          << r.fault_bits << ",\"" << r.reason << "\"\n";
    }
    return true;
}

// =====================================================================
// 告警 CSV
// =====================================================================
inline bool write_alarms_csv(const std::string& path,
                             const std::vector<AlarmEntry>& alarms) {
    std::ofstream f(path.c_str());
    if (!f.is_open()) return false;
    f << "t_s,time,level,source,message\n";
    for (const auto& a : alarms) {
        f << sim_report_detail::fmt(a.t, 1) << "," << sim_report_detail::hhmm(a.t) << ","
          << a.level << "," << a.source << ",\"" << a.message << "\"\n";
    }
    return true;
}

// =====================================================================
// 汇总 JSON
// =====================================================================
inline bool write_summary_json(const std::string& path,
                               const Sim24hResult& r,
                               const Sim24hConfig& cfg) {
    std::ofstream f(path.c_str());
    if (!f.is_open()) return false;
    const auto& e = r.econ;
    const auto& c = r.curves;
    f << std::fixed << std::setprecision(4);
    f << "{\n";
    f << "  \"ok\": " << (r.ok ? "true" : "false") << ",\n";
    f << "  \"error\": \"" << r.error << "\",\n";
    f << "  \"steps\": " << r.steps << ",\n";
    f << "  \"log_rows\": " << r.log_rows << ",\n";
    f << "  \"wall_s\": " << sim_report_detail::fmt(r.wall_s, 3) << ",\n";
    f << "  \"curves\": {\n";
    f << "    \"points\": " << c.points << ", \"step_s\": " << c.step_s << ",\n";
    f << "    \"load_min_kw\": " << c.load_min_kw << ", \"load_max_kw\": " << c.load_max_kw
      << ", \"load_avg_kw\": " << c.load_avg_kw << ",\n";
    f << "    \"pv_max_kw\": " << c.pv_max_kw << ",\n";
    f << "    \"e_load_kwh\": " << c.e_load_kwh << ", \"e_pv_kwh\": " << c.e_pv_kwh << ",\n";
    f << "    \"price_min\": " << c.price_min << ", \"price_max\": " << c.price_max
      << ", \"price_avg\": " << c.price_avg << "\n";
    f << "  },\n";
    f << "  \"economics\": {\n";
    f << "    \"e_import_kwh\": " << e.e_import_kwh << ", \"e_export_kwh\": " << e.e_export_kwh << ",\n";
    f << "    \"e_charge_kwh\": " << e.e_charge_kwh << ", \"e_discharge_kwh\": " << e.e_discharge_kwh << ",\n";
    f << "    \"throughput_kwh\": " << e.throughput_kwh << ", \"equiv_cycles\": " << e.equiv_cycles << ",\n";
    f << "    \"pv_self_use_kwh\": " << e.pv_self_use_kwh << ",\n";
    f << "    \"peak_grid_kw\": " << e.peak_grid_kw << ", \"peak_grid_base_kw\": " << e.peak_grid_base_kw << ",\n";
    f << "    \"cost_energy_cny\": " << e.cost_energy_cny << ",\n";
    f << "    \"cost_demand_cny\": " << e.cost_demand_cny << ",\n";
    f << "    \"cost_total_cny\": " << e.cost_total_cny << ",\n";
    f << "    \"cost_total_base_cny\": " << e.cost_total_base_cny << ",\n";
    f << "    \"saving_energy_cny\": " << e.saving_energy_cny << ",\n";
    f << "    \"saving_demand_cny\": " << e.saving_demand_cny << ",\n";
    f << "    \"saving_total_cny\": " << e.saving_total_cny << ",\n";
    f << "    \"cost_degradation_cny\": " << e.cost_degradation_cny << ",\n";
    f << "    \"net_benefit_cny\": " << e.net_benefit_cny << ",\n";
    f << "    \"saving_pct\": " << e.saving_pct() << "\n";
    f << "  },\n";
    f << "  \"state\": {\n";
    f << "    \"soc_min\": " << e.soc_min << ", \"soc_max\": " << e.soc_max
      << ", \"soc_avg\": " << e.soc_avg << ", \"soc_end\": " << e.soc_end << ",\n";
    f << "    \"max_abs_cmd_kw\": " << e.max_abs_cmd_kw << ", \"max_temp_c\": " << e.max_temp_c << ",\n";
    f << "    \"max_grid_kw\": " << r.max_grid_kw << ", \"min_grid_kw\": " << r.min_grid_kw << "\n";
    f << "  },\n";
    f << "  \"invariants\": {\n";
    f << "    \"out_of_interval\": " << r.out_of_interval << ",\n";
    f << "    \"over_limit\": " << r.over_limit << ",\n";
    f << "    \"gated_nonzero\": " << r.gated_nonzero << ",\n";
    f << "    \"grid_breach\": " << r.grid_breach << ",\n";
    f << "    \"grid_breach_soc_limited\": " << r.grid_breach_soc_limited << ",\n";
    f << "    \"tr_breach\": " << r.tr_breach << ",\n";
    f << "    \"soc_violation\": " << e.soc_violation << ",\n";
    f << "    \"hard_ok\": " << (r.hard_invariants_ok() ? "true" : "false") << ",\n";
    f << "    \"safety_ok\": " << (r.safety_invariants_ok() && e.soc_violation == 0
                                   ? "true" : "false") << ",\n";
    f << "    \"all_ok\": " << (r.invariants_ok() && e.soc_violation == 0
                                ? "true" : "false") << "\n";
    f << "  },\n";
    f << "  \"alarms\": {\n";
    f << "    \"total\": " << r.alarm_count << ", \"fault\": " << r.fault_count << "\n";
    f << "  },\n";
    f << "  \"faults\": {\n";
    f << "    \"active_ticks\": " << r.fault_ticks << ",\n";
    f << "    \"windows\": [";
    for (size_t i = 0; i < cfg.fault_windows.size(); ++i) {
        const auto& w = cfg.fault_windows[i];
        f << (i ? ",\n      " : "\n      ")
          << "{\"begin_s\": " << w.t_begin_s << ", \"end_s\": " << w.t_end_s
          << ", \"kind\": " << w.kind << ", \"note\": \"" << w.note << "\"}";
    }
    f << (cfg.fault_windows.empty() ? "" : "\n    ") << "]\n";
    f << "  },\n";
    f << "  \"config\": {\n";
    f << "    \"dt_s\": " << cfg.dt_s << ", \"log_every\": " << cfg.log_every
      << ", \"duration_s\": " << cfg.duration_s << ",\n";
    f << "    \"battery_capacity_kwh\": " << cfg.plant.battery_capacity_kwh << ",\n";
    f << "    \"pcs_rated_kw\": " << cfg.limits.pcs_rated_dis_kw << ",\n";
    f << "    \"transformer_capacity_kw\": " << cfg.limits.transformer_capacity_kw << ",\n";
    f << "    \"d_target_kw\": " << cfg.limits.d_target_kw << ",\n";
    f << "    \"soc_init\": " << cfg.plant.soc_init << ",\n";
    f << "    \"soc_min\": " << cfg.safety.soc_min << ", \"soc_max\": " << cfg.safety.soc_max << "\n";
    f << "  }\n";
    f << "}\n";
    return true;
}

// =====================================================================
// HTML 报告（单文件、离线可看、内联 SVG）
// =====================================================================
inline bool write_report_html(const std::string& path,
                              const Sim24hResult& r,
                              const Sim24hConfig& cfg,
                              const ForecastSeries* fc = nullptr) {
    using namespace sim_report_detail;

    // ---- 降采样到 ≤ 288 点（5 min）用于绘图 ----
    const auto& log = r.log;
    if (log.empty()) return false;
    const int stride = std::max(1, static_cast<int>(log.size() / 288));

    std::vector<double> xs;
    std::vector<double> y_load, y_pv, y_grid, y_bat, y_soc, y_price, y_cmd;
    for (size_t i = 0; i < log.size(); i += stride) {
        const auto& s = log[i];
        xs.push_back(s.t / 3600.0);
        y_load.push_back(s.p_load);
        y_pv.push_back(s.p_pv);
        y_grid.push_back(s.p_grid);
        y_bat.push_back(s.p_actual);
        y_soc.push_back(s.soc * 100.0);
        y_cmd.push_back(s.p_cmd);
        double pr = 0.0;
        if (fc && fc->loaded) fc->sample(s.t, nullptr, nullptr, &pr);
        y_price.push_back(pr);
    }

    std::string c_power = svg_chart(
        "功率曲线（kW）",
        {{"负荷 P_load", "#6b7280", y_load, true},
         {"光伏 P_pv", "#f59e0b", y_pv, false},
         {"关口 P_grid", "#2563eb", y_grid, false},
         {"电池 P_bat(实际)", "#16a34a", y_bat, false}},
        xs, "kW");

    std::string c_soc = svg_chart(
        "SOC 曲线（%）",
        {{"SOC", "#7c3aed", y_soc, false}},
        xs, "%", 860, 200);

    std::string c_cmd = svg_chart(
        "指令 vs 实际（kW）",
        {{"P_cmd", "#dc2626", y_cmd, true},
         {"P_actual", "#16a34a", y_bat, false}},
        xs, "kW", 860, 200);

    std::string c_price = svg_chart(
        "分时电价（元/kWh）",
        {{"电价", "#0891b2", y_price, false}},
        xs, "元/kWh", 860, 170);

    const auto& e = r.econ;
    const auto& c = r.curves;

    std::ostringstream o;
    o << "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n<meta charset=\"utf-8\">\n"
         "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
         "<title>" << cfg.title << "</title>\n<style>\n"
      << ":root{--fg:#111827;--mut:#6b7280;--line:#e5e7eb;--bg:#f9fafb;"
         "--ok:#16a34a;--warn:#d97706;--bad:#dc2626;}\n"
         "*{box-sizing:border-box}\n"
         "body{margin:0;padding:28px 20px 48px;background:var(--bg);color:var(--fg);"
         "font:14px/1.6 system-ui,-apple-system,'Segoe UI','Microsoft YaHei',sans-serif}\n"
         ".wrap{max-width:960px;margin:0 auto}\n"
         "h1{font-size:21px;margin:0 0 4px}\n"
         ".sub{color:var(--mut);font-size:12.5px;margin-bottom:20px}\n"
         ".card{background:#fff;border:1px solid var(--line);border-radius:10px;"
         "padding:16px 18px;margin-bottom:16px}\n"
         ".card h2{font-size:15px;margin:0 0 12px;font-weight:600}\n"
         ".kpis{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}\n"
         ".kpi{background:var(--bg);border:1px solid var(--line);border-radius:8px;padding:10px 12px}\n"
         ".kpi .l{font-size:11.5px;color:var(--mut)}\n"
         ".kpi .v{font-size:19px;font-weight:650;margin-top:2px}\n"
         ".kpi .u{font-size:11.5px;color:var(--mut);font-weight:400}\n"
         "table{width:100%;border-collapse:collapse;font-size:12.5px}\n"
         "th,td{text-align:left;padding:6px 8px;border-bottom:1px solid var(--line)}\n"
         "th{color:var(--mut);font-weight:500;background:#fbfcfd}\n"
         "td.n,th.n{text-align:right;font-variant-numeric:tabular-nums}\n"
         ".pill{display:inline-block;padding:1px 8px;border-radius:999px;font-size:11.5px;"
         "border:1px solid}\n"
         ".pill.ok{color:var(--ok);border-color:#bbf7d0;background:#f0fdf4}\n"
         ".pill.bad{color:var(--bad);border-color:#fecaca;background:#fef2f2}\n"
         ".pill.warn{color:var(--warn);border-color:#fde68a;background:#fffbeb}\n"
         ".mut{color:var(--mut)}\n"
         ".svgbox{overflow-x:auto}\n"
         "</style>\n</head>\n<body>\n<div class=\"wrap\">\n";

    o << "<h1>" << cfg.title << "</h1>\n";
    o << "<div class=\"sub\">24 h 全场景离线仿真 · 步长 " << fmt(cfg.dt_s, 1)
      << " s · 日志粒度 " << fmt(r.rec_dt_s, 0) << " s · 共 " << r.steps
      << " 拍（" << r.log_rows << " 条记录）· 墙钟耗时 " << fmt(r.wall_s, 2)
      << " s</div>\n";

    // ---- 结论 ----
    {
        const bool hard = r.hard_invariants_ok();
        const bool safe = r.safety_invariants_ok() && e.soc_violation == 0;
        o << "<div class=\"card\" style=\"padding:12px 18px\">"
          << "<span class=\"pill " << (hard ? "ok" : "bad") << "\">硬不变量 "
          << (hard ? "全部通过" : "存在违规") << "</span> "
          << "<span class=\"pill " << (safe ? "ok" : "warn") << "\">安全不变量 "
          << (safe ? "全部通过" : "存在越限（属场景结论）") << "</span>"
          << "</div>\n";
    }
    o << "<div class=\"card\"><h2>结论</h2><div class=\"kpis\">\n";
    auto kpi = [&](const std::string& l, const std::string& v, const std::string& u) {
        o << "<div class=\"kpi\"><div class=\"l\">" << l << "</div><div class=\"v\">"
          << v << " <span class=\"u\">" << u << "</span></div></div>\n";
    };
    kpi("日总电费（含储能）", fmt(e.cost_total_cny, 0), "元");
    kpi("日总电费（无储能）", fmt(e.cost_total_base_cny, 0), "元");
    kpi("节省（未扣衰减）", fmt(e.saving_total_cny, 0), "元");
    kpi("电池衰减成本", fmt(e.cost_degradation_cny, 0), "元");
    kpi("净收益", fmt(e.net_benefit_cny, 0), "元/日");
    kpi("节省比例", fmt(e.saving_pct(), 1), "%");
    o << "</div>\n";
    o << "<p class=\"mut\" style=\"margin:12px 0 0;font-size:12.5px\">"
      << "节省拆分：电度电费 " << fmt(e.saving_energy_cny, 0) << " 元 + 需量电费 "
      << fmt(e.saving_demand_cny, 0) << " 元；净收益 = 节省 − 电池度电衰减（"
      << fmt(cfg.econ.degradation_cny_per_kwh_dis(), 3) << " 元/kWh 放电量）。</p>\n";
    o << "</div>\n";

    // ---- 不变量 ----
    o << "<div class=\"card\"><h2>不变量校验</h2>\n";
    o << "<p class=\"mut\" style=\"margin:0 0 10px;font-size:12.5px\">"
         "<b>硬不变量</b>是 EMS 对设备端声明的权限区间与设备能力 —— 任何场景、任何时刻"
         "都不得违反，违反即架构级问题；<b>安全不变量</b>是物理量，受<b>能量预算</b>影响"
         "（SOC 用尽即无削峰能力），越限是有效场景结论而非缺陷。</p>\n";
    o << "<table>\n<tr><th>不变量</th><th>类别</th><th class=\"n\">违规</th><th>判定</th></tr>\n";
    auto row = [&](const std::string& n, const char* kind, int v, bool steady_only) {
        o << "<tr><td>" << n << (steady_only ? " <span class=\"mut\">(稳态窗口)</span>" : "")
          << "</td><td class=\"mut\">" << kind << "</td><td class=\"n\">" << v << "</td><td>"
          << (v == 0 ? "<span class=\"pill ok\">通过</span>"
                     : "<span class=\"pill bad\">违规</span>")
          << "</td></tr>\n";
    };
    row("指令逃逸 p_cmd ∉ [p_lower, p_upper]", "硬", r.out_of_interval, false);
    row("功率超限 |p_cmd| > min(PCS, BMS)", "硬", r.over_limit, false);
    row("门控失效（非运行态非零指令）", "硬", r.gated_nonzero, false);
    row("关口越界", "安全", r.grid_breach, true);
    row("变压器越限", "安全", r.tr_breach, true);
    row("SOC 越界", "安全", e.soc_violation, false);
    o << "</table>\n";
    if (r.grid_breach_soc_limited > 0) {
        o << "<p class=\"mut\" style=\"margin:10px 0 0;font-size:12.5px\">"
          << "关口越界中有 <b>" << r.grid_breach_soc_limited << "</b> 拍发生在 SOC 触底/触顶 —— "
             "储能已无可用容量，属<b>能量预算</b>结果（设备故障改变了当日能量轨迹），"
             "不是控制失效。</p>\n";
    }
    o << "<p class=\"mut\" style=\"margin:10px 0 0;font-size:12.5px\">"
      << "关口范围 [" << fmt(r.min_grid_kw, 1) << ", " << fmt(r.max_grid_kw, 1)
      << "] kW；SOC [" << fmt(e.soc_min, 3) << ", " << fmt(e.soc_max, 3)
      << "]；最大温度 " << fmt(e.max_temp_c, 1) << " °C；最大 |指令| "
      << fmt(e.max_abs_cmd_kw, 0) << " kW。</p>\n";
    o << "</div>\n";

    // ---- 故障注入窗口（仅当有）----
    if (!cfg.fault_windows.empty()) {
        o << "<div class=\"card\"><h2>故障注入窗口</h2><table>\n"
             "<tr><th>时间窗</th><th>故障</th><th class=\"n\">持续</th></tr>\n";
        for (const auto& w : cfg.fault_windows) {
            o << "<tr><td>" << hhmm(w.t_begin_s) << " – " << hhmm(w.t_end_s) << "</td><td>"
              << w.note << " <span class=\"mut\">(kind=" << w.kind << ")</span></td>"
              << "<td class=\"n\">" << fmt((w.t_end_s - w.t_begin_s) / 60.0, 1)
              << " min</td></tr>\n";
        }
        o << "</table>\n<p class=\"mut\" style=\"margin:10px 0 0;font-size:12.5px\">"
          << "故障窗内共 " << r.fault_ticks << " 拍；"
          << "关注点是「窗口内是否正确门控」与「窗口后是否能重新并网」。</p>\n";
        o << "</div>\n";
    }

    // ---- 曲线 ----
    o << "<div class=\"card\"><h2>功率曲线</h2><div class=\"svgbox\">" << c_power
      << "</div></div>\n";
    o << "<div class=\"card\"><h2>SOC 曲线</h2><div class=\"svgbox\">" << c_soc
      << "</div></div>\n";
    o << "<div class=\"card\"><h2>指令跟随</h2><div class=\"svgbox\">" << c_cmd
      << "</div></div>\n";
    o << "<div class=\"card\"><h2>分时电价</h2><div class=\"svgbox\">" << c_price
      << "</div></div>\n";

    // ---- 电量与经济明细 ----
    o << "<div class=\"card\"><h2>电量与经济明细</h2><table>\n";
    auto kv = [&](const std::string& k, const std::string& v, const std::string& u) {
        o << "<tr><td>" << k << "</td><td class=\"n\">" << v
          << " <span class=\"mut\">" << u << "</span></td></tr>\n";
    };
    kv("日用电量", fmt(c.e_load_kwh, 1), "kWh");
    kv("日发电量（光伏）", fmt(c.e_pv_kwh, 1), "kWh");
    kv("购电量", fmt(e.e_import_kwh, 1), "kWh");
    kv("上网电量", fmt(e.e_export_kwh, 1), "kWh");
    kv("光伏自用", fmt(e.pv_self_use_kwh, 1), "kWh");
    kv("储能充电", fmt(e.e_charge_kwh, 1), "kWh");
    kv("储能放电", fmt(e.e_discharge_kwh, 1), "kWh");
    kv("等效循环", fmt(e.equiv_cycles, 2), "次/日");
    kv("最大需量（含储能）", fmt(e.peak_grid_kw, 1), "kW");
    kv("最大需量（无储能）", fmt(e.peak_grid_base_kw, 1), "kW");
    kv("电度电费", fmt(e.cost_energy_cny, 1), "元");
    kv("需量电费", fmt(e.cost_demand_cny, 1), "元");
    kv("总费用", fmt(e.cost_total_cny, 1), "元");
    o << "</table></div>\n";

    // ---- 分时用电分布 ----
    const double e_sum = e.e_import_kwh > 1e-9 ? e.e_import_kwh : 1.0;
    o << "<div class=\"card\"><h2>分时购电分布</h2><table>\n"
         "<tr><th>时段</th><th class=\"n\">购电量 (kWh)</th><th class=\"n\">占比</th></tr>\n";
    auto tou = [&](const std::string& n, double v) {
        o << "<tr><td>" << n << "</td><td class=\"n\">" << fmt(v, 1)
          << "</td><td class=\"n\">" << fmt(100.0 * v / e_sum, 1) << "%</td></tr>\n";
    };
    tou("谷（≤0.35 元）", r.e_valley_kwh);
    tou("平（0.35~0.80）", r.e_flat_kwh);
    tou("峰（0.80~1.10）", r.e_peak_kwh);
    tou("尖（>1.10）", r.e_sharp_kwh);
    o << "</table>\n";
    o << "<p class=\"mut\" style=\"margin:10px 0 0;font-size:12.5px\">"
      << "谷段充电 " << fmt(r.e_chg_valley_kwh, 1) << " kWh；峰/尖段放电 "
      << fmt(r.e_dis_peak_kwh, 1) << " kWh —— 低充高放是峰谷套利收益的直接来源。</p>\n";
    o << "</div>\n";

    // ---- 告警 ----
    o << "<div class=\"card\"><h2>告警日志（" << r.alarm_count << " 条，其中 FAULT "
      << r.fault_count << " 条）</h2>\n";
    if (r.alarms.empty()) {
        o << "<p class=\"mut\">无告警。</p>\n";
    } else {
        o << "<div style=\"max-height:360px;overflow:auto\"><table>\n"
             "<tr><th>时间</th><th>级别</th><th>来源</th><th>说明</th></tr>\n";
        const size_t cap = std::min<size_t>(r.alarms.size(), 200);
        for (size_t i = 0; i < cap; ++i) {
            const auto& a = r.alarms[i];
            const char* cls = (a.level == "FAULT") ? "bad"
                            : (a.level == "ALARM") ? "bad"
                            : (a.level == "WARN")  ? "warn" : "ok";
            o << "<tr><td>" << hhmm(a.t) << "</td><td><span class=\"pill " << cls << "\">"
              << a.level << "</span></td><td>" << a.source << "</td><td>"
              << a.message << "</td></tr>\n";
        }
        if (r.alarms.size() > cap)
            o << "<tr><td colspan=\"4\" class=\"mut\">… 其余 "
              << (r.alarms.size() - cap) << " 条见 alarms.csv</td></tr>\n";
        o << "</table></div>\n";
    }
    o << "</div>\n";

    o << "<p class=\"mut\" style=\"font-size:11.5px\">生成自 10/ 周期 10 离线仿真；"
         "曲线用内联 SVG，单文件离线可看。</p>\n";
    o << "</div>\n</body>\n</html>\n";

    std::ofstream f(path.c_str());
    if (!f.is_open()) return false;
    f << o.str();
    return true;
}

// =====================================================================
// 一次性落盘全部产物
// =====================================================================
inline int write_all_reports(const std::string& dir,
                             const Sim24hResult& r,
                             const Sim24hConfig& cfg,
                             const ForecastSeries* fc = nullptr) {
    int n = 0;
    if (write_timeseries_csv(dir + "/timeseries.csv", r.log))      ++n;
    if (write_alarms_csv(dir + "/alarms.csv", r.alarms))           ++n;
    if (write_summary_json(dir + "/summary.json", r, cfg))         ++n;
    if (write_report_html(dir + "/report.html", r, cfg, fc))       ++n;
    return n;
}

} // namespace ems
