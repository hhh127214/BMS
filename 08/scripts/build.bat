@echo off
REM =====================================================================
REM 08/ 一键编译：周期 8 优化调度与实时控制协同 
REM   头文件搜索路径：src ..\04\src ..\05\src ..\06\src ..\07\src
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译 08/ 单元测试 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\07\src\rtdb tests\test_dispatch_coordinator.cpp -o build\test_dispatch_coordinator.exe
if errorlevel 1 (
    echo [FAIL] test_dispatch_coordinator.exe 编译失败
    exit /b 1
)

echo === 编译 08/ 演示程序 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\07\src\rtdb src\main.cpp -o build\coord_demo.exe
if errorlevel 1 (
    echo [FAIL] coord_demo.exe 编译失败
    exit /b 1
)

echo.
echo === 编译成功 ===
echo   build\test_dispatch_coordinator.exe
echo   build\coord_demo.exe
echo.
echo 运行单元测试： 
echo   build\test_dispatch_coordinator.exe
echo.
echo 运行演示程序： 
echo   build\coord_demo.exe
endlocal
