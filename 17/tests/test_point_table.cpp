// =====================================================================
// 17/tests/test_point_table.cpp —— 全点表 + 两层映射 + self_check（C4 / B2）
//
//   T01 内置全点表：规模 / 枚举下标 / 追加点区锚点
//   T02 点名唯一且非空 + 按名查索引（含"找不到返回 -1"）
//   T03 八元组字段齐备且合法（类型/设备/点区分布）
//   T04 两层映射：四种 kind 的条数与覆盖性
//   T05 self_check() == 0，且五类计数各自 == 0（**分别断言**，不只看总和）
//   T06 判据③ 单位一致：把来源点单位改坏 → unit_mismatch 红
//   T07 判据② 来源可解析：把来源点名删掉 → unresolved_sources 红
//   T08 判据⑤ 索引与下标一致：把映射表两条对调 → index_mismatch 红
//   T09 判据①④ 点名/量程：构造坏表 → 对应计数红
//   T10 （反向守卫 —— 已并入 T06~T09：每个判据用例都是"先确认同一份数据干净、
//        再改坏它、再看它是否红"，两步在同一条用例里，不单独占一个编号）
//   T11 抽象表口径（40 点 / EXT 起点 32）—— 防止有人顺手改 40 点表
//   T12 设备侧点区分布与 BMS 安全链点齐备
//
// 编译：见 17/scripts/build_test.bat
// =====================================================================

#include "expect.h"

#include "device_point_table.h"
#include "point_mapping.h"
#include "point_table_loader.h"

#include "ems_point_table.h"

#include <map>
#include <string>
#include <vector>

using namespace ems;
using namespace ems::devpt;

static int g_pass = 0;
static int g_fail = 0;

// =====================================================================
// T01 内置全点表规模与下标
// =====================================================================
static void test_01_builtin_shape() {
    std::printf("T01 内置全点表：规模 / 下标 / 追加点锚点\n");

    // 107 = BMS 38 + 电表 35 + PCS 34
    EXPECT_EQ(DP_POINT_COUNT, 107);
    // 追加点锚点必须 == 总点数（新点只能追加在 PCS 段之后）
    EXPECT_EQ(DP_APPEND_BEGIN, DP_POINT_COUNT);
    EXPECT_EQ(DP_APPEND_BEGIN, 107);

    // 段边界（枚举值）钉住：BMS 0..37 / 电表 38..72 / PCS 73..106
    EXPECT_EQ(DP_BMS_CLUSTER_U, 0);
    EXPECT_EQ(DP_METER_U_A, 38);
    EXPECT_EQ(DP_PCS_P_ACT, 73);
    EXPECT_EQ(DP_PCS_RATED_Q_KVAR, 106);

    // 下标 ↔ 点名（抽点）
    EXPECT_STR_EQ(dev_point_name(DP_BMS_SOC), "MEAS.BMS.SOC");
    EXPECT_STR_EQ(dev_point_name(DP_BMS_CHG_FORBID), "STA.BMS.CHG_FORBID");
    EXPECT_STR_EQ(dev_point_name(DP_METER_P_TOTAL), "MEAS.METER.P_TOTAL");
    EXPECT_STR_EQ(dev_point_name(DP_METER_EP_FWD), "MEAS.METER.EP_FWD");
    EXPECT_STR_EQ(dev_point_name(DP_METER_DEMAND_PEAK_TS), "MEAS.METER.DEMAND_PEAK_TS");
    EXPECT_STR_EQ(dev_point_name(DP_PCS_P_ACT), "MEAS.PCS.P_ACT");
    EXPECT_STR_EQ(dev_point_name(DP_PCS_CMD_P_SET), "CMD.PCS.P_SET");

    // 越界不崩，返回空串
    EXPECT_STR_EQ(dev_point_name(-1), "");
    EXPECT_STR_EQ(dev_point_name(DP_POINT_COUNT), "");
}

