// =====================================================================
// 17/ — 两层点表之间的**代码化映射表**（C4）
//
// 依据：docs/规划/模拟器与设备接入梳理.md §5.3
//
//   > 两层之间必须有一个显式的映射表，否则"某个单体过温"这种信息
//   > 永远进不到 EMS。
//
// ★ 为什么映射表必须是**代码**而不是文档里的一张表：
//   文档不会在"有人改了 40 点表的枚举顺序"时变红。映射表如果是代码，
//   self_check() 就能把"抽象表第 i 个点在设备侧找不到来源"变成一条**会红的断言**。
//   本项目已经吃过三次"写了但某条路径上不生效"的亏（§10.4 D4），
//   而"映射表只存在于文档里"正是这一类缺口的温床。
//
// ---------------------------------------------------------------------
// 四种映射关系（为什么不是"一对一"就够）：
//
//   DIRECT    抽象点 = 某个设备点的原值（27 个）—— 绝大多数
//   COMPOSED  抽象点由多个设备点**合成**（2 个）
//               · MEAS.T_C    = mean(T_MAX, T_MIN)   —— 抽象表只要一个温度
//               · STA.OFFLINE = 任一设备通信断        —— 是"三路通信位"的逻辑或非
//             这正是 §5.3 说的"某个单体过温要能进到 EMS"的那条路：
//             设备侧只有极值点，抽象侧要的是聚合量。
//   SINK      抽象点是 **EMS → 设备** 的下行量（3 个 CMD.*）
//             它不是"来源"，而是"落点"：抽象表的 CMD.P_BAT 最终写到
//             全点表的 CMD.PCS.P_SET。方向相反，因此单列一类 ——
//             否则 self_check 会把它误判成"找不到来源"。
//   EXTERNAL  来自调度（IEC104），**不在设备侧的任何点表里**（8 个 EXT.*）
//             显式标注为 EXTERNAL + 必须写明 reason。把它藏进 DIRECT 或
//             干脆不登记，都会让"抽象表 40 点是否都有交代"这条判据失去意义。
//
// 编译：纯头文件（inline）。需要 -I 到 07/src/rtdb（读 40 点表的点名/单位）。
// =====================================================================

#pragma once

#include "device_point_table.h"
#include "point_table_loader.h"

#include "ems_point_table.h"   // 40 点抽象表（只读引用，不改）

#include <cstdio>
#include <string>
#include <vector>

