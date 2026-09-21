@echo off
REM =====================================================================
REM 06/ 一键编译：周期 6 EMS 状态机
REM   头文件搜索路径：src ..\04\src ..\05\src
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译 06/ 单元测试 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\07\src\rtdb tests\test_state_machine.cpp -o build\test_state_machine.exe
if errorlevel 1 (
    echo [FAIL] test_state_machine.exe 编译失败
    exit /b 1
)

echo === 编译 06/ 演示程序 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\07\src\rtdb src\main.cpp -o build\fsm_demo.exe
if errorlevel 1 (
    echo [FAIL] fsm_demo.exe 编译失败
    exit /b 1
)

echo.
echo === 编译成功 ===
echo   build\test_state_machine.exe
echo   build\fsm_demo.exe
echo.
echo 运行单元测试： 
echo   build\test_state_machine.exe
echo.
echo 运行演示程序： 
echo   build\fsm_demo.exe
endlocal
