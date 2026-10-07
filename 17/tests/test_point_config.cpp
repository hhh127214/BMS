// =====================================================================
// 17/tests/test_point_config.cpp —— 点表配置化（C5）
//
// 判据（缺口 §10.3 C5）：
//   ① 内置默认点表 → to_json() → load_json() → 逐点逐字段完全一致（往返幂等）
//   ② 改 JSON 里的量程/单位 → 加载后生效
//   ③ 非法 JSON / 缺字段 / 点名重复 / 量程矛盾
//      → 显式报错并指出**第几个点、哪个字段**（不静默用默认值）
//   ④ 支持**追加点**而不破坏既有索引
//
//   T01 空表加载（最小可用路径）
//   T02 往返幂等：107 点 × 8 字段逐一相等（double 用逐位 ==）
//   T03 往返后顺序/索引一致
//   T04 改量程 / 改单位 → 生效
//   T05 非法 JSON → 行:列
//   T06 缺字段 → 点下标 + 字段名（七个字段逐一测）
//   T07 点名重复 → 指到重复的那一个
//   T08 量程矛盾 / 默认值越量程 → 点 + 字段
//   T09 枚举字段值非法 → 点 + 字段
//   T10 原子性：失败不改动原表
//   T11 追加点：旧索引不动，新点追加在后
//   T12 JSON 宽松（注释 / 尾逗号 / 裸键）
//   T13 数字输出：整数不带小数点、小数最短可往返
//   T14 反向守卫：这些判据确实有区分度
//
// 编译：见 17/scripts/build_test.bat
// =====================================================================

#include "expect.h"

#include "device_point_table.h"
#include "json_min.h"
#include "point_mapping.h"
#include "point_table_loader.h"

#include <cmath>
#include <string>
#include <vector>

using namespace ems;
using namespace ems::devpt;

static int g_pass = 0;
static int g_fail = 0;

// 一行最小合法点
static const char* kOnePoint =
    "{\"points\":[{\"name\":\"T.A\",\"unit\":\"V\",\"type\":\"analog\","
    "\"range\":[0,10],\"default\":1,\"device\":\"bms\",\"zone\":\"meas\"}]}";

// =====================================================================
// T01 空表加载
// =====================================================================
static void test_01_load_into_empty() {
    std::printf("T01 空表加载（最小可用路径）\n");

    DevicePointTable t;
    EXPECT_EQ(t.size(), 0);
    const LoadError e = t.load_json(kOnePoint, false);
    EXPECT(e.ok);
    EXPECT_EQ(t.size(), 1);
    EXPECT_STR_EQ(t.points[0].name, "T.A");
    EXPECT_STR_EQ(t.points[0].unit, "V");
    EXPECT_EQ(t.points[0].type, PT_ANALOG);
    EXPECT_NEAR(t.points[0].range_min, 0.0, 1e-12);
    EXPECT_NEAR(t.points[0].range_max, 10.0, 1e-12);
    EXPECT_NEAR(t.points[0].def, 1.0, 1e-12);
    EXPECT_EQ(t.points[0].device, DK_BMS);
    EXPECT_EQ(t.points[0].zone, PZ_MEAS);
    EXPECT_EQ(t.validate().ok, 1);

    // 顶层不是对象 → 报错
    DevicePointTable t2;
    const LoadError e2 = t2.load_json("[1,2,3]", false);
    EXPECT(!e2.ok);
    EXPECT_EQ(e2.point_index, -1);
    EXPECT_STR_EQ(e2.field, "<root>");
    EXPECT_EQ(t2.size(), 0);          // 失败不留痕
}

