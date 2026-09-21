@echo off
REM =====================================================================
REM  13\ run_sim: 起 Python Modbus 从站（PCS/BMS/电表三合一设备替身） 
REM
REM  用途：现场预演 / 跨语言联调。默认端口 15020（**不是 502** —— 
REM        502 是标准端口，本地起服务用非特权端口更省事）。 
REM
REM  需要 pymodbus： 
REM    python -m venv .venv
REM    .venv\Scripts\pip install -r 13\sim\requirements.txt
REM  然后设 EMS_PYTHON 指向那个 python（或直接用 PATH 上的 python）。 
REM
REM  例：
REM    scripts\run_sim.bat                                   默认工况
REM    scripts\run_sim.bat --load 380 --pv 150 --bias 40     带电表偏差 
REM    scripts\run_sim.bat --bms-dis-forbid                  模拟 BMS 禁放上报
REM    scripts\run_sim.bat --self-check                      只打印点表对照表
REM
REM  另开一个窗口：
REM    build\modbus_probe.exe --port 15020
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if "%EMS_PYTHON%"=="" (
    if exist ".venv\Scripts\python.exe" (
        set PY=%CD%\.venv\Scripts\python.exe
    ) else (
        set PY=python
    )
) else (
    set PY=%EMS_PYTHON%
)

echo === 13\ Python Modbus 从站 ===
echo python : %PY%
echo 脚本   : sim\modbus_slave.py
echo.

%PY% sim\modbus_slave.py --port 15020 %*

endlocal & exit /b %errorlevel%
