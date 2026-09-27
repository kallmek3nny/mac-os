@echo off
REM ---------------------------------------------------------------------------
REM _toolsguard-harness.cmd -- unit test for the :toolsguard subroutine in
REM guest-enable-ssh-via-tools.cmd, and for the vmrun exit-code convention.
REM
REM WHY THIS FILE WAS REWRITTEN ON 2026-08-10
REM -----------------------------------------
REM The previous version of this harness reported "5/5 PASS" while the caller it
REM was supposedly testing could not run AT ALL. It missed two real defects:
REM
REM   1. It stubbed VMRUN as %TEMP%\_fake-vmrun.cmd -- a path with NO spaces and
REM      NO parentheses. The real path is
REM        C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe
REM      and the old :toolsguard interpolated it into a  for /f (' ... ')  body.
REM      cmd.exe parses the ( and ) of (x86) as command-grouping metacharacters
REM      before quote processing, which aborts the WHOLE script with
REM        \VMware\VMware was unexpected at this time.      (exit 255)
REM      Reproduced in tools/_paren-repro.cmd. The stub path made this
REM      impossible to observe.
REM
REM   2. The stub returned only POSITIVE exit codes. Real vmrun exits -1 when it
REM      rejects credentials, and "if errorlevel 1" means "errorlevel >= 1", so
REM      -1 never fires it. Measured in tools/_errorlevel-repro.cmd. Before the
REM      fix, a bogus-credential run of the real script printed
REM        [OK] Credentials authenticate
REM      on the line immediately AFTER vmrun printed its rejection.
REM
REM So this harness now uses tools/"_fake vmrun (x86)"/_fake-vmrun.cmd, whose
REM directory name contains both a space and (x86), and it exercises a negative
REM exit code explicitly. Do not "tidy" that directory name -- the ugliness IS
REM the test.
REM
REM WHAT IS UNDER TEST
REM ------------------
REM :toolsguard is copied here VERBATIM from the fixed guest-enable-ssh-via-tools.cmd.
REM cmd.exe has no parse-check mode, so a broken subroutine only reveals itself
REM when control actually reaches it. Keep the two copies in sync by hand.
REM ---------------------------------------------------------------------------
setlocal EnableExtensions

set "VMRUN=%~dp0_fake vmrun (x86)\_fake-vmrun.cmd"
set "VMX=C:\does\not\matter\Fake.vmx"
set "PASSES=0"
set "FAILS=0"

if not exist "%VMRUN%" (
  echo [ABORT] stub not found at "%VMRUN%"
  echo         Without the stub this harness would test nothing and still pass.
  endlocal
  exit /b 2
)
echo [SETUP] stub = "%VMRUN%"
echo         path contains a space and (x86), matching the real vmrun path.
echo.

REM --- Part 1: :toolsguard returns 0 ONLY for 'running' ----------------------
echo === PART 1: :toolsguard verdicts ===
call :expect running   0 "Tools running"
call :expect installed 1 "Tools installed but not running"
call :expect unknown   1 "Tools state unknown"
call :expect EMPTY     1 "checkToolsState printed nothing"
call :expect NEG       1 "vmrun refused the call and exited -1"
echo.

REM --- Part 2: the exit-code comparison convention ---------------------------
REM This is the part the old harness had no way to express. It proves, against a
REM real negative exit code, that "if errorlevel 1" is the WRONG test and
REM "if not "%%RC%%"=="0"" is the right one.
echo === PART 2: negative exit code handling ===
set "STUBMODE=NEG"
call "%VMRUN%" >nul 2>&1
set "RC=%ERRORLEVEL%"
set "STUBMODE="
echo   captured exit code = [%RC%]

if not "%RC%"=="0" (
  echo   [PASS] compare-against-zero caught the failure.
  set /a PASSES+=1
) else (
  echo   [FAIL] compare-against-zero MISSED a failing exit code.
  set /a FAILS+=1
)

REM Demonstrate the broken idiom on the SAME code. This assertion passes when
REM "if errorlevel 1" does NOT fire -- i.e. it documents the trap rather than
REM asking cmd.exe to behave differently.
set "FIRED=no"
cmd /c exit %RC%
if errorlevel 1 set "FIRED=yes"
if "%FIRED%"=="no" (
  echo   [PASS] confirmed: "if errorlevel 1" does NOT fire on exit %RC%.
  echo          This is exactly why every vmrun check must compare to 0.
  set /a PASSES+=1
) else (
  echo   [NOTE] "if errorlevel 1" DID fire on exit %RC% on this host.
  echo          The trap did not reproduce here, but compare-to-0 is still
  echo          correct and strictly safer. Not counted as a failure.
)
echo.

echo === RESULT: %PASSES% passed, %FAILS% failed ===
if not "%FAILS%"=="0" (
  endlocal
  exit /b 1
)
endlocal
exit /b 0

REM ---------------------------------------------------------------------------
REM :expect <stubmode> <expected-rc> <label>
REM ---------------------------------------------------------------------------
:expect
set "STUBMODE=%~1"
call :toolsguard "harness"
set "GOT=%ERRORLEVEL%"
set "STUBMODE="
if "%GOT%"=="%~2" (
  echo   [PASS] %~3 -- :toolsguard returned %GOT%
  set /a PASSES+=1
) else (
  echo   [FAIL] %~3 -- :toolsguard returned %GOT%, expected %~2
  set /a FAILS+=1
)
exit /b 0

REM ---------------------------------------------------------------------------
REM :toolsguard <label>   -- VERBATIM COPY from guest-enable-ssh-via-tools.cmd
REM
REM Returns 0 if checkToolsState reports 'running' RIGHT NOW, 1 otherwise.
REM Note the temp-file form: %VMRUN% must NEVER be interpolated into a FOR body,
REM because the (x86) parentheses in its real path abort the whole script.
REM ---------------------------------------------------------------------------
:toolsguard
REM ONE DELIBERATE DIVERGENCE FROM THE REAL SCRIPT: the leading  call  below.
REM The real VMRUN is vmrun.EXE and needs no  call ; the harness VMRUN is a
REM batch-file stub, and a batch file invoked from a batch file WITHOUT  call
REM transfers control permanently and never returns. Proved 2026-08-10 by
REM tools/_nocall-repro.cmd, whose "PARENT resumed after no-call invocation"
REM line never printed -- which is exactly how this harness first died, right
REM after its PART 1 banner with zero assertions emitted.
REM
REM The divergence itself was MEASURED rather than assumed, because an unproven
REM stub-vs-real gap is what made the old harness report a false pass.
REM tools/_call-transparency-probe.cmd ran the REAL vmrun.exe both ways:
REM   checkToolsState   direct rc=[0]  out=[running]   | call rc=[0]  out=[running]
REM   bogus credentials direct rc=[-1] out=[Error: Invalid user name...] | call rc=[-1] same
REM Both [MATCH] -- call is transparent for stdout AND for the negative exit
REM code, so this token does not weaken the test.
set "TOOLSTATE="
call "%VMRUN%" checkToolsState "%VMX%" > "%TEMP%\_toolsguard_state.txt" 2>&1
for /f "usebackq delims=" %%S in ("%TEMP%\_toolsguard_state.txt") do set "TOOLSTATE=%%S"
del "%TEMP%\_toolsguard_state.txt" >nul 2>&1
if /i "%TOOLSTATE%"=="running" exit /b 0
echo [TOOLS] checkToolsState = "%TOOLSTATE%" at %~1 -- expected 'running'.
exit /b 1
