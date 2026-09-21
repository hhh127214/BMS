// =====================================================================
// 13/ — Modbus 点表映射（**映射的唯一真相源**）
//
// 本文件回答一个问题：`ems_point_table.h` 的 32 个点，在 Modbus 上落在
// 哪张表、哪个地址、怎么编码。
//
// 与 RT_DB 路线的关键差别（这是接真机时才暴露的东西）：
//   · RT_DB 里"点名 → 索引"就够了（值域是 double，存在共享内存里）。
//   · Modbus 是**寄存器**：16 位、有量纲、要拆字、要分表、要分块读。
//     于是多出三个现场最容易出错的维度：
//       ★ **字序**：32 位浮点占两个寄存器，谁在前协议**没规定**。
//       ★ **缩放**：SOC/温度这类量在表里是整数（千分位/十分位），
//         换算错不会报错，只会让 SOC 从 0.5 变成 5000。
//       ★ **分块**：FC03/04 单次最多 125 个寄存器，且**地址必须连续**。
//         点表一散就得拆成多次请求 —— 现场"采一帧要 3 秒"多半是这个。
//
// 三条不可违反的纪律（都有编译期守卫）：
//   ① 本表覆盖 ems_point_table.h 里 EXT 区之前的**全部**点，顺序与索引严格一致
//      （EXT 区是调度→EMS 的外部设定，不经设备总线，故不绑定）
//      → `bindings_well_formed()` + 覆盖范围 static_assert
//   ② 段的可写性由**点表段**决定，不由本表随便定：
//        量测 / 配置 / 状态 = 只读（输入寄存器 或 离散输入）
//        指令               = 可写（保持寄存器）
//      → `segments_consistent()` + static_assert
//   ③ 需要哪个点，就**必须**能从本表查到；查不到 = 配置缺失，不是"跳过"
//
// 编译：纯头文件，C++17。依赖 13/src/modbus_tcp_client.h 与
//       07/src/rtdb/ems_point_table.h（点表真相源，C 头）。
// =====================================================================

#pragma once

