// =====================================================================
//  BMS · 03/shared/controller_types.h
//
//  控制器通用类型别名/常量，供 03/ 下三个控制器共用。
//  - TimestampMs: 时间戳(毫秒)统一使用 std::uint32_t
//  - PowerKw:    功率单位统一使用 double
//
//  后续可扩展: 日志级别、告警严重等级、接口抽象等。
// =====================================================================
#pragma once

#include <cstdint>

namespace bms {

using TimestampMs = std::uint32_t;
using PowerKw     = double;

} // namespace bms
