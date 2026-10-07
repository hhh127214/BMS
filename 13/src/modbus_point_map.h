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
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>

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
// 运行期点表 —— 现场配置那一层
//
// 上面那张 kBindings 是**编译期**的默认表：默认装配零成本，且有 4 条
// static_assert 守着。但现场点表**由客户/调试工程师提供**——设备厂家给出的
// 是自己的地址规划，不可能为了改一个寄存器地址去重编 C++（现场常常没有
// 编译环境，改完还得重跑一遍测试）。
//
// 于是拆成两层，各管各的：
//   kBindings    —— 编译期默认表。本文件所有 static_assert 只管它；
//                   不加载任何配置时，运行期行为与它**逐位相同**。
//   active_map() —— 运行期活动表。初值从 kBindings 拷贝，可被 CSV 覆盖。
//
// ★ 两条纪律（P3/docs/design.md §6 有同款设计，那边已经踩过）：
//   ① 加载失败**必须回退**默认表，并把出错行号与原因报出来——
//      绝不能"加载了一半就开始跑"。半个点表比没有点表更危险：
//      它会让一部分点读到隔壁设备的寄存器，而每个值看起来都正常。
//   ② 加载进来的每一行都要过**与 static_assert 同一套规则**的校验——
//      编译期守卫拦不住 CSV，必须有一条等价的运行期路径。
//
// ★ 关于分块：kReadBlocks 是编译期常量，但它是**从地址推导出来的**
//   （见上面的地址规划）。地址一变，固定分块就错了，而且是"读到了、但读的是
//   隔壁寄存器"这种静默错。所以分块必须跟着活动表**重新推导**。
// =====================================================================

// 派生分块时的合并阈值：同表内两个点之间空几个寄存器，还值得并进同一次请求。
//
// 为什么不能是 0（"必须严格相邻才合并"）：默认表 IR 段的第 11 号是**刻意留的
// 空位**（见上面的地址规划注释），gap = 1。若不允许合并，默认表会被拆成 4 块，
// "读全 32 点 = 3 次请求"这条契约当场失效（T09/T22 直接断言这个数字）。
//
// 取 16 的取舍：超过它说明两个点在设备侧确实离得远（多半是两段不同的数据区），
// 硬合并会把中间一堆无关寄存器一起读回来，反而拖慢一帧。
constexpr std::uint16_t kMergeGap = 16;

// 分块数上限。默认表 3 块；现场点表散一些也不会超过这个数。
// 超上限直接判非法——与其默默拼出几十次请求，不如让配置人知道表排得太散。
constexpr std::size_t kMaxReadBlocks = 16;

// 活动表 + 活动分块 + 来源信息。
// 来源要留着：排障时"现在跑的到底是哪张表"永远是第一个要回答的问题。
struct PointMap {
    PointBinding items[EMS_EXT_BEGIN]{};
    std::size_t  count = EMS_EXT_BEGIN;   // 恒等于 EMS_EXT_BEGIN（加载时校验行数）
    ReadBlock    blocks[kMaxReadBlocks]{};
    std::size_t  block_count = 0;
    bool         from_csv    = false;   // false = 内置默认表
    char         source[260]{};         // CSV 路径（from_csv 为真时有效）
};

constexpr std::uint16_t block_limit_of(Table t) {
    return (t == Table::kDiscreteInput || t == Table::kCoil) ? kMaxReadBits
                                                             : kMaxReadRegs;
}

