@echo off
rem ====================================================================
rem  Build + run 光伏出力平抑控制器 simulator.
rem  Reads smoothing_S1_normal.csv as default; cycles through S1~S6.
rem ====================================================================

setlocal
pushd "%~dp0.."

call scripts\build.bat || ( popd & exit /b 1 )

echo Running PV smoothing simulator ...
build\smoothing_sim.exe
if errorlevel 1 (
    echo [RUN FAIL] simulator returned non-zero.
    popd
    exit /b 1
)

if exist scripts\plot_smoothing.py (
    where python >nul 2>&1
    if not errorlevel 1 (
        echo Plotting with scripts\plot_smoothing.py ...
        python scripts\plot_smoothing.py
    )
)

echo [RUN OK] figs\ updated.
popd
endlocal
