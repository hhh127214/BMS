@echo off
REM =====================================================================
REM  12/ 单元测试: 编译并运行 T51~T59
REM
REM    T51 A1 功能维度        T52 A2 策略维度       T53 A3 安全维度
REM    T54 A4 性能维度        T55 A5 稳定性维度     T56 A6 协同维度
REM    T57 A7 文档维度        T58 报告渲染三件套    T59 汇总一致性 
REM
REM  测试内部只跑一次验收（七维度共享同一份报告），随后逐维度断言。 
REM  依赖 07/ 的 RT_DB C 库 + -lpsapi。 
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === compile RT_DB C library (gcc) ===
gcc -std=c11 -O2 -D__USE_MINGW_ANSI_STDIO=1 -I ..\07\vendor\rt_db -c ..\07\vendor\rt_db\rt_db_api.c -o build\rt_db_api.o
if errorlevel 1 (
    echo [FAIL] rt_db_api.c compile error
    exit /b 1
)
gcc -std=c11 -O2 -I ..\07\src\rtdb -I ..\07\vendor\rt_db -c ..\07\src\rtdb\ems_point_table.c -o build\ems_point_table.o
if errorlevel 1 (
    echo [FAIL] ems_point_table.c compile error
    exit /b 1
)
gcc -std=c11 -O2 -I ..\07\src\rtdb -I ..\07\vendor\rt_db -c ..\07\src\rtdb\ems_rt_db_setup.c -o build\ems_rt_db_setup.o
if errorlevel 1 (
    echo [FAIL] ems_rt_db_setup.c compile error
    exit /b 1
)

echo === compile + run 12/ acceptance tests ===
g++ -std=c++17 -Wall -O2 ^
    -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\08\src ^
    -I ..\09\src -I ..\10\src -I ..\11\src -I ..\P1\src -I ..\P2\src ^
    -I ..\07\src\rtdb -I ..\07\vendor\rt_db ^
    tests\test_acceptance.cpp build\rt_db_api.o build\ems_point_table.o build\ems_rt_db_setup.o ^
    -lpsapi -o build\test_acceptance.exe
if errorlevel 1 (
    echo [FAIL] compile error
    exit /b 1
)

build\test_acceptance.exe
set RC=%errorlevel%

if %RC% == 0 (
    echo.
    echo === ALL TESTS PASSED ===
) else (
    echo.
    echo === TESTS FAILED ===
)

endlocal & exit /b %RC%
