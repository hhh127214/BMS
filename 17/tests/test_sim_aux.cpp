// =====================================================================
// 17/tests/test_sim_aux.cpp —— 选配模块模拟器（消防 / 温控 / 门禁安防 / 配电开关）
//
// 背景：映射表盘出的 17 个缺口（消防 5 / 温控 7 / 门禁 2 / 配电开关 3）
//   已落为 `data/mod_*.json`。但**点定义了 ≠ 有数** —— 这四个模拟器才是数据源。
//
//   T01  消防：正常态（状态位 0，压力有缓慢漂移）
//   T02  消防：烟感动作 → 火警 → 延时后喷洒释放
//   T03  消防：喷洒**锁存**（探测器复位后仍为 1）
//   T04  消防：通信中断 → 故障，且优先级高于火警
//   T05  温控：无制冷时舱温按热模型上升并趋向平衡点
//   T06  温控：带回差的温控器翻转水泵；制冷后舱温被压下来
//   T07  温控：流量有建立过程（不是阶跃），停泵回零
//   T08  温控：水温一阶滞后的接近过程 + 稳态供水低于设定值
//   T09  门禁：门磁可翻转；水浸**锁存**且需显式复位
//   T10  配电：急停按下 → 分闸 + 原因码；未复位时合闸被拒
//   T11  点名交叉核对（对着**加载后的整表**）
//   T12  全部在动自证 + 反向守卫
//
// 编译：见 17/scripts/build_test.bat
// =====================================================================

#include "expect.h"

#include "aux_sim.h"
#include "device_point_table.h"
#include "point_sink.h"
#include "point_table_loader.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace ems;
using namespace ems::devsim;
using namespace ems::devpt;

static int g_pass = 0;
static int g_fail = 0;

static const double kPi = 3.14159265358979323846;

// =====================================================================
// 装配整张点表（内置 107 + 四个选配模块），供交叉核对使用
// =====================================================================
static DevicePointTable load_full_table() {
    DevicePointTable t = DevicePointTable::builtin();
    // ★ 顺序即索引：站点追加点先落，再落四个选配模块（与 dev_points.exe 默认一致）
    const char* mods[] = {"data/append_points.json",
                          "data/mod_fire.json", "data/mod_hvac.json",
                          "data/mod_sec.json",  "data/mod_switch.json"};
    for (const char* f : mods) {
        const LoadError e = t.load_file(f, true);
        EXPECT(e.ok);
    }
    return t;
}

// =====================================================================
// T01 消防：正常态
// =====================================================================
static void test_01_fire_normal() {
    std::printf("T01 消防：正常态（state=0，压力缓慢漂移）\n");

    FireSim f;
    TraceSink s;
    f.publish_static(s);
    for (int k = 0; k < 1200; ++k) f.step(k * 0.5, 0.5, s);   // 600 s

    EXPECT(s.has(auxpt::fire_state()));
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::fire_state())->max_v), 0);   // 始终正常
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::fire_smoke())->max_v), 0);
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::fire_released())->max_v), 0);

    // 压力在动（正弦漂移），否则"压力正常"这条断言等于没断言
    EXPECT(s.spread(auxpt::fire_press()) > 0.002);
}

// =====================================================================
// T02 消防：烟感动作 → 火警 → 延时到 → 喷洒
// =====================================================================
static void test_02_fire_release_delay() {
    std::printf("T02 消防：烟感动作后延时 5 s 才喷洒\n");

    FireSim f;
    TraceSink s;
    f.mutable_cfg().smoke_detected = true;
    f.mutable_cfg().release_delay_s = 5.0;

    // 报警后第 2 拍（t=1.0s）还没喷
    for (int k = 0; k < 2; ++k) f.step(k * 0.5, 0.5, s);
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::fire_state())->last), 1);      // 已进火警
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::fire_released())->last), 0);   // 但还没喷

    // 走到 6 s → 已喷
    for (int k = 2; k < 12; ++k) f.step(k * 0.5, 0.5, s);
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::fire_released())->last), 1);
}

