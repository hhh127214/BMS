@echo off
rem ====================================================================
rem  Build the pv_smoothing_controller module.
rem
rem  Sources: src\SmoothingController.cpp
rem  Tests:   tests\main_smoothing_csv.cpp
rem
rem  Output: build\smoothing_sim.exe
rem ====================================================================

setlocal
pushd "%~dp0.."

if not exist build mkdir build

where g++ >nul 2>&1
if errorlevel 1 (
    echo [BUILD FAIL] g++ not found in PATH.
    popd
    exit /b 1
)

echo Compiling ...
g++ -std=c++17 -O2 -Wall -Wextra ^
    -I "..\shared" ^
    -I "src" ^
    tests\main_smoothing_csv.cpp src\SmoothingController.cpp ^
    -o build\smoothing_sim.exe

if errorlevel 1 (
    echo [BUILD FAIL] g++ returned non-zero.
    popd
    exit /b 1
)

echo [BUILD OK] build\smoothing_sim.exe
popd
endlocal
