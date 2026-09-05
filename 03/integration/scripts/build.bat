@echo off
rem ====================================================================
rem  Build the three-controller integration simulator.
rem
rem  Cross-directory includes — all controller headers resolved via -I
rem  (avoids MinGW + non-ASCII path #include 限制).
rem
rem  Sources:
rem    src\IntegrationMain.cpp
rem    ..\anti_reverse_controller\src\AntiReverseController.cpp
rem    ..\pv_smoothing_controller\src\SmoothingController.cpp
rem    ..\demand_management_controller\src\DemandController.cpp
rem
rem  Output: build\integration_sim.exe
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

echo Compiling three-controller integration ...
g++ -std=c++17 -O2 -Wall -Wextra ^
    -I "..\shared" ^
    -I "..\anti_reverse_controller\src" ^
    -I "..\pv_smoothing_controller\src" ^
    -I "..\demand_management_controller\src" ^
    src\IntegrationMain.cpp ^
    "..\anti_reverse_controller\src\AntiReverseController.cpp" ^
    "..\pv_smoothing_controller\src\SmoothingController.cpp" ^
    "..\demand_management_controller\src\DemandController.cpp" ^
    -o build\integration_sim.exe

if errorlevel 1 (
    echo [BUILD FAIL] g++ returned non-zero.
    popd
    exit /b 1
)

echo [BUILD OK] build\integration_sim.exe
popd
endlocal
