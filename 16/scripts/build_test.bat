@echo off
REM =====================================================================
REM  16\ build_test: 通信加固五层单元测试（T01~T20 / T30~T40 / T50~T65 / T70~T78 / T80~T89） 
REM
REM  五层，职责不重叠（这是本模块测试结构的核心设计）： 
REM
REM    test_backoff_arbiter.exe  重连退避 + 主备双链路仲裁（T01~T20） 
REM        纯内存、无外部依赖。确定性 PRNG 让"同一 seed 同一序列"可复现； 
REM        假链路 FakeLink 让"主链路连续 N 次失败才切换"变成可精确驱动的判据。 
REM
REM    test_sntp.exe             SNTP 对时（T30~T40） 
REM        进程内 UDP 假 SNTP 服务端（端口 12345），不依赖公网。 
REM        可注入**单向路径延迟**与处理时间 —— 否则 offset 的符号都验不出来。 
REM
REM    test_modbus_rtu.exe       Modbus RTU 串口（T50~T65） 
REM        真串口时序无法在单测里造，所以分帧器把"静默间隔"做成显式参数： 
REM        feed(bytes, gap_ms)。半包不给 gap / 给够 gap 于是变成普通函数入参。 
REM        CRC 已知答案向量已与 pymodbus 交叉核对（见 docs/README.md 坑记录）。 
REM
REM    test_ftp.exe              FTP 定值下发通道（T70~T78） 
REM        进程内极简 FTP 服务端（控制口 2121），能精确造出多行 banner、 
REM        150/226 分段、PASV 端口这些坑，并逐位对账收发字节。 
REM
REM    test_tls.exe              传输层安全（T80~T89） 
REM        SChannel 真 TLS 客户端 x **Python ssl（OpenSSL）** 服务端。 
REM        需要外部 Python；缺失时本层显式打印 SKIPPED=N 并以 0 退出。 
REM        ★ 为什么必须用别人的 TLS 实现当对端：自己写两端等于自己批改 
REM          自己的卷子 —— SChannel 客户端配 SChannel 服务端，双方共享 
REM          同一个 bug 时测试照样全绿。 
REM
REM  环境变量 EMS_PYTHON：指向 python.exe（TLS 层用）。 
REM    定位顺序：EMS_PYTHON → PATH 上的 python.exe / py.exe。 
REM    都不可用时 TLS 层打印 SKIPPED 并以 0 退出（**skip 而非 fail**）。 
REM
REM  ★ 每层都把完整输出落一份到 build\<层名>_status.txt： 
REM    ① 控制台输出与落盘内容逐字一致，出问题可回看； 
REM    ② 末行的 PASS= / FAIL= / SKIPPED= 是**纯 ASCII**，可以直接被 
REM       findstr + for /f 解析出来加总，不必去 grep 中文（编码问题会骗人）。 
REM
REM  ★ 为什么非要把 SKIP 喊出来：SKIP 本身是有意的（环境问题不是代码缺陷）， 
REM    但如果脚本仍然印「五层全部通过」，那么「静默跳过」与「真的跑过」 
REM    在最终输出上**依然不可区分** —— 这正是本项目最忌讳的一类失败。 
REM
REM  ★ 写法约定（本文件刻意遵守，别改回去）： 
REM    · 块内路径类变量一律 !VAR!（见 新模块开发约定.md §4.2）； 
REM    · **含 !VAR! 的行保持纯 ASCII** —— 中文经 CP936 错位解码时会把紧随其后的 
REM      ! 当成双字节字的第二个字节吃掉，变量就静默不展开了（本项目实测踩到， 
REM      现象是 "PASS=!TOTAL!" 原样打出来）。中文只出现在不含变量的 echo 行； 
REM    · 多语句分支用 goto + 标签，不用 if (...) 块 —— 块内的中文/&/) 更容易被 
REM      错位解码破坏块结构，而块结构坏掉时脚本可能**什么都不做就退出 0**。 
REM =====================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0\.."

if not exist build mkdir build

