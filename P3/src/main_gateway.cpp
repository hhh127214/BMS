// =====================================================================
// P3 / IEC104 网关进程
//
//   调度主站 ──TCP:2404──► 本进程 ──只读──► RT_DB 共享内存段
//                                  └─只写 EXT.*（可选，A3.1）
//
// 用法：
//   iec104_gateway.exe [--port 2404] [--ca 1] [--bind 0.0.0.0]
//                      [--seconds 60] [--periodic 5]
//                      [--utc-offset 8] [--accept-commands] [--dump-raw]
//                      [--ext-rt-db] [--ext-stale 300]
//                      [--point-map <file.csv>]
//
// ---------------------------------------------------------------------
// 三种受理模式（安全默认值 = 第一种）
// ---------------------------------------------------------------------
//   ① 默认（不加任何开关）：**一律否定确认**。
//      历史原因已消失（EXT 点区已就位），但这个默认仍然保留 ——
//      "网关能不能真的改变 EMS 的运行"应该是**显式打开**的，不是默认状态。
//
//   ② `--accept-commands`：**DRY-RUN**，只记录不落地（向后兼容的旧行为）。
//      联调命令通道用；它**不会**让调度真的控制到 EMS。
//
//   ③ `--ext-rt-db`：**受理并落到 RT_DB 的 EXT 区**（A3.1 生产配置）。
//      ★ 回执与实际落地一致：`RtDbExtWriter` 写成功才回肯定确认，
//        写失败（段未连接 / 分区守卫拒绝）回否定确认并带上原因。
//        "受理与否必须端到端可追溯"是安全相关要求，不能无条件回 ACT_CON。
//
// ---------------------------------------------------------------------
// 为什么默认拒命令
// ---------------------------------------------------------------------
// 如果网关默默接受（回肯定确认）却不落地，调度会以为命令生效了 ——
// 这比拒绝危险得多。所以默认一律回**否定确认**，调度侧明确看到"未受理"。
//
// ---------------------------------------------------------------------
// 落点的边界：网关只能写 EXT.*，写不出 CMD.*
// ---------------------------------------------------------------------
// 调度遥控若直写 `CMD.*`，就绕过了 `05/` 安全引擎，等于远端能直接开 PCS。
// 这条纪律由**类型**表达：`IExtSetpointSink` 只有六个具名 setter，
// 没有任何"按索引写"的入口 —— 见 ext_setpoint_sink.h。
//
//   6001 有功功率设定 → 应映射到**权限区间**（收窄），不是覆盖 p_desired
//                        （语义与取舍见 07/src/rtdb/ext_setpoints.h 末尾）
// =====================================================================

// ★ 顺序有讲究：rtdb_source.h 内部会**先** include rtdb_quality.h，
//   而 rtdb_quality.h 必须在任何 C++ 标准库头之前 —— 否则 rt_db_structs.h
//   的 C11 原子宏会把 libstdc++ 的 std::atomic_* 声明展开成一堆乱码。
//   所以这一行必须排第一，别往下挪。（rtdb_quality.h 里有一道 #error 守着，
//   挪错了会直接报错，不会变成一堆莫名其妙的编译失败。）
#include "rtdb_source.h"

#include "rtdb_ext_sink.h"      // 只写 EXT.* 的适配器（A3.1）

#include "iec104_server.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------------
// 命令出口
//
//   三种模式互斥，按"谁更弱"排序：默认拒 < DRY-RUN < 真落点。
//   两个开关同时给时取**更强**的那个（--ext-rt-db），因为"以为在 dry-run
//   其实真落了"是危险方向上的误解，反过来只是白跑。
// ---------------------------------------------------------------------
class GatewaySink : public iec104::ICommandSink {
public:
    GatewaySink(bool accept_dry_run, iec104::IExtSetpointSink* ext)
        : dry_run_(accept_dry_run), ext_(ext) {}