// =====================================================================
// T02 往返幂等：逐点逐字段完全一致
// =====================================================================
static void test_02_roundtrip_identity() {
    std::printf("T02 往返幂等：107 点 × 8 字段逐一相等\n");

    const DevicePointTable a = DevicePointTable::builtin();
    const std::string js = a.to_json(2);

    DevicePointTable b;
    const LoadError e = b.load_json(js, false);
    EXPECT(e.ok);
    EXPECT_EQ(b.size(), a.size());
    EXPECT_EQ(b.size(), 107);

    int diff = 0, exact_double_diff = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        const DevicePoint& x = a.points[i];
        const DevicePoint& y = b.points[i];
        if (x.name != y.name) ++diff;
        if (x.unit != y.unit) ++diff;
        if (x.type != y.type) ++diff;
        if (x.device != y.device) ++diff;
        if (x.zone != y.zone) ++diff;
        // ★ double 用**逐位相等**（不是 EXPECT_NEAR）：
        //   往返幂等若只验近似，那"用 %.6g 输出"这种精度损失照样能过，
        //   而现场表现是"点表加载后量程被悄悄改了小数点后第 7 位"。
        if (!(x.range_min == y.range_min)) { ++exact_double_diff; ++diff; }
        if (!(x.range_max == y.range_max)) { ++exact_double_diff; ++diff; }
        if (!(x.def == y.def))             { ++exact_double_diff; ++diff; }
    }
    EXPECT_EQ(diff, 0);
    EXPECT_EQ(exact_double_diff, 0);

    // 二次往返：dump(parse(dump(x))) 与 dump(x) 逐字节相同（幂等，不只是等价）
    const std::string js2 = b.to_json(2);
    EXPECT_STR_EQ(js2, js);

    // 加载回来的表仍能通过双层自检
    EXPECT_EQ(self_check(b), 0);
}

// =====================================================================
// T03 往返后顺序 / 索引一致
// =====================================================================
static void test_03_roundtrip_order() {
    std::printf("T03 往返后顺序 / 索引一致\n");

    const DevicePointTable a = DevicePointTable::builtin();
    DevicePointTable b;
    EXPECT(b.load_json(a.to_json(2), false).ok);

    int idx_diff = 0;
    for (int i = 0; i < DP_POINT_COUNT; ++i) {
        if (b.index_of(dev_point_name(i)) != i) ++idx_diff;
    }
    EXPECT_EQ(idx_diff, 0);
    EXPECT_EQ(b.index_of("MEAS.BMS.SOC"), DP_BMS_SOC);
    EXPECT_EQ(b.index_of("MEAS.PCS.E_TOTAL_KWH"), DP_PCS_E_TOTAL_KWH);
    // 追加点区没被填充（内置表就是 107 点 —— 锚点纪律）
    EXPECT_EQ(static_cast<int>(b.size()), DP_APPEND_BEGIN);
}

// =====================================================================
// T04 改量程 / 单位 → 生效
// =====================================================================
static void test_04_edit_takes_effect() {
    std::printf("T04 改量程 / 单位 → 加载后生效\n");

    const DevicePointTable a = DevicePointTable::builtin();
    std::string js = a.to_json(2);

    // 换站现场最常见的两类改动：
    //   ① 量程不同（换了个 630 kVA 的变压器、电表量程从 600 V 改 690 V）
    //   ② 单位写法不同（老表用 "degC"，新表用 "C"）
    const std::size_t p = js.find("\"MEAS.METER.U_A\"");
    EXPECT(p != std::string::npos);
    const std::size_t rp = js.find("[0, 600]", p);
    EXPECT(rp != std::string::npos);
    js.replace(rp, std::string("[0, 600]").size(), "[0, 690]");

    const std::size_t tp = js.find("\"MEAS.BMS.T_MAX\"");
    EXPECT(tp != std::string::npos);
    const std::size_t up = js.find("\"degC\"", tp);
    EXPECT(up != std::string::npos);
    js.replace(up, std::string("\"degC\"").size(), "\"C\"   ");

    DevicePointTable b;
    const LoadError e = b.load_json(js, false);
    EXPECT(e.ok);

    const DevicePoint* ua = b.find("MEAS.METER.U_A");
    EXPECT(ua != nullptr);
    EXPECT_NEAR(ua->range_max, 690.0, 1e-12);      // 新量程生效
    EXPECT_NEAR(ua->range_min, 0.0, 1e-12);

    const DevicePoint* tmax = b.find("MEAS.BMS.T_MAX");
    EXPECT(tmax != nullptr);
    EXPECT_STR_EQ(tmax->unit, "C");                // 新单位生效

    // ★ 反向守卫：改坏成"超出新量程的默认值"必须被拒 —— 证明加载器**真的在校验**，
    //   而不是"读进来就完事"。
    std::string bad = js;
    const std::size_t rp2 = bad.find("[0, 690]", 0);
    EXPECT(rp2 != std::string::npos);
    bad.replace(rp2, std::string("[0, 690]").size(), "[0, 0]");   // min == max
    DevicePointTable c;
    const LoadError e2 = c.load_json(bad, false);
    EXPECT(!e2.ok);
    EXPECT_EQ(c.size(), 0);
}