namespace ems {
namespace devpt {

// =====================================================================
// 映射关系
// =====================================================================
enum MapKind {
    MK_DIRECT   = 0,
    MK_COMPOSED = 1,
    MK_SINK     = 2,
    MK_EXTERNAL = 3
};

inline const char* map_kind_name(int k) {
    switch (k) {
        case MK_DIRECT:   return "direct";
        case MK_COMPOSED: return "composed";
        case MK_SINK:     return "sink";
        case MK_EXTERNAL: return "external";
        default:          return "invalid";
    }
}

// 一条映射
struct EmsToDeviceMap {
    int         ems_index;     // 抽象表索引（== 数组下标，self_check ⑤ 守着）
    int         kind;
    const char* sources[4];    // DIRECT/SINK 用 1 个；COMPOSED 用 >=2 个
    const char* formula;       // COMPOSED 的合成式（人可读，非空）
    const char* out_unit;      // COMPOSED 的输出单位；DIRECT/SINK 取来源点单位
    const char* note;          // EXTERNAL 的 reason 必须写这里
};

// ---------------------------------------------------------------------
// 映射表：**下标 == 抽象表索引**
//
// ★ 顺序必须与 07/src/rtdb/ems_point_table.h 的枚举**逐条对齐**。
//   self_check ⑤ 会用 `m.ems_index == i` 把这条钉住 ——
//   所以这里每一条都显式写 ems_index，而不是靠"我数对了"。
// ---------------------------------------------------------------------
inline const EmsToDeviceMap kEmsToDeviceMap[EMS_POINT_COUNT] = {
    // ---------------- 量测（设备 → EMS） ----------------
    {EMS_P_LOAD, MK_DIRECT, {"MEAS.METER.P_LOAD"}, nullptr, nullptr,
     "负荷由关口电表的分路计量给出（现场唯一权威计量点）"},
    {EMS_P_PV, MK_DIRECT, {"MEAS.METER.P_PV"}, nullptr, nullptr,
     "光伏同样走电表分路 —— 不用 '光伏逆变器自己报' 的口径，保证与关口同源"},
    {EMS_P_BAT, MK_DIRECT, {"MEAS.PCS.P_ACT"}, nullptr, nullptr,
     "电池实际功率取 PCS 交流侧有功（设备侧执行结果，不是 EMS 的期望）"},
    {EMS_P_GRID, MK_DIRECT, {"MEAS.METER.P_TOTAL"}, nullptr, nullptr,
     "★ A2 修复后的口径：关口功率 = 电表读数，**不是** load-pv-bat 的推算"},
    {EMS_SOC, MK_DIRECT, {"MEAS.BMS.SOC"}, nullptr, nullptr, ""},
    {EMS_T_C, MK_COMPOSED, {"MEAS.BMS.T_MAX", "MEAS.BMS.T_MIN"},
     "mean(T_MAX, T_MIN)", "degC",
     "抽象表只要一个电池温度；设备侧的权威量是最高/最低温度对 → 取均值"},
    {EMS_SOH, MK_DIRECT, {"MEAS.BMS.SOH"}, nullptr, nullptr, ""},

    // ---------------- 指令（EMS → 设备） ----------------
    {EMS_CMD_P_BAT, MK_SINK, {"CMD.PCS.P_SET"}, nullptr, nullptr,
     "EMS 的有功设定最终写到 PCS 的 P_SET"},
    {EMS_CMD_P_UPPER, MK_SINK, {"CMD.PCS.P_UPPER"}, nullptr, nullptr,
     "EMS 自声明的允许上界 → PCS 的限值对（部分网关支持限值对下发）"},
    {EMS_CMD_P_LOWER, MK_SINK, {"CMD.PCS.P_LOWER"}, nullptr, nullptr, ""},

    // ---------------- 配置（静态） ----------------
    {EMS_CFG_CAP_KWH, MK_DIRECT, {"CFG.BMS.CAP_KWH"}, nullptr, nullptr, ""},
    {EMS_CFG_MAX_CHG, MK_DIRECT, {"STA.PCS.MAX_CHG_KW"}, nullptr, nullptr,
     "PCS 额定充电幅度取自 PCS 自己的能力点"},
    {EMS_CFG_MAX_DIS, MK_DIRECT, {"STA.PCS.MAX_DIS_KW"}, nullptr, nullptr, ""},
    {EMS_CFG_BMS_CHG_LIM, MK_DIRECT, {"STA.BMS.CHG_LIMIT_KW"}, nullptr, nullptr,
     "★ 安全链：BMS 的充电限值会随温度/SOC 动态变化，不是常数"},
    {EMS_CFG_BMS_DIS_LIM, MK_DIRECT, {"STA.BMS.DIS_LIMIT_KW"}, nullptr, nullptr, ""},
    {EMS_CFG_TR_KVA, MK_DIRECT, {"CFG.METER.TR_KVA"}, nullptr, nullptr,
     "变压器容量挂在关口电表侧（计量与容量同属一次侧）"},
    {EMS_CFG_D_TARGET, MK_DIRECT, {"CFG.METER.D_TARGET"}, nullptr, nullptr, ""},
    {EMS_CFG_TAU_S, MK_DIRECT, {"CFG.PCS.TAU_S"}, nullptr, nullptr, ""},
    {EMS_CFG_RAMP_KW_S, MK_DIRECT, {"CFG.PCS.RAMP_KW_PER_S"}, nullptr, nullptr, ""},
    {EMS_CFG_STANDBY, MK_DIRECT, {"CFG.PCS.STANDBY_KW"}, nullptr, nullptr, ""},
    {EMS_CFG_ETA_CHG, MK_DIRECT, {"CFG.BMS.ETA_CHG"}, nullptr, nullptr, ""},
    {EMS_CFG_ETA_DIS, MK_DIRECT, {"CFG.BMS.ETA_DIS"}, nullptr, nullptr, ""},
    // ★ 单位对齐的坑：抽象表 CFG.SOC_PHYS_MIN/MAX 的单位是 "-"（比例 0.05/0.95），
    //   所以设备侧 CFG.BMS.SOC_MIN/MAX 也必须是 "-" 且量程 [0,1]。
    //   写成 "%" + 5..95 会让 self_check ③ 红 —— 这是**有意**的严格：
    //   现场最常见的错误就是"把比例和百分数混着填"。
    {EMS_CFG_SOC_MIN, MK_DIRECT, {"CFG.BMS.SOC_MIN"}, nullptr, nullptr, ""},
    {EMS_CFG_SOC_MAX, MK_DIRECT, {"CFG.BMS.SOC_MAX"}, nullptr, nullptr, ""},

    // ---------------- 状态（设备 → EMS） ----------------
    {EMS_STA_BMS, MK_DIRECT, {"STA.BMS.COMM_OK"}, nullptr, nullptr, ""},
    {EMS_STA_PCS, MK_DIRECT, {"STA.PCS.COMM_OK"}, nullptr, nullptr, ""},
    {EMS_STA_METER, MK_DIRECT, {"STA.METER.COMM_OK"}, nullptr, nullptr, ""},
    {EMS_STA_FAULT, MK_DIRECT, {"STA.PCS.FAULT"}, nullptr, nullptr, ""},
    {EMS_STA_OFFLINE, MK_COMPOSED,
     {"STA.BMS.COMM_OK", "STA.PCS.COMM_OK", "STA.METER.COMM_OK"},
     "any(comm_ok == 0)", "bool",
     "设备离线 = 任一类设备通信断。抽象表只有一个 OFFLINE 位，设备侧有三个通信位"},
    {EMS_STA_VALID, MK_DIRECT, {"STA.METER.DATA_VALID"}, nullptr, nullptr,
     "数据有效性以关口电表为准（它同时是 P_GRID 与负荷/光伏的来源）"},

    // ---- BMS 保护（设备 → EMS，安全输入）----
    {EMS_STA_BMS_CHG_FORBID, MK_DIRECT, {"STA.BMS.CHG_FORBID"}, nullptr, nullptr,
     "★ L0 最底层那道锁：禁充位置 1 → 05/ 折进 (p_lower,p_upper)"},
    {EMS_STA_BMS_DIS_FORBID, MK_DIRECT, {"STA.BMS.DIS_FORBID"}, nullptr, nullptr,
     "★ 同上，禁放"},

    // ---------------- EXT 点区：外部设定（调度 → EMS） ----------------
    {EMS_EXT_P_SETPOINT,   MK_EXTERNAL, {nullptr}, nullptr, nullptr,
     "来自 IEC104 调度侧（P3/），RT_DB 段里没有设备侧来源 —— 显式 EXTERNAL"},
    {EMS_EXT_P_UPPER_SET,  MK_EXTERNAL, {nullptr}, nullptr, nullptr,
     "同上（外部设定的允许上界）"},
    {EMS_EXT_P_LOWER_SET,  MK_EXTERNAL, {nullptr}, nullptr, nullptr, "同上"},
    {EMS_EXT_D_TARGET,     MK_EXTERNAL, {nullptr}, nullptr, nullptr, "同上"},
    {EMS_EXT_PCS_ONOFF,    MK_EXTERNAL, {nullptr}, nullptr, nullptr, "同上"},
    {EMS_EXT_EMS_ENABLE,   MK_EXTERNAL, {nullptr}, nullptr, nullptr, "同上"},
    {EMS_EXT_SEQ,          MK_EXTERNAL, {nullptr}, nullptr, nullptr,
     "本区写入序号（发布信号）—— 由网关维护，不是设备量"},
    {EMS_EXT_TS,           MK_EXTERNAL, {nullptr}, nullptr, nullptr,
     "本区最后写入时标（陈旧判定）—— 同上"}
};

// =====================================================================
// 自检报告
// =====================================================================
struct SelfCheckReport {
    // 五类不一致项计数（C4 的判据 ①~⑤）
    int dup_or_empty_names = 0;   // ① 点名唯一且非空
    int unresolved_sources = 0;   // ② 抽象表每点都能找到来源
    int unit_mismatch      = 0;   // ③ 单位一致
    int range_invalid      = 0;   // ④ 量程合理
    int index_mismatch     = 0;   // ⑤ 索引与数组下标一致
    int total              = 0;

