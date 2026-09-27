@echo off
REM ---------------------------------------------------------------------------
REM _paren-repro-b.cmd -- validate ONLY the proposed fix (form B).
REM
REM WHY A SECOND FILE
REM -----------------
REM _paren-repro.cmd tested form A then form B in one script. Form A did not
REM merely fail: cmd.exe aborted the ENTIRE script at the parse error
REM ("\VMware\VMware was unexpected at this time.", exit 255) and form B's
REM banner never printed. So that run proved the defect but proved NOTHING
REM about the fix. Form B must be exercised in a script form A cannot kill.
REM
REM Form B: redirect vmrun's stdout to a temp file, then read the FILE with
REM   for /f "usebackq". The "(x86)" path never sits inside a FOR body, so
REM   cmd.exe never sees its parentheses as command-grouping metacharacters.
REM ---------------------------------------------------------------------------
setlocal EnableExtensions

set "VMRUN=C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe"
set "VMX=C:\Users\Kentl\macos-vm\sonoma\Sonoma.vmx"
set "TSFILE=%TEMP%\_ts_probe_b.txt"

echo == form B: redirect to temp file, then read the FILE ==
set "TS_B=<never-set>"
"%VMRUN%" checkToolsState "%VMX%" > "%TSFILE%" 2>&1
set "RC=%ERRORLEVEL%"
for /f "usebackq delims=" %%S in ("%TSFILE%") do set "TS_B=%%S"
echo   vmrun exit = %RC%
echo   TS_B=[%TS_B%]

REM Prove the parsed value is usable in a comparison, which is what the real
REM :toolsguard subroutine does with it.
if /i "%TS_B%"=="running"   echo   COMPARE: matched 'running'
if /i "%TS_B%"=="installed" echo   COMPARE: matched 'installed'
if /i "%TS_B%"=="<never-set>" echo   COMPARE: FAILED -- variable never assigned

del "%TSFILE%" >nul 2>&1
echo == done ==
endlocal
exit /b 0
