@echo off
REM =====================================================================
REM 05/ 单元测试：编译 + 跑（周期 5 安全约束引擎） 
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译并运行 05/ 单元测试 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\06\src tests\test_safety_engine.cpp -o build\test_safety_engine.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)

build\test_safety_engine.exe
set RC=%errorlevel%

if %RC% == 0 (
    echo.
    echo === ALL TESTS PASSED ===
) else (
    echo.
    echo === TESTS FAILED ===
)

endlocal & exit /b %RC%
