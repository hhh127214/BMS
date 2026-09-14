// =====================================================================
// P3/ 单元测试 —— IEC 60870-5-104 通信适配器（Iec104DeviceIO）
//
//   T41: APCI 逐字节 —— I / S / U 三种帧型、序号编解码、LEN 边界
//   T42: ASDU 逐字节 —— 类型 1/13/50/70/100/200 的布局、小端字节序、品质位
//   T43: 会话时序 —— STARTDT 握手、总召唤三段式、心跳、k/w 窗口、CA 校验
//   T44: 闭环等价 —— 104 链路 vs 进程内点表，400 拍逐位一致；
//        另做**量化边界**：标准 float32（M_ME_NC_1）下的偏差量级
//   T45: 通信中断 —— 链路断 / 对端哑（t3 判据）两条不可信路径 + 自愈
//   T46: 映射契约 —— 30 点 ↔ IOA ↔ 类型标识 三者一致（漂移守卫）
//
// 为什么 104 的"逐位等价"要单独做一遍（而不是复用 Modbus 的结论）：
//   104 与 Modbus 在介质层面没有任何共用代码 —— 序号状态机、小端字节序、
//   从站主动上送、按信息体类型分段，全都不一样。Modbus 全绿只能说明 Modbus
//   写对了。两个都逐位一致，"适配器这一层不改变算法行为"才是一句可复用的
//   结论，而不是两个孤立事实。
//
// 编译：见 P3/scripts/build_test_iec104.bat
// =====================================================================

#include "data_models.h"
#include "device_io.h"
#include "ems_point_table.h"
#include "iec104_codec.h"
#include "iec104_device_io.h"
#include "memory_device_io.h"
#include "realtime_loop.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace ems;
using namespace ems::iec104;

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

#define EXPECT_NEAR(a, b, eps)                                            \
    do {                                                                  \
        double va = (a), vb = (b);                                        \
        if (std::fabs(va - vb) <= (eps)) {                                \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va << " vs " << #b << "="  \
                      << vb << " (eps " << (eps) << ")" << std::endl;     \
        }                                                                 \
    } while (0)

static bool almost_equal(double a, double b, double eps = 0.0) {
    return std::fabs(a - b) <= eps;
}

static std::string hex_of(const uint8_t* p, size_t n) {
    static const char* d = "0123456789ABCDEF";
    std::string s;
    char buf[4];
    for (size_t i = 0; i < n; ++i) {
        buf[0] = d[(p[i] >> 4) & 0xF];
        buf[1] = d[p[i] & 0xF];
        buf[2] = '\0';
        if (i) s += ' ';
        s += buf;
    }
    return s;
}

static void env_at(int step, double& p_load_kw, double& p_pv_kw) {
    const double t = step * 0.1;
    p_load_kw = 380.0 + 120.0 * std::sin(t / 30.0);
    p_pv_kw   = (t > 5.0 && t < 25.0) ? 150.0 + 60.0 * std::sin(t / 5.0) : 0.0;
}

// 与 Modbus / RT_DB 两个测试**逐行一致**的装配序列
static void configure_runtime(EmsRuntime& rt) {
    rt.config().dt_s = 0.1;
    rt.config().enable_realtime_correction = true;
    rt.config().l2_correction_max_kw = 100.0;
    rt.safety_params().grid_p_min_kw = -1e9;
    rt.safety_params().ramp_kw_per_s = 1e9;
    rt.apply_configs();
    rt.fsm().request_run(true);
    rt.device_limits().transformer_capacity_kw = 800.0;
    rt.device_limits().d_target_kw = 250.0;
}

struct MemoryRig {
    MemoryDeviceIO dev;
    EmsRuntime     rt;
    MemoryRig() {
        rt.attach_device(&dev);
        configure_runtime(rt);
    }
    void run(int n, int step_base = 0) {
        rt.run(n, 0.1, [&](EmsRuntime&, int i) {
            double load = 0.0, pv = 0.0;
            env_at(step_base + i, load, pv);
            dev.set_environment(load, pv);
        });
    }
};

// 被测实现：EmsRuntime ← Iec104DeviceIO ←[104 报文]→ 被控站仿真
struct Iec104Rig {
    MemoryDeviceIO               dev;        // 设备替身
    Iec104ControlledStationSim   station;    // 调度侧被控站
    LoopbackIec104Transport      transport;
    Iec104DeviceIO               io;
    EmsRuntime                   rt;

    explicit Iec104Rig(Iec104Profile profile, uint16_t ca = 1)
        : station(profile, ca), transport(&station), io(&transport, profile, ca) {
        station.attach_device(&dev);
        // 设备侧推进（现场由硬件完成）
        io.set_device_pump([&](double cmd, double dt) { dev.execute(cmd, dt); });
        io.open();                              // STARTDT + 总召唤
        rt.attach_device(&io);                  // ← 读限值：来自 CFG 段信息体
        configure_runtime(rt);
    }
    void run(int n, int step_base = 0) {
        rt.run(n, 0.1, [&](EmsRuntime&, int i) {
            double load = 0.0, pv = 0.0;
            env_at(step_base + i, load, pv);
            dev.set_environment(load, pv);
        });
    }
};

