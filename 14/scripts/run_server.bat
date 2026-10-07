@echo off
REM =====================================================================
REM 14/ 平台层：启动后端服务（默认 127.0.0.1:8765） 
REM   透传参数，例如： run_server.bat --port 9000
REM =====================================================================

setlocal
cd /d "%~dp0\..\.."

set PY=%EMS_PYTHON%
if "%PY%"=="" set PY=python

"%PY%" 14\src\server.py %*
endlocal
