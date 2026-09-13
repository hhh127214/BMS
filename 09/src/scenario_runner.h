// =====================================================================
// 09/ — 周期 9：多策略组合测试（闭环时序级）
//
// 依据：工商业储能EMS调控策略设计方案.md §7 周期 9
//   「重点验证场景：①峰谷套利+BMS降功率 ②峰谷套利+变压器过载
//     ③需量管理+防逆流 ④光伏平抑+防逆流 ⑤动态优化+需量管理
//     ⑥需求响应+峰谷套利+BMS限制 ⑦9策略全量同时启用
//     周期目标：杜绝策略指令冲突、互相覆盖、频繁切换、双向充放电、功率超限」
//
// 与 04/tests/test_arbiter.cpp 的 T11~T17 的区别（**不是重复**）：
//   04/ 的 7 场景是**单拍仲裁级** —— 手工构造 StrategyResult 列表，调一次
//   arbitrate()，验证区间收敛正确。
//   本模块是**闭环时序级** —— 真实策略 + 真实安全引擎 + 真实状态机 +
//   真实被控对象，连续跑上万拍，验证**时序上**不出现：
//     · 频繁切换（相邻拍来回摆）
//     · 双向充放电（指令反复跨零）
//     · 功率超限（突破 PCS/BMS/变压器限值）
//     · 指令逃逸（p_cmd 落到 [p_lower, p_upper] 之外）
//   单拍正确 ≠ 时序正确。极限环、死锁、滞环失效都只在时序上暴露。
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "data_models.h"
#include "dispatch_coordinator.h"
#include "plant_model.h"
#include "realtime_loop.h"
#include "safety_engine.h"
#include "state_machine.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace ems {

// =====================================================================
// 场景配置
// =====================================================================
struct ScenarioConfig {
    std::string id;      // "S1".."S7"
    std::string name;    // 场景名
    std::string focus;   // 本场景重点验证什么

    double dt_s  = 0.1;
    int    steps = 12000;   // 1200 s @ 10Hz

    PlantConfig        plant;
    DeviceLimits       limits;
    SafetyParams       safety;
    CoordinatorConfig  coord;
    StateMachineConfig fsm;

    // 预测序列（驱动优化层 / 峰谷策略 / TOU 分类）
    bool   use_forecast   = true;
    double fc_base_load   = 200.0;
    double fc_pv_peak     = 300.0;
    double fc_step_s      = 12.5;    // 96 点 → 1200 s

    // 环境脚本（真实负荷/光伏）
    std::function<void(double t, double* load, double* pv)> env;

    // ---- 场景特定不变量 ----
    double grid_min_required = -1e18;   // 关口下限（防逆流 = 0）
    double grid_max_required =  1e18;   // 关口上限（需量 / 进线容量）
    double tr_check_cap_kva  =  0.0;    // >0 时检查变压器负载率
    double tr_check_tol      =  1.05;   // 变压器负载率允许的暂态超调系数

    // 稳态判定窗口（拍）。关口/变压器这两个**安全不变量**只在"稳态"下判定：
    //   · 门控期（INIT/SELF_CHECK/READY/FAULT）储能不动作，此时关口越限
    //     完全由负荷·光伏本身造成 —— EMS 想动也动不了，不属控制律失效；
    //   · 门控解除后的若干拍内，PCS 受**一阶惯性 + 传输延时**约束，
    //     实际功率从 0 爬升到指令值需要时间（S3 实测 ~9 拍，S4 ~35 拍），
    //     这段过渡期的关口越限是执行机构动态，不是策略冲突。
    // 因此：只有"连续非门控拍数 > settle_ticks"才计入合规判定。
    // 单拍仲裁级测试（04/）不存在该问题；这是闭环时序级测试必须显式
    // 区分"暂态 / 稳态"的地方。
    int    settle_ticks      = 50;      // 5 s @ 10 Hz
};

// =====================================================================
// 场景结果
// =====================================================================
struct ScenarioResult {
    std::string id, name, focus;
    LoopMetrics metrics;

    int    samples        = 0;
    double max_abs_cmd    = 0.0;
    double max_grid       = 0.0;
    double min_grid       = 0.0;
    double soc_min        = 1.0;
    double soc_max        = 0.0;
    double allow_chg_kw   = 0.0;   // 允许充电幅度
    double allow_dis_kw   = 0.0;   // 允许放电幅度

