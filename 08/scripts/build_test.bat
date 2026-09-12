@echo off
REM =====================================================================
REM 08/ 单元测试：编译 + 跑（周期 8 优化调度与实时控制协同）
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译并运行 08/ 单元测试 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src tests\test_dispatch_coordinator.cpp -o build\test_dispatch_coordinator.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)

build\test_dispatch_coordinator.exe
set RC=%errorlevel%

if %RC% == 0 (
    echo.
    echo === ALL TESTS PASSED ===
) else (
    echo.
    echo === TESTS FAILED ===
)

endlocal & exit /b %RC%