#include "modbus_tcp_client.h"   // 13/  字节序工具 / WordOrder / 协议常量
#include "ems_point_table.h"     // 07/  点表真相源（40 点；EXT 区自 EMS_EXT_BEGIN 起不绑定）

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace ems {
namespace modbus {

// =====================================================================
// Modbus 四张表
//
// 现场务必分清（名字相近，行为差别很大）：
//   kCoil          0xxxx  FC01 读 / FC05·FC15 写   **可读写位**
//   kDiscreteInput 1xxxx  FC02 读                  **只读位**
//   kInputReg      3xxxx  FC04 读                  **只读寄存器**
//   kHoldingReg    4xxxx  FC03 读 / FC06·FC16 写   **可读写寄存器**
// =====================================================================
enum class Table : std::uint8_t {
    kCoil = 0,
    kDiscreteInput,
    kInputReg,
    kHoldingReg,
};

// 点的编码方式
enum class Encoding : std::uint8_t {
    kF32 = 0,   // 32 位浮点，占 2 个寄存器（字序见 WordOrder）
    kU16,       // 无符号 16 位整数，物理值 = raw / scale
    kI16,       // 有符号 16 位整数，物理值 = raw / scale
    kBit,       // 单个位（离散输入 / 线圈）
};

struct PointBinding {
    std::size_t   index;       // ems_point_table.h 的枚举值（0..31）
    const char*   name;        // 必须与 EMS_POINT_NAMES[index] 逐字相同
    Table         table;
    std::uint16_t address;     // 所在表的起始地址
    Encoding      encoding;
    WordOrder     word_order;  // 仅 kF32 有意义
    double        scale;       // 仅 kU16 / kI16 有意义（物理值 = raw / scale）
    bool          writable;

    // 占用几个寄存器（位类型为 0）
    constexpr std::uint16_t reg_count() const {
        return (encoding == Encoding::kF32) ? 2 : (encoding == Encoding::kBit ? 0 : 1);
    }
};

// =====================================================================
// 映射表本体
//
// 地址规划（**段内连续**是刻意的：它让"读全 32 点"只需 3 次请求）。
//
//   ┌ 输入寄存器（只读，FC04）────────────────────────────────────┐
//   │ 0..10   量测 7 点（P_LOAD/P_PV/P_BAT/P_GRID/SOC/T_C/SOH）  │
//   │ 11      空一格，让配置段从对齐地址 12 起                     │
//   │ 12..39  配置 14 点（每点 2 寄存器）                          │
//   └────────────────────────────────────────────────────────────┘
//   ┌ 保持寄存器（可写，FC03/06/16）──────────────────────────────┐
//   │ 0..5    指令 3 点（P_BAT / P_UPPER / P_LOWER）              │
//   └────────────────────────────────────────────────────────────┘
//   ┌ 离散输入（只读位，FC02）────────────────────────────────────┐
//   │ 0..7    状态 8 位（含两个 BMS 禁充放安全位）                 │
//   └────────────────────────────────────────────────────────────┘
//
// ★ 关于 `MEAS.P_BAT` 用**低字在前**（其它 f32 都是高字在前）：
//   这是**刻意的**，不是笔误。现场同一台设备不同数据区用不同字序是常态
//   （不同厂家/不同固件版本/不同寄存器区都可能不一样），而"统一成一种"
//   恰恰会让测试失去区分度 —— 一个把字序写死的实现必须在这里露馅。
//   写错字序的表现**不是差一点**，是数值直接变成天文数字或 NaN，
//   所以 device_io 里有 kNonFinite 守卫，本文件有 T05 的字节级断言。
// =====================================================================
// ★ 为什么数组长度是 `EMS_EXT_BEGIN` 而不是 `EMS_POINT_COUNT`（A3.1 起）：
//   EXT 点区是**调度 → EMS** 的外部设定，根本不经过设备总线 ——
//   Modbus 侧没有它的语义位置。把数组**按 EXT 区起点定长**，
//   这个"不覆盖 EXT"的事实就写进了类型，而不是靠注释提醒。
//   若将来真要让 Modbus 暴露某个 EXT 点，必须改这里 —— 那正是应该被拦下来的动作。
constexpr PointBinding kBindings[EMS_EXT_BEGIN] = {
    // ---- 量测（只读，输入寄存器）----
    /* [EMS_P_LOAD]  */ { EMS_P_LOAD,  "MEAS.P_LOAD",  Table::kInputReg, 0,
                          Encoding::kF32, WordOrder::kHighWordFirst, 0.0,  false },
    /* [EMS_P_PV]    */ { EMS_P_PV,    "MEAS.P_PV",    Table::kInputReg, 2,
                          Encoding::kF32, WordOrder::kHighWordFirst, 0.0,  false },
    /* [EMS_P_BAT]   */ { EMS_P_BAT,   "MEAS.P_BAT",   Table::kInputReg, 4,
                          Encoding::kF32, WordOrder::kLowWordFirst,  0.0,  false },
    /* [EMS_P_GRID]  */ { EMS_P_GRID,  "MEAS.P_GRID",  Table::kInputReg, 6,
                          Encoding::kF32, WordOrder::kHighWordFirst, 0.0,  false },
    /* [EMS_SOC]     */ { EMS_SOC,     "MEAS.SOC",     Table::kInputReg, 8,
                          Encoding::kU16, WordOrder::kHighWordFirst, 10000.0, false },
    /* [EMS_T_C]     */ { EMS_T_C,     "MEAS.T_C",     Table::kInputReg, 9,
                          Encoding::kI16, WordOrder::kHighWordFirst, 10.0,   false },
    /* [EMS_SOH]     */ { EMS_SOH,     "MEAS.SOH",     Table::kInputReg, 10,
                          Encoding::kU16, WordOrder::kHighWordFirst, 10000.0, false },

    // ---- 指令（可写，保持寄存器）----
    /* [EMS_CMD_P_BAT] */ { EMS_CMD_P_BAT, "CMD.P_BAT",   Table::kHoldingReg, 0,
                            Encoding::kF32, WordOrder::kHighWordFirst, 0.0, true },
    /* [EMS_CMD_P_UPPER] */ { EMS_CMD_P_UPPER, "CMD.P_UPPER", Table::kHoldingReg, 2,
                              Encoding::kF32, WordOrder::kHighWordFirst, 0.0, true },
    /* [EMS_CMD_P_LOWER] */ { EMS_CMD_P_LOWER, "CMD.P_LOWER", Table::kHoldingReg, 4,
                              Encoding::kF32, WordOrder::kHighWordFirst, 0.0, true },

    // ---- 配置（设备提供，只读；EMS 读不了就只能靠"装配期注入"，那是退路不是正路）----
    /* [EMS_CFG_CAP_KWH]  */ { EMS_CFG_CAP_KWH,  "CFG.BAT_CAP_KWH",        Table::kInputReg, 12,
                               Encoding::kF32, WordOrder::kHighWordFirst, 0.0, false },
    /* [EMS_CFG_MAX_CHG]  */ { EMS_CFG_MAX_CHG,  "CFG.PCS_MAX_CHG",        Table::kInputReg, 14,
                               Encoding::kF32, WordOrder::kHighWordFirst, 0.0, false },
    /* [EMS_CFG_MAX_DIS]  */ { EMS_CFG_MAX_DIS,  "CFG.PCS_MAX_DIS",        Table::kInputReg, 16,
                               Encoding::kF32, WordOrder::kHighWordFirst, 0.0, false },
    /* [EMS_CFG_BMS_CHG_LIM] */ { EMS_CFG_BMS_CHG_LIM, "CFG.BMS_CHG_LIM",  Table::kInputReg, 18,
                                 Encoding::kF32, WordOrder::kHighWordFirst, 0.0, false },
    /* [EMS_CFG_BMS_DIS_LIM] */ { EMS_CFG_BMS_DIS_LIM, "CFG.BMS_DIS_LIM",  Table::kInputReg, 20,
                                 Encoding::kF32, WordOrder::kHighWordFirst, 0.0, false },
    /* [EMS_CFG_TR_KVA]   */ { EMS_CFG_TR_KVA,   "CFG.TRANSFORMER_KVA",    Table::kInputReg, 22,
                               Encoding::kF32, WordOrder::kHighWordFirst, 0.0, false },
    /* [EMS_CFG_D_TARGET] */ { EMS_CFG_D_TARGET, "CFG.D_TARGET",           Table::kInputReg, 24,
                               Encoding::kF32, WordOrder::kHighWordFirst, 0.0, false },
    /* [EMS_CFG_TAU_S]    */ { EMS_CFG_TAU_S,    "CFG.TAU_S",              Table::kInputReg, 26,
                               Encoding::kF32, WordOrder::kHighWordFirst, 0.0, false },
    /* [EMS_CFG_RAMP_KW_S]*/ { EMS_CFG_RAMP_KW_S, "CFG.PCS_RAMP_KW_PER_S", Table::kInputReg, 28,
                               Encoding::kF32, WordOrder::kHighWordFirst, 0.0, false },
    /* [EMS_CFG_STANDBY]  */ { EMS_CFG_STANDBY,  "CFG.PCS_STANDBY",        Table::kInputReg, 30,
                               Encoding::kF32, WordOrder::kHighWordFirst, 0.0, false },
    /* [EMS_CFG_ETA_CHG]  */ { EMS_CFG_ETA_CHG,  "CFG.ETA_CHG",            Table::kInputReg, 32,
                               Encoding::kF32, WordOrder::kHighWordFirst, 0.0, false },
    /* [EMS_CFG_ETA_DIS]  */ { EMS_CFG_ETA_DIS,  "CFG.ETA_DIS",            Table::kInputReg, 34,
                               Encoding::kF32, WordOrder::kHighWordFirst, 0.0, false },
    /* [EMS_CFG_SOC_MIN]  */ { EMS_CFG_SOC_MIN,  "CFG.SOC_PHYS_MIN",       Table::kInputReg, 36,
                               Encoding::kF32, WordOrder::kHighWordFirst, 0.0, false },
    /* [EMS_CFG_SOC_MAX]  */ { EMS_CFG_SOC_MAX,  "CFG.SOC_PHYS_MAX",       Table::kInputReg, 38,
                               Encoding::kF32, WordOrder::kHighWordFirst, 0.0, false },

    // ---- 状态（只读位，离散输入）----
    /* [EMS_STA_BMS]         */ { EMS_STA_BMS,         "STA.BMS_COMM_OK",   Table::kDiscreteInput, 0,
                                  Encoding::kBit, WordOrder::kHighWordFirst, 0.0, false },
    /* [EMS_STA_PCS]         */ { EMS_STA_PCS,         "STA.PCS_COMM_OK",   Table::kDiscreteInput, 1,
                                  Encoding::kBit, WordOrder::kHighWordFirst, 0.0, false },
    /* [EMS_STA_METER]       */ { EMS_STA_METER,       "STA.METER_COMM_OK", Table::kDiscreteInput, 2,
                                  Encoding::kBit, WordOrder::kHighWordFirst, 0.0, false },
    /* [EMS_STA_FAULT]       */ { EMS_STA_FAULT,       "STA.PCS_FAULT",     Table::kDiscreteInput, 3,
                                  Encoding::kBit, WordOrder::kHighWordFirst, 0.0, false },
    /* [EMS_STA_OFFLINE]     */ { EMS_STA_OFFLINE,     "STA.OFFLINE",       Table::kDiscreteInput, 4,
                                  Encoding::kBit, WordOrder::kHighWordFirst, 0.0, false },
    /* [EMS_STA_VALID]       */ { EMS_STA_VALID,       "STA.DATA_VALID",    Table::kDiscreteInput, 5,
                                  Encoding::kBit, WordOrder::kHighWordFirst, 0.0, false },
    // ★ 这两个是**安全输入**：经 read_limits() → DeviceLimits → 04/S01(kBmsForbid, L0)
    //   → 05/ 折进 (p_lower, p_upper)。现场网关必须每拍显式写 —— 位类型尤其危险：
    //   从站不刷新时它停在 0 = "允许充放"，与"没配这条线"在值上不可区分。
    //   fail-safe 由 05/ 的 bms_comm_lost → [0,0] **独立**承担（见 EMS_STA_BMS）。
    /* [EMS_STA_BMS_CHG_FORBID] */ { EMS_STA_BMS_CHG_FORBID, "STA.BMS_CHG_FORBID",
                                     Table::kDiscreteInput, 6,
                                     Encoding::kBit, WordOrder::kHighWordFirst, 0.0, false },
    /* [EMS_STA_BMS_DIS_FORBID] */ { EMS_STA_BMS_DIS_FORBID, "STA.BMS_DIS_FORBID",
                                     Table::kDiscreteInput, 7,
                                     Encoding::kBit, WordOrder::kHighWordFirst, 0.0, false },
};

// =====================================================================
// 编译期守卫 —— 让"映射表与点表脱节"变成**编译错误**而不是现场事故
// =====================================================================

constexpr std::size_t kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);

// ⓪ 覆盖范围：本表必须覆盖 EXT 区之前的**每一个**点
//
// ★ 这条守卫的写法是刻意的（A3.1 加的 EXT 点区把原来的"覆盖全部点"撑破了）。
//   不写成"覆盖全部点"，是因为 EXT 区**本来就不该**出现在这里；
//   也不写成"覆盖前 32 个"，因为 32 是个会漂的数字。
//
//   `EMS_EXT_BEGIN` 是"设备侧点的个数"，所以 `kBindingCount == EMS_EXT_BEGIN`
//   这句话的准确含义是：**未绑定的点恰好是 EXT 区，一个不多一个不少。**
//   将来有人加了个新设备点（名字不是 EXT.*）却忘了在 kBindings 里绑定，
//   他会把新点追加在**点表末尾**（点表纪律），于是 EXT 区被推后、
//   `EMS_EXT_BEGIN` 变大，而 `kBindingCount` 没变 → 这里立刻红。
static_assert(kBindingCount == static_cast<std::size_t>(EMS_EXT_BEGIN),
              "modbus 映射表必须覆盖 EXT 区之前的全部点："
              "新增了设备点却忘了在 kBindings 里绑定（未绑定的点会静默读不到）。");

// ⓪' EXT 区必须是点表的**尾巴**
//   上面那条断言依赖"EXT 在末尾"这个前提。前提本身也要钉住 ——
//   否则有人把新点插到 EXT 之后，未绑定集合就不再"恰好是 EXT 区"了，
//   而上面那条断言照样通过（数字还是对的，含义已经错了）。
//   ★ 同一事实在 C 侧（ems_point_table.c）也有一份等价守卫：
//     那里守的是"数组长度别落队"，这里守的是"点区边界别错位"。
static_assert(EMS_EXT_END == EMS_POINT_COUNT,
              "EXT 点区必须是点表末尾：把点插到 EXT 之后会让"
              "「未绑定集合 == EXT 区」这个前提失效。");

// ① 逐点覆盖、位置 == 索引、点名非空
constexpr bool bindings_well_formed() {
    for (std::size_t i = 0; i < kBindingCount; ++i) {
        if (kBindings[i].name == nullptr) return false;
        if (kBindings[i].index != i)      return false;   // 位置必须等于索引
        if (kBindings[i].name[0] == '\0') return false;
        // 位类型的缩放必须为 0（位不做量纲换算），寄存器类型的必须 > 0
        if (kBindings[i].encoding == Encoding::kBit) {
            if (kBindings[i].scale != 0.0) return false;
            if (kBindings[i].table != Table::kDiscreteInput &&
                kBindings[i].table != Table::kCoil) return false;
        } else if (kBindings[i].encoding != Encoding::kF32) {
            if (!(kBindings[i].scale > 0.0)) return false;
            if (kBindings[i].table != Table::kInputReg &&
                kBindings[i].table != Table::kHoldingReg) return false;
        }
    }
    return true;
}
static_assert(bindings_well_formed(),
              "modbus 映射表与 ems_point_table.h 的索引/编码不一致");

// ② 段的可写性由点表段决定（量测/配置/状态只读；指令可写）
constexpr bool segments_consistent() {
    for (std::size_t i = 0; i < kBindingCount; ++i) {
        const PointBinding& b = kBindings[i];
        const bool is_cmd = (i >= static_cast<std::size_t>(EMS_CMD_P_BAT) &&
                             i <= static_cast<std::size_t>(EMS_CMD_P_LOWER));
        if (is_cmd) {
            if (!b.writable)                  return false;
            if (b.table != Table::kHoldingReg) return false;
        } else {
            if (b.writable) return false;     // 量测/配置/状态一律只读
        }
        const bool is_meas = (i <= static_cast<std::size_t>(EMS_SOH));
        const bool is_cfg  = (i >= static_cast<std::size_t>(EMS_CFG_CAP_KWH) &&
                              i <= static_cast<std::size_t>(EMS_CFG_SOC_MAX));
        const bool is_sta  = (i >= static_cast<std::size_t>(EMS_STA_BMS));
        if (is_meas && b.table != Table::kInputReg)      return false;
        if (is_cfg  && b.table != Table::kInputReg)      return false;
        if (is_sta  && b.table != Table::kDiscreteInput) return false;
    }
    return true;
}
static_assert(segments_consistent(),
              "modbus 映射表的表类型/可写性与 ems_point_table.h 的段定义冲突");

// ③ 三个指令点必须在**同一张表且地址连续**
//
// 为什么这是硬要求（现场语义，不是实现便利）：
//   `write_command()` 要一次 FC16 把 (p_bat, p_upper, p_lower) 写下去。
//   若拆成三个事务，设备可能执行到"**新的功率 + 旧的权限区间**"的中间态 ——
//   那正是安全层最不能容忍的一瞬间（新指令可能已经越过了旧区间）。
//   地址连续是"能原子下发"的物理前提，所以在这里用编译期守卫钉住。
constexpr bool cmd_points_contiguous() {
    const PointBinding& a = kBindings[EMS_CMD_P_BAT];
    const PointBinding& b = kBindings[EMS_CMD_P_UPPER];
    const PointBinding& c = kBindings[EMS_CMD_P_LOWER];
    return a.table == b.table && b.table == c.table &&
           a.table == Table::kHoldingReg &&
           static_cast<std::uint16_t>(a.address + 2) == b.address &&
           static_cast<std::uint16_t>(b.address + 2) == c.address;
}
static_assert(cmd_points_contiguous(),
              "指令点必须同表且地址连续 —— 否则权限区间无法与指令原子下发");

// =====================================================================
// 分块读规划
//
// 为什么要有这个：FC03/04 单次最多 125 个寄存器，且**地址必须连续**。
// 逐点读 32 次 = 32 个往返；本表把段内地址排齐，于是：
//
//   输入寄存器 0..39   （40 个）  ← 量测 7 + 空 1 + 配置 14×2
//   保持寄存器 0..5    （ 6 个）  ← 指令 3×2（读回用于写后校验）
//   离散输入   0..7    （ 8 位）  ← 状态 8
//
//   → **3 次请求**读全 32 点。T09 直接断言这个数字：
//     谁把实现改回"一点一次"，请求次数就从 3 变成 32，立刻红。
// =====================================================================
struct ReadBlock {
    Table         table;
    std::uint16_t start;
    std::uint16_t count;   // 寄存器个数（位类型为位数）
};

constexpr ReadBlock kReadBlocks[3] = {
    { Table::kInputReg,      0, 40 },
    { Table::kHoldingReg,    0,  6 },
    { Table::kDiscreteInput, 0,  8 },
};
constexpr std::size_t kReadBlockCount = 3;

static_assert(kReadBlocks[0].count <= kMaxReadRegs, "输入寄存器分块超过 FC04 上限");
static_assert(kReadBlocks[1].count <= kMaxReadRegs, "保持寄存器分块超过 FC03 上限");
static_assert(kReadBlocks[2].count <= kMaxReadBits, "离散输入分块超过 FC02 上限");

// 分块覆盖面校验：每个点都必须落在**恰好一个**分块里。
// 漏掉一个点 = 该点永远是默认值，而这种错在运行期完全静默。
constexpr bool blocks_cover_all_points() {
    for (std::size_t i = 0; i < kBindingCount; ++i) {
        const PointBinding& b = kBindings[i];
        bool covered = false;
        for (std::size_t k = 0; k < kReadBlockCount; ++k) {
            if (kReadBlocks[k].table != b.table) continue;
            const std::uint16_t width =
                (b.encoding == Encoding::kBit) ? 1 : b.reg_count();
            if (b.address >= kReadBlocks[k].start &&
                static_cast<std::uint32_t>(b.address) + width <=
                    static_cast<std::uint32_t>(kReadBlocks[k].start) +
                        kReadBlocks[k].count) {
                covered = true;
            }
        }
        if (!covered) return false;
    }
    return true;
}
static_assert(blocks_cover_all_points(), "有分块没覆盖到的点 —— 该点会恒为默认值");

// 分块之间不重叠（重叠会导致同一个点被两个分块解码成两份，埋在后面的那份永远赢）
constexpr bool blocks_disjoint() {
    for (std::size_t a = 0; a < kReadBlockCount; ++a) {
        for (std::size_t b = a + 1; b < kReadBlockCount; ++b) {
            if (kReadBlocks[a].table != kReadBlocks[b].table) continue;
            const std::uint32_t a0 = kReadBlocks[a].start;
            const std::uint32_t a1 = a0 + kReadBlocks[a].count;
            const std::uint32_t b0 = kReadBlocks[b].start;
            const std::uint32_t b1 = b0 + kReadBlocks[b].count;
            if (a0 < b1 && b0 < a1) return false;
        }
    }
    return true;
}
static_assert(blocks_disjoint(), "分块地址区间重叠");

// =====================================================================
// 查询
// =====================================================================
inline const PointBinding& binding_for_index(std::size_t index) {
    return kBindings[index < kBindingCount ? index : 0];
}

// 按点名查找；找不到返回 nullptr（**不要**退化成"返回默认点"——
// 现场点名打错必须报出来，静默用默认点等于采了一个假值）
inline const PointBinding* binding_for_name(const char* name) {
    if (name == nullptr) return nullptr;
    for (std::size_t i = 0; i < kBindingCount; ++i) {
        if (kBindings[i].name != nullptr && std::strcmp(kBindings[i].name, name) == 0) {
            return &kBindings[i];
        }
    }
    return nullptr;
}

// 某个分块在缓冲区里的偏移（0 = 不存在）
inline std::size_t block_offset(Table t, std::size_t block_index) {
    (void)t;
    std::size_t off = 0;
    for (std::size_t k = 0; k < block_index && k < kReadBlockCount; ++k) {
        off += kReadBlocks[k].count;
    }
    return off;
}

// 解码失败的原因（**分开报**：现场排障时"值不合理"和"字节不对"处置完全不同）
enum class DecodeError {
    kNone = 0,
    kWrongTable,     // 点不在这种表里（映射表/调用方不一致）
    kNotEnoughData,  // 缓冲区长度不够
    kNonFinite,      // 浮点解出 NaN / Inf ★ 字序写错的典型症状
    kOutOfRange,     // 整数编码超出可表达范围（写回时会发生）
};

// =====================================================================
// 解码：原始寄存器/位 → 物理值（double）
//
// ★ **参数语义（很容易踩）**：`regs` / `bits` 是**整块缓冲区**，
//   下标就是点的**绝对地址** `b.address`，而 `reg_count` / `bit_count`
//   是这块缓冲区的**总长度**（不是本点的宽度）。
//   例：解 `MEAS.SOC`（IR[8]，u16）要传长度 ≥ 9 的输入寄存器缓冲区。
//   这样设计是因为调用方（modbus_device_io.h）手里本来就是整块缓冲区 ——
//   每点再切一个子数组既浪费也会掩盖"地址算错"的 bug。
//
// 本层**只做数值转换，不做业务判断** —— 不 clamp SOC 到 [0,1]，
// 不把 -300 kW 当异常。业务的合理性由 modbus_device_io.h 与 05/ 安全层管。
// 在这里顺手 clamp 会让"设备给出越界值"这件事**永远不被告知**。
//
// 失败原因**分开报**：现场排障时"值不合理"（kOutOfRange）与
// "字节不对"（kNonFinite —— 通常是字序配错）处置完全不同。
// =====================================================================
inline DecodeError decode_point(const PointBinding& b, const std::uint16_t* regs,
                                std::size_t reg_count, const bool* bits,
                                std::size_t bit_count, double* out) {
    if (out == nullptr) return DecodeError::kWrongTable;

    switch (b.encoding) {
    case Encoding::kBit: {
        if (b.table != Table::kDiscreteInput && b.table != Table::kCoil) {
            return DecodeError::kWrongTable;
        }
        if (bits == nullptr || b.address >= bit_count) return DecodeError::kNotEnoughData;
        *out = bits[b.address] ? 1.0 : 0.0;
        return DecodeError::kNone;
    }
    case Encoding::kF32: {
        if (b.table != Table::kInputReg && b.table != Table::kHoldingReg) {
            return DecodeError::kWrongTable;
        }
        if (regs == nullptr ||
            static_cast<std::size_t>(b.address) + 2 > reg_count) {
            return DecodeError::kNotEnoughData;
        }
        const float v = get_f32(regs + b.address, b.word_order);
        // ★ 这一行是现场第一大坑的守卫：字序写错时解出来的通常**不是**"差一点"
        //   的值，而是 NaN / Inf / 天文数字。放它进算法，05/ 的区间求交会得到
        //   全 NaN 的权限区间，而故障语义上"数据无效"与"数据是 NaN"完全是两回事。
        if (!std::isfinite(v)) return DecodeError::kNonFinite;
        *out = static_cast<double>(v);
        return DecodeError::kNone;
    }
    case Encoding::kU16: {
        if (b.table != Table::kInputReg && b.table != Table::kHoldingReg) {
            return DecodeError::kWrongTable;
        }
        if (regs == nullptr || b.address >= reg_count) return DecodeError::kNotEnoughData;
        *out = static_cast<double>(regs[b.address]) / b.scale;
        return DecodeError::kNone;
    }
    case Encoding::kI16: {
        if (b.table != Table::kInputReg && b.table != Table::kHoldingReg) {
            return DecodeError::kWrongTable;
        }
        if (regs == nullptr || b.address >= reg_count) return DecodeError::kNotEnoughData;
        // 有符号：位模式按 int16_t 重解释（**不是** (int)reg - 32768 那种手写偏移，
        // 那种写法在 reg 恰好是 32768 时会错）
        const std::int16_t raw = static_cast<std::int16_t>(regs[b.address]);
        *out = static_cast<double>(raw) / b.scale;
        return DecodeError::kNone;
    }
    }
    return DecodeError::kWrongTable;
}

// 编码：物理值 → 原始寄存器（写指令用）
inline DecodeError encode_point(const PointBinding& b, double value,
                                std::uint16_t* regs, std::size_t reg_count,
                                bool* bits, std::size_t bit_count) {
    switch (b.encoding) {
    case Encoding::kBit: {
        if (bits == nullptr || b.address >= bit_count) return DecodeError::kNotEnoughData;
        bits[b.address] = (value > 0.5);
        return DecodeError::kNone;
    }
    case Encoding::kF32: {
        if (regs == nullptr || static_cast<std::size_t>(b.address) + 2 > reg_count) {
            return DecodeError::kNotEnoughData;
        }
        if (!std::isfinite(value)) return DecodeError::kNonFinite;
        put_f32(regs + b.address, static_cast<float>(value), b.word_order);
        return DecodeError::kNone;
    }
    case Encoding::kU16: {
        if (regs == nullptr || b.address >= reg_count) return DecodeError::kNotEnoughData;
        if (!std::isfinite(value)) return DecodeError::kNonFinite;
        const double raw = value * b.scale;
        // 溢出**不静默饱和**：饱和会让"指令 5000 kW"变成"指令 65535"，
        // 设备照做就出事。宁可报错让上层知道这条线走不通。
        if (raw < 0.0 || raw > 65535.0) return DecodeError::kOutOfRange;
        regs[b.address] = static_cast<std::uint16_t>(raw + 0.5);
        return DecodeError::kNone;
    }
    case Encoding::kI16: {
        if (regs == nullptr || b.address >= reg_count) return DecodeError::kNotEnoughData;
        if (!std::isfinite(value)) return DecodeError::kNonFinite;
        const double raw = value * b.scale;
        if (raw < -32768.0 || raw > 32767.0) return DecodeError::kOutOfRange;
        regs[b.address] = static_cast<std::uint16_t>(static_cast<std::int16_t>(raw < 0 ? raw - 0.5 : raw + 0.5));
        return DecodeError::kNone;
    }
    }
    return DecodeError::kWrongTable;
}

inline const char* decode_error_name(DecodeError e) {
    switch (e) {
    case DecodeError::kNone:          return "ok";
    case DecodeError::kWrongTable:    return "wrong table";
    case DecodeError::kNotEnoughData: return "not enough data";
    case DecodeError::kNonFinite:     return "non-finite value";
    case DecodeError::kOutOfRange:    return "value out of range";
    }
    return "unknown";
}

// =====================================================================
// 映射表自检（运行期；与 static_assert 互补）
//
// static_assert 保证**本文件内部**自洽（位置/段/分块），但保证不了
// "本文件的点名与 ems_point_table.h 的点名逐字相同" —— 那是跨翻译单元的
// 字符串比较，只能在运行期做。现场配错了点表就是这个函数报出来。
// 返回不一致的点数，0 = 完全一致。
// =====================================================================
inline int self_check() {
    int bad = 0;
    for (std::size_t i = 0; i < kBindingCount; ++i) {
        const PointBinding& b = kBindings[i];
        const char* truth = EMS_POINT_NAMES[i];
        if (b.name == nullptr || truth == nullptr) { ++bad; continue; }
        if (std::strcmp(b.name, truth) != 0) { ++bad; continue; }
        // 反查必须回到同一个索引（有重复点名时这条会红）
        const PointBinding* back = binding_for_name(truth);
        if (back == nullptr || back->index != i) { ++bad; }
    }
    return bad;
}

// 覆盖的点数（应当等于 EMS_EXT_BEGIN —— EXT 区不经设备总线，故不绑定）
inline std::size_t mapped_point_count() {
    std::size_t n = 0;
    for (std::size_t i = 0; i < kBindingCount; ++i) {
        if (kBindings[i].name != nullptr) ++n;
    }
    return n;
}

// 读全全部点所需的请求次数（= 分块数）。T09 断言它 == 3。
inline std::size_t full_scan_request_count() { return kReadBlockCount; }

// 人类可读的一行（日志/自检用）
inline std::string describe_binding(const PointBinding& b) {
    char buf[160];
    const char* enc = (b.encoding == Encoding::kF32) ? "f32"
                    : (b.encoding == Encoding::kU16) ? "u16"
                    : (b.encoding == Encoding::kI16) ? "i16" : "bit";
    const char* tbl = (b.table == Table::kInputReg)      ? "IR"
                    : (b.table == Table::kHoldingReg)    ? "HR"
                    : (b.table == Table::kCoil)          ? "CO" : "DI";
    std::snprintf(buf, sizeof(buf), "%-22s %s[%u] %s%s%s", b.name, tbl,
                  static_cast<unsigned>(b.address), enc,
                  (b.encoding == Encoding::kF32 &&
                   b.word_order == WordOrder::kLowWordFirst) ? " (word-swap)" : "",
                  b.writable ? " rw" : " ro");
    return buf;
}

} // namespace modbus
} // namespace ems