    bool on_command(int ioa, iec104::Kind kind, double value, bool select,
                    int ca, std::string* err) override {
        if (select) return true;   // 选择步：只占位，不算受理（执行步才裁决）

        char line[256];
        std::snprintf(line, sizeof(line),
                      "IOA=%d %s value=%.3f CA=%d", ioa,
                      iec104::kind_name(kind), value, ca);

        // ---- ① 真落点 ----
        if (ext_ != nullptr) {
            if (!iec104::dispatch_ext_setpoint(*ext_, ioa, value, err)) {
                ++rejected_;
                std::printf("[CMD ] 拒绝（落点失败）：%s\n", line);
                return false;
            }
            records_.push_back(line);
            last_command_ms_ = iec104::now_utc_ms();
            std::printf("[CMD ] 受理(已落 EXT 区)：%s\n", line);
            return true;
        }

        // ---- ② DRY-RUN ----
        if (!dry_run_) {
            if (err) *err = "网关未启用命令受理（--ext-rt-db 落点 / --accept-commands 试运行）";
            ++rejected_;
            std::printf("[CMD ] 拒绝：%s\n", line);
            return false;
        }
        records_.push_back(line);
        last_command_ms_ = iec104::now_utc_ms();
        std::printf("[CMD ] 受理(DRY-RUN，不落地)：%s\n", line);
        return true;
    }

    void on_connection(bool opened, const char* peer) override {
        std::printf("[CONN] %s %s\n", opened ? "主站接入" : "主站断开", peer);
        if (opened) { ++open_conns_; return; }
        if (open_conns_ > 0) --open_conns_;

        // ★ 只在**最后一个**主站断开时清空外部设定。
        //   为什么不是"任何一个断开就清"：max_conn 允许 4 个并发主站，
        //   一个是备用通道，它掉线不该把在用的那个的设定清掉。
        if (open_conns_ == 0 && ext_ != nullptr) {
            std::string e;
            if (ext_->clear_all(&e)) {
                last_command_ms_ = 0;      // 已回到"无外部设定"
                std::printf("[EXT ] 主站全部断开 → 外部设定已清空（回到无约束）\n");
            } else {
                std::printf("[EXT ] 主站断开但清空失败：%s\n", e.c_str());
            }
        }
    }

    const std::vector<std::string>& records() const { return records_; }
    int rejected() const { return rejected_; }
    std::uint64_t last_command_ms() const { return last_command_ms_; }
    void note_cleared() { last_command_ms_ = 0; }

private:
    bool dry_run_;
    iec104::IExtSetpointSink* ext_;
    std::vector<std::string> records_;
    int rejected_ = 0;
    int open_conns_ = 0;
    std::uint64_t last_command_ms_ = 0;
};

void usage() {
    std::printf(
        "iec104_gateway — EMS 的 IEC 60870-5-104 从站（对上接调度 / 虚拟电厂）\n"
        "\n"
        "  --port N         监听端口（默认 2404）\n"
        "  --ca N           公共地址 CA（默认 1，须与调度转发表一致）\n"
        "  --bind IP        绑定网卡（默认 0.0.0.0）\n"
        "  --seconds N      运行时长，0 = 一直跑（默认 60）\n"
        "  --periodic N     周期全量上报间隔秒（默认 5）\n"
        "  --deadband F     变化死区，相对量程比例（默认 0.002）\n"
        "  --utc-offset N   CP56Time2a 时区偏移小时（默认 8 = 北京时间）\n"
        "  --stale-ms N     数据超龄判据（毫秒）。超过则品质置 NT(0x40) 而非 IV。\n"
        "                   0 = 关闭（默认）。现场建议取 3~5 倍设备写周期，\n"
        "                   例：设备每 1 s 写一次 → --stale-ms 5000。\n"
        "                   用途：设备通信断了以后点还在、值还是最后一次的好值，\n"
        "                   没有这条判据的话调度看不出来。\n"
        "  --accept-commands  受理遥控/遥调（**DRY-RUN**，只记录不落地）\n"
        "  --ext-rt-db      受理遥控/遥调并**落到 RT_DB 的 EXT 区**（生产配置）\n"
        "                   写成功才回肯定确认；失败回否定确认并带原因。\n"
        "                   落点只能是 EXT.*（类型上写不出 CMD.*）——\n"
        "                   6001 映射到权限区间（收窄），不是覆盖 p_desired。\n"
        "  --ext-stale N    外部设定空闲超时（秒，默认 300）。超过则清空外部设定。\n"
        "                   0 = 本进程不做，交给 EMS 侧按 EXT.TS 兜底。\n"
        "                   ★ 网关被杀时本判据不会执行，所以**必须**有 EMS 侧兜底；\n"
        "                     这一条只是让状态早点变对。\n"
        "  --dump-raw       打印前 200 个原始报文帧（现场排障）\n"
        "  --point-map F    转发表 CSV（9 列：ioa,kind,group,name,src,scale,offset,unit,note）。\n"
        "                   加载失败回退到内置默认表并报错；格式见 iec104_point_map.h。\n"
        "  --self-check-only  只跑自检就退出（上电首查）\n");
}

} // namespace

