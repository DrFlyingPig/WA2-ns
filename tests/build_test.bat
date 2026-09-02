@echo off
rem build_test.bat - build headless core tests (MSVC)
rem usage: tests\build_test.bat [path to vcvars64.bat]
setlocal
set "VCVARS=%~1"
if "%VCVARS%"=="" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS%" goto havevc
echo vcvars64.bat not found
echo usage: build_test.bat [path to vcvars64.bat]
exit /b 1
:havevc
call "%VCVARS%" >nul
if not exist out mkdir out

cl /nologo /std:c++17 /EHsc /W3 /utf-8 /I src ^
  tests\test_core.cpp ^
  src\wa2\util.cpp src\wa2\archive.cpp src\wa2\res.cpp ^
  src\wa2\script.cpp src\wa2\funcs.cpp src\wa2\state.cpp ^
  src\wa2\sjis.cpp src\wa2\sjis_table.cpp ^
  /Fe:out\test_core.exe
if errorlevel 1 exit /b 1
out\test_core.exe
if errorlevel 1 exit /b 1

rem Build the exact cumulative D2-D5/F1 macro set used by the F1 NRO.
rem Keep this separate so conditional tests cannot be skipped by the base build.
cl /nologo /std:c++17 /EHsc /W3 /utf-8 ^
  /DWA2_DIAG_STREAM_LONG_SE ^
  /DWA2_DIAG_C4_RELATIVE_TIMER ^
  /DWA2_DIAG_C4_CLICK_ADVANCE ^
  /DWA2_DIAG_C4_SYNC_CLICK_ADVANCE ^
  /DWA2_DIAG_CHARACTER_LIFECYCLE ^
  /I src ^
  tests\test_core.cpp ^
  src\wa2\util.cpp src\wa2\archive.cpp src\wa2\res.cpp ^
  src\wa2\script.cpp src\wa2\funcs.cpp src\wa2\state.cpp ^
  src\wa2\sjis.cpp src\wa2\sjis_table.cpp ^
  /Fe:out\test_core_f1.exe
if errorlevel 1 exit /b 1
out\test_core_f1.exe
if errorlevel 1 exit /b 1

rem Build the exact cumulative D2-D5/F1/F2 macro set used by the F2 NRO.
cl /nologo /std:c++17 /EHsc /W3 /utf-8 ^
  /DWA2_DIAG_STREAM_LONG_SE ^
  /DWA2_DIAG_C4_RELATIVE_TIMER ^
  /DWA2_DIAG_C4_CLICK_ADVANCE ^
  /DWA2_DIAG_C4_SYNC_CLICK_ADVANCE ^
  /DWA2_DIAG_CHARACTER_LIFECYCLE ^
  /DWA2_DIAG_TEXT_FIT ^
  /I src ^
  tests\test_core.cpp ^
  src\wa2\util.cpp src\wa2\archive.cpp src\wa2\res.cpp ^
  src\wa2\script.cpp src\wa2\funcs.cpp src\wa2\state.cpp ^
  src\wa2\sjis.cpp src\wa2\sjis_table.cpp ^
  /Fe:out\test_core_f2.exe
if errorlevel 1 exit /b 1
out\test_core_f2.exe
if errorlevel 1 exit /b 1

rem Build the exact cumulative D2-D5/F1/F2/F3 macro set used by the F3 NRO.
cl /nologo /std:c++17 /EHsc /W3 /utf-8 ^
  /DWA2_DIAG_STREAM_LONG_SE ^
  /DWA2_DIAG_C4_RELATIVE_TIMER ^
  /DWA2_DIAG_C4_CLICK_ADVANCE ^
  /DWA2_DIAG_C4_SYNC_CLICK_ADVANCE ^
  /DWA2_DIAG_CHARACTER_LIFECYCLE ^
  /DWA2_DIAG_TEXT_FIT ^
  /DWA2_DIAG_TEXT_REFLOW ^
  /I src ^
  tests\test_core.cpp ^
  src\wa2\util.cpp src\wa2\archive.cpp src\wa2\res.cpp ^
  src\wa2\script.cpp src\wa2\funcs.cpp src\wa2\state.cpp ^
  src\wa2\sjis.cpp src\wa2\sjis_table.cpp ^
  /Fe:out\test_core_f3.exe
if errorlevel 1 exit /b 1
out\test_core_f3.exe
if errorlevel 1 exit /b 1

