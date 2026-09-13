// =====================================================================
// 10/ — 周期 10：24h 离线仿真测试 · 日曲线导入
//
// 依据：工商业储能EMS调控策略设计方案.md §7 周期 10
//   「搭建离线仿真环境，导入负荷曲线、光伏曲线、电价时序数据、
//     SOC 初始参数、BMS/PCS/变压器设备参数」
//
// 曲线容器直接复用 08/ 的 ForecastSeries（96 点 / 15 min / 阶梯保持采样），
// 保证「离线仿真」与「闭环测试」用的是**同一条曲线语义** ——
// 否则仿真结论无法与 07/09 的测试结论互相印证。
//
// 支持两种来源：
//   ① 从 CSV 文件导入（现场实测/预测导出）
//   ② 内置典型日生成器（无文件时的兜底，保证演示可跑）
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "dispatch_coordinator.h"   // ForecastSeries

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ems {

// =====================================================================
// 典型日曲线参数（内置生成器）
// =====================================================================
struct TypicalDayParams {
    double base_load_kw = 250.0;    // 基础负荷
    double load_am_peak_kw = 150.0; // 上午峰增量（10:00）
    double load_pm_peak_kw = 200.0; // 晚间峰增量（19:00）
    double pv_peak_kw   = 300.0;    // 光伏峰值
    double pv_start_h   = 6.0;      // 光伏起
    double pv_end_h     = 18.0;     // 光伏止
    double step_s       = 900.0;    // 15 min
    int    n_points     = 96;       // 24 h

    // 分时电价（工商业两部制，元/kWh）
    double price_valley = 0.30;     // 谷
    double price_flat   = 0.60;     // 平
    double price_peak   = 1.00;     // 峰
    double price_sharp  = 1.20;     // 尖
};

// 分时电价分类（与 07/09 的 TOU 口径一致）
inline double typical_price_at(double h, const TypicalDayParams& p) {
    if (h < 7.0)  return p.price_valley;
    if (h < 9.0)  return p.price_flat;
    if (h < 12.0) return p.price_peak;
    if (h < 14.0) return p.price_flat;
    if (h < 17.0) return p.price_peak;
    if (h < 21.0) return p.price_sharp;
    if (h < 23.0) return p.price_flat;
    return p.price_valley;
}

// =====================================================================
// 内置典型日曲线
// =====================================================================
inline ForecastSeries make_typical_day_curves(const TypicalDayParams& p = TypicalDayParams{}) {
    ForecastSeries fc;
    fc.step_s = p.step_s;
    const double slot_h = p.step_s / 3600.0;
    for (int i = 0; i < p.n_points; ++i) {
        const double h = i * slot_h;
        fc.price.push_back(typical_price_at(h, p));
        fc.p_load_kw.push_back(
            p.base_load_kw
            + p.load_am_peak_kw * std::exp(-std::pow(h - 10.0, 2.0) / 8.0)
            + p.load_pm_peak_kw * std::exp(-std::pow(h - 19.0, 2.0) / 6.0));
        fc.p_pv_kw.push_back(
            (h >= p.pv_start_h && h <= p.pv_end_h)
                ? p.pv_peak_kw * std::sin(3.14159265358979323846 * (h - p.pv_start_h)
                                          / (p.pv_end_h - p.pv_start_h))
                : 0.0);
    }
    fc.loaded = true;
    return fc;
}

// =====================================================================
// 从 CSV 导入日曲线
//
// 格式（首行可为表头，以 '#' 开头的行忽略）：
//   idx,time_h,price_cny_kwh,p_load_kw,p_pv_kw
// 也兼容只有 3 列（price,p_load,p_pv）的简表。
// =====================================================================
inline bool load_day_curves_csv(const std::string& path,
                                ForecastSeries& out,
                                double step_s = 900.0,
                                std::string* err = nullptr) {
    std::ifstream f(path.c_str());
    if (!f.is_open()) {
        if (err) *err = "cannot open file: " + path;
        return false;
    }

    ForecastSeries fc;
    fc.step_s = step_s;
    std::string line;
    int line_no = 0;
    int bad = 0;
    while (std::getline(f, line)) {
        ++line_no;
        // 去空白与 CR
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                                 line.back() == ' '  || line.back() == '\t'))
            line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        // 逗号切分
        std::vector<std::string> tok;
        std::string cur;
        for (char ch : line) {
            if (ch == ',') { tok.push_back(cur); cur.clear(); }
            else           cur.push_back(ch);
        }
        tok.push_back(cur);
        if (tok.size() < 3) continue;

        // 判定列：表头行（含非数字）直接跳过
        auto num = [](const std::string& s, double* v) {
            try { size_t used = 0; *v = std::stod(s, &used);
                  return used == s.size() || used > 0; }
            catch (...) { return false; }
        };
        double a = 0, b = 0, c = 0;
        double pr = 0, ld = 0, pv = 0;
        if (tok.size() >= 5) {
            // idx, time_h, price, load, pv
            if (!num(tok[2], &a) || !num(tok[3], &b) || !num(tok[4], &c)) continue;
            pr = a; ld = b; pv = c;
        } else {
            // price, load, pv
            if (!num(tok[0], &a) || !num(tok[1], &b) || !num(tok[2], &c)) continue;
            pr = a; ld = b; pv = c;
        }
        fc.price.push_back(pr);
        fc.p_load_kw.push_back(ld);
        fc.p_pv_kw.push_back(pv);
    }

    if (fc.size() == 0) {
        if (err) *err = "no valid data row (line_no=" + std::to_string(line_no) + ")";
        return false;
    }
    fc.loaded = true;
    out = fc;
    if (err) err->clear();
    (void)bad;
    return true;
}

// =====================================================================
// 曲线统计（仿真报告用）
// =====================================================================
struct CurveStats {
    int    points = 0;
    double step_s = 900.0;
    double load_min_kw = 0.0, load_max_kw = 0.0, load_avg_kw = 0.0;
    double pv_max_kw = 0.0;
    double e_load_kwh = 0.0;    // 日用电量
    double e_pv_kwh = 0.0;      // 日发电量
    double price_min = 0.0, price_max = 0.0, price_avg = 0.0;
    double e_pv_overload_kwh = 0.0;  // 光伏 > 负荷 的余电电量（需要储能/上网消化）
};

inline CurveStats analyze_curves(const ForecastSeries& fc) {
    CurveStats s;
    s.points = static_cast<int>(fc.size());
    s.step_s = fc.step_s;
    if (s.points == 0) return s;

    const double slot_h = fc.step_s / 3600.0;
    s.load_min_kw = 1e18;
    s.price_min = 1e18;
    for (int i = 0; i < s.points; ++i) {
        const double ld = fc.p_load_kw[i], pv = fc.p_pv_kw[i], pr = fc.price[i];
        s.load_min_kw = std::min(s.load_min_kw, ld);
        s.load_max_kw = std::max(s.load_max_kw, ld);
        s.pv_max_kw   = std::max(s.pv_max_kw, pv);
        s.price_min   = std::min(s.price_min, pr);
        s.price_max   = std::max(s.price_max, pr);
        s.load_avg_kw += ld;
        s.price_avg   += pr;
        s.e_load_kwh  += ld * slot_h;
        s.e_pv_kwh    += pv * slot_h;
        if (pv > ld) s.e_pv_overload_kwh += (pv - ld) * slot_h;
    }
    s.load_avg_kw /= s.points;
    s.price_avg   /= s.points;
    return s;
}

} // namespace ems