    // ---- 违规计数 ----
    int over_limit      = 0;   // |p_cmd| 超出设备允许
    int out_of_interval = 0;   // p_cmd 逃出 [p_lower, p_upper]
    int grid_breach     = 0;   // 关口越界
    int tr_breach       = 0;   // 变压器越限
    int gated_nonzero   = 0;   // 非运行态仍有非零指令（门控不变量）
    double flip_rate    = 0.0; // 指令符号翻转率（双向充放电度量）
    double rev_rate     = 0.0; // 指令方向反转率（频繁切换度量）

    std::vector<std::string> violations;

    bool ok() const { return violations.empty(); }

    std::string to_string() const {
        std::ostringstream os;
        os << std::fixed << std::setprecision(3);
        os << (ok() ? "[PASS] " : "[FAIL] ") << id << " " << name << "\n";
        os << "        " << focus << "\n";
        os << "        samples=" << samples
           << " |cmd|max=" << max_abs_cmd << "kW"
           << " grid=[" << min_grid << ", " << max_grid << "]kW"
           << " SOC=[" << soc_min << ", " << soc_max << "]\n";
        os << "        flip=" << flip_rate << "/s rev=" << rev_rate << "/s"
           << " over_limit=" << over_limit
           << " out_of_interval=" << out_of_interval
           << " grid_breach=" << grid_breach
           << " tr_breach=" << tr_breach
           << " gated_nonzero=" << gated_nonzero << "\n";
        for (const auto& v : violations) os << "        !! " << v << "\n";
        return os.str();
    }
};

// =====================================================================
// 预测序列构造（96 点，压缩到场景时间窗）
// =====================================================================
inline ForecastSeries make_forecast(double base_load, double pv_peak, double step_s) {
    ForecastSeries fc;
    fc.step_s = step_s;
    for (int i = 0; i < 96; ++i) {
        const double h = i * 0.25;   // 名义时刻（小时）
        // 峰谷电价：谷 0.30 / 平 0.60 / 峰 1.00 / 尖 1.20
        const double pr = (h < 7.0) ? 0.30 : (h < 9.0 ? 0.60 : (h < 12.0 ? 1.00 :
                          (h < 14.0 ? 0.60 : (h < 17.0 ? 1.00 : (h < 21.0 ? 1.20 :
                          (h < 23.0 ? 0.60 : 0.30))))));
        // 双峰负荷
        const double ld = base_load
                        + 120.0 * std::exp(-std::pow(h - 10.0, 2.0) / 8.0)
                        + 160.0 * std::exp(-std::pow(h - 19.0, 2.0) / 6.0);
        const double pv = (h >= 6.0 && h <= 18.0)
                          ? pv_peak * std::sin(3.14159265358979 * (h - 6.0) / 12.0)
                          : 0.0;
        fc.price.push_back(pr);
        fc.p_load_kw.push_back(ld);
        fc.p_pv_kw.push_back(pv);
    }
    fc.loaded = true;
    return fc;
}

