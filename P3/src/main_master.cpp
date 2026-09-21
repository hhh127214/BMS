// =====================================================================
// P3 / IEC104 主站模拟器（调度侧对端工具）
//
// 用途：
//   1) 联调时替代真实调度主站 —— 我们的从站（iec104_gateway.exe）有没有问题，
//      用它一连就知道；
//   2) 对接第三方系统时，先用它验证「对方主站能不能收到我们的数据」；
//   3) 现场排障：把收到的每个 IOA 按 iec104_point_map.h 翻成中文名打出来，
//      不用再对着转发表数点位。
//
// 用法：
//   iec104_master.exe [--host 127.0.0.1] [--port 2404] [--ca 1]
//                     [--seconds 30] [--gi-interval 10]
//                     [--set 6001 120.5] [--cmd 5001 1] [--dump-raw]
//
// 行为：连接 → STARTDT → 立即总召唤一次，之后每 gi-interval 秒再召一次
//       → 收到的所有 ASDU 打表 → 到期退出并给汇总。
//
// ---------------------------------------------------------------------
// 为什么这个文件要 include 库的「内部」头
// ---------------------------------------------------------------------
// lib60870 没有为带时标类型（M_ME_TF_1 / M_SP_TB_1）提供公开取值接口，
// 只有 _create / _destroy / _getTimestamp。主站侧要读值只能用内部结构体
// （information_objects_internal.h）。**从站侧（我们的网关）不需要**，
// 因为它只负责发。详见 vendor/lib60870/README.md 与 docs/README.md。
// =====================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "cs104_connection.h"
#include "hal_time.h"

#include "information_objects_internal.h"   // 见文件头说明

#include "iec104_point_map.h"
#include "iec104_time.h"

#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

struct ConnState {
    bool opened = false, startdt = false, closed = false, failed = false;
    int  gi_con = 0, gi_term = 0;
    int  cmd_pos = 0, cmd_neg = 0;
    int  rx_asdu = 0;
};

ConnState g_st;

void on_connection(void*, CS104_Connection, CS104_ConnectionEvent e) {
    switch (e) {
    case CS104_CONNECTION_OPENED:               g_st.opened  = true; std::printf("[CONN] 已连接\n"); break;
    case CS104_CONNECTION_CLOSED:               g_st.closed  = true; std::printf("[CONN] 连接关闭\n"); break;
    case CS104_CONNECTION_FAILED:               g_st.failed  = true; std::printf("[CONN] 连接失败\n"); break;
    case CS104_CONNECTION_STARTDT_CON_RECEIVED: g_st.startdt = true; std::printf("[CONN] STARTDT 确认，数据传输已启动\n"); break;
    case CS104_CONNECTION_STOPDT_CON_RECEIVED:  std::printf("[CONN] STOPDT 确认\n"); break;
    }
}

const char* cot_text(int cot) {
    switch (cot) {
    case 1:  return "周期";
    case 3:  return "自发";
    case 6:  return "激活";
    case 7:  return "激活确认";
    case 10: return "激活终止";
    case 20: return "总召唤";
    default: return "其它";
    }
}

// 把一个 IOA 翻成中文名（现场排障的关键：不用对着转发表数点位）
std::string name_of(int ioa) {
    std::size_t n = 0;
    const iec104::PointDef* up = iec104::upload_table(&n);
    if (const iec104::PointDef* d = iec104::find_by_ioa(up, n, ioa))
        return std::string(d->name) + " [" + d->src + "]";
    const iec104::PointDef* dn = iec104::download_table(&n);
    if (const iec104::PointDef* d = iec104::find_by_ioa(dn, n, ioa))
        return std::string(d->name) + " (下行点)";
    return "(未知 IOA)";
}

std::string ts_text(std::uint64_t wire_ms) {
    // wire_ms 是 CP56Time2a 字段按库的 UTC 口径还原出来的值；
    // 我们按北京时间写入过 +8h，这里原样显示就是北京时间。
    const std::time_t t = static_cast<std::time_t>(wire_ms / 1000);
    struct tm g{};
#if defined(_WIN32)
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
                  g.tm_hour, g.tm_min, g.tm_sec,
                  static_cast<int>(wire_ms % 1000));
    return buf;
}

