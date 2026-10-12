@echo off
rem Build (and optionally run) test targets with the MSVC environment loaded.
rem A plain bash shell has cl.exe on PATH but no INCLUDE/LIB, so standard
rem headers like <cstdio> fail with C1083. vcvars64 supplies them.
rem The inherited PATH is cut down first: entries such as
rem "C:\Program Files (x86)\VMware\." break vcvars64's parenthesized blocks
rem ("\VMware was unexpected at this time"). vcvars64 needs vswhere.exe from
rem the VS Installer directory, so that stays on the trimmed PATH.
rem Usage: tests\msvc_build.bat [target ...]   (no targets = build all)
setlocal
set "CMAKE_DIR="
set "NINJA_DIR="
for %%i in (cmake.exe) do set "CMAKE_DIR=%%~dp$PATH:i"
for %%i in (ninja.exe) do set "NINJA_DIR=%%~dp$PATH:i"
set "VSWHERE_DIR=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
set "PATH=%SystemRoot%\system32;%SystemRoot%;%SystemRoot%\System32\WindowsPowerShell\v1.0\;%VSWHERE_DIR%;%CMAKE_DIR%;%NINJA_DIR%"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not "%~1"=="" goto next
cmake --build build || exit /b 1
exit /b 0
:next
if "%~1"=="" exit /b 0
cmake --build build --target %1 || exit /b 1
if exist "build\%1.exe" "build\%1.exe" || exit /b 1
shift
goto next
