@echo off
REM =====================================================================
REM P3/ demo runner: build then run scenario F/G/H
REM NOTE: intentionally ASCII-only.
REM =====================================================================

setlocal
cd /d "%~dp0\.."

call scripts\build.bat
if errorlevel 1 exit /b 1

build\ems_comms.exe

endlocal & exit /b %errorlevel%
