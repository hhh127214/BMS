@echo off
REM =====================================================================
REM P3/ Modbus adapter tests: T31-T36
REM   frames byte-exact / exception+stale / map contract /
REM   closed-loop equivalence / link loss / fault over registers
REM NOTE: this file is intentionally ASCII-only (see CHANGES.md 18.x).
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === [1/2] compile ems_point_table.c (gcc) ===
gcc -std=c11 -O2 -I ..\07\src\rtdb -c ..\07\src\rtdb\ems_point_table.c -o build\ems_point_table.o
if errorlevel 1 exit /b 1
if not exist build\ems_point_table.o exit /b 1

echo === [2/2] compile and run P3/ Modbus tests ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\07\src\rtdb -I ..\08\src ^
    tests\test_modbus.cpp build\ems_point_table.o -o build\test_modbus.exe
if errorlevel 1 exit /b 1

build\test_modbus.exe
set RC=%errorlevel%

if %RC% == 0 (
    echo.
    echo === ALL TESTS PASSED ===
) else (
    echo.
    echo === TESTS FAILED ===
)

endlocal & exit /b %RC%