// =====================================================================
// 场景运行器
// =====================================================================
inline ScenarioResult run_scenario(const ScenarioConfig& cfg) {
    ScenarioResult r;
    r.id = cfg.id; r.name = cfg.name; r.focus = cfg.focus;

    EmsRuntime rt;
    rt.config().dt_s = cfg.dt_s;
    rt.config().log_every = 1;
    rt.config().enable_log = true;

    rt.configure_plant(cfg.plant);

    // 顺序要紧：configure_plant() 内部会 refresh_device_limits() 重置 dev_，
    // 所以设备限制必须在它之后设置。
    rt.device_limits()      = cfg.limits;
    rt.safety_params()      = cfg.safety;
    rt.coordinator_config() = cfg.coord;
    rt.fsm_config()         = cfg.fsm;

    if (cfg.use_forecast) {
        rt.set_forecast(make_forecast(cfg.fc_base_load, cfg.fc_pv_peak, cfg.fc_step_s));
    }
    rt.apply_configs();

    rt.run(cfg.steps, cfg.dt_s, [&cfg](EmsRuntime& rr, int i) {
        double load = 0.0, pv = 0.0;
        if (cfg.env) cfg.env(i * cfg.dt_s, &load, &pv);
        rr.set_environment(load, pv);
    });

    // ---------------- 指标提取 ----------------
    r.metrics   = rt.metrics();
    r.flip_rate = r.metrics.flip_rate_per_s;
    r.rev_rate  = r.metrics.reversal_rate_per_s;
    r.samples   = static_cast<int>(rt.log().size());

    r.allow_chg_kw = std::min(cfg.limits.pcs_rated_chg_kw, cfg.limits.bms_chg_limit_kw);
    r.allow_dis_kw = std::min(cfg.limits.pcs_rated_dis_kw, cfg.limits.bms_dis_limit_kw);

    const double eps = 1e-6;
    r.min_grid = 1e18;
    // 稳态判定：连续非门控拍数（见 ScenarioConfig::settle_ticks 说明）
    int ungated_run = 0;
    for (const auto& rec : rt.log()) {
        r.max_abs_cmd = std::max(r.max_abs_cmd, std::fabs(rec.p_cmd));
        r.max_grid    = std::max(r.max_grid, rec.p_grid);
        r.min_grid    = std::min(r.min_grid, rec.p_grid);
        r.soc_min     = std::min(r.soc_min, rec.soc);
        r.soc_max     = std::max(r.soc_max, rec.soc);

        if (rec.state_gated) ungated_run = 0;
        else                 ++ungated_run;
        const bool steady = (ungated_run > cfg.settle_ticks);

        // ① 功率超限：|p_cmd| 超出 PCS / BMS 允许（**任何**时刻都必须成立 ——
        //    这是 EMS 自己声明的权限区间，与执行机构动态无关）
        if (rec.p_cmd >  r.allow_dis_kw + eps) ++r.over_limit;
        if (rec.p_cmd < -r.allow_chg_kw - eps) ++r.over_limit;

        // ② 指令逃逸：p_cmd 必须落在仲裁给出的区间内（**任何**时刻，同上）
        if (rec.p_cmd > rec.p_upper + eps || rec.p_cmd < rec.p_lower - eps)
            ++r.out_of_interval;

        // ③ 关口越界（仅稳态）
        if (steady) {
            if (rec.p_grid < cfg.grid_min_required - eps) ++r.grid_breach;
            if (rec.p_grid > cfg.grid_max_required + eps) ++r.grid_breach;

            // ④ 变压器负载率（与 05/ 同口径：|P_grid| + 0.1·P_load）
            if (cfg.tr_check_cap_kva > 0.0) {
                const double tr_load = std::fabs(rec.p_grid) + 0.1 * rec.p_load;
                if (tr_load > cfg.safety.tr_overload_th * cfg.tr_check_cap_kva * cfg.tr_check_tol)
                    ++r.tr_breach;
            }
        }

        // ⑤ 门控不变量：非运行态必须恒 0（**任何**时刻）
        if (!rt.fsm().output_enabled() && std::fabs(rec.p_cmd) > eps) ++r.gated_nonzero;
    }
    if (r.samples == 0) r.min_grid = 0.0;

    // ---------------- 不变量判定 ----------------
    auto add = [&r](bool bad, const std::string& msg) {
        if (bad) r.violations.push_back(msg);
    };

    add(r.over_limit > 0,
        "功率超限 " + std::to_string(r.over_limit) + " 次（|p_cmd| 超出 PCS/BMS 允许）");
    add(r.out_of_interval > 0,
        "指令逃逸 " + std::to_string(r.out_of_interval) + " 次（p_cmd 不在 [p_lower, p_upper]）");
    add(r.grid_breach > 0,
        "关口越界 " + std::to_string(r.grid_breach) + " 次（稳态窗口内要求 [" +
        std::to_string((long long)cfg.grid_min_required) + ", " +
        std::to_string((long long)cfg.grid_max_required) + "] kW）");
    add(r.tr_breach > 0,
        "变压器越限 " + std::to_string(r.tr_breach) + " 次（稳态窗口内 > " +
        std::to_string(cfg.safety.tr_overload_th * cfg.tr_check_cap_kva * cfg.tr_check_tol) + " kW）");
    add(r.gated_nonzero > 0,
        "门控失效 " + std::to_string(r.gated_nonzero) + " 次（非运行态仍有非零指令）");
    // 频繁切换 / 双向充放电（沿用 07/ 的工程口径：翻转 ≤0.2/s、反转 ≤1.0/s）
    add(r.flip_rate > 0.20,
        "双向充放电过于频繁 flip=" + std::to_string(r.flip_rate) + "/s > 0.20/s");
    add(r.rev_rate > 1.00,
        "指令频繁切换 rev=" + std::to_string(r.rev_rate) + "/s > 1.00/s");
    // SOC 不得越界（留 1% 量测容差）
    add(r.soc_min < cfg.safety.soc_min - 0.01,
        "SOC 越下界 " + std::to_string(r.soc_min) + " < " + std::to_string(cfg.safety.soc_min));
    add(r.soc_max > cfg.safety.soc_max + 0.01,
        "SOC 越上界 " + std::to_string(r.soc_max) + " > " + std::to_string(cfg.safety.soc_max));

    return r;
}

