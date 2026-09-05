#define _USE_MATH_DEFINES   // 让 MinGW g++ 的 <cmath> 暴露 M_PI
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <string>
#include "SmoothingController.h"

/**
 * @brief 生成正弦光伏功率
 */
double gen_pv_sine(double time, double base, double amp, double period) {
    return base + amp * std::sin(2.0 * M_PI * time / period);
}

/**
 * @brief 运行单个仿真场景，输出CSV文件
 * @param scenario_name         场景名称
 * @param total_steps           总步数
 * @param Ts                    控制周期（秒）
 * @param P_pv_base             光伏基准功率（kW）
 * @param P_pv_amp              光伏波动幅值（kW）
 * @param wave_period           波动周期（秒）
 * @param SOC_init              初始SOC（%）
 * @param E_battery             电池容量（kWh），影响SOC变化速率
 * @param enable_comm_fault     是否模拟通信中断
 * @param fault_start           故障开始时间（秒）
 * @param fault_duration        故障持续时间（秒）
 * @param enable_smoothing_toggle 是否模拟平抑功能开关
 * @param toggle_start          关闭开始时间（秒）
 * @param toggle_duration       关闭持续时间（秒）
 * @param controller            控制器对象
 */
void runScenario(const std::string& scenario_name,
                 int total_steps, double Ts,
                 double P_pv_base, double P_pv_amp, double wave_period,
                 double SOC_init, double E_battery,
                 bool enable_comm_fault, double fault_start, double fault_duration,
                 bool enable_smoothing_toggle, double toggle_start, double toggle_duration,
                 SmoothingController& controller) {
    // 重置控制器状态
    controller.reset();

    std::string csv_filename = "smoothing_" + scenario_name + ".csv";
    std::ofstream csv_file(csv_filename);
    if (!csv_file.is_open()) {
        std::cerr << "无法打开输出文件: " << csv_filename << std::endl;
        return;
    }

    csv_file << "Time_s,P_pv_kW,P_smooth_kW,P_comp_kW,SOC_pct,P_grid_kW,data_valid,smoothing_enabled\n";
    csv_file << std::fixed << std::setprecision(3);

    // 安全约束
    const double P_chg_max = 100.0;
    const double P_dis_max = 100.0;
    bool charge_enabled = true;
    bool discharge_enabled = true;

    // 初始化SOC
    double SOC = SOC_init;

    for (int step = 0; step < total_steps; ++step) {
        double time = step * Ts;

        // 生成光伏功率
        double P_pv = gen_pv_sine(time, P_pv_base, P_pv_amp, wave_period);

        // 通信故障模拟
        bool data_valid = true;
        if (enable_comm_fault && time >= fault_start && time < fault_start + fault_duration) {
            data_valid = false;
        }

        // 功能开关模拟
        bool smoothing_enabled = true;
        if (enable_smoothing_toggle && time >= toggle_start && time < toggle_start + toggle_duration) {
            smoothing_enabled = false;
        }

        // 调用控制器
        double P_comp = controller.update(P_pv, SOC,
                                          P_chg_max, P_dis_max,
                                          charge_enabled, discharge_enabled,
                                          smoothing_enabled, data_valid);

        // 理想储能响应
        double P_bat = P_comp;
        double P_grid = P_pv + P_bat;

        // 动态更新SOC：SOC变化 = -P_comp * Ts / (36 * E_battery)
        SOC -= P_comp * Ts / (36.0 * E_battery);
        // 限制SOC在[0, 100]
        if (SOC > 100.0) SOC = 100.0;
        if (SOC < 0.0) SOC = 0.0;

        // 写入CSV
        csv_file << time << ","
                 << P_pv << ","
                 << controller.getSmoothValue() << ","
                 << P_comp << ","
                 << SOC << ","
                 << P_grid << ","
                 << (data_valid ? 1 : 0) << ","
                 << (smoothing_enabled ? 1 : 0) << "\n";
    }

    csv_file.close();
    std::cout << "场景 " << scenario_name << " 完成，输出 " << csv_filename << std::endl;
}

