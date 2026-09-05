@echo off
rem ====================================================================
rem  Build the anti_reverse_controller module.
rem
rem  Sources: src\AntiReverseController.cpp
rem  Tests:   tests\main.cpp (CLI simulator) + tests\test_ar.cpp (unit)
rem
rem  Outputs:
rem    build\ar_sim.exe       — interactive simulator (uses sim_sc*.csv)
rem    build\test_ar.exe      — unit tests
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

echo Compiling src\*.cpp ...
g++ -std=c++17 -O2 -Wall -Wextra ^
    -I "..\shared" ^
    -c src\AntiReverseController.cpp ^
    -o build\AntiReverseController.o
if errorlevel 1 ( echo [BUILD FAIL] src\ compile & popd & exit /b 1 )

echo Compiling tests\main.cpp ...
g++ -std=c++17 -O2 -Wall -Wextra ^
    -I "..\shared" ^
    -I "src" ^
    tests\main.cpp build\AntiReverseController.o ^
    -o build\ar_sim.exe
if errorlevel 1 ( echo [BUILD FAIL] tests\main.cpp & popd & exit /b 1 )

echo Compiling tests\test_ar.cpp ...
g++ -std=c++17 -O2 -Wall -Wextra ^
    -I "..\shared" ^
    -I "src" ^
    tests\test_ar.cpp build\AntiReverseController.o ^
    -o build\test_ar.exe
if errorlevel 1 ( echo [BUILD FAIL] tests\test_ar.cpp & popd & exit /b 1 )

echo [BUILD OK] build\ar_sim.exe + build\test_ar.exe
popd
endlocal
