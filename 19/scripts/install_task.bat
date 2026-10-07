@echo off
REM =====================================================================
REM  19/ P5 现场部署：安装开机自启（Windows 计划任务） 
REM
REM  形态决定：计划任务（不是 Windows 服务）。理由见 docs/README.md
REM  「部署形态」一节，这里只给结论： 
REM    ① 零第三方依赖 —— schtasks.exe 系统自带； 
REM    ② 真·Windows 服务要做 SCM 握手（StartServiceCtrlDispatcher），
REM       Python 侧不做握手会被 SCM 约 30 秒后杀掉，且 sc query 只说
REM       STOPPED，看起来像"服务自己崩了"。要走服务路线见
REM       install_service.bat（那里给了配方与前置条件）。 
REM    ③ 服务化真正值钱的那部分（开机自启 / 失败重启 / SYSTEM 账号） 
REM       计划任务同样有；而"按顺序拉起 + 退避 + 级联重启"由 
REM       19/src/ems_supervisor.py 自己做，外面包什么都得自己写。 
REM
REM  安装根怎么定：默认 = 本脚本上溯两级（即 <root>\19\scripts\ 的 <root>）。 
REM  可用 EMS_SITE_ROOT 覆盖（现场把 19\ 工具与产物分开放时用）。 
REM
REM  前置条件： 
REM    · 管理员权限（/RU SYSTEM 需要）
REM    · 安装根路径**不含空格**（schtasks 的 /TR 嵌套引号对空格很敏感） 
REM    · <root>\19\src\ems_supervisor.py 与 <root>\11\build\*.exe 已就位 
REM
REM  用法： 
REM    install_task.bat            安装
REM    install_task.bat --dry-run   只打印将要执行的命令，**不产生任何文件**
REM    install_task.bat --check     只做前置检查，**不产生任何文件**
REM
REM  ★ 本文件的三条 .bat 纪律（见 docs/规划/新模块开发约定.md §4）：
REM    行尾必须 CRLF；含 !VAR! 的行保持纯 ASCII； 
REM    分支用 goto + 标签，不用 if ( ) 块（路径含括号会让块解析崩）。 
REM =====================================================================

setlocal enabledelayedexpansion
REM  %~dp0 = <root>\19\scripts\  → 上溯两级 = <root>
cd /d "%~dp0\..\.."
set "ROOT=!CD!"
if not "%EMS_SITE_ROOT%"=="" set "ROOT=%EMS_SITE_ROOT%"

set "TASKNAME=EMS_Site_P5"
set "SUP=!ROOT!\19\src\ems_supervisor.py"
set "DRY=0"
set "CHECKONLY=0"

if /I "%~1"=="--dry-run" set "DRY=1"
if /I "%~1"=="--check" set "CHECKONLY=1"

echo ============================================
echo  P5 install scheduled task: !TASKNAME!
echo  install root: !ROOT!
echo ============================================

REM ---------- [1/5] 安装根路径不含空格 ----------
echo !ROOT!| findstr /C:" " >nul
if not errorlevel 1 goto path_has_space

REM ---------- [2/5] 找 Python 解释器 ----------
REM  顺序：EMS_PYTHON -> <root>\13\.venv -> PATH 上的 python
set "PYEXE=%EMS_PYTHON%"
if not "!PYEXE!"=="" goto have_python
if exist "!ROOT!\13\.venv\Scripts\python.exe" set "PYEXE=!ROOT!\13\.venv\Scripts\python.exe"
if not "!PYEXE!"=="" goto have_python
set "PYEXE=python"
:have_python
echo python: !PYEXE!
"!PYEXE!" -c "import sys;print(sys.version.split()[0])" >nul 2>&1
if errorlevel 1 goto no_python

REM ---------- [3/5] 守护器脚本就位 ----------
if not exist "!SUP!" goto no_supervisor

REM ---------- [4/5] 前置检查（只读：不建目录、不写配置） ----------
REM  注意 conf\supervisor.json 不存在时 check 会用内置默认清单，所以这里 
REM  不需要先写文件 —— 保证 --check / --dry-run **零副作用**。 
"!PYEXE!" "!SUP!" --root "!ROOT!" check
if errorlevel 1 goto check_failed

if "!CHECKONLY!"=="1" goto check_only

set "TASKCMD=\"!PYEXE!\" \"!SUP!\" --root \"!ROOT!\" run"
set "SCHTASKCMD=schtasks /Create /TN \"!TASKNAME!\" /SC ONSTART /RU SYSTEM /RL HIGHEST /F /TR "!TASKCMD!""

if "!DRY!"=="1" goto dry_run_task

REM ---------- [5/5] 建目录 + 落默认清单 + 注册任务 ----------
REM  只有真正安装才写盘。默认清单是**起点**：现场按站点改 conf\supervisor.json
REM  （把 exe 换成真实产物路径），改完重跑本脚本（/F 会覆盖任务定义）。 
if not exist "!ROOT!\conf" mkdir "!ROOT!\conf"
if not exist "!ROOT!\log" mkdir "!ROOT!\log"
if not exist "!ROOT!\conf\supervisor.json" "!PYEXE!" "!SUP!" --root "!ROOT!" --write-config

net session >nul 2>&1
if errorlevel 1 goto need_admin

echo.
echo [EXEC] schtasks /Create /TN "!TASKNAME!" /SC ONSTART /RU SYSTEM /RL HIGHEST /F
schtasks /Create /TN "!TASKNAME!" /SC ONSTART /RU SYSTEM /RL HIGHEST /F /TR "!TASKCMD!"
if errorlevel 1 goto task_failed

echo.
echo [OK] scheduled task registered: !TASKNAME!
echo      verify : schtasks /Query /TN !TASKNAME! /V /FO LIST
echo      run now: schtasks /Run   /TN !TASKNAME!
echo      stop   : schtasks /End   /TN !TASKNAME!
echo      remove : 19\scripts\uninstall_task.bat
endlocal & exit /b 0

:dry_run_task
echo.
echo [DRY-RUN] would register scheduled task, command line:
echo   !SCHTASKCMD!
echo.
echo [OK] dry-run: nothing was changed
endlocal & exit /b 0

:check_only
echo.
echo [OK] check only: prerequisites satisfied (nothing was changed)
endlocal & exit /b 0

:path_has_space
echo [FAIL] install root contains a space: !ROOT!
echo        schtasks /TR nesting is fragile with spaces.
echo        Use a space-free install root, e.g. D:\EMS  (see docs: deployment)
endlocal & exit /b 1

:no_python
echo [FAIL] no usable python: !PYEXE!
echo        set EMS_PYTHON to a python.exe, or create 13\.venv
endlocal & exit /b 1

:no_supervisor
echo [FAIL] missing !SUP!
echo        copy the 19\ tree into the install root first
endlocal & exit /b 1

:check_failed
echo [FAIL] supervisor preflight failed (see lines above)
echo        usually: 11\build\*.exe not built / not copied into the install root
endlocal & exit /b 1

:need_admin
echo [FAIL] administrator rights required (/RU SYSTEM)
echo        right-click cmd.exe -^> Run as administrator, then rerun
endlocal & exit /b 1

:task_failed
echo [FAIL] schtasks /Create failed with errorlevel !errorlevel!
echo        usual causes: existing task with /F not honoured, policy restrictions
endlocal & exit /b 1
