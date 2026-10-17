@echo off
REM =====================================================================
REM  21\ build: DL/T645-2007 主站
REM
REM  产物： 
REM    build\meter645.exe         主站 CLI（--sim / --tcp / --serial） 
REM    build\sim_tcp_server.exe   TCP 仿真从站（联调用，**不是交付物**） 
REM
REM  为什么 sim_tcp_server 也在这里编：
REM    TcpLink 这条路必须能对着一个"会分片的字节流"实测一次，
REM    否则它只有编译证据。把它放在同一份脚本里，保证每次全量构建 
REM    都顺手产出这个对端，联调时不用另找环境。 
REM =====================================================================

setlocal
cd /d "%~dp0\.."
if not exist build mkdir build

set INC=-I src
set LNK=-lws2_32

echo [21] 编译 meter645.exe（DL/T645 主站 CLI） 
g++ -std=c++17 -Wall -Wextra -O2 %INC% src\main_meter645.cpp %LNK% -o build\meter645.exe
if errorlevel 1 ( echo [FAIL] main_meter645 compile error & exit /b 1 )

echo [21] 编译 sim_tcp_server.exe（TCP 仿真从站） 
g++ -std=c++17 -Wall -Wextra -O2 %INC% src\sim_tcp_server.cpp %LNK% -o build\sim_tcp_server.exe
if errorlevel 1 ( echo [FAIL] sim_tcp_server compile error & exit /b 1 )

echo [OK] 21\ DL/T645 主站编译完成
endlocal
exit /b 0
