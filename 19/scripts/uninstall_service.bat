@echo off
REM =====================================================================
REM  19/ P5 现场部署：卸载 Windows 服务（与 install_service.bat 配套） 
REM
REM  用法： 
REM    uninstall_service.bat            停止并删除服务 
REM    uninstall_service.bat --dry-run   只打印命令 
REM  退出码：0 成功或本来就没装 / 1 权限或 sc.exe 失败
REM =====================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0\..\.."
set "ROOT=!CD!"
set "SVCNAME=EMS_Site_P5"
set "DRY=0"
if /I "%~1"=="--dry-run" set "DRY=1"

echo ============================================
echo  P5 uninstall service: !SVCNAME!
echo ============================================

sc.exe query "!SVCNAME!" >nul 2>&1
if errorlevel 1 goto not_installed

if "!DRY%"=="1" goto dry_run_del

net session >nul 2>&1
if errorlevel 1 goto need_admin

echo [1/2] stop service
sc.exe stop "!SVCNAME!" >nul 2>&1
echo [2/2] delete service
sc.exe delete "!SVCNAME!"
if errorlevel 1 goto delete_failed

echo.
echo [OK] service removed: !SVCNAME!
endlocal & exit /b 0

:dry_run_del
echo.
echo [DRY-RUN] would run:
echo   sc.exe stop   !SVCNAME!
echo   sc.exe delete !SVCNAME!
echo.
echo [OK] dry-run: nothing was changed
endlocal & exit /b 0

:not_installed
echo [SKIP] service not installed: !SVCNAME!  (nothing to do)
endlocal & exit /b 0

:need_admin
echo [FAIL] administrator rights required
endlocal & exit /b 1

:delete_failed
echo [FAIL] sc.exe delete failed with errorlevel !errorlevel!
endlocal & exit /b 1
