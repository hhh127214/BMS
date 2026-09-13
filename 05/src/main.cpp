// =====================================================================
// 05/ — 周期 5：统一安全约束引擎 演示
//
//   场景 A：9 类安全约束逐条触发对照表
//     BMS 禁充 / BMS 禁放 / BMS 全闭 / BMS 降功率 / SOC 上下限 /
//     电池温度 / PCS 限功率 / 变压器容量 / 并网（倒送与需量）/
//     功率变化率 / 区间矛盾
//
//   目标（设计文档 §7 周期 5）：实现所有策略指令无法突破设备及系统安全边界。
//
// 编译：g++ -std=c++17 -Wall -O2 -I src -I ../04/src src/main.cpp -o build/safety_demo.exe
// =====================================================================

#include "data_models.h"
#include "strategy_base.h"
#include "safety_engine.h"

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
// 场景 A（周期 5）：安全约束引擎逐约束对照表
// =====================================================================
struct SafetyCase {
    std::string title;
    RealtimeSnapshot rt;
    DeviceLimits dev;
    GridQuality gq;
    SafetyParams params;
    double p_last = 0.0;
};

static SafetyCase base_case(const std::string& title) {
    SafetyCase c;
    c.title = title;
    // 基线：PCS 200/200，BMS 200/200，变压器 400，负荷 200，光伏 100
    c.dev.pcs_rated_chg_kw = 200.0;
    c.dev.pcs_rated_dis_kw = 200.0;
    c.dev.bms_chg_limit_kw = 200.0;
    c.dev.bms_dis_limit_kw = 200.0;
    c.dev.transformer_capacity_kw = 400.0;
    c.dev.d_target_kw = 250.0;
    c.rt.timestamp = 0.0;
    c.rt.p_load_kw = 200.0;
    c.rt.p_pv_kw   = 100.0;
    c.rt.p_grid_kw = 100.0;
    c.rt.soc = 0.50;
    c.rt.temperature_c = 25.0;
    c.rt.meters_alive["BMS"] = true;
    c.rt.meters_alive["METER"] = true;
    c.rt.meters_alive["PCS"] = true;
    c.params.ramp_kw_per_s = 200.0;
    c.params.enable_ramp = false;    // 基线关掉变化率，单看各条约束的独立效果
    c.params.grid_p_min_kw = -1e9;   // 基线放开并网限值，单看设备侧约束
    return c;
}

