#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include "AntiReverseController.h"

// =====================================================================
// 防逆流控制器仿真：覆盖设计文档 7.1 全部测试场景 + 1 个陈旧积分压力场景
// 功率平衡（一拍ZOH离散化，P_bat=-req）：
//   P_grid(k) = P_load(k) - P_pv(k) + req(k-1)
// =====================================================================

struct ScenarioResult
{
    double min_P_grid;   // 全程最小并网功率（最大倒送深度），kW
    double max_req;      // 最大充电指令，kW
    double settle_t;     // 扰动发生后首次进入死区(|P_grid-P_grid_min|<deadband_enter)的时间，s
    double P_grid_end;   // 仿真结束时并网功率（稳态残差），kW
};

// 通用场景运行器：warmup_s 预热建立文档声明的初始稳态（预热采用扰动前的恒定
// warm_load/warm_pv，避免预热窗口误入扰动时段）；loadFn/pvFn 为扰动函数；
// t_disturb 为首个扰动时刻，settle 指标仅在扰动发生后统计（进入死区即视为收敛，
// 死区进入阈值决定了允许的稳态残差）
template <typename F1, typename F2>
ScenarioResult runScenario(const std::string& name, const AntiReverseConfig& cfg,
                           F1 loadFn, F2 pvFn,
                           double P_chg_max,
                           double warm_load, double warm_pv, double warmup_s,
                           double t_disturb, double t_end,
                           const std::string& csvName)
{
    AntiReverseController ctrl(cfg);
    std::ofstream f(csvName.c_str());
    if (!f.is_open())
    {
        std::cerr << "open " << csvName << " failed\n";
        std::exit(1);
    }
    f << "Time(s)\tP_load(kW)\tP_pv(kW)\tP_grid(kW)\tChargeReq(kW)\n";

    const double Ts = cfg.Ts;
    double P_grid = 0.0;
    double req = 0.0;
    ScenarioResult r = {1e9, 0.0, -1.0, 0.0};

    // 预热阶段（不写入CSV，仅建立初始稳态）
    for (double t = 0.0; t < warmup_s + 1e-9; t += Ts)
    {
        P_grid = warm_load - warm_pv + req;
        req = ctrl.update(P_grid, warm_pv, P_chg_max, true);
    }

    // 正式仿真
    bool settled = false;
    for (double t = 0.0; t <= t_end + 1e-9; t += Ts)
    {
        P_grid = loadFn(t) - pvFn(t) + req;
        req = ctrl.update(P_grid, pvFn(t), P_chg_max, true);

        r.min_P_grid = std::min(r.min_P_grid, P_grid);
        r.max_req    = std::max(r.max_req, req);
        r.P_grid_end = P_grid;

        if (!settled && t >= t_disturb &&
            std::fabs(P_grid - cfg.P_grid_min) < cfg.deadband_enter)
        {
            settled = true;
            r.settle_t = t;
        }

        f.precision(4);
        f << t << "\t" << loadFn(t) << "\t" << pvFn(t) << "\t"
          << P_grid << "\t" << req << "\n";
    }
    f.close();

    std::cout << "[" << name << "]  min_P_grid=" << r.min_P_grid << " kW, "
              << "max_req=" << r.max_req << " kW, "
              << "settle=" << (r.settle_t >= 0.0 ? std::to_string(r.settle_t) : "N/A") << " s, "
              << "P_grid_end=" << r.P_grid_end << " kW\n";
    return r;
}

// ---------------- 扰动函数 ----------------
// 场景1：光伏突增（负载100恒定，PV 50→200 阶跃@t=1s）
static double load_c100(double) { return 100.0; }
static double pv_step_50_200(double t) { return (t < 1.0) ? 50.0 : 200.0; }

// 场景2：负载突降（PV250恒定，负载200→100 阶跃@t=1s）
static double load_drop_200_100(double t) { return (t < 1.0) ? 200.0 : 100.0; }
static double pv_c250(double) { return 250.0; }

