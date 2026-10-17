// =====================================================================
// 21/ — DL/T645 点表绑定（EMS 抽象点 → DI 的**唯一真相源**）
//
// 与 13/src/modbus_point_map.h 同构，但有一个本质差别要说清楚：
//
//   Modbus 一张 active.csv 可以绑满 32 个点（一台网关聚合了 BMS/PCS/电表）。
//   一块 645 电表**只知道自己测的那一路** —— 它答不出 SOC、答不出禁放位、
//   也答不出分路负荷。所以本表的默认绑定**刻意只有一行**：
//
//       EMS_P_GRID ← 0x02030000 瞬时总有功功率（关口）
//
//   这与 17/src/point_mapping.h 的口径一致：
//     EMS_P_LOAD ← MEAS.METER.P_LOAD（分路计量，**另一块表**的事）
//     EMS_P_PV   ← MEAS.METER.P_PV （同上）
//   单块关口表给不出分路，**不编来源** —— 编了就是"看起来有数据"。
//   现场若要分路，属"多表/多实例"范畴（见 21/docs/README.md 未做项）。
//
// ---------------------------------------------------------------------
// 三条编译期/运行期纪律（self_check 逐条守着）：
//   ① index 必须落在设备点区（< EMS_EXT_BEGIN）—— EXT 是调度侧的点，不经设备总线；
//   ② name 必须与 EMS_POINT_NAMES[index] 逐字一致 —— 抽象表枚举变了这里必须红；
//   ③ DI 必须在内置 DI 表里找得到（dlt645_codec.h 的 builtin_specs）——
//      现场新增点位走外置 DI 表，不许往这里塞"本地查无此项"的 DI。
// =====================================================================

#pragma once

#include "dlt645_codec.h"      // 21/  DI 定义 + Scale（变比）
#include "ems_point_table.h"   // 07/  40 点抽象表（只读引用，不改）

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ems {
namespace meter645 {

// ---------------------------------------------------------------------
// 一条绑定：EMS 抽象点 ← 645 DI
// ---------------------------------------------------------------------
struct PointBinding {
    std::size_t     index;   // ems_point_table.h 的枚举值
    const char*     name;    // 必须与 EMS_POINT_NAMES[index] 逐字相同
    std::uint32_t   di;      // 数据标识（显示序，如 0x02030000）
    dlt645::Scale   scale;   // PT/CT 变比；默认 {1,0} = 不改变读数
};

// ---------------------------------------------------------------------
// 默认绑定表
//
// ★ 只有一行的理由见文件头。将来现场出现"一台表多个馈线 DI"的表型，
//   往这里加行即可，但每一行都必须过 self_check 的三条判据。
// ---------------------------------------------------------------------
constexpr PointBinding kBindings[] = {
    // 关口功率：645 的 0x02030000 是**带符号**的 3 字节 BCD，
    //   正 = 正向（进口），负 = 反向（倒送）—— 与 EMS_P_GRID 的
    //   "进口为正"口径一致，符号方向由 dlt645_codec.h 的 sign_bit 保证。
    { EMS_P_GRID, "MEAS.P_GRID", 0x02030000u, {1.0, 0.0} },
};
constexpr std::size_t kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);

inline const PointBinding* binding_for_index(std::size_t ems_index,
                                             const PointBinding* table = kBindings,
                                             std::size_t count = kBindingCount) {
    for (std::size_t i = 0; i < count; ++i) {
        if (table[i].index == ems_index) return &table[i];
    }
    return nullptr;
}

// ---------------------------------------------------------------------
// 自检：把"映射表被改坏"变成可计数的失败，而不是运行期的怪数
// ---------------------------------------------------------------------
struct MapCheck {
    int index_out_of_range = 0;  // ① index 越过设备点区
    int name_mismatch      = 0;  // ② 点名与抽象表不一致
    int di_unknown         = 0;  // ③ DI 不在内置表里
    int dup_index          = 0;  // 同一抽象点被绑两次（后写的会悄悄盖掉先写的）
    int total              = 0;
    int checked            = 0;  // 正向证据：实际检查了多少行

    void add(int* c) { ++(*c); ++total; }
};

inline MapCheck self_check_map(const PointBinding* table = kBindings,
                               std::size_t count = kBindingCount) {
    MapCheck r;
    r.checked = static_cast<int>(count);
    for (std::size_t i = 0; i < count; ++i) {
        const PointBinding& b = table[i];

        if (b.index >= static_cast<std::size_t>(EMS_EXT_BEGIN)) {
            r.add(&r.index_out_of_range);
        }
        if (b.index < static_cast<std::size_t>(EMS_POINT_COUNT)) {
            if (b.name == nullptr ||
                std::strcmp(b.name, EMS_POINT_NAMES[b.index]) != 0) {
                r.add(&r.name_mismatch);
            }
        }
        if (dlt645::find_builtin(b.di) == nullptr) {
            r.add(&r.di_unknown);
        }
        for (std::size_t j = i + 1; j < count; ++j) {
            if (table[j].index == b.index) r.add(&r.dup_index);
        }
    }
    return r;
}

} // namespace meter645
} // namespace ems