// =====================================================================
// T02 点名唯一 / 非空 / 按名查索引
// =====================================================================
static void test_02_names() {
    std::printf("T02 点名唯一且非空 + 按名查索引\n");

    std::map<std::string, int> seen;
    int dup = 0, empty = 0;
    for (int i = 0; i < DP_POINT_COUNT; ++i) {
        const std::string n = dev_point_name(i);
        if (n.empty()) ++empty;
        if (seen.count(n)) ++dup;
        seen[n] = i;
    }
    EXPECT_EQ(empty, 0);
    EXPECT_EQ(dup, 0);
    EXPECT_EQ(static_cast<int>(seen.size()), DP_POINT_COUNT);

    // 每个点都能反查到自己的下标（⑤ 的"下标一致"在设备侧的体现）
    int mismatch = 0;
    for (int i = 0; i < DP_POINT_COUNT; ++i) {
        if (dev_point_index_of(dev_point_name(i)) != i) ++mismatch;
    }
    EXPECT_EQ(mismatch, 0);

    EXPECT_EQ(dev_point_index_of("MEAS.BMS.SOC"), DP_BMS_SOC);
    EXPECT_EQ(dev_point_index_of("STA.BMS.DIS_FORBID"), DP_BMS_DIS_FORBID);
    EXPECT_EQ(dev_point_index_of("MEAS.METER.DEMAND_PEAK"), DP_METER_DEMAND_PEAK);
    EXPECT_EQ(dev_point_index_of("STA.PCS.MODE"), DP_PCS_MODE);

    // ★ 查不到必须返回 -1，**不能**退化成 0 号点（那是"采了个假值"的经典写法）
    EXPECT_EQ(dev_point_index_of("NO.SUCH.POINT"), -1);
    EXPECT_EQ(dev_point_index_of(""), -1);
    EXPECT_EQ(dev_point_index_of(nullptr), -1);
    // 大小写敏感（点名是协议契约，不容"宽容匹配"）
    EXPECT_EQ(dev_point_index_of("meas.bms.soc"), -1);
}

// =====================================================================
// T03 八元组字段齐备 + 分布
// =====================================================================
static void test_03_fields() {
    std::printf("T03 八元组字段齐备 / 设备与点区分布\n");

    int n_bms = 0, n_meter = 0, n_pcs = 0;
    int n_analog = 0, n_digital = 0;
    int z_meas = 0, z_sta = 0, z_cfg = 0, z_alm = 0, z_cmd = 0;
    int bad_type = 0, bad_dev = 0, bad_zone = 0, bad_range = 0, bad_def = 0;
    int empty_unit = 0;
    for (int i = 0; i < DP_POINT_COUNT; ++i) {
        const DevicePointDef& d = kBuiltinPoints[i];
        switch (d.device) {
            case DK_BMS:   ++n_bms;   break;
            case DK_METER: ++n_meter; break;
            case DK_PCS:   ++n_pcs;   break;
            default:       ++bad_dev; break;
        }
        if (d.type == PT_ANALOG)       ++n_analog;
        else if (d.type == PT_DIGITAL) ++n_digital;
        else                           ++bad_type;

        switch (d.zone) {
            case PZ_MEAS: ++z_meas; break;
            case PZ_STA:  ++z_sta;  break;
            case PZ_CFG:  ++z_cfg;  break;
            case PZ_ALM:  ++z_alm;  break;
            case PZ_CMD:  ++z_cmd;  break;
            default:      ++bad_zone; break;
        }
        if (!(d.range_min < d.range_max)) ++bad_range;
        if (d.def < d.range_min || d.def > d.range_max) ++bad_def;
        if (d.unit == nullptr || d.unit[0] == '\0') ++empty_unit;
    }
    EXPECT_EQ(n_bms, 38);
    EXPECT_EQ(n_meter, 35);
    EXPECT_EQ(n_pcs, 34);
    EXPECT_EQ(n_bms + n_meter + n_pcs, DP_POINT_COUNT);

    EXPECT_GT0(n_analog);
    EXPECT_GT0(n_digital);
    EXPECT_EQ(bad_type, 0);
    EXPECT_EQ(bad_dev, 0);
    EXPECT_EQ(bad_zone, 0);
    EXPECT_EQ(bad_range, 0);
    EXPECT_EQ(bad_def, 0);
    // 每个点都必须有单位（"-" 也算有 —— 空格才叫没有）
    EXPECT_EQ(empty_unit, 0);

    EXPECT_GT0(z_meas);
    EXPECT_GT0(z_sta);
    EXPECT_GT0(z_cfg);
    EXPECT_GT0(z_alm);
    EXPECT_GT0(z_cmd);
    EXPECT_EQ(z_meas + z_sta + z_cfg + z_alm + z_cmd, DP_POINT_COUNT);
}

