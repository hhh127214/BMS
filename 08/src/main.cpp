// =====================================================================
// 08/ — 周期 8：优化调度与实时控制协同 演示
//
//   场景 D：24h 分层管控
//     预测分析 → 优化层生成日间运行计划 → 实时控制层动态纠偏 →
//     安全层兜底约束 → PCS 执行
//   目标（设计文档 §7 周期 8）：实现**优化层规划、实时层纠偏、安全层兜底**
//   的分层管控体系。
//
//   计划来源优先取 01/ 的 MILP 计划（data/day_plan_sample.json），
//   缺失时回退到本模块的贪心规划器（build_plan_greedy）。
//
// 编译：g++ -std=c++17 -Wall -O2 -I src -I ../04/src -I ../05/src
//           -I ../06/src -I ../07/src src/main.cpp -o build/coord_demo.exe
// =====================================================================

#include "realtime_loop.h"
#include "plan_loader.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace ems;

// =====================================================================
// 工具
// =====================================================================
static void banner(const std::string& s) {
    std::printf("\n");
    std::printf("============================================================\n");
    std::printf("  %s\n", s.c_str());
    std::printf("============================================================\n");
}
static void sub(const std::string& s) {
    std::printf("\n---- %s ----\n", s.c_str());
}

// 演示用 96 点（15 min）预测：负荷 / 光伏 / 电价
// 公式与 tools/make_plan_sample.py 中的一致，保证 01/ 的 MILP 输入与
// 05/ 的实时层预测序列**同源**。
static ForecastSeries build_demo_forecast() {
    ForecastSeries fc;
    fc.step_s = 900.0;   // 15 min
    const int n = 96;
    for (int i = 0; i < n; ++i) {
        double h = i * 0.25;   // 小时

        // 电价：峰谷分时
        double price;
        if (h < 7.0)        price = 0.30;   // 谷
        else if (h < 9.0)   price = 0.60;   // 平
        else if (h < 12.0)  price = 1.00;   // 峰
        else if (h < 14.0)  price = 0.60;   // 平
        else if (h < 17.0)  price = 1.00;   // 峰
        else if (h < 21.0)  price = 1.20;   // 尖峰
        else if (h < 23.0)  price = 0.60;   // 平
        else                price = 0.30;   // 谷

        // 负荷：基础 180 + 上午/傍晚峰 + 日周期波动
        double load = 180.0
                    + 120.0 * std::exp(-std::pow(h - 10.0, 2.0) / 8.0)
                    + 160.0 * std::exp(-std::pow(h - 19.0, 2.0) / 6.0)
                    + 40.0 * std::sin(2.0 * 3.14159265358979 * h / 24.0);

        // 光伏：6:00-18:00 正弦出力，峰值 300 kW
        double pv = 0.0;
        if (h >= 6.0 && h <= 18.0) {
            pv = 300.0 * std::sin(3.14159265358979 * (h - 6.0) / 12.0);
        }

        fc.price.push_back(price);
        fc.p_load_kw.push_back(load);
        fc.p_pv_kw.push_back(pv);
    }
    fc.loaded = true;
    return fc;
}