// =====================================================================
// T05 非法 JSON → 行:列
// =====================================================================
static void test_05_bad_json() {
    std::printf("T05 非法 JSON → 带行:列\n");

    DevicePointTable t;
    const LoadError e = t.load_json("{\n  \"points\": [\n    {\"name\": \"A\",}\n  \n", false);
    EXPECT(!e.ok);
    EXPECT_EQ(e.point_index, -1);
    EXPECT_STR_EQ(e.field, "<json>");
    // 必须带出行号（"人看文件找问题"的第一现场）
    EXPECT(e.message.find("JSON 解析错误") != std::string::npos);
    EXPECT(e.to_string().find(":") != std::string::npos);
    EXPECT_EQ(t.size(), 0);

    // 缺少 points 键
    DevicePointTable t2;
    const LoadError e2 = t2.load_json("{\"count\": 0}", false);
    EXPECT(!e2.ok);
    EXPECT_STR_EQ(e2.field, "points");
    // points 不是数组
    DevicePointTable t3;
    EXPECT(!t3.load_json("{\"points\": 5}", false).ok);
    // 点表项不是对象
    DevicePointTable t4;
    const LoadError e4 = t4.load_json("{\"points\": [5]}", false);
    EXPECT(!e4.ok);
    EXPECT_EQ(e4.point_index, 0);
    EXPECT_STR_EQ(e4.field, "<point>");
}

// =====================================================================
// helper：由一个"完整合法点"的 JSON 文本挖掉一个字段
// =====================================================================
static std::string point_json_without(const std::string& field) {
    // 逐字段拼一个点，缺哪个就不拼哪个
    std::vector<std::pair<std::string, std::string>> kv = {
        {"name",    "\"T.A\""},
        {"unit",    "\"V\""},
        {"type",    "\"analog\""},
        {"range",   "[0, 10]"},
        {"default", "1"},
        {"device",  "\"pcs\""},
        {"zone",    "\"sta\""}
    };
    std::string body;
    for (const auto& p : kv) {
        if (p.first == field) continue;
        if (!body.empty()) body += ",";
        body += "\"" + p.first + "\":" + p.second;
    }
    return "{\"points\":[{" + body + "}]}";
}

// =====================================================================
// T06 缺字段 → 点下标 + 字段名
// =====================================================================
static void test_06_missing_fields() {
    std::printf("T06 缺字段 → 第几个点 / 哪个字段\n");

    const char* fields[] = {"name", "unit", "type", "range", "default", "device", "zone"};
    for (const char* f : fields) {
        DevicePointTable t;
        const LoadError e = t.load_json(point_json_without(f), false);
        if (e.ok) { EXPECT(false); continue; }
        ++g_pass;                                     // 必须报错
        EXPECT_EQ(e.point_index, 0);
        EXPECT_STR_EQ(e.field, f);
        EXPECT_EQ(t.size(), 0);                       // **不静默用默认值**
    }

    // 字段类型写错（default 写成字符串）也必须报错，而不是被当 0
    DevicePointTable t;
    const LoadError e = t.load_json(
        "{\"points\":[{\"name\":\"T.A\",\"unit\":\"V\",\"type\":\"analog\","
        "\"range\":[0,10],\"default\":\"one\",\"device\":\"pcs\",\"zone\":\"sta\"}]}", false);
    EXPECT(!e.ok);
    EXPECT_STR_EQ(e.field, "default");
    EXPECT_EQ(t.size(), 0);
}

