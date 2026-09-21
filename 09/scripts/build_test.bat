@echo off
REM =====================================================================
REM 09/ 单元测试：编译 + 跑（周期 9 多策略组合测试 · 闭环时序级）
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译并运行 09/ 多策略组合测试 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\08\src -I ..\07\src\rtdb tests\test_multi_strategy.cpp -o build\test_multi_strategy.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)

build\test_multi_strategy.exe
set RC=%errorlevel%

if %RC% == 0 (
    echo.
    echo === ALL TESTS PASSED ===
) else (
    echo.
    echo === TESTS FAILED ===
)

endlocal & exit /b %RC%
