@echo off
REM =====================================================================
REM  19/ P5 现场部署：Windows 服务路线（**未落地，只给配方**） 
REM
REM  为什么不做成"一条命令就装成服务"： 
REM    把 **控制台程序** 用 sc.exe 注册成服务，SCM 会等它调用 
REM    StartServiceCtrlDispatcher + 报 SERVICE_RUNNING。不报，SCM 约 30 秒后
REM    直接杀掉进程；而 sc query 只显示 STOPPED / 反复重启 —— 
REM    现场看到的现象是"服务自己崩了"，实际是**根本没握手**。 
REM    Python 侧要握手就得引 pywin32（破坏零依赖，约定 §1）。 
REM
REM  什么时候该换这条路（满足任一条就该换）：
REM    ① 客户运维规范要求服务出现在 services.msc，且能用 net start/stop 管理； 
REM    ② 需要"服务失败自动恢复"的**系统级**策略与事件日志集成；
REM    ③ 需要以特定服务账号（而非 SYSTEM）运行并纳入服务权限审计。 
REM  换法二选一： 
REM    (a) 写一个 C++ 真服务壳（StartServiceCtrlDispatcher + CreateProcess 拉起
REM        ems_supervisor.py），壳本身零依赖，符合约定 §1； 
REM    (b) 用一个已实现 SCM 握手的包装器（第三方，不推荐，破坏零依赖）。 
REM
REM  用法： 
REM    set EMS_SERVICE_WRAPPER=D:\EMS\bin\ems_service_host.exe
REM    install_service.bat                 安装（需要已设 EMS_SERVICE_WRAPPER） 
REM    install_service.bat --dry-run        只打印命令 
REM    install_service.bat --recipe         只打印配方与前置条件（默认也是这个）
REM
REM  退出码：0 成功 / 1 用法或权限错误 / 2 未安装（缺 SCM 包装器）
REM =====================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0\..\.."
set "ROOT=!CD!"
set "SVCNAME=EMS_Site_P5"
set "SUP=!ROOT!\19\src\ems_supervisor.py"
set "DRY=0"

if /I "%~1"=="--dry-run" set "DRY=1"
if /I "%~1"=="--recipe" goto recipe

set "WRAP=%EMS_SERVICE_WRAPPER%"

if "!WRAP!"=="" goto recipe
if not exist "!WRAP!" goto wrapper_missing

REM  包装器约定：它自己负责 SCM 握手，并把下面这条命令拉起为子进程。 
set "SVCBIN=\"!WRAP!\" --python \"!ROOT!\13\.venv\Scripts\python.exe\" --script \"!SUP!\" --arg --root --arg \"!ROOT!\" --arg run"

if "!DRY%"=="1" goto dry_run_svc

net session >nul 2>&1
if errorlevel 1 goto need_admin

echo [EXEC] sc.exe create !SVCNAME!
sc.exe create "!SVCNAME!" binPath= "!SVCBIN!" start= auto DisplayName= "EMS Site (P5)"
if errorlevel 1 goto sc_failed
sc.exe description "!SVCNAME!" "EMS site deployment supervisor (19/)"
echo.
echo [OK] service registered: !SVCNAME!
echo      verify : sc.exe query !SVCNAME!
echo      start  : sc.exe start !SVCNAME!
echo      remove : 19\scripts\uninstall_service.bat
endlocal & exit /b 0

:dry_run_svc
echo.
echo [DRY-RUN] would run:
echo   sc.exe create !SVCNAME! binPath= "!SVCBIN!" start= auto
echo.
echo [OK] dry-run: nothing was changed
endlocal & exit /b 0

:recipe
echo ============================================
echo  P5 Windows service route: NOT INSTALLED
echo  install root: !ROOT!
echo ============================================
echo.
echo This script deliberately refuses to run without an SCM-capable wrapper.
echo Reason: a console program registered via sc.exe never reports
echo SERVICE_RUNNING, so the SCM kills it after ~30 s.
echo.
echo To adopt the service route, do ONE of:
echo   (a) build a C++ service host that calls StartServiceCtrlDispatcher and
echo       spawns:  python ^<root^>\19\src\ems_supervisor.py --root ^<root^> run
echo   (b) install a wrapper that implements the SCM handshake
echo       (third party - breaks the zero-dependency rule, not recommended)
echo.
echo Then set:
echo   set EMS_SERVICE_WRAPPER=^<path to wrapper exe^>
echo   set EMS_PYTHON=^<path to python.exe^>
echo   install_service.bat
echo.
echo Recommended alternative that needs none of the above:
echo   19\scripts\install_task.bat
echo   (Task Scheduler: start-at-boot, SYSTEM account, console hidden)
echo.
echo [SKIP] service route not installed; use install_task.bat instead
endlocal & exit /b 2

:wrapper_missing
echo [FAIL] EMS_SERVICE_WRAPPER points to a missing file: !WRAP!
endlocal & exit /b 1

:need_admin
echo [FAIL] administrator rights required
endlocal & exit /b 1

:sc_failed
echo [FAIL] sc.exe create failed with errorlevel !errorlevel!
endlocal & exit /b 1
