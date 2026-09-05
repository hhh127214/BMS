@echo off
rem ====================================================================
rem  Build + run 需量管理控制器 simulator.
rem  Reads demand_sim.csv; runs the sliding-window demand controller.
rem ====================================================================

setlocal
pushd "%~dp0.."

call scripts\build.bat || ( popd & exit /b 1 )

echo Running demand controller simulator ...
build\demand_sim.exe
if errorlevel 1 (
    echo [RUN FAIL] simulator returned non-zero.
    popd
    exit /b 1
)

if exist scripts\plot_demand.py (
    where python >nul 2>&1
    if not errorlevel 1 (
        echo Plotting with scripts\plot_demand.py ...
        python scripts\plot_demand.py
    )
)

echo [RUN OK] figs\ updated.
popd
endlocal
