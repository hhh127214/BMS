@echo off
rem ====================================================================
rem  Build the three-controller integration simulator.
rem
rem  Cross-directory includes — all controller headers resolved via -I
rem  (avoids MinGW + Chinese-path #include 限制).
rem
rem  Sources:
rem    src\IntegrationMain.cpp
rem    ..\防逆流控制器\src\AntiReverseController.cpp
rem    ..\光伏出力平抑控制器\src\SmoothingController.cpp
rem    ..\需量管理控制器\src\DemandController.cpp
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
    -I "..\防逆流控制器\src" ^
    -I "..\光伏出力平抑控制器\src" ^
    -I "..\需量管理控制器\src" ^
    src\IntegrationMain.cpp ^
    "..\防逆流控制器\src\AntiReverseController.cpp" ^
    "..\光伏出力平抑控制器\src\SmoothingController.cpp" ^
    "..\需量管理控制器\src\DemandController.cpp" ^
    -o build\integration_sim.exe

if errorlevel 1 (
    echo [BUILD FAIL] g++ returned non-zero.
    popd
    exit /b 1
)

echo [BUILD OK] build\integration_sim.exe
popd
endlocal