    std::vector<std::string> details;   // 明细（上限 20 条，够定位）

    // 正向证据（自检跑到了多少东西 —— 防止"表是空的所以全过"）
    int checked_device_points = 0;
    int checked_ems_points    = 0;
    int direct_count          = 0;
    int composed_count        = 0;
    int sink_count            = 0;
    int external_count        = 0;

    void add(int* counter, const std::string& msg) {
        ++(*counter);
        ++total;
        if (details.size() < 20) details.push_back(msg);
    }
    std::string text() const {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "点表自检：total=%d  (①点名=%d ②来源=%d ③单位=%d ④量程=%d ⑤索引=%d)\n"
                      "  设备点 %d 个 / 抽象点 %d 个 | 映射 direct=%d composed=%d sink=%d external=%d\n",
                      total, dup_or_empty_names, unresolved_sources, unit_mismatch,
                      range_invalid, index_mismatch,
                      checked_device_points, checked_ems_points,
                      direct_count, composed_count, sink_count, external_count);
        std::string s = buf;
        for (const auto& d : details) s += "  · " + d + "\n";
        return s;
    }
};

// =====================================================================
// self_check —— 返回**不一致项总数**
//
// 默认校验内置点表 + 内置映射表；传入自定义表/映射表则校验该表
// （换站后仍要能跑同一条判据，测试也靠这个参数把映射表**故意改坏**来验判据有区分度）。
// =====================================================================
inline SelfCheckReport self_check_report(const DevicePointTable& table,
                                         const EmsToDeviceMap* map = kEmsToDeviceMap) {
    SelfCheckReport r;
    r.checked_device_points = static_cast<int>(table.size());
    r.checked_ems_points    = EMS_POINT_COUNT;

    // ---- ④ / ①(部分) / ⑤(部分)：交给表自身的 validate ----
    const LoadError ve = table.validate(0);
    if (!ve.ok) {
        // 按字段分类计数
        if (ve.field == "name")                                        r.add(&r.dup_or_empty_names, ve.to_string());
        else if (ve.field == "range" || ve.field == "default")         r.add(&r.range_invalid, ve.to_string());
        else if (ve.field == "index")                                  r.add(&r.index_mismatch, ve.to_string());
        else                                                           r.add(&r.dup_or_empty_names, ve.to_string());
    }

    // ---- ① 抽象表的点名也必须非空（读侧校验，不改 40 点表）----
    for (int i = 0; i < EMS_POINT_COUNT; ++i) {
        if (EMS_POINT_NAMES[i] == nullptr || EMS_POINT_NAMES[i][0] == '\0') {
            r.add(&r.dup_or_empty_names,
                  "抽象表点[" + std::to_string(i) + "] 点名为空");
        }
    }

    // ---- ⑤ 映射表下标 == 抽象表索引 ----
    for (int i = 0; i < EMS_POINT_COUNT; ++i) {
        if (map[i].ems_index != i) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "映射表[%d].ems_index=%d ≠ %d（抽象表枚举顺序变了而映射表没跟）",
                          i, map[i].ems_index, i);
            r.add(&r.index_mismatch, buf);
        }
    }

    // ---- ② 来源可解析 + ③ 单位一致 ----
    for (int i = 0; i < EMS_POINT_COUNT; ++i) {
        const EmsToDeviceMap& m = map[i];
        const char* ems_name = EMS_POINT_NAMES[i];
        const char* ems_unit = EMS_POINT_UNITS[i];
        const std::string where = std::string("抽象点[") + std::to_string(i) + "] " +
                                  (ems_name ? ems_name : "<null>");

        switch (m.kind) {
            case MK_DIRECT:
            case MK_SINK: {
                if (m.sources[0] == nullptr) {
                    r.add(&r.unresolved_sources, where + "：kind=" +
                          map_kind_name(m.kind) + " 但没有来源点");
                    break;
                }
                const DevicePoint* src = table.find(m.sources[0]);
                if (src == nullptr) {
                    r.add(&r.unresolved_sources,
                          where + "：来源点 '" + m.sources[0] + "' 在全点表里不存在");
                    break;
                }
                if (m.sources[1] != nullptr) {
                    r.add(&r.unresolved_sources, where + "：DIRECT/SINK 只能有一个来源点");
                }
                // ③ 单位一致（严格相等 —— 见映射表里单位对齐的注释）
                if (ems_unit != nullptr && src->unit != ems_unit) {
                    r.add(&r.unit_mismatch, where + "：单位 '" + std::string(ems_unit) +
                          "' ≠ 来源 '" + src->name + "' 的单位 '" + src->unit + "'");
                }
                if (m.kind == MK_DIRECT)      ++r.direct_count;
                else                          ++r.sink_count;
                break;
            }
            case MK_COMPOSED: {
                int n = 0;
                for (int s = 0; s < 4 && m.sources[s] != nullptr; ++s) {
                    ++n;
                    if (table.find(m.sources[s]) == nullptr) {
                        r.add(&r.unresolved_sources,
                              where + "：合成来源点 '" + m.sources[s] + "' 不存在");
                    }
                }
                if (n < 2) {
                    r.add(&r.unresolved_sources,
                          where + "：COMPOSED 至少要 2 个来源点，实为 " + std::to_string(n));
                }
                if (m.formula == nullptr || m.formula[0] == '\0') {
                    r.add(&r.unresolved_sources, where + "：COMPOSED 缺 formula");
                }
                if (m.out_unit == nullptr || m.out_unit[0] == '\0') {
                    r.add(&r.unit_mismatch, where + "：COMPOSED 缺 out_unit");
                } else if (ems_unit != nullptr && std::string(m.out_unit) != ems_unit) {
                    r.add(&r.unit_mismatch, where + "：out_unit '" + std::string(m.out_unit) +
                          "' ≠ 抽象单位 '" + std::string(ems_unit) + "'");
                }
                ++r.composed_count;
                break;
            }
            case MK_EXTERNAL: {
                if (m.sources[0] != nullptr) {
                    r.add(&r.unresolved_sources,
                          where + "：EXTERNAL 不应有来源点（来源='" +
                          std::string(m.sources[0]) + "'）");
                }
                if (m.note == nullptr || m.note[0] == '\0') {
                    r.add(&r.unresolved_sources, where + "：EXTERNAL 必须写 reason（note）");
                }
                ++r.external_count;
                break;
            }
            default:
                r.add(&r.unresolved_sources, where + "：未知映射 kind=" +
                      std::to_string(m.kind));
                break;
        }
    }

    // 覆盖性：四种 kind 的计数必须加起来等于抽象表点数 ——
    // 否则说明有分支没走到（例如枚举值被改坏），而"没走到"最容易伪装成"没问题"。
    if (r.direct_count + r.composed_count + r.sink_count + r.external_count
        != EMS_POINT_COUNT) {
        r.add(&r.unresolved_sources,
              "映射覆盖不全：direct+composed+sink+external = " +
              std::to_string(r.direct_count + r.composed_count + r.sink_count +
                             r.external_count) + " ≠ " + std::to_string(EMS_POINT_COUNT));
    }
    return r;
}

