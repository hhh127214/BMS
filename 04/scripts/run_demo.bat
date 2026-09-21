@echo off
REM =====================================================================
REM 04/ 演示程序：编译 + 跑 
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译 04/ 演示程序 ===
g++ -std=c++17 -Wall -O2 -I src src/main.cpp -o build/strategy_demo.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)

echo === 运行演示 ===
echo 输出重定向到 build\demo_output.txt
build\strategy_demo.exe > build\demo_output.txt 2>&1
type build\demo_output.txt
endlocal