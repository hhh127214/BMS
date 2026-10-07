// =====================================================================
// 20/ 单元测试 —— 告警模型与装配器
//
//   T01: 等级枚举口径（与 14/schema.sql 对齐）+ 名称往返 + 日志通道映射
//   T02: 六类故障源策略表逐条对齐 11/docs/README.md §4（等级/处置/设备侧 fail-safe）
//   T03: SET/CLEAR 成对（同一条告警的完整生命周期）
//   T04: 不重复刷屏（持续 100 拍 → 1 条 SET，repeat=100）
//   T05: 反向守卫 —— 关掉边沿检测 → 记录数 = 拍数（证明 T04 的断言有区分度）
//   T06: SOC 越限 + 回差（不抖动）
//   T07: 温度预警 + 温度故障（EMERGENCY）
//   T08: 变压器过载 / 需量超契约 / 防逆流
//   T09: 状态迁移点事件（等级由目标态决定，迁移链不被合并）
//   T10: ★ 手工构造的生产快照（只用 P0 结构体，零 Sim24hConfig）
//   T11: ★ 仿真产物 vs 生产快照，同一个装配器行为一致（+ 反向守卫）
//   T12: 事件顺序稳定（连续两拍相同输入不产出新事件）
//
// 编译：见 20/scripts/build_test.bat
// =====================================================================

#include "alarm_model.h"
#include "alarm_assembler.h"
#include "alarm_input.h"

// —— 仅 T11 用：把 10/ 的仿真产物喂进同一个装配器 ——
#include "sim_24h.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace ems;

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT(cond)                                                      \
    do {                                                                  \
        if (cond) { ++g_pass; }                                           \
        else {                                                            \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #cond << std::endl;                     \
        }                                                                 \
    } while (0)

#define EXPECT_EQ(a, b)                                                   \
    do {                                                                  \
        long long va = (long long)(a), vb = (long long)(b);               \
        if (va == vb) { ++g_pass; }                                       \
        else {                                                            \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va << " vs " << #b << "="  \
                      << vb << std::endl;                                 \
        }                                                                 \
    } while (0)

#define EXPECT_NEAR(a, b, eps)                                            \
    do {                                                                  \
        double va = (a), vb = (b);                                        \
        if (std::fabs(va - vb) <= (eps)) { ++g_pass; }                    \
        else {                                                            \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va << " vs " << #b << "="  \
                      << vb << " (eps " << (eps) << ")" << std::endl;     \
        }                                                                 \
    } while (0)

#define EXPECT_STR(a, b)                                                  \
    do {                                                                  \
        std::string va = (a), vb = (b);                                   \
        if (va == vb) { ++g_pass; }                                       \
        else {                                                            \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "='" << va << "' vs " << #b       \
                      << "='" << vb << "'" << std::endl;                  \
        }                                                                 \
    } while (0)

// ---------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------
static AlarmInput make_input(double t) {
    AlarmInput in;
    in.t = t;
    in.state = EmsState::kNormal;
    in.soc = 0.5;
    in.temperature_c = 25.0;
    in.p_grid_kw = 100.0;
    in.p_load_kw = 100.0;
    return in;
}

// 默认阈值（与 10/ 默认场景接近，但**不来自 Sim24hConfig**）
static AlarmContext make_ctx() {
    AlarmContext c;
    c.soc_low  = 0.10;
    c.soc_high = 0.90;
    c.soc_hysteresis = 0.02;
    c.temp_warn_c = 45.0;
    c.temp_fault_c = 55.0;
    c.temp_hysteresis_c = 2.0;
    c.transformer_capacity_kw = 500.0;
    c.tr_overload_th = 0.95;
    c.tr_load_pv_share = 0.10;
    c.demand_target_kw = 400.0;
    c.forbid_reverse = true;
    c.grid_min_kw = 0.0;
    return c;
}

