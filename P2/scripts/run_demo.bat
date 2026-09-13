@echo off
REM =====================================================================
REM P2/ 产品化 P2：运行演示（可观测性）
REM   产物：build\ems_observe.exe + out\soe.* / out\metrics.*
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build
if not exist out   mkdir out

echo === 编译 P2/ 产品化 P2 可观测性演示 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\08\src -I ..\10\src src\main.cpp -o build\ems_observe.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)

echo.
echo === 1/4 24h 闭环 + 观察者汇总 ===
build\ems_observe.exe --demo 6

echo.
echo === 2/4 指标与 log_every 无关（核心价值） ===
build\ems_observe.exe --decouple

echo.
echo === 3/4 故障场景 SOE ===
build\ems_observe.exe --fault 12

echo.
echo === 4/4 导出 SOE / 指标 ===
build\ems_observe.exe --export out 6

endlocal
