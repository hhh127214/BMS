// =====================================================================
// 07/ 单元测试 —— 实时库镜像写入器（RtdbMirror）
//
// 背景：`--device modbus` 只把结果写进实录 CSV，RT_DB 的 MEAS.* 没人写，
// 上端 IEC104 于是常年上送初值（0 / SOC 50% / 25℃）且品质 GOOD。
// 本组测的就是补上去的那一段。
//
//   T35: 写入内容 —— MEAS 七点 / STA 八点 / CFG 六点逐点等于入参
//   T36: **只读巡检不许写 CMD** —— cmd=nullptr 时 CMD 三点保持原值
//        （这是本组最要紧的一条：CMD.P_BAT=0 会被调度读成"要求 0 kW"）
//   T37: 闭环路径写 CMD 三点
//   T38: 品质 —— data_valid=false 时 MEAS 降为 UNCERTAIN，STA 仍是 GOOD
//        （"通信断了"本身就是真实值，不能因为通信断了把它标成不可信）
//   T39: 无来源的 CFG 点保持初值，不被凭空造值
//   T40: 统计计数
//
// 编译（见 07/scripts/build_test_rtdb.bat）：
//   需要链接 rt_db_api.o / ems_point_table.o / ems_rt_db_setup.o
// =====================================================================

#include "data_models.h"
#include "ems_point_table.h"
#include "ems_rt_db_setup.h"
#include "rt_db_api.h"
#include "rtdb_mirror.h"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

using namespace ems;

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT(cond)                                                      \
    do {                                                                  \
        if (cond) {                                                       \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #cond << std::endl;                     \
        }                                                                 \
    } while (0)

static bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

// ---- 测试装置：整份测试共用一个段，开场 reset 到点表初值 ----
static rt_db_handle_t g_h{};
static bool g_conn = false;

static bool init_env() {
    bool created = false;
    if (!ems_rt_db_setup(/*reset=*/true, &created)) return false;
    return rt_db_init(&g_h, nullptr);
}

static void reset_env() {
    bool created = false;
    ems_rt_db_setup(/*reset=*/true, &created);
}

static double get(std::size_t i) {
    double v = -12345.0;
    long   q = -1;
    if (!rt_db_get_value(&g_h, i, &v, &q, nullptr)) return -12345.0;
    return v;
}

static long get_quality(std::size_t i) {
    double v = 0.0;
    long   q = -1;
    if (!rt_db_get_value(&g_h, i, &v, &q, nullptr)) return -1;
    return q;
}

static bool set(std::size_t i, double v) {
    return rt_db_set_value(&g_h, i, v, rtdb_quality::kGood);
}