static bool same_stream(const std::vector<AlarmEvent>& a, const std::vector<AlarmEvent>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].code != b[i].code) return false;
        if (a[i].severity != b[i].severity) return false;
        if (a[i].is_set != b[i].is_set) return false;
        if (a[i].source != b[i].source) return false;
        if (a[i].source_id != b[i].source_id) return false;
        if (std::fabs(a[i].t - b[i].t) > 1e-9) return false;
    }
    return true;
}

// =====================================================================
// T01 等级枚举口径
// =====================================================================
static void test_01_severity() {
    std::printf("--- T01 severity 口径 ---\n");
    EXPECT_STR(alarm_severity_name(AlarmSeverity::kInfo), "INFO");
    EXPECT_STR(alarm_severity_name(AlarmSeverity::kWarning), "WARNING");
    EXPECT_STR(alarm_severity_name(AlarmSeverity::kDerated), "DERATED");
    EXPECT_STR(alarm_severity_name(AlarmSeverity::kFault), "FAULT");
    EXPECT_STR(alarm_severity_name(AlarmSeverity::kEmergency), "EMERGENCY");

    // 顺序：与 14/schema.sql 的枚举顺序一致（数值越大越严重）
    EXPECT((int)AlarmSeverity::kInfo < (int)AlarmSeverity::kWarning);
    EXPECT((int)AlarmSeverity::kWarning < (int)AlarmSeverity::kDerated);
    EXPECT((int)AlarmSeverity::kDerated < (int)AlarmSeverity::kFault);
    EXPECT((int)AlarmSeverity::kFault < (int)AlarmSeverity::kEmergency);

    // 名称往返
    for (int i = 0; i <= 4; ++i) {
        const AlarmSeverity s = (AlarmSeverity)i;
        AlarmSeverity back = AlarmSeverity::kInfo;
        EXPECT(alarm_severity_from_name(alarm_severity_name(s), &back));
        EXPECT(back == s);
    }
    AlarmSeverity tmp;
    EXPECT(!alarm_severity_from_name("WARN", &tmp));   // 旧 10/ 口径不再接受

    // 日志通道映射（DERATED 折到 WARN，但条目本身仍是 DERATED）
    EXPECT(soe_level_of(AlarmSeverity::kFault) == SoeLevel::kError);
    EXPECT(soe_level_of(AlarmSeverity::kEmergency) == SoeLevel::kFatal);
    EXPECT(soe_level_of(AlarmSeverity::kDerated) == SoeLevel::kWarn);

    // 状态 → 等级
    EXPECT(alarm_severity_for_state(EmsState::kFault) == AlarmSeverity::kFault);
    EXPECT(alarm_severity_for_state(EmsState::kEmergency) == AlarmSeverity::kEmergency);
    EXPECT(alarm_severity_for_state(EmsState::kDerated) == AlarmSeverity::kDerated);
    EXPECT(alarm_severity_for_state(EmsState::kNormal) == AlarmSeverity::kInfo);
}

