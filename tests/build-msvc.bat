@echo off
REM
REM Build and run the host-side decode tests with MSVC.
REM
REM Two quoting hazards had to be worked around here, both worth recording so
REM nobody re-introduces them:
REM
REM 1. No `if not exist "%VCVARS%"` guard. cmd parses the parentheses in
REM    "Program Files (x86)" as block delimiters inside an IF, producing
REM    "\Microsoft was unexpected at this time."
REM
REM 2. PATH is reset to a minimal safe value before calling vcvars64.bat.
REM    The inherited PATH contains unquoted entries with parentheses (VMware,
REM    among others) and vcvars' own internal IF statements break on them:
REM    "\VMware\VMware was unexpected at this time." vcvars rebuilds the
REM    compiler PATH from scratch anyway, so nothing is lost.
REM
REM Usage (from anywhere):
REM   tests\build-msvc.bat
REM

setlocal

REM 3. The VS Installer directory is on this minimal PATH on purpose.
REM    VsDevCmd.bat (line ~180) pushd's into it and then invokes bare
REM    `vswhere.exe`. Without the directory on PATH that resolves to
REM    "'vswhere.exe' is not recognized..." printed as the FIRST line of
REM    output. It is harmless -- vcvars still sets the environment and the
REM    `where cl` check below passes -- but it reads exactly like a build
REM    failure and has already cost time once.
set "PATH=%SystemRoot%\system32;%SystemRoot%;%SystemRoot%\System32\Wbem;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"

call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :vcvars_failed

REM cmake and ninja were dropped by the PATH reset; add them back explicitly.
set "PATH=%PATH%;C:\Program Files\CMake\bin;%APPDATA%\Python\Python314\Scripts"

where cl >nul 2>&1
if errorlevel 1 goto :no_cl

cd /d "%~dp0.."

cmake -S tests -B tests\build -G Ninja
if errorlevel 1 exit /b 1

cmake --build tests\build
if errorlevel 1 exit /b 1

echo.
echo === running tests ===
REM ctest rather than a hardcoded exe name. An earlier version ran only
REM test_decode.exe, so test_falcon.cpp built successfully and then silently
REM never ran -- a green wrapper that was not testing what it claimed. Suites
REM registered via add_kext_test() in CMakeLists.txt are now picked up
REM automatically.
ctest --test-dir tests\build --output-on-failure
exit /b %errorlevel%

:vcvars_failed
echo ERROR: vcvars64.bat did not initialise the MSVC environment.
exit /b 1

:no_cl
echo ERROR: cl.exe not on PATH after vcvars64.bat ran.
exit /b 1