// =====================================================================
// T07 点名重复
// =====================================================================
static void test_07_duplicate_name() {
    std::printf("T07 点名重复 → 指到重复的那一个\n");

    DevicePointTable t;
    const LoadError e = t.load_json(
        "{\"points\":["
        "{\"name\":\"DUP\",\"unit\":\"V\",\"type\":\"analog\",\"range\":[0,1],\"default\":0,\"device\":\"bms\",\"zone\":\"meas\"},"
        "{\"name\":\"OTHER\",\"unit\":\"V\",\"type\":\"analog\",\"range\":[0,1],\"default\":0,\"device\":\"bms\",\"zone\":\"meas\"},"
        "{\"name\":\"DUP\",\"unit\":\"A\",\"type\":\"analog\",\"range\":[0,1],\"default\":0,\"device\":\"bms\",\"zone\":\"meas\"}"
        "]}", false);
    EXPECT(!e.ok);
    EXPECT_EQ(e.point_index, 2);                 // 报**重复的那一个**（下标 2）
    EXPECT_STR_EQ(e.field, "name");
    EXPECT_STR_EQ(e.point_name, "DUP");
    EXPECT_EQ(t.size(), 0);

    // 与**已加载**的表重复（append 场景）也必须报 —— 且下标按整表数
    DevicePointTable t2;
    EXPECT(t2.load_json(kOnePoint, false).ok);       // 现在表里有 T.A（下标 0）
    const LoadError e2 = t2.load_json(
        "{\"points\":["
        "{\"name\":\"T.B\",\"unit\":\"V\",\"type\":\"analog\",\"range\":[0,1],\"default\":0,\"device\":\"bms\",\"zone\":\"meas\"},"
        "{\"name\":\"T.A\",\"unit\":\"V\",\"type\":\"analog\",\"range\":[0,1],\"default\":0,\"device\":\"bms\",\"zone\":\"meas\"}"
        "]}", true);
    EXPECT(!e2.ok);
    EXPECT_EQ(e2.point_index, 2);                // 整表下标 2（第 2 个新增点）
    EXPECT_EQ(t2.size(), 1);                     // 原表没被破坏
}

// =====================================================================
// T08 量程矛盾 / 默认值越量程
// =====================================================================
static void test_08_range_conflicts() {
    std::printf("T08 量程矛盾 / 默认值越量程\n");

    auto one = [](const char* range, const char* def) {
        return std::string("{\"points\":[{\"name\":\"T.A\",\"unit\":\"V\",\"type\":\"analog\","
                           "\"range\":") + range + ",\"default\":" + def +
               ",\"device\":\"bms\",\"zone\":\"meas\"}]}";
    };

    {   // min > max
        DevicePointTable t;
        const LoadError e = t.load_json(one("[10, 0]", "1"), false);
        EXPECT(!e.ok);
        EXPECT_EQ(e.point_index, 0);
        EXPECT_STR_EQ(e.field, "range");
    }
    {   // min == max
        DevicePointTable t;
        const LoadError e = t.load_json(one("[5, 5]", "5"), false);
        EXPECT(!e.ok);
        EXPECT_STR_EQ(e.field, "range");
    }
    {   // 默认值越上限
        DevicePointTable t;
        const LoadError e = t.load_json(one("[0, 10]", "11"), false);
        EXPECT(!e.ok);
        EXPECT_STR_EQ(e.field, "default");
    }
    {   // 默认值越下限
        DevicePointTable t;
        const LoadError e = t.load_json(one("[0, 10]", "-1"), false);
        EXPECT(!e.ok);
        EXPECT_STR_EQ(e.field, "default");
    }
    {   // 边界值恰好等于 min/max 必须**通过**（反向守卫：别把合法值也拒了）
        DevicePointTable t;
        EXPECT(t.load_json(one("[0, 10]", "10"), false).ok);
        DevicePointTable t2;
        EXPECT(t2.load_json(one("[0, 10]", "0"), false).ok);
    }
    {   // range 不是两元素
        DevicePointTable t;
        EXPECT(!t.load_json(one("[0, 1, 2]", "0"), false).ok);
        DevicePointTable t2;
        EXPECT(!t2.load_json(one("[0]", "0"), false).ok);
    }
}

