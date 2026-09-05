#include <iostream>
#include <fstream>
#include <iomanip>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>
#include "DemandController.h"

// =====================================================================
// 需量管理控制器仿真（滑动窗口模型，覆盖设计文档7.3测试场景）
// 功率平衡（一拍ZOH，P_grid反映上一拍放电指令的效果）：
//   P_grid(k) = P_load(k) - P_bat(k-1),  P_bat = P_req
// =====================================================================

// 时间分段工况
struct Phase {
    double t0, t1;          // [t0, t1)
    double P_load;          // 负载功率 kW
    double P_dis_max;       // 最大允许放电功率 kW
    bool en;                // 放电使能
    bool comm;              // 关口电表通信异常
};

static const Phase& phaseAt(const std::vector<Phase>& phases, double t)
{
    for (size_t i = 0; i < phases.size(); ++i)
    {
        if (t >= phases[i].t0 && t < phases[i].t1) return phases[i];
    }
    return phases.back();
}

struct ScenarioResult {
    double max_P_grid;     // 最大关口功率 kW
    double max_A;          // 全程最大窗口平均 kW
    double max_A_normal;   // 额定工况（使能且无通信故障且P_dis_max>0）下最大窗口平均 kW
    double max_P_req;      // 最大放电指令 kW
    double over_ratio;     // 窗口平均超过 D_target 的时间占比
};

static ScenarioResult runScenario(const std::string& name,
                                  const DemandController::Config& cfg,
                                  const std::vector<Phase>& phases,
                                  const std::string& csvName)
{
    DemandController ctrl(cfg);
    std::ofstream f(csvName.c_str());
    if (!f.is_open())
    {
        std::cerr << "open " << csvName << " failed\n";
        std::exit(1);
    }
    f << "Time_s,Load_kW,P_grid_kW,P_bat_kW,A_window_kW,P_req_kW,en,comm,alarm\n";
    f << std::fixed << std::setprecision(3);

    const double Ts = cfg.Ts;
    double P_bat = 0.0;
    ScenarioResult r = {0.0, 0.0, 0.0, 0.0, 0.0};
    long over_cnt = 0, total_cnt = 0;
    const double t_end = phases.back().t1;

    for (double t = 0.0; t <= t_end + 1e-9; t += Ts)
    {
        const Phase& ph = phaseAt(phases, t);
        double P_grid = ph.P_load - P_bat;                    // 一拍ZOH
        double P_req = ctrl.update(P_grid, ph.P_dis_max, ph.en, ph.comm);
        P_bat = P_req;

        double A = ctrl.getWindowAverage();
        bool normal_op = ph.en && ph.P_dis_max > 0.0 && !ph.comm;
        r.max_P_grid = std::max(r.max_P_grid, P_grid);
        r.max_A      = std::max(r.max_A, A);
        if (normal_op) r.max_A_normal = std::max(r.max_A_normal, A);
        r.max_P_req  = std::max(r.max_P_req, P_req);
        ++total_cnt;
        if (A > cfg.D_target) ++over_cnt;

        f << t << "," << ph.P_load << "," << P_grid << "," << P_bat << ","
          << A << "," << P_req << ","
          << (ph.en ? 1 : 0) << "," << (ph.comm ? 1 : 0) << ","
          << (ctrl.hasAlarm() ? 1 : 0) << "\n";
    }
    f.close();
    r.over_ratio = total_cnt ? static_cast<double>(over_cnt) / total_cnt : 0.0;

    std::cout << "[" << name << "]  max_P_grid=" << r.max_P_grid
              << " kW, max_A=" << r.max_A
              << " kW, max_A(额定)=" << r.max_A_normal
              << " kW, max_P_req=" << r.max_P_req
              << " kW, over_ratio=" << r.over_ratio << "\n";
    return r;
}