static int diff_logs(const std::vector<StepRecord>& a, const std::vector<StepRecord>& b,
                     double eps = 0.0, bool verbose = false) {
    int diff = 0;
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const StepRecord& x = a[i];
        const StepRecord& y = b[i];
        const bool same =
            almost_equal(x.p_cmd, y.p_cmd, eps) && almost_equal(x.p_actual, y.p_actual, eps) &&
            almost_equal(x.p_grid, y.p_grid, eps) && almost_equal(x.soc, y.soc, eps) &&
            almost_equal(x.temp, y.temp, eps) && almost_equal(x.p_lower, y.p_lower, eps) &&
            almost_equal(x.p_upper, y.p_upper, eps) && almost_equal(x.plan_target, y.plan_target, eps) &&
            almost_equal(x.correction, y.correction, eps) && almost_equal(x.t, y.t, eps) &&
            x.state == y.state && x.clamped == y.clamped &&
            x.safety_clip == y.safety_clip && x.state_gated == y.state_gated &&
            x.hold_last == y.hold_last && x.fault_bits == y.fault_bits &&
            x.reason == y.reason;
        if (!same) {
            ++diff;
            if (verbose && diff <= 3) {
                std::cerr << "        第 " << i << " 拍不同: A(cmd=" << x.p_cmd
                          << " soc=" << x.soc << " reason=" << x.reason
                          << ")  B(cmd=" << y.p_cmd << " soc=" << y.soc
                          << " reason=" << y.reason << ")\n";
            }
        }
    }
    return diff;
}

// =====================================================================
// T41: APCI 逐字节
// =====================================================================
static void test_41_apci_byte_exact() {
    std::cerr << "[T41] APCI 逐字节：I/S/U 帧型、序号编解码、LEN 边界 ...\n";

    uint8_t f[512];

    // ---- U 帧：STARTDT / STOPDT / TESTFR 的 act 与 con ----
    {
        const uint8_t exp_start_act[] = {0x68, 0x04, 0x03, 0x07, 0x00, 0x00};
        const uint8_t exp_start_con[] = {0x68, 0x04, 0x03, 0x0B, 0x00, 0x00};
        const uint8_t exp_stop_act[]  = {0x68, 0x04, 0x03, 0x13, 0x00, 0x00};
        const uint8_t exp_test_act[]  = {0x68, 0x04, 0x03, 0x43, 0x00, 0x00};
        const uint8_t exp_test_con[]  = {0x68, 0x04, 0x03, 0x83, 0x00, 0x00};
        size_t n = wrap_u_frame(f, sizeof(f), kStartDtAct);
        EXPECT(n == 6 && std::memcmp(f, exp_start_act, 6) == 0);
        n = wrap_u_frame(f, sizeof(f), kStartDtCon);
        EXPECT(n == 6 && std::memcmp(f, exp_start_con, 6) == 0);
        n = wrap_u_frame(f, sizeof(f), kStopDtAct);
        EXPECT(n == 6 && std::memcmp(f, exp_stop_act, 6) == 0);
        n = wrap_u_frame(f, sizeof(f), kTestFrAct);
        EXPECT(n == 6 && std::memcmp(f, exp_test_act, 6) == 0);
        n = wrap_u_frame(f, sizeof(f), kTestFrCon);
        EXPECT(n == 6 && std::memcmp(f, exp_test_con, 6) == 0);

        ApciHeader h;
        EXPECT(parse_apdu(exp_test_act, 6, h) == 6);
        EXPECT(h.kind == kFrameU && h.ufunc == kTestFrAct);
        EXPECT(parse_apdu(exp_start_con, 6, h) == 6);
        EXPECT(h.kind == kFrameU && h.ufunc == kStartDtCon);
    }

    // ---- S 帧：0x01 + N(R) ----
    {
        const uint8_t exp[] = {0x68, 0x04, 0x01, 0x00, 0x0A, 0x00};   // N(R)=5
        const size_t n = wrap_s_frame(f, sizeof(f), 5);
        EXPECT(n == 6 && std::memcmp(f, exp, 6) == 0);
        ApciHeader h;
        EXPECT(parse_apdu(exp, 6, h) == 6);
        EXPECT(h.kind == kFrameS && h.nr == 5);
    }

    // ---- I 帧控制域：N(S)/N(R) 各 15 bit ----
    {
        uint8_t c[4];
        build_i_apci(c, 4, 0, 0);
        const uint8_t e0[] = {0x00, 0x00, 0x00, 0x00};
        EXPECT(std::memcmp(c, e0, 4) == 0);
        build_i_apci(c, 4, 1, 1);
        const uint8_t e1[] = {0x02, 0x00, 0x02, 0x00};
        EXPECT(std::memcmp(c, e1, 4) == 0);
        build_i_apci(c, 4, 256, 256);
        const uint8_t e2[] = {0x00, 0x02, 0x00, 0x02};
        EXPECT(std::memcmp(c, e2, 4) == 0);
        // 边界：15 bit 最大值
        uint8_t apdu[8] = {0x68, 0x04, 0, 0, 0, 0, 0, 0};
        build_i_apci(apdu + 2, 4, 16383, 16383);
        ApciHeader h;
        EXPECT(parse_apdu(apdu, 6, h) == 6);
        EXPECT(h.kind == kFrameI);
        EXPECT(h.ns == 16383 && h.nr == 16383);
        EXPECT(apdu[2] == 0xFE && apdu[3] == 0x7F);
    }

    // ---- 帧长边界：ASDU 上限 249（LEN=253，APDU=255）----
    {
        uint8_t asdu[300] = {0};
        EXPECT(wrap_i_frame(f, sizeof(f), 0, 0, asdu, 249) == 255);
        EXPECT(f[1] == 253);
        EXPECT(wrap_i_frame(f, sizeof(f), 0, 0, asdu, 250) == 0);   // 超限必须拒绝
        EXPECT(wrap_i_frame(f, sizeof(f), 0, 0, asdu, 0) == 0);
        // 缓冲区不足也拒绝，而不是截断
        EXPECT(wrap_i_frame(f, 100, 0, 0, asdu, 120) == 0);
    }

    // ---- 字节流边界：半个帧必须"攒够再解析"，不能瞎猜 ----
    {
        uint8_t asdu[32];
        const size_t an = make_asdu_general_interrogation(asdu, sizeof(asdu), kCOT_Act, 1);
        const size_t fn = wrap_i_frame(f, sizeof(f), 0, 0, asdu, an);
        EXPECT(fn == 16);
        ApciHeader h;
        EXPECT(parse_apdu(f, 5, h) == 0);          // 头都不齐
        EXPECT(parse_apdu(f, fn - 1, h) == 0);     // 差一个字节
        EXPECT(parse_apdu(f, fn, h) == fn);
        // 两帧粘在一起：第一次解析只吃第一帧
        uint8_t two[64];
        std::memcpy(two, f, fn);
        std::memcpy(two + fn, f, fn);
        EXPECT(parse_apdu(two, fn * 2, h) == fn);
        EXPECT(parse_apdu(two + fn, fn, h) == fn);
        // 起始字节不是 0x68 → 非法
        uint8_t bad[16];
        std::memcpy(bad, f, fn);
        bad[0] = 0x69;
        EXPECT(parse_apdu(bad, fn, h) == 0);
        // LEN 小于 4 → 非法
        std::memcpy(bad, f, fn);
        bad[1] = 3;
        EXPECT(parse_apdu(bad, fn, h) == 0);
    }

    std::cerr << "        U 帧 6 字节 / S 帧 6 字节 / I 帧 6+ASDU / LEN 上限 253\n";
}

