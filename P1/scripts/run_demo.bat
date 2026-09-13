@echo off
REM =====================================================================
REM P1/ 产品化 P1：运行演示（配置化）
REM   产物：build\ems_config.json / CONFIG.md / schema.json / captured.json
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译 P1/ 产品化 P1 配置化演示 ===
g++ -std=c++17 -Wall -O2 -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\08\src src\main.cpp -o build\ems_config.exe
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)

echo.
echo === 1/6 A/B 对照演示：只改配置，行为随之改变 ===
build\ems_config.exe --demo

echo.
echo === 2/6 校验样例配置 ===
build\ems_config.exe --check data\ems_config.sample.json

echo.
echo === 3/6 用样例配置装配并跑 24h ===
build\ems_config.exe --apply data\ems_config.sample.json

echo.
echo === 4/6 存取往返一致性 ===
build\ems_config.exe --roundtrip data\ems_config.sample.json

echo.
echo === 5/6 生成配置模板与文档 ===
build\ems_config.exe --dump-template > build\ems_config.json
build\ems_config.exe --dump-doc      > build\CONFIG.md
build\ems_config.exe --dump-schema   > build\schema.json
echo   已写出 build\ems_config.json / build\CONFIG.md / build\schema.json

echo.
echo === 6/6 导出运行中的当前配置 ===
build\ems_config.exe --capture build\captured.json

endlocal
