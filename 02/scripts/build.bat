@echo off
rem ====================================================================
rem  Build A组（安全与保护）约束管理模块 (C++17)
rem  Source under src/, output under build/.
rem
rem  Usage: scripts\build.bat
rem  Prereq: g++ (MinGW-w64 / MSYS2). Tested with g++ 13.x.
rem ====================================================================

setlocal
pushd "%~dp0.."

if not exist build mkdir build

where g++ >nul 2>&1
if errorlevel 1 (
    echo [BUILD FAIL] g++ not found in PATH. Install MinGW-w64 and retry.
    popd
    exit /b 1
)

echo Compiling src\safety_constraint_manager.cpp ...
g++ -std=c++17 -O2 -Wall -Wextra -o build\safety_constraint_manager.exe src\safety_constraint_manager.cpp
if errorlevel 1 (
    echo [BUILD FAIL] g++ returned non-zero.
    popd
    exit /b 1
)

echo [BUILD OK] build\safety_constraint_manager.exe
popd
endlocal