static void scenario5_safety() {
    banner("场景 A（周期 5）：统一安全约束引擎 —— 9 类约束逐条触发对照");

    std::vector<SafetyCase> cases;

    cases.push_back(base_case("① 基线（全部正常）"));

    {
        SafetyCase c = base_case("② BMS 禁止充电");
        c.dev.bms_chg_forbidden = true;
        cases.push_back(c);
    }
    {
        SafetyCase c = base_case("③ BMS 禁止放电");
        c.dev.bms_dis_forbidden = true;
        cases.push_back(c);
    }
    {
        SafetyCase c = base_case("④ BMS 禁止充放（L0 全闭）");
        c.dev.bms_chg_forbidden = true;
        c.dev.bms_dis_forbidden = true;
        cases.push_back(c);
    }
    {
        SafetyCase c = base_case("⑤ BMS 请求降功率（放 120）");
        c.dev.bms_dis_limit_kw = 120.0;
        cases.push_back(c);
    }
    {
        SafetyCase c = base_case("⑥ SOC 触及下限（0.08）");
        c.rt.soc = 0.08;
        cases.push_back(c);
    }
    {
        SafetyCase c = base_case("⑦ SOC 触及上限（0.93）");
        c.rt.soc = 0.93;
        cases.push_back(c);
    }
    {
        SafetyCase c = base_case("⑧ SOC 预警区（0.86）");
        c.rt.soc = 0.86;
        cases.push_back(c);
    }
    {
        SafetyCase c = base_case("⑨ 电池温度预警（47℃）");
        c.rt.temperature_c = 47.0;
        cases.push_back(c);
    }
    {
        SafetyCase c = base_case("⑩ 电池温度故障（58℃）");
        c.rt.temperature_c = 58.0;
        cases.push_back(c);
    }
    {
        SafetyCase c = base_case("⑪ PCS 降额（0.6）");
        c.params.pcs_derate_ratio = 0.6;
        cases.push_back(c);
    }
    {
        SafetyCase c = base_case("⑫ 变压器轻度过载");
        c.rt.p_grid_kw = 400.0;   // ratio = (400 + 200*0.1)/400 = 1.05
        cases.push_back(c);
    }
    {
        SafetyCase c = base_case("⑬ 变压器极端过载");
        c.rt.p_grid_kw = 480.0;   // ratio = (480 + 20)/400 = 1.25
        cases.push_back(c);
    }
    {
        SafetyCase c = base_case("⑭ 并网不允许倒送");
        c.params.grid_p_min_kw = 0.0;
        c.rt.p_pv_kw = 400.0;     // 无储能时 base = 200-400 = -200（要倒送）
        c.rt.p_grid_kw = -200.0;
        cases.push_back(c);
    }
    {
        SafetyCase c = base_case("⑮ 并网进线容量上限 150");
        c.params.grid_p_max_kw = 150.0;
        c.rt.p_load_kw = 400.0;
        c.rt.p_grid_kw = 300.0;
        cases.push_back(c);
    }
    {
        SafetyCase c = base_case("⑯ 电网频率越限（51.0Hz）");
        c.gq.freq_hz = 51.0;
        cases.push_back(c);
    }
    {
        SafetyCase c = base_case("⑰ 电网电压越限（1.15pu）");
        c.gq.volt_pu = 1.15;
        cases.push_back(c);
    }
    {
        SafetyCase c = base_case("⑱ 功率变化率（上拍 +100，限 200kW/s）");
        c.params.enable_ramp = true;
        c.params.ramp_kw_per_s = 200.0;
        c.p_last = 100.0;
        cases.push_back(c);
    }
    {
        SafetyCase c = base_case("⑲ BMS 通信丢失");
        c.rt.meters_alive["BMS"] = false;
        cases.push_back(c);
    }

    std::printf("\n%-30s %-10s %-6s %22s  %-26s %s\n",
                "触发条件", "动作", "层级", "收敛区间 [下界, 上界]", "区间主约束", "reason");
    std::printf("%s\n", std::string(152, '-').c_str());

    int n_derate = 0, n_l0 = 0, n_emerg = 0, n_contra = 0;
    for (const auto& c : cases) {
        SafetyEngine eng(c.params);          // 每个用例一个干净引擎（避免滞环锁存串场）
        SafetyVerdict v = eng.evaluate(c.rt, c.dev, c.gq, c.p_last, 0.1);
        if (v.derated)       ++n_derate;
        if (v.l0_active)     ++n_l0;
        if (v.emergency)     ++n_emerg;
        if (v.contradiction) ++n_contra;

        char lo[32], hi[32], iv[64];
        if (v.p_lower <= -1e17) std::snprintf(lo, sizeof(lo), "-inf");
        else                    std::snprintf(lo, sizeof(lo), "%.1f", v.p_lower);
        if (v.p_upper >= 1e17)  std::snprintf(hi, sizeof(hi), "+inf");
        else                    std::snprintf(hi, sizeof(hi), "%.1f", v.p_upper);
        std::snprintf(iv, sizeof(iv), "[%s, %s]", lo, hi);

        const char* action = "正常";
        if (v.emergency)          action = "紧急停机";
        else if (v.contradiction) action = "区间矛盾";
        else if (v.l0_hard)       action = "L0 禁闭";
        else if (v.derated)       action = "降功率";
        else if (v.l1_active)     action = "L1 限界";

        std::string lvl = "-";
        if (v.l0_active)    lvl = "L0";
        else if (v.l1_active) lvl = "L1";

        std::printf("%-30s %-10s %-6s %22s  %-26s %s\n",
                    c.title.c_str(), action, lvl.c_str(), iv,
                    v.trace().c_str(), v.reason.c_str());
    }

    std::printf("\n统计：%zu 个用例 → L0 禁闭 %d 个、L1 降额 %d 个、紧急停机 %d 个、区间矛盾 %d 个\n",
                cases.size(), n_l0, n_derate, n_emerg, n_contra);

    // 逐条约束的归属说明
    sub("9 类约束 → 收敛语义映射");
    std::printf("%-22s %-4s %s\n", "约束", "层级", "区间语义");
    std::printf("%-22s %-4s %s\n", "bms_forbid", "L0", "禁充 → p_lower=0；禁放 → p_upper=0；通信丢失 → [0,0]");
    std::printf("%-22s %-4s %s\n", "soc_limit", "L0", "触及下限禁放 / 触及上限禁充 / 预警区折减 50%");
    std::printf("%-22s %-4s %s\n", "battery_temp", "L0", "≥55℃ 禁充放（紧急）；≥45℃ 折减 50%");
    std::printf("%-22s %-4s %s\n", "bms_derate", "L1", "min(BMS 上送限值, PCS 额定)");
    std::printf("%-22s %-4s %s\n", "pcs_limit", "L1", "PCS 额定充放幅度 × 折减系数");
    std::printf("%-22s %-4s %s\n", "transformer_limit", "L1", "过载 → P_bat 可行带 [base−half, base+half]；与设备区间无交则饱和投影（尽力缓解）");
    std::printf("%-22s %-4s %s\n", "grid_connect", "L1", "P_grid ≥ g_min → p_upper ≤ base−g_min；P_grid ≤ g_max → p_lower ≥ base−g_max");
    std::printf("%-22s %-4s %s\n", "grid_quality", "L1", "频率/电压越限 → [0,0]（并网合规）");
    std::printf("%-22s %-4s %s\n", "ramp_rate", "L1", "|p_cmd − p_last| ≤ ramp_kw_per_s × dt");
}


// =====================================================================
int main() {
    std::printf("============================================================\n");
    std::printf("  工商业储能 EMS —— 05/ 周期 5 安全约束引擎演示\n");
    std::printf("============================================================\n");

    scenario5_safety();

    banner("周期 5 交付小结");
    std::printf("  9 类约束 → 统一折叠成一个 (p_lower, p_upper) 区间，逐条 trace 可定位\n");
    std::printf("  变化率是**后置限速器**而非区间约束（不参与求交，故无假矛盾）\n");
    std::printf("  区间矛盾 ≠ 紧急：映射为 DERATED（可自恢复），绝不锁存 EMERGENCY\n");
    std::printf("\n");
    return 0;
}
