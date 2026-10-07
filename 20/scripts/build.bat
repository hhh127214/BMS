@echo off
REM =====================================================================
REM  20/ build: 编译本模块的可执行产物 
REM
REM    soe_dump.exe     SOE 落盘文件的读取 / 过滤 / 导出（--dump 入口） 
REM    alarm_demo.exe   告警能力 + 持久化 端到端演示 
REM
REM  测试请走 build_test.bat。 
REM
REM  ★ .bat 纪律（本项目实测，都是静默失败、退出码仍是 0）：
REM    含 !VAR! 的行保持纯 ASCII；多语句分支用 goto + 标签，不用 if () 块。 
REM =====================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0\.."

if not exist build mkdir build

set INC=-I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\08\src -I ..\07\src\rtdb -I ..\10\src -I ..\P2\src

echo === [1/2] soe_dump.exe ===
g++ -std=c++17 -Wall -O2 !INC! src\soe_dump.cpp -o build\soe_dump.exe
if errorlevel 1 goto :fail

echo === [2/2] alarm_demo.exe ===
g++ -std=c++17 -Wall -O2 !INC! src\alarm_demo.cpp -o build\alarm_demo.exe
if errorlevel 1 goto :fail

echo [OK] 20\ build ok
endlocal & exit /b 0

:fail
echo [FAIL] 20\ build failed
endlocal & exit /b 1
