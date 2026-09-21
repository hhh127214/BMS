// =====================================================================
// P3 / IEC104 点表映射：**本项目唯一的 IOA 真相源**
//
// ---------------------------------------------------------------------
// 这个文件解决什么问题
// ---------------------------------------------------------------------
// IEC104 里调度只认两个坐标：**CA（公共地址）+ IOA（信息对象地址）**。
// 调度方会给一张「转发表」，写明哪个 IOA 是哪个量、什么类型、什么系数。
//
// 而 EMS 内部用的是**业务点名**（`MEAS.P_GRID` / `CMD.P_BAT` …）。
// 两者必须有一张显式映射表，否则「调度要的第 4004 点到底是什么」永远说不清。
//
// 本文件就是那张表 —— 也是现场唯一需要改的东西。
// 30 个内部点 → 24 个上送点 + 6 个下行点。
// ★ 这里的"30"指**本映射表覆盖的点数**，不是 `ems_point_table.h` 的总点数
//   （该表 2026-09-20 起为 40 点：32 设备侧 + 8 EXT 外部设定）。
//   映射表是**子集**，不必跟随点表总数；self_check() 只做"src 点名必须存在"的
//   子集校验，所以点表扩容不会打断网关（无需改本文件）。
//   **遗留决策**：这两个 BMS 保护位要不要上送调度，属设计待定 —— 见
//   `docs/规划/模拟器与设备接入梳理.md` §10。
//
// ---------------------------------------------------------------------
// 表 = 数据：内置默认表 + 可选 CSV 覆盖（load_point_map_csv）
// ---------------------------------------------------------------------
// 现场换转发表**不用重编**：网关加 `--point-map <file>` 指定 CSV（格式见文件尾注释）。
// 加载失败 → 回退到内置默认表，并在日志/退出码里写清是哪一行错。
// 默认表仍是编译进去的兜底，任何时候都不会「没表可用」。详细设计见 docs/design.md §6。
//
// ---------------------------------------------------------------------
// IOA 区间约定（**本项目自定，现场必须按调度下发的转发表改**）
// ---------------------------------------------------------------------
//   1001..1999   遥信（单点 / 双点）
//   4001..4999   遥测（模拟量，短浮点）
//   5001..5999   遥控（单点 / 双点命令）
//   6001..6999   遥调（设点命令）
//   8001..8999   累计量（电度）
//
// ---------------------------------------------------------------------
// 「好点/坏点」的判定：系数变了，量纲就变了
// ---------------------------------------------------------------------
// SOC 在 EMS 内部是 0..1 的小数，调度要的是百分数 → scale = 100。
// 这类换算**必须写在表里**，散在代码里就会出现「有个地方乘了 100，
// 有个地方没乘」的口径分裂 —— 这正是要把它集中到一张表的原因。
// =====================================================================

#ifndef EMS_P3_IEC104_POINT_MAP_H
#define EMS_P3_IEC104_POINT_MAP_H

#include "ems_point_table.h"    // EMS_POINT_NAMES / EMS_POINT_UNITS 的真相源

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <vector>

