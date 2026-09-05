@echo off
rem ====================================================================
rem  Start the battery optimizer service (default port 8000).
rem  Usage: scripts\run.bat [port]
rem  Reads the binary from ..\build\battery_server.exe (built by build.bat)
rem ====================================================================

setlocal
pushd "%~dp0.."

if not exist build\battery_server.exe (
    echo [RUN FAIL] binary not found. Run scripts\build.bat first.
    popd
    exit /b 1
)

echo Starting battery_server on port %1 ...
build\battery_server.exe %1
popd
endlocal
