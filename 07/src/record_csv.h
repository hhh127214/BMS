// =====================================================================
// 07/ —— 现场实录 CSV 写出器（--record）
//
//   为什么单独成头文件：列格式是「07/ 现场进程」与「14/ 平台 live 导入」
//   之间唯一的接口契约，必须与 10/ 的 write_timeseries_csv() **逐列同构**，
//   否则 14/ 的 CSV_MAP 会对不上列。单独成头文件 + 单测锁定列顺序，
//   比散在 main_field.cpp 里更能防止"某天手滑少写一列"。
//
//   ★ 与 10/ 的唯一差别是**时刻口径**（这正是 #106 的主题）：
//     10/ 仿真 : t_s = 一天内的秒（0..86400），time = "HH:MM"
//     07/ 现场 : t_s = Unix 墙钟秒，          time = "YYYY-MM-DD HH:MM:SS"
//     所以列序、列名、数值精度三者必须与 10/ 完全一致，只有时间两列的口径不同。
//
//   20 列（与 10/src/sim_report.h write_timeseries_csv 逐字对齐）：
//     t_s, time, state, P_load_kW, P_pv_kW, P_grid_kW, P_cmd_kW, P_actual_kW,
//     SOC, T_C, P_lower, P_upper, plan_target, correction, clamped,
//     safety_clip, state_gated, hold_last, fault_bits, reason
// =====================================================================

#ifndef EMS_07_RECORD_CSV_H
#define EMS_07_RECORD_CSV_H

#include "realtime_loop.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace ems {
namespace record {

// 表头：与 10/ write_timeseries_csv() 逐字一致。
inline const char* csv_header() {
    return "t_s,time,state,P_load_kW,P_pv_kW,P_grid_kW,P_cmd_kW,P_actual_kW,"
           "SOC,T_C,P_lower,P_upper,plan_target,correction,clamped,safety_clip,"
           "state_gated,hold_last,fault_bits,reason";
}

// Unix 墙钟秒 -> "YYYY-MM-DD HH:MM:SS"（本地时区）。
// 用 std::gmtime 会偏 8 小时，这里必须用 localtime 才能让界面上显示的
// 时间与现场墙上的钟一致。
inline std::string wall_time_str(double unix_s) {
    const std::time_t t = static_cast<std::time_t>(unix_s);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return buf;
}

// 把一行 StepRecord 序列化成 CSV 的一行（不含换行）。
// reason 可能含逗号/引号，按 CSV 规范加引号。
inline std::string csv_row(const StepRecord& r, double unix_s) {
    char num[64];
    std::string line;
    line.reserve(160);

    std::snprintf(num, sizeof(num), "%.3f", unix_s);
    line += num; line += ",";
    line += wall_time_str(unix_s); line += ",";
    line += state_name(r.state); line += ",";

    std::snprintf(num, sizeof(num), "%.3f", r.p_load);  line += num; line += ",";
    std::snprintf(num, sizeof(num), "%.3f", r.p_pv);    line += num; line += ",";
    std::snprintf(num, sizeof(num), "%.3f", r.p_grid);  line += num; line += ",";
    std::snprintf(num, sizeof(num), "%.3f", r.p_cmd);   line += num; line += ",";
    std::snprintf(num, sizeof(num), "%.3f", r.p_actual); line += num; line += ",";
    std::snprintf(num, sizeof(num), "%.3f", r.soc);     line += num; line += ",";
    std::snprintf(num, sizeof(num), "%.3f", r.temp);    line += num; line += ",";
    std::snprintf(num, sizeof(num), "%.3f", r.p_lower); line += num; line += ",";
    std::snprintf(num, sizeof(num), "%.3f", r.p_upper); line += num; line += ",";
    std::snprintf(num, sizeof(num), "%.3f", r.plan_target); line += num; line += ",";
    std::snprintf(num, sizeof(num), "%.3f", r.correction);  line += num; line += ",";
    line += (r.clamped ? "1" : "0"); line += ",";
    line += (r.safety_clip ? "1" : "0"); line += ",";
    line += (r.state_gated ? "1" : "0"); line += ",";
    line += (r.hold_last ? "1" : "0"); line += ",";
    std::snprintf(num, sizeof(num), "%d", r.fault_bits); line += num; line += ",";

    line += "\"";
    for (char c : r.reason) {
        if (c == '"') line += "\"\"";
        else line += c;
    }
    line += "\"";
    return line;
}

// 增量写出器：--record 是边跑边追加，不能每次全量重写。
//   · 第一次调用写表头，之后只追加数据行。
//   · 用 std::fflush 每行落盘 —— 现场进程可能被强杀，flush 后不丢已写数据。
class RecordWriter {
public:
    RecordWriter() = default;
    explicit RecordWriter(const std::string& path) : path_(path) {}

    bool open() { return open(path_); }

    bool open(const std::string& path) {
        path_ = path;
        f_ = std::fopen(path_.c_str(), "w");
        if (!f_) return false;
        std::fprintf(f_, "%s\n", csv_header());
        std::fflush(f_);
        return true;
    }

    bool write(const StepRecord& r, double unix_s) {
        if (!f_) return false;
        std::fprintf(f_, "%s\n", csv_row(r, unix_s).c_str());
        std::fflush(f_);
        return true;
    }

    void close() {
        if (f_) { std::fclose(f_); f_ = nullptr; }
    }

    ~RecordWriter() { close(); }

private:
    std::string path_;
    std::FILE*  f_ = nullptr;
};

}  // namespace record
}  // namespace ems

#endif  // EMS_07_RECORD_CSV_H
