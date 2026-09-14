@echo off
REM =====================================================================
REM P3/ communication adapters - demo build (scenario F/G/H)
REM NOTE: intentionally ASCII-only (see CHANGES.md 18.x for the reason).
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === [1/2] compile ems_point_table.c (gcc) ===
gcc -std=c11 -O2 -I ..\07\src\rtdb -c ..\07\src\rtdb\ems_point_table.c -o build\ems_point_table.o
if errorlevel 1 exit /b 1

echo === [2/2] compile P3/ demo ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\07\src\rtdb -I ..\08\src ^
    src\main.cpp build\ems_point_table.o -o build\ems_comms.exe
if errorlevel 1 exit /b 1

echo.
echo === BUILD OK: build\ems_comms.exe ===

endlocal & exit /b 0