// =====================================================================
// T03 消防：喷洒锁存
// =====================================================================
static void test_03_fire_release_latch() {
    std::printf("T03 消防：喷洒锁存（探测器复位后仍为 1）\n");

    FireSim f;
    TraceSink s;
    f.mutable_cfg().smoke_detected = true;
    for (int k = 0; k < 20; ++k) f.step(k * 0.5, 0.5, s);
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::fire_released())->last), 1);

    // 探测器复位（误报被排除）
    f.mutable_cfg().smoke_detected = false;
    for (int k = 20; k < 40; ++k) f.step(k * 0.5, 0.5, s);
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::fire_released())->last), 1);  // 仍然锁存

    // 只有显式复位才清除
    f.reset();
    f.step(0.0, 0.5, s);
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::fire_released())->last), 0);
}

// =====================================================================
// T04 消防：通信中断 → 故障，且优先级最高
// =====================================================================
static void test_04_fire_comm_fault_priority() {
    std::printf("T04 消防：通信中断 = 故障，优先级高于火警\n");

    FireSim f;
    TraceSink s;
    f.mutable_cfg().smoke_detected = true;
    f.step(0.0, 0.5, s);
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::fire_state())->last), 1);   // 火警

    f.mutable_cfg().comm_ok = false;                                    // 同时断通信
    f.step(0.5, 0.5, s);
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::fire_state())->last), 2);   // 故障优先

    // 恢复通信后回到火警（喷洒已锁存 ⇒ 仍是火警态）
    f.mutable_cfg().comm_ok = true;
    f.step(1.0, 0.5, s);
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::fire_state())->last), 1);
}

// =====================================================================
// T05 温控：无制冷时舱温上升并趋向平衡点
// =====================================================================
static void test_05_hvac_heat_up() {
    std::printf("T05 温控：无制冷时舱温按一阶热模型上升\n");

    HvacSimConfig c;
    c.t_init_c = 28.0;
    c.t_out_c  = 30.0;
    HvacSim h(c);
    TraceSink s;

    // 强制停机（onoff=0）：只有热源 + 自然散热
    for (int k = 0; k < 2400; ++k) h.step(k * 0.5, 0.5, s, false, 25.0, 8.0);

    const double t_end = s.stat(auxpt::hvac_t_amb())->last;
    // 平衡温度 = T_out + Q/k = 30 + 8/0.35 ≈ 52.9 K；800 kJ/K 热容下 1200 s 还远未到
    EXPECT(t_end > 28.0);
    EXPECT(t_end < 52.9);
}

// =====================================================================
// T06 温控：带回差的温控器翻转水泵 + 制冷生效
// =====================================================================
static void test_06_hvac_thermostat() {
    std::printf("T06 温控：回差 ±0.5 K，水泵启停翻转且制冷压低舱温\n");

    HvacSimConfig c;
    c.t_init_c       = 34.0;   // 高于设定值 ⇒ 应立即启泵
    c.t_out_c        = 30.0;
    c.hyst_k         = 1.0;
    c.c_th_kj_per_k  = 400.0;  // 小舱体：热得快也冷得快 ⇒ 温控能来回翻转
    HvacSim h(c);
    TraceSink s;

    // 热源恒定 2 kW；制冷 6 kW 足以把舱温压到设定值以下 ⇒ 必然产生启停循环
    for (int k = 0; k < 3000; ++k) h.step(k * 0.5, 0.5, s, true, 25.0, 2.0);  // 1500 s

    const TraceSink::Stat* pump = s.stat(auxpt::hvac_pump());
    EXPECT(pump->min_v <= 0.0);
    EXPECT(pump->max_v >= 1.0);
    EXPECT(pump->changes >= 2);                       // ★ 反向守卫：确实翻转过

    // 制冷把舱温压到设定值附近（而不是一路涨到平衡温度 52.9）
    EXPECT(s.stat(auxpt::hvac_t_amb())->last < 32.0);
}