// 品质字节的 4 个位（IEC 60870-5-101 §7.2.6.3）。
// 主站侧完全解码，而不是只看"非 0 就是坏点"——
// NT（陈旧）与 IV（不可用）在现场是两种完全不同的处置：
//   NT → 画面照显示，标灰，禁止拿去算（值还是真的）
//   IV → 画面显示"----"，告警
const char* quality_text(int q) {
    if (q == 0) return nullptr;
    static thread_local char buf[96];
    char bits[64] = "";
    auto add = [&bits](const char* s) { std::strcat(bits, s); };
    if (q & 0x80) add("IV ");
    if (q & 0x40) add("NT ");
    if (q & 0x20) add("SB ");
    if (q & 0x10) add("BL ");
    if (q & 0x01) add("OV ");
    std::snprintf(buf, sizeof(buf), "   [品质 0x%02X %s]", q, bits);
    return buf;
}

void print_one(int ioa, const char* kind_txt, double value,
               int quality, bool has_ts, std::uint64_t ts_wire_ms) {
    // 注意：时标串必须放进具名 std::string 再取 c_str() ——
    // 写成 `(cond ? ("..." + f()).c_str() : "")` 会拿到已经析构的临时对象。
    std::string tail;
    if (has_ts) tail = "   @ " + ts_text(ts_wire_ms);
    if (const char* qt = quality_text(quality)) tail += qt;
    std::printf("    IOA %-5d %-18s %-26s = %12.3f%s\n",
                ioa, kind_txt, name_of(ioa).c_str(), value, tail.c_str());
}

void on_raw(void*, uint8_t* msg, int size, bool sent) {
    static int dumped = 0;
    if (dumped++ > 400) return;   // 只打前 400 帧，避免刷屏
    std::printf("  [%s] ", sent ? "TX" : "RX");
    for (int i = 0; i < size && i < 32; ++i) std::printf("%02X ", msg[i]);
    if (size > 32) std::printf("... (%d B)", size);
    std::printf("\n");
}

bool on_asdu(void*, int, CS101_ASDU asdu) {
    const int type = static_cast<int>(CS101_ASDU_getTypeID(asdu));
    const int cot  = static_cast<int>(CS101_ASDU_getCOT(asdu));
    const int n    = CS101_ASDU_getNumberOfElements(asdu);
    ++g_st.rx_asdu;

    if (type == 100) {   // C_IC_NA_1
        if (cot == 7)  { ++g_st.gi_con;  std::printf("  <- 总召唤 激活确认\n"); }
        if (cot == 10) { ++g_st.gi_term; std::printf("  <- 总召唤 激活终止（本次结束）\n"); }
        return true;
    }
    if ((type == 45 || type == 46 || type == 50) && cot == 7) {
        const bool neg = CS101_ASDU_isNegative(asdu);
        if (neg) { ++g_st.cmd_neg; std::printf("  <- 命令 否定确认（未受理）\n"); }
        else     { ++g_st.cmd_pos; std::printf("  <- 命令 肯定确认（已受理）\n"); }
        return true;
    }

    std::printf("  <- ASDU type=%d COT=%s(%d) CA=%d 共 %d 点\n",
                type, cot_text(cot), cot, CS101_ASDU_getCA(asdu), n);

    for (int i = 0; i < n; ++i) {
        InformationObject io = CS101_ASDU_getElement(asdu, i);
        if (io == nullptr) continue;
        const int ioa = InformationObject_getObjectAddress(io);
        switch (type) {
        case M_ME_NC_1: {   // 13 短浮点遥测
            auto m = reinterpret_cast<MeasuredValueShort>(io);
            print_one(ioa, "遥测/短浮点",
                      MeasuredValueShort_getValue(m),
                      MeasuredValueShort_getQuality(m), false, 0);
            break;
        }
        case M_ME_TF_1: {   // 36 短浮点遥测 + 时标
            auto m = reinterpret_cast<struct sMeasuredValueShortWithCP56Time2a*>(io);
            print_one(ioa, "遥测/短浮点+时标", m->value, m->quality,
                      true, CP56Time2a_toMsTimestamp(&m->timestamp));
            break;
        }
        case M_ME_NB_1: {   // 11 标度化遥测
            auto m = reinterpret_cast<MeasuredValueScaled>(io);
            print_one(ioa, "遥测/标度化",
                      MeasuredValueScaled_getValue(m),
                      MeasuredValueScaled_getQuality(m), false, 0);
            break;
        }
        // 说明：M_ME_TE_1(35，标度化+时标) 没在这里解析 ——
        // 它的内部结构体把值存成 encodedValue[2] 而不是 double/float
        // （见 information_objects_internal.h:569），且本项目的上送表只用
        // M_ME_NC_1 / M_ME_TF_1 / M_SP_NA_1，不会发出这个类型。
        case M_SP_NA_1: {   // 1 单点遥信
            auto s = reinterpret_cast<SinglePointInformation>(io);
            print_one(ioa, "遥信/单点",
                      SinglePointInformation_getValue(s) ? 1.0 : 0.0,
                      SinglePointInformation_getQuality(s), false, 0);
            break;
        }
        case M_SP_TB_1: {   // 30 单点遥信 + 时标
            auto s = reinterpret_cast<struct sSinglePointWithCP56Time2a*>(io);
            print_one(ioa, "遥信/单点+时标", s->value ? 1.0 : 0.0, s->quality,
                      true, CP56Time2a_toMsTimestamp(&s->timestamp));
            break;
        }
        default:
            std::printf("    IOA %-5d (type %d 未在本工具里解析)\n", ioa, type);
            break;
        }
        InformationObject_destroy(io);
    }
    return true;
}

