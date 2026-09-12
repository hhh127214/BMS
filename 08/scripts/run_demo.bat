@echo off
REM =====================================================================
REM 08/ 演示程序：编译 + 跑（周期 8 优化调度与实时控制协同）
REM   24h 分层管控：优化层规划 / 实时层纠偏 / 安全层兜底 
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译 08/ 演示程序 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src src\main.cpp -o build\coord_demo.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)

echo === 运行演示 ===
echo 输出重定向到 build\demo_output.txt
build\coord_demo.exe > build\demo_output.txt 2>&1
type build\demo_output.txt
endlocal