// =====================================================================
// T02 六类故障源策略表 —— 逐条对齐 11/docs/README.md §4
// =====================================================================
static void test_02_six_fault_table() {
    std::printf("--- T02 六类故障源 × 谁该动 ---\n");
    // kind 1 BMS 通信丢失
    {
        const AlarmPolicy p = alarm_policy(AlarmCode::kCommLostBms);
        EXPECT(p.severity == AlarmSeverity::kFault);
        EXPECT(p.action == AlarmAction::kGateZero);
        EXPECT(p.owner == AlarmOwner::kEms);
        EXPECT(!p.device_self_protect);          // §4：设备侧 fail-safe = 否
    }
    // kind 2 电表通信丢失（不进 FAULT）
    {
        const AlarmPolicy p = alarm_policy(AlarmCode::kCommLostMeter);
        EXPECT(p.severity == AlarmSeverity::kWarning);   // 明确**不是** FAULT
        EXPECT(p.action == AlarmAction::kHoldLast);
        EXPECT(!p.device_self_protect);
    }
    // kind 3 PCS 通信丢失
    {
        const AlarmPolicy p = alarm_policy(AlarmCode::kCommLostPcs);
        EXPECT(p.severity == AlarmSeverity::kFault);
        EXPECT(p.action == AlarmAction::kGateZero);
        EXPECT(p.device_self_protect);           // §4：是
    }
    // kind 4 PCS 故障
    {
        const AlarmPolicy p = alarm_policy(AlarmCode::kPcsFaultSet);
        EXPECT(p.severity == AlarmSeverity::kFault);
        EXPECT(p.action == AlarmAction::kGateZero);
        EXPECT(p.device_self_protect);           // §4：是
    }
    // kind 5 设备离线
    {
        const AlarmPolicy p = alarm_policy(AlarmCode::kDeviceOffline);
        EXPECT(p.severity == AlarmSeverity::kFault);
        EXPECT(p.action == AlarmAction::kGateZero);
        EXPECT(p.device_self_protect);           // §4：是
    }
    // kind 6 数据品质劣化
    {
        const AlarmPolicy p = alarm_policy(AlarmCode::kDataStaleStart);
        EXPECT(p.severity == AlarmSeverity::kFault);
        EXPECT(p.action == AlarmAction::kGateZero);
        EXPECT(!p.device_self_protect);          // §4：否
    }

    // 反向守卫：device_self_protect==true 的**恰好**是 {kind 3,4,5}
    //   —— §4 判据原文 `dev_self_protect = (kind ∈ {3,4,5})`
    const AlarmCode six[] = {
        AlarmCode::kCommLostBms, AlarmCode::kCommLostMeter, AlarmCode::kCommLostPcs,
        AlarmCode::kPcsFaultSet, AlarmCode::kDeviceOffline, AlarmCode::kDataStaleStart,
    };
    int self_protect = 0, gate_zero = 0;
    for (AlarmCode c : six) {
        if (alarm_policy(c).device_self_protect) ++self_protect;
        if (alarm_policy(c).action == AlarmAction::kGateZero) ++gate_zero;
    }
    EXPECT_EQ(self_protect, 3);      // 只有 3 类要求设备侧动作
    EXPECT_EQ(gate_zero, 5);         // 5 类要求 EMS 门控归零（仅电表类走 HOLD_LAST）
}

// =====================================================================
// T03 SET/CLEAR 成对
// =====================================================================
static void test_03_set_clear_pair() {
    std::printf("--- T03 SET/CLEAR 配对 ---\n");
    AlarmAssembler asm_(make_ctx());

    std::vector<AlarmEvent> ev;
    AlarmInput prev, cur;
    // t=10..19 通信丢失
    for (int i = 0; i < 25; ++i) {
        cur = make_input((double)i);
        cur.bms_comm_ok = !(i >= 10 && i < 20);
        auto e = asm_.update(cur, i == 0 ? nullptr : &prev);
        ev.insert(ev.end(), e.begin(), e.end());
        prev = cur;
    }
    // 一个 SET + 一个 CLEAR
    EXPECT_EQ(ev.size(), 2);
    EXPECT(ev[0].is_set);
    EXPECT(ev[0].code == AlarmCode::kCommLostBms);
    EXPECT_NEAR(ev[0].t, 10.0, 1e-9);
    EXPECT(!ev[1].is_set);
    EXPECT(ev[1].code == AlarmCode::kCommRestored);   // 配对码来自 alarm_clear_code()
    EXPECT_NEAR(ev[1].t, 20.0, 1e-9);
    EXPECT_EQ(ev[0].pair_id, ev[1].pair_id);          // 同一条生命周期的 id

    const AlarmRecord* r = &asm_.records()[0];
    EXPECT(r->cleared());
    EXPECT(!r->active);
    EXPECT_NEAR(r->t_set, 10.0, 1e-9);
    EXPECT_NEAR(r->t_clear, 20.0, 1e-9);
    EXPECT_NEAR(r->duration_s(), 10.0, 1e-9);
    EXPECT_EQ(r->repeat, 10);                         // 持续 10 拍
    EXPECT_STR(r->key(), "COMM:BMS:COMM_LOST_BMS");
}

