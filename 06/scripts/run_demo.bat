@echo off
REM =====================================================================
REM 06/ 演示程序：编译 + 跑（周期 6 EMS 状态机） 
REM   全状态流转 INIT→…→EMERGENCY→复位 
REM   输出门控真值表 / SOE 迁移记录
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译 06/ 演示程序 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\07\src\rtdb src\main.cpp -o build\fsm_demo.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)

echo === 运行演示 ===
echo 输出重定向到 build\demo_output.txt
build\fsm_demo.exe > build\demo_output.txt 2>&1
type build\demo_output.txt
endlocal