// =====================================================================
// T04 两层映射的 kind 分布与覆盖性
// =====================================================================
static void test_04_mapping_shape() {
    std::printf("T04 两层映射：四种 kind 的条数 / 覆盖性\n");

    const DevicePointTable t = DevicePointTable::builtin();
    const SelfCheckReport rep = self_check_report(t);

    // 27 direct + 2 composed + 3 sink + 8 external == 40
    EXPECT_EQ(rep.direct_count,   27);
    EXPECT_EQ(rep.composed_count, 2);
    EXPECT_EQ(rep.sink_count,     3);
    EXPECT_EQ(rep.external_count, 8);
    EXPECT_EQ(rep.direct_count + rep.composed_count + rep.sink_count + rep.external_count,
              EMS_POINT_COUNT);

    // 映射表本身长度必须是 40（下标 == 抽象索引）
    EXPECT_EQ(EMS_POINT_COUNT, 40);
    EXPECT_EQ(rep.checked_ems_points, 40);

    // 抽点核对映射的落点（含 A2 的关键一条：关口功率取电表）
    EXPECT_STR_EQ(device_source_of(EMS_P_GRID), "MEAS.METER.P_TOTAL");
    EXPECT_STR_EQ(device_source_of(EMS_P_BAT),  "MEAS.PCS.P_ACT");
    EXPECT_STR_EQ(device_source_of(EMS_SOC),    "MEAS.BMS.SOC");
    EXPECT_STR_EQ(device_source_of(EMS_CFG_BMS_CHG_LIM), "STA.BMS.CHG_LIMIT_KW");
    EXPECT_STR_EQ(device_source_of(EMS_STA_BMS_DIS_FORBID), "STA.BMS.DIS_FORBID");
    // CMD 是下行（sink）——方向相反，必须仍是"有落点"的
    EXPECT_STR_EQ(device_source_of(EMS_CMD_P_BAT), "CMD.PCS.P_SET");
    // 合成式必须带上来源，而不是只写一个名字
    const std::string tsrc = device_source_of(EMS_T_C);
    EXPECT(tsrc.find("mean(T_MAX, T_MIN)") != std::string::npos);
    EXPECT(tsrc.find("MEAS.BMS.T_MAX") != std::string::npos);
    EXPECT(tsrc.find("MEAS.BMS.T_MIN") != std::string::npos);
    // 越界不能崩
    EXPECT_STR_EQ(device_source_of(-1), "<越界>");
    EXPECT_STR_EQ(device_source_of(EMS_POINT_COUNT), "<越界>");
}

// =====================================================================
// T05 self_check() == 0（逐类断言，不只看总和）
// =====================================================================
static void test_05_self_check_clean() {
    std::printf("T05 self_check() == 0（五类各自为 0）\n");

    const DevicePointTable t = DevicePointTable::builtin();
    const SelfCheckReport rep = self_check_report(t);

    EXPECT_EQ(rep.dup_or_empty_names, 0);
    EXPECT_EQ(rep.unresolved_sources, 0);
    EXPECT_EQ(rep.unit_mismatch, 0);
    EXPECT_EQ(rep.range_invalid, 0);
    EXPECT_EQ(rep.index_mismatch, 0);
    EXPECT_EQ(rep.total, 0);
    EXPECT_EQ(self_check(), 0);

    // 正向守卫：自检确实**跑到了**东西（不是"表是空的所以全过"）
    EXPECT_EQ(rep.checked_device_points, 107);
    EXPECT_EQ(rep.checked_ems_points, 40);

    // 运行时的表（加载器构建）也必须干净 —— 两条路径同一份判据
    EXPECT_EQ(self_check(DevicePointTable::builtin()), 0);
}

// =====================================================================
// T06 判据③：单位一致（改坏来源单位 → 红）
// =====================================================================
static void test_06_unit_mismatch() {
    std::printf("T06 判据③ 单位一致：改坏来源单位必须红\n");

    DevicePointTable t = DevicePointTable::builtin();
    const int i = t.index_of("MEAS.METER.P_TOTAL");
    EXPECT(i >= 0);
    EXPECT_STR_EQ(t.points[static_cast<std::size_t>(i)].unit, "kW");

    t.points[static_cast<std::size_t>(i)].unit = "MW";      // 故意改坏
    const SelfCheckReport rep = self_check_report(t);
    EXPECT_GT0(rep.unit_mismatch);
    EXPECT_GT0(rep.total);

    // 反向守卫：改回来就干净（证明上一条红的是"单位"，不是别的原因）
    t.points[static_cast<std::size_t>(i)].unit = "kW";
    EXPECT_EQ(self_check_report(t).total, 0);
}