// =====================================================================
// T09 枚举字段非法
// =====================================================================
static void test_09_bad_enums() {
    std::printf("T09 枚举字段值非法 → 点 + 字段\n");

    auto one = [](const char* type, const char* dev, const char* zone) {
        return std::string("{\"points\":[{\"name\":\"T.A\",\"unit\":\"V\",\"type\":\"") + type +
               "\",\"range\":[0,1],\"default\":0,\"device\":\"" + dev + "\",\"zone\":\"" + zone +
               "\"}]}";
    };
    { DevicePointTable t; const LoadError e = t.load_json(one("analog2", "bms", "meas"), false);
      EXPECT(!e.ok); EXPECT_STR_EQ(e.field, "type"); }
    { DevicePointTable t; const LoadError e = t.load_json(one("analog", "ups", "meas"), false);
      EXPECT(!e.ok); EXPECT_STR_EQ(e.field, "device"); }
    { DevicePointTable t; const LoadError e = t.load_json(one("digital", "pcs", "fast"), false);
      EXPECT(!e.ok); EXPECT_STR_EQ(e.field, "zone"); }
    // 合法组合必须通过
    EXPECT(DevicePointTable().load_json(one("digital", "meter", "alm"), false).ok);
}

// =====================================================================
// T10 原子性
// =====================================================================
static void test_10_atomicity() {
    std::printf("T10 原子性：失败不改动原表\n");

    DevicePointTable t;
    EXPECT(t.load_json(kOnePoint, false).ok);
    const std::string before = t.to_json(2);

    // 第二个点缺 zone → 整批失败，原表（含第一个点）必须原封不动
    const LoadError e = t.load_json(
        "{\"points\":["
        "{\"name\":\"T.B\",\"unit\":\"V\",\"type\":\"analog\",\"range\":[0,1],\"default\":0,\"device\":\"bms\",\"zone\":\"meas\"},"
        "{\"name\":\"T.C\",\"unit\":\"V\",\"type\":\"analog\",\"range\":[0,1],\"default\":0,\"device\":\"bms\"}"
        "]}", true);
    EXPECT(!e.ok);
    EXPECT_EQ(e.point_index, 2);                  // 整表下标 = base(1) + 1
    EXPECT_STR_EQ(e.field, "zone");
    EXPECT_EQ(t.size(), 1);                       // ★ 半加载比失败更危险
    EXPECT_STR_EQ(t.to_json(2), before);

    // 全量替换模式下失败，同样不能清空原表
    const LoadError e2 = t.load_json("{\"points\":[{\"name\":\"X\"}]}", false);
    EXPECT(!e2.ok);
    EXPECT_EQ(t.size(), 1);
    EXPECT_STR_EQ(t.to_json(2), before);
}