int main(int argc, char** argv) {
    iec104::ServerConfig cfg;
    double seconds   = 60.0;
    int    utc_hours = 8;
    long long stale_ms = 0;          // 0 = 关闭数据超龄判据
    bool   accept    = false;
    bool   ext_write = false;        // 落到 RT_DB EXT 区（生产）
    double ext_stale_s = 300.0;      // 外部设定空闲超时（秒）；0 = 本进程不做
    bool   check_only = false;
    const char* point_map = nullptr; // 转发表 CSV（可选，null = 内置默认表）

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_next = (i + 1 < argc);
        if      (a == "--port"      && has_next) cfg.port = std::atoi(argv[++i]);
        else if (a == "--ca"        && has_next) cfg.ca   = std::atoi(argv[++i]);
        else if (a == "--bind"      && has_next) cfg.bind_addr = argv[++i];
        else if (a == "--seconds"   && has_next) seconds  = std::atof(argv[++i]);
        else if (a == "--periodic"  && has_next) cfg.periodic_s = std::atof(argv[++i]);
        else if (a == "--deadband"  && has_next) cfg.change_deadband = std::atof(argv[++i]);
        else if (a == "--utc-offset" && has_next) utc_hours = std::atoi(argv[++i]);
        else if (a == "--stale-ms"  && has_next) stale_ms = std::atoll(argv[++i]);
        else if (a == "--accept-commands") accept = true;
        else if (a == "--ext-rt-db")       ext_write = true;
        else if (a == "--ext-stale" && has_next) ext_stale_s = std::atof(argv[++i]);
        else if (a == "--dump-raw")  cfg.dump_raw = true;
        else if (a == "--point-map" && has_next) point_map = argv[++i];
        else if (a == "--self-check-only") check_only = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::printf("未知参数：%s\n", a.c_str()); usage(); return 2; }
    }
    iec104::set_utc_offset_seconds(utc_hours * 3600);

    // 「请求的模式」与「实际生效的模式」是两个变量。
    // ★ 自检失败会把 ext_write 降级为 false，但启动横幅是在**连段之前**打的 ——
    //   如果报告段直接用 ext_write 判断，就会把"请求了落点"说成"跑了落点"。
    //   所以这里留一份不可变的请求意图，报告段据它决定要不要打统计。
    const bool ext_requested = ext_write;

    std::printf("=========================================================\n");
    std::printf(" EMS IEC 60870-5-104 从站网关（P3）\n");
    std::printf(" 监听            : %s:%d\n", cfg.bind_addr.c_str(), cfg.port);
    std::printf(" 公共地址 CA     : %d\n", cfg.ca);
    std::printf(" 时标时区        : %s  (CP56Time2a 按本地时间上送)\n",
                iec104::utc_offset_text());
    std::printf(" 周期上报        : %.1f s  变化死区 %.3f\n",
                cfg.periodic_s, cfg.change_deadband);
    // 具名变量，不写成三元里现构造的临时对象 —— 那个坑本项目已经踩过一次
    const std::string stale_text = stale_ms > 0
        ? ("超过 " + std::to_string(stale_ms) + " ms → 品质 NT(0x40)")
        : "关闭（不判陈旧）";
    std::printf(" 数据超龄判据    : %s\n", stale_text.c_str());
    const char* mode_text = ext_write ? "落 RT_DB EXT 区（生产，写成功才回肯定确认）"
                          : accept    ? "DRY-RUN（只记录，**不**落到点表）"
                                      : "关（一律否定确认）";
    std::printf(" 命令受理        : %s\n", mode_text);
    if (ext_write) {
        // 具名变量，不写成三元里现构造的临时对象（本项目在这上面踩过一次）
        const std::string ext_stale_text = ext_stale_s > 0.0
            ? (std::to_string(static_cast<int>(ext_stale_s)) + " s → 超时清空" +
               "（★ 网关被杀时本判据不执行，靠 EMS 侧按 EXT.TS 兜底）")
            : std::string("本进程不做（只靠 EMS 侧按 EXT.TS 兜底）");
        std::printf(" 外部设定空闲超时: %s\n", ext_stale_text.c_str());
    }
    std::printf("=========================================================\n");

    // ---------- 0) 转发表 CSV（可选，必须在 resolve()/建从站之前）----------
    // ★ 时序：RtDbReadOnlySource::open_owned() 会 resolve() 上送表、Iec104Server
    //   构造会 capture upload_table() 的指针 —— CSV 加载必须在这两者之前完成，
    //   否则「已加载」的表不会被用上。
    if (point_map != nullptr) {
        std::string pm;
        if (iec104::load_point_map_csv(point_map, &pm)) {
            std::size_t un = 0, dn = 0;
            iec104::upload_table(&un);
            iec104::download_table(&dn);
            std::printf("[OK  ] 转发表已从 CSV 加载：%s（上送 %zu / 下行 %zu）\n",
                        point_map, un, dn);
        } else {
            std::printf("[FAIL] 转发表 CSV 加载失败：\n%s", pm.c_str());
            std::printf("       ★ 已回退到内置默认表。\n");
            if (check_only) return 1;      // 上电首查：表坏必须暴露成非零退出码
        }
    }

    // ---------- 1) 接 RT_DB（只读）----------
    iec104::RtDbReadOnlySource source;
    if (!source.open_owned(nullptr)) {
        std::printf("[FATAL] 连不上 RT_DB 共享内存段。\n");
        std::printf("        先跑初始化器并让它常驻：\n");
        std::printf("          ..\\11\\build\\rtdb_initializer.exe --seconds 600\n");
        std::printf("        （Windows 上段是页面文件映射对象，最后一个句柄关闭即销毁，\n");
        std::printf("          所以初始化器不能做成「跑完就退」的短命进程。）\n");
        return 1;
    }
    std::printf("[OK  ] RT_DB 段已连接\n");
    source.set_stale_limit_ms(stale_ms);

    // ---------- 1b) 外部设定落点（可选）----------
    // ★ 借用**同一个**连接句柄：一个进程一条连接，适配器不自己 open。
    //   `borrowed_handle()` 返回可写句柄这件事**不构成写权限** —— 写不写由
    //   类型决定（`IExtSetpointSink` 只有六个具名 setter）。
    iec104::RtDbExtWriter ext_writer(source.borrowed_handle());
    if (ext_write) {
        // ★ 自检失败必须**拒写**，而不是"警告后继续"。
        //   分区守卫在 write_raw() 里还有一道，这里是让问题在启动横幅上就可见：
        //   若点表被改坏（EXT 区被挪走 / 点名对不上），静默继续会造成
        //   "命令回执是肯定、值落到别的点" —— 本项目最忌讳的一类失败。
        const int ext_bad = ext_writer.self_check();
        if (ext_bad == 0) {
            std::printf("[OK  ] EXT 落点自检通过（6 个设定点全在 EXT 区，点名可按名字解析）\n");
        } else {
            std::printf("[FAIL] EXT 落点自检发现 %d 处问题 —— **拒绝写**，命令一律否定确认。\n",
                        ext_bad);
            std::printf("       两种可能：① 有点不在 EXT 区；② 段里按名字找不到该点（点表与段不匹配）。\n");
            std::printf("       处置：核对该段是不是用当前 ems_point_table.c 初始化的（点名/索引必须同源）。\n");
            ext_write = false;      // 降级为默认拒，而不是带着坏映射继续写
            std::printf("       ★ 本次运行**实际**受理模式 = 默认拒；上方横幅的"
                        "「落 RT_DB EXT 区」已作废。\n");
        }
    }

    // ---------- 2) 自检 ----------
    std::string report;
    const int bad = iec104::gateway_self_check(source, &report);
    std::size_t un = 0, dn = 0;
    iec104::upload_table(&un);
    iec104::download_table(&dn);
    std::printf("[OK  ] 点表映射：上送 %zu 点 / 下行 %zu 点\n", un, dn);
    if (bad > 0) {
        std::printf("[FAIL] 自检发现 %d 处问题：\n%s", bad, report.c_str());
        if (check_only) return 1;
        std::printf("       继续运行，但这些问题点会以 INVALID 品质上送。\n");
    } else {
        std::printf("[OK  ] 自检通过（IOA 唯一 / 区间正确 / 点名全部可解析）\n");
    }
    if (check_only) return bad > 0 ? 1 : 0;

    // ---------- 3) 起从站 ----------
    GatewaySink sink(accept, ext_write ? &ext_writer : nullptr);
    iec104::Iec104Server server(cfg, &source, &sink);
    if (!server.start()) {
        std::printf("[FATAL] 从站启动失败（端口 %d 被占？）\n", cfg.port);
        return 1;
    }
    std::printf("[OK  ] 从站已监听，等待调度主站接入……\n\n");

    // ---------- 4) 主循环 ----------
    const std::uint64_t t0 = iec104::now_utc_ms();
    std::uint64_t last_report = t0;
    std::uint64_t last_clear_try = 0;    // 清空失败时的重试节流（见下）
    while (true) {
        const std::uint64_t now = iec104::now_utc_ms();
        server.tick(now);

        // ---- 外部设定空闲超时 ----
        // 「调度侧忘了 / 链路单向静默」这种情况：命令通道看着正常，
        // 但设定会**永远**生效。判据必须不依赖对端配合，所以放在本进程里。
        // ★ 它**不覆盖**本进程被杀（那时这段循环不跑）→ EMS 侧仍须按 EXT.TS 兜底。
        //   两条判据正交互补，不是二选一。
        if (ext_write && ext_stale_s > 0.0 && sink.last_command_ms() > 0 &&
            now - sink.last_command_ms() >
                static_cast<std::uint64_t>(ext_stale_s * 1000.0) &&
            now - last_clear_try > 5000) {   // 失败时 5 s 重试一次，不要把日志刷爆
            last_clear_try = now;
            std::string e;
            if (ext_writer.clear_all(&e)) {
                sink.note_cleared();
                std::printf("[EXT ] 外部设定空闲超时（> %.0f s）→ 已清空（回到无约束）\n",
                            ext_stale_s);
            } else {
                // ★ 不清 last_command_ms_：清空没成功就必须继续重试，
                //   否则"打印一行失败然后当没事"—— 设定留在原地且再也没人管它。
                std::printf("[EXT ] 空闲超时但清空失败（%s）—— 5 s 后重试\n", e.c_str());
            }
        }

        if (seconds > 0.0 && (now - t0) >= static_cast<std::uint64_t>(seconds * 1000.0))
            break;

        if (now - last_report >= 10000) {   // 每 10 s 打一次心跳
            last_report = now;
            const auto& s = server.stats();
            // EXT 段也放进心跳：现场排查"调度说下发了、EMS 说没收到"时，
            // 第一眼看的就是「写了几次 / 序号走到哪」—— 没有这两个数就只能靠猜。
            std::printf("[T+%3llus] 连接 %llu 开/%llu 断  总召 %llu 完成/%llu 请求  "
                        "周期 %llu 自发 %llu  命令 %llu 收/%llu 拒  "
                        "坏读 %d 陈旧 %d  NT点次 %llu  EXT 写 %d 清 %d SEQ %lld%s\n",
                        static_cast<unsigned long long>(iec104::ms_to_s(now - t0)),
                        static_cast<unsigned long long>(s.connects),
                        static_cast<unsigned long long>(s.disconnects),
                        static_cast<unsigned long long>(s.gi_completed),
                        static_cast<unsigned long long>(s.gi_requests),
                        static_cast<unsigned long long>(s.periodic_asdu),
                        static_cast<unsigned long long>(s.spontaneous_asdu),
                        static_cast<unsigned long long>(s.commands_rx),
                        static_cast<unsigned long long>(s.commands_rejected),
                        source.misses(), source.stale(),
                        static_cast<unsigned long long>(s.points_not_topical),
                        ext_writer.writes(), ext_writer.clears(), ext_writer.seq(),
                        ext_write ? "" : "（落点未启用）");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    server.stop();

    // ---------- 5) 报告 ----------
    const auto& s = server.stats();
    std::printf("\n=========================================================\n");
    std::printf(" 运行结束\n");
    std::printf("---------------------------------------------------------\n");
    std::printf(" 连接       : 接入 %llu 次 / 断开 %llu 次 / STARTDT %llu\n",
                (unsigned long long)s.connects, (unsigned long long)s.disconnects,
                (unsigned long long)s.startdt);
    std::printf(" 总召唤     : 请求 %llu / 完成 %llu（未完成 = 主站中途断开）\n",
                (unsigned long long)s.gi_requests, (unsigned long long)s.gi_completed);
    std::printf(" 上送       : 周期 %llu 帧 / 自发 %llu 帧 / 点次 %llu\n",
                (unsigned long long)s.periodic_asdu,
                (unsigned long long)s.spontaneous_asdu,
                (unsigned long long)s.points_uploaded);
    std::printf(" 坏点       : %llu 点次（品质位置 INVALID）\n",
                (unsigned long long)s.points_invalid);
    std::printf(" 对钟       : %llu 次", (unsigned long long)s.clock_syncs);
    if (s.clock_syncs > 0)
        std::printf("，最近一次 %s（%s）", server.last_clock_sync_text().c_str(),
                    iec104::utc_offset_text());
    std::printf("\n");
    std::printf(" 命令       : 收到 %llu / 受理 %llu / 拒绝 %llu\n",
                (unsigned long long)s.commands_rx,
                (unsigned long long)s.commands_accepted,
                (unsigned long long)s.commands_rejected);
    std::printf("              （未识别 IOA %llu / 未知 CA %llu / 未知 COT %llu / 类型不符 %llu）\n",
                (unsigned long long)s.unknown_ioa, (unsigned long long)s.unknown_ca,
                (unsigned long long)s.unknown_cot, (unsigned long long)s.illegal_type);
    if (!server.last_command_text().empty())
        std::printf(" 最近命令   : %s\n", server.last_command_text().c_str());
    std::printf("---------------------------------------------------------\n");
    if (s.connects == 0) {
        std::printf(" [警告] 全程没有任何主站接入 —— 检查调度侧 IP / 端口 / 防火墙\n");
    } else if (s.gi_requests == 0) {
        std::printf(" [警告] 有连接但没有总召唤 —— 调度侧没有走标准上线流程\n");
    } else if (s.gi_completed < s.gi_requests) {
        std::printf(" [警告] 总召唤未全部完成 —— 排查主站侧 t1 超时 / 网络丢包\n");
    } else {
        std::printf(" [OK] 总召唤全部完成，上送通道正常\n");
    }

    // ---- 外部设定落点统计 ----
    // 只在**请求过**落点时打（`ext_requested`，不是 ext_write —— 被自检降级后
    // ext_write 已是 false，用它判断会把"跑了但降级"这件事从报告里抹掉）。
    if (ext_requested) {
        std::printf("---------------------------------------------------------\n");
        std::printf(" 外部设定   : 写入 %d 次 / 清空 %d 次 / 拒绝 %d 次   末值 SEQ=%lld\n",
                    ext_writer.writes(), ext_writer.clears(), ext_writer.rejects(),
                    ext_writer.seq());
        if (ext_writer.writes() == 0)
            std::printf("              （全程无外部设定写入：要么主站没下发，要么落点自检降级后一律拒）\n");
        if (ext_writer.seq() == 0)
            std::printf("              （SEQ 仍为 0 = 从未成功发布；EMS 侧据此判 present=false）\n");
    }
    std::printf("=========================================================\n");
    return 0;
}