// =====================================================================
// T42: ASDU 逐字节（含小端字节序与品质位）
// =====================================================================
static void test_42_asdu_byte_exact() {
    std::cerr << "[T42] ASDU 逐字节：类型 1/13/50/70/100/200、小端序、品质位 ...\n";

    uint8_t asdu[512];

    // ---- 类型 13 M_ME_NC_1，SQ=1，1 个对象：IOA=0x4001 value=1.0 ----
    {
        InfoFloat it[1];
        it[0].ioa = 0x004001;
        it[0].value = 1.0;
        it[0].qds = kQdsGood;
        const size_t n = make_asdu_measurements(asdu, sizeof(asdu), kCOT_Per, 1, it, 1);
        const uint8_t exp[] = {0x0D, 0x81, 0x01, 0x00, 0x01, 0x00,   // 类型/SQ+数量/COT/OA/CA
                               0x01, 0x40, 0x00,                     // IOA 小端 0x004001
                               0x00, 0x00, 0x80, 0x3F,               // float32 小端 1.0
                               0x00};                                // QDS
        EXPECT(n == sizeof(exp));
        EXPECT(std::memcmp(asdu, exp, sizeof(exp)) == 0);
        if (n != sizeof(exp) || std::memcmp(asdu, exp, sizeof(exp)) != 0) {
            std::cerr << "        实际: " << hex_of(asdu, n) << "\n";
        }
    }

    // ---- 类型 1 M_SP_NA_1，SQ=1：IOA=0x4201 value=true ----
    {
        InfoSingle it[1];
        it[0].ioa = 0x004201;
        it[0].value = true;
        it[0].qds = kQdsGood;
        const size_t n = make_asdu_single_points(asdu, sizeof(asdu), kCOT_Per, 1, it, 1);
        const uint8_t exp[] = {0x01, 0x81, 0x01, 0x00, 0x01, 0x00,
                               0x01, 0x42, 0x00, 0x01};
        EXPECT(n == sizeof(exp));
        EXPECT(std::memcmp(asdu, exp, sizeof(exp)) == 0);
    }

    // ---- 类型 50 C_SE_NC_1：IOA=0x4301 value=100.0 执行 ----
    {
        const size_t n = make_asdu_setpoint_float(asdu, sizeof(asdu), kCOT_Act, 1, 0x004301,
                                                  100.0, make_qoc(false));
        const uint8_t exp[] = {0x32, 0x01, 0x06, 0x00, 0x01, 0x00,
                               0x01, 0x43, 0x00,
                               0x00, 0x00, 0xC8, 0x42,   // float32 小端 100.0
                               0x00};
        EXPECT(n == sizeof(exp));
        EXPECT(std::memcmp(asdu, exp, sizeof(exp)) == 0);
        // 选择（S/E=1）→ QOS = 0x80
        const size_t n2 = make_asdu_setpoint_float(asdu, sizeof(asdu), kCOT_Act, 1, 0x004301,
                                                   100.0, make_qoc(true));
        EXPECT(n2 == sizeof(exp));
        EXPECT(asdu[13] == 0x80);
    }

    // ---- 类型 100 C_IC_NA_1 总召唤 ----
    {
        const size_t n = make_asdu_general_interrogation(asdu, sizeof(asdu), kCOT_Act, 1, 20);
        const uint8_t exp[] = {0x64, 0x01, 0x06, 0x00, 0x01, 0x00,
                               0x00, 0x00, 0x00, 0x14};
        EXPECT(n == sizeof(exp));
        EXPECT(std::memcmp(asdu, exp, sizeof(exp)) == 0);
        uint8_t qoi = 0;
        ApciHeader ah; AsduView v;
        uint8_t frame[64];
        const size_t fn = wrap_i_frame(frame, sizeof(frame), 0, 0, asdu, n);
        EXPECT(parse_apdu(frame, fn, ah) == fn);
        EXPECT(parse_asdu(frame, fn, ah, v));
        EXPECT(decode_general_interrogation(v, &qoi) && qoi == 20);
        EXPECT(v.h.type_id == kC_IC_NA_1 && v.h.cot == kCOT_Act && v.h.ca == 1 && !v.h.sq);
    }

    // ---- 类型 70 M_EI_NA_1 ----
    {
        const size_t n = make_asdu_init_end(asdu, sizeof(asdu), 1);
        const uint8_t exp[] = {0x46, 0x01, 0x04, 0x00, 0x01, 0x00,
                               0x00, 0x00, 0x00, 0x00};
        EXPECT(n == sizeof(exp));
        EXPECT(std::memcmp(asdu, exp, sizeof(exp)) == 0);
    }

    // ---- 类型 45 / 46 单点、双点命令（S/E 位）----
    {
        size_t n = make_asdu_single_command(asdu, sizeof(asdu), kCOT_Act, 1, 0x004201, true, false);
        const uint8_t exp45[] = {0x2D, 0x01, 0x06, 0x00, 0x01, 0x00, 0x01, 0x42, 0x00, 0x01};
        EXPECT(n == sizeof(exp45) && std::memcmp(asdu, exp45, sizeof(exp45)) == 0);
        n = make_asdu_single_command(asdu, sizeof(asdu), kCOT_Act, 1, 0x004201, true, true);
        EXPECT(asdu[9] == 0x81);                 // 选择位
        n = make_asdu_double_command(asdu, sizeof(asdu), kCOT_Act, 1, 0x004201, 2, false);
        EXPECT(n == 10 && asdu[9] == 0x02);      // DCS=2 合
        EXPECT(asdu[0] == 0x2E);
    }

    // ---- 类型 200（私有宽精度）：必须**位级无损** ----
    {
        const double probes[] = {0.1, -1.0 / 3.0, 3.14159265358979, 1e-9, 123456.789012345};
        for (double pv : probes) {
            InfoFloat it[1];
            it[0].ioa = 0x004001;
            it[0].value = pv;
            it[0].qds = kQdsGood;
            const size_t n = make_asdu_wide(asdu, sizeof(asdu), kCOT_Per, 1, it, 1);
            EXPECT(n == 18);
            uint8_t frame[64];
            const size_t fn = wrap_i_frame(frame, sizeof(frame), 0, 0, asdu, n);
            ApciHeader ah; AsduView v;
            EXPECT(parse_apdu(frame, fn, ah) == fn);
            EXPECT(parse_asdu(frame, fn, ah, v));
            std::vector<InfoFloat> out;
            EXPECT(decode_wide(v, out) && out.size() == 1);
            EXPECT(std::memcmp(&pv, &out[0].value, 8) == 0);   // 位级恒等
        }
        // 0.1 的 IEEE754 小端字节（用于对照 Modbus 的大端字序）
        InfoFloat it[1];
        it[0].ioa = 0x004001; it[0].value = 0.1; it[0].qds = kQdsGood;
        const size_t n = make_asdu_wide(asdu, sizeof(asdu), kCOT_Per, 1, it, 1);
        EXPECT(n == 18);
        const uint8_t exp_tail[] = {0x9A, 0x99, 0x99, 0x99, 0x99, 0x99, 0xB9, 0x3F, 0x00};
        EXPECT(std::memcmp(asdu + 9, exp_tail, sizeof(exp_tail)) == 0);
    }

    // ---- 品质位：IV 置位必须判为无效 ----
    {
        EXPECT(qds_valid(kQdsGood));
        EXPECT(qds_valid(kQdsNotTopical));
        EXPECT(!qds_valid(kQdsInvalid));
        EXPECT(!qds_valid(static_cast<uint8_t>(kQdsInvalid | kQdsBlocked)));
        EXPECT(siq_value(qds_to_siq(kQdsGood, true)));
        EXPECT(!siq_value(qds_to_siq(kQdsGood, false)));
        EXPECT(siq_to_qds(qds_to_siq(kQdsBlocked, true)) == kQdsBlocked);
        EXPECT(qoc_is_select(make_qoc(true)) && !qoc_is_select(make_qoc(false)));
    }

    // ---- 长度语义自校验：SQ=1 的对象数必须与信息体长度相符 ----
    {
        InfoFloat it[3];
        for (int i = 0; i < 3; ++i) { it[i].ioa = 0x004001 + i; it[i].value = i * 1.5; it[i].qds = 0; }
        const size_t n = make_asdu_measurements(asdu, sizeof(asdu), kCOT_Per, 1, it, 3);
        uint8_t frame[64];
        const size_t fn = wrap_i_frame(frame, sizeof(frame), 0, 0, asdu, n);
        ApciHeader ah; AsduView v;
        EXPECT(parse_asdu(frame, fn, ah, v));
        EXPECT(asdu_length_ok(v));
        std::vector<InfoFloat> out;
        EXPECT(decode_measurements(v, out) && out.size() == 3);
        EXPECT(out[1].ioa == 0x004002 && out[2].ioa == 0x004003);
        EXPECT_NEAR(out[2].value, 3.0, 1e-6);
        // 篡改 count → 长度语义必须失败（不能把畸形帧当有效数据）
        frame[6 + 1] = 0x85;
        EXPECT(parse_asdu(frame, fn, ah, v));
        EXPECT(!asdu_length_ok(v));
    }

    std::cerr << "        类型 1/13/45/46/50/70/100/200 布局与小端序全部逐字节对上\n";
}