void usage() {
    std::printf(
        "iec104_master — IEC 60870-5-104 主站（调度侧对端模拟器）\n"
        "\n"
        "  --host IP        从站地址（默认 127.0.0.1）\n"
        "  --port N         端口（默认 2404）\n"
        "  --ca N           公共地址（默认 1）\n"
        "  --seconds N      运行时长（默认 20）\n"
        "  --gi-interval N  每隔 N 秒重发一次总召唤（默认 10，0 = 只召一次）\n"
        "  --set IOA VALUE  下发遥调（C_SE_NC_1，浮点设点）\n"
        "  --cmd IOA 0|1    下发遥控（C_SC_NA_1，单点命令）\n"
        "  --dump-raw       打印原始报文帧\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 2404, ca = 1;
    double seconds = 20.0, gi_interval = 10.0;
    bool dump_raw = false;
    std::vector<std::pair<int, double>> sets;
    std::vector<std::pair<int, int>>    cmds;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool nx = (i + 1 < argc);
        if      (a == "--host" && nx) host = argv[++i];
        else if (a == "--port" && nx) port = std::atoi(argv[++i]);
        else if (a == "--ca"   && nx) ca   = std::atoi(argv[++i]);
        else if (a == "--seconds" && nx) seconds = std::atof(argv[++i]);
        else if (a == "--gi-interval" && nx) gi_interval = std::atof(argv[++i]);
        else if (a == "--set" && i + 2 < argc) {
            const int ioa = std::atoi(argv[++i]);
            const double v = std::atof(argv[++i]);
            sets.push_back({ioa, v});
        }
        else if (a == "--cmd" && i + 2 < argc) {
            const int ioa = std::atoi(argv[++i]);
            const int v = std::atoi(argv[++i]);
            cmds.push_back({ioa, v});
        }
        else if (a == "--dump-raw") dump_raw = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::printf("未知参数：%s\n", a.c_str()); usage(); return 2; }
    }

    std::printf("=========================================================\n");
    std::printf(" IEC104 主站模拟器 → %s:%d  (CA=%d)\n", host.c_str(), port, ca);
    std::printf("=========================================================\n");

    CS104_Connection con = CS104_Connection_create(host.c_str(), port);
    if (con == nullptr) { std::printf("[FATAL] 创建连接对象失败\n"); return 1; }
    CS104_Connection_setConnectionHandler(con, on_connection, nullptr);
    CS104_Connection_setASDUReceivedHandler(con, on_asdu, nullptr);
    if (dump_raw) CS104_Connection_setRawMessageHandler(con, on_raw, nullptr);

    // 对钟：告诉从站当前本地时间（北京时间口径，见 iec104_time.h）
    if (!CS104_Connection_connect(con)) {
        std::printf("[FATAL] 连不上 %s:%d —— 从站起了吗？防火墙放行了吗？\n",
                    host.c_str(), port);
        CS104_Connection_destroy(con);
        return 1;
    }
    CS104_Connection_sendStartDT(con);

    // 等 STARTDT_CON（从站收到 STARTDT 之前不会主动上报）
    for (int i = 0; i < 200 && !g_st.startdt; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!g_st.startdt) std::printf("[WARN] 没等到 STARTDT_CON，继续尝试\n");

    // 对钟
    {
        struct sCP56Time2a ts;
        iec104::make_local_cp56time2a(&ts, iec104::now_utc_ms());
        CS104_Connection_sendClockSyncCommand(con, ca, &ts);
    }

    // 首次总召唤
    std::printf("\n-- 总召唤（站召唤 QOI=20）--\n");
    CS104_Connection_sendInterrogationCommand(con, CS101_COT_ACTIVATION, ca,
                                              IEC60870_QOI_STATION);

    // 下发命令（等 1 s 让总召唤数据先回来）
    if (!sets.empty() || !cmds.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        for (auto& s : sets) {
            std::printf("\n-- 下发遥调 IOA=%d value=%.3f (%s) --\n",
                        s.first, s.second, name_of(s.first).c_str());
            InformationObject io = reinterpret_cast<InformationObject>(
                SetpointCommandShort_create(nullptr, s.first,
                                            static_cast<float>(s.second), false, 0));
            CS104_Connection_sendProcessCommandEx(con, CS101_COT_ACTIVATION, ca, io);
            InformationObject_destroy(io);
        }
        for (auto& c : cmds) {
            std::printf("\n-- 下发遥控 IOA=%d state=%d (%s) --\n",
                        c.first, c.second, name_of(c.first).c_str());
            InformationObject io = reinterpret_cast<InformationObject>(
                SingleCommand_create(nullptr, c.first, c.second != 0, false, 0));
            CS104_Connection_sendProcessCommandEx(con, CS101_COT_ACTIVATION, ca, io);
            InformationObject_destroy(io);
        }
    }

    // 主循环
    const std::uint64_t t0 = iec104::now_utc_ms();
    std::uint64_t last_gi = t0;
    while (iec104::now_utc_ms() - t0 < static_cast<std::uint64_t>(seconds * 1000.0)) {
        if (gi_interval > 0.0 &&
            iec104::now_utc_ms() - last_gi >=
                static_cast<std::uint64_t>(gi_interval * 1000.0)) {
            last_gi = iec104::now_utc_ms();
            std::printf("\n-- 周期总召唤 --\n");
            CS104_Connection_sendInterrogationCommand(con, CS101_COT_ACTIVATION, ca,
                                                      IEC60870_QOI_STATION);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::printf("\n=========================================================\n");
    std::printf(" 汇总\n");
    std::printf("---------------------------------------------------------\n");
    std::printf(" 收到 ASDU      : %d\n", g_st.rx_asdu);
    std::printf(" 总召唤         : 确认 %d / 终止 %d  %s\n", g_st.gi_con, g_st.gi_term,
                (g_st.gi_con > 0 && g_st.gi_con == g_st.gi_term) ? "(成对，正常)"
                                                                : "(不成对，异常 ★)");
    std::printf(" 命令确认       : 肯定 %d / 否定 %d\n", g_st.cmd_pos, g_st.cmd_neg);
    std::printf("---------------------------------------------------------\n");
    if (g_st.rx_asdu == 0)
        std::printf(" [警告] 一帧都没收到 —— 从站没在发，或 STARTDT 没成功\n");
    else if (g_st.gi_term == 0)
        std::printf(" [警告] 总召唤没有 ACT_TERM —— 从站侧没发终止帧（见 docs/README.md §4.1）\n");
    else
        std::printf(" [OK] 通信正常\n");
    std::printf("=========================================================\n");

    CS104_Connection_close(con);
    CS104_Connection_destroy(con);
    return 0;
}
