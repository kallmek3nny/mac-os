@echo off
REM ---------------------------------------------------------------------------
REM _keyresolve-harness.cmd -- unit test for the public-key resolution block
REM added to guest-enable-ssh-via-tools.cmd.
REM
REM WHY THIS EXISTS
REM ---------------
REM The real block runs AFTER the credential gate, so the only cheap test of the
REM real script (the no-creds refusal) exits before ever reaching it. Testing it
REM for real would mean contacting the live Sonoma guest with credentials that do
REM not exist yet.
REM
REM The specific risks being tested, none of which are obvious:
REM
REM   * `for /f "usebackq delims="` on a path containing SPACES -- usebackq is
REM     required precisely because the path is double-quoted; without it cmd
REM     treats the quoted string as a literal to parse, not a filename.
REM   * `if not defined GUEST_KEY` inside a for-loop body with NO delayed
REM     expansion. This is the load-bearing trick: `defined` is evaluated at
REM     execution time, so it correctly keeps only the FIRST line, whereas
REM     `if "%GUEST_KEY%"==""` would be frozen at parse time and keep the LAST.
REM   * a multi-line .pub file must yield exactly the first line.
REM   * a missing file must yield an EMPTY key and continue, not abort.
REM
REM The block below is a VERBATIM copy of the one in the real script.
REM ---------------------------------------------------------------------------

setlocal EnableExtensions
set "FAILURES=0"
set "SANDBOX=%TEMP%\_keyresolve test dir"
REM ^ deliberately contains a space, mirroring "D:\nerd shit\other projects"

if exist "%SANDBOX%" rmdir /s /q "%SANDBOX%" >nul 2>&1
mkdir "%SANDBOX%" >nul 2>&1

set "KEYLINE=ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIPFGC+QqUOIS/ZqcceE8rN1uQzFwNKE08XGOtCgNTb3T kentl-windows"
> "%SANDBOX%\good.pub" echo %KEYLINE%

> "%SANDBOX%\multi.pub" echo %KEYLINE%
>>"%SANDBOX%\multi.pub" echo ssh-ed25519 AAAASECONDKEYSHOULDBEIGNORED second-key

echo === CASE 1: key file present, path contains a space ===
set "MACOS_GUEST_KEYFILE=%SANDBOX%\good.pub"
call :resolvekey
if "%GUEST_KEY%"=="%KEYLINE%" (
  echo   [ok] read back byte-for-byte
) else (
  echo   [FAIL] expected: %KEYLINE%
  echo          got     : %GUEST_KEY%
  set /a FAILURES+=1
)
echo.

echo === CASE 2: missing key file -^> empty, must NOT abort ===
set "MACOS_GUEST_KEYFILE=%SANDBOX%\does-not-exist.pub"
call :resolvekey
if defined GUEST_KEY (
  echo   [FAIL] GUEST_KEY should be empty, got: %GUEST_KEY%
  set /a FAILURES+=1
) else (
  echo   [ok] empty, control continued past the block
)
echo.

echo === CASE 3: multi-line .pub -^> FIRST line only ===
REM This is what `if not defined` buys us. With `if "%%GUEST_KEY%%"==""` the
REM comparison is frozen at parse time and every iteration overwrites, so the
REM SECOND key would win and the wrong key would be authorised.
set "MACOS_GUEST_KEYFILE=%SANDBOX%\multi.pub"
call :resolvekey
if "%GUEST_KEY%"=="%KEYLINE%" (
  echo   [ok] first line kept, second ignored
) else (
  echo   [FAIL] wrong line selected: %GUEST_KEY%
  set /a FAILURES+=1
)
echo.

echo === CASE 4: quoting survives the guest command line ===
REM Step 4 embeds the key as '%%GUEST_KEY%%' inside a single-quoted shell arg.
REM The key contains SPACES and a '+' and a '/'. Verify none of that breaks the
REM surrounding quotes, i.e. the composed string still has exactly one pair.
set "MACOS_GUEST_KEYFILE=%SANDBOX%\good.pub"
call :resolvekey
set "COMPOSED=/bin/sh /tmp/guest-enable-ssh.sh '%GUEST_KEY%'"
echo   composed: %COMPOSED%
echo %COMPOSED% | findstr /c:"'ssh-ed25519 " >nul
if errorlevel 1 (
  echo   [FAIL] key did not survive into the quoted argument
  set /a FAILURES+=1
) else (
  echo   [ok] key is inside the single-quoted argument, spaces intact
)
echo %COMPOSED% | findstr /c:"kentl-windows'" >nul
if errorlevel 1 (
  echo   [FAIL] closing quote not immediately after the key comment
  set /a FAILURES+=1
) else (
  echo   [ok] argument closes correctly after the trailing comment
)
echo.

echo === CASE 5: empty key degrades to '' (password-only, not a crash) ===
set "GUEST_KEY="
set "COMPOSED=/bin/sh /tmp/guest-enable-ssh.sh '%GUEST_KEY%'"
echo   composed: %COMPOSED%
echo %COMPOSED% | findstr /c:"guest-enable-ssh.sh ''" >nul
if errorlevel 1 (
  echo   [FAIL] empty key did not produce a well-formed empty argument
  set /a FAILURES+=1
) else (
  echo   [ok] degrades to '' -- guest script reads that as "no key"
)
echo.

rmdir /s /q "%SANDBOX%" >nul 2>&1

echo ==========================================================
if "%FAILURES%"=="0" (
  echo [PASS] all key-resolution cases behave as specified.
  exit /b 0
)
echo [FAIL] %FAILURES% key-resolution case^(s^) misbehaved.
exit /b 1

REM --- VERBATIM copy of the block under test ---------------------------------
:resolvekey
set "GUEST_KEY="
if exist "%MACOS_GUEST_KEYFILE%" (
  for /f "usebackq delims=" %%K in ("%MACOS_GUEST_KEYFILE%") do (
    if not defined GUEST_KEY set "GUEST_KEY=%%K"
  )
)
exit /b 0
