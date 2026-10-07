@echo off
REM =====================================================================
REM  15/ 平台界面：一键（静态契约自检） 
REM   供 build_all.bat 调用；也可单独跑。 
REM
REM  15/ 没有编译步骤（纯静态、零打包），所以这里只有自检。 
REM =====================================================================

setlocal
cd /d "%~dp0\.."

set FAIL=0

call scripts\run_selftest.bat || set FAIL=1

if "%FAIL%"=="1" (
    echo [15 FAIL] 平台界面静态契约自检未通过
    endlocal
    exit /b 1
)
echo [15 OK] 平台界面就绪（静态契约自检通过） 
endlocal
