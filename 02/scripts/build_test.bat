@echo off
rem ====================================================================
rem  Build the A组 safety constraint manager unit tests.
rem
rem  Source: tests\safety_test.cpp (uses #define main to share translation
rem          unit with safety_constraint_manager.cpp).
rem  Output: build\safety_test.exe
rem
rem  Usage: scripts\build_test.bat
rem ====================================================================

setlocal
pushd "%~dp0.."

if not exist build mkdir build

where g++ >nul 2>&1
if errorlevel 1 (
    echo [BUILD FAIL] g++ not found in PATH.
    popd
    exit /b 1
)

echo Compiling tests\safety_test.cpp ...
g++ -std=c++17 -O2 -Wall -Wextra -o build\safety_test.exe tests\safety_test.cpp
if errorlevel 1 (
    echo [BUILD FAIL] g++ returned non-zero.
    popd
    exit /b 1
)

echo [BUILD OK] build\safety_test.exe
echo Running tests ...
build\safety_test.exe
popd
endlocal