if "!EMS_PYTHON!"=="" echo [info] EMS_PYTHON 未设置，TLS 层将尝试 PATH 上的 python.exe / py.exe

set INC=-I src
set LNK=-lws2_32
set LNKTLS=-lws2_32 -lsecur32 -lcrypt32
set FAIL=0

REM ---------- [1/5] 重连退避 + 主备双链路 ----------
echo === [1/5] test_backoff_arbiter.exe   T01~T20 重连退避 + 主备双链路仲裁 ===
g++ -std=c++17 -Wall -O2 %INC% tests\test_backoff_arbiter.cpp %LNK% -o build\test_backoff_arbiter.exe
if errorlevel 1 set CF=test_backoff_arbiter
if errorlevel 1 goto :CFAIL
del /q build\backoff_arbiter_status.txt 2>nul
build\test_backoff_arbiter.exe > build\backoff_arbiter_status.txt 2>&1
set RC=!ERRORLEVEL!
type build\backoff_arbiter_status.txt
set P1=0
set F1=1
set S1=0
set OK1=1
for /f "tokens=2,4,6 delims== " %%a in ('findstr /b "PASS=" build\backoff_arbiter_status.txt') do (
    set P1=%%a
    set F1=%%b
    set S1=%%c
    set OK1=0
)
if "!OK1!"=="1" echo [FAIL] state line not found: test_backoff_arbiter
if "!OK1!"=="1" set FAIL=1
if not "!RC!"=="0" echo [FAIL] tests failed: test_backoff_arbiter
if not "!RC!"=="0" set FAIL=1

REM ---------- [2/5] SNTP 对时 ----------
echo.
echo === [2/5] test_sntp.exe   T30~T40 SNTP 对时 ===
g++ -std=c++17 -Wall -O2 %INC% tests\test_sntp.cpp %LNK% -o build\test_sntp.exe
if errorlevel 1 set CF=test_sntp
if errorlevel 1 goto :CFAIL
del /q build\sntp_status.txt 2>nul
build\test_sntp.exe > build\sntp_status.txt 2>&1
set RC=!ERRORLEVEL!
type build\sntp_status.txt
set P2=0
set F2=1
set S2=0
set OK2=1
for /f "tokens=2,4,6 delims== " %%a in ('findstr /b "PASS=" build\sntp_status.txt') do (
    set P2=%%a
    set F2=%%b
    set S2=%%c
    set OK2=0
)
if "!OK2!"=="1" echo [FAIL] state line not found: test_sntp
if "!OK2!"=="1" set FAIL=1
if not "!RC!"=="0" echo [FAIL] tests failed: test_sntp
if not "!RC!"=="0" set FAIL=1

REM ---------- [3/5] Modbus RTU ----------
echo.
echo === [3/5] test_modbus_rtu.exe   T50~T65 Modbus RTU 串口 ===
g++ -std=c++17 -Wall -O2 %INC% tests\test_modbus_rtu.cpp %LNK% -o build\test_modbus_rtu.exe
if errorlevel 1 set CF=test_modbus_rtu
if errorlevel 1 goto :CFAIL
del /q build\modbus_rtu_status.txt 2>nul
build\test_modbus_rtu.exe > build\modbus_rtu_status.txt 2>&1
set RC=!ERRORLEVEL!
type build\modbus_rtu_status.txt
set P3=0
set F3=1
set S3=0
set OK3=1
for /f "tokens=2,4,6 delims== " %%a in ('findstr /b "PASS=" build\modbus_rtu_status.txt') do (
    set P3=%%a
    set F3=%%b
    set S3=%%c
    set OK3=0
)
if "!OK3!"=="1" echo [FAIL] state line not found: test_modbus_rtu
if "!OK3!"=="1" set FAIL=1
if not "!RC!"=="0" echo [FAIL] tests failed: test_modbus_rtu
if not "!RC!"=="0" set FAIL=1

