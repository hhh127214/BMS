// =====================================================================
// 21/ — DL/T 645-2007 数据项层（DI 表 + BCD 解码 + 变比换算）
//
// 帧层给的是字节，这一层把字节变成"220.5 V"这种能进算法的数。
//
// ---------------------------------------------------------------------
// ★★ 三个必须在这里挡住的静默失败
// ---------------------------------------------------------------------
// ① **BCD 里出现 0xA~0xF 就是坏数据，不能当数算。**
//    BCD 每个半字节只能 0~9。若线上干扰把 0x05 打成 0x0F，按位权累加会
//    算出一个"看起来完全正常"的数（5 → 15）。于是电压从 220.5 变成 661.5，
//    越限判据照常跑，只是跑在假世界上 —— 这正是本项目最忌的一类。
//
// ② **符号位和数字位挤在同一个字节里（功率类）。**
//    DL/T645-2007 规定功率是 3 字节 BCD，**最高字节的 bit7 是符号位**，
//    剩下 7 位（高半字节只有 3 位有效）才装数字。
//    不剥离符号位直接解码 → 负功率被当成 +80000 之类的天文数字；
//    剥离了却忘了"高半字节只剩 3 位" → 符号位被算进最高位权。
//
// ③ **长度必须与 DI 声明的一致。**
//    例：电压是 2 字节、电流是 3 字节。若把电流的 3 字节响应当电压解，
//    前 2 字节照样是合法 BCD，**解得出一个数**，只是它完全不是电压。
//    所以 decode 必须同时校验 `n == spec.bytes`，不能"有多少解多少"。
//
// ---------------------------------------------------------------------
// ★ DI 表的权威性边界（必读）
// ---------------------------------------------------------------------
// 下面内置的 DI 是**常用子集**，来自 DL/T 645-2007 附录 A.2。
// 但附录 A.2 里 DI 与"字节数 / 小数位 / 是否带符号"是三列独立信息，
// 抄错一列不会报错、只会算出错数 —— 所以：
//   · 本表只作为**默认值**，现场必须按表计说明书逐条核对；
//   · `verify_note()` 对每个内置项都返回一句核对提示；
//   · 正式点位请走外置 `di_table.csv`（见 di_table_loader.h），
//     改表不用改代码、不用重编。
// 这条纪律与 MeterSim645 作者自己踩的坑同源（他的原话：DI 表里的电网频率
// 是占位值，对接真表前务必按附录 A.2 原文核对）。
// =====================================================================

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ems {
namespace dlt645 {

// ---------------------------------------------------------------------
// 一个数据项的定义
// ---------------------------------------------------------------------
struct DiSpec {
    std::uint32_t di = 0;        // 显示序，如 0x02010100
    const char* name = "";       // 中文名
    const char* unit = "";       // 单位；空串表示无量纲
    std::uint8_t bytes = 0;      // 数据域字节数（不含 4 字节 DI）
    std::int8_t decimals = 0;    // 小数位
    bool sign_bit = false;       // 最高字节 bit7 是否为符号位（功率类）
    const char* note = "";       // 核对提示 / 方言差异
};

// 常用子集（10 项）。★ 每一项都要现场核对，理由见文件头。
inline const DiSpec* builtin_specs(std::size_t* count) {
    static const DiSpec kSpecs[] = {
        // DI          名称                单位      字节 小数 符号  备注
        {0x00010000u, "正向有功总电能",     "kWh",      4,  2, false, "费率/象限变体多，须核对"},
        {0x00020000u, "反向有功总电能",     "kWh",      4,  2, false, "同上"},
        {0x02010100u, "A 相电压",           "V",        2,  1, false, ""},
        {0x02010200u, "B 相电压",           "V",        2,  1, false, ""},
        {0x02010300u, "C 相电压",           "V",        2,  1, false, ""},
        {0x02020100u, "A 相电流",           "A",        3,  3, false, ""},
        {0x02020200u, "B 相电流",           "A",        3,  3, false, ""},
        {0x02020300u, "C 相电流",           "A",        3,  3, false, ""},
        {0x02030000u, "瞬时总有功功率",     "kW",       3,  4, true,  "★ 带符号位，最易错"},
        {0x02060000u, "总功率因数",         "",         2,  3, true,  "★ 带符号位（感性/容性）"},
        {0x02800002u, "电网频率",           "Hz",       2,  2, false, "★ 部分厂家用 0x02800001，须核对"},
    };
    if (count) *count = sizeof(kSpecs) / sizeof(kSpecs[0]);
    return kSpecs;
}

// 按 DI 查内置表；找不到返回 nullptr（**不要**回退成"默认 2 字节"）
inline const DiSpec* find_builtin(std::uint32_t di) {
    std::size_t n = 0;
    const DiSpec* p = builtin_specs(&n);
    for (std::size_t i = 0; i < n; ++i) {
        if (p[i].di == di) return &p[i];
    }
    return nullptr;
}

// ---------------------------------------------------------------------
// 变比换算
// ---------------------------------------------------------------------
// 现场电表常经 PT/CT 接入，表里读数要 × 变比 + 偏移才是真实工程量。
// 默认 1 / 0 —— **默认值必须是"不改变读数"的那个**，否则没配变比的
// 站会整体偏大或偏小，而这种偏差在界面上看不出异常。
struct Scale {
    double mul = 1.0;
    double add = 0.0;
};

inline double apply_scale(double raw, const Scale& s) { return raw * s.mul + s.add; }

// ---------------------------------------------------------------------
// 解码结果
// ---------------------------------------------------------------------
struct DecodeResult {
    bool ok = false;
    double raw = 0.0;      // 未乘变比的读数（已带小数位）
    double value = 0.0;    // 乘变比后的工程量
    const char* why = "";  // 失败原因（中文，直接进日志）
};

// ---------------------------------------------------------------------
// BCD 解码
// ---------------------------------------------------------------------
// p 指向**数据域里 DI 之后的第一个字节**（传输序，低位在前）。
// 语义：p[0] 的低半字节是最低位 10^0。
//
// 为什么用 double 而不是整数：最多 8 位十进制（4 字节），
// double 的 53 位有效位绰绰有余；且带小数位时本来就要落到浮点。
inline DecodeResult decode_bcd(const DiSpec& spec, const std::uint8_t* p, std::size_t n,
                               const Scale& scale = Scale()) {
    DecodeResult r;
    if (n != static_cast<std::size_t>(spec.bytes)) {
        r.why = "数据长度与 DI 声明不符";
        return r;
    }
    if (spec.bytes == 0 || spec.bytes > 4) {
        r.why = "DI 声明的字节数非法（只支持 1~4）";
        return r;
    }

    // 复制一份，便于剥离符号位而不改调用方缓冲区
    std::uint8_t b[4] = {0, 0, 0, 0};
    for (std::size_t i = 0; i < n; ++i) b[i] = p[i];

    bool negative = false;
    if (spec.sign_bit) {
        // ★ 符号位在**最高字节**（b[n-1]）的 bit7
        negative = (b[n - 1] & 0x80) != 0;
        b[n - 1] = static_cast<std::uint8_t>(b[n - 1] & 0x7F);
    }

    // ★ 逐半字节校验：出现 10~15 即坏数据，直接拒（见文件头 ①）
    double v = 0.0;
    double weight = 1.0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t lo = static_cast<std::uint8_t>(b[i] & 0x0F);
        const std::uint8_t hi = static_cast<std::uint8_t>((b[i] >> 4) & 0x0F);
        if (lo > 9 || hi > 9) {
            r.why = "BCD 半字节越界（>9），数据已损坏";
            return r;
        }
        v += static_cast<double>(lo) * weight;
        weight *= 10.0;
        v += static_cast<double>(hi) * weight;
        weight *= 10.0;
    }

    if (negative) v = -v;

    // 落小数位
    double div = 1.0;
    for (int i = 0; i < spec.decimals; ++i) div *= 10.0;
    r.raw = v / div;
    r.value = apply_scale(r.raw, scale);
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------
// 反向：工程量 → BCD 字节（**假表 / 自测用**，主站侧不用）
// ---------------------------------------------------------------------
// 存在的理由：进程内的 fake 从站要能"按真实值回帧"，而回帧必须过一遍
// 与解码严格对称的编码 —— 否则两边共同误解时测试全绿（自己和自己对答案）。
// ★ 本函数刻意**不做四舍五入到 decimals 之外**，超出可表示范围直接失败。
inline bool encode_bcd(const DiSpec& spec, double value_in, const Scale& scale,
                       std::vector<std::uint8_t>* out, const char** why = nullptr) {
    if (!out) return false;
    if (spec.bytes == 0 || spec.bytes > 4) {
        if (why) *why = "DI 声明的字节数非法";
        return false;
    }

    // 先反算变比：value = raw*mul + add  →  raw = (value - add)/mul
    if (scale.mul == 0.0) {
        if (why) *why = "变比 mul 为 0";
        return false;
    }
    const double raw = (value_in - scale.add) / scale.mul;

    bool negative = raw < 0.0;
    if (negative && !spec.sign_bit) {
        if (why) *why = "该 DI 不支持负值，但传入值为负";
        return false;
    }

    double div = 1.0;
    for (int i = 0; i < spec.decimals; ++i) div *= 10.0;

    // llround：按小数位取整。用 llround 而非 floor，避免 220.49999 被截成 2204
    long long scaled = static_cast<long long>(std::llround(std::fabs(raw) * div));

    const int max_digits = static_cast<int>(spec.bytes) * 2;
    if (scaled < 0) {
        if (why) *why = "取整溢出";
        return false;
    }
    // 带符号位时，最高半字节只剩 3 位可用 → 有效位少一位
    long long limit = 1;
    for (int i = 0; i < max_digits; ++i) limit *= 10;
    if (spec.sign_bit) limit /= 10;
    if (scaled >= limit) {
        if (why) *why = "数值超出该 DI 的表示范围";
        return false;
    }

    out->assign(spec.bytes, 0);
    long long t = scaled;
    for (int i = 0; i < static_cast<int>(spec.bytes); ++i) {
        int lo = static_cast<int>(t % 10); t /= 10;
        int hi = static_cast<int>(t % 10); t /= 10;
        (*out)[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((hi << 4) | lo);
    }
    if (spec.sign_bit && negative) {
        (*out)[static_cast<std::size_t>(spec.bytes) - 1] =
            static_cast<std::uint8_t>((*out)[static_cast<std::size_t>(spec.bytes) - 1] | 0x80);
    }
    return true;
}

}  // namespace dlt645
}  // namespace ems