// =====================================================================
// T04 不重复刷屏 + T05 边沿检测反向守卫
// =====================================================================
static void test_04_no_flood() {
    std::printf("--- T04 不重复刷屏 ---\n");
    AlarmAssembler asm_(make_ctx());
    AlarmInput prev, cur;
    for (int i = 0; i < 100; ++i) {
        cur = make_input((double)i);
        cur.bms_comm_ok = false;
        asm_.update(cur, i == 0 ? nullptr : &prev);
        prev = cur;
    }
    EXPECT_EQ(asm_.size(), 1);            // 100 拍只有 1 条
    EXPECT_EQ(asm_.records()[0].repeat, 100);
    EXPECT_EQ(asm_.records()[0].active, 1);
    EXPECT_EQ(asm_.active().size(), 1);
}

static void test_05_edge_reverse_guard() {
    std::printf("--- T05 反向守卫：去掉边沿检测 ---\n");
    // 同一场景，两个模式：
    //   边沿检测开 → 1 条（T04 的断言）
    //   边沿检测关 → 100 条（“每拍都记”）
    // 若有人把边沿检测改坏（退化成逐拍记录），T04 的 `size()==1` 立刻红。
    const AlarmContext ctx = make_ctx();

    AlarmAssembler with_edge(ctx);
    AlarmAssembler no_edge(ctx, AlarmAssembler::Config{false});

    AlarmInput prev, cur;
    for (int i = 0; i < 100; ++i) {
        cur = make_input((double)i);
        cur.bms_comm_ok = false;
        with_edge.update(cur, i == 0 ? nullptr : &prev);
        no_edge.update(cur, i == 0 ? nullptr : &prev);
        prev = cur;
    }
    EXPECT_EQ(with_edge.size(), 1);       // 硬判据
    EXPECT_EQ(no_edge.size(), 100);       // 反向守卫：无去抖就是 100 条
    EXPECT(no_edge.size() > with_edge.size());

    // 阈值类同样受边沿检测约束：SOC 持续越限 50 拍 → 1 条
    AlarmAssembler a2(ctx);
    for (int i = 0; i < 50; ++i) {
        AlarmInput in = make_input((double)i);
        in.soc = 0.05;
        a2.update(in);
    }
    EXPECT_EQ(a2.count_code(AlarmCode::kSocLowStart), 1);
}

// =====================================================================
// T06 SOC 越限 + 回差
// =====================================================================
static void test_06_soc_hysteresis() {
    std::printf("--- T06 SOC 越限 + 回差 ---\n");
    AlarmAssembler a(make_ctx());
    // 先跌到 0.09（<0.10 越下限）→ SET @ t=1
    // 0.099 / 0.105 / 0.115 都在回差带 [0.10, 0.12] 内 → **不解除**
    // 0.15（> 0.10+0.02）→ CLEAR @ t=5
    // 0.95 越上限 → SET @ t=6；0.905/0.895 在回差带 [0.88, 0.90] 内 → 不解除
    // 0.80（< 0.90-0.02）→ CLEAR @ t=9
    std::vector<double> soc = {0.50, 0.09, 0.099, 0.105, 0.115, 0.15, 0.95, 0.905, 0.895, 0.80};
    int set_cnt = 0, clear_cnt = 0;
    double low_clear_t = -1.0, high_clear_t = -1.0;
    for (size_t i = 0; i < soc.size(); ++i) {
        AlarmInput in = make_input((double)i);
        in.soc = soc[i];
        for (const auto& e : a.update(in)) {
            if (e.is_set) ++set_cnt;
            else {
                ++clear_cnt;
                if (e.code == AlarmCode::kSocLowEnd)  low_clear_t  = e.t;
                if (e.code == AlarmCode::kSocHighEnd) high_clear_t = e.t;
            }
        }
    }
    EXPECT_EQ(set_cnt, 2);
    EXPECT_EQ(clear_cnt, 2);
    EXPECT(a.count_code(AlarmCode::kSocLowStart) == 1);
    EXPECT(a.count_code(AlarmCode::kSocHighStart) == 1);
    EXPECT(a.cleared_code(AlarmCode::kSocLowStart));
    EXPECT(a.cleared_code(AlarmCode::kSocHighStart));

    // ★ 回差的判据就在这里：解除只能发生在**越过回差带之后**，
    //   即 t=5 / t=9。若把回差去掉（只用裸阈值），解除会提前到 t=3 / t=8，
    //   下面两条断言立刻红 —— 这是"回差真的生效"的唯一可区分证据。
    EXPECT_NEAR(low_clear_t, 5.0, 1e-9);
    EXPECT_NEAR(high_clear_t, 9.0, 1e-9);
    // 反向守卫：越限期间 `repeat` 覆盖整段（含回差带内的 t=3..4 / t=8）
    for (const auto& r : a.records()) {
        if (r.code == AlarmCode::kSocLowStart)  EXPECT_EQ(r.repeat, 4);   // t=1..4
        if (r.code == AlarmCode::kSocHighStart) EXPECT_EQ(r.repeat, 3);   // t=6..8
    }
}

