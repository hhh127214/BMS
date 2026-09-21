@echo off
REM =====================================================================
REM 07/ 演示程序：编译 + 跑（周期 7 实时控制闭环） 
REM   100ms 闭环全链路 + 无延迟量化 
REM   并网点抖动治理三档对照 
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译 07/ 演示程序 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\08\src -I src\rtdb src\main.cpp -o build\loop_demo.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)

echo === 运行演示 ===
echo 输出重定向到 build\demo_output.txt
build\loop_demo.exe > build\demo_output.txt 2>&1
type build\demo_output.txt
endlocal