int main() {
    // 控制器配置
    SmoothingController::Config cfg;
    cfg.tau = 60.0;
    cfg.Ts = 0.1;
    cfg.SOC_low = 20.0;
    cfg.SOC_high = 80.0;
    cfg.SOC_min = 10.0;
    cfg.SOC_max = 90.0;

    SmoothingController controller(cfg);

    const double Ts = 0.1;
    const int steps_300s = 3000;   // 300秒

    // ---------- 场景S1：正常平滑，SOC动态变化（大容量电池，SOC变化极小） ----------
    runScenario("S1_normal", steps_300s, Ts,
                200.0, 50.0, 120.0,   // 光伏基准200，幅值50，周期120s
                50.0, 200.0,          // 初始SOC 50%，电池容量200kWh
                false, 0, 0,
                false, 0, 0,
                controller);

    // ---------- 场景S2：高SOC限充电（阶跃上升） ----------
{
    const int steps = steps_300s;
    const double E_battery = 10.0;      // 小容量加速SOC变化
    const double P_pv_before = 200.0;   // 阶跃前光伏
    const double P_pv_after = 300.0;    // 阶跃后光伏
    const double step_time = 10.0;      // 阶跃时刻
    const double SOC_init = 75.0;       // 初始SOC接近上限

    controller.reset();
    std::ofstream csv_file("smoothing_S2_highSOC.csv");
    csv_file << "Time_s,P_pv_kW,P_smooth_kW,P_comp_kW,SOC_pct,P_grid_kW,data_valid,smoothing_enabled\n";
    csv_file << std::fixed << std::setprecision(3);

    double SOC = SOC_init;
    double P_pv = P_pv_before;
    for (int step = 0; step < steps; ++step) {
        double time = step * Ts;
        if (time >= step_time) P_pv = P_pv_after;

        // 调用控制器
        double P_comp = controller.update(P_pv, SOC, 100.0, 100.0,
                                          true, true, true, true);
        double P_grid = P_pv + P_comp;

        // 动态更新SOC
        SOC -= P_comp * Ts / (36.0 * E_battery);
        if (SOC > 100.0) SOC = 100.0;
        if (SOC < 0.0) SOC = 0.0;

        csv_file << time << "," << P_pv << ","
                 << controller.getSmoothValue() << "," << P_comp << ","
                 << SOC << "," << P_grid << ",1,1\n";
    }
    csv_file.close();
}

// ---------- 场景S3：低SOC限放电（阶跃下降） ----------
{
    const int steps = steps_300s;
    const double E_battery = 10.0;
    const double P_pv_before = 200.0;
    const double P_pv_after = 100.0;
    const double step_time = 10.0;
    const double SOC_init = 25.0;

    controller.reset();
    std::ofstream csv_file("smoothing_S3_lowSOC.csv");
    csv_file << "Time_s,P_pv_kW,P_smooth_kW,P_comp_kW,SOC_pct,P_grid_kW,data_valid,smoothing_enabled\n";
    csv_file << std::fixed << std::setprecision(3);

    double SOC = SOC_init;
    double P_pv = P_pv_before;
    for (int step = 0; step < steps; ++step) {
        double time = step * Ts;
        if (time >= step_time) P_pv = P_pv_after;

        double P_comp = controller.update(P_pv, SOC, 100.0, 100.0,
                                          true, true, true, true);
        double P_grid = P_pv + P_comp;

        SOC -= P_comp * Ts / (36.0 * E_battery);
        if (SOC > 100.0) SOC = 100.0;
        if (SOC < 0.0) SOC = 0.0;

        csv_file << time << "," << P_pv << ","
                 << controller.getSmoothValue() << "," << P_comp << ","
                 << SOC << "," << P_grid << ",1,1\n";
    }
    csv_file.close();
}
    // ---------- 场景S4：通信中断（正常SOC，大容量） ----------
    runScenario("S4_commfault", steps_300s, Ts,
                200.0, 50.0, 120.0,
                50.0, 200.0,
                true, 100.0, 10.0,    // 100s处通信中断10s
                false, 0, 0,
                controller);

    // ---------- 场景S5：功能开关切换 ----------
    runScenario("S5_toggle", steps_300s, Ts,
                200.0, 50.0, 120.0,
                50.0, 200.0,
                false, 0, 0,
                true, 100.0, 10.0,    // 100s处关闭平抑10s
                controller);

    // ---------- 场景S6：剧烈波动 ----------
    runScenario("S6_rough", steps_300s, Ts,
                200.0, 120.0, 30.0,   // 大幅值、快波动
                50.0, 200.0,
                false, 0, 0,
                false, 0, 0,
                controller);

    std::cout << "all the scenarios have been completed!" << std::endl;
    return 0;
}