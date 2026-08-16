@echo off
setlocal

set MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207
set SDK10=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0
rem ↓ 修改为你的 After Effects SDK Examples 路径
set AEBASE=D:\work\AI\work\AESDK\out\ae25.6_61.64bit.AfterEffectsSDK\Examples

set PATH=%MSVC%\bin\Hostx64\x64;%SDK10%\bin\%SDKVER%\x64;%PATH%
set INCLUDE=%MSVC%\include;%SDK10%\Include\%SDKVER%\ucrt;%SDK10%\Include\%SDKVER%\um;%SDK10%\Include\%SDKVER%\shared;%SDK10%\Include\%SDKVER%\winrt;%AEBASE%\Headers;%AEBASE%\Headers\SP;%AEBASE%\Headers\Win;%AEBASE%\Resources;%AEBASE%\Util
set LIB=%MSVC%\lib\x64;%SDK10%\Lib\%SDKVER%\ucrt\x64;%SDK10%\Lib\%SDKVER%\um\x64

rem 相对本脚本目录
set SRC=%~dp0
set OUT=%~dp0build
if not exist "%OUT%" mkdir "%OUT%"

echo [1/4] Compile plugin shell...
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /DNDEBUG /DWIN32 /D_WINDOWS /D_USRDLL /I"%SRC%" ^
   /c "%SRC%\FillingEffect.cpp" /Fo"%OUT%\FillingEffect.obj" || goto :err

echo [2/4] Compile core...
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /c ^
   "%SRC%\dissolve_core.cpp" /Fo"%OUT%\dissolve_core.obj" || goto :err
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /c ^
   "%SRC%\dissolve_styles.cpp" /Fo"%OUT%\dissolve_styles.obj" || goto :err
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /c ^
   "%SRC%\dissolve_direct.cpp" /Fo"%OUT%\dissolve_direct.obj" || goto :err
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /c ^
   "%SRC%\gl_renderer.cpp" /Fo"%OUT%\gl_renderer.obj" || goto :err

echo [3/4] Compile PiPL resource (cl /EP -> strip #line -> PiPLTool -> cl /EP -> rc)...
cl /nologo /E /I"%AEBASE%\Headers" /I"%AEBASE%\Resources" ^
   "%SRC%\FillingEffectPiPL.r" > "%OUT%\FillingEffectPiPL.rr" || goto :err
findstr /v /r "^#line" "%OUT%\FillingEffectPiPL.rr" > "%OUT%\FillingEffectPiPL_noline.rr" || goto :err
"%AEBASE%\Resources\PiPLtool.exe" "%OUT%\FillingEffectPiPL_noline.rr" "%OUT%\FillingEffectPiPL.rrc" || goto :err
cl /nologo /E /DMSWindows /I"%AEBASE%\Headers" /I"%AEBASE%\Resources" ^
   "%OUT%\FillingEffectPiPL.rrc" > "%OUT%\FillingEffect.rc" || goto :err
rc /nologo /fo"%OUT%\FillingEffect.res" "%OUT%\FillingEffect.rc" || goto :err

echo [4/4] Link...
link /nologo /dll /out:"%OUT%\FillingEffect.aex" ^
   "%OUT%\FillingEffect.obj" "%OUT%\dissolve_core.obj" "%OUT%\dissolve_styles.obj" "%OUT%\dissolve_direct.obj" ^
   "%OUT%\gl_renderer.obj" ^
   "%OUT%\FillingEffect.res" || goto :err

echo.
echo [+] BUILD OK: %OUT%\FillingEffect.aex
exit /b 0

:err
echo [!] BUILD FAILED
exit /b 1
