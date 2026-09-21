// =====================================================================
// 11/ 联调进程 0：RT_DB 初始化器（rtdb_initializer.exe）
//
// 职责：建共享内存段 + 注册全点表（EMS_POINT_COUNT）+ **常驻保活**。
//
// 为什么必须常驻（07/RT_DB 接入踩坑 1 的部署形态）：
//   Windows 下段是页面文件支撑的文件映射对象，最后一个句柄关闭即销毁。
//   ems_rt_db_setup() 内部保留只读存活句柄，但句柄属于**本进程** ——
//   "跑完就退"的短命初始化器会把段一起带走。因此本进程按 --seconds
//   参数保持存活，覆盖整个联调窗。
//
// 用法：rtdb_initializer.exe [--seconds N]   （默认 60 s）
// =====================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "ems_point_table.h"
#include "ems_rt_db_setup.h"
#include "rt_db_api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
static void sleep_s(double s) { Sleep(static_cast<DWORD>(s * 1000.0)); }
#else
#include <unistd.h>
static void sleep_s(double s) { usleep(static_cast<useconds_t>(s * 1e6)); }
#endif

int main(int argc, char** argv) {
    double seconds = 60.0;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--seconds") == 0) seconds = std::atof(argv[i + 1]);
    }

    bool created = false;
    if (!ems_rt_db_setup(/*reset=*/true, &created)) {
        std::fprintf(stderr, "[INITIALIZER] FAIL: RT_DB segment setup failed\n");
        return 1;
    }
    std::printf("[INITIALIZER] segment %s, %d points registered, holding %.0f s...\n",
                created ? "CREATED" : "REUSED", (int)EMS_POINT_COUNT, seconds);
    std::fflush(stdout);

    // 常驻保活：段活到本进程退出（worker 进程各自 rt_db_init 连接）
    const double slice = 0.5;
    for (double t = 0.0; t < seconds; t += slice) sleep_s(slice);

    std::printf("[INITIALIZER] hold window finished, segment released on exit\n");
    return 0;
}