// =====================================================================
// T43: 会话时序（STARTDT / 总召唤 / 心跳 / k-w 窗口 / CA 校验）
// =====================================================================
static void test_43_session_and_windows() {
    std::cerr << "[T43] 会话时序：STARTDT、总召唤三段式、心跳、k/w 窗口 ...\n";

    Iec104Rig rig(Iec104Profile::kWidePrivate);
    EXPECT(rig.io.is_open());
    EXPECT(rig.io.self_check() == 0);

    // ---- 建链：STARTDT 已被确认；总召完成三段式 ----
    EXPECT(rig.io.startdt_ok());
    EXPECT(rig.io.gi_done());
    EXPECT(rig.station.started());
    EXPECT(rig.io.gi_actcon() >= 1);
    EXPECT(rig.io.gi_actterm() >= 1);
    EXPECT(rig.io.init_end() >= 1);            // 类型 70 初始化结束
    EXPECT(rig.station.gi_count() == 1);       // 只召唤了一次

    // ---- 序号自洽：我方 N(S) = 发过的 I 帧数；N(R) = 对端下一个 N(S) ----
    EXPECT(rig.io.vs() == static_cast<uint16_t>(rig.io.i_frames_tx()) + 0);
    EXPECT(rig.io.vr() == rig.station.vs());
    EXPECT(rig.io.unacked_tx() == 0);

    // ---- 连续轮询：心跳与确认必须都出现，且窗口不被击穿 ----
    int max_unacked = 0;
    const int station_vs_before = rig.station.vs();
    for (int i = 0; i < 40; ++i) {
        rig.io.poll(0);
        max_unacked = std::max(max_unacked, rig.io.unacked_tx());
    }
    EXPECT(rig.io.testfr_tx() > 0);
    EXPECT(rig.io.testfr_rx() > 0);
    EXPECT(rig.io.s_frames_tx() > 0);
    EXPECT(max_unacked <= kWindowK);
    EXPECT(rig.io.window_stalls() == 0);
    EXPECT(rig.io.vr() == rig.station.vs());
    EXPECT(rig.station.vs() > station_vs_before);          // 从站在持续上送
    EXPECT(rig.io.malformed() == 0);
    EXPECT(rig.io.bad_ca() == 0);
    EXPECT(rig.io.unknown_type() == 0);

    // ---- 上行 I 帧数与下行 I 帧数各自独立计数 ----
    EXPECT(rig.io.i_frames_rx() > 0);
    EXPECT(rig.io.i_frames_tx() == 1);                      // 只有总召那一条

    // ---- 指令：写 CMD 段 → 从站回激活确认（同 IOA 回显）----
    PowerCommand cmd;
    cmd.p_bat_cmd_kw = 123.0;
    cmd.p_upper = 150.0;
    cmd.p_lower = -80.0;
    EXPECT(rig.io.write_command(cmd));
    rig.io.poll(0);
    EXPECT(rig.station.setpoint_count() >= 3);
    EXPECT(rig.io.setpoint_actcon() >= 1);
    EXPECT(rig.io.setpoint_echo() >= 1);
    EXPECT(rig.io.i_frames_tx() == 4);                      // 总召 + 3 条遥调
    // 设备侧确实收到了下行的功率设定值
    rig.io.poll(0);
    EXPECT_NEAR(rig.dev.get(EMS_POINT_NAMES[EMS_CMD_P_BAT]), 123.0, 1e-4);
    EXPECT(std::fabs(rig.dev.get(EMS_POINT_NAMES[EMS_CMD_P_UPPER]) - 150.0) < 1e-3);
    EXPECT(std::fabs(rig.dev.get(EMS_POINT_NAMES[EMS_CMD_P_LOWER]) + 80.0) < 1e-3);

    // ---- CA 校验：公共地址不匹配必须被丢弃（不能把别的站的数据当自己的）----
    {
        MemoryDeviceIO          dev2;
        Iec104ControlledStationSim st(Iec104Profile::kStandard, /*ca=*/2);
        st.attach_device(&dev2);
        LoopbackIec104Transport tr(&st);
        Iec104DeviceIO          io(&tr, Iec104Profile::kStandard, /*ca=*/1);   // 故意不匹配
        EXPECT(io.open() == false);          // 总召数据始终到不了 → 建链判据不成立
        EXPECT(io.startdt_ok() == true);     // U 帧没有 CA，握手本身是通的
        EXPECT(io.gi_done() == false);
        EXPECT(io.bad_ca() > 0);             // 不匹配的帧被计数丢弃，而不是当成数据
        io.poll(0);
        EXPECT(io.bad_ca() > 0);
        // 数据一片也没进缓存（缓存仍是点表初值）
        EXPECT(std::fabs(io.cached_value(EMS_P_BAT)) < 1e-12);
        EXPECT(std::fabs(io.cached_value(EMS_SOC) - 0.5) < 1e-12);
    }

    // ---- 未知类型 / 畸形长度 / STOPDT：从站的处理必须可判定（不静默吞掉）----
    {
        MemoryDeviceIO          dev2;
        Iec104ControlledStationSim st(Iec104Profile::kStandard, 1);
        st.attach_device(&dev2);
        LoopbackIec104Transport tr(&st);
        EXPECT(tr.connect());

        uint8_t asdu[64], frame[128];
        // ① 未知类型标识：计数但不影响链路
        EXPECT(write_asdu_header(asdu, sizeof(asdu), 99, false, 1, kCOT_Act, false, false, 0, 1) == 6);
        put_u24_le(asdu + 6, 0x004301);
        asdu[9] = 0x00;
        size_t fn = wrap_i_frame(frame, sizeof(frame), 0, 0, asdu, 10);
        EXPECT(fn == 16 && tr.send(frame, fn));
        EXPECT(st.unknown_type() == 1);
        EXPECT(st.malformed() == 0);            // "不认识"不等于"格式错"
        EXPECT(st.started() == false);          // 没 STARTDT 就不该开始上送

        // ② 畸形长度：C_SE_NC_1 的信息体只有 1 字节
        const size_t an = write_asdu_header(asdu, sizeof(asdu), kC_SE_NC_1, false, 1,
                                            kCOT_Act, false, false, 0, 1);
        EXPECT(an == 6);
        put_u24_le(asdu + 6, 0x004301);
        fn = wrap_i_frame(frame, sizeof(frame), 1, 0, asdu, 7);
        EXPECT(tr.send(frame, fn));
        EXPECT(st.malformed() == 1);

        // ③ STARTDT → STOPDT：启动/停止必须都有确认
        uint8_t uf[8];
        size_t un = wrap_u_frame(uf, sizeof(uf), kStartDtAct);
        EXPECT(tr.send(uf, un));
        EXPECT(st.started() == true);
        un = wrap_u_frame(uf, sizeof(uf), kTestFrAct);
        EXPECT(tr.send(uf, un));
        uint8_t rx[512];
        const size_t rn = st.take(rx, sizeof(rx));
        EXPECT(rn > 0);
        // 收到的帧里必须能找到 STARTDT con 与 TESTFR con
        int found_start_con = 0, found_test_con = 0;
        size_t off = 0;
        while (off < rn) {
            ApciHeader ah;
            const size_t fn2 = parse_apdu(rx + off, rn - off, ah);
            if (fn2 == 0) break;
            if (ah.kind == kFrameU && ah.ufunc == kStartDtCon) ++found_start_con;
            if (ah.kind == kFrameU && ah.ufunc == kTestFrCon)  ++found_test_con;
            off += fn2;
        }
        EXPECT(found_start_con == 1);
        EXPECT(found_test_con == 1);
        un = wrap_u_frame(uf, sizeof(uf), kStopDtAct);
        EXPECT(tr.send(uf, un));
        EXPECT(st.started() == false);
    }

    std::cerr << "        总召 I 帧=1 / 心跳=" << rig.io.testfr_tx() << "/" << rig.io.testfr_rx()
              << " / S 帧=" << rig.io.s_frames_tx() << " / 最大未确认=" << max_unacked
              << "（k=" << kWindowK << "）\n";
}

