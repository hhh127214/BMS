@echo off
REM =====================================================================
REM  19/ P5 现场部署：**前台**跑守护器（现场调试 / 演练用）
REM
REM  用途：计划任务装好之前先在控制台里看着它跑。现场排查也用它 —— 
REM        守护器的每条决策（start / ready / exit / cascade / kill） 
REM        都会打到屏幕上，同时落 <root>\log\supervisor.log。 
REM
REM  用法： 
REM    run_supervisor.bat                    一直跑
REM    run_supervisor.bat run --duration 60   只跑 60 秒（演练） 
REM    run_supervisor.bat check               只做前置检查 
REM    run_supervisor.bat plan                只看启动顺序
REM
REM  ---------------------------------------------------------------------
REM  ★ 不要把本脚本的 stdin 重定向到 NUL： 
REM    Windows 上 isatty(NUL) 返回真，而 stdin 指向 NUL 时读到的是 EOF。 
REM    虽然 ems_supervisor.py 的 stdin 看门狗**默认关闭**（正是为了这个坑），
REM    但一旦有人用 EMS_SUPERVISOR_STDIN_WATCHDOG=1 打开它，
REM    "> nul < nul" 会让守护器打印完 banner 就立刻退出。 
REM    所以本脚本**不做任何 stdin 重定向**。 
REM  ---------------------------------------------------------------------
REM =====================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0\..\.."
set "ROOT=!CD!"
set "SUP=!ROOT!\19\src\ems_supervisor.py"

set "PYEXE=%EMS_PYTHON%"
if not "!PYEXE!"=="" goto have_python
if exist "!ROOT!\13\.venv\Scripts\python.exe" set "PYEXE=!ROOT!\13\.venv\Scripts\python.exe"
if not "!PYEXE!"=="" goto have_python
set "PYEXE=python"
:have_python

if not exist "!SUP!" goto no_supervisor

REM  ★ 本脚本**不做任何写盘**：conf\supervisor.json 不存在时守护器用内置默认清单， 
REM    log\ 目录由守护器自己在 run 时建。这样 `plan` / `check` 是零副作用的 —— 
REM    现场演练"看一眼启动顺序"不该改任何东西。 

"!PYEXE!" "!SUP!" --root "!ROOT!" %*
set "RC=%errorlevel%"
echo.
echo [DONE] exit code = !RC!
echo        log: !ROOT!\log\supervisor.log
endlocal & exit /b %RC%

:no_supervisor
echo [FAIL] missing !SUP!
endlocal & exit /b 1
