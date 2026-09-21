@echo off
REM =====================================================================
REM 07/ RT_DB 接入测试：编译 C 实时库 + 链接 C++ 适配器 + 跑 
REM   T25~T29（点表契约 / 跨内存边界闭环等价 / 双连接 / 故障驱动状态机 / 关口功率单一数据源） 
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译 RT_DB C 实现（gcc） ===
gcc -std=c11 -O2 -D__USE_MINGW_ANSI_STDIO=1 -I vendor\rt_db -c vendor\rt_db\rt_db_api.c -o build\rt_db_api.o
if errorlevel 1 (
    echo [FAIL] rt_db_api.c 编译失败
    exit /b 1
)

echo === 编译 EMS 点表与初始化器（gcc） ===
gcc -std=c11 -O2 -I src\rtdb -I vendor\rt_db -c src\rtdb\ems_point_table.c -o build\ems_point_table.o
if errorlevel 1 (
    echo [FAIL] ems_point_table.c 编译失败
    exit /b 1
)
gcc -std=c11 -O2 -I src\rtdb -I vendor\rt_db -c src\rtdb\ems_rt_db_setup.c -o build\ems_rt_db_setup.o
if errorlevel 1 (
    echo [FAIL] ems_rt_db_setup.c 编译失败
    exit /b 1
)

echo === 编译并运行 07/ RT_DB 接入测试 ===
g++ -std=c++17 -Wall -O2 -I src -I src\rtdb -I vendor\rt_db -I ..\04\src -I ..\05\src -I ..\06\src -I ..\08\src ^
    tests\test_rtdb_device_io.cpp build\rt_db_api.o build\ems_point_table.o build\ems_rt_db_setup.o ^
    -o build\test_rtdb_device_io.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)

build\test_rtdb_device_io.exe
set RC=%errorlevel%

if %RC% == 0 (
    echo.
    echo === ALL TESTS PASSED ===
) else (
    echo.
    echo === TESTS FAILED ===
)

endlocal & exit /b %RC%
