@echo off
rem ====================================================================
rem  Build the pure-C battery optimizer service (B 组 C 策略优化服务)
rem  All sources live in src/, build artefact goes to ../build/
rem
rem  Usage: scripts\build.bat
rem  Prereq: gcc (MinGW / MSYS2). Tested with gcc 13.x.
rem ====================================================================

setlocal

rem Move to the directory of this script, then up to module root.
pushd "%~dp0.."

if not exist build mkdir build

where gcc >nul 2>&1
if errorlevel 1 (
    echo [BUILD FAIL] gcc not found in PATH. Install MinGW and retry.
    popd
    exit /b 1
)

echo Compiling src\*.c ...
gcc -O2 -Wall -Wextra -o build\battery_server.exe ^
    src\main.c src\json_util.c src\solver.c src\lp.c src\milp.c ^
    -lws2_32

if errorlevel 1 (
    echo [BUILD FAIL] gcc returned non-zero.
    popd
    exit /b 1
)

echo [BUILD OK] build\battery_server.exe
popd
endlocal