// =====================================================================
// T07 判据②：来源可解析（删掉来源点 → 红）
// =====================================================================
static void test_07_unresolved_source() {
    std::printf("T07 判据② 来源可解析：删掉来源点必须红\n");

    DevicePointTable t = DevicePointTable::builtin();
    const int i = t.index_of("MEAS.BMS.SOH");
    EXPECT(i >= 0);
    t.points.erase(t.points.begin() + i);          // 把 MEAS.SOH 的来源删掉
    const SelfCheckReport rep = self_check_report(t);
    EXPECT_GT0(rep.unresolved_sources);
    EXPECT_GT0(rep.total);
}

// =====================================================================
// T08 判据⑤：映射表下标 == 抽象索引（对调两条 → 红）
// =====================================================================
static void test_08_index_mismatch() {
    std::printf("T08 判据⑤ 索引与下标一致：对调映射表两条必须红\n");

    std::vector<EmsToDeviceMap> bad;
    for (int i = 0; i < EMS_POINT_COUNT; ++i) bad.push_back(kEmsToDeviceMap[i]);
    // 对调第 4、5 条（EMS_SOC / EMS_T_C）
    std::swap(bad[4], bad[5]);
    const DevicePointTable t = DevicePointTable::builtin();
    const SelfCheckReport rep = self_check_report(t, bad.data());
    EXPECT_GT0(rep.index_mismatch);
    EXPECT_GT0(rep.total);

    // 反向守卫：原表干净
    EXPECT_EQ(self_check_report(t, kEmsToDeviceMap).index_mismatch, 0);
}

// =====================================================================
// T09 判据①④：点名重复 / 量程矛盾（坏表 → 红）
// =====================================================================
static void test_09_bad_table() {
    std::printf("T09 判据①④ 点名重复 / 量程矛盾必须红\n");

    {   // ① 点名重复
        DevicePointTable t = DevicePointTable::builtin();
        t.points[5].name = t.points[4].name;
        const LoadError e = t.validate();
        EXPECT(!e.ok);
        EXPECT_EQ(e.point_index, 5);                 // 指出**第几个点**
        EXPECT_STR_EQ(e.field, "name");              // 指出**哪个字段**
        EXPECT_GT0(self_check_report(t).dup_or_empty_names);
    }
    {   // ① 点名空
        DevicePointTable t = DevicePointTable::builtin();
        t.points[7].name.clear();
        const LoadError e = t.validate();
        EXPECT(!e.ok);
        EXPECT_EQ(e.point_index, 7);
        EXPECT_STR_EQ(e.field, "name");
    }
    {   // ④ 量程矛盾
        DevicePointTable t = DevicePointTable::builtin();
        t.points[3].range_min = 5.0;
        t.points[3].range_max = 1.0;                 // min > max
        const SelfCheckReport rep = self_check_report(t);
        EXPECT_GT0(rep.range_invalid);
    }
    {   // ④ 默认值越量程
        DevicePointTable t = DevicePointTable::builtin();
        t.points[11].def = 500.0;                    // SOC 默认 500% > 量程上限 100
        const SelfCheckReport rep = self_check_report(t);
        EXPECT_GT0(rep.range_invalid);
        const LoadError e = t.validate();
        EXPECT_STR_EQ(e.field, "default");
    }
}

