// =====================================================================
// P1/ 单元测试 —— 产品化 P1：配置化
//
//   T201 JSON 解析：标量 / 对象 / 数组 / 嵌套
//   T202 JSON 放宽语法：注释 / 尾逗号 / 裸键 / 裸标识符
//   T203 JSON 错误定位：报错带 行:列
//   T204 JSON 往返：dump → parse 一致
//   T205 字段绑定：类型不符报错 / 未知键警告
//   T206 类型容忍：字符串数字 / "on"/"off" / 1/0
//   T207 缺省语义：缺 section / 缺字段 → 保留结构体默认值
//   T208 全配置往返：to_json → from_json 幂等（89 字段）
//   T209 语义校验：逐类错误能被抓出
//   T210 语义校验：默认配置零错误
//   T211 装配顺序：device_limits 不被 configure_plant 冲掉（陷阱①）
//   T212 装配顺序：OutputShaper 拾取配置（陷阱③）
//   T213 装配顺序：协同层设备参数与 dev_ 一致（陷阱②）
//   T214 策略装配：未知 id 报错 / 参数与开关生效
//   T215 导出回灌：capture → save → load → apply 行为一致
//   T216 端到端：改配置 → 行为改变
//   T217 关键项缺省提示
//   T218 文档生成：模板 / Markdown / Schema 非空且含关键键
//
// 编译：g++ -std=c++17 -Wall -O2 -I src -I ../04/src -I ../05/src
//           -I ../06/src -I ../07/src -I ../08/src
//           tests/test_config.cpp -o build/test_config.exe
// =====================================================================

#include "config_doc.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

using namespace ems;

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT(cond)                                                      \
    do {                                                                  \
        if (cond) {                                                       \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #cond << std::endl;                     \
        }                                                                 \
    } while (0)

