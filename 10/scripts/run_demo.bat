@echo off
REM =====================================================================
REM 10/ 周期 10：运行演示（EMS 24h 离线仿真） 
REM   产物：build\timeseries.csv / alarms.csv / summary.json / report.html
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

echo === 运行典型日仿真 ===
build\sim_demo.exe --out build
echo.
echo === 运行故障注入日仿真（产物在 build\fault\）===
if not exist build\fault mkdir build\fault
build\sim_demo.exe --fault --out build\fault
echo.
echo === 运行异常工况日仿真（产物在 build\stress\）===
rem  这是 SOC / TEMP / TRANSFORMER 三类告警的唯一数据来源：
rem  典型日里它们恒为 0，因为系统是健康的（关口被需量压在 400、变压器留 100 裕度、
rem  SOC 稳在带内）—— 那是"调度有效"的证据，不是缺陷。
if not exist build\stress mkdir build\stress
build\sim_demo.exe --stress --out build\stress

endlocal
