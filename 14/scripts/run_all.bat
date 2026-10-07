@echo off
REM =====================================================================
REM 14/ 平台层：一键（导入 + 自检） 
REM   供 build_all.bat 调用；也可单独跑。 
REM =====================================================================

setlocal
cd /d "%~dp0\.."

set PY=%EMS_PYTHON%
if "%PY%"=="" set PY=python

set FAIL=0

call scripts\import_sim.bat || set FAIL=1
call scripts\run_selftest.bat || set FAIL=1

if "%FAIL%"=="1" (
    echo [14 FAIL] 平台层有步骤失败
    endlocal
    exit /b 1
)
echo [14 OK] 平台层就绪（数据库 + 接口自检均通过） 
endlocal
