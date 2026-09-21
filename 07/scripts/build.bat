@echo off
REM =====================================================================
REM 07/ 一键编译：周期 7 实时控制闭环
REM   头文件搜索路径：src ..\04\src ..\05\src ..\06\src ..\08\src
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译 07/ 单元测试 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\08\src -I src\rtdb tests\test_realtime_loop.cpp -o build\test_realtime_loop.exe
if errorlevel 1 (
    echo [FAIL] test_realtime_loop.exe 编译失败
    exit /b 1
)

echo === 编译 07/ 演示程序 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\08\src -I src\rtdb src\main.cpp -o build\loop_demo.exe
if errorlevel 1 (
    echo [FAIL] loop_demo.exe 编译失败
    exit /b 1
)

echo.
echo === 编译成功 ===
echo   build\test_realtime_loop.exe
echo   build\loop_demo.exe
echo.
echo 运行单元测试： 
echo   build\test_realtime_loop.exe
echo.
echo 运行演示程序： 
echo   build\loop_demo.exe
endlocal
