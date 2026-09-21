@echo off
REM =====================================================================
REM 07/ 单元测试：编译 + 跑（周期 7 实时控制闭环） 
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译并运行 07/ 单元测试 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\08\src -I src\rtdb tests\test_realtime_loop.cpp -o build\test_realtime_loop.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)

build\test_realtime_loop.exe
set RC=%errorlevel%

if %RC% == 0 (
    echo.
    echo === ALL TESTS PASSED ===
) else (
    echo.
    echo === TESTS FAILED ===
)

endlocal & exit /b %RC%