// 从点表推导读分块。
// 规则：按表分组 → 表内按地址升序 → 贪心合并（间隙 ≤ kMergeGap 且不超该表上限）。
// 失败（地址重叠 / 分块过散）返回 false 并写 why。
inline bool derive_read_blocks(PointMap& m, std::string* why) {
    m.block_count = 0;
    // 表的处理顺序决定块序。保持"输入寄存器 → 保持寄存器 → 离散输入"与默认表
    // kReadBlocks 一致，这样默认表推导出来的块序不会变。
    const Table order[4] = { Table::kInputReg, Table::kHoldingReg,
                             Table::kDiscreteInput, Table::kCoil };

    for (int ti = 0; ti < 4; ++ti) {
        const Table t = order[ti];

        // 本表的点索引，按 address 升序。点数很小（≤ 40）且顺序基本已排好，
        // 用插入排序：不做无谓的 <algorithm> 依赖，也不会成为热点。
        std::size_t idx[EMS_EXT_BEGIN];
        std::size_t n = 0;
        for (std::size_t i = 0; i < m.count; ++i) {
            if (m.items[i].table == t) idx[n++] = i;
        }
        for (std::size_t a = 1; a < n; ++a) {
            const std::size_t key = idx[a];
            std::size_t b = a;
            while (b > 0 && m.items[idx[b - 1]].address > m.items[key].address) {
                idx[b] = idx[b - 1];
                --b;
            }
            idx[b] = key;
        }

        const std::uint32_t lim = block_limit_of(t);
        bool      open = false;
        ReadBlock cur{ t, 0, 0 };

        for (std::size_t a = 0; a < n; ++a) {
            const PointBinding& p = m.items[idx[a]];
            const std::uint32_t w = (p.encoding == Encoding::kBit) ? 1u : p.reg_count();

            if (!open) {
                cur  = ReadBlock{ t, p.address, static_cast<std::uint16_t>(w) };
                open = true;
                continue;
            }

            const std::uint32_t end = static_cast<std::uint32_t>(cur.start) + cur.count;
            if (p.address < end) {
                if (why) {
                    *why = std::string("点 ") + p.name +
                           " 的地址落在同表前一个点的区间内（地址重叠）";
                }
                return false;
            }
            const std::uint32_t gap  = p.address - end;
            const std::uint32_t span = end + gap + w - cur.start;

            if (gap <= kMergeGap && span <= lim) {
                cur.count = static_cast<std::uint16_t>(span);
            } else {
                if (m.block_count >= kMaxReadBlocks) {
                    if (why) *why = "分块数超过上限（点表排得太散，一次快照要发太多次请求）";
                    return false;
                }
                m.blocks[m.block_count++] = cur;
                cur = ReadBlock{ t, p.address, static_cast<std::uint16_t>(w) };
            }
        }
        if (open) {
            if (m.block_count >= kMaxReadBlocks) {
                if (why) *why = "分块数超过上限（点表排得太散，一次快照要发太多次请求）";
                return false;
            }
            m.blocks[m.block_count++] = cur;
        }
    }
    return true;
}

// 内置默认表：从 kBindings 拷一份并推导分块。
// ★ 这里推导出来的块必须与 kReadBlocks **逐字段相等**。T48 直接断言这件事——
//   它是"两层结构没有偷偷改变默认行为"的正面证据，而不是靠人眼比对。
inline PointMap make_default_map() {
    PointMap m;
    m.count = EMS_EXT_BEGIN;
    for (std::size_t i = 0; i < EMS_EXT_BEGIN; ++i) m.items[i] = kBindings[i];
    std::string why;
    derive_read_blocks(m, &why);
    return m;
}

inline PointMap& active_map() {
    static PointMap m = make_default_map();
    return m;
}

inline const PointMap& active_point_map()  { return active_map(); }
inline const ReadBlock* active_blocks()     { return active_map().blocks; }
inline std::size_t      active_block_count() { return active_map().block_count; }
inline std::size_t      active_binding_count() { return active_map().count; }

// 恢复内置默认表（放弃已加载的 CSV）。
inline void reset_point_map() { active_map() = make_default_map(); }

