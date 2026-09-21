@echo off
REM =====================================================================
REM  11/ 3-process integration demo (period 11 deliverable)
REM
REM  process 0  rtdb_initializer.exe  create segment + register 30 points,
REM                                    hold alive for 60 s (Windows rule)
REM  process 1  device_side.exe       BMS/PCS/meter/PV/transformer/load
REM                                    writes MEAS/STA/CFG, reads CMD
REM  process 2  ems_side.exe          EMS full stack via RtDbDeviceIO,
REM                                    writes CMD, reads MEAS/STA/CFG
REM
REM  The two worker processes each keep their own 100 ms tick (Pacing), and
REM  both stay alive for the whole window -- that is what makes the loop
REM  genuinely closed. Running them at full speed would finish the device
REM  side in milliseconds and freeze the measurements in the segment.
REM  600 steps @ 0.1 s at speed 4 => ~15 s wall clock. Keep the SAME
REM  --speed on both sides, otherwise their time axes drift apart.
REM =====================================================================

setlocal
cd /d "%~dp0\.."

set STEPS=600
set DT=0.1
set SPEED=4

if not exist build\rtdb_initializer.exe (
    echo [FAIL] binaries not found, run scripts\build.bat first
    exit /b 1
)

echo === period 11 system integration: 3-process demo ===
echo [0] start initializer (holds segment 60 s)
start "rtdb_initializer" /B build\rtdb_initializer.exe --seconds 60

REM give the initializer a moment to create the segment
timeout /t 1 /nobreak >nul

echo [1] start device side (%STEPS% steps @ %DT% s, speed %SPEED%x, faults 20-25 / 40-45 s, BMS forbid-discharge 30-35 s)
start "device_side" /B build\device_side.exe --steps %STEPS% --dt %DT% --speed %SPEED% ^
    --fault 4:20:25 --fault 6:40:45 --fault 8:30:35

REM give the device process a moment to publish initial points
timeout /t 1 /nobreak >nul

echo [2] start EMS side (full stack, observer on)
build\ems_side.exe --steps %STEPS% --dt %DT% --speed %SPEED% --log-every 1
set RC=%errorlevel%

echo.
echo === EMS report ===
type build\integration_ems_report.txt
echo.
echo === device report ===
type build\integration_device_report.txt

echo.
if %RC% == 0 (
    echo === INTEGRATION DEMO OK ===
) else (
    echo === INTEGRATION DEMO FAILED (rc=%RC%) ===
)
endlocal & exit /b %RC%
