@echo off
rem ============================================================
rem  TestFill - install only (run as Administrator)
rem ============================================================
setlocal

set SRC=%~dp0build\FillingEffect.aex
set EFXDIR=C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\Plug-ins\Effects

if not exist "%EFXDIR%" (
    echo [ERROR] AE 2026 plugins folder not found:
    echo   %EFXDIR%
    pause
    exit /b 1
)
if not exist "%SRC%" (
    echo [ERROR] source not found: %SRC%
    pause
    exit /b 1
)
copy /y "%SRC%" "%EFXDIR%\FillingEffect.aex" >nul
if errorlevel 1 (
    echo [ERROR] copy failed - run as Administrator.
    pause
    exit /b 1
)
echo [OK] Installed: %EFXDIR%\FillingEffect.aex
echo Restart After Effects, then: Effects -^> Test -^> Test
pause
