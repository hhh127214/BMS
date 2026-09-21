@echo off
REM =====================================================================
REM  12/ 正式验收：跑七维度验收，落盘三件套报告 
REM
REM    build\acceptance.exe
REM      1. 七维度验收（A1..A7） 
REM      2. 标准输出逐项结论（判据 + 实测） 
REM      3. 写 12\docs\ACCEPTANCE-REPORT.{md,json,html}
REM      4. 退出码 0 = 全部通过；1 = 存在未通过项（CI 门禁用）
REM
REM  可选参数（透传给 exe）：
REM    --quiet              只打印维度汇总与失败项 
REM    --perf-steps N       A4 性能采样拍数
REM    --rtdb-ticks N       A5 跨进程长稳拍数 
REM    --scenario-scale K   A6 场景倍数
REM    --no-write           只打印不落盘
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build\acceptance.exe (
    echo [INFO] acceptance.exe 不存在，先编译 
    call scripts\build.bat
    if errorlevel 1 exit /b 1
)

echo.
echo ============================================================
echo  周期 12 最终验收 —— 工商业储能 EMS V2.0
echo ============================================================
echo.

build\acceptance.exe --root .. %*
set RC=%errorlevel%

echo.
if %RC% == 0 (
    echo ============================================================
    echo  验收通过 —— EMS V2.0 可发布 
    echo  报告: 12\docs\ACCEPTANCE-REPORT.md / .json / .html
    echo ============================================================
) else (
    echo ============================================================
    echo  验收不通过 —— 存在未达标项，禁止发布 
    echo  报告: 12\docs\ACCEPTANCE-REPORT.md / .json / .html
    echo ============================================================
)

endlocal & exit /b %RC%
