@echo off
rem ====================================================================
rem  Build all C / C++ components of the BMS project.
rem
rem  Components built (in order):
rem    01\  B组  strategy server          (gcc,  src/*.c)
rem    02\  A组  safety constraint mgr    (g++,  src/*.cpp)
rem    02\        safety tests             (g++,  tests/*.cpp)
rem    03/anti_reverse_controller/         anti-reverse controller
rem    03/pv_smoothing_controller/         pv smoothing controller
rem    03/demand_management_controller/    demand controller
rem    03/integration/                     three-controller integration
rem    04\  策略管理层 + 仲裁器           (g++,  src/*.cpp + src/*.h)
rem    04\        单元测试                (g++,  tests/*.cpp)
rem    05\  周期 5：安全约束引擎          (g++,  src/*.h)
rem    06\  周期 6：EMS 状态机            (g++,  src/*.h)
rem    07\  周期 7：实时控制闭环          (g++,  src/*.h)
rem    07\  产品化 P0/P0.5 设备 I/O 抽象   (g++,  src/device_io.h + sim/memory 适配器)
rem    08\  周期 8：优化调度与实时协同     (g++,  src/*.h)
rem    05..08 各带单元测试                 (g++,  tests/*.cpp)
rem    09\  周期9 多策略组合测试(闭环时序级) (g++,  tests/*.cpp)
rem
rem  Usage: scripts\build_all.bat
rem         scripts\build_all.bat --no-test    skip all */tests builds
rem ====================================================================

setlocal
set BUILD_TESTS=1
if /I "%1"=="--no-test" set BUILD_TESTS=0

pushd "%~dp0.."

set FAIL=0

echo.
echo ============================================
echo 1/19 01\  B组 strategy server (gcc)
echo ============================================
call 01\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 2/19 02\  A组 safety mgr demo (g++)
echo ============================================
call 02\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 3/19 02\  A组 safety unit tests (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 02\scripts\build_test.bat || set FAIL=1
)
echo.
echo ============================================
echo 4/19 03\anti_reverse_controller\
echo ============================================
call "03\anti_reverse_controller\scripts\build.bat" || set FAIL=1
echo.
echo ============================================
echo 5/19 03\pv_smoothing_controller\
echo ============================================
call "03\pv_smoothing_controller\scripts\build.bat" || set FAIL=1
echo.
echo ============================================
echo 6/19 03\demand_management_controller\
echo ============================================
call "03\demand_management_controller\scripts\build.bat" || set FAIL=1
echo.
echo ============================================
echo 7/19 03\integration\
echo ============================================
call 03\integration\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 8/19 04\ 策略管理层 + 仲裁器 (g++)
echo ============================================
call 04\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 9/19 04\ 单元测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 04\scripts\build_test.bat || set FAIL=1
)
echo.
echo ============================================
echo 10/19 05\ 周期5 安全约束引擎 (g++)
echo ============================================
call 05\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 11/19 05\ 单元测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 05\scripts\build_test.bat || set FAIL=1
)
echo.
echo ============================================
echo 12/19 06\ 周期6 EMS 状态机 (g++)
echo ============================================
call 06\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 13/19 06\ 单元测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 06\scripts\build_test.bat || set FAIL=1
)
echo.
echo ============================================
echo 14/19 07\ 周期7 实时控制闭环 (g++)
echo ============================================
call 07\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 15/19 07\ 单元测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 07\scripts\build_test.bat || set FAIL=1
)
echo.
echo ============================================
echo 16/19 08\ 周期8 优化调度与实时控制协同 (g++)
echo ============================================
call 08\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 17/19 08\ 单元测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 08\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 18/19 07\ 产品化 P0/P0.5 设备 I/O 抽象测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 07\scripts\build_test_device_io.bat || set FAIL=1
)

echo.
echo ============================================
echo 19/19 09\ 周期9 多策略组合测试 (闭环时序级) (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 09\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
if "%FAIL%"=="1" (
    echo [BUILD ALL FAIL] Some component failed. Check output above.
    popd
    endlocal
    exit /b 1
) else (
    echo [BUILD ALL OK] All 19 components built.
    popd
    endlocal
    exit /b 0
)
