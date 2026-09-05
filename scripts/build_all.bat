@echo off
rem ====================================================================
rem  Build all C / C++ components of the BMS project.
rem
rem  Components built (in order):
rem    01\  B组  strategy server          (gcc,  src/*.c)
rem    02\  A组  safety constraint mgr    (g++,  src/*.cpp)
rem    02\        safety tests             (g++,  tests/*.cpp)
rem    03/防逆流控制器/                  anti-reverse controller
rem    03/光伏出力平抑控制器/            pv smoothing controller
rem    03/需量管理控制器/                demand controller
rem    03/integration/                    three-controller integration
rem
rem  Usage: scripts\build_all.bat
rem         scripts\build_all.bat --no-test    skip 02/tests
rem ====================================================================

setlocal
set BUILD_TESTS=1
if /I "%1"=="--no-test" set BUILD_TESTS=0

pushd "%~dp0.."

set FAIL=0

echo ============================================
echo 1/7  01\  B组 strategy server (gcc)
echo ============================================
call 01\scripts\build.bat || set FAIL=1

echo.
echo ============================================
echo 2/7  02\  A组 safety mgr demo (g++)
echo ============================================
call 02\scripts\build.bat || set FAIL=1

if "%BUILD_TESTS%"=="1" (
    echo.
    echo ============================================
    echo 3/7  02\  A组 safety unit tests (g++)
    echo ============================================
    call 02\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 4/7  03\防逆流控制器\
echo ============================================
call "03\防逆流控制器\scripts\build.bat" || set FAIL=1

echo.
echo ============================================
echo 5/7  03\光伏出力平抑控制器\
echo ============================================
call "03\光伏出力平抑控制器\scripts\build.bat" || set FAIL=1

echo.
echo ============================================
echo 6/7  03\需量管理控制器\
echo ============================================
call "03\需量管理控制器\scripts\build.bat" || set FAIL=1

echo.
echo ============================================
echo 7/7  03\integration\
echo ============================================
call 03\integration\scripts\build.bat || set FAIL=1

echo.
echo ============================================
if "%FAIL%"=="1" (
    echo [BUILD ALL FAIL] Some component failed. Check output above.
    popd
    endlocal
    exit /b 1
) else (
    echo [BUILD ALL OK] All 7 components built.
    popd
    endlocal
    exit /b 0
)
