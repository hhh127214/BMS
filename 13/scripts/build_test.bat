@echo off
REM =====================================================================
REM  13\ build_test: Modbus 单元测试 T01~T15 / T21~T30 / T40~T47
REM
REM  三层，职责不重叠（这是本模块测试结构的核心设计）： 
REM
REM    test_modbus_tcp.exe        协议层（T01~T15） 
REM        socket ↔ 字节。内置 fake 从站（13/src/fake_modbus_slave.h），
REM        **不需要 Python**，且能人为造出半包/串包/异常码/ByteCount 不符。 
REM        → 保证"我们的客户端符合协议，且异常路径处理正确"。 
REM
REM    test_modbus_device_io.exe  适配器契约（T21~T30） 
REM        32 点快照 / 分块读契约 / 指令原子性 / 部分成功 / 安全位。 
REM        同样只用 fake 从站，**不需要 Python**。 
REM
REM    test_modbus_bridge.exe     跨语言联调（T40~T47） 
REM        C++ 主站 ↔ **Python pymodbus** 从站。需要外部 Python + pymodbus， 
REM        所以单独一层、且**环境缺失时 SKIP 而非 FAIL**。 
REM        → 保证"两个独立实现之间能真正互通"。 
REM
REM  ★ 为什么必须分三层：自己写两端等于自己和自己对答案。 
REM    协议层与契约层用的 fake 从站也是我们写的，双方共同误解协议时
REM    谁都发现不了（比如都把 32 位字序记成 AB）。只有拿别人的实现 
REM    （pymodbus）当对端，"我们的客户端符合协议"才第一次有外部证据。 
REM
REM  环境变量 EMS_PYTHON：指向装了 pymodbus 的 python.exe。 
REM    定位顺序：EMS_PYTHON → 本目录下 .venv\Scripts\python.exe → PATH 上的 python。 
REM    全都不可用时，bridge 层打印 SKIPPED=7 并以 0 退出（**skip 而非 fail**）。 
REM    建环境：python -m venv .venv
REM            .venv\Scripts\pip install -r sim\requirements.txt
REM
REM  ★ 全量基线因此有**两个合法值**（必须按本次实际跑到的那个上报）： 
REM      有 pymodbus → 13\ 贡献 439，全量 9323
REM      无 pymodbus → 13\ 贡献 361（第三层 7 条 SKIP），全量 9245
REM    判据：本脚本末尾那行回显的**层数**，或 build\bridge_status.txt 里的 SKIPPED=。 
REM    ★ 为什么非要把这件事喊出来：SKIP 本身是有意的（环境问题不是代码缺陷）， 
REM      但如果构建脚本仍然印「三层测试全部通过」，那么「静默跳过」与「真的跑过」 
REM      在最终输出上**依然不可区分** —— 这正是本项目最忌讳的一类失败。 
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

REM  ---------- python 解释器定位 ----------
REM  与 C++ 侧 which_python() 同一套顺序：显式变量优先，其次本地 venv。 
REM  为什么加 .venv 这一级：现场/CI 常常忘了设 EMS_PYTHON， 
REM  于是跨语言层整体 SKIP —— 看起来"跑过了"，实际这一层根本没验。 
REM  自动捡起 .venv 能让"照文档建了环境"的人默认就跑到这一层。 
if "%EMS_PYTHON%"=="" (
    if exist ".venv\Scripts\python.exe" (
        set EMS_PYTHON=%CD%\.venv\Scripts\python.exe
        echo [info] EMS_PYTHON 未设置，自动采用 .venv\Scripts\python.exe
    )
)

REM  ---------- 先确保 ems_point_table.o 存在 ----------
REM  ★ 无条件重编，**不要**改成 `if not exist`： 
REM    点表扩容（32 -> 40 点）时头文件变了而 .o 不重编，会出现 
REM    「同名点表两套内容」—— 编译期不报错，运行期按名字/索引取点错位。 
gcc -std=c11 -O2 -I ..\07\src\rtdb -I ..\07\vendor\rt_db ^
    -c ..\07\src\rtdb\ems_point_table.c -o build\ems_point_table.o