// =====================================================================
// T07 温控：流量建立过程
// =====================================================================
static void test_07_flow_ramp() {
    std::printf("T07 温控：流量有建立过程（不是阶跃）\n");

    HvacSim h;
    TraceSink s;

    h.step(0.0, 0.5, s, true, 25.0, 2.0);
    const double f1 = s.stat(auxpt::hvac_flow())->last;

    h.step(0.5, 0.5, s, true, 25.0, 2.0);
    const double f2 = s.stat(auxpt::hvac_flow())->last;

    EXPECT(f2 > f1);                 // 在爬升
    EXPECT(f2 < 40.0);               // 但还没到额定 ⇒ 说明确实在"爬"而非"跳"

    // 停泵后回零：也要有过程，且最终归零
    double last = f2;
    for (int k = 2; k < 200; ++k) { h.step(k * 0.5, 0.5, s, false, 25.0, 0.0); }
    EXPECT(std::fabs(s.stat(auxpt::hvac_flow())->last) < 1e-9);
    (void)last;
}

// =====================================================================
// T08 温控：水温滞后并低于设定值
// =====================================================================
static void test_08_water_lag() {
    std::printf("T08 温控：水温一阶滞后，稳态供水低于设定值\n");

    // ★ 工况为什么必须让水泵**一直开着**：
    //   水温的目标值 = (泵开 ? t_set−5 : t_amb)。泵一停，目标就变成舱温，
    //   水温随即向舱温漂移 —— 这是设计内的启停循环，不是可判收敛的稳态。
    //   第一版照 T06 的小热源口径写，舱温很快跌破 t_set−hyst/2 停泵，
    //   末态水温自然漂到 24.7 ℃，于是"收敛到 20 ℃"必然红。
    //   现在把热源取到与制冷量相当 ⇒ 舱温被顶在 T_out 附近（30 ℃），
    //   远高于 t_set+0.5 ⇒ 温控器不会停泵 ⇒ 水温有稳定目标可追。
    HvacSimConfig c;
    c.t_init_c      = 30.0;
    c.t_out_c       = 30.0;
    c.c_th_kj_per_k = 4000.0;   // 大热容 ⇒ 舱温几乎不动，避免抖动把泵来回打
    c.cool_kw       = 40.0;
    HvacSim h(c);
    TraceSink s;

    const double t_set         = 25.0;
    const double supply_target = t_set - 5.0;      // 供水比设定值更冷 5 K

    // ① 滞后：泵刚启动的头两拍只能"接近"目标，不能一步到位
    h.step(0.0, 0.5, s, true, t_set, 40.0);
    const double tw1 = s.stat(auxpt::hvac_t_water())->last;
    EXPECT(tw1 < 30.0);                    // 动了
    EXPECT(tw1 > supply_target + 5.0);     // 但离目标还远 ⇒ 确实是一阶滞后

    h.step(0.5, 0.5, s, true, t_set, 40.0);
    const double tw2 = s.stat(auxpt::hvac_t_water())->last;
    EXPECT(tw2 < tw1);                     // 单调下行

    // ② 稳态：2000 s = 100 个 water_lag(20 s) ⇒ 残差 e^-100，可忽略
    for (int k = 2; k < 4000; ++k) h.step(k * 0.5, 0.5, s, true, t_set, 40.0);
    const double tw = s.stat(auxpt::hvac_t_water())->last;
    EXPECT_NEAR(tw, supply_target, 0.5);

    // ③ 供水若比设定值还热就是控制失效。
    //   只在**稳态段**判：初值水温 = 舱温(30 ℃) 是"停机一夜后"的合理物理初值，
    //   把它算作失效会让这条断言永远不可能通过。
    EXPECT(tw < t_set);
    EXPECT(tw < s.stat(auxpt::hvac_t_amb())->last);
}

