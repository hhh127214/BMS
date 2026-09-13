@echo off
REM =====================================================================
REM 10/ 周期 10：编译 + 运行（EMS 24h 离线仿真 · 单元测试）
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译并运行 10/ 周期 10 EMS 24h 离线仿真单元测试 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\08\src tests\test_sim_24h.cpp -o build\test_sim_24h.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)

build\test_sim_24h.exe
set RC=%errorlevel%

if %RC% == 0 (
    echo.
    echo === ALL TESTS PASSED ===
) else (
    echo.
    echo === TESTS FAILED ===
)

endlocal & exit /b %RC%
