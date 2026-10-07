@echo off
REM =====================================================================
REM 07/ 现场进程入口测试：参数解析单测 + 输出等价性判据
REM
REM   T-F1 ~ T-F8  参数解析（tests/test_field_entry.cpp）
REM   P-F1         **输出等价性**：main_field.exe 不传 --device 时，
REM                与 loop_demo.exe 的输出必须逐字节相同。
REM                唯一允许的差异是 mean_cycle —— 它是每拍耗时的墙钟统计，
REM                同一程序连跑两次都不一样，所以比对前先过滤掉。
REM
REM   为什么 P-F1 不写在 C++ 单测里：
REM     它要跑两个**可执行文件**并比对输出，单测进程做不了这件事。
REM =====================================================================

setlocal
cd /d "%~dp0\.."

if not exist build mkdir build

echo === 编译 07/ 现场入口单元测试 ===
g++ -std=c++17 -Wall -O2 -I src tests\test_field_entry.cpp -o build\test_field_entry.exe
if errorlevel 1 (
    echo [FAIL] test_field_entry.exe 编译失败
    exit /b 1
)

echo === 编译 main_field.exe 与 loop_demo.exe ===
call "%~dp0build_field.bat" >nul
if errorlevel 1 (
    echo [FAIL] build_field.bat 失败 —— 请单独跑一次看报错
    exit /b 1
)

echo.
echo === 跑单元测试 T-F1 ~ T-F8 ===
build\test_field_entry.exe
if errorlevel 1 (
    echo [FAIL] 单元测试未通过
    exit /b 1
)

echo.
echo === P-F1 输出等价性：main_field（不传 --device）vs loop_demo ===
build\loop_demo.exe > build\_parity_a.txt
build\main_field.exe > build\_parity_b.txt
findstr /V "mean_cycle" build\_parity_a.txt > build\_parity_a2.txt
findstr /V "mean_cycle" build\_parity_b.txt > build\_parity_b2.txt
fc /B build\_parity_a2.txt build\_parity_b2.txt >nul
if errorlevel 1 (
    echo [FAIL] P-F1 不成立：默认路径的输出与演示程序不一致
    echo        跑 "fc build\_parity_a2.txt build\_parity_b2.txt" 看具体差异
    exit /b 1
)
echo [ OK ] P-F1 成立：除 mean_cycle（计时噪声）外逐字节一致

echo.
echo =========================================
echo  07/ 现场入口测试全部通过
echo =========================================
endlocal