// =====================================================================
// T09 门禁安防：门磁翻转 + 水浸锁存
// =====================================================================
static void test_09_sec_latch() {
    std::printf("T09 门禁安防：门磁可翻转；水浸锁存且需显式复位\n");

    SecSimConfig c;
    c.door_auto_s = 100.0;                 // 每 100 s 开 25 s
    SecSim x(c);
    TraceSink s;
    for (int k = 0; k < 400; ++k) x.step(k * 0.5, 0.5, s);   // 200 s

    const TraceSink::Stat* door = s.stat(auxpt::sec_door());
    EXPECT(door->min_v <= 0.0);
    EXPECT(door->max_v >= 1.0);
    EXPECT(door->changes >= 2);

    // 水浸：注入一次后，即使撤掉输入也必须保持
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::sec_water())->max_v), 0);
    x.mutable_cfg().water_hit = true;
    x.step(200.0, 0.5, s);
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::sec_water())->last), 1);

    x.mutable_cfg().water_hit = false;
    x.step(200.5, 0.5, s);
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::sec_water())->last), 1);  // 锁存

    x.reset();
    x.step(201.0, 0.5, s);
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::sec_water())->last), 0);  // 显式复位才清
}

// =====================================================================
// T10 配电开关：急停 → 分闸 + 原因码；未复位时合闸被拒
//
// ★ 这是本文件最要紧的一条：急停按下后，合闸命令**必须**被拒绝。
//   如果这条没钉住，最危险的失效就是"急停按下、程序照样下发合闸"。
// =====================================================================
static void test_10_estop_blocks_close() {
    std::printf("T10 配电：急停按下 → 分闸 + 原因码；未复位时合闸被拒\n");

    SwitchSim w;
    TraceSink s;
    w.step(0.0, 0.5, s);
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::sw_breaker())->last), 1);   // 初始合闸
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::sw_estop())->last), 0);

    // ① 急停按下
    w.mutable_cfg().estop_pressed = true;
    const bool denied_1 = !w.request_close(0.5, 0.5, s, true);     // 请求合闸
    EXPECT(denied_1);                                           // ★ 必须被拒
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::sw_breaker())->last), 0);
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::sw_estop())->last), 1);
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::sw_trip())->last),
            static_cast<int>(SwitchSim::kReasonEstop));

    // ② 复位后合闸才被接受
    w.mutable_cfg().estop_pressed = false;
    const bool ok = w.request_close(1.0, 0.5, s, true);
    EXPECT(ok);
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::sw_breaker())->last), 1);
    EXPECT_EQ(static_cast<int>(s.stat(auxpt::sw_trip())->last),
            static_cast<int>(SwitchSim::kReasonNone));
}

