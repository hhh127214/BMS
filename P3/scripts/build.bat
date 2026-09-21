@echo off
REM =====================================================================
REM  P3\ build: IEC104 网关（从站）
REM
REM  产物： 
REM    build\iec104_gateway.exe      独立网关进程（读 RT_DB，对上开 104 从站） 
REM
REM  依赖： 
REM    vendor\lib60870\build\lib60870.a   ← 缺则自动先编（vendor\lib60870\build.bat） 
REM    ..\07\vendor\rt_db                 RT_DB C 实现
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

REM ---------- [1/4] 第三方库 ----------
echo === [1/4] lib60870 static library ===
if not exist ..\vendor\lib60870\build\lib60870.a (
    echo     not built yet - building it now
    call ..\vendor\lib60870\build.bat
    if errorlevel 1 (
        echo [FAIL] lib60870 build
        exit /b 1
    )
) else (
    echo     up to date: ..\vendor\lib60870\build\lib60870.a
)

REM ---------- [2/4] RT_DB C 实现 ----------
echo === [2/4] RT_DB C library ===
REM  ★ 无条件重编，**不要**改成 `if not exist`： 
REM    存在性判断只看文件在不在、不看源头有没有变。点表扩容时
REM    （32 点 -> 40 点）头文件变了而这里的 .o 不重编，就会出现
REM    「同名点表两套内容」—— 编译期一个字都不说，运行期按名字找点失效。 
REM    这三个 .c 都很小，重编代价可忽略。 
gcc -std=c11 -O2 -D__USE_MINGW_ANSI_STDIO=1 -I ..\07\vendor\rt_db ^
    -c ..\07\vendor\rt_db\rt_db_api.c -o build\rt_db_api.o
if errorlevel 1 ( echo [FAIL] rt_db_api.c & exit /b 1 )
gcc -std=c11 -O2 -I ..\07\src\rtdb -I ..\07\vendor\rt_db ^
    -c ..\07\src\rtdb\ems_point_table.c -o build\ems_point_table.o
if errorlevel 1 ( echo [FAIL] ems_point_table.c & exit /b 1 )
gcc -std=c11 -O2 -I ..\07\src\rtdb -I ..\07\vendor\rt_db ^
    -c ..\07\src\rtdb\ems_rt_db_setup.c -o build\ems_rt_db_setup.o
if errorlevel 1 ( echo [FAIL] ems_rt_db_setup.c & exit /b 1 )
set RTDBOBJS=build\rt_db_api.o build\ems_point_table.o build\ems_rt_db_setup.o

REM ---------- [3/4] 包含路径 ----------
REM  注意：lib60870 是 C99 库，头文件带 extern "C"，C++ 可直接 include。 
REM  它的 win32 HAL 会带进 <windows.h>，所以必须定义 NOMINMAX， 
REM  否则 min/max 宏会破坏 std::min / std::max（项目里大量使用）。 
set INC=-I src ^
 -I ..\vendor\lib60870\config ^
 -I ..\vendor\lib60870\src\inc\api ^
 -I ..\vendor\lib60870\src\inc\internal ^
 -I ..\vendor\lib60870\src\common\inc ^
 -I ..\vendor\lib60870\src\hal\inc ^
 -I ..\07\src\rtdb ^
 -I ..\07\vendor\rt_db

set LNK=..\vendor\lib60870\build\lib60870.a %RTDBOBJS% -lws2_32 -liphlpapi -lbcrypt

REM ---------- [4/4] 网关 ----------
REM  INC 里已包含 src\inc\internal
REM  拆除了旧版重复的 -I： 
REM  那个路径里混进了一个 VT 控制字符
REM  (0x0B) 而不是字母 v
REM  ---- g++ 只发一条警告就继续，错得毫无声响。 
echo === [3/4] iec104_gateway.exe ===
g++ -std=c++17 -Wall -O2 -DNOMINMAX %INC% ^
    src\main_gateway.cpp %LNK% -o build\iec104_gateway.exe
if errorlevel 1 (
    echo [FAIL] iec104_gateway compile error
    exit /b 1
)

echo === [4/4] iec104_master.exe ===
REM 主站模拟器需要库的「内部」头：带时标类型没有公开取值接口（见源文件头注释）
g++ -std=c++17 -Wall -O2 -DNOMINMAX %INC% ^
    src\main_master.cpp %LNK% -o build\iec104_master.exe
if errorlevel 1 (
    echo [FAIL] iec104_master compile error
    exit /b 1
)

echo.
echo === BUILD OK ===
echo   build\iec104_gateway.exe   [--port 2404 --ca 1 --seconds 60 --accept-commands]
echo   run scripts\run_gateway.bat for a self-check + 60s listen demo

endlocal & exit /b 0