// =====================================================================
// T11 抽象表口径（防止顺手改 40 点表）
// =====================================================================
static void test_11_abstract_contract() {
    std::printf("T11 抽象表口径：40 点 / EXT 起点 32\n");

    EXPECT_EQ(EMS_POINT_COUNT, 40);
    EXPECT_EQ(EMS_EXT_BEGIN, 32);
    EXPECT_EQ(EMS_EXT_END, EMS_POINT_COUNT);
    EXPECT_STR_EQ(EMS_POINT_NAMES[EMS_P_GRID], "MEAS.P_GRID");
    // ★ 两层的命名**故意不同**：抽象表用 `STA.BMS_DIS_FORBID`（下划线分隔组），
    //   全点表用 `STA.BMS.DIS_FORBID`（点号分段）。这正是"必须有映射表"的
    //   现实原因 —— 名字对不上时，靠人眼核对 40×107 的组合一定会错。
    EXPECT_STR_EQ(EMS_POINT_NAMES[EMS_STA_BMS_DIS_FORBID], "STA.BMS_DIS_FORBID");
    EXPECT_STR_EQ(dev_point_name(DP_BMS_DIS_FORBID), "STA.BMS.DIS_FORBID");
    EXPECT_STR_EQ(EMS_POINT_UNITS[EMS_SOC], "%");
    EXPECT_STR_EQ(EMS_POINT_UNITS[EMS_CFG_SOC_MIN], "-");
    // 抽象表的点名也必须唯一非空（self_check ① 读侧校验的对象）
    std::map<std::string, int> seen;
    int dup = 0, empty = 0;
    for (int i = 0; i < EMS_POINT_COUNT; ++i) {
        const std::string n = EMS_POINT_NAMES[i] ? EMS_POINT_NAMES[i] : "";
        if (n.empty()) ++empty;
        if (seen.count(n)) ++dup;
        seen[n] = i;
    }
    EXPECT_EQ(empty, 0);
    EXPECT_EQ(dup, 0);
}

// =====================================================================
// T12 BMS 安全链点齐备
// =====================================================================
static void test_12_safety_points() {
    std::printf("T12 BMS 安全链点齐备\n");

    // §10.2 B2 点名要求 BMS 侧必须有"允许充放/禁充放" —— 逐条查
    const char* must_have[] = {
        "STA.BMS.CHG_FORBID", "STA.BMS.DIS_FORBID",
        "STA.BMS.CHG_LIMIT_KW", "STA.BMS.DIS_LIMIT_KW",
        "MEAS.BMS.INSUL_R", "MEAS.BMS.LEAK_I",
        "MEAS.BMS.CELL_V_MAX", "MEAS.BMS.CELL_V_MAX_IDX",
        "MEAS.BMS.CELL_V_MIN", "MEAS.BMS.CELL_V_MIN_IDX",
        "MEAS.BMS.T_MAX", "MEAS.BMS.T_MIN", "MEAS.BMS.T_MAX_IDX",
        "ALM.BMS.LEVEL", "ALM.BMS.CODE",
        "MEAS.METER.EP_FWD", "MEAS.METER.EP_REV",
        "MEAS.METER.DEMAND_NOW", "MEAS.METER.DEMAND_PEAK",
        "MEAS.METER.DEMAND_PEAK_TS",
        "MEAS.PCS.Q_ACT", "STA.PCS.MODE",
        "MEAS.PCS.UDC", "MEAS.PCS.IDC",
        "ALM.PCS.FAULT_CODE", "MEAS.PCS.E_DAY_KWH", "MEAS.PCS.E_TOTAL_KWH"
    };
    int missing = 0;
    for (const char* n : must_have) {
        if (dev_point_index_of(n) < 0) {
            ++missing;
            std::printf("    缺失: %s\n", n);
        }
    }
    EXPECT_EQ(missing, 0);

    // 两个禁位必须是**数字量**（0/1 位，不是模拟量）—— 它们经 DI 走 Modbus
    EXPECT_EQ(kBuiltinPoints[DP_BMS_CHG_FORBID].type, PT_DIGITAL);
    EXPECT_EQ(kBuiltinPoints[DP_BMS_DIS_FORBID].type, PT_DIGITAL);
    // 禁充放默认必须是 0（允许）—— 与 A1 的纪律一致：
    //   默认 1 会让"设备侧尚未上线"直接锁死 [0,0]（冷启动不可用）
    EXPECT_NEAR(kBuiltinPoints[DP_BMS_CHG_FORBID].def, 0.0, 1e-12);
    EXPECT_NEAR(kBuiltinPoints[DP_BMS_DIS_FORBID].def, 0.0, 1e-12);
}

int main() {
    TEST_BANNER("17/ 设备全点表 + 两层映射 + self_check（C4/B2）");

    test_01_builtin_shape();
    test_02_names();
    test_03_fields();
    test_04_mapping_shape();
    test_05_self_check_clean();
    test_06_unit_mismatch();
    test_07_unresolved_source();
    test_08_index_mismatch();
    test_09_bad_table();
    test_11_abstract_contract();
    test_12_safety_points();

    TEST_TAIL();
    return g_fail == 0 ? 0 : 1;
}