int main()
{
    // 参数：压缩窗口100s（等价900s滑动窗口，N=round(100/0.1)=1000）
    // 说明：N越大，单样本对窗口平均的影响越小；真实900s窗口(N=9000)下
    // 限速爬坡期的瞬时超调可忽略（本压缩仿真N=1000下约≤0.5%）。
    DemandController::Config cfg;
    cfg.T_window = 100.0;
    cfg.D_target = 250.0;
    cfg.dP_max = 50.0;
    cfg.Kp_avg = 100.0;
    cfg.Ki = 20.0;       // 增强积分：维持放电基值，抑制窗口历史导致的通断游猎
    cfg.Ts = 0.1;

    // 场景1：综合工况（文档7.3场景的串联）
    //   [0,105)  负载200（启动：窗口在t=100填满，此前为窗口未满算法）
    //   [105,150)负载300（需量动作：满窗算法，滚动平均精确收敛到 D_target=250）
    //   [150,155)通信中断（输出0）
    //   [155,170)负载300（恢复）
    //   [170,175)禁止放电（输出0，窗口仍记录实际功率）
    //   [175,185)负载300（恢复）
    //   [185,190)P_dis_max=0（输出0）
    //   [190,205)负载250（回落，放电退出）
    //   [205,220)负载200（安全）
    std::vector<Phase> sc1 = {
        {  0.0, 105.0, 200.0, 150.0, true,  false},
        {105.0, 150.0, 300.0, 150.0, true,  false},
        {150.0, 155.0, 300.0, 150.0, true,  true},
        {155.0, 170.0, 300.0, 150.0, true,  false},
        {170.0, 175.0, 300.0, 150.0, false, false},
        {175.0, 185.0, 300.0, 150.0, true,  false},
        {185.0, 190.0, 300.0,   0.0, true,  false},
        {190.0, 205.0, 250.0, 150.0, true,  false},
        {205.0, 220.0, 200.0, 150.0, true,  false},
    };
    runScenario("1-综合工况", cfg, sc1, "demand_sim.csv");

    // 场景2：持续高负荷/已越限
    //   负载450 > D_target+P_dis_max(400)：即使满功率放电，P_grid=300 仍超目标
    //   → 窗口平均必然越限，验证告警与满功率放电
    std::vector<Phase> sc2 = {
        { 0.0, 105.0, 200.0, 150.0, true, false},
        {105.0, 155.0, 450.0, 150.0, true, false},
        {155.0, 180.0, 200.0, 150.0, true, false},
    };
    runScenario("2-持续高负荷-已越限", cfg, sc2, "demand_sim_over.csv");

    // 场景3：纯需量动作（无安全中断，隔离验证 PR-02 窗口平均 ±1%）
    //   [0,105) 负载200（窗口填满）
    //   [105,210)负载300（需量动作，满窗算法）
    //   [210,230)负载200（负荷回落）
    std::vector<Phase> sc3 = {
        {  0.0, 105.0, 200.0, 150.0, true, false},
        {105.0, 210.0, 300.0, 150.0, true, false},
        {210.0, 230.0, 200.0, 150.0, true, false},
    };
    runScenario("3-纯需量动作", cfg, sc3, "demand_sim_perf.csv");

    // 场景4：长时需量（验证债务自然滚出后的真正稳态与长期稳定性）
    //   [0,105)  负载200（窗口填满）
    //   [105,330)负载300（持续225s：窗口完全滚出瞬态样本，验证无极限环、
    //            稳态收敛到 X=D_target+(L−D)/(1+N)=250.05）
    //   [330,360)负载200（回落）
    std::vector<Phase> sc4 = {
        {  0.0, 105.0, 200.0, 150.0, true, false},
        {105.0, 330.0, 300.0, 150.0, true, false},
        {330.0, 360.0, 200.0, 150.0, true, false},
    };
    runScenario("4-长时需量-长期稳定", cfg, sc4, "demand_sim_long.csv");

    std::cout << "finished, demand_sim*.csv generated\n";
    return 0;
}
