@echo off
rem ====================================================================
rem  Build + run 防逆流控制器 simulator on default data\.
rem  Reads sim_sc1_pv_step.csv as input; writes sim results to data\.
rem ====================================================================

setlocal
pushd "%~dp0.."

call scripts\build.bat || ( popd & exit /b 1 )

if not exist data mkdir data

echo Running AR simulator ...
build\ar_sim.exe
if errorlevel 1 (
    echo [RUN FAIL] simulator returned non-zero.
    popd
    exit /b 1
)

if exist scripts\plot.py (
    where python >nul 2>&1
    if not errorlevel 1 (
        echo Plotting with scripts\plot.py ...
        python scripts\plot.py
    )
)

echo [RUN OK] data\ + figs\ updated.
popd
endlocal
