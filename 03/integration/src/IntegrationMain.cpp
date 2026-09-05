#include <iostream>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

// 共享 clamper 工具经 -I 03/shared 解析
#include "clamper.h"
// 各控制器头文件经 -I ../<控制器>/src 解析
#include "AntiReverseController.h"
#include "DemandController.h"
#include "SmoothingController.h"

enum MergeMode { MODE_SMOOTH, MODE_CHARGE, MODE_DISCHARGE };

/**
 * @brief 策略合并器（覆盖逻辑，不叠加）
 */
double mergeOutputsWithHysteresis(double P_rev, double P_dem, double P_smooth,
                                  double P_chg_max, double P_dis_max,
                                  bool charge_enabled, bool discharge_enabled,
                                  MergeMode& mode, int& switch_cnt) {
    const double rev_deadband = 0.5;
    const double dem_threshold = 10.0;
    const int    switch_delay = 3;

    // 防逆流绝对优先
    if (P_rev > rev_deadband) {
        mode = MODE_CHARGE;
        switch_cnt = 0;
        double P_cmd = -P_rev;
        if (!charge_enabled) return 0.0;
        return bms::clamp(P_cmd, -P_chg_max, 0.0);
    }

    MergeMode desired_mode;
    if (P_dem > dem_threshold) {
        desired_mode = MODE_DISCHARGE;
    } else {
        desired_mode = MODE_SMOOTH;
    }

    if (desired_mode != mode) {
        switch_cnt++;
        if (switch_cnt >= switch_delay) {
            mode = desired_mode;
            switch_cnt = 0;
        }
    } else {
        switch_cnt = 0;
    }

    if (mode == MODE_DISCHARGE) {
        double P_cmd = P_dem;
        if (!discharge_enabled) return 0.0;
        return bms::clamp(P_cmd, 0.0, P_dis_max);
    } else {
        double P_cmd = P_smooth;
        if (P_cmd < 0.0) {
            if (!charge_enabled) return 0.0;
            return bms::clamp(P_cmd, -P_chg_max, 0.0);
        } else {
            if (!discharge_enabled) return 0.0;
            return bms::clamp(P_cmd, 0.0, P_dis_max);
        }
    }
}

int main() {
    // 防逆流参数：降低增益，增大死区
    AntiReverseConfig rev_cfg;
    rev_cfg.Kp = 0.8;
    rev_cfg.Ki = 0.08;
    rev_cfg.Kff = 1.0;
    rev_cfg.Ts = 0.1;
    rev_cfg.deadband_enter = 1.0;   // 增大死区
    rev_cfg.deadband_exit = 2.0;
    rev_cfg.P_grid_min = 0.0;
    rev_cfg.integral_max = 60000.0;
    rev_cfg.integral_min = -60000.0;
    AntiReverseController rev_ctrl(rev_cfg);

    // 需量参数：保持足够增益
    DemandController::Config dem_cfg;
    dem_cfg.T_window = 10.0;
    dem_cfg.D_target = 250.0;
    dem_cfg.dP_max = 50.0;
    dem_cfg.Kp_avg = 2.0;
    dem_cfg.Ki = 0.5;
    dem_cfg.integral_max = 5000.0;
    dem_cfg.Ts = 0.1;
    DemandController dem_ctrl(dem_cfg);

    // 平抑控制器
    SmoothingController::Config sm_cfg;
    sm_cfg.tau = 60.0;
    sm_cfg.Ts = 0.1;
    sm_cfg.SOC_low = 20.0;
    sm_cfg.SOC_high = 80.0;
    sm_cfg.SOC_min = 10.0;
    sm_cfg.SOC_max = 90.0;
    SmoothingController sm_ctrl(sm_cfg);

    const double Ts = 0.1;
    const int total_steps = 3000;
    const double P_chg_max = 150.0;
    const double P_dis_max = 150.0;
    const bool charge_enabled = true;
    const bool discharge_enabled = true;
    const bool smoothing_enabled = true;
    const bool data_valid = true;
    const bool comm_fault = false;
    const double max_change_per_step = 10.0;

    double SOC = 50.0;
    const double battery_capacity_kWh = 1000.0;
    double P_bat_prev = 0.0;

    MergeMode current_mode = MODE_SMOOTH;
    int switch_counter = 0;

    // 防逆流输出一阶滤波
    double rev_filtered = 0.0;
    const double alpha_rev = 0.3;  // 滤波系数，越小越平滑

    std::ofstream csv("data/integration_sim.csv");
    if (!csv.is_open()) {
        std::cerr << "无法打开输出文件 data/integration_sim.csv！" << std::endl;
        std::cerr << "提示：请在模块根目录执行 scripts\\run.bat。" << std::endl;
        return 1;
    }
    csv << "Time_s,P_load_kW,P_pv_kW,P_grid_kW,P_bat_kW,SOC_pct,Rev_kW,Dem_kW,Sm_kW,Mode\n";
    csv << std::fixed << std::setprecision(3);

    for (int step = 0; step < total_steps; ++step) {
        double time = step * Ts;

        double P_load, P_pv;
        if (time < 50.0) {
            P_load = 200.0;
            P_pv = 150.0;
        } else if (time < 100.0) {
            P_load = 200.0;
            P_pv = 300.0;
        } else if (time < 200.0) {
            P_load = 450.0;
            P_pv = 150.0;
        } else {
            P_load = 200.0;
            P_pv = 150.0;
        }

        double P_grid = P_load - P_pv - P_bat_prev;

        double P_rev_raw = rev_ctrl.update(P_grid, P_pv, P_chg_max, charge_enabled);
        // 对防逆流输出进行一阶低通滤波，平滑指令
        rev_filtered = alpha_rev * P_rev_raw + (1.0 - alpha_rev) * rev_filtered;
        double P_rev = rev_filtered;

        double P_smooth = sm_ctrl.update(P_pv, SOC,
                                         P_chg_max, P_dis_max,
                                         charge_enabled, discharge_enabled,
                                         smoothing_enabled, data_valid);
        double P_dem = dem_ctrl.update(P_grid, P_dis_max, discharge_enabled, comm_fault);

        double P_bat_cmd = mergeOutputsWithHysteresis(P_rev, P_dem, P_smooth,
                                                      P_chg_max, P_dis_max,
                                                      charge_enabled, discharge_enabled,
                                                      current_mode, switch_counter);

        if (P_bat_cmd > P_bat_prev + max_change_per_step) {
            P_bat_cmd = P_bat_prev + max_change_per_step;
        } else if (P_bat_cmd < P_bat_prev - max_change_per_step) {
            P_bat_cmd = P_bat_prev - max_change_per_step;
        }
        P_bat_prev = P_bat_cmd;

        double energy_change_kWh = P_bat_cmd * Ts / 3600.0;
        SOC -= energy_change_kWh / battery_capacity_kWh * 100.0;
        SOC = std::max(10.0, std::min(90.0, SOC));

        csv << time << ","
            << P_load << ","
            << P_pv << ","
            << P_grid << ","
            << P_bat_prev << ","
            << SOC << ","
            << P_rev << ","
            << P_dem << ","
            << P_smooth << ","
            << static_cast<int>(current_mode) << "\n";
    }

    csv.close();
    std::cout << "集成仿真完成，数据已写入 integration_sim.csv" << std::endl;
    return 0;
}