// =====================================================================
// 运行期校验 —— 与 static_assert 同一套规则，只是搬到了运行期
//
// 为什么必须有一份运行期的：CSV 加载时编译器帮不上忙。而"CSV 里少一行"、
// "字序写成 abcd"这一类错误，表现出来都是**读到了一个看似正常的数值**，
// 不校验就一定会静默地跑在错表上。
// =====================================================================
inline bool validate_map(const PointMap& m, std::string* why) {
    auto fail = [&](const std::string& s) { if (why) *why = s; return false; };

    if (m.count != static_cast<std::size_t>(EMS_EXT_BEGIN)) {
        return fail("点数不符：应为 " + std::to_string(EMS_EXT_BEGIN) + " 行");
    }

    for (std::size_t i = 0; i < m.count; ++i) {
        const PointBinding& b = m.items[i];
        const char* truth = EMS_POINT_NAMES[i];

        // ① 位置 == 索引，点名逐字等于点表真相源。
        //    这不是形式主义：点位靠索引寻址，插错一行后面会全体错位。
        if (b.index != i) {
            return fail("第 " + std::to_string(i + 1) + " 行的索引不对（应为 " +
                        std::to_string(i) + "）");
        }
        if (truth == nullptr || b.name == nullptr || std::strcmp(b.name, truth) != 0) {
            return fail("第 " + std::to_string(i + 1) + " 行的点名应为 " +
                        (truth ? truth : "?") + "，实际是 " + (b.name ? b.name : "(空)"));
        }

        // ② 编码与表类型必须匹配；缩放规则随编码而定
        if (b.encoding == Encoding::kBit) {
            if (b.table != Table::kDiscreteInput && b.table != Table::kCoil) {
                return fail(std::string("点 ") + b.name +
                            " 是位类型，只能落在离散输入或线圈表");
            }
            if (b.scale != 0.0) {
                return fail(std::string("点 ") + b.name + " 是位类型，缩放必须为 0");
            }
        } else if (b.encoding == Encoding::kF32) {
            if (b.table != Table::kInputReg && b.table != Table::kHoldingReg) {
                return fail(std::string("点 ") + b.name +
                            " 是浮点，只能落在输入寄存器或保持寄存器表");
            }
        } else {
            if (!(b.scale > 0.0)) {
                return fail(std::string("点 ") + b.name +
                            " 的缩放必须大于 0（0 会在换算时除零）");
            }
            if (b.table != Table::kInputReg && b.table != Table::kHoldingReg) {
                return fail(std::string("点 ") + b.name +
                            " 是整数编码，只能落在输入寄存器或保持寄存器表");
            }
        }

        // ③ 段的可写性由点表段决定（量测/配置/状态只读；指令可写）
        const bool is_cmd = (i >= static_cast<std::size_t>(EMS_CMD_P_BAT) &&
                             i <= static_cast<std::size_t>(EMS_CMD_P_LOWER));
        if (is_cmd) {
            if (!b.writable) return fail(std::string("指令点 ") + b.name + " 必须可写");
            if (b.table != Table::kHoldingReg) {
                return fail(std::string("指令点 ") + b.name + " 必须在保持寄存器表");
            }
        } else if (b.writable) {
            return fail(std::string("点 ") + b.name + " 属于只读段，不能标为可写");
        }
    }

    // ④ 指令三点必须同表且地址连续 —— 原子下发的前提（见上面的 static_assert）
    const PointBinding& ca = m.items[EMS_CMD_P_BAT];
    const PointBinding& cb = m.items[EMS_CMD_P_UPPER];
    const PointBinding& cc = m.items[EMS_CMD_P_LOWER];
    if (!(ca.table == cb.table && cb.table == cc.table && ca.table == Table::kHoldingReg &&
          static_cast<std::uint16_t>(ca.address + 2) == cb.address &&
          static_cast<std::uint16_t>(cb.address + 2) == cc.address)) {
        return fail("指令点必须同表且地址连续（否则权限区间无法与指令原子下发）");
    }

    // ⑤ 分块必须覆盖到每一个点。漏一个点 = 该点恒为默认值，
    //    而这种错在运行期完全静默（值看着就是个正常数字）。
    for (std::size_t i = 0; i < m.count; ++i) {
        const PointBinding& p = m.items[i];
        const std::uint32_t w = (p.encoding == Encoding::kBit) ? 1u : p.reg_count();
        bool covered = false;
        for (std::size_t k = 0; k < m.block_count; ++k) {
            if (m.blocks[k].table != p.table) continue;
            if (p.address >= m.blocks[k].start &&
                static_cast<std::uint32_t>(p.address) + w <=
                    static_cast<std::uint32_t>(m.blocks[k].start) + m.blocks[k].count) {
                covered = true;
            }
        }
        if (!covered) {
            return fail(std::string("点 ") + p.name +
                        " 没有被任何分块覆盖（该点会恒为默认值）");
        }
    }
    return true;
}

