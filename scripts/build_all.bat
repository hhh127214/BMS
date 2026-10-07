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
rem    14\  平台层后端 + SQLite 数据库        (python, src/*.py + tests/selftest.py)
rem    15\  平台层前端 + 运行界面            (static, index.html + assets/*.js)
rem    16\  通信加固：重连退避 / 主备双链路 / SNTP / Modbus RTU / TLS / FTP
rem    17\  设备全点表与高保真模拟器（B2~B5 通路缺失 / C1~C5 规模问题） 
rem    18\  性能基准与压测（SLA 硬门禁 + 24h 长跑）  ★ 时序敏感，勿并发
rem    19\  现场部署 P5（守护器 / 配置版本回滚 / 日志轮转 / 作业指导书）
rem    20\  告警能力独立 + SOE 持久化 + 配置持久化 
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
echo 1/41 01\  B组 strategy server (gcc)
echo ============================================
call 01\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 2/41 02\  A组 safety mgr demo (g++)
echo ============================================
call 02\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 3/41 02\  A组 safety unit tests (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 02\scripts\build_test.bat || set FAIL=1
)
echo.
echo ============================================
echo 4/41 03\anti_reverse_controller\
echo ============================================
call "03\anti_reverse_controller\scripts\build.bat" || set FAIL=1
echo.
echo ============================================
echo 5/41 03\pv_smoothing_controller\
echo ============================================
call "03\pv_smoothing_controller\scripts\build.bat" || set FAIL=1
echo.
echo ============================================
echo 6/41 03\demand_management_controller\
echo ============================================
call "03\demand_management_controller\scripts\build.bat" || set FAIL=1
echo.
echo ============================================
echo 7/41 03\integration\
echo ============================================
call 03\integration\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 8/41 04\ 策略管理层 + 仲裁器 (g++)
echo ============================================
call 04\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 9/41 04\ 单元测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 04\scripts\build_test.bat || set FAIL=1
)
echo.
echo ============================================
echo 10/41 05\ 周期5 安全约束引擎 (g++)
echo ============================================
call 05\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 11/41 05\ 单元测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 05\scripts\build_test.bat || set FAIL=1
)
echo.
echo ============================================
echo 12/41 06\ 周期6 EMS 状态机 (g++)
echo ============================================
call 06\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 13/41 06\ 单元测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 06\scripts\build_test.bat || set FAIL=1
)
echo.
echo ============================================
echo 14/41 07\ 周期7 实时控制闭环 (g++)
echo ============================================
call 07\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 15/41 07\ 单元测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 07\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 16/41 07\ 现场入口 main_field：设备接入闭环 (g++)
echo ============================================
call 07\scripts\build_field.bat || set FAIL=1
echo.
echo ============================================
echo 17/41 08\ 周期8 优化调度与实时控制协同 (g++)
echo ============================================
call 08\scripts\build.bat || set FAIL=1
echo.
echo ============================================
echo 18/41 08\ 单元测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 08\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 19/41 07\ 产品化 P0/P0.5 设备 I/O 抽象测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 07\scripts\build_test_device_io.bat || set FAIL=1
)

echo.
echo ============================================
echo 20/41 07\ RT_DB 接入：共享内存适配器 + 点表自检 (g++/gcc)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 07\scripts\build_test_rtdb.bat || set FAIL=1
)

echo.
echo ============================================
echo 21/41 07\ 现场入口测试：参数解析 + 输出等价性 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 07\scripts\build_test_field.bat || set FAIL=1
)

echo.
echo ============================================
echo 22/41 09\ 周期9 多策略组合测试 (闭环时序级) (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 09\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 23/41 10\ 周期10 EMS 24h 离线仿真测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 10\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 24/41 P1\ 产品化 P1 配置化 单元测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call P1\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 25/41 P2\ 产品化 P2 可观测性 单元测试 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call P2\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 26/41 11\ 周期11 系统级联调：三进程编译 (g++/gcc)
echo ============================================
call 11\scripts\build.bat || set FAIL=1

echo.
echo ============================================
echo 27/41 11\ 周期11 系统级联调：单元测试 T41~T49 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 11\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 28/41 12\ 周期12 最终验收：验收器编译 (g++/gcc)
echo ============================================
call 12\scripts\build.bat || set FAIL=1

echo.
echo ============================================
echo 29/41 12\ 周期12 最终验收：模块自测 T51~T59 (g++)
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 12\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 30/41 vendor\lib60870 + P3\ IEC104 从站网关（g++/gcc） 
echo ============================================
rem  第三方库显式编一次：P3\scripts\build.bat 也会自己发现并补编，
rem  但写在这里能让失败停在正确的位置（而不是埋在 P3 里面）。 
call vendor\lib60870\build.bat || set FAIL=1
call P3\scripts\build.bat || set FAIL=1

echo.
echo ============================================
echo 31/41 P3\ IEC104 网关单元测试 T61~T72 + EXT 落点 T73~T76（g++） 
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call P3\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 32/41 13\ Modbus TCP 设备接入：点表核对工具（g++/gcc） 
echo ============================================
rem  本模块主体是**头文件**（modbus_tcp_client.h / modbus_point_map.h /
rem  modbus_device_io.h），由 07\ 装配层包含使用；probe 只是它的一个消费者。 
call 13\scripts\build.bat || set FAIL=1

echo.
echo ============================================
echo 33/41 13\ Modbus 三层测试 T01~T47（g++） 
echo ============================================
rem  EMS_PYTHON 未设置时，跨语言层（T40~T47）会打印 SKIPPED=N 并以 0 退出。 
rem  那是**有意的** —— 环境缺失不是代码缺陷；但 SKIP 必须在输出里显式出现。 
if "%BUILD_TESTS%"=="1" (
    call 13\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
echo 34/41 14\ 平台层后端（Python 标准库：导入 + 接口自检） 
echo ============================================
rem  14/ 不是 C/C++ 组件（是平台层后端服务 + SQLite），但同样进全量回归。 
rem  依赖 10\build 的仿真产物；产物不存在时自检显式 SKIPPED 并以 0 退出。 
call 14\scripts\run_all.bat || set FAIL=1

echo.
echo ============================================
echo 35/41 15\ 平台层前端（纯静态：Node 静态契约自检） 
echo ============================================
rem  15/ 没有编译步骤（零依赖、零打包、单页离线可开），进全量回归的是 
rem  它的静态契约自检：引用的资源齐不齐、前端调的每个 /api/* 后端是否真有、 
rem  菜单/页面/角色门槛三者是否一致。 
rem  找不到 Node 时显式 SKIP 并以 0 退出（环境缺失不是代码缺陷）。 
call 15\scripts\run_all.bat || set FAIL=1

echo.
echo ============================================
echo 36/41 16\ 通信加固：重连退避 / 主备双链路 / SNTP / RTU / TLS / FTP 
echo ============================================
rem  16\ 是**头文件模块**：它的"产物"就是 build\ 下那几个测试可执行文件， 
rem  所以 --no-test 时退化为 build.bat（只编译、不跑用例）—— 
rem  "编过了"与"跑过了"必须能分辨，这是 16\ 自己的纪律。 
rem  TLS 层的对端是 Python ssl（OpenSSL）：拿别人的实现当对端，避免自己批 
rem  自己的卷子。缺 python 时该层显式 SKIP 并以 0 退出（环境缺失不是缺陷）。 
if "%BUILD_TESTS%"=="1" (
    call 16\scripts\build_test.bat || set FAIL=1
)
if not "%BUILD_TESTS%"=="1" (
    call 16\scripts\build.bat || set FAIL=1
)

echo.
echo ============================================
echo 37/41 17\ 设备全点表与高保真模拟器（B2~B5 / C1~C5） 
echo ============================================
rem  两层点表（07\ 抽象 40 点 vs 17\ 设备全点表）+ 点表配置化（换站改 JSON 不改源码） 
rem  + 电表/BMS/PCS 三个高保真模拟器 + 分级节拍/死区/批量写。 
rem  零第三方依赖（不依赖 Python / Node / pymodbus）。 
if "%BUILD_TESTS%"=="1" (
    call 17\scripts\build_test.bat || set FAIL=1
)
if not "%BUILD_TESTS%"=="1" (
    call 17\scripts\build.bat || set FAIL=1
)

echo.
echo ============================================
echo 38/41 18\ 性能基准与压测：SLA 门禁 + 24h 长跑 
echo ============================================
rem  仪器类模块：报的不是"通过/失败"，而是**在什么条件下测出的什么数**。 
rem  ★ 本步是**时序敏感**的：它测 p99 / 加速比 / 内存增长，前提是无并发负载。 
rem    build_all 是逐步串行的，满足这个前提；但**不要**再并行跑第二次全量构建。 
rem  产物报告落 18\build\BENCH-REPORT.md。 
if "%BUILD_TESTS%"=="1" (
    call 18\scripts\build_test.bat || set FAIL=1
)
if not "%BUILD_TESTS%"=="1" (
    call 18\scripts\build.bat || set FAIL=1
)

echo.
echo ============================================
echo 39/41 19\ 现场部署 P5（守护器 / 配置版本回滚 / 日志轮转 / 作业指导书） 
echo ============================================
rem  主体是 Python 部署与运维脚本，用 Python 测（同 14\ 15\ 的口径，不设 
rem  BUILD_TESTS 开关）。缺 11\ 产物 / 缺 ems_config.exe 时相关用例显式 SKIP。 
call 19\scripts\build_test.bat || set FAIL=1

echo.
echo ============================================
echo 40/41 20\ 告警能力与持久化：编译产物（soe_dump / alarm_demo） 
echo ============================================
rem  20\ 有**真产物**（不只是测试）：soe_dump.exe 读 SOE 落盘文件， 
rem  alarm_demo.exe 是告警 + 持久化的端到端演示。与测试分开两步， 
rem  这样 --no-test 时仍然产出可交付的 exe。 
call 20\scripts\build.bat || set FAIL=1

echo.
echo ============================================
echo 41/41 20\ 告警能力与持久化：单元测试（告警装配 / SOE / 配置） 
echo ============================================
if "%BUILD_TESTS%"=="1" (
    call 20\scripts\build_test.bat || set FAIL=1
)

echo.
echo ============================================
if "%FAIL%"=="1" (
    echo [BUILD ALL FAIL] Some component failed. Check output above.
    popd
    endlocal
    exit /b 1
) else (
    echo [BUILD ALL OK] All 41 components built.
    popd
    endlocal
    exit /b 0
)
