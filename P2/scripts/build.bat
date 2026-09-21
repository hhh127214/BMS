@echo off
REM =====================================================================
REM P2/ 产品化 P2：编译（可观测性 · 演示程序） 
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译 P2/ 产品化 P2 可观测性演示 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\08\src -I ..\10\src -I ..\07\src\rtdb src\main.cpp -o build\ems_observe.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)
echo [OK] build\ems_observe.exe
endlocal