// =====================================================================
// T44: 闭环等价 + 量化边界
// =====================================================================
static void test_44_closed_loop_equivalence() {
    std::cerr << "[T44] 闭环等价（104 链路 vs 进程内点表） ...\n";

    const int N = 400;

    MemoryRig ref;
    ref.run(N);

    // 运行 B：全部经 104 报文，介质用宽精度私有类型（承载 double 全部有效位）
    Iec104Rig wide(Iec104Profile::kWidePrivate);
    wide.run(N);

    EXPECT(ref.rt.log().size() == static_cast<std::size_t>(N));
    EXPECT(wide.rt.log().size() == static_cast<std::size_t>(N));
    EXPECT(std::string(ref.rt.device()->name()) == "MemoryDeviceIO(PointTable)");
    EXPECT(std::string(wide.rt.device()->name()) == "Iec104DeviceIO(104)");

    const int diff = diff_logs(ref.rt.log(), wide.rt.log(), 0.0, true);
    EXPECT(diff == 0);
    EXPECT(wide.rt.fsm().state() == ref.rt.fsm().state());
    EXPECT(wide.rt.fsm().history().size() == ref.rt.fsm().history().size());

    double max_cmd_ref = 0.0, max_cmd_wide = 0.0;
    for (const auto& r : ref.rt.log())  max_cmd_ref  = std::max(max_cmd_ref,  std::fabs(r.p_cmd));
    for (const auto& r : wide.rt.log()) max_cmd_wide = std::max(max_cmd_wide, std::fabs(r.p_cmd));
    EXPECT(max_cmd_ref > 50.0);
    EXPECT(almost_equal(max_cmd_ref, max_cmd_wide));
    EXPECT(std::fabs(ref.rt.log().back().soc - 0.5) > 1e-6);
    EXPECT(wide.io.stale_reads() == 0);
    EXPECT(wide.io.self_check() == 0);
    EXPECT(wide.io.bad_ca() == 0);
    EXPECT(wide.io.malformed() == 0);
    EXPECT(wide.io.unknown_type() == 0);
    EXPECT(wide.station.malformed() == 0);
    EXPECT(wide.station.bad_ca() == 0);
    EXPECT(wide.station.unknown_type() == 0);
    EXPECT(wide.station.vs() < 32768);        // 序号没回绕

    // ---- 运行 C：现场标准 M_ME_NC_1（float32）→ 量化边界 ----
    Iec104Rig std32(Iec104Profile::kStandard);
    std32.run(N);

    int topo_diff = 0;
    double max_abs_cmd_diff = 0.0, max_rel_cmd_diff = 0.0;
    const std::size_t n = std::min(ref.rt.log().size(), std32.rt.log().size());
    for (std::size_t i = 0; i < n; ++i) {
        const StepRecord& a = ref.rt.log()[i];
        const StepRecord& c = std32.rt.log()[i];
        if (a.state != c.state || a.state_gated != c.state_gated ||
            a.safety_clip != c.safety_clip || a.hold_last != c.hold_last ||
            a.fault_bits != c.fault_bits || a.clamped != c.clamped ||
            a.reason != c.reason) {
            ++topo_diff;
        }
        const double d = std::fabs(a.p_cmd - c.p_cmd);
        max_abs_cmd_diff = std::max(max_abs_cmd_diff, d);
        max_rel_cmd_diff = std::max(max_rel_cmd_diff, d / std::max(1.0, std::fabs(a.p_cmd)));
    }
    EXPECT(topo_diff == 0);
    EXPECT(std32.rt.fsm().state() == ref.rt.fsm().state());
    EXPECT(std32.io.stale_reads() == 0);
    EXPECT(std32.io.malformed() == 0);
    EXPECT(max_abs_cmd_diff < 0.5);
    EXPECT(max_rel_cmd_diff < 5e-4);
    EXPECT(std::fabs(ref.rt.log().back().soc - std32.rt.log().back().soc) < 1e-4);

    std::cerr << "        逐位差异 " << diff << " 拍 / 峰值指令 " << max_cmd_ref << " kW / SOC "
              << ref.rt.log().front().soc << " → " << ref.rt.log().back().soc << "\n";
    std::cerr << "        f32 量化边界: 决策拓扑差异 " << topo_diff << " 拍, |Δcmd|max="
              << max_abs_cmd_diff << " kW, 相对 " << max_rel_cmd_diff << "\n";
}