// =====================================================================
// 场景 1：峰谷套利 + BMS 降功率
//   验证：BMS 动态降功率（充 100 / 放 150）时，峰谷套利的大功率指令
//         不得突破 BMS 限值 —— 这是"互相覆盖"的典型场景。
// =====================================================================
inline ScenarioConfig scenario_1() {
    ScenarioConfig c;
    c.id = "S1";
    c.name = "峰谷套利 + BMS降功率";
    c.focus = "BMS 降功率时峰谷指令不得越限（互相覆盖检查）";

    c.plant.battery_capacity_kwh = 1000.0;
    c.plant.pcs_max_chg_kw = c.plant.pcs_max_dis_kw = 250.0;
    c.plant.pcs_ramp_kw_per_s = 400.0;

    c.limits.pcs_rated_chg_kw = c.limits.pcs_rated_dis_kw = 250.0;
    c.limits.bms_chg_limit_kw = 100.0;   // ← BMS 只允许 100kW 充电
    c.limits.bms_dis_limit_kw = 150.0;   // ← 只允许 150kW 放电
    c.limits.transformer_capacity_kw = 800.0;
    c.limits.d_target_kw = 800.0;        // 本场景不考核需量

    c.safety.soc_min = 0.10;
    c.safety.soc_max = 0.90;
    c.safety.grid_p_min_kw = -1e9;       // 不考核倒送
    c.safety.grid_p_max_kw = 1e9;

    c.env = [](double, double* load, double* pv) { *load = 200.0; *pv = 50.0; };
    return c;
}

// =====================================================================
// 场景 2：峰谷套利 + 变压器过载
//   验证：变压器负载率（|P_grid| + 0.1·P_load）不得超过阈值。
//         这是"功率超限"里最容易漏的一路 —— 变压器不是储能设备，
//         但储能动作会直接影响它。
// =====================================================================
inline ScenarioConfig scenario_2() {
    ScenarioConfig c;
    c.id = "S2";
    c.name = "峰谷套利 + 变压器过载";
    c.focus = "变压器负载率 |P_grid|+0.1·P_load 不越限";

    c.plant.battery_capacity_kwh = 1000.0;
    c.plant.pcs_max_chg_kw = c.plant.pcs_max_dis_kw = 250.0;
    c.plant.pcs_ramp_kw_per_s = 400.0;

    c.limits.pcs_rated_chg_kw = c.limits.pcs_rated_dis_kw = 250.0;
    c.limits.bms_chg_limit_kw = c.limits.bms_dis_limit_kw = 250.0;
    c.limits.transformer_capacity_kw = 300.0;   // ← 小容量变压器
    c.limits.d_target_kw = 800.0;

    c.safety.soc_min = 0.10;
    c.safety.soc_max = 0.90;
    c.safety.grid_p_min_kw = -1e9;
    c.safety.grid_p_max_kw = 1e9;

    c.tr_check_cap_kva = 300.0;

    // 大负荷 → 关口功率高 → 变压器逼近过载
    c.env = [](double, double* load, double* pv) { *load = 260.0; *pv = 20.0; };
    return c;
}

// =====================================================================
// 场景 3：需量管理 + 防逆流
//   验证：两个 L2 控制器同时发声时，既不突破契约需量，也不倒送。
//         （防逆流要求 P_bat ≥ base，需量要求 P_bat ≥ base − D_target，
//          两者方向相同但幅度不同 → 取更严者，不能互相覆盖。）
// =====================================================================
inline ScenarioConfig scenario_3() {
    ScenarioConfig c;
    c.id = "S3";
    c.name = "需量管理 + 防逆流";
    c.focus = "需量不突破 且 不倒送（两个 L2 策略的从严合并）";

    c.plant.battery_capacity_kwh = 1000.0;
    c.plant.pcs_max_chg_kw = c.plant.pcs_max_dis_kw = 250.0;
    c.plant.pcs_ramp_kw_per_s = 400.0;

    c.limits.pcs_rated_chg_kw = c.limits.pcs_rated_dis_kw = 250.0;
    c.limits.bms_chg_limit_kw = c.limits.bms_dis_limit_kw = 250.0;
    c.limits.transformer_capacity_kw = 800.0;
    c.limits.d_target_kw = 250.0;        // ← 契约需量 250kW

    c.safety.soc_min = 0.10;
    c.safety.soc_max = 0.90;
    c.safety.grid_p_min_kw = 0.0;        // ← 不允许倒送
    c.safety.grid_p_max_kw = 250.0;      // ← 需量硬兜底（L1）

    c.grid_min_required = -5.0;          // 允许 5kW 滤波暂态
    c.grid_max_required = 250.0 * 1.02;  // 允许 2% 暂态

    // 负荷超过契约需量 → 需量管理必须放电削峰
    c.env = [](double t, double* load, double* pv) {
        *load = 320.0 + 40.0 * std::sin(t / 100.0);
        *pv = 30.0;
    };
    return c;
}