// 场景3：光伏波动（负载100恒定，PV三角波50↔150，周期4s，斜坡率50kW/s）
static double load_c100b(double) { return 100.0; }
static double pv_triangle(double t)
{
    const double period = 4.0;
    double p = std::fmod(t, period);
    double tri = (p < period / 2.0) ? (2.0 * p / period)
                                    : (2.0 * (period - p) / period);   // 0→1→0
    return 50.0 + 50.0 * tri;                                          // 50~150
}

// 场景4：无逆流（负载100，PV 50恒定）
static double load_c100c(double) { return 100.0; }
static double pv_c50(double) { return 50.0; }

// 场景5：光伏骤降（先大盈余充电，再骤降 → 验证陈旧积分清零）
//         PV: t<2→50, 2≤t<32→200, t≥32→50
static double load_c100d(double) { return 100.0; }
static double pv_jump_drop(double t)
{
    if (t < 2.0) return 50.0;
    if (t < 32.0) return 200.0;
    return 50.0;
}

int main()
{
    const double P_chg_max = 200.0;   // BMS允许最大充电功率 kW
    const double Ts = 0.1;            // 控制周期 s

    // 参数整定说明（对齐文档第4章建议范围）：
    //   Kp=0.5：文档 0.5~1.0，取中值保证离散稳定性（Kp→1.0 在 Ts=0.1s+一拍采样延迟
    //           下处于临界稳定，可能出现两拍交替振荡）；
    //   Ki=0.1：文档 0.05~0.1 上限，加快积分收敛（位置式PI的积分速度=Ki*Ts，量级
    //           决定阶跃的完整收敛时间，详见设计文档第4章修订说明）；
    //   Kff：阶跃验证场景置0（文档整定步骤），波动场景取1.0；
    //   integral_max=60000：积分项最大输出=Ki*Ts*integral_max=600kW，
    //           覆盖场景最大持续充电需求150kW（所需积分=150/(Ki*Ts)=15000，留足裕量）。
    const AntiReverseConfig cfgStep = {0.5, 0.1, 0.0, Ts, 2.0, 4.0, 0.0, 60000.0, -60000.0};
    const AntiReverseConfig cfgOsc  = {0.5, 0.1, 1.0, Ts, 2.0, 4.0, 0.0, 60000.0, -60000.0};

    std::cout << "AntiReverse Controller Simulation (Ts=" << Ts << " s)\n";

    // 场景1：光伏突增（初始 P_grid=50，阶跃后需充电100kW 保持 P_grid=0）
    // 完整收敛受积分速度限制（Ki=0.1 时时间常数约14s，进入死区约65s）
    runScenario("1-光伏突增", cfgStep, load_c100, pv_step_50_200,
                P_chg_max, 100.0, 50.0, 60.0, 1.0, 90.0, "sim_sc1_pv_step.csv");

    // 场景2：负载突降（初始 P_grid≈0 充电50kW；突降后需充电150kW）
    runScenario("2-负载突降", cfgStep, load_drop_200_100, pv_c250,
                P_chg_max, 200.0, 250.0, 60.0, 1.0, 90.0, "sim_sc2_load_drop.csv");

    // 场景3：光伏波动（开启光伏前馈 Kff=1.0，扰动自t=0开始）
    runScenario("3-光伏波动", cfgOsc, load_c100b, pv_triangle,
                P_chg_max, 100.0, 50.0, 0.0, 0.0, 20.0, "sim_sc3_pv_osc.csv");

    // 场景4：无逆流（控制器应全程输出0，无扰动）
    runScenario("4-无逆流", cfgStep, load_c100c, pv_c50,
                P_chg_max, 100.0, 50.0, 0.0, 1e9, 5.0, "sim_sc4_no_reverse.csv");

    // 场景5：光伏骤降（先充电再骤降，验证陈旧积分清零，避免安全侧过充）
    runScenario("5-光伏骤降", cfgStep, load_c100d, pv_jump_drop,
                P_chg_max, 100.0, 50.0, 60.0, 2.0, 44.0, "sim_sc5_pv_drop.csv");

    std::cout << "finished, sim_scN_*.csv generated\n";
    return 0;
}

