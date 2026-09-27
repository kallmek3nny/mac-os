@echo off
REM ---------------------------------------------------------------------------
REM _call-transparency-probe.cmd -- is  call "prog.exe" args  identical to
REM "prog.exe" args  for stdout and for the exit code, INCLUDING a negative one?
REM
REM WHY THIS MATTERS
REM ----------------
REM tools/_toolsguard-harness.cmd carries a copy of :toolsguard from
REM guest-enable-ssh-via-tools.cmd. The real script sets VMRUN to vmrun.EXE; the
REM harness must set it to a batch-file stub. A batch file invoked from a batch
REM file WITHOUT  call  transfers control and never returns -- proved 2026-08-10
REM by tools/_nocall-repro.cmd, whose "PARENT resumed after no-call invocation"
REM line never printed. That is exactly how the harness died immediately after
REM its PART 1 banner with zero assertions.
REM
REM So the harness copy needs a leading  call . That is a one-token divergence
REM from the real script, and an unproven divergence is precisely the kind of
REM stub-vs-real gap that made the OLD harness report "5/5 PASS" against a
REM caller that could not run at all. This probe measures the divergence against
REM the REAL vmrun.exe instead of assuming it is harmless.
REM
REM Both a zero exit and vmrun's NEGATIVE credential-rejection exit are tested,
REM because the negative code is the whole reason the error checks were rewritten.
REM ---------------------------------------------------------------------------
setlocal EnableExtensions

set "VMRUN=C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe"
set "VMX=C:\Users\Kentl\macos-vm\sonoma\Sonoma.vmx"
set "U=__probe_no_such_user__"
set "P=__probe_no_such_pass__"
set "T=%TEMP%\_call_transparency"

if not exist "%VMRUN%" (
  echo [ABORT] real vmrun not found at "%VMRUN%"
  endlocal
  exit /b 2
)

echo === CASE 1: checkToolsState, expected to succeed ===
"%VMRUN%" checkToolsState "%VMX%" > "%T%_1a.txt" 2>&1
set "RC1A=%ERRORLEVEL%"
call "%VMRUN%" checkToolsState "%VMX%" > "%T%_1b.txt" 2>&1
set "RC1B=%ERRORLEVEL%"
set "OUT1A="
set "OUT1B="
for /f "usebackq delims=" %%S in ("%T%_1a.txt") do if not defined OUT1A set "OUT1A=%%S"
for /f "usebackq delims=" %%S in ("%T%_1b.txt") do if not defined OUT1B set "OUT1B=%%S"
echo   direct : rc=[%RC1A%] out=[%OUT1A%]
echo   call   : rc=[%RC1B%] out=[%OUT1B%]
if "%RC1A%"=="%RC1B%" (if "%OUT1A%"=="%OUT1B%" (echo   [MATCH]) else (echo   [DIFFER: stdout])) else (echo   [DIFFER: exit code])
echo.

echo === CASE 2: bogus credentials, expected to exit NEGATIVE ===
"%VMRUN%" -gu "%U%" -gp "%P%" runProgramInGuest "%VMX%" /usr/bin/true > "%T%_2a.txt" 2>&1
set "RC2A=%ERRORLEVEL%"
call "%VMRUN%" -gu "%U%" -gp "%P%" runProgramInGuest "%VMX%" /usr/bin/true > "%T%_2b.txt" 2>&1
set "RC2B=%ERRORLEVEL%"
set "OUT2A="
set "OUT2B="
for /f "usebackq delims=" %%S in ("%T%_2a.txt") do if not defined OUT2A set "OUT2A=%%S"
for /f "usebackq delims=" %%S in ("%T%_2b.txt") do if not defined OUT2B set "OUT2B=%%S"
echo   direct : rc=[%RC2A%] out=[%OUT2A%]
echo   call   : rc=[%RC2B%] out=[%OUT2B%]
if "%RC2A%"=="%RC2B%" (if "%OUT2A%"=="%OUT2B%" (echo   [MATCH]) else (echo   [DIFFER: stdout])) else (echo   [DIFFER: exit code])
echo.

echo === CASE 3: does control RETURN after each form? ===
echo   this line proves the probe survived both invocation styles.

del "%T%_1a.txt" "%T%_1b.txt" "%T%_2a.txt" "%T%_2b.txt" >nul 2>&1
echo == done ==
endlocal
exit /b 0