// =====================================================================
// 场景 4：光伏平抑 + 防逆流
//   验证：光伏大幅波动时，储能平抑的同时不得倒送。
//         这是最容易出问题的组合 —— 平抑要求快速跟随，
//         防逆流要求严格不倒送，两者在并网点附近直接对抗。
// =====================================================================
inline ScenarioConfig scenario_4() {
    ScenarioConfig c;
    c.id = "S4";
    c.name = "光伏平抑 + 防逆流";
    c.focus = "光伏波动下平抑与不倒送并存（并网点附近对抗）";

    c.plant.battery_capacity_kwh = 1000.0;
    c.plant.pcs_max_chg_kw = c.plant.pcs_max_dis_kw = 250.0;
    c.plant.pcs_ramp_kw_per_s = 400.0;

    c.limits.pcs_rated_chg_kw = c.limits.pcs_rated_dis_kw = 250.0;
    c.limits.bms_chg_limit_kw = c.limits.bms_dis_limit_kw = 250.0;
    c.limits.transformer_capacity_kw = 800.0;
    c.limits.d_target_kw = 800.0;

    c.safety.soc_min = 0.10;
    c.safety.soc_max = 0.90;
    c.safety.grid_p_min_kw = 0.0;        // ← 不允许倒送
    c.safety.grid_p_max_kw = 1e9;

    c.grid_min_required = -5.0;

    // 光伏剧烈波动（云遮），负荷偏低 → 光伏余电必须被储能吸收
    c.env = [](double t, double* load, double* pv) {
        *load = 120.0;
        *pv   = 200.0 + 120.0 * std::sin(t / 25.0) + 40.0 * std::sin(t / 7.0);
    };
    return c;
}

// =====================================================================
// 场景 5：动态优化 + 需量管理
//   验证：优化层给出的日计划被需量约束正确压缩，且计划跟踪不失效。
//         （优化层与实时层都不得突破安全边界。）
// =====================================================================
inline ScenarioConfig scenario_5() {
    ScenarioConfig c;
    c.id = "S5";
    c.name = "动态优化 + 需量管理";
    c.focus = "优化层计划与需量约束协同，计划跟踪不失效";

    c.plant.battery_capacity_kwh = 1000.0;
    c.plant.pcs_max_chg_kw = c.plant.pcs_max_dis_kw = 250.0;
    c.plant.pcs_ramp_kw_per_s = 400.0;
    c.plant.pcs_tau_s = 0.3;
    c.plant.pcs_deadtime_s = 0.1;

    c.limits.pcs_rated_chg_kw = c.limits.pcs_rated_dis_kw = 250.0;
    c.limits.bms_chg_limit_kw = c.limits.bms_dis_limit_kw = 250.0;
    c.limits.transformer_capacity_kw = 700.0;
    c.limits.d_target_kw = 350.0;

    c.safety.soc_min = 0.10;
    c.safety.soc_max = 0.90;
    c.safety.grid_p_min_kw = 0.0;
    c.safety.grid_p_max_kw = 350.0;      // ← 契约需量硬兜底

    c.coord.reopt_period_s = 300.0;

    c.grid_min_required = -5.0;
    c.grid_max_required = 350.0 * 1.02;

    c.fc_base_load = 180.0;
    c.fc_pv_peak   = 300.0;

    // 环境跟随预测（这样优化层的计划才是"对的"）
    c.env = [](double t, double* load, double* pv) {
        const double h = std::fmod(t, 1200.0) / 1200.0 * 24.0;
        *load = 180.0 + 120.0 * std::exp(-std::pow(h - 10.0, 2.0) / 8.0)
                      + 160.0 * std::exp(-std::pow(h - 19.0, 2.0) / 6.0);
        *pv   = (h >= 6.0 && h <= 18.0)
                ? 300.0 * std::sin(3.14159265358979 * (h - 6.0) / 12.0) : 0.0;
    };
    return c;
}

