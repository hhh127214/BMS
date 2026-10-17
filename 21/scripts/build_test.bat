@echo off
REM =====================================================================
REM  21\ build_test: DL/T645 单元测试
REM
REM  三层，职责不重叠： 
REM
REM    test_dlt645_frame.exe    帧层 + 数据项（T01~T22） 
REM       地址 BCD / CS / +0x33 / DI 反序 / BCD 半字节校验 / 符号位 / 变比。 
REM       全内存，零硬件，零外部实现。 
REM
REM    test_dlt645_master.exe   主站 + 进程内假表（T31~T45） 
REM       超时重试 / 半帧 / 坏 CS / 异常应答 / 地址不符 / DI 不符 /
REM       漏加 0x33 / 残留应答 / 统计分类。 
REM
REM    test_meter645_io.exe     645 接进 EMS（T50~T63） 
REM       DI → IDeviceIO → 快照端到端 / 超时保旧值 / 写拒绝 /
REM       绑定表自检四类坏表 / 坏表零流量。 
REM       ★ 要链 07/src/rtdb/ems_point_table.c（抽象点表真相源）。 
REM
REM  ★ 为什么没有 13\ 那样的"跨语言层"： 
REM    645 没有现成的、可 pip 安装的独立实现可以当对端（c104 / pymodbus
REM    那类库没有 645）。所以"两个独立实现互通"这条证据， 
REM    在本模块改为 **跨进程 TCP 实测**： 
REM        build\sim_tcp_server.exe + meter645.exe --tcp 127.0.0.1:PORT
REM    它证明的是"我们的链路实现对着真实的、会分片的字节流能收敛"， 
REM    而不是"我们和别人的实现能对上话" —— 这两件事不同，不要混说。 
REM
REM  ★ 断言汇总行（PASS=）不存在即判定失败：
REM    stdout 重定向到文件时是全缓冲的，进程异常终止会把整份输出连同 
REM    汇总行一起吞掉，而退出码仍是 0。见 :check_summary。 
REM =====================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0\.."
if not exist build mkdir build

set INC=-I src
set FAIL=0

echo === [1/2] test_dlt645_frame.exe（T01~T22 帧层 + 数据项） ===
g++ -std=c++17 -Wall -Wextra -O2 %INC% ^
    tests\test_dlt645_frame.cpp -o build\test_dlt645_frame.exe
if errorlevel 1 ( echo [FAIL] test_dlt645_frame compile error & exit /b 1 )
build\test_dlt645_frame.exe > build\layer1.log 2>&1
set L1_RC=%ERRORLEVEL%
type build\layer1.log
if not "%L1_RC%"=="0" ( echo [FAIL] test_dlt645_frame tests did not pass & set FAIL=1 )
call :check_summary build\layer1.log test_dlt645_frame

echo.
echo === [2/2] test_dlt645_master.exe（T31~T45 主站 + 假表） ===
g++ -std=c++17 -Wall -Wextra -O2 %INC% ^
    tests\test_dlt645_master.cpp -o build\test_dlt645_master.exe
if errorlevel 1 ( echo [FAIL] test_dlt645_master compile error & exit /b 1 )
build\test_dlt645_master.exe > build\layer2.log 2>&1
set L2_RC=%ERRORLEVEL%
type build\layer2.log
if not "%L2_RC%"=="0" ( echo [FAIL] test_dlt645_master tests did not pass & set FAIL=1 )
call :check_summary build\layer2.log test_dlt645_master

echo.
echo === [3/3] test_meter645_io.exe（T50~T63 645 → IDeviceIO → EMS 快照） ===
REM 这一层要链 07/ 的点表真相源（ems_point_table.c，C 实现），
REM 因为 Meter645DeviceIO 的缓存与绑定都按 EMS 抽象点索引。 
gcc -std=c11 -O2 -I ..\07\src\rtdb -c ..\07\src\rtdb\ems_point_table.c -o build\ems_point_table.o
if errorlevel 1 ( echo [FAIL] ems_point_table.c compile error & exit /b 1 )
g++ -std=c++17 -Wall -Wextra -O2 %INC% -I ..\04\src -I ..\07\src\rtdb ^
    tests\test_meter645_io.cpp build\ems_point_table.o -o build\test_meter645_io.exe
if errorlevel 1 ( echo [FAIL] test_meter645_io compile error & exit /b 1 )
build\test_meter645_io.exe > build\layer3.log 2>&1
set L3_RC=%ERRORLEVEL%
type build\layer3.log
if not "%L3_RC%"=="0" ( echo [FAIL] test_meter645_io tests did not pass & set FAIL=1 )
call :check_summary build\layer3.log test_meter645_io

echo.
if "%FAIL%"=="1" (
    echo [FAIL] 21\ DL/T645 测试有失败项
    endlocal & exit /b 1
)
echo [OK] 21\ DL/T645 三层测试全部通过
endlocal & exit /b 0

REM =====================================================================
REM  :check_summary <logfile> <name>
REM
REM  汇总行必须存在 —— 否则按"未通过"处理，而不是静默计 0。 
REM  这条闸的价值：日志里"一步一条断言都没有"与"这一步跑过了"在 
REM  汇总统计上不可区分，而 baseline.py 正是按各 exe 的汇总行相加的。 
REM
REM  ★ 注意 %~1 不直接写进 if 块：块内展开时值里若有括号会崩解析
REM    （本仓库实际踩过，见 fix_bat_encoding.py 的 BLOCK VAR 检查）。 
REM    先赋给普通变量，再在块里用，路径里有什么括号都不影响。 
REM =====================================================================
:check_summary
set "SUMMARY_LOG=%~1"
set "SUMMARY_NAME=%~2"
findstr /B /C:"PASS=" "%SUMMARY_LOG%" >nul 2>nul
if errorlevel 1 (
    echo [FAIL] %SUMMARY_NAME% 的输出里没有汇总行 PASS= —— 视为未通过
    echo        常见原因：stdout 缓冲区没被 flush（见本文件 :check_summary 注释）。 
    set FAIL=1
)
goto :eof