// =====================================================================
// T45: 通信中断（链路断 / 对端哑）与自愈
// =====================================================================
static void test_45_comm_interruption() {
    std::cerr << "[T45] 通信中断：链路断 + 对端哑（t3 判据）+ 自愈 ...\n";

    // ---- 模式 A：链路断开 → 数据不可信 → FAULT ----
    {
        Iec104Rig rig(Iec104Profile::kWidePrivate);
        rig.run(40);
        EXPECT(rig.io.stale_reads() == 0);
        EXPECT(rig.io.read_status().data_valid == true);
        const double p_bat_before = rig.io.cached_value(EMS_P_BAT);
        EXPECT(std::fabs(p_bat_before) > 1.0);

        const int stale_before = rig.io.stale_reads();
        rig.transport.set_link_up(false);
        EXPECT(rig.io.poll(0) == false);
        EXPECT(rig.io.stale_reads() > stale_before);
        EXPECT(rig.io.read_status().data_valid == false);
        EXPECT_NEAR(rig.io.cached_value(EMS_P_BAT), p_bat_before, 1e-9);  // 旧值被保留

        rig.run(5, 40);
        EXPECT((rig.rt.log().back().fault_bits & (1 << 4)) != 0);   // data_invalid
        EXPECT(rig.rt.fsm().state() == EmsState::kFault);
        EXPECT(std::fabs(rig.rt.log().back().p_cmd) < 1e-9);

        // 自愈
        rig.transport.set_link_up(true);
        EXPECT(rig.io.poll(0) == true);
        EXPECT(rig.io.read_status().data_valid == true);
        const int rx_before = rig.io.i_frames_rx();
        rig.run(20, 45);
        EXPECT(rig.io.i_frames_rx() > rx_before);
    }

    // ---- 模式 B：链路在、对端不上送（t3 判据）→ 同样判为不可信 ----
    {
        Iec104Rig rig(Iec104Profile::kWidePrivate);
        rig.io.set_stale_after_polls(3);      // 连续 >3 轮没有新 I 帧 → 不可信
        rig.run(40);
        EXPECT(rig.io.read_status().data_valid == true);
        EXPECT(rig.io.polls_without_data() == 0);

        const int stale_before = rig.io.stale_reads();
        rig.transport.inject_silence(100000);  // 对端"哑了"：链路在，但一个字节都不发
        rig.run(6, 40);
        EXPECT(rig.io.stale_reads() > stale_before);
        EXPECT(rig.io.polls_without_data() > 3);
        EXPECT((rig.rt.log().back().fault_bits & (1 << 4)) != 0);
        EXPECT(rig.rt.fsm().state() == EmsState::kFault);

        // 对端恢复上送 → 自愈
        rig.transport.inject_silence(0);
        EXPECT(rig.io.poll(0) == true);
        EXPECT(rig.io.polls_without_data() == 0);
        EXPECT(rig.io.read_status().data_valid == true);
    }

    // ---- 保持队列不因中断而乱序：序号必须单调、且窗口从未被击穿 ----
    {
        Iec104Rig rig(Iec104Profile::kWidePrivate);
        rig.run(30);
        uint16_t last_ns = 0;
        int backjumps = 0;
        for (int i = 0; i < 30; ++i) {
            const int vs_before = rig.io.vs();
            rig.io.poll(0);
            if (rig.io.vs() < vs_before) ++backjumps;    // 我方 N(S) 不该回退
            last_ns = rig.io.vs();
        }
        EXPECT(backjumps == 0);
        EXPECT(last_ns == static_cast<uint16_t>(rig.io.i_frames_tx()));
        EXPECT(rig.io.unacked_tx() <= kWindowK);
    }

    std::cerr << "        链路断 → FAULT / 对端哑（t3）→ FAULT / 恢复后自愈\n";
}