// =====================================================================
// T07 温度：预警 + 故障（EMERGENCY）
// =====================================================================
static void test_07_temperature() {
    std::printf("--- T07 温度预警/故障 ---\n");
    AlarmAssembler a(make_ctx());
    std::vector<double> temp = {25.0, 46.0, 52.0, 56.0, 57.0};
    for (size_t i = 0; i < temp.size(); ++i) {
        AlarmInput in = make_input((double)i);
        in.temperature_c = temp[i];
        a.update(in);
    }
    EXPECT_EQ(a.count_code(AlarmCode::kTempHighStart), 1);      // 预警 1 条（不刷屏）
    EXPECT_EQ(a.count_code(AlarmCode::kFsmEmergencyStop), 1);   // 温度故障 → EMERGENCY
    const AlarmPolicy pe = alarm_policy(AlarmCode::kFsmEmergencyStop);
    EXPECT(pe.severity == AlarmSeverity::kEmergency);
    // 找不到 >= EMERGENCY 的应为 1 条
    EXPECT_EQ(a.count_at_least(AlarmSeverity::kEmergency), 1);
}

// =====================================================================
// T08 变压器 / 需量 / 防逆流
// =====================================================================
static void test_08_thresholds() {
    std::printf("--- T08 变压器/需量/防逆流 ---\n");
    AlarmContext ctx = make_ctx();
    AlarmAssembler a(ctx);

    AlarmInput in = make_input(1.0);
    in.p_grid_kw = 480.0;     // tr_load = 480 + 0.1*100 = 490 > 500*0.95=475
    in.p_load_kw = 100.0;
    a.update(in);
    EXPECT_EQ(a.count_code(AlarmCode::kTrOverloadStart), 1);

    in = make_input(2.0);
    in.p_grid_kw = 420.0;     // > demand_target 400
    a.update(in);
    EXPECT_EQ(a.count_code(AlarmCode::kDemandBreachStart), 1);

    in = make_input(3.0);
    in.p_grid_kw = -5.0;      // < 0 - 1 → 倒送
    a.update(in);
    EXPECT_EQ(a.count_code(AlarmCode::kGridReverseStart), 1);

    // 反向守卫：关掉"禁止倒送"后，同样的 -5 kW **不应**产生倒送告警
    AlarmContext no_rev = make_ctx();
    no_rev.forbid_reverse = false;
    AlarmAssembler b(no_rev);
    AlarmInput in2 = make_input(1.0);
    in2.p_grid_kw = -5.0;
    b.update(in2);
    EXPECT_EQ(b.count_code(AlarmCode::kGridReverseStart), 0);
}

