@echo off
REM =====================================================================
REM  19/ P5 现场部署：跑全部测试并汇总 
REM
REM  为什么用 Python：本模块的主体是**部署与运维脚本**（守护器 / 配置版本 /
REM  日志轮转），不是头文件库。测试对象就是这些脚本，用 Python 测最直接。 
REM  判据风格仍照项目约定：断言式、印 PASS= / FAIL= 收尾行（见 tests/harness.py）。 
REM
REM  四个测试，职责不重叠： 
REM
REM    test_config_store.py  配置落盘与版本（snapshot / list / diff / rollback） 
REM        判据 ① 快照往返逐字段一致 ② diff 指出"哪个字段从 X 变成 Y"
REM             ③ 回滚后生效值 == 目标版本 ④ 回滚前自动备份（版本数 +1 不是 -1） 
REM        含 [G] 真调 P1/build/ems_config.exe --capture（缺 exe 时显式 SKIP） 
REM
REM    test_log_rotator.py   SOE / metrics 落盘与轮转 
REM        判据 ① 超大小阈值触发轮转 ② 保留份数正确（多的删最旧）
REM             ③ 不丢事件（确定性事件流逐条可比） ④ 轮转后仍能按时间范围查到旧事件 
REM        含导出周期算式 T x r < capacity 的边界两侧反向守卫 
REM
REM    test_supervisor.py    进程守护器 
REM        · 按部署顺序拉起（初始化器最先、就绪后才起下游） 
REM        · 存活检测 + 异常重启 + 退避 + 级联重启
REM        · 就绪超时 = 启动失败（不许退化成 sleep） 
REM        · stdin 看门狗默认必须关闭（>nul / NUL 上 isatty 为真的坑） 
REM        · [H] 段用 11/ 真三进程端到端（缺产物时显式 SKIP） 
REM
REM    test_scripts.py       安装脚本与文档 
REM        · 全部 .bat 过 fix_bat_encoding.py --check（CRLF / 块内路径变量） 
REM        · 含 !VAR! 的行必须是纯 ASCII（中文紧邻 !VAR! 会静默吃掉变量）
REM        · install_task.bat / uninstall_task.bat --dry-run 真跑一次，不改系统
REM        · 现场作业指导书的**点表核对清单**与 13/ 的 --dump-tsv 逐点对账
REM
REM  ---------------------------------------------------------------------
REM  ★ 环境缺失一律显式 SKIP（约定 §4.5）：缺 11/ 产物、缺 ems_config.exe、 
REM    缺 13/sim 都会打印 SKIPPED=n 并以**参与汇总**的方式报出来， 
REM    不会把"没验"写成"验过了"。 
REM ---------------------------------------------------------------------
REM =====================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0\.."
set "MODROOT=!CD!"

set "PYEXE=%EMS_PYTHON%"
if not "!PYEXE!"=="" goto have_python
set "PYEXE=python"
:have_python

"!PYEXE!" -c "import sys" >nul 2>&1
if errorlevel 1 goto no_python

if not exist build mkdir build
del /q build\test_status.txt >nul 2>&1

set NFAIL=0
set TOTAL=0
set TOTFAIL=0
set TOTSKIP=0

echo ============================================
echo  19/ P5 site deployment: running 4 test suites
echo  python: !PYEXE!
echo ============================================

call :run test_config_store
call :run test_log_rotator
call :run test_supervisor
call :run test_scripts

REM  纯 ASCII 状态行落盘：供集成方/上层脚本判定（不要靠 grep 中文输出） 
> build\test_status.txt echo PASS=!TOTAL! FAIL=!TOTFAIL! SKIPPED=!TOTSKIP!

echo.
if not "!NFAIL!"=="0" goto failed

echo [OK] 19\ P5 site deployment: 4 test suite(s) all passed
echo      assertions total: PASS=!TOTAL!  FAIL=0  SKIPPED=!TOTSKIP!
if not "!TOTSKIP!"=="0" goto report_skip
endlocal & exit /b 0

:report_skip
echo      [SKIP] !TOTSKIP! assertion^(s^) explicitly skipped ^(environment limit, not a defect^)
echo             printed on purpose: "skipped" must stay distinguishable from "ran".
endlocal & exit /b 0

:failed
echo [FAIL] 19\ P5 site deployment: !NFAIL! test suite^(s^) did not pass
echo        assertions total: PASS=!TOTAL!  FAIL=!TOTFAIL!  SKIPPED=!TOTSKIP!
endlocal & exit /b 1

:no_python
echo [FAIL] no usable python: !PYEXE!
echo        set EMS_PYTHON to a python.exe, or put python on PATH
endlocal & exit /b 1


REM =====================================================================
REM  :run —— 跑一个 Python 测试并累加它的 PASS= / FAIL= / SKIPPED= 行 
REM
REM  ★ 只用裸文件名与相对路径做参数（本仓库根路径含括号 D:\wb(cn)\BMS）。 
REM  ★ 块内读取路径语义的变量一律 !VAR! 延迟展开（约定 §4.2）。 
REM  ★ 每个测试的收尾行格式是契约：行首 PASS=、单个空格分隔。 
REM =====================================================================
:run
set "TNAME=%~1"
set "TLOG=build\!TNAME!.stdout.log"
set "TERR=build\!TNAME!.stderr.log"

echo.
echo === !TNAME! ===
"!PYEXE!" tests\!TNAME!.py > "!TLOG!" 2> "!TERR!"
set "TRC=0"
if errorlevel 1 set "TRC=1"

type "!TLOG!"
if "!TRC!"=="1" (
    echo ---------- !TNAME! stderr ----------
    type "!TERR!"
    echo [FAIL] !TNAME! has failing assertions
    set /a NFAIL+=1
)

for /f "tokens=1,2,3 delims= " %%A in ('findstr /b "PASS=" "!TLOG!" 2^>nul') do (
    for /f "tokens=2 delims==" %%P in ("%%A") do set /a TOTAL+=%%P
    for /f "tokens=2 delims==" %%F in ("%%B") do set /a TOTFAIL+=%%F
    for /f "tokens=2 delims==" %%Q in ("%%C") do set /a TOTSKIP+=%%Q
)
exit /b 0
