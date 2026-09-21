@echo off
REM =====================================================================
REM  P3/ IEC104 从站网关 —— 上电首查 + 演示
REM
REM  做三件事，对应现场上电后的固定动作：
REM    [1] 自检     点表映射表 + RT_DB 段里点名能否全部解析
REM                  少一个点 = 有一个上送量永远是坏点，必须在开 104 之前查出来 
REM    [2] 起从站   监听 2404，等调度来连
REM    [3] 回读     用库自带的主站模拟器连自己，跑一遍 
REM                  握手 → 对钟 → 总召唤 → 遥调 → 遥控
REM
REM  前置条件： 
REM    rtdb_initializer.exe **必须常驻**。Windows 上 RT_DB 段是页面文件映射
REM    对象，最后一个句柄关闭即销毁 —— 初始化器跑完就退 = 段没了 = 网关连不上。 
REM    下面用 `start /B ... --seconds 120` 让它活 120 s。 
REM
REM  本脚本**不修改任何东西**：网关对 RT_DB 全段只读（类型上就没有 write 方法）。 
REM =====================================================================

setlocal
cd /d "%~dp0\.."

set PORT=2404
set CA=1
set SECONDS=60
set STALE=0

if not exist build\iec104_gateway.exe (
    echo [FAIL] build\iec104_gateway.exe 不存在，先跑 scripts\build.bat
    exit /b 1
)
if not exist ..\11\build\rtdb_initializer.exe (
    echo [FAIL] ..\11\build\rtdb_initializer.exe 不存在，先跑 11\scripts\build.bat
    exit /b 1
)

echo === P3 IEC104 gateway: self-check + listen demo ===
echo.
echo [0] start RT_DB initializer (holds the segment for 120 s)
start "rtdb_initializer" /B ..\11\build\rtdb_initializer.exe --seconds 120
timeout /t 2 /nobreak >nul

echo.
echo [1] gateway self-check
build\iec104_gateway.exe --self-check-only
if errorlevel 1 (
    echo.
    echo [FAIL] 自检不通过 —— 先解决上面的问题，别急着开 104 端口
    exit /b 1
)

echo.
echo [2] start the slave on port %PORT% (CA=%CA%, %SECONDS% s)
REM  还没接调度主站，所以命令默认一律否定确认；演示用 --accept-commands 走 DRY-RUN。 
REM  正式现场：不要加 --accept-commands，等 EXT 点区落地后由装配层决定谁受理。 
start "iec104_gateway" /B build\iec104_gateway.exe --port %PORT% --ca %CA% --seconds %SECONDS% --accept-commands
timeout /t 2 /nobreak >nul

echo.
echo [3] loopback test with the bundled master simulator
echo     (handshake -^- clock sync -^- general interrogation -^- setpoint -^- command)
if not exist build\iec104_master.exe (
    echo [SKIP] build\iec104_master.exe 不存在（--no-test 构建时不会产出）
    echo        现在可以用任何第三方 IEC104 主站工具连 127.0.0.1:%PORT%
    pause
    goto :done
)
build\iec104_master.exe --port %PORT% --ca %CA% --seconds 8 --gi-interval 6 --set 6001 120.5 --cmd 5001 1
echo.
echo   看两件事： 
echo     * 总召唤必须是「激活确认 -> 数据 -> 激活终止」三段齐全 
echo       少了终止帧，主站在等，会一直重召（见 docs/README.md 2.1） 
echo     * 每条上送点后面**不该出现 [品质 0x80 IV]**
echo       出现 = 源侧读不到，查点名映射或 RT_DB 段 

:done
echo.
echo === 演示结束（网关在 %SECONDS% s 后自行退出；initializer 120 s 后退出）===
endlocal & exit /b 0
