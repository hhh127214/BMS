@echo off
REM =====================================================================
REM 06/ 单元测试：编译 + 跑（周期 6 EMS 状态机） 
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译并运行 06/ 单元测试 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src tests\test_state_machine.cpp -o build\test_state_machine.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)

build\test_state_machine.exe
set RC=%errorlevel%

if %RC% == 0 (
    echo.
    echo === ALL TESTS PASSED ===
) else (
    echo.
    echo === TESTS FAILED ===
)

endlocal & exit /b %RC%
