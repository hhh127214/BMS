@echo off
REM =====================================================================
REM  vendor\lib60870 build
REM
REM  把 lib60870-C 2.4.1（上游源码，未做任何改动）编成本项目用的静态库： 
REM      build\lib60870.a
REM
REM  上游：https://github.com/mz-automation/lib60870
REM  说明：本目录下是**原样上游代码**，不修改。所有适配都写在 P3\ 里。 
REM
REM  为什么不用上游的 Makefile / CMakeLists： 
REM    · 本机没有 make（项目统一直接调 gcc/g++，见 scripts\build_all.bat）；
REM    · 上游 CMake 会连带编 examples / tests，并把 TLS(mbedtls) 拉进来；
REM      我们只用明文 104，不需要 TLS，裁剪后 23 个 C 文件即可。 
REM
REM  MinGW 注意事项： 
REM    · 上游是 C99 代码，必须用 -std=gnu99（C++ 的 .cpp 不能直接编这些文件）； 
REM    · win32 HAL 需要链 ws2_32 / iphlpapi / bcrypt（见 P3\scripts\build.bat）。 
REM =====================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0"

if not exist build mkdir build
if exist build\obj rmdir /s /q build\obj
mkdir build\obj

set INC=-Iconfig -Isrc\inc\api -Isrc\inc\internal -Isrc\common\inc -Isrc\hal\inc -Isrc\file-service

REM 只编 win32 HAL + 协议层，共 23 个 .c（对应上游 src\CMakeLists.txt 的 
REM lib_common_SRCS + BUILD_COMMON + lib_windows_SRCS，去掉 linux/bsd/macos/tls） 
set SRCS=^
 src\common\linked_list.c ^
 src\file-service\file_server.c ^
 src\hal\memory\lib_memory.c ^
 src\hal\serial\win32\serial_port_win32.c ^
 src\hal\socket\win32\socket_win32.c ^
 src\hal\thread\win32\thread_win32.c ^
 src\hal\time\win32\time.c ^
 src\iec60870\apl\cpXXtime2a.c ^
 src\iec60870\cs101\cs101_asdu.c ^
 src\iec60870\cs101\cs101_bcr.c ^
 src\iec60870\cs101\cs101_information_objects.c ^
 src\iec60870\cs101\cs101_master.c ^
 src\iec60870\cs101\cs101_master_connection.c ^
 src\iec60870\cs101\cs101_queue.c ^
 src\iec60870\cs101\cs101_slave.c ^
 src\iec60870\cs104\cs104_connection.c ^
 src\iec60870\cs104\cs104_frame.c ^
 src\iec60870\cs104\cs104_slave.c ^
 src\iec60870\frame.c ^
 src\iec60870\lib60870_common.c ^
 src\iec60870\link_layer\buffer_frame.c ^
 src\iec60870\link_layer\link_layer.c ^
 src\iec60870\link_layer\serial_transceiver_ft_1_2.c

echo === [1/2] compile 23 C sources (gcc -std=gnu99 -O2) ===
set N=0
for %%f in (%SRCS%) do (
    gcc -std=gnu99 -O2 -Wall %INC% -c %%f -o build\obj\%%~nf.o
    if errorlevel 1 (
        echo [FAIL] %%f
        exit /b 1
    )
    set /a N+=1
)
echo     compiled !N! files

echo === [2/2] archive build\lib60870.a ===
ar rcs build\lib60870.a build\obj\*.o
if errorlevel 1 (
    echo [FAIL] ar failed
    exit /b 1
)

echo.
echo === BUILD OK ===
echo   vendor\lib60870\build\lib60870.a   (link with: -lws2_32 -liphlpapi -lbcrypt)
echo   headers: -I vendor\lib60870\config -I vendor\lib60870\src\inc\api -I vendor\lib60870\src\hal\inc

endlocal & exit /b 0
