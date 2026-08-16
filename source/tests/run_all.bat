@echo off
rem run_all.bat - FillingEffect full regression (7 suites)
setlocal
cd /d D:\work\AI\work\FillingEffect\tests
set FAIL=0
for %%T in (test_core test_styles test_composite test_gpu growth_regress test_direct direct_batch dbg_render env_test) do (
    echo === %%T ===
    "%%T.exe" > "%%T.out.txt" 2>&1
    if errorlevel 1 (
        echo %%T FAILED
        set FAIL=1
    ) else (
        echo %%T OK
    )
)
if "%FAIL%"=="1" (
    echo === SOME FAILED ===
    exit /b 1
) else (
    echo === ALL PASSED ===
    exit /b 0
)
