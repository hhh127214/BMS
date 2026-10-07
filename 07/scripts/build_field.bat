@echo off
REM =====================================================================
REM 07/ 现场进程入口编译：main_field.exe
REM
REM   三条依赖链都要，缺一条就链不上：
REM     ① 07/src/rtdb/ems_point_table.c   点表真相源（C 头 + C 实现）
REM     ② 13/src/modbus_device_io.h       Modbus TCP 主站适配器（头文件即主体）
REM        + -lws2_32（WinSock2；ModbusDeviceIO 只写 TCP，不碰 iphlpapi/bcrypt）
REM     ③ 07/vendor/rt_db/rt_db_api.c      共享内存实时库（--device rtdb 用）
REM
REM   为什么要 -I ..\13\src：
REM     13/ 的主体就是**头文件**，按 13/scripts/build.bat 开头的既定意图
REM     「由 07/ 装配层包含使用」。modbus_probe.exe 只是它的一个消费者，
REM     所以 Modbus 适配器**不另立进程** —— 现场进程就是本程序。
REM
REM   顺带重编 loop_demo.exe：供 build_test_field.bat 做输出等价性比对。
REM
REM   用法（注意默认只读，不传 --control 不会下发任何指令）：
REM     build\main_field.exe --help
REM     build\main_field.exe                                    仿真（等于演示）
REM     build\main_field.exe --device rtdb --steps 20 -v          看实时库真实数据
REM     build\main_field.exe --device modbus --host 10.0.0.7     现场只读核对
REM     build\main_field.exe --device modbus --host 10.0.0.7 --control   真下发
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === [1/4] 编译 RT_DB C 实现（共享内存实时库）===
gcc -std=c11 -O2 -D__USE_MINGW_ANSI_STDIO=1 -I vendor\rt_db -c vendor\rt_db\rt_db_api.c -o build\rt_db_api.o
if errorlevel 1 (
    echo [FAIL] rt_db_api.c 编译失败
    exit /b 1
)

echo === [2/4] 编译 EMS 点表（点表真相源，C 实现）===
gcc -std=c11 -O2 -I src\rtdb -I vendor\rt_db -c src\rtdb\ems_point_table.c -o build\ems_point_table.o
if errorlevel 1 (
    echo [FAIL] ems_point_table.c 编译失败
    exit /b 1
)

echo === [3/4] 编译现场入口 main_field.exe ===
g++ -std=c++17 -Wall -O2 -I src -I src\rtdb -I vendor\rt_db -I ..\04\src -I ..\05\src -I ..\06\src -I ..\08\src -I ..\13\src src\main_field.cpp build\rt_db_api.o build\ems_point_table.o -lws2_32 -o build\main_field.exe
if errorlevel 1 (
    echo [FAIL] main_field.exe 编译失败
    exit /b 1
)

echo === [4/4] 编译演示程序 loop_demo.exe（等价性比对用）===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\08\src -I src\rtdb src\main.cpp -o build\loop_demo.exe
if errorlevel 1 (
    echo [FAIL] loop_demo.exe 编译失败
    exit /b 1
)

echo.
echo === 编译成功 ===
echo   build\main_field.exe   现场入口（--device sim^|rtdb^|modbus，默认 sim）
echo   build\loop_demo.exe    演示程序
echo.
echo 下一步：scripts\build_test_field.bat  （单元测试 + 输出等价性判据）
endlocal
