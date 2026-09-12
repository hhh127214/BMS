@echo off
REM =====================================================================
REM 05/ 一键编译：周期 5 安全约束引擎
REM   头文件搜索路径：src ..\04\src ..\06\src
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译 05/ 单元测试 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\06\src tests\test_safety_engine.cpp -o build\test_safety_engine.exe
if errorlevel 1 (
    echo [FAIL] test_safety_engine.exe 编译失败
    exit /b 1
)

echo === 编译 05/ 演示程序 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\06\src src\main.cpp -o build\safety_demo.exe
if errorlevel 1 (
    echo [FAIL] safety_demo.exe 编译失败
    exit /b 1
)

echo.
echo === 编译成功 ===
echo   build\test_safety_engine.exe
echo   build\safety_demo.exe
echo.
echo 运行单元测试： 
echo   build\test_safety_engine.exe
echo.
echo 运行演示程序： 
echo   build\safety_demo.exe
endlocal
