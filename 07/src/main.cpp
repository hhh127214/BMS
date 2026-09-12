// =====================================================================
// 07/ — 周期 7：实时控制闭环 演示
//
//   场景 C：100ms 闭环全链路 + 「无延迟 / 无振荡」量化验收
//     试验 1：指令跟随与倒送抑制（无延迟）
//     试验 2：并网点抖动治理三档对照（基线 / 量测滤波 / 量测滤波+死区滞环）
//
//   闭环链路：电表实时数据 → EMS算法计算 → 策略仲裁 → 安全约束 →
//             PCS执行 → 实际功率反馈 → EMS迭代修正
//   目标（设计文档 §7 周期 7）：实现毫秒级实时控制闭环，无延迟、无振荡。
//
// 编译：g++ -std=c++17 -Wall -O2 -I src -I ../04/src -I ../05/src
//           -I ../06/src -I ../08/src src/main.cpp -o build/loop_demo.exe
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

// =====================================================================
// 场景 C（周期 7）：实时控制闭环
// =====================================================================
struct LoopCaseResult {
    LoopMetrics m;
    int    nseg = 0;
    double seg_steady_err[5] = {0, 0, 0, 0, 0};  // 各段平均绝对跟踪误差
    double seg_min_grid[5]   = {0, 0, 0, 0, 0};  // 各段关口最小功率（<0 即倒送）
    double seg_max_grid[5]   = {0, 0, 0, 0, 0};  // 各段关口最大功率
    double seg_mean_grid[5]  = {0, 0, 0, 0, 0};
};

static LoopCaseResult run_loop_case(const std::function<void(EmsRuntime&, double)>& env,
                                    int steps, double dt,
                                    const std::vector<double>& seg_edges,
                                    double ramp_kw_per_s, bool deadband_on,
                                    double l2_limit_kw) {
    EmsRuntime rt;
    rt.config().dt_s = dt;
    rt.config().l2_correction_max_kw = l2_limit_kw;
    rt.config().output_deadband_kw = deadband_on ? 2.0 : 0.0;
    rt.config().output_switch_delay = deadband_on ? 3 : 1;
    rt.device_limits().transformer_capacity_kw = 800.0;   // 隔离变压器约束
    rt.device_limits().d_target_kw = 250.0;
    rt.safety_params().ramp_kw_per_s = ramp_kw_per_s;
    rt.safety_params().grid_p_min_kw = 0.0;               // 不允许倒送（硬边界）
    rt.safety_params().grid_p_max_kw = 1e9;
    rt.apply_configs();
    rt.shaper().set_deadband(rt.config().output_deadband_kw);
    rt.shaper().set_switch_delay(rt.config().output_switch_delay);

    rt.run(steps, dt, [&](EmsRuntime& r, int i) { env(r, i * dt); });

    LoopCaseResult out;
    out.m = rt.metrics();
    out.nseg = static_cast<int>(seg_edges.size()) + 1;

    std::vector<double> err_sum(out.nseg, 0.0), cnt(out.nseg, 0.0);
    for (int k = 0; k < out.nseg; ++k) {
        out.seg_min_grid[k] = 1e18;
        out.seg_max_grid[k] = -1e18;
    }
    for (const auto& r : rt.log()) {
        int seg = 0;
        while (seg < static_cast<int>(seg_edges.size()) && r.t >= seg_edges[seg]) ++seg;
        err_sum[seg] += std::fabs(r.p_actual - r.p_cmd);
        cnt[seg] += 1.0;
        out.seg_min_grid[seg] = std::min(out.seg_min_grid[seg], r.p_grid);
        out.seg_max_grid[seg] = std::max(out.seg_max_grid[seg], r.p_grid);
        out.seg_mean_grid[seg] += r.p_grid;
    }
    for (int k = 0; k < out.nseg; ++k) {
        out.seg_steady_err[k] = cnt[k] > 0 ? err_sum[k] / cnt[k] : 0.0;
        out.seg_mean_grid[k]  = cnt[k] > 0 ? out.seg_mean_grid[k] / cnt[k] : 0.0;
        if (cnt[k] <= 0) { out.seg_min_grid[k] = 0.0; out.seg_max_grid[k] = 0.0; }
    }
    return out;
}

