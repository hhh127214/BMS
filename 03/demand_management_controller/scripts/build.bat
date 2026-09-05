@echo off
rem ====================================================================
rem  Build the demand_management_controller module.
rem
rem  Sources: src\DemandController.cpp
rem  Tests:   tests\main_demand_csv.cpp + tests\demand_test.cpp
rem
rem  Outputs:
rem    build\demand_sim.exe      — CSV-driven simulator
rem    build\demand_test.exe     — unit tests
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

echo Compiling src\DemandController.cpp ...
g++ -std=c++17 -O2 -Wall -Wextra ^
    -I "..\shared" ^
    -c src\DemandController.cpp -o build\DemandController.o
if errorlevel 1 ( echo [BUILD FAIL] src\ compile & popd & exit /b 1 )

echo Compiling tests\main_demand_csv.cpp ...
g++ -std=c++17 -O2 -Wall -Wextra ^
    -I "..\shared" -I "src" ^
    tests\main_demand_csv.cpp build\DemandController.o ^
    -o build\demand_sim.exe
if errorlevel 1 ( echo [BUILD FAIL] tests\main_demand_csv.cpp & popd & exit /b 1 )

echo Compiling tests\demand_test.cpp ...
g++ -std=c++17 -O2 -Wall -Wextra ^
    -I "..\shared" -I "src" ^
    tests\demand_test.cpp build\DemandController.o ^
    -o build\demand_test.exe
if errorlevel 1 ( echo [BUILD FAIL] tests\demand_test.cpp & popd & exit /b 1 )

echo [BUILD OK] build\demand_sim.exe + build\demand_test.exe
popd
endlocal
