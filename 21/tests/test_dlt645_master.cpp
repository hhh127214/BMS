// =====================================================================
// 21/ 单元测试（二）—— DL/T645 主站 + 进程内假表
//
//   T31 基本读成功（一次成）
//   T32 超时 → kTimeout，且重试次数正确
//   T33 前 N 次丢包 → 重试后成功（★ ok_after_retry 必须被点亮）
//   T34 CS 错 → BAD_FRAME
//   T35 半帧 → TIMEOUT（不能误报成"帧坏了"）
//   T36 异常应答 0xD1 → ABNORMAL + 错误码
//   T37 应答地址不符 → ADDR_MISMATCH
//   T38 应答 DI 不符 → DI_MISMATCH
//   T39 数据域漏加 0x33（厂家方言错）→ BAD_FRAME（BCD 越界）
//   T40 变比端到端
//   T41 统计口径分类（超时 / 坏帧 / 异常 不合并）
//   T42 ★ 残留应答被清缓冲挡住（否则读到上一轮的值）
//   T43 表只应答发给自己的帧（多表共总线）
//   T44 本地无此 DI → 根本不发请求
//   T45 链路未打开 → LINK_ERROR
//
// ---------------------------------------------------------------------
// ★ 这组测试要回答的问题不是"能不能读到数"，而是
//   "读数不对时，**能不能分清是哪一种不对**"
// ---------------------------------------------------------------------
// 现场排查最怕的不是故障，是"所有故障长得一样"：
//   · 超时多  → 查线、查波特率、查地址
//   · 坏帧多  → 查干扰、查网关分片、查厂家方言
//   · 异常码多 → 查 DI 表、查表型是否支持该点
//   · 地址不符 → 查多表地址配置
// 这四类如果合成一个 fail 计数，现场只能"换块表试试"。
// 所以 T41 专门断言它们**分别计数**。
// =====================================================================

#include "dlt645_codec.h"
#include "dlt645_frame.h"
#include "dlt645_master.h"
#include "dlt645_transport.h"
#include "fake_meter_slave.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace ems::dlt645;

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