// =====================================================================
int main() {
    std::cout << "=========================================================\n"
              << " 07/ 实时库镜像写入器单元测试（T35 ~ T40）\n"
              << "=========================================================\n";

    if (!init_env()) {
        std::cerr << "[FAIL] 建不出共享内存段，无法继续\n";
        return 1;
    }
    g_conn = true;
    reset_env();

    RtdbMirror m;
    std::string err;
    if (!m.open(&err)) {
        std::cerr << "[FAIL] RtdbMirror::open 失败：" << err << "\n";
        return 1;
    }
    EXPECT(m.is_open());

    // ---- T36 前置：先让 CMD 处于"EMS 已经下发过"的状态 ----
    //   如果实现里在只读路径误写了 CMD=0，这一组会立刻抓到。
    set(EMS_CMD_P_BAT,   33.0);
    set(EMS_CMD_P_UPPER, 190.0);
    set(EMS_CMD_P_LOWER, 32.0);

    RtdbMirror::Sample s;
    s.p_load = 380.0;
    s.p_pv   = 150.0;
    s.p_bat  = 32.7;
    s.p_grid = 199.3;
    s.soc    = 0.545;
    s.t_c    = 26.5;
    s.soh    = 0.987;

    DeviceStatus st;            // 默认：通信全正常、数据有效
    DeviceLimits lim;
    lim.pcs_rated_chg_kw        = 200.0;
    lim.pcs_rated_dis_kw        = 200.0;
    lim.bms_chg_limit_kw        = 180.0;
    lim.bms_dis_limit_kw        = 175.0;
    lim.bms_chg_forbidden       = true;   // 禁充 = 1
    lim.bms_dis_forbidden       = false;  // 禁放 = 0
    lim.transformer_capacity_kw = 400.0;
    lim.d_target_kw             = 260.0;

    // -----------------------------------------------------------------
    std::cout << "\n[T35] 只读巡检：MEAS / STA / CFG 逐点写入\n";
    m.publish(s, nullptr, st, lim);

    EXPECT(near(get(EMS_P_LOAD), 380.0));
    EXPECT(near(get(EMS_P_PV),   150.0));
    EXPECT(near(get(EMS_P_BAT),  32.7));
    EXPECT(near(get(EMS_P_GRID), 199.3));
    EXPECT(near(get(EMS_SOC),    0.545));
    EXPECT(near(get(EMS_T_C),    26.5));
    EXPECT(near(get(EMS_SOH),    0.987));

    EXPECT(near(get(EMS_STA_BMS),   1.0));
    EXPECT(near(get(EMS_STA_PCS),   1.0));
    EXPECT(near(get(EMS_STA_METER), 1.0));
    EXPECT(near(get(EMS_STA_FAULT), 0.0));
    EXPECT(near(get(EMS_STA_OFFLINE), 0.0));
    EXPECT(near(get(EMS_STA_VALID),   1.0));
    EXPECT(near(get(EMS_STA_BMS_CHG_FORBID), 1.0));
    EXPECT(near(get(EMS_STA_BMS_DIS_FORBID), 0.0));

    EXPECT(near(get(EMS_CFG_MAX_CHG),     200.0));
    EXPECT(near(get(EMS_CFG_MAX_DIS),     200.0));
    EXPECT(near(get(EMS_CFG_BMS_CHG_LIM), 180.0));
    EXPECT(near(get(EMS_CFG_BMS_DIS_LIM), 175.0));
    EXPECT(near(get(EMS_CFG_TR_KVA),      400.0));
    EXPECT(near(get(EMS_CFG_D_TARGET),    260.0));

    // -----------------------------------------------------------------
    std::cout << "\n[T36] 只读巡检不许写 CMD（cmd=nullptr）\n";
    EXPECT(near(get(EMS_CMD_P_BAT),   33.0));
    EXPECT(near(get(EMS_CMD_P_UPPER), 190.0));
    EXPECT(near(get(EMS_CMD_P_LOWER), 32.0));

    // -----------------------------------------------------------------
    std::cout << "\n[T37] 闭环路径写 CMD 三点\n";
    RtdbMirror::Command c;
    c.p_bat   = 45.5;
    c.p_upper = 185.0;
    c.p_lower = 30.0;
    m.publish(s, &c, st, lim);
    EXPECT(near(get(EMS_CMD_P_BAT),   45.5));
    EXPECT(near(get(EMS_CMD_P_UPPER), 185.0));
    EXPECT(near(get(EMS_CMD_P_LOWER), 30.0));

    // -----------------------------------------------------------------
    std::cout << "\n[T38] 品质：数据无效时 MEAS 降 NT，STA 仍 GOOD\n";
    DeviceStatus bad;
    bad.data_valid   = false;
    bad.bms_comm_ok  = false;      // BMS 断链
    m.publish(s, nullptr, bad, lim);

    EXPECT(get_quality(EMS_SOC)     == rtdb_quality::kUncertain);
    EXPECT(get_quality(EMS_P_GRID)  == rtdb_quality::kUncertain);
    // "BMS 通信断了"是真实状态 —— 必须原样送出且标 GOOD，
    // 标成 NT 会让主站把"断链告警"本身也当成不可信。
    EXPECT(get_quality(EMS_STA_BMS) == rtdb_quality::kGood);
    EXPECT(near(get(EMS_STA_BMS), 0.0));
    EXPECT(near(get(EMS_STA_VALID), 0.0));

    // -----------------------------------------------------------------
    std::cout << "\n[T39] 无来源的 CFG 点保持初值（不凭空造值）\n";
    // 这七个点的来源不在 Modbus 点表里 —— 只能保留初始化器写的默认值。
    EXPECT(near(get(EMS_CFG_CAP_KWH), 1000.0));
    EXPECT(near(get(EMS_CFG_TAU_S),      0.0));
    EXPECT(near(get(EMS_CFG_RAMP_KW_S),  1e9));
    EXPECT(near(get(EMS_CFG_STANDBY),    2.0));
    EXPECT(near(get(EMS_CFG_ETA_CHG),    1.0));
    EXPECT(near(get(EMS_CFG_ETA_DIS),    1.0));
    EXPECT(near(get(EMS_CFG_SOC_MIN),    0.05));
    EXPECT(near(get(EMS_CFG_SOC_MAX),    0.95));

    // -----------------------------------------------------------------
    std::cout << "\n[T40] 统计计数\n";
    // 上面一共 publish 三次
    EXPECT(m.stats().steps  == 3);
    EXPECT(m.stats().failed == 0);
    EXPECT(m.stats().wrote  >  0);

    std::cout << "\n=========================================\n"
              << " PASS=" << g_pass << "  FAIL=" << g_fail << "\n"
              << " 镜像统计：steps=" << m.stats().steps
              << " wrote=" << m.stats().wrote
              << " failed=" << m.stats().failed << "\n"
              << "=========================================\n";

    if (g_conn) rt_db_cleanup(&g_h);
    return (g_fail == 0) ? 0 : 1;
}
