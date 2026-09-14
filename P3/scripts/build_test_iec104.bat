@echo off
REM =====================================================================
REM P3/ IEC 60870-5-104 adapter tests: T41-T46
REM   APCI byte-exact / ASDU byte-exact / session + k-w windows /
REM   closed-loop equivalence / comm interruption / map contract
REM NOTE: intentionally ASCII-only.
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === [1/2] compile ems_point_table.c (gcc) ===
gcc -std=c11 -O2 -I ..\07\src\rtdb -c ..\07\src\rtdb\ems_point_table.c -o build\ems_point_table.o
if errorlevel 1 exit /b 1
if not exist build\ems_point_table.o exit /b 1

echo === [2/2] compile and run P3/ IEC104 tests ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\07\src\rtdb -I ..\08\src ^
    tests\test_iec104.cpp build\ems_point_table.o -o build\test_iec104.exe
if errorlevel 1 exit /b 1

build\test_iec104.exe
set RC=%errorlevel%

if %RC% == 0 (
    echo.
    echo === ALL TESTS PASSED ===
) else (
    echo.
    echo === TESTS FAILED ===
)

endlocal & exit /b %RC%