#define EXPECT_EQ(a, b)                                                   \
    do {                                                                  \
        long long va = (long long)(a), vb = (long long)(b);               \
        if (va == vb) {                                                   \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << " : " << #a << "=" << va << " vs " << #b << "="  \
                      << vb << std::endl;                                 \
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

// ---------------------------------------------------------------------
// 测试台：一条内存回环链路 + 一台假表 + 一个主站
// ---------------------------------------------------------------------
struct Rig {
    Address addr;
    BytePipe to_slave;    // 主站 → 表
    BytePipe to_master;   // 表 → 主站
    LoopbackLink link;
    test::FakeMeterSlave slave;
    Dlt645Master master;

    explicit Rig(const MasterOptions& opt = MasterOptions())
        : link(&to_slave, &to_master),
          slave(&to_slave, &to_master, addr_default()),
          master(&link, addr_default(), opt) {
        addr.from_string("000000000001");
        slave.set_options(default_opts());
        master.set_pump([this] { slave.poll(); });
        link.open();
    }

    static Address addr_default() {
        Address a;
        a.from_string("000000000001");
        return a;
    }

    static test::FakeMeterSlave::Options default_opts() {
        test::FakeMeterSlave::Options o;
        o.corrupt_cs = false;
        return o;
    }
};

static const std::uint32_t kVoltDi = 0x02010100u;

// =====================================================================
// T31 基本读
// =====================================================================
static void t31_basic_read() {
    std::printf("T31 基本读成功（一次成）\n");
    Rig rig;
    rig.slave.set_value(kVoltDi, 220.5);

    ReadResult r = rig.master.read(kVoltDi);
    EXPECT(r.st == ReadStatus::kOk);
    EXPECT_NEAR(r.value, 220.5, 1e-9);
    EXPECT_EQ(r.attempts, 1);
    EXPECT_EQ(rig.slave.rx_frames(), 1);
    EXPECT_EQ(rig.master.stats().ok, 1);
    EXPECT_EQ(rig.master.stats().retries, 0);

    // 再读一次，值改了必须跟着变（防止"缓存了第一次的值"）
    rig.slave.set_value(kVoltDi, 231.4);
    ReadResult r2 = rig.master.read(kVoltDi);
    EXPECT(r2.st == ReadStatus::kOk);
    EXPECT_NEAR(r2.value, 231.4, 1e-9);
}

// =====================================================================
// T32 超时
// =====================================================================
static void t32_timeout() {
    std::printf("T32 超时 → 重试次数正确\n");
    MasterOptions o;
    o.retries = 2;
    Rig rig(o);
    rig.slave.set_value(kVoltDi, 220.5);
    rig.slave.options().drop_response = true;

    ReadResult r = rig.master.read(kVoltDi);
    EXPECT(r.st == ReadStatus::kTimeout);
    EXPECT_EQ(r.attempts, 3);              // 1 次首发 + 2 次重试
    EXPECT_EQ(rig.master.stats().requests, 3);
    EXPECT_EQ(rig.master.stats().retries, 2);
    EXPECT_EQ(rig.master.stats().ok, 0);
    EXPECT_EQ(rig.master.stats().timeouts, 3);
    // ★ 超时必须**不是** 0 值：把超时当 0 会让"0 V"看起来完全正常
    EXPECT(r.st != ReadStatus::kOk);
}

// =====================================================================
// T33 重试后成功
// =====================================================================
static void t33_ok_after_retry() {
    std::printf("T33 前 N 次丢包 → 重试后成功\n");
    MasterOptions o;
    o.retries = 2;
    Rig rig(o);
    rig.slave.set_value(kVoltDi, 220.5);
    rig.slave.options().drop_first = 1;    // 第一次不回

    ReadResult r = rig.master.read(kVoltDi);
    EXPECT(r.st == ReadStatus::kOk);
    EXPECT_EQ(r.attempts, 2);
    EXPECT_NEAR(r.value, 220.5, 1e-9);

    // ★ 这条是"重试真的起作用了"的唯一正面证据。
    //   只断言 ok==1 是看不出区别的（一次就成也满足）。
    EXPECT_EQ(rig.master.stats().ok_after_retry, 1);
    EXPECT_EQ(rig.master.stats().retries, 1);
    EXPECT_EQ(rig.master.stats().timeouts, 1);
}

// =====================================================================
// T34 CS 错
// =====================================================================
static void t34_bad_cs() {
    std::printf("T34 CS 错 → BAD_FRAME\n");
    MasterOptions o;
    o.retries = 0;
    Rig rig(o);
    rig.slave.set_value(kVoltDi, 220.5);
    rig.slave.options().corrupt_cs = true;

    ReadResult r = rig.master.read(kVoltDi);
    EXPECT(r.st == ReadStatus::kBadFrame);
    EXPECT_EQ(rig.master.stats().bad_frames, 1);
    EXPECT_EQ(rig.master.stats().timeouts, 0);   // ★ 坏帧不能算成超时
}

// =====================================================================
// T35 半帧
// =====================================================================
static void t35_half_frame() {
    std::printf("T35 半帧 → TIMEOUT（不是 BAD_FRAME）\n");
    MasterOptions o;
    o.retries = 0;
    Rig rig(o);
    rig.slave.set_value(kVoltDi, 220.5);
    rig.slave.options().truncate_tail = true;

    ReadResult r = rig.master.read(kVoltDi);
    // ★ 为什么必须是 TIMEOUT 而不是 BAD_FRAME：
    //   "少了尾巴"在时间维度上是"还没收完"，重发整帧即可；
    //   而"CS 错"是收到了完整帧但内容坏了。两者的现场处置完全不同
    //   （前者查网关分片/波特率，后者查干扰/表方言）。
    EXPECT(r.st == ReadStatus::kTimeout);
    EXPECT_EQ(rig.master.stats().timeouts, 1);
    EXPECT_EQ(rig.master.stats().bad_frames, 0);
}

// =====================================================================
// T36 异常应答
// =====================================================================
static void t36_abnormal() {
    std::printf("T36 异常应答 0xD1 → ABNORMAL + 错误码\n");
    MasterOptions o;
    o.retries = 0;
    Rig rig(o);
    rig.slave.set_value(kVoltDi, 220.5);
    rig.slave.options().reject_all = true;

    ReadResult r = rig.master.read(kVoltDi);
    EXPECT(r.st == ReadStatus::kAbnormal);
    EXPECT_EQ(r.err_code, kErrNoData);
    EXPECT(std::strlen(r.why) > 0);
    // ★ 异常应答只有 1 字节数据域。若先按"读数据应答"解析，
    //   会因长度不足报 BAD_FRAME —— 把"表说没这个点"误诊成"线路坏了"。
    EXPECT_EQ(rig.master.stats().abnormals, 1);
    EXPECT_EQ(rig.master.stats().bad_frames, 0);
}

// =====================================================================
// T37 地址不符
// =====================================================================
static void t37_addr_mismatch() {
    std::printf("T37 应答地址不符 → ADDR_MISMATCH\n");
    MasterOptions o;
    o.retries = 0;
    Rig rig(o);
    rig.slave.set_value(kVoltDi, 220.5);
    rig.slave.options().wrong_address = true;

    ReadResult r = rig.master.read(kVoltDi);
    EXPECT(r.st == ReadStatus::kAddrMismatch);
    EXPECT_EQ(rig.master.stats().addr_mismatch, 1);
    // ★ 若不做地址校验，这一帧的值（220.5）会被当成**本表**的值收下 ——
    //   多表共总线时就是"隔壁表的电压"。这是最隐蔽的一类错。
    EXPECT(r.st != ReadStatus::kOk);
}

// =====================================================================
// T38 DI 不符
// =====================================================================
static void t38_di_mismatch() {
    std::printf("T38 应答 DI 不符 → DI_MISMATCH\n");
    MasterOptions o;
    o.retries = 0;
    Rig rig(o);
    rig.slave.set_value(kVoltDi, 220.5);
    rig.slave.options().reply_wrong_di = true;

    ReadResult r = rig.master.read(kVoltDi);
    EXPECT(r.st == ReadStatus::kDiMismatch);
    EXPECT_EQ(rig.master.stats().di_mismatch, 1);
}

// =====================================================================
// T39 漏加 0x33
// =====================================================================
static void t39_missing_pad() {
    std::printf("T39 数据域漏加 0x33 → BAD_FRAME\n");
    MasterOptions o;
    o.retries = 0;
    Rig rig(o);
    rig.slave.set_value(kVoltDi, 220.5);
    rig.slave.options().corrupt_pad = true;

    ReadResult r = rig.master.read(kVoltDi);
    // DI 是对的（假表只破坏数据域），所以不会掉进 DI_MISMATCH；
    // 减 0x33 之后字节变成 0xD2 0xEF，半字节越界 → 解码拒绝。
    EXPECT(r.st == ReadStatus::kBadFrame);
    EXPECT_EQ(rig.master.stats().di_mismatch, 0);
}

// =====================================================================
// T40 变比
// =====================================================================
static void t40_scale() {
    std::printf("T40 变比端到端\n");
    Rig rig;
    rig.slave.set_value(kVoltDi, 220.5);
    Scale sc;
    sc.mul = 40.0;

    ReadResult r = rig.master.read(kVoltDi, sc);
    EXPECT(r.st == ReadStatus::kOk);
    EXPECT_NEAR(r.raw, 220.5, 1e-9);
    EXPECT_NEAR(r.value, 8820.0, 1e-9);
}

// =====================================================================
// T41 统计口径
// =====================================================================
static void t41_stats_classification() {
    std::printf("T41 统计口径分类\n");
    MasterOptions o;
    o.retries = 0;
    Rig rig(o);
    rig.slave.set_value(kVoltDi, 220.5);

    // 正常 2 次
    EXPECT(rig.master.read(kVoltDi).st == ReadStatus::kOk);
    EXPECT(rig.master.read(kVoltDi).st == ReadStatus::kOk);
    // 超时 1 次
    rig.slave.options().drop_response = true;
    EXPECT(rig.master.read(kVoltDi).st == ReadStatus::kTimeout);
    rig.slave.options().drop_response = false;
    // 坏帧 1 次
    rig.slave.options().corrupt_cs = true;
    EXPECT(rig.master.read(kVoltDi).st == ReadStatus::kBadFrame);
    rig.slave.options().corrupt_cs = false;
    // 异常 1 次
    rig.slave.options().reject_all = true;
    EXPECT(rig.master.read(kVoltDi).st == ReadStatus::kAbnormal);

    const MasterStats& s = rig.master.stats();
    EXPECT_EQ(s.requests, 5);
    EXPECT_EQ(s.ok, 2);
    EXPECT_EQ(s.timeouts, 1);
    EXPECT_EQ(s.bad_frames, 1);
    EXPECT_EQ(s.abnormals, 1);
    // 四类相加必须等于失败数（3），不能有"没归类"的失败
    EXPECT_EQ(s.timeouts + s.bad_frames + s.abnormals +
              s.addr_mismatch + s.di_mismatch, s.requests - s.ok);
}

// =====================================================================
// T42 残留应答
// =====================================================================
static void t42_stale_response() {
    std::printf("T42 残留应答被清缓冲挡住\n");
    Rig rig;
    rig.slave.set_value(kVoltDi, 222.2);

    // 先造一个"上一轮的应答"塞进接收缓冲（模拟迟到帧）
    Frame old;
    old.addr = rig.addr;
    old.ctrl = kReadDataResp;
    std::uint8_t diw[4];
    di_to_wire(kVoltDi, diw);
    old.data.assign(diw, diw + 4);
    const DiSpec* spec = find_builtin(kVoltDi);
    std::vector<std::uint8_t> body;
    const char* why = nullptr;
    EXPECT(spec != nullptr && encode_bcd(*spec, 111.1, Scale(), &body, &why));
    old.data.insert(old.data.end(), body.begin(), body.end());
    const std::vector<std::uint8_t> stale = encode(old, 2);
    rig.to_master.push(stale.data(), stale.size());
    EXPECT_EQ(rig.to_master.size(), stale.size());

    ReadResult r = rig.master.read(kVoltDi);
    EXPECT(r.st == ReadStatus::kOk);
    // ★ 必须是新值 222.2，不是残留帧里的 111.1。
    //   删掉 transact() 开头那行 flush_input()，这条立刻变红 ——
    //   这正是"值全对但全是上一次的"那类现场故障。
    EXPECT_NEAR(r.value, 222.2, 1e-9);
    EXPECT(std::fabs(r.value - 111.1) > 1.0);   // 反向守卫：两值确实不同
}

// =====================================================================
// T43 只应答自己的帧
// =====================================================================
static void t43_address_filter() {
    std::printf("T43 表只应答发给自己的帧\n");
    Rig rig;
    rig.slave.set_value(kVoltDi, 220.5);

    // 手工发一帧给"另一个地址"，表必须装作没看见
    Address other;
    other.from_string("000000000099");
    BytePipe sink;   // 丢弃表发出来的东西
    test::FakeMeterSlave s2(&rig.to_slave, &sink, other);

    Frame req = Dlt645Master::make_read_request(rig.addr, kVoltDi);
    const std::vector<std::uint8_t> raw = encode(req, 2);
    rig.to_slave.push(raw.data(), raw.size());

    // rig.slave 的地址是 ...001，请求发给 ...001 → 它应处理
    // s2 的地址是 ...099 → 它必须忽略
    s2.poll();
    EXPECT_EQ(s2.ignored(), 1);
    EXPECT_EQ(s2.tx_frames(), 0);

    // 清掉 s2 没消费的字节，避免干扰后续
    rig.to_slave.clear();
}

// =====================================================================
// T44 本地无此 DI
// =====================================================================
static void t44_no_spec() {
    std::printf("T44 本地无此 DI → 不发请求\n");
    Rig rig;
    const int before = rig.slave.rx_frames();

    ReadResult r = rig.master.read(0xDEADBEEFu);
    EXPECT(r.st == ReadStatus::kNoSpec);
    // ★ 关键：**一个字节都不该发出去**。
    //   若"不知道格式也先读回来再说"，会把长度不定的数据当已知格式解，
    //   解出一个数 —— 而那是最难查的一类假数据。
    EXPECT_EQ(rig.slave.rx_frames(), before);
}

// =====================================================================
// T45 链路未打开
// =====================================================================
static void t45_link_closed() {
    std::printf("T45 链路未打开 → LINK_ERROR\n");
    Rig rig;
    rig.slave.set_value(kVoltDi, 220.5);
    rig.link.close();

    ReadResult r = rig.master.read(kVoltDi);
    EXPECT(r.st == ReadStatus::kLinkError);
    // 链路层失败不该白白重试
    EXPECT_EQ(r.attempts, 1);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    t31_basic_read();
    t32_timeout();
    t33_ok_after_retry();
    t34_bad_cs();
    t35_half_frame();
    t36_abnormal();
    t37_addr_mismatch();
    t38_di_mismatch();
    t39_missing_pad();
    t40_scale();
    t41_stats_classification();
    t42_stale_response();
    t43_address_filter();
    t44_no_spec();
    t45_link_closed();

    std::printf("\nPASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
