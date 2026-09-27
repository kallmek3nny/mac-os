@echo off
REM ---------------------------------------------------------------------------
REM _paren-repro.cmd -- isolated repro for the abort observed on 2026-08-10:
REM
REM   guest-enable-ssh-via-tools.cmd printed
REM       === STEP 1: VMware Tools state ===
REM       \VMware\VMware was unexpected at this time.
REM
REM Hypothesis: the doubled-quote  for /f ('""%VMRUN%" args"')  form breaks when
REM VMRUN contains the parentheses of "C:\Program Files (x86)\...". cmd.exe
REM parses the ( and ) of (x86) as command-grouping metacharacters inside the
REM FOR body before quote processing, splitting the path.
REM
REM The _toolsguard-harness.cmd unit test could NOT have caught this: it stubbed
REM VMRUN with %TEMP%\_fake-vmrun.cmd, a path with no spaces and no parentheses.
REM
REM Form A = the current (suspect) form. Form B = the proposed fix: redirect to
REM a temp file, then read the FILE, so the path never sits inside a FOR body.
REM ---------------------------------------------------------------------------
setlocal EnableExtensions

set "VMRUN=C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe"
set "VMX=C:\Users\Kentl\macos-vm\sonoma\Sonoma.vmx"

echo == form A: original doubled-quote for /f form ==
set "TS_A=<never-set>"
for /f "delims=" %%S in ('""%VMRUN%" checkToolsState "%VMX%""') do set "TS_A=%%S"
echo   TS_A=[%TS_A%]

echo == form B: redirect to temp file, then read the FILE ==
set "TS_B=<never-set>"
"%VMRUN%" checkToolsState "%VMX%" > "%TEMP%\_ts_probe.txt" 2>&1
for /f "usebackq delims=" %%S in ("%TEMP%\_ts_probe.txt") do set "TS_B=%%S"
echo   TS_B=[%TS_B%]
del "%TEMP%\_ts_probe.txt" >nul 2>&1

echo == done ==
endlocal
exit /b 0