// =====================================================================
// CSV 读写 —— 配置的交换格式
//
// 列：name,table,address,encoding,word_order,scale,writable,unit,note
//   name       点名（必须与点表真相源逐字相同；行序即索引，不许乱序）
//   table      IR / HR / DI / CO
//   address    起始地址（十进制）
//   encoding   f32 / u16 / i16 / bit
//   word_order high(ABCD) / low(CDAB)——仅 f32 有意义，其它填空即可
//   scale      u16/i16 的缩放（物理值 = raw / scale）；bit 填 0；f32 忽略
//   writable   rw / ro
//   unit/note  仅信息，不参与解析（方便对着厂家手册看）
//
// ★ 为什么是 CSV 而不是数据库：13/ 的纪律是**只依赖 WinSock**（见 build.bat
//   的说明）。CSV 零依赖、可人工编辑（现场应急改一行不用起任何工具）、
//   也能进版本库对比。P3 那条线已用同一套思路落地（load_point_map_csv）。
// =====================================================================
inline std::string trim_copy(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' ||
                     s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

inline std::string upper_copy(const std::string& s) {
    std::string u;
    u.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        u += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }
    return u;
}

constexpr std::size_t kMaxCsvCols = 16;
struct CsvRow {
    std::string col[kMaxCsvCols];
    std::size_t n = 0;
};

// 切一行。支持双引号包裹（note 列可能含逗号），"" 表示一个字面引号。
inline void split_csv_line(const std::string& line, CsvRow& out) {
    out.n = 0;
    std::string cur;
    bool inQuote = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (inQuote) {
            if (ch == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
                else inQuote = false;
            } else {
                cur += ch;
            }
        } else if (ch == '"') {
            inQuote = true;
        } else if (ch == ',') {
            if (out.n < kMaxCsvCols) out.col[out.n++] = cur;
            cur.clear();
        } else if (ch != '\r' && ch != '\n') {
            cur += ch;
        }
    }
    if (out.n < kMaxCsvCols) out.col[out.n++] = cur;
}

// 无符号十进制解析。**不用 atoi**：atoi 对 "abc" 返回 0 且不报错，
// 那正好是"地址字段写错却静默变成地址 0"的经典事故。
inline bool parse_u16(const std::string& s, std::uint16_t* out) {
    if (s.empty()) return false;
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10u + static_cast<std::uint32_t>(s[i] - '0');
        if (v > 65535u) return false;
    }
    *out = static_cast<std::uint16_t>(v);
    return true;
}

inline bool parse_scale(const std::string& s, double* out) {
    if (s.empty()) return false;
    const char* p = s.c_str();
    char* end = nullptr;
    const double v = std::strtod(p, &end);
    if (end == p || (end != nullptr && *end != '\0')) return false;
    *out = v;
    return true;
}

inline bool parse_table(const std::string& s, Table* out) {
    const std::string u = upper_copy(s);
    if (u == "IR" || u == "INPUT_REG" || u == "INPUTREG") { *out = Table::kInputReg;      return true; }
    if (u == "HR" || u == "HOLDING_REG" || u == "HOLDINGREG") { *out = Table::kHoldingReg; return true; }
    if (u == "DI" || u == "DISCRETE_INPUT")               { *out = Table::kDiscreteInput; return true; }
    if (u == "CO" || u == "COIL")                         { *out = Table::kCoil;         return true; }
    return false;
}

inline bool parse_encoding(const std::string& s, Encoding* out) {
    const std::string u = upper_copy(s);
    if (u == "F32" || u == "FLOAT")        { *out = Encoding::kF32; return true; }
    if (u == "U16")                        { *out = Encoding::kU16; return true; }
    if (u == "I16" || u == "S16" || u == "INT16") { *out = Encoding::kI16; return true; }
    if (u == "BIT" || u == "BOOL")         { *out = Encoding::kBit; return true; }
    return false;
}

inline bool parse_word_order(const std::string& s, WordOrder* out) {
    const std::string u = upper_copy(s);
    if (u.empty() || u == "HIGH" || u == "ABCD" || u == "BIG")    { *out = WordOrder::kHighWordFirst; return true; }
    if (u == "LOW" || u == "CDAB" || u == "LITTLE" || u == "SWAP") { *out = WordOrder::kLowWordFirst;  return true; }
    return false;
}

