@echo off
REM =====================================================================
REM P2/ 产品化 P2：编译 + 运行（可观测性 · 单元测试）
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译并运行 P2/ 产品化 P2 可观测性单元测试 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\08\src -I ..\10\src tests\test_observe.cpp -o build\test_observe.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)

build\test_observe.exe
set RC=%errorlevel%

if %RC% == 0 (
    echo.
    echo === ALL TESTS PASSED ===
) else (
    echo.
    echo === TESTS FAILED ===
)

endlocal & exit /b %RC%
