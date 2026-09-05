@echo off
rem ====================================================================
rem  Run the A组 safety constraint manager demo.
rem  Default writes log in UTF-16 LE (matches the originally shipped format).
rem  Usage:   scripts\run_demo.bat            (UTF-16 LE -> out.txt)
rem           scripts\run_demo.bat utf8        (UTF-8 -> out.txt)
rem
rem  Reads the binary from ..\build\safety_constraint_manager.exe.
rem  Build it first with scripts\build.bat if missing.
rem ====================================================================

setlocal
pushd "%~dp0.."

if not exist build\safety_constraint_manager.exe (
    echo Binary not found. Building ...
    call scripts\build.bat || ( popd & exit /b 1 )
)

if not exist log mkdir log

set OUT=log\out.txt
set ENC=utf16-le
if not "%1"=="" set ENC=%1

if /I "%ENC%"=="utf8" (
    build\safety_constraint_manager.exe > %OUT%
) else (
    powershell -NoProfile -Command "$OutputEncoding=[System.Text.Encoding]::UTF8; ..\build\safety_constraint_manager.exe | Out-File -Encoding Unicode %OUT%"
)

echo Demo finished. Log written to %OUT% (%ENC% encoding).
popd
endlocal
