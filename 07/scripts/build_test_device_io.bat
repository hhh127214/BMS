@echo off
REM =====================================================================
REM 07/ 单元测试：编译 + 跑（产品化 P0/P0.5 设备 I/O 抽象与适配器可换性）
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译并运行 07/ P0+P0.5 设备 I/O 抽象测试 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\08\src -I src\rtdb tests\test_device_io.cpp -o build\test_device_io.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)

build\test_device_io.exe
set RC=%errorlevel%

if %RC% == 0 (
    echo.
    echo === ALL TESTS PASSED ===
) else (
    echo.
    echo === TESTS FAILED ===
)

endlocal & exit /b %RC%