REM ---------- [4/5] FTP 定值通道 ----------
echo.
echo === [4/5] test_ftp.exe   T70~T78 FTP 定值下发通道 ===
g++ -std=c++17 -Wall -O2 %INC% tests\test_ftp.cpp %LNK% -o build\test_ftp.exe
if errorlevel 1 set CF=test_ftp
if errorlevel 1 goto :CFAIL
del /q build\ftp_status.txt 2>nul
build\test_ftp.exe > build\ftp_status.txt 2>&1
set RC=!ERRORLEVEL!
type build\ftp_status.txt
set P4=0
set F4=1
set S4=0
set OK4=1
for /f "tokens=2,4,6 delims== " %%a in ('findstr /b "PASS=" build\ftp_status.txt') do (
    set P4=%%a
    set F4=%%b
    set S4=%%c
    set OK4=0
)
if "!OK4!"=="1" echo [FAIL] state line not found: test_ftp
if "!OK4!"=="1" set FAIL=1
if not "!RC!"=="0" echo [FAIL] tests failed: test_ftp
if not "!RC!"=="0" set FAIL=1

REM ---------- [5/5] TLS ----------
REM  缺 python 时本层打印 SKIPPED=N 并以 0 退出 —— 那是**有意的**。 
echo.
echo === [5/5] test_tls.exe   T80~T89 传输层安全 [SChannel x Python ssl] ===
g++ -std=c++17 -Wall -O2 %INC% tests\test_tls.cpp %LNKTLS% -o build\test_tls.exe
if errorlevel 1 set CF=test_tls
if errorlevel 1 goto :CFAIL
del /q build\tls_status.txt 2>nul
build\test_tls.exe > build\tls_status.txt 2>&1
set RC=!ERRORLEVEL!
type build\tls_status.txt
set P5=0
set F5=1
set S5=0
set OK5=1
for /f "tokens=2,4,6 delims== " %%a in ('findstr /b "PASS=" build\tls_status.txt') do (
    set P5=%%a
    set F5=%%b
    set S5=%%c
    set OK5=0
)
if "!OK5!"=="1" echo [FAIL] state line not found: test_tls
if "!OK5!"=="1" set FAIL=1
if not "!RC!"=="0" echo [FAIL] tests failed: test_tls
if not "!RC!"=="0" set FAIL=1

REM ---------- 收尾 ----------
set /a TOTAL=!P1!+!P2!+!P3!+!P4!+!P5!
set /a TFAIL=!F1!+!F2!+!F3!+!F4!+!F5!
set /a TSKIP=!S1!+!S2!+!S3!+!S4!+!S5!

echo.
if "!FAIL!"=="1" goto :TESTS_FAIL
if not "!TSKIP!"=="0" goto :TESTS_SKIP

echo [OK] 16\ 通信加固 五层测试全部通过 
echo      合计 PASS=!TOTAL! FAIL=!TFAIL! SKIPPED=!TSKIP!
echo      逐层 backoff=!P1! sntp=!P2! modbus_rtu=!P3! ftp=!P4! tls=!P5!
endlocal
exit /b 0

:TESTS_FAIL
echo [FAIL] 16\ 通信加固 五层测试有失败项 
echo       合计 PASS=!TOTAL! FAIL=!TFAIL! SKIPPED=!TSKIP!
echo       逐层 backoff=!P1! sntp=!P2! modbus_rtu=!P3! ftp=!P4! tls=!P5!
endlocal
exit /b 1

:TESTS_SKIP
echo [SKIP] 16\ TLS 层有未运行用例，因为环境里没有 python（这不是代码缺陷） 
echo        本次 16\ 的口径是 PASS=!TOTAL! FAIL=!TFAIL! SKIPPED=!TSKIP!，TLS 端到端断言未验证 
echo        逐层 backoff=!P1! sntp=!P2! modbus_rtu=!P3! ftp=!P4! tls=!P5!
echo        补法 把 python.exe 放进 PATH，或 set EMS_PYTHON 指向 python 的绝对路径 
endlocal
exit /b 0

:CFAIL
echo [FAIL] compile error in: !CF!
endlocal
exit /b 1
