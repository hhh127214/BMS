@echo off
REM =====================================================================
REM  11/ build: compile the three integration processes
REM   rtdb_initializer.exe  (segment creator + holder)
REM   device_side.exe       (device-side process, 6 data sources)
REM   ems_side.exe          (EMS-side process, full stack)
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === [1/4] compile RT_DB C library (gcc) ===
gcc -std=c11 -O2 -D__USE_MINGW_ANSI_STDIO=1 -I ..\07\vendor\rt_db -c ..\07\vendor\rt_db\rt_db_api.c -o build\rt_db_api.o
if errorlevel 1 (
    echo [FAIL] rt_db_api.c compile error
    exit /b 1
)

gcc -std=c11 -O2 -I ..\07\src\rtdb -I ..\07\vendor\rt_db -c ..\07\src\rtdb\ems_point_table.c -o build\ems_point_table.o
if errorlevel 1 (
    echo [FAIL] ems_point_table.c compile error
    exit /b 1
)

gcc -std=c11 -O2 -I ..\07\src\rtdb -I ..\07\vendor\rt_db -c ..\07\src\rtdb\ems_rt_db_setup.c -o build\ems_rt_db_setup.o
if errorlevel 1 (
    echo [FAIL] ems_rt_db_setup.c compile error
    exit /b 1
)

set INC=-I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\08\src -I ..\P2\src -I ..\07\src\rtdb -I ..\07\vendor\rt_db
set RTDBOBJS=build\rt_db_api.o build\ems_point_table.o build\ems_rt_db_setup.o

echo === [2/4] rtdb_initializer.exe ===
g++ -std=c++17 -Wall -O2 -I src -I ..\07\src\rtdb -I ..\07\vendor\rt_db ^
    src\main_initializer.cpp %RTDBOBJS% -o build\rtdb_initializer.exe
if errorlevel 1 (
    echo [FAIL] rtdb_initializer compile error
    exit /b 1
)

echo === [3/4] device_side.exe ===
g++ -std=c++17 -Wall -O2 %INC% ^
    src\main_device.cpp %RTDBOBJS% -o build\device_side.exe
if errorlevel 1 (
    echo [FAIL] device_side compile error
    exit /b 1
)

echo === [4/4] ems_side.exe ===
g++ -std=c++17 -Wall -O2 %INC% ^
    src\main_ems.cpp %RTDBOBJS% -o build\ems_side.exe
if errorlevel 1 (
    echo [FAIL] ems_side compile error
    exit /b 1
)

echo.
echo === BUILD OK ===
echo   build\rtdb_initializer.exe   segment creator, holds for N seconds
echo   build\device_side.exe        device process  [--steps N --dt 0.1 --speed K --fault kind:t1:t2]
echo   build\ems_side.exe           EMS process     [--steps N --dt 0.1 --speed K --log-every K]
echo   (both workers must use the SAME --speed, see integration_runner.h / Pacing)
echo   run scripts\run_integration.bat for the 3-process demo

endlocal & exit /b 0