// =====================================================================
// T11 追加点：不破坏既有索引
// =====================================================================
static void test_11_append_points() {
    std::printf("T11 追加点：旧索引不动，新点在后\n");

    const DevicePointTable base = DevicePointTable::builtin();
    DevicePointTable t = base;
    EXPECT_EQ(t.size(), 107);

    // 换站新增：一个 BMS 消防信号 + 一个电表费率时段
    const LoadError e = t.load_json(
        "{\"points\":["
        "{\"name\":\"STA.BMS.FIRE_ALARM\",\"unit\":\"bool\",\"type\":\"digital\","
        "\"range\":[0,1],\"default\":0,\"device\":\"bms\",\"zone\":\"sta\"},"
        "{\"name\":\"STA.METER.TARIFF\",\"unit\":\"-\",\"type\":\"digital\","
        "\"range\":[0,4],\"default\":0,\"device\":\"meter\",\"zone\":\"sta\"}"
        "]}", true);
    EXPECT(e.ok);
    EXPECT_EQ(t.size(), 109);

    // ★ 既有 107 点的下标与字段**逐点不变**
    int moved = 0;
    for (int i = 0; i < 107; ++i) {
        if (t.index_of(base.points[static_cast<std::size_t>(i)].name) != i) ++moved;
        if (t.points[static_cast<std::size_t>(i)].unit !=
            base.points[static_cast<std::size_t>(i)].unit) ++moved;
        if (!(t.points[static_cast<std::size_t>(i)].def ==
              base.points[static_cast<std::size_t>(i)].def)) ++moved;
    }
    EXPECT_EQ(moved, 0);

    // 新点在末尾
    EXPECT_EQ(t.index_of("STA.BMS.FIRE_ALARM"), 107);
    EXPECT_EQ(t.index_of("STA.METER.TARIFF"), 108);
    EXPECT_EQ(t.validate().ok, 1);

    // 追加一整套（变电所扩容：簇数从 4 到 8）也不会让既有索引漂移
    DevicePointTable u = base;
    std::string many = "{\"points\":[";
    for (int k = 0; k < 20; ++k) {
        if (k) many += ",";
        many += "{\"name\":\"MEAS.EXT.CLUSTER_" + std::to_string(k) +
                "\",\"unit\":\"V\",\"type\":\"analog\",\"range\":[0,1000],\"default\":0,"
                "\"device\":\"bms\",\"zone\":\"meas\"}";
    }
    many += "]}";
    EXPECT(u.load_json(many, true).ok);
    EXPECT_EQ(u.size(), 127);
    EXPECT_EQ(u.index_of("MEAS.BMS.SOC"), DP_BMS_SOC);      // 既有索引没漂
    EXPECT_EQ(u.index_of("MEAS.EXT.CLUSTER_19"), 126);
}

// =====================================================================
// T12 JSON 宽松：注释 / 尾逗号 / 裸键
// =====================================================================
static void test_12_lenient_json() {
    std::printf("T12 JSON 宽松（注释 / 尾逗号 / 裸键）\n");

    const char* text =
        "// 现场临时注释掉一个点位定义\n"
        "# 另一种注释写法\n"
        "{\n"
        "  /* 块注释 */\n"
        "  points: [\n"
        "    { name: \"T.A\", unit: \"V\", type: analog, range: [0, 10],\n"
        "      default: 1, device: bms, zone: meas, },   // 尾逗号合法\n"
        "  ],\n"
        "}\n";
    DevicePointTable t;
    const LoadError e = t.load_json(text, false);
    EXPECT(e.ok);
    EXPECT_EQ(t.size(), 1);
    EXPECT_STR_EQ(t.points[0].name, "T.A");
    EXPECT_EQ(t.points[0].type, PT_ANALOG);

    // 但"非法"仍然要是非法：值起始字符乱写
    DevicePointTable t2;
    EXPECT(!t2.load_json("{\"points\":[{\"name\":@}]}", false).ok);
    // 未闭合的块注释
    DevicePointTable t3;
    EXPECT(!t3.load_json("/* 没关\n{\"points\":[]}", false).ok);
}

// =====================================================================
// T13 数字输出：最短可往返
// =====================================================================
static void test_13_number_format() {
    std::printf("T13 数字输出：整数不带小数点 / 小数最短可往返\n");

    using ems::jmin::num_to_string;
    EXPECT_STR_EQ(num_to_string(0.0), "0");
    EXPECT_STR_EQ(num_to_string(1000.0), "1000");
    EXPECT_STR_EQ(num_to_string(-40.0), "-40");
    EXPECT_STR_EQ(num_to_string(1.0e9), "1000000000");
    EXPECT_STR_EQ(num_to_string(0.95), "0.95");
    EXPECT_STR_EQ(num_to_string(3.35), "3.35");
    EXPECT_STR_EQ(num_to_string(0.05), "0.05");
    EXPECT_STR_EQ(num_to_string(1.0e10), "10000000000");

    // ★ 逐位往返（这才是判据①真正依赖的性质）
    const double vals[] = {0.1, 0.3, 1.0 / 3.0, 3.35, 0.95, 1e9, 1e10, 4e9,
                           -1e6, 0.00025, 1e-12};
    int bad = 0;
    for (double v : vals) {
        const std::string s = num_to_string(v);
        if (std::strtod(s.c_str(), nullptr) != v) ++bad;
    }
    EXPECT_EQ(bad, 0);

    // NaN / inf 输出成 null（JSON 里没有 NaN 字面量）
    EXPECT_STR_EQ(num_to_string(std::nan("")), "null");
    EXPECT_STR_EQ(num_to_string(1.0 / 0.0), "null");
}

