@echo off
REM =====================================================================
REM 10/ 周期 10：编译（EMS 24h 离线仿真 · 演示程序） 
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译 10/ 周期 10 EMS 24h 离线仿真演示 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\08\src -I ..\07\src\rtdb src\main.cpp -o build\sim_demo.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)
echo [OK] build\sim_demo.exe
endlocal
