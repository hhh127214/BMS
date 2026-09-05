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
rem
rem  Usage: scripts\build_all.bat
rem         scripts\build_all.bat --no-test    skip 02/tests + 04/tests
rem ====================================================================

setlocal
set BUILD_TESTS=1
if /I "%1"=="--no-test" set BUILD_TESTS=0

pushd "%~dp0.."

set FAIL=0

echo ============================================
echo 1/9  01\  B组 strategy server (gcc)
echo ============================================
call 01\scripts\build.bat || set FAIL=1

echo.
echo ============================================
echo 2/9  02\  A组 safety mgr demo (g++)
echo ============================================
call 02\scripts\build.bat || set FAIL=1

if "%BUILD_TESTS%"=="1" (
    echo.
    echo ============================================
    echo 3/9  02\  A组 safety unit tests (g++)
    echo ============================================
    call 02\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 4/9  03\anti_reverse_controller\
echo ============================================
call "03\anti_reverse_controller\scripts\build.bat" || set FAIL=1

echo.
echo ============================================
echo 5/9  03\pv_smoothing_controller\
echo ============================================
call "03\pv_smoothing_controller\scripts\build.bat" || set FAIL=1

echo.
echo ============================================
echo 6/9  03\demand_management_controller\
echo ============================================
call "03\demand_management_controller\scripts\build.bat" || set FAIL=1

echo.
echo ============================================
echo 7/9  03\integration\
echo ============================================
call 03\integration\scripts\build.bat || set FAIL=1

echo.
echo ============================================
echo 8/9  04\ 策略管理层 + 仲裁器 (g++)
echo ============================================
call 04\scripts\build.bat || set FAIL=1

if "%BUILD_TESTS%"=="1" (
    echo.
    echo ============================================
    echo 9/9  04\ 单元测试 (g++)
    echo ============================================
    call 04\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
if "%FAIL%"=="1" (
    echo [BUILD ALL FAIL] Some component failed. Check output above.
    popd
    endlocal
    exit /b 1
) else (
    echo [BUILD ALL OK] All 9 components built.
    popd
    endlocal
    exit /b 0
)
