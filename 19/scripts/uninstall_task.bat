@echo off
REM =====================================================================
REM  19/ P5 现场部署：卸载开机自启（Windows 计划任务） 
REM
REM  卸载前**先停守护器**，再删任务 —— 顺序反了的话，任务虽然没了，
REM  守护器进程还活着并继续守着三个 worker（现场表现为"停不掉"）。 
REM
REM  用法： 
REM    uninstall_task.bat            停止并删除任务 
REM    uninstall_task.bat --dry-run   只打印将要执行的命令，不改系统 
REM
REM  ★ 与 install_task.bat 同一条纪律：CRLF、含 !VAR! 的行纯 ASCII、 
REM    分支用 goto 标签而不是 if ( ) 块。 
REM =====================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0\..\.."
set "ROOT=!CD!"
set "TASKNAME=EMS_Site_P5"
set "DRY=0"
if /I "%~1"=="--dry-run" set "DRY=1"

echo ============================================
echo  P5 uninstall scheduled task: !TASKNAME!
echo ============================================

REM  --dry-run 必须在**任何 schtasks 调用之前**返回：现场演练时
REM  不该因为"查一下在不在"就碰系统状态（也避开受限环境里的黑名单）。 
if "!DRY!"=="1" goto dry_run_del

where schtasks >nul 2>&1
if errorlevel 1 goto no_schtasks

schtasks /Query /TN "!TASKNAME!" >nul 2>&1
if errorlevel 1 goto not_installed

net session >nul 2>&1
if errorlevel 1 goto need_admin

echo [1/2] stop running instance
schtasks /End /TN "!TASKNAME!" >nul 2>&1
REM  守护器收到终止后会把三个 worker 一起收掉（见 ems_supervisor 的 shutdown） 
echo [2/2] delete task
schtasks /Delete /TN "!TASKNAME!" /F
if errorlevel 1 goto delete_failed

echo.
echo [OK] scheduled task removed: !TASKNAME!
echo      leftover config/logs were kept: !ROOT!\conf , !ROOT!\log
endlocal & exit /b 0

:dry_run_del
echo.
echo [DRY-RUN] would run:
echo   schtasks /End    /TN !TASKNAME!
echo   schtasks /Delete /TN !TASKNAME! /F
echo.
echo [OK] dry-run: nothing was changed
endlocal & exit /b 0

:not_installed
echo [SKIP] task not installed: !TASKNAME!  (nothing to do)
endlocal & exit /b 0

:no_schtasks
echo [FAIL] schtasks.exe not available on PATH
echo        this host cannot manage scheduled tasks; check PATH / policy
endlocal & exit /b 1

:need_admin
echo [FAIL] administrator rights required
endlocal & exit /b 1

:delete_failed
echo [FAIL] schtasks /Delete failed with errorlevel !errorlevel!
endlocal & exit /b 1