// =====================================================================
// T46: 映射契约（30 点 ↔ IOA ↔ 类型标识）
// =====================================================================
static void test_46_map_contract() {
    std::cerr << "[T46] 映射契约：30 点点名/段归属/IOA/类型标识 ...\n";

    for (Iec104Profile prof : {Iec104Profile::kStandard, Iec104Profile::kWidePrivate}) {
        Iec104PointMap m = build_iec104_map(prof);
        const bool wide = (prof == Iec104Profile::kWidePrivate);
        EXPECT(m.meas_type_id == (wide ? kM_ME_WIDE : kM_ME_NC_1));
        EXPECT(m.count_of_group(0) == 7);
        EXPECT(m.count_of_group(1) == 14);
        EXPECT(m.count_of_group(2) == 6);
        EXPECT(m.count_of_group(3) == 3);

        for (int i = 0; i < Iec104PointMap::kPointCount; ++i) {
            EXPECT(std::strcmp(m.pts[i].point, EMS_POINT_NAMES[i]) == 0);
            const int g = iec104_point_group(i);
            EXPECT(m.pts[i].group == g);
            const uint32_t base = m.base_of_group(g);
            const int first = m.first_index_of_group(g);
            EXPECT(m.pts[i].ioa == base + static_cast<uint32_t>(i - first));
            if (g == 0 || g == 1) {
                EXPECT(m.pts[i].type_id == m.meas_type_id);
            } else if (g == 2) {
                EXPECT(m.pts[i].type_id == kM_SP_NA_1);
            } else {
                EXPECT(m.pts[i].type_id == kC_SE_NC_1);
            }
            EXPECT(m.index_of_ioa(m.pts[i].ioa) == i);
            EXPECT(m.find_point(EMS_POINT_NAMES[i]) != nullptr);
        }
        // IOA 全局唯一（无重叠）
        for (int i = 0; i < Iec104PointMap::kPointCount; ++i) {
            int hits = 0;
            for (int j = 0; j < Iec104PointMap::kPointCount; ++j) {
                if (m.pts[j].ioa == m.pts[i].ioa) ++hits;
            }
            EXPECT(hits == 1);
        }
        EXPECT(m.index_of_ioa(0x00FFFF) == -1);

        // 适配器自检（点名/段/IOA/类型四者一致）
        Iec104ControlledStationSim st(prof, 1);
        LoopbackIec104Transport    tr(&st);
        Iec104DeviceIO             io(&tr, prof, 1);
        EXPECT(io.self_check() == 0);
    }

    // ---- 设备侧替身与真相源同方言 ----
    MemoryDeviceIO dev;
    std::size_t alias = 0;
    for (int i = 0; i < EMS_POINT_COUNT; ++i) {
        if (dev.has_point(EMS_POINT_NAMES[i])) ++alias;
    }
    EXPECT(alias == static_cast<std::size_t>(EMS_POINT_COUNT));

    // ---- 段地址区间（现场用抓包工具时按这个读）----
    Iec104PointMap ms = build_iec104_map(Iec104Profile::kStandard);
    EXPECT(ms.pts[0].ioa == 0x004001);
    EXPECT(ms.pts[EMS_CMD_P_BAT].ioa == 0x004301);
    EXPECT(ms.pts[EMS_CFG_CAP_KWH].ioa == 0x004101);
    EXPECT(ms.pts[EMS_STA_BMS].ioa == 0x004201);

    // ---- 标准类型的**分辨率**是客观事实：float32 装不下 0.1 ----
    {
        InfoFloat it[1];
        it[0].ioa = 0x004001; it[0].value = 0.1; it[0].qds = 0;
        uint8_t asdu[64], frame[96];
        const size_t an = make_asdu_measurements(asdu, sizeof(asdu), kCOT_Per, 1, it, 1);
        const size_t fn = wrap_i_frame(frame, sizeof(frame), 0, 0, asdu, an);
        ApciHeader ah; AsduView v;
        EXPECT(parse_asdu(frame, fn, ah, v));
        std::vector<InfoFloat> out;
        EXPECT(decode_measurements(v, out) && out.size() == 1);
        EXPECT(std::fabs(out[0].value - 0.1) > 0.0);          // 有损
        EXPECT(std::fabs(out[0].value - 0.1) < 1e-8);         // 但误差在 float32 分辨率内
        // 宽精度通道对同一个值是无损的
        const size_t an2 = make_asdu_wide(asdu, sizeof(asdu), kCOT_Per, 1, it, 1);
        const size_t fn2 = wrap_i_frame(frame, sizeof(frame), 0, 0, asdu, an2);
        EXPECT(parse_asdu(frame, fn2, ah, v));
        std::vector<InfoFloat> out2;
        EXPECT(decode_wide(v, out2) && std::memcmp(&it[0].value, &out2[0].value, 8) == 0);
    }

    std::cerr << "        IOA 段: 遥测 0x4001+ / 参数 0x4101+ / 状态 0x4201+ / 遥调 0x4301+\n";
}

// =====================================================================
int main() {
    std::cerr << "=========================================\n"
              << " P3/ IEC 60870-5-104 通信适配器 单元测试\n"
              << "=========================================\n";

    test_41_apci_byte_exact();
    test_42_asdu_byte_exact();
    test_46_map_contract();
    test_43_session_and_windows();
    test_44_closed_loop_equivalence();
    test_45_comm_interruption();

    std::cerr << "=========================================\n"
              << " PASS=" << g_pass << "  FAIL=" << g_fail << "\n"
              << "=========================================\n";
    return (g_fail == 0) ? 0 : 1;
}
