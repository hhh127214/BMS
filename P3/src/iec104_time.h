// =====================================================================
// P3 / IEC104 时标层：把「库的 UTC 口径」扳回「国内调度要的北京时间」
//
// ---------------------------------------------------------------------
// 为什么必须有这个文件
// ---------------------------------------------------------------------
// lib60870 的 CP56Time2a 走的是 gmtime（UTC），不做任何时区换算：
//
//   cpXXtime2a.c:477  CP56Time2a_setFromMsTimestamp()
//       gmtime_s(&tmTime, &timeVal);      /* ← UTC，不是 localtime */
//
// 于是 CP56Time2a_createFromMsTimestamp(t, Hal_getTimeInMs()) 填进去的是 UTC 时标。
// 而 IEC 60870-5-101/104 在国内的现场口径是 **北京时间**（= UTC+8，夏令时位置 0）：
// 调度主站把 CP56Time2a 直接当本地时间显示与存档，不做换算。
//
// ---------------------------------------------------------------------
// 为什么这个坑极其隐蔽（本项目的第三次「单进程测不出来」）
// ---------------------------------------------------------------------
// 库对这两个函数是**严格互逆**的：
//   createFromMsTimestamp  → gmtime 拆字段
//   toMsTimestamp          → my_mktime 把字段当 UTC 合回去（见 cpXXtime2a.c:296 注释）
//
// 所以「写进去再读出来」永远恒等 —— **自环测试 100% 通过**。
// 只有把时标拿去跟墙钟比，或者跟调度主站的实际显示比，才会发现差了 8 小时。
//
// 处理方式：**写入前加偏移、读取后减偏移**，全程只在这一个文件里做。
// 业务代码禁止直接调用 CP56Time2a_createFromMsTimestamp / toMsTimestamp。
// =====================================================================

#ifndef EMS_P3_IEC104_TIME_H
#define EMS_P3_IEC104_TIME_H

#include "iec60870_common.h"
#include "hal_time.h"

#include <cstdint>
#include <cstdio>

namespace iec104 {

// ---------------------------------------------------------------------
// UTC 偏移（秒）。国内固定 +8h，不跟夏令时。
// 留成可配是为了同一份代码能跑在别的时区的现场；现场调试可用
// `--utc-offset` 覆盖（见 main_gateway.cpp），不要改这里的默认值。
// ---------------------------------------------------------------------
inline int& utc_offset_ref() {
    static int v = 8 * 3600;   // 北京时间
    return v;
}
inline void set_utc_offset_seconds(int sec) { utc_offset_ref() = sec; }
inline int  utc_offset_seconds() { return utc_offset_ref(); }

inline const char* utc_offset_text() {
    static char buf[16];
    const int s = utc_offset_seconds();
    const char sign = (s < 0) ? '-' : '+';
    const int a = (s < 0) ? -s : s;
    std::snprintf(buf, sizeof(buf), "UTC%c%02d:%02d", sign, a / 3600, (a % 3600) / 60);
    return buf;
}

// UTC 毫秒 → 本地时间 CP56Time2a。
// 必须用栈上的 struct sCP56Time2a（CP56Time2a 本身是指针 typedef）。
inline void make_local_cp56time2a(CP56Time2a out, std::uint64_t utc_ms) {
    const std::uint64_t local_ms =
        utc_ms + static_cast<std::uint64_t>(utc_offset_seconds()) * 1000ULL;
    CP56Time2a_createFromMsTimestamp(out, local_ms);
    CP56Time2a_setSummerTime(out, false);   // 国内无夏令时；不置位，避免调度侧误判
}

// 本地时间 CP56Time2a → UTC 毫秒（与上面互逆）。
inline std::uint64_t cp56time2a_local_to_utc_ms(const CP56Time2a t) {
    return CP56Time2a_toMsTimestamp(t)
         - static_cast<std::uint64_t>(utc_offset_seconds()) * 1000ULL;
}

// 本进程当前 UTC 毫秒（网关的**唯一**时间源；测试可注入）
inline std::uint64_t now_utc_ms() { return Hal_getTimeInMs(); }

// 毫秒 → 秒（**唯一的** ms→s 换算口，供心跳/超时/报告统一调用）。
//
// 为什么单独抽一个函数：这条换算曾经写错（`(now - t0) / 1000000ull`），
// 于是网关心跳在头 **1000 秒**里恒打印 `T+0s` —— 现象只有"时钟看起来卡住"，
// 编译、单测、协议交互**全部正常**，是典型静默出错。
// 抽成纯函数之后它就能被断言（见 tests/test_iec104.cpp），不必靠肉眼看日志。
inline std::uint64_t ms_to_s(std::uint64_t ms) { return ms / 1000ULL; }

// 人读格式，用于日志与报告（把 CP56Time2a 按本地时间打印）
inline void format_cp56time2a_local(const CP56Time2a t, char* buf, int buf_size) {
    std::snprintf(buf, static_cast<std::size_t>(buf_size),
                  "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  CP56Time2a_getYear(t) + 2000,
                  CP56Time2a_getMonth(t),
                  CP56Time2a_getDayOfMonth(t),
                  CP56Time2a_getHour(t),
                  CP56Time2a_getMinute(t),
                  CP56Time2a_getSecond(t),
                  CP56Time2a_getMillisecond(t));
}

} // namespace iec104

#endif // EMS_P3_IEC104_TIME_H
