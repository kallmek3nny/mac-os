@echo off
REM ---------------------------------------------------------------------------
REM _errorlevel-repro.cmd -- why did guest-enable-ssh-via-tools.cmd print
REM   "[OK] Credentials authenticate" immediately after vmrun printed
REM   "Error: Invalid user name or password for the guest OS"?
REM
REM MEASURED FACT (2026-08-10, host shell): vmrun runProgramInGuest with bogus
REM credentials exits 127. So a plain  if errorlevel 1  MUST fire. This script
REM isolates whether the exit code survives the quoted "(x86)" invocation and a
REM preceding  call :subroutine  (which resets ERRORLEVEL on return).
REM ---------------------------------------------------------------------------
setlocal EnableExtensions

set "VMRUN=C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe"
set "VMX=C:\Users\Kentl\macos-vm\sonoma\Sonoma.vmx"
set "U=__probe_no_such_user__"
set "P=__probe_no_such_pass__"

echo == case 1: bare invocation, then if errorlevel 1 ==
"%VMRUN%" -gu "%U%" -gp "%P%" runProgramInGuest "%VMX%" /usr/bin/true
echo   captured %%ERRORLEVEL%% = [%ERRORLEVEL%]
if errorlevel 1 (echo   if-errorlevel-1: FIRED ^(correct^)) else (echo   if-errorlevel-1: DID NOT FIRE ^(defect^))

echo == case 2: same, but a  call :noop  runs first ==
call :noop
"%VMRUN%" -gu "%U%" -gp "%P%" runProgramInGuest "%VMX%" /usr/bin/true
if errorlevel 1 (echo   if-errorlevel-1: FIRED ^(correct^)) else (echo   if-errorlevel-1: DID NOT FIRE ^(defect^))

echo == case 3: call :guard first, THEN vmrun, mirroring the real script ==
call :guard "probe"
if errorlevel 1 echo   guard reported not-running
"%VMRUN%" -gu "%U%" -gp "%P%" runProgramInGuest "%VMX%" /usr/bin/true
echo   captured %%ERRORLEVEL%% = [%ERRORLEVEL%]
if errorlevel 1 (echo   if-errorlevel-1: FIRED ^(correct^)) else (echo   if-errorlevel-1: DID NOT FIRE ^(defect^))

echo == done ==
endlocal
exit /b 0

:noop
exit /b 0

:guard
"%VMRUN%" checkToolsState "%VMX%" > "%TEMP%\_el_probe.txt" 2>&1
for /f "usebackq delims=" %%S in ("%TEMP%\_el_probe.txt") do set "TS=%%S"
del "%TEMP%\_el_probe.txt" >nul 2>&1
if /i "%TS%"=="running" exit /b 0
exit /b 1