// =====================================================================
// T14 反向守卫：判据确实有区分度
// =====================================================================
static void test_14_guards() {
    std::printf("T14 反向守卫：这些判据确实能红\n");

    // ① 用**近似**比较就抓不到的差别：这里证明 double 是逐位可比的
    const DevicePointTable a = DevicePointTable::builtin();
    const DevicePoint* p = a.find("CFG.PCS.RAMP_KW_PER_S");
    EXPECT(p != nullptr);
    EXPECT(p->def == 400.0);
    EXPECT(p->range_max == 1.0e10);           // 抽象表的"极大数 = 不限制"约定
    EXPECT(std::fabs(p->def - 400.0) == 0.0);

    // ② 校验器不是"永远返回 true"：随便造 5 个坏表，必须全部被拒
    int rejected = 0;
    const char* bads[] = {
        "{\"points\":[{\"name\":\"\",\"unit\":\"V\",\"type\":\"analog\",\"range\":[0,1],"
        "\"default\":0,\"device\":\"bms\",\"zone\":\"meas\"}]}",
        "{\"points\":[{\"name\":\"A\",\"unit\":\"V\",\"type\":\"analog\",\"range\":[2,1],"
        "\"default\":0,\"device\":\"bms\",\"zone\":\"meas\"}]}",
        "{\"points\":[{\"name\":\"A\",\"unit\":\"V\",\"type\":\"analog\",\"range\":[0,1],"
        "\"default\":9,\"device\":\"bms\",\"zone\":\"meas\"}]}",
        "{\"points\":[{\"name\":\"A\",\"unit\":\"V\",\"type\":\"weird\",\"range\":[0,1],"
        "\"default\":0,\"device\":\"bms\",\"zone\":\"meas\"}]}",
        "{\"points\":[{\"name\":\"A\",\"unit\":\"V\",\"type\":\"analog\",\"range\":[0,1],"
        "\"default\":0,\"device\":\"ufo\",\"zone\":\"meas\"}]}"
    };
    for (const char* b : bads) {
        DevicePointTable t;
        if (!t.load_json(b, false).ok) ++rejected;
    }
    EXPECT_EQ(rejected, 5);

    // ③ 同一份合法数据必须通过（证明上面拒的不是"加载器本来就不工作"）
    DevicePointTable ok;
    EXPECT(ok.load_json(kOnePoint, false).ok);
    EXPECT_EQ(ok.size(), 1);

    // ★ 而**只有 1 个点**的表跑两层自检**必须报"来源找不到"** —— 这不是缺陷，
    //   是判据②的本意：抽象表 40 点的来源全在这张 1 点表里找不到。
    //   这一条同时证明了 self_check 不是"表非空就返回 0"。
    EXPECT_GT0(self_check(ok));
    EXPECT_GT0(self_check_report(ok).unresolved_sources);
    // 反向守卫：完整的内置表则是 0 —— 差别只来自"表全不全"
    EXPECT_EQ(self_check(), 0);
}

int main() {
    TEST_BANNER("17/ 点表配置化（C5）—— 往返 / 报错定位 / 追加点");

    test_01_load_into_empty();
    test_02_roundtrip_identity();
    test_03_roundtrip_order();
    test_04_edit_takes_effect();
    test_05_bad_json();
    test_06_missing_fields();
    test_07_duplicate_name();
    test_08_range_conflicts();
    test_09_bad_enums();
    test_10_atomicity();
    test_11_append_points();
    test_12_lenient_json();
    test_13_number_format();
    test_14_guards();

    TEST_TAIL();
    return g_fail == 0 ? 0 : 1;
}
