@echo off
REM =====================================================================
REM 14/ 平台层：把 10/ 的仿真产物导入 SQLite
REM   数据源：10\build\（timeseries.csv / alarms.csv / summary.json / fault\） 
REM   产物：  14\data\ems.db
REM
REM   可用 EMS_PYTHON 指定解释器；未设置时用 PATH 里的 python。 
REM =====================================================================

setlocal
cd /d "%~dp0\..\.."

set PY=%EMS_PYTHON%
if "%PY%"=="" set PY=python

echo ============================================
echo  14/ 导入仿真产物到平台数据库
echo ============================================
"%PY%" 14\src\importer.py %*
if errorlevel 1 (
    echo [FAIL] 14/ 导入失败
    exit /b 1
)
echo [OK] 14/ 导入完成
endlocal