inline bool parse_writable(const std::string& s, bool* out) {
    const std::string u = upper_copy(s);
    if (u == "RW" || u == "1" || u == "TRUE" || u == "W")  { *out = true;  return true; }
    if (u == "RO" || u == "0" || u == "FALSE" || u == "R") { *out = false; return true; }
    return false;
}

// 下面三个取名 `*_code` 而不是 `*_name`：13/tests/test_modbus_bridge.cpp 里
// 已有同名的静态辅助函数（它自己那份 CSV 对照用），而它在同一个 namespace 里。
// 重名的代价是**编译期歧义**（两边都能匹配），所以这里用不同的名字划清界限 ——
// 不动测试，也不靠 using 去消歧。
inline const char* table_code(Table t) {
    switch (t) {
    case Table::kInputReg:      return "IR";
    case Table::kHoldingReg:    return "HR";
    case Table::kDiscreteInput: return "DI";
    case Table::kCoil:          return "CO";
    }
    return "?";
}

inline const char* encoding_code(Encoding e) {
    switch (e) {
    case Encoding::kF32: return "f32";
    case Encoding::kU16: return "u16";
    case Encoding::kI16: return "i16";
    case Encoding::kBit: return "bit";
    }
    return "?";
}

inline const char* word_order_code(WordOrder w) {
    return (w == WordOrder::kLowWordFirst) ? "low" : "high";
}

// 导出当前表为 CSV。导出 → 人工填 → 加载回来，这条路是**双向**的，
// 所以导出的东西可以直接当模板发给客户或调试工程师。
inline std::string export_point_map_csv(const PointMap& m) {
    std::string out;
    out.reserve(4096);
    out += "name,table,address,encoding,word_order,scale,writable,unit,note\n";
    for (std::size_t i = 0; i < m.count; ++i) {
        const PointBinding& b = m.items[i];
        char sc[32];
        if (b.encoding == Encoding::kBit) std::snprintf(sc, sizeof(sc), "0");
        else                              std::snprintf(sc, sizeof(sc), "%g", b.scale);
        char line[320];
        std::snprintf(line, sizeof(line), "%s,%s,%u,%s,%s,%s,%s,,\n",
                      b.name ? b.name : "",
                      table_code(b.table),
                      static_cast<unsigned>(b.address),
                      encoding_code(b.encoding),
                      word_order_code(b.word_order),
                      sc,
                      b.writable ? "rw" : "ro");
        out += line;
    }
    return out;
}

