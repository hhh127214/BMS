// =====================================================================
//  BMS · 03/shared/clamper.h
//
//  通用 clamp 工具，供 03/ 下三个控制器 (防逆流 / 光伏出力平抑 / 需量管理)
//  共用。删除了原本散落在每个控制器 .h/.cpp 中的 clamp 重複实现。
//
//  使用示例:
//      #include "shared/clamper.h"
//      double x = bms::clamp(value, 0.0, 100.0);
//
//  兼容性: 仅依赖 <algorithm>, 与 C++11/14/17/20 兼容。
// =====================================================================
#pragma once

#include <algorithm>

namespace bms {

inline double clamp(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace bms
