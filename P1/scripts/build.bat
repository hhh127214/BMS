@echo off
REM =====================================================================
REM P1/ 产品化 P1：编译（配置化 · 演示程序）
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译 P1/ 产品化 P1 配置化演示 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\08\src src\main.cpp -o build\ems_config.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)
echo [OK] build\ems_config.exe
endlocal