// =====================================================================
// 场景 6：需求响应 + 峰谷套利 + BMS 限制
//   验证：三层经济/安全策略同时发声时的优先级 ——
//         BMS 限制（L0/L1）必须压过 DR 与峰谷（L3）。
// =====================================================================
inline ScenarioConfig scenario_6() {
    ScenarioConfig c;
    c.id = "S6";
    c.name = "需求响应 + 峰谷套利 + BMS限制";
    c.focus = "BMS 限值压过 DR/峰谷（跨层优先级检查）";

    c.plant.battery_capacity_kwh = 1000.0;
    c.plant.pcs_max_chg_kw = c.plant.pcs_max_dis_kw = 250.0;
    c.plant.pcs_ramp_kw_per_s = 400.0;

    c.limits.pcs_rated_chg_kw = c.limits.pcs_rated_dis_kw = 250.0;
    c.limits.bms_chg_limit_kw = 80.0;    // ← BMS 严格限充
    c.limits.bms_dis_limit_kw = 120.0;   // ← BMS 严格限放
    c.limits.transformer_capacity_kw = 800.0;
    c.limits.d_target_kw = 800.0;

    c.safety.soc_min = 0.10;
    c.safety.soc_max = 0.90;
    c.safety.grid_p_min_kw = -1e9;
    c.safety.grid_p_max_kw = 1e9;

    c.env = [](double, double* load, double* pv) { *load = 300.0; *pv = 100.0; };
    return c;
}

// =====================================================================
// 场景 7：9 策略全量同时启用
//   验证：所有策略 + 所有安全约束同时生效时，全部不变量同时成立。
//         这是"压力测试"—— 任何策略间的耦合缺陷都会在这里暴露。
// =====================================================================
inline ScenarioConfig scenario_7() {
    ScenarioConfig c;
    c.id = "S7";
    c.name = "9策略全量同时启用";
    c.focus = "全部策略+全部安全约束同时生效，全部不变量成立";

    c.plant.battery_capacity_kwh = 1000.0;
    c.plant.pcs_max_chg_kw = c.plant.pcs_max_dis_kw = 250.0;
    c.plant.pcs_ramp_kw_per_s = 400.0;
    c.plant.pcs_tau_s = 0.3;
    c.plant.pcs_deadtime_s = 0.1;

    c.limits.pcs_rated_chg_kw = c.limits.pcs_rated_dis_kw = 250.0;
    c.limits.bms_chg_limit_kw = 200.0;
    c.limits.bms_dis_limit_kw = 200.0;
    c.limits.transformer_capacity_kw = 400.0;
    c.limits.d_target_kw = 300.0;

    c.safety.soc_min = 0.10;
    c.safety.soc_max = 0.90;
    c.safety.grid_p_min_kw = 0.0;        // 不倒送
    c.safety.grid_p_max_kw = 300.0;      // 契约需量
    c.coord.reopt_period_s = 300.0;

    c.grid_min_required = -5.0;
    c.grid_max_required = 300.0 * 1.02;

    c.fc_base_load = 200.0;
    c.fc_pv_peak   = 320.0;

    c.env = [](double t, double* load, double* pv) {
        const double h = std::fmod(t, 1200.0) / 1200.0 * 24.0;
        *load = 200.0 + 130.0 * std::exp(-std::pow(h - 10.0, 2.0) / 8.0)
                      + 170.0 * std::exp(-std::pow(h - 19.0, 2.0) / 6.0);
        *pv   = (h >= 6.0 && h <= 18.0)
                ? 320.0 * std::sin(3.14159265358979 * (h - 6.0) / 12.0) : 0.0;
        // 叠加云遮波动，制造平抑需求
        *pv  += 30.0 * std::sin(t / 9.0);
        if (*pv < 0.0) *pv = 0.0;
    };
    return c;
}

// =====================================================================
// 全部 7 个场景
// =====================================================================
inline std::vector<ScenarioConfig> all_scenarios() {
    return { scenario_1(), scenario_2(), scenario_3(), scenario_4(),
             scenario_5(), scenario_6(), scenario_7() };
}

} // namespace ems