// =====================================================================
// 场景 D（周期 8）：优化调度与实时控制协同
// =====================================================================
static void scenario8_dispatch_coordination() {
    banner("场景 D（周期 8）：优化调度与实时控制协同 —— 24h 计划 + 15min 滚动 + 实时纠偏");

    EmsRuntime rt;
    rt.config().dt_s = 0.1;
    rt.config().log_every = 100;   // 24h × 10Hz 全记录太大，降采样 100 倍

    // 设备与安全参数（工商业储能典型值）
    PlantConfig pc;
    pc.battery_capacity_kwh = 1000.0;
    pc.soc_init = 0.50;
    pc.pcs_max_chg_kw = 250.0;
    pc.pcs_max_dis_kw = 250.0;
    pc.pcs_ramp_kw_per_s = 400.0;
    pc.pcs_tau_s = 0.3;
    pc.pcs_deadtime_s = 0.1;
    rt.configure_plant(pc);

    rt.device_limits().pcs_rated_chg_kw = 250.0;
    rt.device_limits().pcs_rated_dis_kw = 250.0;
    rt.device_limits().bms_chg_limit_kw = 250.0;
    rt.device_limits().bms_dis_limit_kw = 250.0;
    rt.device_limits().transformer_capacity_kw = 700.0;
    rt.device_limits().d_target_kw = 350.0;    // 契约需量

    rt.config().l2_correction_max_kw = 100.0;  // 实时层有界纠偏

    rt.safety_params().soc_min = 0.10;
    rt.safety_params().soc_max = 0.90;
    rt.safety_params().soc_warn_low = 0.15;
    rt.safety_params().soc_warn_high = 0.85;
    rt.safety_params().ramp_kw_per_s = 300.0;
    rt.safety_params().grid_p_min_kw = 0.0;      // 不允许倒送
    // 契约需量 350kW 作为**并网硬边界**（L1 兜底）：
    //   实时层（L2 需量管理）先做前瞻性削峰，安全层再兜住任何漏网之鱼。
    //   这正是"优化层规划 → 实时层纠偏 → 安全层兜底"三层各司其职的体现。
    rt.safety_params().grid_p_max_kw = 350.0;
    rt.coordinator_config().reopt_period_s = 900.0;  // 15 min 滚动
    rt.apply_configs();

    // 需量管理控制器增益整定（默认 Kp_avg=2.0 过于激进，会饱和到边界）
    rt.manager().set_param(strategy_id::kDemandMgmt, "Kp_avg", 0.6);

    // ---- 预测分析 ----
    ForecastSeries fc = build_demo_forecast();
    rt.set_forecast(fc);
    std::printf("\n预测序列：%zu 点 × %.0f min = %.0f h\n",
                fc.size(), fc.step_s / 60.0, fc.duration_s() / 3600.0);
    double p_lo = 0.0, p_hi = 0.0;
    fc.price_quantiles(&p_lo, &p_hi);
    std::printf("电价分位：谷阈 %.2f 元/kWh，峰阈 %.2f 元/kWh\n", p_lo, p_hi);

    // ---- 优化层：优先接 01/ 的 MILP 计划，失败则降级 ----
    sub("优化层：日间运行计划");
    bool from_01 = false;
    std::string init_plan_source = "heuristic_greedy（内置兜底）";
    // 从 08/ 目录跑取第一个；从仓库根目录跑取第二个
    for (const char* path : {"data/day_plan_sample.json", "08/data/day_plan_sample.json"}) {
        if (rt.load_plan_file(path)) {
            from_01 = true;
            const DayPlan& p = rt.coordinator().plan();
            init_plan_source = p.source + "（" + path + "）";
            std::printf("  已加载 01/ 优化层计划：%s\n", path);
            std::printf("  strategy=%s  horizon=%d  step=%.0fs  source=%s\n",
                        p.strategy.c_str(), p.horizon, p.step_s, p.source.c_str());
            std::printf("  计划充电 %.1f kWh / 放电 %.1f kWh / SOC 摆幅 %.1f%%\n",
                        p.planned_charge_kwh(), p.planned_discharge_kwh(),
                        p.soc_swing() * 100.0);
            break;
        }
    }
    if (!from_01) {
        std::printf("  未找到 01/ 计划样例 → 使用内置贪心兜底优化器（功能降级）\n");
    }

    // ---- 24h 闭环 ----
    sub("24h 闭环运行（优化层规划 → 实时层纠偏 → 安全层兜底）");
    auto env = [&fc](EmsRuntime& r, int i) {
        double t = i * 0.1;
        double load = 0.0, pv = 0.0, pr = 0.0;
        fc.sample(t, &load, &pv, &pr);
        r.set_environment(load, pv);
    };
    const int steps = static_cast<int>(24 * 3600 / 0.1);
    std::printf("  仿真步数 %d（dt=0.1s，24h）...\n", steps);
    rt.run(steps, 0.1, env);

    const auto& st = rt.coordinator().stats();
    const DayPlan& plan = rt.coordinator().plan();
    const auto& m = rt.metrics();

    std::printf("\n  ── ① 优化层（规划）──\n");
    std::printf("    初始计划来源    : %s\n", init_plan_source.c_str());
    std::printf("    滚动重优化      : %d 次（每 15 min 一次，24h 理论 96 次）\n",
                rt.coordinator().reopt_count());
    std::printf("    末次计划来源    : %s\n", plan.source.c_str());
    std::printf("      └ 说明：滚动重算在**本进程内**用启发式近似；真实部署应把\n");
    std::printf("        实测 SOC 回灌给 01/ 的 MILP HTTP 服务重解（同接口，见 01/docs/api.md）。\n");
    std::printf("    末次计划充电/放电: %.1f / %.1f kWh（未来 24h 计划量）\n",
                plan.planned_charge_kwh(), plan.planned_discharge_kwh());
    std::printf("    末次计划 SOC 摆幅: %.1f%%\n", plan.soc_swing() * 100.0);

    std::printf("\n  ── ② 实时层（纠偏）──\n");
    std::printf("    平均绝对纠偏量  : %.2f kW（SOC 反馈 + 负荷前馈 + 窗口电量预算）\n",
                st.mean_abs_correction());
    std::printf("    纠偏权限上限    : ±%.0f kW（超出部分由 L0/L1 安全层兜底）\n",
                rt.config().l2_correction_max_kw);

    std::printf("\n  ── ③ 跟踪效果 ──\n");
    std::printf("    计划-实际偏差   : 均值 %.2f kW / 最大 %.2f kW\n",
                st.mean_abs_track_err(), st.max_abs_track_err);
    std::printf("    平均 SOC 偏差   : %.4f（vs 计划 SOC 轨迹）\n", st.mean_abs_soc_err());
    std::printf("    在计划上占比    : %.1f%%（容差 ±%.0f kW）\n",
                st.on_plan_ratio() * 100.0, rt.coordinator_config().on_plan_tol_kw);

    std::printf("\n  ── ④ 关口/需量 KPI ──\n");
    std::printf("    关口功率均值    : %.1f kW\n", m.mean_grid_kw);
    std::printf("    关口功率峰值    : %.1f kW（契约需量 %.0f kW）\n",
                m.max_grid_kw, rt.device_limits().d_target_kw);
    std::printf("    倒送功率        : 最大 %.1f kW，累计 %.1f s（%.1f%% 时间）\n",
                m.max_reverse_kw, m.reverse_duration_s,
                100.0 * m.reverse_duration_s / (24.0 * 3600.0));

    std::printf("\n  ── ⑤ 安全层（兜底）──\n");
    std::printf("    安全裁剪次数    : %d（采样 %d 条，实际步数 %d）\n",
                m.safety_clip_count, m.samples, steps);
    std::printf("    状态门控次数    : %d；状态迁移 %d 次\n",
                m.state_gate_count, m.state_changes);

    std::printf("\n  ── ⑥ 能量账本与 SOC ──\n");
    std::printf("    实际充电 %.1f kWh / 实际放电 %.1f kWh / 末态 SOC %.3f\n",
                rt.plant().energy_charged_kwh(), rt.plant().energy_discharged_kwh(),
                rt.plant().soc());
    double soc_lo = 1e18, soc_hi = -1e18;
    for (const auto& r : rt.log()) {
        soc_lo = std::min(soc_lo, r.soc);
        soc_hi = std::max(soc_hi, r.soc);
    }
    std::printf("    全过程 SOC ∈ [%.3f, %.3f]（安全区间 [%.2f, %.2f]）\n",
                soc_lo, soc_hi, rt.safety_params().soc_min, rt.safety_params().soc_max);

    std::printf("\n  ── ⑦ 闭环指标 ──\n");
    std::printf("    %s\n", m.to_string().c_str());

    rt.dump_csv("build/scenario8_24h.csv");
    std::printf("\n  详细时序已导出：build/scenario8_24h.csv\n");

    sub("分层协同职责小结");
    std::printf("  预测分析层 : 96 点负荷/光伏/电价时序（%.0f min 分辨率）\n", fc.step_s / 60.0);
    std::printf("  优化层     : 01/ MILP 日间计划 + 每 15 min 以实测 SOC 滚动重算\n");
    std::printf("  实时层     : SOC 反馈 + 负荷前馈 + 窗口电量预算 → 100ms 纠偏\n");
    std::printf("  安全层     : 9 类约束统一收敛，任何指令不得突破设备/并网边界\n");
    std::printf("  执行层     : PCS 按 PermissionRange 限幅执行，实际功率回灌下一拍\n");
}


// =====================================================================
int main() {
    std::printf("============================================================\n");
    std::printf("  工商业储能 EMS —— 08/ 周期 8 优化调度与实时控制协同演示\n");
    std::printf("============================================================\n");

    scenario8_dispatch_coordination();

    banner("周期 8 交付小结");
    std::printf("  优化层：01/ MILP 日间计划（96 槽）；缺失时贪心规划器兜底\n");
    std::printf("  实时层：SOC 偏差反馈(P+I, 抗饱和) + 负荷偏差前馈 + 窗口电量预算\n");
    std::printf("  安全层：L0/L1 区间收敛兜底，任何纠偏都无法突破安全边界\n");
    std::printf("  回退规划器预留光伏余电裕度，避免凌晨充满导致中午被迫倒送\n");
    std::printf("\n");
    return 0;
}
