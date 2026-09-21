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
rem    10\  周期10 EMS 24h 离线仿真测试 (g++,  tests/*.cpp)
rem    11\  周期11 系统级联调（三进程 + T41~T49） 
rem    12\  周期12 最终验收（七维度验收器 + T51~T59） 
rem    P1\  产品化 P1 配置化              (g++,  src/*.h + tests/*.cpp)
rem    P2\  产品化 P2 可观测性            (g++,  src/*.h + tests/*.cpp)
rem    vendor\lib60870                     IEC 60870-5-104 库（gcc, C99, 静态库） 
rem    P3\  产品化 P3 IEC104 从站网关    (g++,  src/*.h + src/*.cpp)
rem    P3\        网关单元测试 T61~T72   (g++,  tests/*.cpp)
rem    13\  Modbus TCP 设备接入：点表核对工具  (g++,  src/main_probe.cpp)
rem    13\        Modbus 三层测试 T01~T47      (g++,  tests/*.cpp)
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
echo 1/31 01\  B组 strategy server (gcc)
echo ============================================
call 01\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 2/31 02\  A组 safety mgr demo (g++)
echo ============================================
call 02\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 3/31 02\  A组 safety unit tests (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 02\scripts\build_test.bat || set FAIL=1
)
echo.
echo ============================================
echo 4/31 03\anti_reverse_controller\
echo ============================================
call "03\anti_reverse_controller\scripts\build.bat" || set FAIL=1
echo.
echo ============================================
echo 5/31 03\pv_smoothing_controller\
echo ============================================
call "03\pv_smoothing_controller\scripts\build.bat" || set FAIL=1
echo.
echo ============================================
echo 6/31 03\demand_management_controller\
echo ============================================
call "03\demand_management_controller\scripts\build.bat" || set FAIL=1
echo.
echo ============================================
echo 7/31 03\integration\
echo ============================================
call 03\integration\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 8/31 04\ 策略管理层 + 仲裁器 (g++)
echo ============================================
call 04\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 9/31 04\ 单元测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 04\scripts\build_test.bat || set FAIL=1
)
echo.
echo ============================================
echo 10/31 05\ 周期5 安全约束引擎 (g++)
echo ============================================
call 05\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 11/31 05\ 单元测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 05\scripts\build_test.bat || set FAIL=1
)
echo.
echo ============================================
echo 12/31 06\ 周期6 EMS 状态机 (g++)
echo ============================================
call 06\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 13/31 06\ 单元测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 06\scripts\build_test.bat || set FAIL=1
)
echo.
echo ============================================
echo 14/31 07\ 周期7 实时控制闭环 (g++)
echo ============================================
call 07\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 15/31 07\ 单元测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 07\scripts\build_test.bat || set FAIL=1
)
echo.
echo ============================================
echo 16/31 08\ 周期8 优化调度与实时控制协同 (g++)
echo ============================================
call 08\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 17/31 08\ 单元测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 08\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 18/31 07\ 产品化 P0/P0.5 设备 I/O 抽象测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 07\scripts\build_test_device_io.bat || set FAIL=1
)

echo.
echo ============================================
echo 19/31 07\ RT_DB 接入：共享内存适配器 + 点表自检 (g++/gcc)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 07\scripts\build_test_rtdb.bat || set FAIL=1
)

echo.
echo ============================================
echo 20/31 09\ 周期9 多策略组合测试 (闭环时序级) (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 09\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 21/31 10\ 周期10 EMS 24h 离线仿真测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 10\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 22/31 P1\ 产品化 P1 配置化 单元测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call P1\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 23/31 P2\ 产品化 P2 可观测性 单元测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call P2\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 24/31 11\ 周期11 系统级联调：三进程编译 (g++/gcc)
echo ============================================
call 11\scripts\build.bat || set FAIL=1

echo.
echo ============================================
echo 25/31 11\ 周期11 系统级联调：单元测试 T41~T49 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 11\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 26/31 12\ 周期12 最终验收：验收器编译 (g++/gcc)
echo ============================================
call 12\scripts\build.bat || set FAIL=1

echo.
echo ============================================
echo 27/31 12\ 周期12 最终验收：模块自测 T51~T59 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 12\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 28/31 vendor\lib60870 + P3\ IEC104 从站网关（g++/gcc） 
echo ============================================
rem  第三方库显式编一次：P3\scripts\build.bat 也会自己发现并补编，
rem  但写在这里能让失败停在正确的位置（而不是埋在 P3 里面）。 
call vendor\lib60870\build.bat || set FAIL=1
call P3\scripts\build.bat || set FAIL=1

echo.
echo ============================================
echo 29/31 P3\ IEC104 网关单元测试 T61~T72 + EXT 落点 T73~T76（g++） 
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call P3\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 30/31 13\ Modbus TCP 设备接入：点表核对工具（g++/gcc） 
echo ============================================
rem  本模块主体是**头文件**（modbus_tcp_client.h / modbus_point_map.h /
rem  modbus_device_io.h），由 07\ 装配层包含使用；probe 只是它的一个消费者。 
call 13\scripts\build.bat || set FAIL=1

echo.
echo ============================================
echo 31/31 13\ Modbus 三层测试 T01~T47（g++） 
echo ============================================
rem  EMS_PYTHON 未设置时，跨语言层（T40~T47）会打印 SKIPPED=N 并以 0 退出。 
rem  那是**有意的** —— 环境缺失不是代码缺陷；但 SKIP 必须在输出里显式出现。 
if "%BUILD_TESTS%"=="1" (
    call 13\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
if "%FAIL%"=="1" (
    echo [BUILD ALL FAIL] Some component failed. Check output above.
    popd
    endlocal
    exit /b 1
) else (
    echo [BUILD ALL OK] All 31 components built.
    popd
    endlocal
    exit /b 0
)