// =====================================================================
// T09 状态迁移点事件
// =====================================================================
static void test_09_transitions() {
    std::printf("--- T09 状态迁移点事件 ---\n");
    AlarmAssembler a(make_ctx());
    const EmsState seq[] = {EmsState::kInit, EmsState::kSelfCheck, EmsState::kReady,
                            EmsState::kNormal, EmsState::kFault, EmsState::kReady,
                            EmsState::kNormal, EmsState::kEmergency};
    AlarmInput prev;
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); ++i) {
        AlarmInput in = make_input((double)i);
        in.state = seq[i];
        a.update(in, i == 0 ? nullptr : &prev);
        prev = in;
    }
    // 7 次迁移 → 7 条点事件（**不被合并**，迁移链完整）
    EXPECT_EQ(a.count_code(AlarmCode::kFsmTransition), 7);
    // FAULT 迁移 1 次、EMERGENCY 迁移 1 次
    int fault_tr = 0, emg_tr = 0;
    for (const auto& r : a.records()) {
        if (r.code != AlarmCode::kFsmTransition) continue;
        if (r.severity == AlarmSeverity::kFault) ++fault_tr;
        if (r.severity == AlarmSeverity::kEmergency) ++emg_tr;
        EXPECT(r.is_point());                       // 点事件：SET==CLEAR
    }
    EXPECT_EQ(fault_tr, 1);
    EXPECT_EQ(emg_tr, 1);
}

// =====================================================================
// T10 ★ 手工构造的生产快照（只用 P0 结构体）
//
//   本用例**不涉及任何 Sim24hConfig / StepRecord** —— 数据全部来自
//   RealtimeSnapshot / DeviceStatus / DeviceLimits / EmsState，
//   即 11/main_ems.cpp、14/ 平台后端会拿到的那些东西。
// =====================================================================
static void test_10_production_only() {
    std::printf("--- T10 生产入口（无仿真结构体）---\n");

    DeviceLimits dev;
    dev.transformer_capacity_kw = 500.0;
    dev.d_target_kw = 400.0;

    AlarmAssembler a(make_ctx());

    struct Tick { double t; bool bms_ok; bool pcs_fault; bool offline; EmsState st; bool trans; };
    const Tick script[] = {
        {0.0,  true,  false, false, EmsState::kNormal, false},
        {1.0,  true,  false, false, EmsState::kNormal, false},
        {2.0,  false, false, false, EmsState::kNormal, false},   // BMS 通信丢失
        {3.0,  false, false, false, EmsState::kNormal, false},
        {4.0,  true,  false, false, EmsState::kFault,  true},    // 通信恢复但状态机进 FAULT
        {5.0,  true,  false, false, EmsState::kNormal, false},
        {6.0,  true,  true,  false, EmsState::kFault,  true},    // PCS 故障
        {7.0,  true,  true,  true,  EmsState::kFault,  false},   // 设备离线
        {8.0,  true,  false, false, EmsState::kNormal, false},   // 全部恢复
    };

    AlarmInput prev;
    int i = 0;
    for (const Tick& tk : script) {
        ProductionSnapshot ps;
        ps.rt.timestamp = tk.t;
        ps.rt.p_grid_kw = 100.0;
        ps.rt.p_load_kw = 100.0;
        ps.rt.soc = 0.5;
        ps.rt.temperature_c = 25.0;
        ps.st.bms_comm_ok    = tk.bms_ok;
        ps.st.pcs_comm_ok    = true;
        ps.st.meter_comm_ok  = true;
        ps.st.pcs_fault      = tk.pcs_fault;
        ps.st.device_offline = tk.offline;
        ps.st.data_valid     = true;
        ps.dev = dev;
        ps.state = tk.st;
        ps.has_transition = tk.trans && i > 0;
        if (ps.has_transition) { ps.state_from = prev.state; ps.state_to = tk.st; }

        AlarmInput in;
        alarm_input_from_production(ps, in);
        a.update(in, i == 0 ? nullptr : &prev);
        prev = in;
        ++i;
    }

    // ★ "设备离线 ⇒ BMS 通信丢失" 是**状态机口径**（`detect_faults()` 里
    //   `bms_comm_lost = !bms_comm_ok || device_offline`），不是重复报 ——
    //   所以这里 BMS 通信共出现 2 次（t=2 真丢、t=7 因离线而派生）。
    EXPECT_EQ(a.count_code(AlarmCode::kCommLostBms), 2);
    EXPECT_EQ(a.count_code(AlarmCode::kPcsFaultSet), 1);
    EXPECT_EQ(a.count_code(AlarmCode::kDeviceOffline), 1);
    // 状态序列实际变了 4 次：→FAULT(t=4) →NORMAL(t=5) →FAULT(t=6) →NORMAL(t=8)。
    // 迁移是**点事件**，逐次成条（迁移链不被合并）—— 这是与"持续条件"的关键区别。
    EXPECT_EQ(a.count_code(AlarmCode::kFsmTransition), 4);
    EXPECT(a.cleared_code(AlarmCode::kCommLostBms));   // t=4 恢复
    EXPECT(a.cleared_code(AlarmCode::kPcsFaultSet));
    EXPECT(a.cleared_code(AlarmCode::kDeviceOffline));

    // 逐条核对处置（与 11/§4 表一致）
    for (const auto& r : a.records()) {
        if (r.code == AlarmCode::kCommLostBms) {
            EXPECT(r.severity == AlarmSeverity::kFault);
            EXPECT(r.action == AlarmAction::kGateZero);
            EXPECT(r.owner == AlarmOwner::kEms);
            EXPECT(!alarm_policy(r.code).device_self_protect);
        }
        if (r.code == AlarmCode::kPcsFaultSet) {
            EXPECT(r.severity == AlarmSeverity::kFault);
            EXPECT(r.owner == AlarmOwner::kBoth);
        }
    }
    // 该场景全程无 ECON/SAFETY 阈值越限 → 不应产生阈值类告警（防"乱报"）
    EXPECT_EQ(a.count_code(AlarmCode::kSocLowStart), 0);
    EXPECT_EQ(a.count_code(AlarmCode::kTrOverloadStart), 0);
}

