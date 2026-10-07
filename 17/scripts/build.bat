@echo off
REM =====================================================================
REM  17\ build —— 只编译，不运行（跑测试请用 scripts\build_test.bat） 
REM
REM  产物（全部落 build\，不入库）：
REM    build\ems_point_table.o     07\ 点表真相源的 C 实现（与 07/11/13 共用同一个 .o） 
REM    build\rt_db_api.o           RT_DB 共享内存 API（vendor） 
REM    build\ems_rt_db_setup.o     EMS 段建立/初始化 
REM    build\test_point_table.exe  两层点表结构（C4） 
REM    build\test_point_config.exe 点表配置化 JSON 往返（C5） 
REM    build\test_sim_meter.exe    电表模拟器（B4） 
REM    build\test_sim_bms.exe      BMS 模拟器（B5） 
REM    build\test_sim_pcs.exe      PCS 模拟器（B3） 
REM    build\test_scale_rtdb.exe   分级节拍 + 死区 + 批量写（C1/C2/C3） 
REM
REM  本模块**零第三方依赖**：不需要 Python / pymodbus / Node / lib60870。 
REM  依赖只有 07\ 的点表与 vendor 的 RT_DB（都是仓库内源码）。 
REM
REM  ★ 三份点表要对账（D3 纪律）：
REM    07\src\rtdb\ems_point_table.*   EMS 抽象视图（40 点，算法契约） 
REM    17\src\device_point_table.h     设备全点表（107 点，忠于厂家） 
REM    17\src\point_mapping.h          两层之间的显式映射（DIRECT/COMPOSED/SINK/EXTERNAL） 
REM    新增点表时必须同时扩 self_check()，否则"点名逐字一致"这条硬约束无人守。 
REM =====================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0\.."

if not exist build mkdir build

REM ---------- [1/2] C 目标文件 ----------
REM  ★ 无条件重编：点表头文件变了而 .o 不重编 =「同名点表两套内容」，
REM    编译期不报错、运行期按名字/索引取点错位。 
echo === [1/2] C objects: ems_point_table / rt_db_api / ems_rt_db_setup ===
gcc -std=c11 -O2 -I ..\07\src\rtdb -I ..\07\vendor\rt_db ^
    -c ..\07\src\rtdb\ems_point_table.c -o build\ems_point_table.o
if errorlevel 1 ( echo [FAIL] ems_point_table.c compile error & endlocal & exit /b 1 )

gcc -std=c11 -O2 -I ..\07\vendor\rt_db ^
    -c ..\07\vendor\rt_db\rt_db_api.c -o build\rt_db_api.o
if errorlevel 1 ( echo [FAIL] rt_db_api.c compile error & endlocal & exit /b 1 )

gcc -std=c11 -O2 -I ..\07\src\rtdb -I ..\07\vendor\rt_db ^
    -c ..\07\src\rtdb\ems_rt_db_setup.c -o build\ems_rt_db_setup.o
if errorlevel 1 ( echo [FAIL] ems_rt_db_setup.c compile error & endlocal & exit /b 1 )

REM ---------- [2/2] 六个测试可执行文件 ----------
set "INC=-I src -I tests -I ..\07\src\rtdb -I ..\07\vendor\rt_db"
set "LNK=build\ems_point_table.o"
set "LNK_RTDB=build\ems_point_table.o build\rt_db_api.o build\ems_rt_db_setup.o"

echo.
echo === [2/2] test executables ===
call :cc test_point_table  tests\test_point_table.cpp
if errorlevel 1 endlocal & exit /b 1
call :cc test_point_config tests\test_point_config.cpp
if errorlevel 1 endlocal & exit /b 1
call :cc test_sim_meter    tests\test_sim_meter.cpp
if errorlevel 1 endlocal & exit /b 1
call :cc test_sim_bms      tests\test_sim_bms.cpp
if errorlevel 1 endlocal & exit /b 1
call :cc test_sim_pcs      tests\test_sim_pcs.cpp
if errorlevel 1 endlocal & exit /b 1
call :cc test_scale_rtdb   tests\test_scale_rtdb.cpp rtdb
if errorlevel 1 endlocal & exit /b 1

echo.
echo === BUILD OK ===
echo   run tests: scripts\build_test.bat
endlocal & exit /b 0


REM =====================================================================
REM  :cc —— 编译一个测试（%1 名 / %2 源码 / %3 为 rtdb 时链 RT_DB 三 .o） 
REM  ★ 这里**不能** endlocal：会连带关掉调用方的延迟展开； 
REM    失败靠 exit /b 1 回给调用方，由调用方 endlocal 并退出脚本。 
REM =====================================================================
:cc
set "TNAME=%~1"
set "TSRC=%~2"
set "TLNK=!LNK!"
if /i "%~3"=="rtdb" set "TLNK=!LNK_RTDB!"
g++ -std=c++17 -Wall -O2 !INC! "!TSRC!" !TLNK! -o "build\!TNAME!.exe"
if errorlevel 1 (
    echo [FAIL] !TNAME! compile error
    exit /b 1
)
echo    ok  build\!TNAME!.exe
exit /b 0
