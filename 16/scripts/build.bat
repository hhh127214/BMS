@echo off
REM =====================================================================
REM  16\ build: 只编译，不运行 
REM
REM  16\ 是 header-only 模块，没有 .lib/.dll 之类的库产物 —— 
REM  它的"产物"就是 tests\ 下五个测试可执行文件。所以本脚本做的是 
REM  **编译冒烟检查**：把五个 exe 都编出来放到 build\，不跑任何用例。 
REM
REM  什么时候用它： 
REM    · 只想确认改动没有把编译搞坏（比 build_test.bat 快，且不受端口占用影响）； 
REM    · 集成方只需要"能编译"这一级证据时。 
REM  跑用例请用 build_test.bat。 
REM
REM  ★ 报告里必须能区分"编过了"与"跑过了"：本脚本只印 compiled， 
REM    不印 passed —— 否则"编译成功"会被误读成"测试通过"。 
REM
REM  ★ 写法约定同 build_test.bat：含变量的行保持纯 ASCII，多语句分支用 goto。 
REM =====================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0\.."

if not exist build mkdir build

set INC=-I src
set LNK=-lws2_32
set LNKTLS=-lws2_32 -lsecur32 -lcrypt32

echo === 16\ 编译五个测试可执行文件 [只编译，不运行] ===

echo [1/5] test_backoff_arbiter.exe
g++ -std=c++17 -Wall -O2 %INC% tests\test_backoff_arbiter.cpp %LNK% -o build\test_backoff_arbiter.exe
if errorlevel 1 set CF=test_backoff_arbiter
if errorlevel 1 goto :CFAIL

echo [2/5] test_sntp.exe
g++ -std=c++17 -Wall -O2 %INC% tests\test_sntp.cpp %LNK% -o build\test_sntp.exe
if errorlevel 1 set CF=test_sntp
if errorlevel 1 goto :CFAIL

echo [3/5] test_modbus_rtu.exe
g++ -std=c++17 -Wall -O2 %INC% tests\test_modbus_rtu.cpp %LNK% -o build\test_modbus_rtu.exe
if errorlevel 1 set CF=test_modbus_rtu
if errorlevel 1 goto :CFAIL

echo [4/5] test_ftp.exe
g++ -std=c++17 -Wall -O2 %INC% tests\test_ftp.cpp %LNK% -o build\test_ftp.exe
if errorlevel 1 set CF=test_ftp
if errorlevel 1 goto :CFAIL

echo [5/5] test_tls.exe
g++ -std=c++17 -Wall -O2 %INC% tests\test_tls.cpp %LNKTLS% -o build\test_tls.exe
if errorlevel 1 set CF=test_tls
if errorlevel 1 goto :CFAIL

echo [OK] 16\ 五个测试可执行文件已编译到 build\ 目录 
echo      注意 本脚本只编译、未运行任何用例；跑用例请用 build_test.bat
endlocal
exit /b 0

:CFAIL
echo [FAIL] compile error in: !CF!
endlocal
exit /b 1
