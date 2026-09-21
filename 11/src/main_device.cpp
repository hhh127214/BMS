// =====================================================================
// 11/ 联调进程 1：设备侧进程（device_side.exe）
//
// 六类数据源（BMS / PCS / 电表 / 光伏 / 变压器 / 负荷）→ 共享内存段。
// 只写 MEAS/STA/CFG，只读 CMD —— 与 EMS 进程（ems_side.exe）互为相反方向。
//
// 故障剧本：--fault kind:begin:end 可重复（秒，模拟时间）。
//   kind: 1=BMS通信断 2=电表通信断 3=PCS通信断 4=PCS故障 5=设备离线 6=品质位劣化
//
// 节拍：--speed K 表示把模拟时间压缩 K 倍（sleep = dt/K），默认 1.0 = 实时。
//   **必须节拍**：全速跑会在几毫秒内跑完整个窗口，段里的量测随即冻住，
//   EMS 侧看到的"闭环"就成了开环（p_actual 恒定，跟踪误差恒 0）。
//
// 用法：device_side.exe [--steps N] [--dt 0.1] [--speed K]
//                       [--fault kind:t1:t2] ... [--report path]
// =====================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "integration_runner.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    int    steps  = 3000;
    double dt     = 0.1;
    double speed  = 1.0;
    std::string report_path = "build/integration_device_report.txt";
    std::vector<ems::DevFaultWindow> script;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--steps" && i + 1 < argc)     steps = std::atoi(argv[++i]);
        else if (a == "--dt" && i + 1 < argc)   dt = std::atof(argv[++i]);
        else if (a == "--speed" && i + 1 < argc) speed = std::atof(argv[++i]);
        else if (a == "--report" && i + 1 < argc) report_path = argv[++i];
        else if (a == "--fault" && i + 1 < argc) {
            ems::DevFaultWindow fw;
            std::sscanf(argv[++i], "%d:%lf:%lf", &fw.kind, &fw.t_begin_s, &fw.t_end_s);
            fw.note = ems::dev_fault_kind_name(fw.kind);
            script.push_back(fw);
        }
    }

    // 连接共享内存段（初始化器必须先起）
    rt_db_handle_t h{};
    bool connected = false;
    for (int retry = 0; retry < 40 && !connected; ++retry) {
        connected = rt_db_init(&h, nullptr);
        if (!connected) {
            std::printf("[DEVICE] waiting for RT_DB segment (retry %d)...\n", retry);
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    if (!connected) {
        std::fprintf(stderr, "[DEVICE] FAIL: cannot connect RT_DB segment "
                             "(run rtdb_initializer.exe first)\n");
        return 1;
    }

    ems::DeviceSideSim device(&h, script);

    std::printf("[DEVICE] connected, six data sources online "
                "(BMS/PCS/METER/PV/TRANSFORMER/LOAD), running %d steps @ %.3f s "
                "(speed %.2fx, wall %.1f s)\n",
                steps, dt, speed, steps * dt / (speed > 0.0 ? speed : 1.0));
    if (!script.empty()) {
        std::printf("[DEVICE] fault script:\n");
        for (const auto& fw : script) {
            std::printf("         %s  [%.1f, %.1f) s\n",
                        ems::dev_fault_kind_name(fw.kind), fw.t_begin_s, fw.t_end_s);
        }
    }
    std::fflush(stdout);

    ems::Pacing pacer(dt, speed);
    for (int i = 0; i < steps; ++i) {
        device.step(i * dt, dt);
        pacer.wait();          // 与 EMS 进程同时活着，才叫闭环
    }

    const auto& st = device.stats();
    std::printf("[DEVICE] done: steps=%d cmd_peak=%.1f kW out_of_interval=%d "
                "failsafe=%d fault_ticks=%d\n",
                st.steps, st.max_abs_cmd_kw, st.cmd_out_of_interval,
                st.failsafe_ticks, st.comm_fault_ticks);

    // 报告落盘（联调产物）
    {
        std::ofstream f(report_path.c_str());
        if (f.is_open()) {
            f << device.report_text();
            for (const auto& fw : script) {
                f << "  剧本窗 " << ems::dev_fault_kind_name(fw.kind)
                  << "  [" << fw.t_begin_s << ", " << fw.t_end_s << ") s\n";
            }
        }
    }

    rt_db_cleanup(&h);
    return (st.cmd_out_of_interval == 0) ? 0 : 2;   // 硬契约违例 → 非零退出码
}
