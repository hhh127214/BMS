@echo off
REM =====================================================================
REM  12/ build: 周期 12 最终验收 —— 编译验收运行器 CLI
REM
REM    build\acceptance.exe   七维度验收 
REM                            A1 功能 / A2 策略 / A3 安全 / A4 性能
REM                            A5 稳定性 / A6 多策略协同 / A7 文档
REM
REM  依赖 07/ 的 RT_DB C 库（3 个 .c），用 gcc 单独编；
REM  C++ 侧另需 -lpsapi（A4 的进程工作集采样）。 
REM
REM  本脚本只编译。运行正式验收请用 scripts\run_acceptance.bat。 
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === [1/4] compile RT_DB C library (gcc) ===
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

set INC=-I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\08\src -I ..\09\src -I ..\10\src -I ..\11\src -I ..\P1\src -I ..\P2\src -I ..\07\src\rtdb -I ..\07\vendor\rt_db
set RTDBOBJS=build\rt_db_api.o build\ems_point_table.o build\ems_rt_db_setup.o

echo === [2/4] acceptance.exe ===
g++ -std=c++17 -Wall -O2 %INC% ^
    src\main_acceptance.cpp %RTDBOBJS% -lpsapi -o build\acceptance.exe
if errorlevel 1 (
    echo [FAIL] acceptance.exe compile error
    exit /b 1
)

echo === [3/4] 链接期自检（--help）===
build\acceptance.exe --help >nul
if errorlevel 1 (
    echo [FAIL] acceptance.exe cannot start
    exit /b 1
)

echo === [4/4] BUILD OK ===
echo   build\acceptance.exe   七维度验收器
echo   运行 scripts\run_acceptance.bat 生成正式验收报告

endlocal & exit /b 0