#define EXPECT_NEAR(a, b, tol)                                            \
    do {                                                                  \
        const double _a = (a), _b = (b), _t = (tol);                      \
        if (std::fabs(_a - _b) <= _t) {                                   \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : |" << #a << " - " << #b << "| = "            \
                      << std::fabs(_a - _b) << " > " << _t                \
                      << "  (" << _a << " vs " << _b << ")" << std::endl; \
        }                                                                 \
    } while (0)

// ---------------------------------------------------------------------
// 共用：一个可辨识的测试配置（刻意让 limits 与 plant 不一致，
// 以便检出"装配顺序错误导致 limits 被 plant 覆盖"）
// ---------------------------------------------------------------------
static EmsConfig make_test_config() {
    EmsConfig c;
    c.name        = "unit-test";
    c.description = "P1 单元测试配置";
    c.site        = "测试站";
    c.version     = "2.1";

    c.loop.dt_s            = 1.0;
    c.loop.enable_log      = true;
    c.loop.log_every       = 1;
    c.loop.demand_window_s = 900.0;
    c.loop.output_deadband_kw = 7.5;      // 与 OutputShaper 默认 2.0 不同
    c.loop.output_switch_delay = 5;       // 与默认 3 不同

    c.plant.battery_capacity_kwh = 500.0;
    c.plant.soc_init             = 0.50;
    c.plant.soc_phys_min         = 0.05;
    c.plant.soc_phys_max         = 0.98;
    c.plant.pcs_max_chg_kw       = 250.0;   // ← read_limits 会把它写进 dev_
    c.plant.pcs_max_dis_kw       = 250.0;
    c.plant.noise_kw             = 0.0;
    c.plant.noise_soc            = 0.0;
    c.plant.seed                 = 7u;

    c.limits.pcs_rated_chg_kw        = 200.0;  // ← 与 plant 的 250 不同
    c.limits.pcs_rated_dis_kw        = 180.0;  // ← 与 plant 的 250 不同
    c.limits.bms_chg_limit_kw        = 200.0;
    c.limits.bms_dis_limit_kw        = 180.0;
    c.limits.transformer_capacity_kw = 630.0;  // ← 与 DeviceLimits 默认 250 不同
    c.limits.d_target_kw             = 320.0;  // ← 与 DeviceLimits 默认 250 不同

    c.safety.soc_min       = 0.10;
    c.safety.soc_max       = 0.95;
    c.safety.soc_warn_low  = 0.15;
    c.safety.soc_warn_high = 0.90;
    c.safety.grid_p_min_kw = -30.0;
    c.safety.grid_p_max_kw = 630.0;

    c.coord.reopt_period_s = 900.0;
    c.fsm.allow_ready_output = false;
    return c;
}

// 合成曲线（与演示程序同源，测试内独立实现避免跨模块依赖）
static double t_load(double h) {
    return 170.0 + 150.0 * std::exp(-std::pow((h - 14.0) / 4.0, 2.0))
                 +  95.0 * std::exp(-std::pow((h - 19.5) / 1.8, 2.0));
}
static double t_pv(double h) {
    if (h < 6.0 || h > 18.5) return 0.0;
    return 230.0 * std::exp(-std::pow((h - 12.5) / 3.2, 2.0));
}
static double t_price(double h) {
    if (h < 8.0 || h >= 22.0) return 0.30;
    if ((h >= 10.0 && h < 12.0) || (h >= 17.0 && h < 21.0)) return 1.10;
    return 0.65;
}
static ForecastSeries make_fc() {
    ForecastSeries fc;
    fc.step_s = 900.0;
    const int n = 96;
    for (int i = 0; i < n; ++i) {
        const double h = (i * 900.0) / 3600.0;
        fc.p_load_kw.push_back(t_load(h));
        fc.p_pv_kw.push_back(t_pv(h));
        fc.price.push_back(t_price(h));
    }
    fc.loaded = true;
    return fc;
}

struct MiniStats {
    int    ticks = 0;
    double max_grid = -1e18;
    double min_grid =  1e18;
    double e_chg = 0.0, e_dis = 0.0;
    double soc_min = 1e18, soc_max = -1e18;
    int    out_of_interval = 0;
};

static MiniStats run_mini(const EmsConfig& cfg, double hours, double dt) {
    EmsRuntime rt;
    rt.init();
    ApplyOptions opt;
    opt.inject_plant = true;
    ConfigDiagnostics d = apply_config(rt, cfg, opt);
    if (!d.ok()) {
        std::cerr << "  (apply_config 失败)\n" << d.to_text();
    }
    rt.set_forecast(make_fc());

    MiniStats s;
    const int n = static_cast<int>(std::lround(hours * 3600.0 / dt));
    rt.run(n, dt, [dt](EmsRuntime& rr, int i) {
        const double h = std::fmod(static_cast<double>(i) * dt, 86400.0) / 3600.0;
        rr.set_environment(t_load(h), t_pv(h));
    });
    for (const auto& r : rt.log()) {
        s.ticks++;
        s.max_grid = std::max(s.max_grid, r.p_grid);
        s.min_grid = std::min(s.min_grid, r.p_grid);
        s.soc_min  = std::min(s.soc_min,  r.soc);
        s.soc_max  = std::max(s.soc_max,  r.soc);
        if (r.p_actual < 0.0) s.e_chg += -r.p_actual * dt / 3600.0;
        else                  s.e_dis +=  r.p_actual * dt / 3600.0;
        if (r.p_cmd < r.p_lower - 1e-9 || r.p_cmd > r.p_upper + 1e-9) s.out_of_interval++;
    }
    return s;
}

// =====================================================================
int main() {
    std::printf("=== P1 单元测试：配置化 ===\n\n");

    // -----------------------------------------------------------------
    std::printf("T201 JSON 解析：标量与容器\n");
    {
        json::ParseError e;
        json::Value v = json::parse("{\"a\": 1, \"b\": 2.5, \"c\": \"x\", "
                                    "\"d\": true, \"e\": [1,2,3], "
                                    "\"f\": {\"g\": null}}", &e);
        EXPECT(e.ok);
        EXPECT(v.is_object());
        EXPECT(v.find("a") && v.find("a")->is_number());
        EXPECT_NEAR(v.find("a")->number(), 1.0, 1e-12);
        EXPECT_NEAR(v.find("b")->number(), 2.5, 1e-12);
        EXPECT(v.find("c")->is_string() && v.find("c")->str == "x");
        EXPECT(v.find("d")->is_bool() && v.find("d")->b);
        EXPECT(v.find("e")->is_array() && v.find("e")->arr.size() == 3);
        EXPECT(v.find("f")->is_object());
        EXPECT(v.find("f")->find("g")->is_null());
        // 负数 / 科学计数
        json::Value w = json::parse("[-1.5e2, +3, 0.001]", &e);
        EXPECT(e.ok);
        EXPECT_NEAR(w.arr[0].number(), -150.0, 1e-9);
        EXPECT_NEAR(w.arr[1].number(), 3.0, 1e-12);
        EXPECT_NEAR(w.arr[2].number(), 0.001, 1e-12);
    }

    // -----------------------------------------------------------------
    std::printf("T202 JSON 放宽语法：注释 / 尾逗号 / 裸键 / 裸标识符\n");
    {
        json::ParseError e;
        const char* txt =
            "{\n"
            "  # 井号行注释\n"
            "  soc_min: 0.1,        // 行注释\n"
            "  /* 块注释 */\n"
            "  mode: auto,\n"
            "  list: [1, 2, 3,],\n"      // 尾逗号
            "  obj: { a: 1, },\n"
            "}\n";
        json::Value v = json::parse(txt, &e);
        EXPECT(e.ok);
        EXPECT(v.is_object());
        EXPECT_NEAR(v.find("soc_min")->number(), 0.1, 1e-12);
        EXPECT(v.find("mode")->is_string() && v.find("mode")->str == "auto");
        EXPECT(v.find("list")->is_array() && v.find("list")->arr.size() == 3);
        EXPECT(v.find("obj")->is_object() && v.find("obj")->has("a"));
    }

    // -----------------------------------------------------------------
    std::printf("T203 JSON 错误定位：报错带 行:列\n");
    {
        json::ParseError e;
        json::parse("{\n  \"a\": 1,\n  \"b\" 2\n}", &e);   // 缺冒号
        EXPECT(!e.ok);
        EXPECT(e.line == 3);            // 第 3 行
        EXPECT(!e.message.empty());
        EXPECT(e.to_string().find("3:") != std::string::npos);

        json::ParseError e2;
        json::parse("{\"a\": [1, 2}", &e2);                 // 数组未闭合
        EXPECT(!e2.ok);

        json::ParseError e3;
        json::parse("{", &e3);
        EXPECT(!e3.ok);

        json::ParseError e4;
        json::parse("{\"a\": 1} trailing", &e4);            // 末尾多余内容
        EXPECT(!e4.ok);
    }

    // -----------------------------------------------------------------
    std::printf("T204 JSON 往返：dump → parse 一致\n");
    {
        json::ParseError e;
        const std::string src =
            "{\"a\":1,\"b\":[1,2,3],\"c\":{\"d\":\"e\\nf\"},\"g\":true,\"h\":null}";
        json::Value v1 = json::parse(src, &e);
        EXPECT(e.ok);
        const std::string dumped = json::dump(v1, 2);
        json::Value v2 = json::parse(dumped, &e);
        EXPECT(e.ok);
        EXPECT(json::dump(v2, 2) == dumped);                 // 幂等
        EXPECT(v2.find("c")->find("d")->str == "e\nf");      // 转义正确
        // 整数不带小数点
        json::Value iv = json::parse("[1, 2.5]", &e);
        const std::string d2 = json::dump(iv, 0);
        EXPECT(d2.find("1") != std::string::npos);
        EXPECT(d2.find("2.5") != std::string::npos);
    }

    // -----------------------------------------------------------------
    std::printf("T205 字段绑定：类型不符报错 / 未知键警告\n");
    {
        ConfigDiagnostics d;
        EmsConfig cfg = from_json(
            json::parse("{\"safety\": {\"soc_min\": \"abc\", \"nonsense\": 1}}", nullptr), &d);
        EXPECT(!d.ok());                                    // soc_min 类型不符
        bool found_type_err = false, found_unknown = false;
        for (const auto& i : d.issues) {
            if (i.path == "safety.soc_min" && i.level == ConfigIssue::Level::kError) found_type_err = true;
            if (i.path == "safety.nonsense" && i.level == ConfigIssue::Level::kWarning) found_unknown = true;
        }
        EXPECT(found_type_err);
        EXPECT(found_unknown);
        // 出错字段保持默认值，不被污染
        EmsConfig def;
        EXPECT_NEAR(cfg.safety.soc_min, def.safety.soc_min, 1e-12);
    }

    // -----------------------------------------------------------------
    std::printf("T206 类型容忍：字符串数字 / on-off / 1-0\n");
    {
        ConfigDiagnostics d;
        EmsConfig cfg = from_json(json::parse(
            "{\"safety\": {\"soc_min\": \"0.25\"},"
            " \"loop\": {\"enable_log\": \"on\", \"enable_safety_engine\": \"false\","
            "           \"log_every\": 3},"
            " \"state_machine\": {\"allow_ready_output\": 1}}", nullptr), &d);
        EXPECT(d.ok());
        EXPECT_NEAR(cfg.safety.soc_min, 0.25, 1e-12);
        EXPECT(cfg.loop.enable_log);                        // "on" → true
        EXPECT(!cfg.loop.enable_safety_engine);             // "false" → false
        EXPECT(cfg.loop.log_every == 3);
        EXPECT(cfg.fsm.allow_ready_output);                 // 1 → true
    }

    // -----------------------------------------------------------------
    std::printf("T207 缺省语义：缺 section / 缺字段 → 保留默认值\n");
    {
        ConfigDiagnostics d;
        EmsConfig cfg = from_json(json::parse("{\"safety\": {\"soc_min\": 0.22}}", nullptr), &d);
        EXPECT(d.ok());
        EmsConfig def;
        EXPECT_NEAR(cfg.safety.soc_min, 0.22, 1e-12);                    // 被覆盖
        EXPECT_NEAR(cfg.safety.soc_max, def.safety.soc_max, 1e-12);      // 同节其它字段 → 默认
        EXPECT_NEAR(cfg.loop.dt_s, def.loop.dt_s, 1e-12);                // 缺整节 → 默认
        EXPECT_NEAR(cfg.plant.battery_capacity_kwh,
                    def.plant.battery_capacity_kwh, 1e-12);
        EXPECT(cfg.strategies.empty());                                  // 无策略
        // 空文档 → 全默认
        ConfigDiagnostics d2;
        EmsConfig empty = from_json(json::parse("{}", nullptr), &d2);
        EXPECT(d2.ok());
        EXPECT_NEAR(empty.loop.dt_s, def.loop.dt_s, 1e-12);
        EXPECT(empty.name == def.name);
    }

    // -----------------------------------------------------------------
    std::printf("T208 全配置往返：to_json → from_json 幂等\n");
    {
        EmsConfig a = make_test_config();
        a.strategies["anti_reverse"].enabled = true;
        a.strategies["anti_reverse"].note    = "防逆流";
        a.strategies["anti_reverse"].params["margin_kw"] = 5.0;
        a.strategies["anti_reverse"].params["alpha"]     = 0.3;
        a.strategies["demand_mgmt"].enabled = false;

        const std::string s1 = json::dump(to_json(a), 2);
        ConfigDiagnostics d;
        EmsConfig b = from_json(json::parse(s1, nullptr), &d);
        EXPECT(d.ok());
        const std::string s2 = json::dump(to_json(b), 2);
        EXPECT(s1 == s2);

        // 逐字段抽查（覆盖 6 个 section + 策略）
        EXPECT(b.name == a.name && b.site == a.site && b.version == a.version);
        EXPECT_NEAR(b.loop.output_deadband_kw, a.loop.output_deadband_kw, 1e-12);
        EXPECT(b.loop.output_switch_delay == a.loop.output_switch_delay);
        EXPECT_NEAR(b.plant.battery_capacity_kwh, a.plant.battery_capacity_kwh, 1e-12);
        EXPECT(b.plant.seed == a.plant.seed);
        EXPECT_NEAR(b.limits.d_target_kw, a.limits.d_target_kw, 1e-12);
        EXPECT_NEAR(b.safety.grid_p_min_kw, a.safety.grid_p_min_kw, 1e-12);
        EXPECT_NEAR(b.coord.reopt_period_s, a.coord.reopt_period_s, 1e-12);
        EXPECT(!b.fsm.allow_ready_output);
        EXPECT(b.strategies.size() == 2);
        EXPECT_NEAR(b.strategies["anti_reverse"].params.at("margin_kw"), 5.0, 1e-12);
        EXPECT(!b.strategies["demand_mgmt"].enabled);
        EXPECT(b.strategies["anti_reverse"].note == "防逆流");

        // 绑定表规模（回归护栏：字段数意外减少说明漏绑）
        Binder bl, bp, bd, bs, bc, bf;
        EmsConfig t;
        bind_fields(bl, t.loop);   bind_fields(bp, t.plant);
        bind_fields(bd, t.limits); bind_fields(bs, t.safety);
        bind_fields(bc, t.coord);  bind_fields(bf, t.fsm);
        const size_t total = bl.fields().size() + bp.fields().size()
                           + bd.fields().size() + bs.fields().size()
                           + bc.fields().size() + bf.fields().size();
        EXPECT(total == 89);
    }

    // -----------------------------------------------------------------
    std::printf("T210 语义校验：默认配置零错误\n");
    {
        EmsConfig def;
        ConfigDiagnostics d = validate(def);
        if (!d.ok()) std::printf("%s", d.to_text().c_str());
        EXPECT(d.ok());

        EmsConfig mine = make_test_config();
        ConfigDiagnostics d2 = validate(mine);
        if (!d2.ok()) std::printf("%s", d2.to_text().c_str());
        EXPECT(d2.ok());
    }

    // -----------------------------------------------------------------
    std::printf("T209 语义校验：逐类错误能被抓出\n");
    {
        auto has_error = [](const ConfigDiagnostics& d, const std::string& path) {
            for (const auto& i : d.issues)
                if (i.level == ConfigIssue::Level::kError && i.path == path) return true;
            return false;
        };
        { EmsConfig c; c.loop.dt_s = 0.0;
          EXPECT(has_error(validate(c), "loop.dt_s")); }
        { EmsConfig c; c.loop.demand_window_s = 0.5;   // < dt_s(0.1)? 不；设 dt_s 更大
          c.loop.dt_s = 1.0;
          EXPECT(has_error(validate(c), "loop.demand_window_s")); }
        { EmsConfig c; c.loop.log_every = 0;
          EXPECT(has_error(validate(c), "loop.log_every")); }
        { EmsConfig c; c.plant.battery_capacity_kwh = 0.0;
          EXPECT(has_error(validate(c), "plant.battery_capacity_kwh")); }
        { EmsConfig c; c.plant.eta_chg = 1.5;
          EXPECT(has_error(validate(c), "plant.eta_chg")); }
        { EmsConfig c; c.plant.soc_phys_min = 0.9; c.plant.soc_phys_max = 0.1;
          EXPECT(has_error(validate(c), "plant.soc_phys_min")); }
        { EmsConfig c; c.plant.soc_init = 0.99;        // 超出物理上限 0.98
          EXPECT(has_error(validate(c), "plant.soc_init")); }
        { EmsConfig c; c.limits.transformer_capacity_kw = 0.0;
          EXPECT(has_error(validate(c), "limits.transformer_capacity_kw")); }
        { EmsConfig c; c.safety.soc_min = 0.9; c.safety.soc_max = 0.5;
          EXPECT(has_error(validate(c), "safety.soc_min")); }
        { EmsConfig c; c.safety.soc_warn_low = 0.5; c.safety.soc_warn_high = 0.3;
          EXPECT(has_error(validate(c), "safety.soc_warn_low")); }
        { EmsConfig c; c.safety.soc_min = 0.01;        // 低于物理下限 0.05
          EXPECT(has_error(validate(c), "safety.soc_min")); }
        { EmsConfig c; c.safety.soc_max = 0.99;        // 高于物理上限 0.98
          EXPECT(has_error(validate(c), "safety.soc_max")); }
        { EmsConfig c; c.safety.temp_warn_c = 80.0; c.safety.temp_fault_c = 60.0;
          EXPECT(has_error(validate(c), "safety.temp_warn_c")); }
        { EmsConfig c; c.safety.grid_p_min_kw = 100.0; c.safety.grid_p_max_kw = 50.0;
          EXPECT(has_error(validate(c), "safety.grid_p_min_kw")); }
        { EmsConfig c; c.safety.grid_filter_alpha = 0.0;
          EXPECT(has_error(validate(c), "safety.grid_filter_alpha")); }
        { EmsConfig c; c.safety.ramp_kw_per_s = 0.0;
          EXPECT(has_error(validate(c), "safety.ramp_kw_per_s")); }
        { EmsConfig c; c.safety.grid_lookahead_max_drop_kw = -1.0;
          EXPECT(has_error(validate(c), "safety.grid_lookahead_max_drop_kw")); }
        { EmsConfig c; c.safety.tr_overload_th = 1.2; c.safety.tr_extreme_th = 1.1;
          EXPECT(has_error(validate(c), "safety.tr_extreme_th")); }
        { EmsConfig c; c.coord.reopt_period_s = 0.0;
          EXPECT(has_error(validate(c), "coordinator.reopt_period_s")); }
        { EmsConfig c; c.fsm.init_hold_s = -1.0;
          EXPECT(has_error(validate(c), "state_machine.init_hold_s")); }
        { EmsConfig c; c.fsm.self_check_cycles = -1;
          EXPECT(has_error(validate(c), "state_machine.self_check_cycles")); }

        // 警告类（不应报错，但要出现 warning）
        auto has_warn = [](const ConfigDiagnostics& d, const std::string& path) {
            for (const auto& i : d.issues)
                if (i.level == ConfigIssue::Level::kWarning && i.path == path) return true;
            return false;
        };
        { EmsConfig c; c.loop.enable_state_machine = false;
          EXPECT(has_warn(validate(c), "loop.enable_state_machine")); }
        { EmsConfig c; c.fsm.allow_ready_output = true;
          EXPECT(has_warn(validate(c), "state_machine.allow_ready_output")); }
        { EmsConfig c; c.limits.d_target_kw = 9999.0;
          EXPECT(has_warn(validate(c), "limits.d_target_kw")); }
    }

    // -----------------------------------------------------------------
    std::printf("T211 装配顺序：device_limits 不被 configure_plant 冲掉（陷阱①）\n");
    {
        EmsConfig cfg = make_test_config();

        // 反例：错误顺序 —— 先设 limits，再 configure_plant
        //   configure_plant() → refresh_device_limits() → read_limits(dev_)
        //   read_limits 内部 out = DeviceLimits{} 会把 transformer/d_target 复位
        EmsRuntime bad;
        bad.init();
        bad.device_limits() = cfg.limits;
        bad.configure_plant(cfg.plant);
        EXPECT(std::fabs(bad.device_limits().transformer_capacity_kw
                         - cfg.limits.transformer_capacity_kw) > 1.0);   // 被冲掉
        EXPECT(std::fabs(bad.device_limits().d_target_kw
                         - cfg.limits.d_target_kw) > 1.0);               // 被冲掉
        EXPECT_NEAR(bad.device_limits().pcs_rated_dis_kw,
                    cfg.plant.pcs_max_dis_kw, 1e-9);                     // 变成 plant 的值

        // 正例：apply_config() 内部顺序正确
        EmsRuntime good;
        good.init();
        ConfigDiagnostics d = apply_config(good, cfg, ApplyOptions());
        EXPECT(d.ok());
        EXPECT_NEAR(good.device_limits().transformer_capacity_kw,
                    cfg.limits.transformer_capacity_kw, 1e-9);
        EXPECT_NEAR(good.device_limits().d_target_kw,
                    cfg.limits.d_target_kw, 1e-9);
        EXPECT_NEAR(good.device_limits().pcs_rated_dis_kw,
                    cfg.limits.pcs_rated_dis_kw, 1e-9);
        EXPECT_NEAR(good.device_limits().pcs_rated_chg_kw,
                    cfg.limits.pcs_rated_chg_kw, 1e-9);
        EXPECT_NEAR(good.device_limits().bms_dis_limit_kw,
                    cfg.limits.bms_dis_limit_kw, 1e-9);
    }

    // -----------------------------------------------------------------
    std::printf("T212 装配顺序：OutputShaper 拾取配置（陷阱③）\n");
    {
        EmsConfig cfg = make_test_config();
        EmsRuntime rt;
        rt.init();
        // init() 里缓存的是结构体默认值 2.0 / 3
        EXPECT_NEAR(rt.shaper().deadband(), 2.0, 1e-12);
        EXPECT(rt.shaper().switch_delay() == 3);

        ConfigDiagnostics d = apply_config(rt, cfg, ApplyOptions());
        EXPECT(d.ok());
        // 必须跟着配置走
        EXPECT_NEAR(rt.shaper().deadband(), cfg.loop.output_deadband_kw, 1e-12);
        EXPECT(rt.shaper().switch_delay() == cfg.loop.output_switch_delay);

        // 行为验证：死区 7.5 kW 时，5 kW 的指令应被清零（连续 5 拍确认后）
        bool held = false;
        double out = 5.0;
        for (int i = 0; i < cfg.loop.output_switch_delay; ++i) {
            out = rt.shaper().shape(5.0, &held);
        }
        EXPECT_NEAR(out, 0.0, 1e-12);

        // 换成小死区 → 同样 5 kW 应原样通过
        EmsConfig cfg2 = cfg;
        cfg2.loop.output_deadband_kw = 1.0;
        EmsRuntime rt2;
        rt2.init();
        ConfigDiagnostics d2 = apply_config(rt2, cfg2, ApplyOptions());
        EXPECT(d2.ok());
        EXPECT_NEAR(rt2.shaper().shape(5.0, &held), 5.0, 1e-12);
    }

    // -----------------------------------------------------------------
    std::printf("T213 装配顺序：协同层设备参数与 dev_ 一致（陷阱②）\n");
    {
        EmsConfig cfg = make_test_config();
        EmsRuntime rt;
        rt.init();
        ConfigDiagnostics d = apply_config(rt, cfg, ApplyOptions());
        EXPECT(d.ok());
        // 协同层持有的设备参数应与 dev_ 一致（apply_configs 在 device_limits 之后调用）
        EXPECT_NEAR(rt.coordinator().device_p_chg_max_kw(),
                    rt.device_limits().pcs_rated_chg_kw, 1e-9);
        EXPECT_NEAR(rt.coordinator().device_p_dis_max_kw(),
                    rt.device_limits().pcs_rated_dis_kw, 1e-9);
        EXPECT_NEAR(rt.coordinator().device_soc_min(),
                    rt.safety_params().soc_min, 1e-9);
        EXPECT_NEAR(rt.coordinator().device_soc_max(),
                    rt.safety_params().soc_max, 1e-9);
        // 安全参数也已生效
        EXPECT_NEAR(rt.safety_params().soc_min, cfg.safety.soc_min, 1e-12);
        EXPECT_NEAR(rt.fsm_config().init_hold_s, cfg.fsm.init_hold_s, 1e-12);
    }

    // -----------------------------------------------------------------
    std::printf("T214 策略装配：未知 id 报错 / 开关与参数生效\n");
    {
        EmsConfig cfg = make_test_config();
        cfg.strategies["no_such_strategy"].enabled = true;
        EmsRuntime rt;
        rt.init();
        ConfigDiagnostics d = apply_config(rt, cfg, ApplyOptions());
        EXPECT(!d.ok());                                    // 未知 id → 错误
        bool found = false;
        for (const auto& i : d.issues)
            if (i.path == "strategies.no_such_strategy") found = true;
        EXPECT(found);

        // 正确 id：开关与参数生效
        EmsConfig ok = make_test_config();
        const std::string id = rt.manager().list_ids().empty()
                             ? std::string("anti_reverse")
                             : rt.manager().list_ids().front();
        ok.strategies[id].enabled = true;
        ok.strategies[id].params["__weight__"] = 123.0;    // 内部键必须被忽略
        ok.strategies[id].params["margin_kw"]  = 8.5;
        EmsRuntime rt2;
        rt2.init();
        ConfigDiagnostics d2 = apply_config(rt2, ok, ApplyOptions());
        EXPECT(d2.ok());
        EXPECT(rt2.manager().get_status(id).enabled);
        StrategyPtr sp = rt2.manager().get_strategy(id);
        EXPECT(sp != nullptr);
        if (sp) {
            EXPECT_NEAR(sp->get_param("margin_kw"), 8.5, 1e-12);
            EXPECT(sp->get_param("__weight__", -1.0) != 123.0);   // 内部键未被覆盖
        }

        // disable 也要生效
        EmsConfig off = make_test_config();
        off.strategies[id].enabled = false;
        EmsRuntime rt3;
        rt3.init();
        ConfigDiagnostics d3 = apply_config(rt3, off, ApplyOptions());
        EXPECT(d3.ok());
        EXPECT(!rt3.manager().get_status(id).enabled);
    }

    // -----------------------------------------------------------------
    std::printf("T215 导出回灌：capture → save → load → apply 行为一致\n");
    {
        EmsConfig cfg = make_test_config();
        EmsRuntime rt;
        rt.init();
        ConfigDiagnostics d = apply_config(rt, cfg, ApplyOptions());
        EXPECT(d.ok());

        EmsConfig cap = capture_config(rt);
        EXPECT(!cap.strategies.empty());                    // 策略被导出
        EXPECT_NEAR(cap.limits.transformer_capacity_kw,
                    cfg.limits.transformer_capacity_kw, 1e-9);
        EXPECT_NEAR(cap.loop.output_deadband_kw,
                    cfg.loop.output_deadband_kw, 1e-9);

        // 落盘 → 读回 → 再装配，行为一致
        const std::string path = "build/_t215_captured.json";
        EXPECT(save_config_file(path, cap));

        EmsConfig reloaded;
        ConfigDiagnostics ld;
        EXPECT(load_config_file(path, &reloaded, &ld));
        EXPECT(ld.ok());

        EmsRuntime rt2;
        rt2.init();
        ConfigDiagnostics d2 = apply_config(rt2, reloaded, ApplyOptions());
        EXPECT(d2.ok());
        EXPECT_NEAR(rt2.device_limits().transformer_capacity_kw,
                    rt.device_limits().transformer_capacity_kw, 1e-9);
        EXPECT_NEAR(rt2.device_limits().pcs_rated_dis_kw,
                    rt.device_limits().pcs_rated_dis_kw, 1e-9);
        EXPECT_NEAR(rt2.shaper().deadband(), rt.shaper().deadband(), 1e-12);
    }

    // -----------------------------------------------------------------
    std::printf("T216 端到端：改配置 → 行为改变\n");
    {
        EmsConfig a = make_test_config();
        a.limits.transformer_capacity_kw = 630.0;
        a.limits.d_target_kw             = 320.0;
        a.safety.grid_p_max_kw           = 630.0;

        EmsConfig b = a;
        b.limits.transformer_capacity_kw = 200.0;
        b.limits.d_target_kw             = 200.0;
        b.safety.grid_p_max_kw           = 200.0;

        MiniStats sa = run_mini(a, 24.0, 1.0);
        MiniStats sb = run_mini(b, 24.0, 1.0);

        std::printf("      基线 关口峰值 %.1f kW / 收紧 关口峰值 %.1f kW\n",
                    sa.max_grid, sb.max_grid);
        std::printf("      基线 充/放 %.1f/%.1f kWh / 收紧 充/放 %.1f/%.1f kWh\n",
                    sa.e_chg, sa.e_dis, sb.e_chg, sb.e_dis);

        EXPECT(sa.ticks > 80000);
        EXPECT(sb.ticks > 80000);
        // 收紧关口上限 → 峰值必须下降
        EXPECT(sb.max_grid < sa.max_grid - 1.0);
        // 硬不变量：任何配置下都不得出现指令逃逸
        EXPECT(sa.out_of_interval == 0);
        EXPECT(sb.out_of_interval == 0);
        // 收紧后储能可用空间变小 → 充电量应显著下降
        EXPECT(sb.e_chg < sa.e_chg);
    }

    // -----------------------------------------------------------------
    std::printf("T217 关键项缺省提示（整节缺省不逐字段提示）\n");
    {
        ConfigDiagnostics d;
        // 只给 dt_s，其余关键项缺省
        EmsConfig cfg = from_json(json::parse("{\"loop\": {\"dt_s\": 0.5}}", nullptr), &d);
        EXPECT(d.ok());                                     // 只是警告，不是错误
        bool warned_demand = false;
        for (const auto& i : d.issues) {
            if (i.path == "loop.demand_window_s") warned_demand = true;
        }
        EXPECT(warned_demand);
        EXPECT_NEAR(cfg.loop.dt_s, 0.5, 1e-12);
        // 缺省项应保持默认值
        EmsConfig def;
        EXPECT_NEAR(cfg.loop.demand_window_s, def.loop.demand_window_s, 1e-12);

        // 语义约定：**整节缺省** = "这一层全用默认值"，不逐字段刷提示；
        // 只有"配了这节但漏了关键键"才提示。
        bool any_plant_warn = false;
        for (const auto& i : d.issues) {
            if (i.path.rfind("plant.", 0) == 0) any_plant_warn = true;
        }
        EXPECT(!any_plant_warn);

        ConfigDiagnostics d2;
        from_json(json::parse("{\"plant\": {\"soc_init\": 0.4}}", nullptr), &d2);
        bool warned_batt = false, warned_pcs = false;
        for (const auto& i : d2.issues) {
            if (i.path == "plant.battery_capacity_kwh") warned_batt = true;
            if (i.path == "plant.pcs_max_dis_kw")       warned_pcs = true;
        }
        EXPECT(warned_batt);                                // 配了 plant 节但漏了关键项
        EXPECT(warned_pcs);

        ConfigDiagnostics d3;
        from_json(json::parse("{\"safety\": {\"soc_min\": 0.2}}", nullptr), &d3);
        bool warned_grid = false;
        for (const auto& i : d3.issues) {
            if (i.path == "safety.grid_p_max_kw") warned_grid = true;
        }
        EXPECT(warned_grid);
    }

    // -----------------------------------------------------------------
    std::printf("T218 文档生成：模板 / Markdown / Schema\n");
    {
        EmsRuntime rt;
        rt.init();
        EmsConfig base = make_test_config();

        const std::string tmpl = render_config_template(&rt, base);
        EXPECT(!tmpl.empty());
        EXPECT(tmpl.find("\"soc_min\"") != std::string::npos);
        EXPECT(tmpl.find("\"d_target_kw\"") != std::string::npos);
        EXPECT(tmpl.find("//") != std::string::npos);            // 带注释
        EXPECT(tmpl.find("strategies") != std::string::npos);
        // 模板必须能被解析（注释/尾逗号合法）
        ConfigDiagnostics td;
        EmsConfig parsed;
        EXPECT(parse_config_text(tmpl, &parsed, &td));
        EXPECT(td.ok());
        EXPECT_NEAR(parsed.limits.d_target_kw, base.limits.d_target_kw, 1e-9);
        EXPECT_NEAR(parsed.loop.output_deadband_kw, base.loop.output_deadband_kw, 1e-9);

        const std::string md = render_config_markdown(&rt, base);
        EXPECT(!md.empty());
        EXPECT(md.find("| `soc_min` |") != std::string::npos);
        EXPECT(md.find("safety") != std::string::npos);
        EXPECT(md.find("strategies") != std::string::npos);

        json::Value schema = render_config_schema(&rt);
        EXPECT(schema.is_object());
        EXPECT(schema.find("safety") && schema.find("safety")->is_array());
        EXPECT(schema.find("strategy_ids") && schema.find("strategy_ids")->is_array());
        // 每个字段条目都要有 key/unit/desc/required/default
        const json::Value* sec = schema.find("safety");
        EXPECT(sec->arr.size() > 0);
        EXPECT(sec->arr[0].has("key") && sec->arr[0].has("default")
               && sec->arr[0].has("required"));

        // 文档字段数与绑定表一致（防止文档漂移）
        size_t schema_total = 0;
        for (const char* k : {"loop", "plant", "limits", "safety", "coordinator", "state_machine"}) {
            const json::Value* s = schema.find(k);
            if (s) schema_total += s->arr.size();
        }
        EXPECT(schema_total == 89);
    }

    // -----------------------------------------------------------------
    std::printf("\n===== 结果 =====\n");
    std::printf("  通过 %d / 失败 %d\n", g_pass, g_fail);
    if (g_fail == 0) std::printf("=== ALL TESTS PASSED ===\n");
    else             std::printf("=== TESTS FAILED ===\n");
    return g_fail == 0 ? 0 : 1;
}
