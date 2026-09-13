@echo off
REM =====================================================================
REM P1/ 产品化 P1：编译 + 运行（配置化 · 单元测试）
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译并运行 P1/ 产品化 P1 配置化单元测试 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\08\src tests\test_config.cpp -o build\test_config.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)

build\test_config.exe
set RC=%errorlevel%

if %RC% == 0 (
    echo.
    echo === ALL TESTS PASSED ===
) else (
    echo.
    echo === TESTS FAILED ===
)

endlocal & exit /b %RC%