static void scenario7_realtime_loop() {
    banner("场景 C（周期 7）：实时控制闭环 —— 100ms 闭环 + 无延迟/无振荡量化");

    std::printf("\n闭环链路：电表实时数据 → EMS算法 → 策略仲裁 → 安全约束 → PCS执行 → 功率反馈 → 迭代修正\n");
    std::printf("被控对象：PCS 死区 0.2s + 一阶惯性 τ=0.5s + 物理变化率 300kW/s + 充放效率 0.95\n");

    // ---------------------------------------------------------------
    // 试验 1：阶跃激励（防逆流 + 需量管理）
    // ---------------------------------------------------------------
    sub("试验 1：阶跃激励 —— 光伏 150→400kW（防逆流）、负荷 200→450kW（需量管理）");
    auto step_env = [](EmsRuntime& r, double t) {
        double load = 200.0, pv = 150.0;
        if (t >= 100.0 && t < 200.0)      { pv = 350.0; load = 200.0; }
        else if (t >= 200.0 && t < 400.0) { load = 450.0; pv = 100.0; }
        else if (t >= 400.0)              { load = 200.0; pv = 150.0; }
        r.set_environment(load, pv);
    };
    {
        auto res = run_loop_case(step_env, 6000, 0.1, {100.0, 200.0, 400.0}, 200.0, true, 100.0);
        std::printf("  闭环指标：%s\n", res.m.to_string().c_str());
        std::printf("\n  %-18s %-12s %-12s %-12s %-12s\n",
                    "时段", "平均跟踪误差", "关口均值", "关口最小", "关口最大");
        const char* names[] = {"0-100s 基线", "100-200s 光伏大发",
                               "200-400s 负荷尖峰", "400-600s 回落"};
        for (int k = 0; k < res.nseg; ++k) {
            std::printf("  %-18s %10.2f kW %10.1f kW %10.1f kW %10.1f kW%s\n",
                        names[k], res.seg_steady_err[k], res.seg_mean_grid[k],
                        res.seg_min_grid[k], res.seg_max_grid[k],
                        res.seg_min_grid[k] < -1.0 ? "   ← 暂态倒送" : "");
        }
        std::printf("\n  判定：无延迟=%s  无振荡=%s\n",
                    res.m.no_lag() ? "成立" : "不成立",
                    res.m.no_oscillation() ? "成立" : "不成立");
        std::printf("  说明：阶跃瞬间的暂态倒送源于 PCS 物理惯性（0.2s 死区 + τ=0.5s），\n");
        std::printf("        稳态段关口功率已稳定在 0 附近，防逆流约束生效。\n");
    }

    // ---------------------------------------------------------------
    // 试验 2：并网点边界抖动的三层治理 —— 量测滤波 / 死区 / 滞环
    // ---------------------------------------------------------------
    sub("试验 2：小信号抖动场景 —— 量测滤波 + 死区+滞环 逐级治理（振荡指标）");
    std::printf("  激励：净负荷恒为 0，但**量测噪声** σ=3kW（关口功率 σ≈4.2kW）\n");
    std::printf("  机理：并网边界的基准量 base = P_load − P_pv 来自电表，带噪声 →\n");
    std::printf("        安全兜底必须把指令压进该边界 → 储能被迫跟随一个**抖动的安全边界**。\n");
    std::printf("        该抖动在安全层之后，输出死区压不住，必须从量测侧根治。\n");
    std::printf("  （隔离光伏平抑策略；需量目标设在均值之上，确保只有防逆流在动作）\n");
    auto ripple_env = [](EmsRuntime& r, double t) {
        (void)t;
        r.set_environment(200.0, 200.0);   // 净负荷恒为 0，抖动全部来自量测噪声
    };
    auto run_ripple = [&](bool deadband, bool filter) {
        EmsRuntime rt;
        rt.config().dt_s = 0.1;
        rt.config().l2_correction_max_kw = 60.0;
        rt.config().output_deadband_kw = deadband ? 5.0 : 0.0;
        rt.config().output_switch_delay = deadband ? 5 : 1;
        PlantConfig pc;                       // 量测噪声注入（被控对象侧）
        pc.noise_kw = 3.0;
        rt.configure_plant(pc);
        rt.device_limits().transformer_capacity_kw = 800.0;
        rt.device_limits().d_target_kw = 260.0;    // 高于净负荷均值 → 需量管理不动作
        rt.safety_params().ramp_kw_per_s = 200.0;
        rt.safety_params().grid_p_min_kw = 0.0;    // 不允许倒送 → 防逆流在边界抖动
        rt.safety_params().grid_filter_alpha = filter ? 0.05 : 1.0;  // 1.0 = 不滤波
        rt.apply_configs();
        rt.shaper().set_deadband(rt.config().output_deadband_kw);
        rt.shaper().set_switch_delay(rt.config().output_switch_delay);
        // 隔离光伏平抑策略：本试验只考察 防逆流 的边界小信号输出 + 死区作用
        rt.manager().stop(strategy_id::kPvSmoothing);
        rt.run(3000, 0.1, [&](EmsRuntime& r, int i) { ripple_env(r, i * 0.1); });
        return rt.metrics();
    };
    struct RippleCase { const char* name; LoopMetrics m; };
    RippleCase cases[] = {
        {"① 基线（无滤波/无死区）", run_ripple(false, false)},
        {"② + 量测滤波(α=0.05)",    run_ripple(false, true)},
        {"③ + 量测滤波 + 死区滞环",  run_ripple(true,  true)},
    };
    std::printf("\n  %-24s %-14s %-14s %-14s %-12s\n",
                "配置", "指令总行程", "方向反转", "大幅跳变", "无振荡判定");
    for (const auto& c : cases) {
        std::printf("  %-24s %-14s %-14s %-14s %-12s\n", c.name,
                    (std::to_string((long long)c.m.cmd_travel_kw) + " kW").c_str(),
                    (std::to_string(c.m.cmd_reversals) + " 次").c_str(),
                    (std::to_string(c.m.cmd_jitter) + " 次").c_str(),
                    c.m.no_oscillation() ? "成立" : "不成立");
    }
    auto drop = [](double on, double base) {
        return base > 0.0 ? 100.0 * (1.0 - on / base) : 0.0;
    };
    const auto& b = cases[0].m;
    std::printf("\n  相对基线（①）的降低幅度：\n");
    std::printf("  %-24s %-16s %-16s %-16s\n", "", "总行程", "方向反转", "大幅跳变");
    for (int i = 1; i < 3; ++i) {
        const auto& m = cases[i].m;
        std::printf("  %-24s %-16s %-16s %-16s\n", cases[i].name,
                    (std::to_string((int)drop(m.cmd_travel_kw, b.cmd_travel_kw)) + "%").c_str(),
                    (std::to_string((int)drop(m.cmd_reversals, b.cmd_reversals)) + "%").c_str(),
                    (std::to_string((int)drop(m.cmd_jitter, b.cmd_jitter)) + "%").c_str());
    }
    std::printf("\n  结论：边界抖动的**根因在量测侧**（安全边界的基准量带噪声），\n");
    std::printf("        故『量测低通滤波』是主药（把抖动从源头压掉），\n");
    std::printf("        『输出死区+滞环』是辅药（滤掉残余的小幅往复，保护 PCS 执行机构）。\n");

    // ---------------------------------------------------------------
    // 试验 3：变化率限制对暂态倒送的影响
    // ---------------------------------------------------------------
    sub("试验 3：变化率限制 50 vs 2000 kW/s —— 对暂态倒送时长的影响");
    std::printf("  （被控对象 PCS 物理变化率上限 300 kW/s：50 时安全层是瓶颈，2000 时物理层是瓶颈）\n");
    auto r_slow = run_loop_case(step_env, 3000, 0.1, {100.0}, 50.0,   true, 100.0);
    auto r_fast = run_loop_case(step_env, 3000, 0.1, {100.0}, 2000.0, true, 100.0);
    std::printf("\n  %-22s %-14s %-14s %-14s\n", "变化率", "最大倒送", "倒送时长", "安全裁剪");
    std::printf("  %-22s %10.1f kW %10.1f s %10d 次\n",
                "50 kW/s（安全层限）", r_slow.m.max_reverse_kw,
                r_slow.m.reverse_duration_s, r_slow.m.safety_clip_count);
    std::printf("  %-22s %10.1f kW %10.1f s %10d 次\n",
                "2000 kW/s（物理层限）", r_fast.m.max_reverse_kw,
                r_fast.m.reverse_duration_s, r_fast.m.safety_clip_count);
    std::printf("\n  结论：变化率限制越紧，PCS 反向越慢，暂态倒送**持续时间越长**；\n");
    std::printf("        越松则倒送时间越短，但 PCS 电流冲击越大（工程折中）。\n");
    std::printf("        峰值倒送由『光伏阶跃瞬间的实际出力』决定，与变化率无关 ——\n");
    std::printf("        根治手段是光伏逆变器限功率（削减 PV）或提前预留充电裕量。\n");
}


// =====================================================================
int main() {
    std::printf("============================================================\n");
    std::printf("  工商业储能 EMS —— 07/ 周期 7 实时控制闭环演示\n");
    std::printf("============================================================\n");

    scenario7_realtime_loop();

    banner("周期 7 交付小结");
    std::printf("  闭环链路：采集 → 故障判定 → 安全预判 → 状态机 → 协同 → 策略\n");
    std::printf("            → 仲裁 → L2纠偏叠加 → 输出整形 → 安全兜底 → 门控 → 执行 → 反馈\n");
    std::printf("  死区/滞环放在输出最后一级；上游加量测低通，否则安全层会把指令\n");
    std::printf("  钳到带噪声的并网边界上，直接覆盖下游死区（抖动治理的关键）\n");
    std::printf("\n");
    return 0;
}