// =====================================================================
// T11 ★ 仿真产物 vs 生产快照 —— 同一个装配器行为一致
//
//   路径 A：AlarmInput 来自 10/ 的 StepRecord
//   路径 B：AlarmInput 来自 P0 契约（RealtimeSnapshot/DeviceStatus/DeviceLimits）
//   断言：两条路径的事件流**逐条一致** → 装配器不依赖 Sim24hConfig。
//
//   反向守卫：扰动路径 B 的一次快照（多置一个 pcs_fault）→ 必须不一致
//             （证明"一致"不是恒真）。
// =====================================================================
static void test_11_equivalence() {
    std::printf("--- T11 仿真入口 vs 生产入口 等价性 ---\n");

    Sim24hConfig cfg = make_default_24h_config();
    cfg.dt_s = 1.0;
    cfg.log_every = 1;
    cfg.duration_s = 300.0;
    cfg.fault_windows.clear();
    cfg.fault_windows.push_back(FaultWindow{50.0, 150.0, 1, "bms_comm_off"});
    cfg.fault_windows.push_back(FaultWindow{180.0, 220.0, 4, "pcs_fault"});
    cfg.fault_windows.push_back(FaultWindow{230.0, 250.0, 2, "meter_off"});

    const Sim24hResult res = run_sim_24h(cfg);
    EXPECT(res.ok);
    EXPECT(res.log.size() == 300);

    const AlarmContext ctx =
        alarm_context_from(cfg.safety, cfg.limits, /*forbid_reverse=*/(cfg.grid_min_required > -1e8));

    // ---- 路径 A：仿真产物 ----
    AlarmAssembler asmA(ctx);
    std::vector<AlarmEvent> evA;
    for (size_t i = 0; i < res.log.size(); ++i) {
        AlarmInput in;
        alarm_input_from_step_record(res.log[i], i == 0 ? nullptr : &res.log[i - 1], in);
        auto e = asmA.update(in);
        evA.insert(evA.end(), e.begin(), e.end());
    }

    // ---- 路径 B：P0 契约（经 production_from_step_record 改写，再喂同一装配器）----
    AlarmAssembler asmB(ctx);
    std::vector<AlarmEvent> evB;
    for (size_t i = 0; i < res.log.size(); ++i) {
        ProductionSnapshot ps;
        production_from_step_record(res.log[i], i == 0 ? nullptr : &res.log[i - 1],
                                    cfg.limits, ps);
        AlarmInput in;
        alarm_input_from_production(ps, in);
        auto e = asmB.update(in);
        evB.insert(evB.end(), e.begin(), e.end());
    }

    // 反向守卫 ①：真的产生了事件（否则"一致"是空集对空集）
    EXPECT(evA.size() > 0);
    EXPECT(evA.size() >= 6);
    EXPECT(same_stream(evA, evB));

    // 两条路径产出的记录也逐条一致（含 SET/CLEAR 配对与等级）
    EXPECT_EQ(asmA.size(), asmB.size());
    for (size_t i = 0; i < asmA.records().size() && i < asmB.records().size(); ++i) {
        EXPECT(asmA.records()[i].code == asmB.records()[i].code);
        EXPECT(asmA.records()[i].severity == asmB.records()[i].severity);
        EXPECT(asmA.records()[i].action == asmB.records()[i].action);
        EXPECT(asmA.records()[i].cleared() == asmB.records()[i].cleared());
    }

    // 具体内容：BMS 通信丢失成对、PCS 故障出现
    EXPECT(asmA.count_code(AlarmCode::kCommLostBms) >= 1);
    EXPECT(asmA.cleared_code(AlarmCode::kCommLostBms));
    EXPECT(asmA.count_code(AlarmCode::kPcsFaultSet) >= 1);
    EXPECT(asmA.count_at_least(AlarmSeverity::kFault) >= 2);

    // ---- 反向守卫 ②：扰动路径 B 的一次快照 → 必须不一致 ----
    AlarmAssembler asmC(ctx);
    std::vector<AlarmEvent> evC;
    for (size_t i = 0; i < res.log.size(); ++i) {
        ProductionSnapshot ps;
        production_from_step_record(res.log[i], i == 0 ? nullptr : &res.log[i - 1],
                                    cfg.limits, ps);
        if (i == 100) ps.st.pcs_fault = true;      // 人为多置一个故障位
        AlarmInput in;
        alarm_input_from_production(ps, in);
        auto e = asmC.update(in);
        evC.insert(evC.end(), e.begin(), e.end());
    }
    EXPECT(!same_stream(evA, evC));                // 比较**有区分度**
}

