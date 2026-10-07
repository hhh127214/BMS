@echo off
REM =====================================================================
REM  18\ build: 性能基准与压测 —— 编译产物
REM
REM  产物（全在 build\ 下，不入库）： 
REM    build\test_perf_stats.exe   计时/统计基础设施单元测试 T01~T11
REM    build\test_perf_bench.exe   B1~B6 基准 + 23 条 SLA 门禁 + 反向验证
REM    build\rt_db_api.o           07\ RT_DB C 库（gcc 单独编）
REM    build\ems_point_table.o     07\ 点表真相源 
REM    build\ems_rt_db_setup.o     07\ RT_DB 装配
REM
REM  为什么必须自己编 07\ 的三个 .c：18\ 的 B2（采集路径）与 B6（长跑）要真的 
REM  走 RT_DB 共享内存；RT_DB 是 C 实现，符号是 C 链接，不能只在 C++ 侧链头文件。 
REM  这三个 .c 属于 07\，18\ **只读不改** —— 编译产物落在 18\build\，07\src 保持原样。 
REM
REM  依赖： 
REM    04\ device_io.h（IDeviceIO 快照契约） 
REM    05\ 06\ 08\ 09\ 10\ 11\ P1\ —— 07\realtime_loop.h 的间接依赖（调度/策略） 
REM    07\ realtime_loop.h + src\rtdb + vendor\rt_db（EmsRuntime / RT_DB） 
REM    13\ modbus_tcp_client.h（B3 编解码，**只 -I 引用，不改 13\**） 
REM    17\ publish_pipeline.h（B5 全量重发 vs 优化后，**只 -I 引用，不改 17\**） 
REM    20\ alarm_assembler.h / alarm_model.h（B4 告警装配） 
REM    P2\ soe.h（B4 SOE 日志） 
REM  WinSock2（-lws2_32，13\ 的 Modbus 客户端要）+ PSAPI（-lpsapi，工作集采样） 
REM
REM  注意：本脚本**不是**给 scripts\build_all.bat 调用的入口 —— 18\ 挂进构建树 
REM  由集成方统一执行（见 docs\规划\新模块开发约定.md §6）。 
REM =====================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0\.."

if not exist build mkdir build

REM ---------- [1/4] 07\ RT_DB C 库 ----------
REM  为什么无条件重编、不用 `if not exist`：点表/RT_DB 头文件一改，旧 .o 就是
REM  "同名不同内容"，编译期不报错、运行期按名字取点错位 —— 静默失败。 
REM  -D__USE_MINGW_ANSI_STDIO=1 是 rt_db_api.c 里 %lld 走 MinGW 自有 printf 的需要。 
echo === [1/4] 07\ RT_DB C 库（rt_db_api / ems_point_table / ems_rt_db_setup）===
gcc -std=c11 -O2 -D__USE_MINGW_ANSI_STDIO=1 -I ..\07\vendor\rt_db ^
    -c ..\07\vendor\rt_db\rt_db_api.c -o build\rt_db_api.o
if errorlevel 1 ( echo [FAIL] rt_db_api.c compile error & exit /b 1 )

gcc -std=c11 -O2 -I ..\07\src\rtdb -I ..\07\vendor\rt_db ^
    -c ..\07\src\rtdb\ems_point_table.c -o build\ems_point_table.o
if errorlevel 1 ( echo [FAIL] ems_point_table.c compile error & exit /b 1 )

gcc -std=c11 -O2 -I ..\07\src\rtdb -I ..\07\vendor\rt_db ^
    -c ..\07\src\rtdb\ems_rt_db_setup.c -o build\ems_rt_db_setup.o
if errorlevel 1 ( echo [FAIL] ems_rt_db_setup.c compile error & exit /b 1 )

REM ---------- [2/4] include / link 参数 ----------
set INC=-I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src ^
 -I ..\08\src -I ..\09\src -I ..\10\src -I ..\11\src -I ..\P1\src -I ..\P2\src ^
 -I ..\13\src -I ..\17\src -I ..\20\src -I ..\07\src\rtdb -I ..\07\vendor\rt_db
set RTDBOBJS=build\rt_db_api.o build\ems_point_table.o build\ems_rt_db_setup.o

REM ---------- [3/4] 计时/统计基础设施单元测试 ----------
REM  只依赖 18\src 自己的头 + PSAPI（perf_clock.h 采进程工作集），
REM  不需要 RT_DB —— 单测要能在没有共享内存的机器上先绿，才能隔离"统计学错了"
REM  与"环境没起来"这两类失败。 
echo.
echo === [3/4] test_perf_stats.exe（T01~T11 计时与统计）===
g++ -std=c++17 -Wall -O2 -I src ^
    tests\test_perf_stats.cpp -lpsapi -o build\test_perf_stats.exe
if errorlevel 1 ( echo [FAIL] test_perf_stats compile error & exit /b 1 )

REM ---------- [4/4] 基准 + SLA 门禁 ----------
echo.
echo === [4/4] test_perf_bench.exe（B1~B6 + SLA 门禁 + 反向验证）===
g++ -std=c++17 -Wall -O2 %INC% ^
    tests\test_perf_bench.cpp %RTDBOBJS% ^
    -lws2_32 -lpsapi -o build\test_perf_bench.exe
if errorlevel 1 ( echo [FAIL] test_perf_bench compile error & exit /b 1 )

echo.
echo === BUILD OK ===
echo   build\test_perf_stats.exe   计时/统计单测
echo   build\test_perf_bench.exe   基准 + SLA 门禁（跑一次会落 build\BENCH-REPORT.md） 
echo   运行 scripts\build_test.bat 编译并运行全部测试 

endlocal & exit /b 0
