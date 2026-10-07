@echo off
REM =====================================================================
REM 14/ 平台层：把 07/ 现场进程 --record 写出的 CSV **增量**入库
REM   数据源：07\build\<实录>.csv（20 列，与 10/ timeseries.csv 同构，
REM           但 t_s = Unix 墙钟秒、time = 完整日期时间）
REM   产物：  14\data\ems.db（scenario.time_base='wall'）
REM
REM   与 import_sim.bat 的区别：
REM     · 增量追加（不删旧行），可反复调用，幂等
REM     · time_base 显式 'wall'，与仿真场景的 'sim' 时间轴分开
REM
REM   用法：
REM     import_live.bat <实录CSV> [scenario_id] [db_path]
REM       实录CSV      07/ 现场进程 --record 的产物（必填）
REM       scenario_id  默认 live
REM       db_path      默认 14\data\ems.db
REM
REM   可用 EMS_PYTHON 指定解释器；未设置时用 PATH 里的 python。
REM =====================================================================

setlocal
cd /d "%~dp0\..\.."

set PY=%EMS_PYTHON%
if "%PY%"=="" set PY=python

if "%~1"=="" (
    echo [FAIL] 需要实录 CSV 路径：import_live.bat ^<csv^> [scenario_id] [db_path]
    exit /b 1
)

"%PY%" 14\src\importer.py --live %*
if errorlevel 1 (
    echo [FAIL] 14/ live 入库失败
    exit /b 1
)
echo [OK] 14/ live 入库完成
endlocal