// =====================================================================
// T12 事件顺序稳定 / 相同输入不重复出事件
// =====================================================================
static void test_12_stable() {
    std::printf("--- T12 顺序稳定 ---\n");
    AlarmAssembler a(make_ctx());
    AlarmInput in = make_input(1.0);
    in.bms_comm_ok = false;
    in.soc = 0.05;
    auto e1 = a.update(in);
    auto e2 = a.update(in);
    EXPECT(e1.size() >= 2);              // 同拍多个条件都报
    EXPECT_EQ(e2.size(), 0);             // 第二拍无新事件
    // 顺序稳定：同一台设备、同样输入，两次独立跑的事件序列一致
    AlarmAssembler b(make_ctx());
    auto f1 = b.update(in);
    EXPECT(same_stream(e1, f1));
}

int main() {
    std::printf("=== 20/ 告警模型与装配器 单元测试 ===\n\n");
    test_01_severity();
    test_02_six_fault_table();
    test_03_set_clear_pair();
    test_04_no_flood();
    test_05_edge_reverse_guard();
    test_06_soc_hysteresis();
    test_07_temperature();
    test_08_thresholds();
    test_09_transitions();
    test_10_production_only();
    test_11_equivalence();
    test_12_stable();

    std::printf("\n");
    if (g_fail == 0) std::printf("ALL TESTS PASSED\n");
    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    // 纯 ASCII 状态行落盘 —— 构建脚本读它汇总，**不去 grep 中文输出**
    // （本仓库的 .bat 在 CP936 下解析 UTF-8 会出静默问题，见 13/ 的先例）。
    {
        std::ofstream f("build/test_alarm_model_status.txt", std::ios::out | std::ios::binary);
        if (f.is_open()) f << "PASS=" << g_pass << " FAIL=" << g_fail << "\n";
    }
    return g_fail == 0 ? 0 : 1;
}
