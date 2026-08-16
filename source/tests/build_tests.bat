@echo off
setlocal
set MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207
set SDK10=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0
set AEBASE=D:\work\AI\work\AESDK\out\ae25.6_61.64bit.AfterEffectsSDK\Examples
set PATH=%MSVC%\bin\Hostx64\x64;%SDK10%\bin\%SDKVER%\x64;%PATH%
set INCLUDE=%MSVC%\include;%SDK10%\Include\%SDKVER%\ucrt;%SDK10%\Include\%SDKVER%\um;%SDK10%\Include\%SDKVER%\shared;%SDK10%\Include\%SDKVER%\winrt;%AEBASE%\Headers;%AEBASE%\Headers\SP;%AEBASE%\Headers\Win;%AEBASE%\Resources;%AEBASE%\Util
set LIB=%MSVC%\lib\x64;%SDK10%\Lib\%SDKVER%\ucrt\x64;%SDK10%\Lib\%SDKVER%\um\x64
cd /d D:\work\AI\work\FillingEffect\tests
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /c /I.. test_core.cpp /Fotest_core.obj || goto :err
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /c /I.. test_styles.cpp /Fotest_styles.obj || goto :err
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /c /I.. test_composite.cpp /Fotest_composite.obj || goto :err
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /c ..\dissolve_core.cpp /Fodissolve_core.obj || goto :err
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /c ..\dissolve_styles.cpp /Fodissolve_styles.obj || goto :err
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /c ..\dissolve_direct.cpp /Fodissolve_direct.obj || goto :err
link /nologo /out:test_core.exe test_core.obj dissolve_core.obj dissolve_styles.obj || goto :err
link /nologo /out:test_styles.exe test_styles.obj dissolve_core.obj dissolve_styles.obj || goto :err
link /nologo /out:test_composite.exe test_composite.obj dissolve_core.obj dissolve_styles.obj || goto :err
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /c /I.. debug_composite.cpp /Fodebug_composite.obj || goto :err
link /nologo /out:debug_composite.exe debug_composite.obj dissolve_core.obj dissolve_styles.obj || goto :err
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /c /I.. test_gpu.cpp /Fotest_gpu.obj || goto :err
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /c ..\gl_renderer.cpp /Fogl_renderer.obj || goto :err
link /nologo /out:test_gpu.exe test_gpu.obj dissolve_core.obj dissolve_styles.obj gl_renderer.obj opengl32.lib user32.lib gdi32.lib || goto :err
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /c /I.. dbg_render.cpp /Fodbg_render.obj || goto :err
link /nologo /out:dbg_render.exe dbg_render.obj dissolve_core.obj dissolve_styles.obj dissolve_direct.obj || goto :err
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /c /I.. growth_regress.cpp /Fogrowth_regress.obj || goto :err
link /nologo /out:growth_regress.exe growth_regress.obj dissolve_core.obj dissolve_styles.obj || goto :err
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /c /I.. test_direct.cpp /Fotest_direct.obj || goto :err
link /nologo /out:test_direct.exe test_direct.obj dissolve_core.obj dissolve_styles.obj dissolve_direct.obj || goto :err
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /c /I.. direct_batch.cpp /Fodirect_batch.obj || goto :err
link /nologo /out:direct_batch.exe direct_batch.obj dissolve_core.obj dissolve_styles.obj dissolve_direct.obj || goto :err
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /c /I.. env_test.cpp /Foenv_test.obj || goto :err
link /nologo /out:env_test.exe env_test.obj dissolve_core.obj dissolve_styles.obj dissolve_direct.obj || goto :err
echo TEST_BUILD_OK
exit /b 0
:err
echo TEST_BUILD_FAILED
exit /b 1
