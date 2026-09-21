@echo off
REM =====================================================================
REM  13\ build: Modbus TCP 设备接入（点表核对工具）
REM
REM  产物： 
REM    build\modbus_probe.exe    现场点表核对工具（读 32 点 / 读限值 / 下发指令） 
REM    build\ems_point_table.o   07\ 点表真相源的 C 实现（本模块与测试共用）
REM
REM  本模块的**主体是头文件**（modbus_tcp_client.h / modbus_point_map.h /
REM  modbus_device_io.h），由 07\ 装配层包含使用；probe 只是它的一个消费者。 
REM  所以这里只需要编一个 exe + 一个 C 目标文件。 
REM
REM  依赖：07\ 点表真相源；WinSock2（-lws2_32）。 
REM  不需要 RT_DB 共享内存、不需要 lib60870 —— Modbus 与那两条路线无关。 
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

REM ---------- [1/3] 07\ 点表真相源（C） ----------
REM  为什么测试与 probe 都要链 ems_point_table.o： 
REM    modbus_point_map.h 的点名与索引**直接取自** EMS_POINT_NAMES / 点表枚举， 
REM    不允许在 13\ 里再抄一份。符号定义在 ems_point_table.c 里，
REM    不链就是 undefined reference to `EMS_POINT_NAMES`。 
echo === [1/3] ems_point_table.o (07\ 点表真相源) ===
gcc -std=c11 -O2 -I ..\07\src\rtdb -I ..\07\vendor\rt_db ^
    -c ..\07\src\rtdb\ems_point_table.c -o build\ems_point_table.o
if errorlevel 1 ( echo [FAIL] ems_point_table.c & exit /b 1 )

REM ---------- [2/3] 包含路径 ----------
REM  13\src 自己；07\src\rtdb 取点表；04\src 取 IDeviceIO / RealtimeSnapshot。 
REM  跨模块 include 是**扁平**的（靠 -I 解析），所以搬头文件不用改 include。 
set INC=-I src -I ..\07\src\rtdb -I ..\04\src

REM  -lws2_32：WinSock2。ModbusDeviceIO 只写 TCP，不碰 iphlpapi/bcrypt。 
set LNK=build\ems_point_table.o -lws2_32

REM ---------- [3/3] 现场点表核对工具 ----------
echo === [3/3] modbus_probe.exe ===
g++ -std=c++17 -Wall -O2 %INC% ^
    src\main_probe.cpp %LNK% -o build\modbus_probe.exe
if errorlevel 1 (
    echo [FAIL] modbus_probe compile error
    exit /b 1
)

echo.
echo === BUILD OK ===
echo   build\modbus_probe.exe --plan              只打印分块读计划
echo   build\modbus_probe.exe --host 127.0.0.1 --port 15020
echo   scripts\run_sim.bat                        起 Python 从站（设备替身）

endlocal & exit /b 0