namespace iec104 {

// ---------------------------------------------------------------------
// 点类型：决定用哪个 IEC TypeID、以及值怎么编码
// ---------------------------------------------------------------------
enum class Kind {
    kMeasFloat,      // M_ME_NC_1(13) / M_ME_TF_1(36)   短浮点遥测   —— 首选
    kMeasScaled,     // M_ME_NB_1(11) / M_ME_TE_1(35)   标度化遥测（int16）
    kMeasNormalized, // M_ME_NA_1(9)  / M_ME_TD_1(34)   归一化遥测（-1..1）
    kSinglePoint,    // M_SP_NA_1(1)  / M_SP_TB_1(30)   单点遥信
    kDoublePoint,    // M_DP_NA_1(3)  / M_DP_TB_1(31)   双点遥信
    kCounter,        // M_IT_NA_1(15) / M_IT_TB_1(37)   累计量
    kSetpointFloat,  // C_SE_NC_1(50) 遥调（浮点设点）
    kCommandSingle,  // C_SC_NA_1(45) 遥控（单点命令）
    kCommandDouble,  // C_DC_NA_1(46) 遥控（双点命令）
};

inline const char* kind_name(Kind k) {
    switch (k) {
    case Kind::kMeasFloat:      return "遥测/短浮点";
    case Kind::kMeasScaled:     return "遥测/标度化";
    case Kind::kMeasNormalized: return "遥测/归一化";
    case Kind::kSinglePoint:    return "遥信/单点";
    case Kind::kDoublePoint:    return "遥信/双点";
    case Kind::kCounter:        return "累计量";
    case Kind::kSetpointFloat:  return "遥调/浮点设点";
    case Kind::kCommandSingle:  return "遥控/单点命令";
    case Kind::kCommandDouble:  return "遥控/双点命令";
    }
    return "?";
}

inline bool is_control(Kind k) {
    return k == Kind::kSetpointFloat || k == Kind::kCommandSingle ||
           k == Kind::kCommandDouble;
}
inline bool is_measurement(Kind k) { return !is_control(k); }

// ---------------------------------------------------------------------
// kind 的 CSV 文本 ↔ 枚举（大小写不敏感，未知值报错）
//
// 规范 token（9 个，与 design.md §6 的 CSV 格式一致）：
//   MEAS_FLOAT / MEAS_SCALED / MEAS_NORMALIZED
//   SINGLE_POINT / DOUBLE_POINT / COUNTER
//   SET_FLOAT / CMD_SINGLE / CMD_DOUBLE
// ---------------------------------------------------------------------
inline const char* kind_token(Kind k) {
    switch (k) {
    case Kind::kMeasFloat:      return "MEAS_FLOAT";
    case Kind::kMeasScaled:     return "MEAS_SCALED";
    case Kind::kMeasNormalized: return "MEAS_NORMALIZED";
    case Kind::kSinglePoint:    return "SINGLE_POINT";
    case Kind::kDoublePoint:    return "DOUBLE_POINT";
    case Kind::kCounter:        return "COUNTER";
    case Kind::kSetpointFloat:  return "SET_FLOAT";
    case Kind::kCommandSingle:  return "CMD_SINGLE";
    case Kind::kCommandDouble:  return "CMD_DOUBLE";
    }
    return "?";
}

inline bool kind_from_token(const std::string& raw, Kind* out) {
    std::string u;
    u.reserve(raw.size());
    for (char c : raw) {
        if (c == ' ' || c == '\t') continue;   // 容忍字段里的空白
        u += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if      (u == "MEAS_FLOAT")       *out = Kind::kMeasFloat;
    else if (u == "MEAS_SCALED")      *out = Kind::kMeasScaled;
    else if (u == "MEAS_NORMALIZED")  *out = Kind::kMeasNormalized;
    else if (u == "SINGLE_POINT")     *out = Kind::kSinglePoint;
    else if (u == "DOUBLE_POINT")     *out = Kind::kDoublePoint;
    else if (u == "COUNTER")          *out = Kind::kCounter;
    else if (u == "SET_FLOAT")        *out = Kind::kSetpointFloat;
    else if (u == "CMD_SINGLE")       *out = Kind::kCommandSingle;
    else if (u == "CMD_DOUBLE")       *out = Kind::kCommandDouble;
    else return false;
    return true;
}

// ---------------------------------------------------------------------
// 单调定义
// ---------------------------------------------------------------------
struct PointDef {
    int         ioa;      // IEC 104 信息对象地址
    Kind        kind;
    int         group;    // 总召唤分组 1..16；0 = 不进总召唤（只自发/只接收）
    const char* name;     // 中文名（给人看：报告、日志、对照表）
    const char* src;      // **内部点名**（与 ems_point_table.h 逐字相同）；
                          // 控制点为 ""（值来自调度，不来自点表）
    double      scale;    // 工程量 × scale + offset = 协议值
    double      offset;
    const char* unit;
    const char* note;     // 现场注意点
};

// ---------------------------------------------------------------------
// 可覆盖表的存储（A3 之后：转发表外置 CSV）
//
// 默认表用 string literal；load_point_map_csv() 成功后换成解析出来的表。
// pool 用 std::deque 而不是 std::vector：deque 在 push_back 时**不移动
// 既有元素**（只失效迭代器，不失效引用/指针），所以短串（SSO）的
// c_str() 也不会因扩容而悬空 —— defs 里的 const char* 全部指向 pool。
// ---------------------------------------------------------------------
struct PointMapStore {
    std::vector<PointDef>   defs;
    std::deque<std::string> pool;
};
inline PointMapStore& upload_store()   { static PointMapStore s; return s; }
inline PointMapStore& download_store() { static PointMapStore s; return s; }

// 清掉已加载的覆盖表，回到内置默认（测试用；网关进程内不调用）。
inline void reset_point_map() {
    upload_store()   = PointMapStore{};
    download_store() = PointMapStore{};
}

// =====================================================================
// 上送（EMS → 调度）：24 点
//
// 覆盖设计方案的四个业务面：
//   · 功率流        负荷 / 光伏 / 电池 / 关口
//   · 电池状态      SOC / 温度 / SOH
//   · EMS 决策     当前指令 + **权限区间**（这个是本项目独有价值：
//                   调度能直接看到 EMS 允许的调节范围，而不是只看到一个功率值）
//   · 设备与配置    额定容量 / PCS 幅度 / 契约需量 / SOC 边界 / BMS 限值
//   · 通信与故障    6 个状态位
// =====================================================================
inline const PointDef* upload_table(std::size_t* count) {
    PointMapStore& s = upload_store();
    if (!s.defs.empty()) {                    // 已加载 CSV → 用覆盖表
        if (count) *count = s.defs.size();
        return s.defs.data();
    }
    static const PointDef kTable[] = {
        // ---------------- 遥测：功率流 ----------------
        {4001, Kind::kMeasFloat, 1, "负荷有功",   "MEAS.P_LOAD",  1.0, 0.0, "kW",
         "正=用电；关口逆流时负荷仍为正"},
        {4002, Kind::kMeasFloat, 1, "光伏有功",   "MEAS.P_PV",    1.0, 0.0, "kW",
         "始终 >= 0（不建模夜间反送）"},
        {4003, Kind::kMeasFloat, 1, "电池功率",   "MEAS.P_BAT",   1.0, 0.0, "kW",
         "**放电为正、充电为负**（项目统一符号约定，调度侧若取反请在转发表注明）"},
        {4004, Kind::kMeasFloat, 1, "关口功率",   "MEAS.P_GRID",  1.0, 0.0, "kW",
         "购电为正、倒送为负。**防逆流 S05 的命门**，也是调度最关心的点"},

        // ---------------- 遥测：电池状态 ----------------
        {4005, Kind::kMeasFloat, 1, "SOC",       "MEAS.SOC",     100.0, 0.0, "%",
         "内部 0..1 → 上送百分数（scale=100）"},
        {4006, Kind::kMeasFloat, 1, "电池温度",   "MEAS.T_C",     1.0, 0.0, "degC", ""},
        {4007, Kind::kMeasFloat, 1, "SOH",       "MEAS.SOH",     100.0, 0.0, "%",
         "内部 0..1 → 上送百分数（scale=100）"},

        // ---------------- 遥测：EMS 决策（含权限区间）----------------
        {4008, Kind::kMeasFloat, 1, "电池功率指令", "CMD.P_BAT",   1.0, 0.0, "kW",
         "EMS 本拍实际下发给 PCS 的值（含 L2 纠偏后的结果）"},
        {4009, Kind::kMeasFloat, 1, "允许功率上界", "CMD.P_UPPER", 1.0, 0.0, "kW",
         "★ 安全引擎收敛出的区间上界 —— 调度据此判断 EMS 还能不能多放"},
        {4010, Kind::kMeasFloat, 1, "允许功率下界", "CMD.P_LOWER", 1.0, 0.0, "kW",
         "★ 安全引擎收敛出的区间下界 —— 调度据此判断 EMS 还能不能多充"},

        // ---------------- 遥测：设备与配置 ----------------
        {4011, Kind::kMeasFloat, 1, "电池额定容量", "CFG.BAT_CAP_KWH",   1.0, 0.0, "kWh", ""},
        {4012, Kind::kMeasFloat, 1, "PCS 额定充电", "CFG.PCS_MAX_CHG",   1.0, 0.0, "kW",  ""},
        {4013, Kind::kMeasFloat, 1, "PCS 额定放电", "CFG.PCS_MAX_DIS",   1.0, 0.0, "kW",  ""},
        {4014, Kind::kMeasFloat, 1, "契约需量",     "CFG.D_TARGET",      1.0, 0.0, "kW",
         "S04 需量管理的目标值，调度常需要核对"},
        {4015, Kind::kMeasFloat, 1, "SOC 物理下限", "CFG.SOC_PHYS_MIN", 100.0, 0.0, "%", ""},
        {4016, Kind::kMeasFloat, 1, "SOC 物理上限", "CFG.SOC_PHYS_MAX", 100.0, 0.0, "%", ""},
        {4017, Kind::kMeasFloat, 1, "BMS 充电限值", "CFG.BMS_CHG_LIM",   1.0, 0.0, "kW",
         "★ 见 docs/design.md §4：真实系统里这个值应由 BMS 点表送来，"
         "当前 RT_DB 未建模（只能在仿真里注入），接真机前必须补"},
        {4018, Kind::kMeasFloat, 1, "BMS 放电限值", "CFG.BMS_DIS_LIM",   1.0, 0.0, "kW",
         "★ 同上"},

        // ---------------- 遥信：通信与故障 ----------------
        {1001, Kind::kSinglePoint, 1, "BMS 通信正常",  "STA.BMS_COMM_OK",   1.0, 0.0, "", ""},
        {1002, Kind::kSinglePoint, 1, "PCS 通信正常",  "STA.PCS_COMM_OK",   1.0, 0.0, "", ""},
        {1003, Kind::kSinglePoint, 1, "电表通信正常",  "STA.METER_COMM_OK", 1.0, 0.0, "", ""},
        {1004, Kind::kSinglePoint, 1, "PCS 故障",      "STA.PCS_FAULT",     1.0, 0.0, "",
         "1=有故障。会驱动 EMS 状态机进 FAULT"},
        {1005, Kind::kSinglePoint, 1, "设备离线",      "STA.OFFLINE",       1.0, 0.0, "", ""},
        {1006, Kind::kSinglePoint, 1, "数据有效",      "STA.DATA_VALID",    1.0, 0.0, "",
         "0 → 本 ASDU 内所有量的品质位置 INVALID"},
    };
    if (count) *count = sizeof(kTable) / sizeof(kTable[0]);
    return kTable;
}

// =====================================================================
// 下行（调度 → EMS）：6 点
//
// ★ 这 6 个点**目前没有落点** —— RT_DB 点表里没有「外部设定」区。
//   网关通过 ICommandSink 把命令交出去，由装配层决定往哪放。
//   方案与影响面见 docs/design.md §5（新增 EXT 点区，6 点，纯追加不动老索引）。
// =====================================================================
inline const PointDef* download_table(std::size_t* count) {
    PointMapStore& s = download_store();
    if (!s.defs.empty()) {
        if (count) *count = s.defs.size();
        return s.defs.data();
    }
    static const PointDef kTable[] = {
        // ---------------- 遥调：设点 ----------------
        {6001, Kind::kSetpointFloat, 0, "有功功率设定", "", 1.0, 0.0, "kW",
         "★ 最高优先级的外部设定：应映射到**权限区间**而不是直接覆盖 p_desired，"
         "否则调度的设定会绕过 L0 安全约束（违反设计方案 §十一「安全优先级最高」）"},
        {6002, Kind::kSetpointFloat, 0, "契约需量设定", "", 1.0, 0.0, "kW",
         "写 CFG.D_TARGET，影响 S04 需量管理的目标"},
        {6003, Kind::kSetpointFloat, 0, "允许上界设定", "", 1.0, 0.0, "kW",
         "★ 调度远程收紧放电权限（如电网检修期）"},
        {6004, Kind::kSetpointFloat, 0, "允许下界设定", "", 1.0, 0.0, "kW",
         "★ 调度远程收紧充电权限"},

        // ---------------- 遥控：开关 ----------------
        {5001, Kind::kCommandSingle, 0, "PCS 远程启停", "", 1.0, 0.0, "",
         "true=启动 / false=停机"},
        {5002, Kind::kCommandSingle, 0, "EMS 投入退出", "", 1.0, 0.0, "",
         "true=EMS 接管 / false=调度闭锁 EMS（只监视不调节）"},
    };
    if (count) *count = sizeof(kTable) / sizeof(kTable[0]);
    return kTable;
}

// ---------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------
inline const PointDef* find_by_ioa(const PointDef* table, std::size_t n, int ioa) {
    for (std::size_t i = 0; i < n; ++i)
        if (table[i].ioa == ioa) return &table[i];
    return nullptr;
}

inline const PointDef* find_upload_by_src(const char* point_name) {
    std::size_t n = 0;
    const PointDef* t = upload_table(&n);
    if (point_name == nullptr) return nullptr;
    for (std::size_t i = 0; i < n; ++i)
        if (std::strcmp(t[i].src, point_name) == 0) return &t[i];
    return nullptr;
}

// 工程量 → 协议值
inline double to_protocol(const PointDef& d, double raw) {
    return raw * d.scale + d.offset;
}
// 协议值 → 工程量（下行用）
inline double from_protocol(const PointDef& d, double wire) {
    return (d.scale == 0.0) ? wire : (wire - d.offset) / d.scale;
}

// ---------------------------------------------------------------------
// 自检：现场上电第一件事。返回问题条数，0 = 表本身没问题。
//
// 查三件事（都是踩过的坑）：
//   1. src 点名必须能在 ems_point_table.h 里找到 —— 否则这个点永远读到 0；
//   2. ioa 必须唯一且落在约定的区间里 —— 撞号会让调度收到错的数据；
//   3. group 必须在 0..16。
// ---------------------------------------------------------------------
inline int self_check_tables(const PointDef* up, std::size_t up_n,
                             const PointDef* dn, std::size_t dn_n,
                             std::string* report) {
    int bad = 0;
    std::string out;
    char line[512];

    for (std::size_t i = 0; i < up_n; ++i) {
        const PointDef& d = up[i];
        // 1) src 点名必须在点表里
        bool found = false;
        for (std::size_t k = 0; k < EMS_POINT_COUNT; ++k) {
            if (std::strcmp(EMS_POINT_NAMES[k], d.src) == 0) { found = true; break; }
        }
        if (!found) {
            ++bad;
            std::snprintf(line, sizeof(line), "  [IOA %d] %s: src 点名 '%s' 不在 ems_point_table.h 中\n",
                          d.ioa, d.name, d.src);
            out += line;
        }
        // 2) IOA 唯一
        for (std::size_t k = i + 1; k < up_n; ++k) {
            if (up[k].ioa == d.ioa) {
                ++bad;
                std::snprintf(line, sizeof(line), "  [IOA %d] 上送表内撞号：%s / %s\n",
                              d.ioa, d.name, up[k].name);
                out += line;
            }
        }
        // 3) 区间
        const int lo = d.ioa / 1000;
        const bool range_ok =
            (d.kind == Kind::kSinglePoint || d.kind == Kind::kDoublePoint) ? (lo == 1) :
            (d.kind == Kind::kCounter)                                     ? (lo == 8) : (lo == 4);
        if (!range_ok) {
            ++bad;
            std::snprintf(line, sizeof(line), "  [IOA %d] %s (%s) 落在错误区间\n",
                          d.ioa, d.name, kind_name(d.kind));
            out += line;
        }
        // 4) group
        if (d.group < 0 || d.group > 16) {
            ++bad;
            std::snprintf(line, sizeof(line), "  [IOA %d] group=%d 越界\n", d.ioa, d.group);
            out += line;
        }
        // 5) 上送点不该是控制类型
        if (is_control(d.kind)) {
            ++bad;
            std::snprintf(line, sizeof(line), "  [IOA %d] %s: 上送表里出现控制类型\n",
                          d.ioa, d.name);
            out += line;
        }
    }

    for (std::size_t i = 0; i < dn_n; ++i) {
        const PointDef& d = dn[i];
        if (!is_control(d.kind)) {
            ++bad;
            std::snprintf(line, sizeof(line), "  [IOA %d] %s: 下行表里出现量测类型\n",
                          d.ioa, d.name);
            out += line;
        }
        const int lo = d.ioa / 1000;
        const bool range_ok = (d.kind == Kind::kSetpointFloat) ? (lo == 6) : (lo == 5);
        if (!range_ok) {
            ++bad;
            std::snprintf(line, sizeof(line), "  [IOA %d] %s (%s) 落在错误区间\n",
                          d.ioa, d.name, kind_name(d.kind));
            out += line;
        }
        for (std::size_t k = i + 1; k < dn_n; ++k) {
            if (dn[k].ioa == d.ioa) {
                ++bad;
                std::snprintf(line, sizeof(line), "  [IOA %d] 下行表内撞号：%s / %s\n",
                              d.ioa, d.name, dn[k].name);
                out += line;
            }
        }
        for (std::size_t k = 0; k < up_n; ++k) {
            if (up[k].ioa == d.ioa) {
                ++bad;
                std::snprintf(line, sizeof(line), "  [IOA %d] 上下行共用同一 IOA：%s / %s\n",
                              d.ioa, up[k].name, d.name);
                out += line;
            }
        }
    }

    if (report) *report = out;
    return bad;
}

// ---------------------------------------------------------------------
// self_check：对**当前生效**的表（内置默认或已加载 CSV）跑同一套规则
// ---------------------------------------------------------------------
inline int self_check(std::string* report = nullptr) {
    std::size_t up_n = 0, dn_n = 0;
    const PointDef* up = upload_table(&up_n);
    const PointDef* dn = download_table(&dn_n);
    return self_check_tables(up, up_n, dn, dn_n, report);
}

// ---------------------------------------------------------------------
// 转发表 CSV 加载（design.md §6）
//
// 格式（9 列）：ioa,kind,group,name,src,scale,offset,unit,note
//   · 首行可为表头（首列小写后 == "ioa" 即跳过）；# 开头为注释；空行跳过。
//   · kind 大小写不敏感（见 kind_from_token），未知值报错。
//   · scale=0 报错（from_protocol 除零）。
//   · name/src/unit 不含逗号；note 可含逗号（取为最后一段）。
//   · 加载后**按 is_control 分表**：量测进上送、控制进下行（与内置默认表同构）。
//   · 全表过 self_check_tables 同一套规则，任一不过 → 回退内置默认表、返回 false。
//
// ★ 时序：必须在 open_owned()/attach()/Iec104Server 构造**之前**调用 ——
//   这些入口都会 capture upload_table() 的指针，晚于本函数加载会让旧指针悬空。
// ---------------------------------------------------------------------
inline bool load_point_map_csv(const char* path, std::string* report) {
    auto fail = [&](const std::string& msg) -> bool {
        if (report) *report = msg;
        return false;
    };
    if (path == nullptr || *path == '\0') return fail("转发表路径为空");

    std::ifstream in(path);
    if (!in.is_open()) return fail(std::string("打不开转发表：") + path);

    struct Row {
        int    ioa = 0;
        Kind   kind = Kind::kMeasFloat;
        int    group = 0;
        std::string name, src, unit, note;
        double scale = 1.0, offset = 0.0;
    };
    std::vector<Row> rows;

    auto trim = [](const std::string& s) -> std::string {
        std::size_t b = 0, e = s.size();
        while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
        while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r')) --e;
        return s.substr(b, e - b);
    };
    auto parse_int = [](const std::string& s, int* out) -> bool {
        if (s.empty()) return false;
        char* end = nullptr;
        errno = 0;
        const long v = std::strtol(s.c_str(), &end, 10);
        if (errno != 0 || end == s.c_str() || *end != '\0') return false;
        if (v < INT_MIN || v > INT_MAX) return false;
        *out = static_cast<int>(v);
        return true;
    };
    auto parse_double = [](const std::string& s, double* out) -> bool {
        if (s.empty()) return false;
        char* end = nullptr;
        errno = 0;
        const double v = std::strtod(s.c_str(), &end);
        if (errno != 0 || end == s.c_str() || *end != '\0') return false;
        *out = v;
        return true;
    };

    std::string line;
    int lineno = 0;
    bool first_data = true;
    while (std::getline(in, line)) {
        ++lineno;
        if (first_data && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);      // 去 UTF-8 BOM
        }
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;

        if (first_data) {
            first_data = false;
            const std::size_t c = t.find(',');
            const std::string h = (c == std::string::npos) ? t : t.substr(0, c);
            std::string htrim;
            for (char ch : h) {
                if (ch != ' ' && ch != '\t')
                    htrim += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            if (htrim == "ioa") continue;      // 表头行
        }

        // 拆 9 列：前 8 个逗号切字段，第 9 段（note）可含逗号
        std::string fields[9];
        int nf = 0;
        std::string cur;
        int splits = 0;
        for (char c : line) {
            if (c == ',' && splits < 8) {
                fields[nf++] = trim(cur); cur.clear(); ++splits;
            } else {
                cur += c;
            }
        }
        fields[nf++] = trim(cur);
        if (nf != 9) {
            return fail("第 " + std::to_string(lineno) + " 行字段数不对（应 9 列，实得 " +
                        std::to_string(nf) + " 列）");
        }

        Row r;
        if (!parse_int(fields[0], &r.ioa))
            return fail("第 " + std::to_string(lineno) + " 行 IOA 不是整数：" + fields[0]);
        if (!kind_from_token(fields[1], &r.kind))
            return fail("第 " + std::to_string(lineno) + " 行 kind 未知：" + fields[1]);
        if (!parse_int(fields[2], &r.group))
            return fail("第 " + std::to_string(lineno) + " 行 group 不是整数：" + fields[2]);
        r.name = fields[3];
        r.src  = fields[4];
        if (!parse_double(fields[5], &r.scale))
            return fail("第 " + std::to_string(lineno) + " 行 scale 不是数：" + fields[5]);
        if (!parse_double(fields[6], &r.offset))
            return fail("第 " + std::to_string(lineno) + " 行 offset 不是数：" + fields[6]);
        r.unit = fields[7];
        r.note = fields[8];
        if (r.scale == 0.0)
            return fail("第 " + std::to_string(lineno) + " 行 scale=0（from_protocol 除零）");
        rows.push_back(std::move(r));
    }

    // 按 is_control 分表，字符串入池取指针（deque 不移动元素，c_str 稳定）
    PointMapStore up, dn;
    for (const Row& r : rows) {
        PointMapStore& target = is_control(r.kind) ? dn : up;
        PointDef d;
        d.ioa    = r.ioa;
        d.kind   = r.kind;
        d.group  = r.group;
        d.scale  = r.scale;
        d.offset = r.offset;
        d.name   = target.pool.emplace_back(r.name).c_str();
        d.src    = target.pool.emplace_back(r.src).c_str();
        d.unit   = target.pool.emplace_back(r.unit).c_str();
        d.note   = target.pool.emplace_back(r.note).c_str();
        target.defs.push_back(d);
    }

    // 与内置表同一套规则校验
    std::string err;
    const int bad = self_check_tables(up.defs.data(), up.defs.size(),
                                      dn.defs.data(), dn.defs.size(), &err);
    if (bad != 0) {
        return fail("转发表校验未通过（" + std::to_string(bad) + " 处）：\n" + err);
    }

    // ★ std::move 安全：vector/deque 的移动构造保证元素地址不变，
    //   所以 defs 里指向 pool 的 const char* 在移动后仍然有效。
    upload_store()   = std::move(up);
    download_store() = std::move(dn);
    if (report) *report = "";
    return true;
}

} // namespace iec104

#endif // EMS_P3_IEC104_POINT_MAP_H