// 从 CSV 加载点表。
//
// 失败时：活动表**一个字节都不改**（仍然跑默认表或上一次成功的表），
// 并把出错行号与原因写进 report。返回值只表示"这次加载成不成功"。
//
// ★ 这里刻意不提供"部分加载"的选项。现场最怕的不是加载失败（失败会报错、
//   会退默认表，行为可预期），而是**加载了一半**：一半的点读新地址、
//   一半读旧地址，两边都能读出"看起来正常"的值。
inline bool load_point_map_csv(const char* path, std::string* report) {
    auto fail = [&](const std::string& s) { if (report) *report = s; return false; };

    if (path == nullptr || path[0] == '\0') return fail("点表路径为空");

    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return fail(std::string("打不开点表文件：") + path);

    PointMap m;
    m.count = EMS_EXT_BEGIN;
    // 先用默认表填底：CSV 没覆盖到的字段不会留下垃圾值。
    // 注意这只是**局部变量**的初值，不是"部分生效"——全部校验通过后才替换活动表。
    for (std::size_t i = 0; i < EMS_EXT_BEGIN; ++i) m.items[i] = kBindings[i];

    char        buf[1024];
    char        msg[512];
    int         lineNo = 0;
    std::size_t row = 0;
    bool        headerSeen = false;

    while (std::fgets(buf, sizeof(buf), f) != nullptr) {
        ++lineNo;
        std::string line = trim_copy(std::string(buf));
        // Excel 另存 CSV 会带 UTF-8 BOM，剥掉它而不是跳过整行——
        // 跳过整行会把"无表头"的表的第一行数据一起吃掉，导致少一行。
        if (line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line = line.substr(3);
        }
        if (line.empty()) continue;
        if (line[0] == '#') continue;          // 注释行

        CsvRow r;
        split_csv_line(line, r);
        if (r.n == 0) continue;

        // 表头（首列是 name/point/点名）跳过；也允许完全没有表头的表
        if (!headerSeen) {
            headerSeen = true;
            const std::string c0 = upper_copy(trim_copy(r.col[0]));
            if (c0 == "NAME" || c0 == "POINT" || r.col[0] == "点名") continue;
        }

        if (row >= static_cast<std::size_t>(EMS_EXT_BEGIN)) {
            std::snprintf(msg, sizeof(msg),
                          "第 %d 行：点表最多 %d 行，多出来的行会被静默丢弃 → 拒绝",
                          lineNo, EMS_EXT_BEGIN);
            std::fclose(f);
            return fail(msg);
        }
        if (r.n < 7) {
            std::snprintf(msg, sizeof(msg),
                          "第 %d 行：至少要有 7 列"
                          "（name,table,address,encoding,word_order,scale,writable），实际 %u 列",
                          lineNo, static_cast<unsigned>(r.n));
            std::fclose(f);
            return fail(msg);
        }

        PointBinding& b = m.items[row];
        b.index = row;

        const std::string nm = trim_copy(r.col[0]);
        const char* truth = EMS_POINT_NAMES[row];
        if (truth == nullptr || nm != truth) {
            std::snprintf(msg, sizeof(msg),
                          "第 %d 行：点名应为 %s，实际是 %s（行序即索引，不能乱序）",
                          lineNo, truth ? truth : "?", nm.c_str());
            std::fclose(f);
            return fail(msg);
        }
        b.name = truth;   // 指回真相源的静态字符串，生命周期无需操心

        if (!parse_table(trim_copy(r.col[1]), &b.table)) {
            std::snprintf(msg, sizeof(msg), "第 %d 行：表类型应为 IR/HR/DI/CO，实际是 %s",
                          lineNo, trim_copy(r.col[1]).c_str());
            std::fclose(f);
            return fail(msg);
        }
        if (!parse_u16(trim_copy(r.col[2]), &b.address)) {
            std::snprintf(msg, sizeof(msg), "第 %d 行：地址应为十进制整数 0..65535，实际是 %s",
                          lineNo, trim_copy(r.col[2]).c_str());
            std::fclose(f);
            return fail(msg);
        }
        if (!parse_encoding(trim_copy(r.col[3]), &b.encoding)) {
            std::snprintf(msg, sizeof(msg), "第 %d 行：编码应为 f32/u16/i16/bit，实际是 %s",
                          lineNo, trim_copy(r.col[3]).c_str());
            std::fclose(f);
            return fail(msg);
        }
        if (!parse_word_order(trim_copy(r.col[4]), &b.word_order)) {
            std::snprintf(msg, sizeof(msg),
                          "第 %d 行：字序应为 high/low（或 ABCD/CDAB），实际是 %s",
                          lineNo, trim_copy(r.col[4]).c_str());
            std::fclose(f);
            return fail(msg);
        }
        if (!parse_scale(trim_copy(r.col[5]), &b.scale)) {
            std::snprintf(msg, sizeof(msg), "第 %d 行：缩放应为数值，实际是 %s",
                          lineNo, trim_copy(r.col[5]).c_str());
            std::fclose(f);
            return fail(msg);
        }
        if (!parse_writable(trim_copy(r.col[6]), &b.writable)) {
            std::snprintf(msg, sizeof(msg), "第 %d 行：可写性应为 rw/ro，实际是 %s",
                          lineNo, trim_copy(r.col[6]).c_str());
            std::fclose(f);
            return fail(msg);
        }

        // 列 7/8（unit/note）只作人眼对照，不参与解析——刻意不校验，
        // 免得现场为了写一句备注还要翻译我们的术语。
        ++row;
    }
    std::fclose(f);

    if (row != static_cast<std::size_t>(EMS_EXT_BEGIN)) {
        std::snprintf(msg, sizeof(msg),
                      "点表只有 %u 行，应为 %d 行（少一行 = 该点恒为默认值，静默）",
                      static_cast<unsigned>(row), EMS_EXT_BEGIN);
        return fail(msg);
    }

    std::string err;
    if (!derive_read_blocks(m, &err)) return fail("分块推导失败：" + err);
    if (!validate_map(m, &err))       return fail("点表校验失败：" + err);

    m.from_csv = true;
    std::snprintf(m.source, sizeof(m.source), "%s", path);

    // ★ 全部通过之后才替换活动表 —— 上面任何一条失败路径都没碰过它。
    active_map() = m;
    if (report) *report = "ok";
    return true;
}

