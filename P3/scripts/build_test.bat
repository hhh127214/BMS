@echo off
REM =====================================================================
REM  P3\ build_test: IEC104 网关单元测试（T61~T72）+ EXT 落点（T73~T76） 
REM
REM  这是「库自带主站 ↔ 我们的从站」的自环测试：真实 TCP、真实报文、 
REM  真实 STARTDT / 总召唤 / 遥控时序。**不需要 RT_DB**（数据源走内存）。 
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

if not exist ..\vendor\lib60870\build\lib60870.a (
    echo === building lib60870 first ===
    call ..\vendor\lib60870\build.bat
    if errorlevel 1 ( echo [FAIL] lib60870 build & exit /b 1 )
)

set INC=-I src ^
 -I ..\vendor\lib60870\config ^
 -I ..\vendor\lib60870\src\inc\api ^
 -I ..\vendor\lib60870\src\inc\internal ^
 -I ..\vendor\lib60870\src\common\inc ^
 -I ..\vendor\lib60870\src\hal\inc ^
 -I ..\07\src\rtdb

REM 为什么测试也要链 ems_point_table.o： 
REM   iec104_point_map.h 的点名直接取自 07/ 的 EMS_POINT_NAMES（同一真相源，
REM   不允许在 P3 里再抄一份），符号定义在 ems_point_table.c 里。 
REM   不链就是 undefined reference to `EMS_POINT_NAMES`。 
REM 不需要 rt_db_api.o / ems_rt_db_setup.o —— 测试用 MemorySource，不碰共享内存。 
REM  ★ 无条件重编（理由见 build.bat：存在性判断会让点表扩容后的 .o 变陈旧）
gcc -std=c11 -O2 -I ..\07\src\rtdb -I ..\07\vendor\rt_db ^
    -c ..\07\src\rtdb\ems_point_table.c -o build\ems_point_table.o
if errorlevel 1 ( echo [FAIL] ems_point_table.c & exit /b 1 )

set FAIL=0

echo === [1/3] compiling test_iec104.exe (T61~T72) ===
g++ -std=c++17 -Wall -O2 -DNOMINMAX %INC% ^
    tests\test_iec104.cpp build\ems_point_table.o ..\vendor\lib60870\build\lib60870.a ^
    -lws2_32 -liphlpapi -lbcrypt -o build\test_iec104.exe
if errorlevel 1 (
    echo [FAIL] test_iec104 compile error
    exit /b 1
)
echo === [2/3] running test_iec104.exe ===
build\test_iec104.exe
if errorlevel 1 ( echo [FAIL] test_iec104 not passed & set FAIL=1 )

REM ---------- [3/3] EXT setpoint sink over RT_DB ----------
REM  Why a separate exe: test_iec104's data source is an in-memory stub,
REM  so it cannot prove that a value really landed on that point in the
REM  shared-memory segment.  This project got burned by exactly that in A1
REM  (cross-memory-boundary + fixture injection == a test blind spot).
REM  WARNING: this test calls ems_rt_db_setup(reset=true) -- it WIPES the
REM  whole RT_DB segment.  Make sure no initializer/gateway is using it.
echo.
echo === [3/3] test_ext_rtdb.exe (T73~T76 sink / publish protocol / fail-safe) ===
gcc -std=c11 -O2 -D__USE_MINGW_ANSI_STDIO=1 -I ..\07\vendor\rt_db ^
    -c ..\07\vendor\rt_db\rt_db_api.c -o build\rt_db_api.o
if errorlevel 1 ( echo [FAIL] rt_db_api.c compile error & exit /b 1 )
gcc -std=c11 -O2 -I ..\07\src\rtdb -I ..\07\vendor\rt_db ^
    -c ..\07\src\rtdb\ems_rt_db_setup.c -o build\ems_rt_db_setup.o
if errorlevel 1 ( echo [FAIL] ems_rt_db_setup.c compile error & exit /b 1 )
g++ -std=c++17 -Wall -O2 -DNOMINMAX %INC% -I ..\07\vendor\rt_db ^
    tests\test_ext_rtdb.cpp build\rt_db_api.o build\ems_point_table.o ^
    build\ems_rt_db_setup.o -lws2_32 -o build\test_ext_rtdb.exe
if errorlevel 1 ( echo [FAIL] test_ext_rtdb compile error & exit /b 1 )
build\test_ext_rtdb.exe
if errorlevel 1 ( echo [FAIL] test_ext_rtdb not passed & set FAIL=1 )

echo.
if "%FAIL%"=="1" ( echo [FAIL] P3 tests failed & endlocal & exit /b 1 )
echo [OK] P3 tests all passed (IEC104 protocol + EXT sink)

endlocal & exit /b 0
