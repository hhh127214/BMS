@echo off
REM =====================================================================
REM  18\ build_test: 编译并运行性能基准 + SLA 门禁
REM
REM  两层，职责不重叠： 
REM
REM    test_perf_stats.exe   计时/统计基础设施的**正确性**（T01~T11）。 
REM        分位数定义（nearest-rank vs 线性插值）、预热、离群值标记、 
REM        mean/stddev（样本口径）、worst_of、门禁逻辑、时钟探针。 
REM        → 保证"尺子本身是准的"。尺子不准，后面所有数字都没意义。 
REM
REM    test_perf_bench.exe   B1~B6 基准 + **23 条 SLA 门禁** + 反向验证。 
REM        B1 单拍闭环 / B2 采集路径 / B3 Modbus 编解码 / B4 SOE+告警 /
REM        B5 全量重发 vs 优化后 / B6 长跑 86400 拍。 
REM        落盘 build\BENCH-REPORT.md（含环境、背景噪声、SLA 对照、可信边界）。 
REM        → 保证"指标达标"，且达标是**可复现、可证伪**的。 
REM
REM  为什么要"单测"和"基准"分两个 exe： 
REM    单测不依赖 RT_DB 共享内存，能在任何机器上先绿；基准强依赖现场环境。 
REM    混在一起的话，一次"共享内存没起来"会被误读成"统计学写错了"。 
REM
REM  ★ 计数口径：本脚本**不去 grep stdout**，而是读 exe 落的纯 ASCII 状态文件 
REM    build\status_*.txt（每行一个 key=value）。理由：本模块输出中英文混排， 
REM    cmd 在 CP936 下抓 UTF-8 管道会乱码/截断；一旦 "PASS=" 那行没抓到，
REM    脚本会把断言数**静默**当 0 上报 —— 看起来跑过、其实是空的。 
REM    这与 13\ 的 bridge_status.txt 是同一套做法。 
REM
REM  ★ 旧状态文件必须先删：否则上一轮留下的 PASS=63 会让本轮"exe 崩了没落盘"
REM    被误判成跑过。宁可多报失败，不可漏报。 
REM
REM  运行方式（**不要**从 Git Bash 直接跑，路径含括号会被 MSYS 拆错）：
REM      python -c "import subprocess;subprocess.run(['cmd','/c','build_test.bat'],cwd=r'D:\wb(cn)\BMS\18\scripts')"
REM =====================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0"

REM ---------- [1/3] 编译 ----------
call "%~dp0build.bat"
set BUILD_RC=!ERRORLEVEL!
if not "!BUILD_RC!"=="0" (
    echo.
    echo [FAIL] 18\ compile did not succeed -- tests not run
    endlocal & exit /b 1
)

cd /d "%~dp0\.."
if not exist build mkdir build

set FAIL=0
set TOT_P=0
set TOT_F=0
set TOT_S=0

REM ---------- [2/3] 逐层运行 ----------
call :run_test perf_stats
call :run_test perf_bench

REM ---------- [3/3] 收尾 ----------
echo.
if "!FAIL!"=="1" (
    echo [FAIL] 18\ 基准/SLA 门禁有失败项
    endlocal & exit /b 1
)

REM  汇总行：**纯 ASCII、单独成行** —— 中文紧邻 !VAR! 会静默吃掉变量，
REM  中文 echo 行后面的数字也可能被吞（本项目 .bat 的踩过的静默失败）。 
echo [OK] 18\ 性能基准与 SLA 门禁全部通过
echo SKIPPED=!TOT_S!
echo PASS=!TOT_P! FAIL=!TOT_F!
echo report: build\BENCH-REPORT.md

endlocal & exit /b 0


REM =====================================================================
REM  子过程：跑一个 test_<name>.exe，读状态文件并入总计
REM =====================================================================
:run_test
set NAME=%~1
set EXE=build\test_!NAME!.exe
set STF=build\status_!NAME!.txt

echo.
echo === running test_!NAME!.exe ===
if not exist "!EXE!" (
    echo [FAIL] !NAME! : exe not found -- compile step produced nothing
    set FAIL=1
    goto :eof
)

REM  先删旧状态：否则上一轮的 PASS= 会被当成这一轮的结果
if exist "!STF!" del /q "!STF!"

"!EXE!"
set RC=!ERRORLEVEL!
if not "!RC!"=="0" (
    echo [FAIL] !NAME! : exited rc=!RC!
    set FAIL=1
)
if not exist "!STF!" (
    echo [FAIL] !NAME! : status file missing -- exe crashed or wrote no status
    set FAIL=1
    goto :eof
)

set P=0
set F=0
set S=0
for /f "tokens=1,2 delims==" %%a in (!STF!) do (
    if "%%a"=="PASS" set P=%%b
    if "%%a"=="FAIL" set F=%%b
    if "%%a"=="SKIPPED" set S=%%b
)
set /a TOT_P+=P
set /a TOT_F+=F
set /a TOT_S+=S

REM  纯 ASCII 结果行：中文紧邻变量会被 codepage 静默吃掉（见 docs\README.md 踩坑） 
echo [result] !NAME! : PASS=!P! FAIL=!F! SKIPPED=!S!
if not "!F!"=="0" set FAIL=1
goto :eof
