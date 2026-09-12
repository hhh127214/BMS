@echo off
REM =====================================================================
REM 05/ 演示程序：编译 + 跑（周期 5 安全约束引擎） 
REM   9 类安全约束逐条触发对照表 
REM   变化率后置限速 / 区间矛盾判定
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译 05/ 演示程序 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\06\src src\main.cpp -o build\safety_demo.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)

echo === 运行演示 ===
echo 输出重定向到 build\demo_output.txt
build\safety_demo.exe > build\demo_output.txt 2>&1
type build\demo_output.txt
endlocal
