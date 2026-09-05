@echo off
rem ====================================================================
rem  Build + run the three-controller integration simulator.
rem  Outputs data\integration_sim.csv (overwrites previous).
rem  Then runs scripts\plot_integration.py to produce figs\integration.png.
rem ====================================================================

setlocal
pushd "%~dp0.."

call scripts\build.bat || ( popd & exit /b 1 )

if not exist data mkdir data
if not exist figs mkdir figs

echo Running integration simulator ...
build\integration_sim.exe
if errorlevel 1 (
    echo [RUN FAIL] simulator returned non-zero.
    popd
    exit /b 1
)

echo Plotting integration result ...
where python >nul 2>&1
if errorlevel 1 (
    echo [SKIP] python not found; skipping plot generation.
    popd
    exit /b 0
)

python scripts\plot_integration.py
if errorlevel 1 (
    echo [PLOT FAIL] plot_integration.py returned non-zero.
    popd
    exit /b 1
)

echo [RUN OK] data\integration_sim.csv + figs\integration.png updated.
popd
endlocal
