@echo off
REM =====================================================================
REM  20/ build_test: 编译并运行「告警能力与持久化」单元测试 
REM
REM    test_alarm_model.exe    告警模型 + 装配器 
REM                             （含 11/§4 六类故障源对齐 / 边沿语义 /
REM                              仿真入口 vs 生产入口 等价性）
REM    test_soe_store.exe      SOE 持久化（落盘 / 检索 / 重启逐条一致 /
REM                              超容量不丢 / 文件格式稳定） 
REM    test_config_store.exe   配置持久化（字段往返 / 变更台账 /
REM                              缺字段与损坏行显式报错）
REM
REM  ★ 汇总数字从**每个 exe 落的纯 ASCII 状态行**里读，不去 grep 中文输出 —— 
REM    本仓库的 .bat 在 CP936 下解析 UTF-8 会出静默问题（见 13/ 的先例）。 
REM
REM  ★ .bat 三条纪律（实测，都是静默失败、退出码仍是 0）：
REM    1. 含 !VAR! 的行保持纯 ASCII —— 中文尾字节会与 ! 配对被吃掉 
REM    2. 汇总数字单独占一行且纯 ASCII —— 中文 echo 之后的数字会被吞
REM    3. 多语句分支用 goto + 标签，不用 if () 块 
REM =====================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0\.."

if not exist build mkdir build

set INC=-I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\08\src -I ..\07\src\rtdb -I ..\10\src -I ..\P2\src

set FAIL=0
set PASS_A=0
set FAIL_A=0
set PASS_S=0
set FAIL_S=0
set PASS_C=0
set FAIL_C=0

echo === [1/3] test_alarm_model.exe ===
g++ -std=c++17 -Wall -O2 !INC! tests\test_alarm_model.cpp -o build\test_alarm_model.exe
if errorlevel 1 goto :compile_fail
del /q build\test_alarm_model_status.txt 2>nul
build\test_alarm_model.exe
if errorlevel 1 set FAIL=1

echo.
echo === [2/3] test_soe_store.exe ===
g++ -std=c++17 -Wall -O2 !INC! tests\test_soe_store.cpp -o build\test_soe_store.exe
if errorlevel 1 goto :compile_fail
del /q build\test_soe_store_status.txt 2>nul
build\test_soe_store.exe
if errorlevel 1 set FAIL=1

echo.
echo === [3/3] test_config_store.exe ===
g++ -std=c++17 -Wall -O2 !INC! tests\test_config_store.cpp -o build\test_config_store.exe
if errorlevel 1 goto :compile_fail
del /q build\test_config_store_status.txt 2>nul
build\test_config_store.exe
if errorlevel 1 set FAIL=1

REM ---------- 读状态文件（缺文件 = 没跑成，按失败处理）----------
set RAN_A=0
if exist build\test_alarm_model_status.txt set RAN_A=1
if "!RAN_A!"=="0" set FAIL=1
set RAN_S=0
if exist build\test_soe_store_status.txt set RAN_S=1
if "!RAN_S!"=="0" set FAIL=1
set RAN_C=0
if exist build\test_config_store_status.txt set RAN_C=1
if "!RAN_C!"=="0" set FAIL=1

if "!RAN_A!"=="1" for /f "tokens=2 delims== " %%a in ('findstr /B "PASS=" build\test_alarm_model_status.txt') do set PASS_A=%%a
if "!RAN_A!"=="1" for /f "tokens=4 delims== " %%a in ('findstr /B "PASS=" build\test_alarm_model_status.txt') do set FAIL_A=%%a
if "!RAN_S!"=="1" for /f "tokens=2 delims== " %%a in ('findstr /B "PASS=" build\test_soe_store_status.txt') do set PASS_S=%%a
if "!RAN_S!"=="1" for /f "tokens=4 delims== " %%a in ('findstr /B "PASS=" build\test_soe_store_status.txt') do set FAIL_S=%%a
if "!RAN_C!"=="1" for /f "tokens=2 delims== " %%a in ('findstr /B "PASS=" build\test_config_store_status.txt') do set PASS_C=%%a
if "!RAN_C!"=="1" for /f "tokens=4 delims== " %%a in ('findstr /B "PASS=" build\test_config_store_status.txt') do set FAIL_C=%%a

set /a TOTAL_PASS=!PASS_A!+!PASS_S!+!PASS_C!
set /a TOTAL_FAIL=!FAIL_A!+!FAIL_S!+!FAIL_C!

echo.
echo === 20/ alarm and persistence test summary ===
echo TOTAL_ASSERTIONS=!TOTAL_PASS!
echo TOTAL_FAIL=!TOTAL_FAIL!
echo SKIPPED=0
echo BY_SUITE=!PASS_A!+!PASS_S!+!PASS_C!

if not "!TOTAL_FAIL!"=="0" set FAIL=1
if "!FAIL!"=="1" goto :failed

echo [OK] 20\ tests all passed
endlocal & exit /b 0

:failed
echo [FAIL] 20\ tests failed
endlocal & exit /b 1

:compile_fail
echo [FAIL] 20\ compile error
endlocal & exit /b 1
