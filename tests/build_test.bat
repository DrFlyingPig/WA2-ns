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
endlocal
