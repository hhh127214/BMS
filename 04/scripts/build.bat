@echo off
REM =====================================================================
REM 04/ 一键编译：策略管理层 + 仲裁器 + 9 策略参考实现 + 演示程序
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译 04/ 单元测试 ===
g++ -std=c++17 -Wall -O2 -I src tests/test_arbiter.cpp -o build/test_arbiter.exe
if errorlevel 1 (
    echo [FAIL] test_arbiter 编译失败
    exit /b 1
)

echo === 编译 04/ 演示程序 ===
g++ -std=c++17 -Wall -O2 -I src src/main.cpp -o build/strategy_demo.exe
if errorlevel 1 (
    echo [FAIL] strategy_demo 编译失败
    exit /b 1
)

echo.
echo === 编译成功 ===
echo   build\test_arbiter.exe
echo   build\strategy_demo.exe
echo.
echo 运行单元测试： 
echo   build\test_arbiter.exe
echo.
echo 运行演示程序： 
echo   build\strategy_demo.exe
endlocal