// =====================================================================
// T11 点名交叉核对 —— 对着**加载后的整表**
//
// ★ 为什么这里不能沿用既有三个模拟器的写法（`dev_point_index_of`）：
//   既有写法查的是**内置点表**，而这 17 个点根本不在内置表里 ——
//   照抄会全部判"未知"，让人误以为"模拟器发错了点名"。
//   正确判据是：点名存在于**本站实际加载出来的那张表**里，且归属设备正确。
//   这条判据随柜型配置动态成立：没装消防的站点，消防点本就不该被发布。
// =====================================================================
static void test_11_names_in_loaded_table() {
    std::printf("T11 点名交叉核对（对着加载后的整张表）\n");

    const DevicePointTable t = load_full_table();
    EXPECT_EQ(t.size(), 128);

    FireSim f; HvacSim h; SecSim x; SwitchSim w;
    TraceSink s;
    f.publish_static(s);
    f.step(0.0, 0.5, s);            // 消防 5 点
    h.step(0.0, 0.5, s, true, 25.0, 2.0);  // 温控 7 点（含 CMD 回读）
    x.step(0.0, 0.5, s);              // 门禁 2 点
    w.step(0.0, 0.5, s);              // 配电 3 点

    int unknown = 0, wrong_dev = 0;
    for (const auto& n : s.names()) {
        const int i = t.index_of(n);
        if (i < 0) { ++unknown; std::printf("    加载后的点表里没有: %s\n", n.c_str()); continue; }
        const int dev = t.points[static_cast<std::size_t>(i)].device;
        //
        // ★ 判据为什么取 name 的**第二段**：点名口径是 `{点区}.{设备}.{测点}`，
        //   设备名不在开头。按前缀取会全判"不匹配"，让人误以为发布错了点。
        //   取第二段同时还能验出"点名写错设备段"这种最不易发现的错。
        std::vector<std::string> tk;
        std::size_t pos = 0, dot = 0;
        while ((dot = n.find('.', pos)) != std::string::npos) {
            tk.push_back(n.substr(pos, dot - pos));
            pos = dot + 1;
        }
        tk.push_back(n.substr(pos));

        // ★ 第二段是大写（`MEAS.FIRE.X`）而 `device_kind_name()` 是小写（`fire`），
        //   所以比较前统一转大写 —— 不然会全判"不匹配"，把对的说成错的。
        std::string dev_tok = tk.size() >= 3 ? tk[1] : std::string();
        std::string dev_nam = device_kind_name(dev);
        for (auto& ch : dev_tok) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
        for (auto& ch : dev_nam) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);

        const bool ok = (tk.size() >= 3) && (dev_tok == dev_nam);
        if (!ok) {
            ++wrong_dev;
            std::printf("    点名与设备类不匹配: %s -> %s\n", n.c_str(),
                        device_kind_name(dev));
        }
    }
    EXPECT_EQ(unknown, 0);
    EXPECT_EQ(wrong_dev, 0);
    // 消防 5 + 温控 7（含 2 个 CMD 回读）+ 门禁 2 + 配电 3 = 17
    EXPECT_EQ(static_cast<int>(s.point_count()), 17);
}