rem Build the exact cumulative D2-D5/F1/F2/F3/F4 macro set used by the F4 NRO.
cl /nologo /std:c++17 /EHsc /W3 /utf-8 ^
  /DWA2_DIAG_STREAM_LONG_SE ^
  /DWA2_DIAG_C4_RELATIVE_TIMER ^
  /DWA2_DIAG_C4_CLICK_ADVANCE ^
  /DWA2_DIAG_C4_SYNC_CLICK_ADVANCE ^
  /DWA2_DIAG_CHARACTER_LIFECYCLE ^
  /DWA2_DIAG_TEXT_FIT ^
  /DWA2_DIAG_TEXT_REFLOW ^
  /DWA2_DIAG_TEXT_SAFE_WIDTH ^
  /I src ^
  tests\test_core.cpp ^
  src\wa2\util.cpp src\wa2\archive.cpp src\wa2\res.cpp ^
  src\wa2\script.cpp src\wa2\funcs.cpp src\wa2\state.cpp ^
  src\wa2\sjis.cpp src\wa2\sjis_table.cpp ^
  /Fe:out\test_core_f4.exe
if errorlevel 1 exit /b 1
out\test_core_f4.exe
if errorlevel 1 exit /b 1

rem Build the exact cumulative D2-D5/F1/F2/F3/F4/F5 macro set used by the F5 NRO.
cl /nologo /std:c++17 /EHsc /W3 /utf-8 ^
  /DWA2_DIAG_STREAM_LONG_SE ^
  /DWA2_DIAG_C4_RELATIVE_TIMER ^
  /DWA2_DIAG_C4_CLICK_ADVANCE ^
  /DWA2_DIAG_C4_SYNC_CLICK_ADVANCE ^
  /DWA2_DIAG_CHARACTER_LIFECYCLE ^
  /DWA2_DIAG_TEXT_FIT ^
  /DWA2_DIAG_TEXT_REFLOW ^
  /DWA2_DIAG_TEXT_SAFE_WIDTH ^
  /DWA2_DIAG_TEXT_SNOW_SAFE ^
  /I src ^
  tests\test_core.cpp ^
  src\wa2\util.cpp src\wa2\archive.cpp src\wa2\res.cpp ^
  src\wa2\script.cpp src\wa2\funcs.cpp src\wa2\state.cpp ^
  src\wa2\sjis.cpp src\wa2\sjis_table.cpp ^
  /Fe:out\test_core_f5.exe
if errorlevel 1 exit /b 1
out\test_core_f5.exe
if errorlevel 1 exit /b 1

rem Build the exact production macro set selected by tools/build_switch.py --release.
rem P3 is intentionally absent because its hardware test was rejected.
cl /nologo /std:c++17 /EHsc /W3 /utf-8 ^
  /DWA2_DIAG_STREAM_LONG_SE ^
  /DWA2_DIAG_C4_RELATIVE_TIMER ^
  /DWA2_DIAG_C4_CLICK_ADVANCE ^
  /DWA2_DIAG_C4_SYNC_CLICK_ADVANCE ^
  /DWA2_DIAG_CACHE_20MB ^
  /DWA2_DIAG_DIRECT_BLOCKLINEAR ^
  /DWA2_DIAG_PARALLEL_FRAMEBUFFER ^
  /DWA2_DIAG_CHARACTER_LIFECYCLE ^
  /DWA2_DIAG_TEXT_FIT ^
  /DWA2_DIAG_TEXT_REFLOW ^
  /DWA2_DIAG_TEXT_SAFE_WIDTH ^
  /DWA2_DIAG_TEXT_SNOW_SAFE ^
  /DWA2_RELEASE_BUILD ^
  /I src ^
  tests\test_core.cpp ^
  src\wa2\util.cpp src\wa2\archive.cpp src\wa2\res.cpp ^
  src\wa2\script.cpp src\wa2\funcs.cpp src\wa2\state.cpp ^
  src\wa2\sjis.cpp src\wa2\sjis_table.cpp ^
  /Fe:out\test_core_release.exe
if errorlevel 1 exit /b 1
out\test_core_release.exe
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /EHsc /W3 /utf-8 /I src ^
  tests\test_framebuffer_swizzle.cpp ^
  /Fe:out\test_framebuffer_swizzle.exe
if errorlevel 1 exit /b 1
out\test_framebuffer_swizzle.exe
if errorlevel 1 exit /b 1
endlocal