if errorlevel 1 ( echo [FAIL] ems_point_table.c & exit /b 1 )

set INC=-I src -I ..\07\src\rtdb -I ..\04\src
set LNK=build\ems_point_table.o -lws2_32

set FAIL=0

REM ---------- [1/4] 协议层 ----------
echo === [1/4] test_modbus_tcp.exe (T01~T15 协议层) ===
g++ -std=c++17 -Wall -O2 %INC% ^
    tests\test_modbus_tcp.cpp %LNK% -o build\test_modbus_tcp.exe
if errorlevel 1 ( echo [FAIL] test_modbus_tcp compile error & exit /b 1 )
build\test_modbus_tcp.exe
if errorlevel 1 ( echo [FAIL] test_modbus_tcp tests did not pass & set FAIL=1 )

REM ---------- [2/4] 适配器契约 ----------
echo.
echo === [2/4] test_modbus_device_io.exe (T21~T30 适配器契约) ===
g++ -std=c++17 -Wall -O2 %INC% ^
    tests\test_modbus_device_io.cpp %LNK% -o build\test_modbus_device_io.exe
if errorlevel 1 ( echo [FAIL] test_modbus_device_io compile error & exit /b 1 )
build\test_modbus_device_io.exe
if errorlevel 1 ( echo [FAIL] test_modbus_device_io tests did not pass & set FAIL=1 )

REM ---------- [3/4] 跨语言联调 ----------
REM  ★ 环境缺失时本层会打印 SKIPPED=N 并以 0 退出 —— 那是**有意的**： 
REM    环境问题不是代码缺陷。但 SKIP 必须显式出现， 
REM    否则"静默跳过"与"真的跑过"在输出上不可区分。 
echo.
echo === [3/4] test_modbus_bridge.exe (T40~T47 跨语言联调) ===
if "%EMS_PYTHON%"=="" (
    echo     EMS_PYTHON 未设置，将尝试 PATH 上的 python
)
g++ -std=c++17 -Wall -O2 %INC% ^
    tests\test_modbus_bridge.cpp %LNK% -o build\test_modbus_bridge.exe
if errorlevel 1 ( echo [FAIL] test_modbus_bridge compile error & exit /b 1 )
REM  先删旧状态文件：否则上一轮留下的 SKIPPED=0 会让本轮被误判成"跑过了"。 
del /q build\bridge_status.txt 2>nul
build\test_modbus_bridge.exe
set BRIDGE_RC=%ERRORLEVEL%
if not "%BRIDGE_RC%"=="0" ( echo [FAIL] test_modbus_bridge tests did not pass & set FAIL=1 )

REM  ---------- 判定第三层是否真的跑了 ----------
REM  读 exe 落的纯 ASCII 状态行（不是去 grep 中文输出，避免编码问题）。 
REM  文件不存在（exe 崩了/没落盘）→ 同样按「未跑」处理，**宁可多报 SKIP 不可漏报**。 
set BRIDGE_SKIPPED=1
findstr /C:"SKIPPED=0" build\bridge_status.txt >nul 2>nul
if not errorlevel 1 set BRIDGE_SKIPPED=0

REM ---------- [4/4] 收尾 ----------
echo.
if "%FAIL%"=="1" (
    echo [FAIL] 13\ Modbus 测试有失败项
    endlocal & exit /b 1
)
if "%BRIDGE_SKIPPED%"=="1" (
    echo [SKIP] 13\ Modbus 前两层通过（223 + 133）；**跨语言层未运行** —— 缺 pymodbus
    echo        这不是代码缺陷，但本次 13\ 只贡献 361 条：全量口径是 **9245**，不是 9323。 
    echo        要补上第三层（83 条）： 
    echo            python -m venv .venv
    echo            .venv\Scripts\pip install -r sim\requirements.txt
    echo            set EMS_PYTHON=%%CD%%\.venv\Scripts\python.exe
    endlocal & exit /b 0
)
echo [OK] 13\ Modbus 三层测试全部通过（439 条：223 + 133 + 83） 
endlocal & exit /b 0