// =====================================================================
// T12 全部在动自证 + 反向守卫
//
// ★ 这条测试真正的价值，不只是"每个点都动过"：
//   它逼着工况必须跑完 **"发生 → 恢复" 完整回路**。
//
//   第一版工况只注入了"事件发生"（烟感动作、急停按下），四个点被判 frozen。
//   查下来**不是实现错，是工况没覆盖恢复路径**：
//     · 喷洒锁存后主机恒为火警 —— 没有现场复位这一步；
//     · 急停复位后断路器不会自己合上 —— 这是**正确的**安全语义，
//       要它动，就得显式发合闸命令。
//
//   一句话：只有"发生"没有"恢复"的自证，两头都不讨好 ——
//   既会把正确的锁存误判成缺陷（让人想去"修"掉锁存），
//   也会让"永远回不来"的实现蒙混过关（反正测试也不看恢复）。
//   所以下面的剧本对每个事件都显式走完恢复那一步。
//
// ★ 反向守卫（为什么光断言"在动"不够）：
//   恒为常量的点也会被上面的判据抓出来，但"动得对不对"另说 ——
//   各 T0x 里那些 `EXPECT_EQ(值)` 才是方向性判据，两者互补，缺一不可。
// =====================================================================
static void test_12_moving() {
    std::printf("T12 全部在动自证\n");

    FireSimConfig fc;
    // 小舱体（热得快也冷得快）⇒ 温控器在观测窗内真能来回翻转。
    // 若用默认 800 kJ/K，1500 s 内舱温跌不破回差下沿，水泵一开到底，
    // STA.HVAC.PUMP_ON 就是个常量 —— 那不叫"在动"。
    HvacSimConfig hc;
    hc.t_init_c      = 34.0;
    hc.t_out_c       = 30.0;
    hc.c_th_kj_per_k = 400.0;   // 与 T06 同口径（时间常数 τ=C/k≈1143 s）
    SecSimConfig sc;
    sc.door_auto_s = 60.0;
    FireSim f(fc); HvacSim h(hc); SecSim x(sc); SwitchSim w;
    TraceSink s;

    const double dt = 0.5;
    for (int k = 0; k < 4000; ++k) {                 // 2000 s
        const double t = k * dt;

        // 消防：前 200 拍正常 → k=200..399 误报（延时到 ⇒ 喷洒锁存）
        //      → k=500 现场复位（清锁存 + 复压）
        f.mutable_cfg().smoke_detected = (k >= 200 && k < 400);
        if (k == 500) f.reset_alarm();
        f.step(t, dt, s);

        // 温控：热源周期波动 ⇒ 舱温穿越回差带 ⇒ 水泵启停
        //
        // ★ 热源均值为什么取 2 kW 而不是 4 kW：
        //   在回差带内，自然散热是**加热**项（舱温 25 ℃ < 环境 30 ℃）：
        //   净制冷 = cool − heat − k·(T_out−T) = 6 − heat − 1.75。
        //   heat 均值取 4 时净制冷只剩 0.25 kW，降温速率 ≈ 0.0006 K/s，
        //   走完 1 K 的回差带要 1600 s ⇒ 观测窗内泵一开到底，判据必红。
        //   取 2 kW（与 T06 同口径）⇒ 净制冷 2.25 kW，回差循环周期 ≈ 270 s。
        h.step(t, dt, s, true, 25.0, 2.0 + 1.0 * std::sin(2.0 * kPi * t / 600.0));

        x.step(t, dt, s);

        // 配电：k=600..649 急停 ⇒ 分闸 + 原因码 5
        //      → 复位后**必须显式发合闸命令**才合上（急停不自恢复，这是正确的安全语义）
        //      → k=700 人工合闸 ⇒ 原因码归 0
        w.mutable_cfg().estop_pressed = (k >= 600 && k < 650);
        if (k == 700) w.request_close(t, dt, s, true);
        else          w.step(t, dt, s);
    }

    const std::vector<std::pair<std::string, double>> req = {
        {auxpt::fire_state(),    1.0},     // 状态在火警/正常间翻转
        {auxpt::fire_smoke(),   1.0},     // 探测器动作/复位
        {auxpt::fire_released(),1.0},     // 喷洒锁存后保持
        {auxpt::fire_press(),   0.01},    // 压力持续变化
        {auxpt::hvac_t_amb(),    2.0},
        {auxpt::hvac_t_return(), 2.0},
        {auxpt::hvac_t_water(),  2.0},
        {auxpt::hvac_flow(),    10.0},    // 泵启停 ⇒ 流量大幅变化
        {auxpt::hvac_pump(),    1.0},    // 水泵翻转
        {auxpt::sec_door(),     1.0},
        {auxpt::sw_breaker(),   1.0},    // 急停 ⇒ 分合闸
        {auxpt::sw_trip(),      1.0},
        {auxpt::sw_estop(),     1.0}
    };

    const MovementReport rep = check_moving(s, req);
    std::printf("    %s\n", rep.text().c_str());
    EXPECT_EQ(static_cast<int>(rep.missing.size()), 0);
    EXPECT_EQ(static_cast<int>(rep.frozen.size()), 0);
}

int main() {
    TEST_BANNER("17/ 选配模块模拟器 —— 消防 / 温控散热 / 门禁安防 / 配电开关");

    test_01_fire_normal();
    test_02_fire_release_delay();
    test_03_fire_release_latch();
    test_04_fire_comm_fault_priority();
    test_05_hvac_heat_up();
    test_06_hvac_thermostat();
    test_07_flow_ramp();
    test_08_water_lag();
    test_09_sec_latch();
    test_10_estop_blocks_close();
    test_11_names_in_loaded_table();
    test_12_moving();

    TEST_TAIL();
    return g_fail == 0 ? 0 : 1;
}