// 便捷入口：只要不一致项总数
inline int self_check(const DevicePointTable& table,
                      const EmsToDeviceMap* map = kEmsToDeviceMap) {
    return self_check_report(table, map).total;
}
inline int self_check() {
    return self_check(DevicePointTable::builtin());
}

// =====================================================================
// 反向解析：抽象点 → 设备来源点名（给人看 / 给工具用）
// =====================================================================
inline std::string device_source_of(int ems_index,
                                    const EmsToDeviceMap* map = kEmsToDeviceMap) {
    if (ems_index < 0 || ems_index >= EMS_POINT_COUNT) return "<越界>";
    const EmsToDeviceMap& m = map[ems_index];
    switch (m.kind) {
        case MK_DIRECT:
        case MK_SINK:
            return m.sources[0] ? std::string(m.sources[0]) : std::string("<无>");
        case MK_COMPOSED: {
            std::string s = m.formula ? m.formula : std::string("<无式>");
            s += "  <- ";
            for (int i = 0; i < 4 && m.sources[i] != nullptr; ++i) {
                if (i) s += " + ";
                s += m.sources[i];
            }
            return s;
        }
        case MK_EXTERNAL:
            return std::string("<外部：") + (m.note ? m.note : "") + ">";
        default:
            return "<非法>";
    }
}

} // namespace devpt
} // namespace ems
