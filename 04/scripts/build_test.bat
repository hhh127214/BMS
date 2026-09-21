@echo off
REM =====================================================================
REM 04/ 单元测试：编译 + 跑 
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译并运行 04/ 单元测试 ===
g++ -std=c++17 -Wall -O2 -I src tests/test_arbiter.cpp -o build/test_arbiter.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)

build\test_arbiter.exe
set RC=%errorlevel%

if %RC% == 0 (
    echo.
    echo === ALL TESTS PASSED ===
) else (
    echo.
    echo === TESTS FAILED ===
)

endlocal & exit /b %RC%