// =====================================================================
// 约定默认路径（配置通道①：约定文件路径）
//
// 背景：客户在 15/ 界面填完点表 → 14/ 落盘 → 13/ 启动时读它。
//   两边靠一套**双方都能自己推算出来**的路径对齐 —— 不靠命令行传参、
//   不靠文档交代、不靠"现场记得住"。
//
// 目录的取法（优先级从高到低）：
//   1. 环境变量 EMS_POINT_MAP_DIR
//   2. <exe 所在目录>/../../config/point-map
//      本模块的 exe 落在 13/build/ 下，所以推算到 <项目根>/config/point-map
//
// 文件名固定 active.csv。为什么不按设备号取：一次装配 = 一组接入参数 +
//   一份点表（见 14/schema.sql 里 device_conn 的注释），所以"当前启用的那一份"
//   是确定的；将来多设备并存时，仍可用 --load-map 显式指定某台的 <设备号>.csv。
//
// 与 14/src/pointmap.py 的 map_dir() 必须保持一致 —— 这是两侧唯一的约定。
// =====================================================================
inline bool file_exists(const char* path) {
    if (path == nullptr) return false;
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    std::fclose(f);
    return true;
}

inline std::string default_point_map_dir() {
    const char* env = std::getenv("EMS_POINT_MAP_DIR");
    if (env != nullptr && *env != '\0') return std::string(env);
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    const DWORD n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string exe(buf, static_cast<std::size_t>(n));
    const std::size_t slash = exe.find_last_of("\\/");
    const std::string dir =
        (slash == std::string::npos) ? std::string(".") : exe.substr(0, slash);
    return dir + "\\..\\..\\config\\point-map";
#else
    return std::string("config/point-map");
#endif
}

inline std::string path_in_dir(const std::string& dir) {
#ifdef _WIN32
    return dir + "\\active.csv";
#else
    return dir + "/active.csv";
#endif
}

inline std::string default_point_map_path() {
    return path_in_dir(default_point_map_dir());
}

// 加载结果的来源，供调用方如实打印（现场排障第一眼就看这行）
enum class MapSource {
    kBuiltin,      // 编译期内置默认表（没有现场点表时）
    kDefaultPath,  // 从约定默认路径读到的
    kExplicit,     // --load-map 命令行显式指定的
};

inline const char* map_source_name(MapSource s) {
    switch (s) {
    case MapSource::kBuiltin:     return "内置默认表";
    case MapSource::kDefaultPath: return "约定路径";
    case MapSource::kExplicit:    return "命令行指定";
    }
    return "?";
}

