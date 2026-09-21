@echo off
REM =====================================================================
REM  11/ unit tests: compile + run T41~T49
REM    T41 comm & role boundary      T42 data acquisition stability
REM    T43 command & state sync      T44 fault trigger & recovery
REM    T45 soak 24h (86400 ticks)    T46 logging end-to-end (P2 SOE)
REM    T47 concurrent read/write     T48 BMS charge/discharge forbid
REM    T49 grid power single source (meter reading)
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === compile RT_DB C library (gcc) ===
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

echo === compile + run 11/ system integration tests ===
g++ -std=c++17 -Wall -O2 ^
    -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\08\src ^
    -I ..\P2\src -I ..\07\src\rtdb -I ..\07\vendor\rt_db ^
    tests\test_system_integration.cpp build\rt_db_api.o build\ems_point_table.o build\ems_rt_db_setup.o ^
    -o build\test_system_integration.exe
if errorlevel 1 (
    echo [FAIL] compile error
    exit /b 1
)

build\test_system_integration.exe
set RC=%errorlevel%

if %RC% == 0 (
    echo.
    echo === ALL TESTS PASSED ===
) else (
    echo.
    echo === TESTS FAILED ===
)

endlocal & exit /b %RC%