// 按约定加载点表。
//
// ★ 三种情形刻意区别对待，这是本函数唯一需要小心的地方：
//   ① 默认路径的文件**不存在** → 不是错误。用内置默认表继续，
//      现场还没配点表时程序必须能起来（这是"开箱可用"的底线）。
//   ② 默认路径的文件**存在但非法** → **硬失败**。这里绝不能静默回退到内置表：
//      那意味着"平台写错了表"和"平台没写表"在现场表现一模一样 ——
//      而前者会让人以为配置生效了，实际跑在另一张表上。
//   ③ --load-map 指定的文件不存在/非法 → 硬失败（调用方本来就显式要求了）。
//
// 拆成 _at(dir, ...) 与无参版：前者让测试能直接指定目录，
// 不必去改环境变量、也不必在测试代码里造目录。
inline bool load_point_map_default_at(const std::string& dir, std::string* report,
                                      MapSource* src, std::string* used_path) {
    const std::string p = path_in_dir(dir);
    if (used_path) *used_path = p;

    if (!file_exists(p.c_str())) {
        if (report) {
            *report = "约定路径下没有点表文件（" + p +
                      "），使用内置默认表。若平台已下发点表，请检查 EMS_POINT_MAP_DIR "
                      "与 14/ 的落盘目录是否一致。";
        }
        if (src) *src = MapSource::kBuiltin;
        return true;   // ① 不是错误
    }

    std::string rep;
    if (!load_point_map_csv(p.c_str(), &rep)) {
        if (report) *report = "约定路径下的点表非法：" + rep;
        // ★ 失败时**不写** *src：本次什么都没加载，活动表仍停在调用方原来的那份。
        //   若在这里塞一个 kBuiltin，就会谎报"用的是内置表"。
        return false;  // ② 硬失败
    }
    if (src) *src = MapSource::kDefaultPath;
    if (report) *report = "ok";
    return true;
}

inline bool load_point_map_default(std::string* report, MapSource* src,
                                   std::string* used_path) {
    return load_point_map_default_at(default_point_map_dir(), report, src, used_path);
}

// =====================================================================
// 查询（全部读**活动表**，不再读编译期常量）
//
// 为什么不直接读 kBindings：CSV 加载之后两者就不一样了。测试代码里直接索引
// kBindings 是安全的（默认装配下两者逐位相同），但**产品路径必须走这里**，
// 否则加载了点表却不生效 —— 那正是本项目最忌讳的"看起来在工作"。
// =====================================================================
inline const PointBinding& binding_for_index(std::size_t index) {
    const PointMap& m = active_map();
    return m.items[index < m.count ? index : 0];
}

// 按点名查找；找不到返回 nullptr（**不要**退化成"返回默认点"——
// 现场点名打错必须报出来，静默用默认点等于采了一个假值）
inline const PointBinding* binding_for_name(const char* name) {
    if (name == nullptr) return nullptr;
    const PointMap& m = active_map();
    for (std::size_t i = 0; i < m.count; ++i) {
        if (m.items[i].name != nullptr && std::strcmp(m.items[i].name, name) == 0) {
            return &m.items[i];
        }
    }
    return nullptr;
}

// 某个分块在缓冲区里的偏移（0 = 不存在）
inline std::size_t block_offset(Table t, std::size_t block_index) {
    (void)t;
    const PointMap& m = active_map();
    std::size_t off = 0;
    for (std::size_t k = 0; k < block_index && k < m.block_count; ++k) {
        off += m.blocks[k].count;
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
//
// 检查的是**活动表**而不是 kBindings：CSV 加载过之后两者不同，
// 而"现在跑的表对不对"才是运维要问的问题。
// =====================================================================
inline int self_check() {
    const PointMap& m = active_map();
    int bad = 0;
    for (std::size_t i = 0; i < m.count; ++i) {
        const PointBinding& b = m.items[i];
        const char* truth = EMS_POINT_NAMES[i];
        if (b.name == nullptr || truth == nullptr) { ++bad; continue; }
        if (std::strcmp(b.name, truth) != 0) { ++bad; continue; }
        if (b.index != i) { ++bad; continue; }        // 位置必须等于索引
        // 反查必须回到同一个索引（有重复点名时这条会红）
        const PointBinding* back = binding_for_name(truth);
        if (back == nullptr || back->index != i) { ++bad; }
    }
    return bad;
}

// 覆盖的点数（应当等于 EMS_EXT_BEGIN —— EXT 区不经设备总线，故不绑定）
inline std::size_t mapped_point_count() {
    const PointMap& m = active_map();
    std::size_t n = 0;
    for (std::size_t i = 0; i < m.count; ++i) {
        if (m.items[i].name != nullptr) ++n;
    }
    return n;
}

// 读全全部点所需的请求次数（= 活动分块数）。
// 默认表是 3；加载了散一些的现场点表后会变大 —— 这正是要暴露给运维的数字。
inline std::size_t full_scan_request_count() { return active_map().block_count; }

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
