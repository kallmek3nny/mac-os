@echo off
REM ---------------------------------------------------------------------------
REM guest-enable-ssh-via-tools.cmd
REM
REM Enable Remote Login (sshd) inside the macOS guest via VMware Tools, with NO
REM keyboard/scancode injection. This exists because, as of 2026-08-07 22:47Z:
REM
REM   * scancode injection is DEAD on this host -- tools/_input-block-probe.py
REM     reports  block: held_by_other=True (BlockInput(F) ret=False err=5),
REM     DELIVERED=False. Some other process holds the system-wide input block.
REM   * VMware Tools IS running in the guest -- vmrun checkToolsState -> running
REM     (VMX pid=4288, boot log 22:39:23Z, guest RPC 'toolbox: Version 12.1.1.12'
REM     at 22:41:26Z). That is guest-originated positive evidence, not absence.
REM
REM So the Tools route is open and is strictly better than typing at a picture.
REM
REM CREDENTIALS
REM -----------
REM vmrun guest operations are NOT anonymous on this VM. Without -gu/-gp you get:
REM   "Error: Anonymous guest operations are not allowed on this virtual machine.
REM    You must call VixVM_LoginInGuest before performing guest operations"
REM and runProgramInGuest will HANG on an interactive "Guest user:" prompt.
REM
REM Credentials are read from the environment ONLY. They are deliberately not
REM stored in this file or anywhere else in the repo:
REM
REM   set MACOS_GUEST_USER=<the account short name from Setup Assistant>
REM   set MACOS_GUEST_PASS=<that account's password>
REM   tools\guest-enable-ssh-via-tools.cmd
REM
REM The account must be an ADMIN account -- `systemsetup -setremotelogin on`
REM requires root, and this script reaches root via `sudo -S` fed from stdin.
REM
REM WHAT IT DOES, IN ORDER
REM ----------------------
REM   0. refuses to run if creds are missing (never prompts -> never hangs)
REM   1. checkToolsState must say 'running'
REM   2. proves the credentials actually authenticate, cheaply, before doing
REM      anything that mutates the guest
REM   3. copies tools/guest-enable-ssh.sh into the guest
REM   4. runs it under sudo, PASSING THE HOST PUBLIC KEY as $1, capturing
REM      stdout/stderr back to the host
REM   5. VERIFIES independently: `systemsetup -getremotelogin` says On AND
REM      launchctl shows the ssh job AND port 22 is actually listening
REM
REM THE PUBLIC KEY ARGUMENT
REM -----------------------
REM guest-enable-ssh.sh takes an optional authorized_keys entry as $1. Earlier
REM revisions of this wrapper invoked it with NO argument, so AUTHKEY was always
REM empty, ~/.ssh/authorized_keys was never written, and the guest stayed
REM password-only -- meaning MACOS_GUEST_PASS had to be re-supplied for every
REM future session forever. That defeated the point of enabling sshd at all.
REM
REM The key is read from a FILE, never from the command line:
REM   default   %USERPROFILE%\.ssh\id_ed25519.pub
REM   override  set MACOS_GUEST_KEYFILE=C:\path\to\some_key.pub
REM
REM A missing key file is a WARNING, not a failure -- the run continues and
REM enables password-only Remote Login, exactly as before. It is reported
REM explicitly so nobody later assumes key auth was set up when it was not.
REM
REM Step 5 matters. "the command exited 0" is not proof sshd is up; only the
REM listening socket is.
REM
REM THE TOOLS DAEMON IS NOT CONTINUOUSLY UP
REM ---------------------------------------
REM checkToolsState is a POINT-IN-TIME SAMPLE, not a guarantee. Measured on this
REM very VM (VMX pid=4288, current boot), from vmware.log:
REM   22:40:23Z  'Tools: Tools heartbeat timeout'
REM   22:41:26Z  toolbox comes up -- guest RPC 'toolbox: Version 12.1.1.12'
REM   22:41:44Z  'TOOLS: appName=toolbox, oldStatus=1, status=0, guestInitiated=1'
REM              -- the GUEST tore it down deliberately, 18 seconds later.
REM
REM So the daemon can vanish BETWEEN two steps of this script. That failure mode
REM looks exactly like a wrong password if you do not distinguish it, which is how
REM an operator ends up re-typing correct credentials for an hour. Every step below
REM therefore re-checks Tools before it runs, via :toolsguard, and a drop is
REM reported as exit 10 ("Tools dropped") -- never as exit 5 ("auth failed").
REM ---------------------------------------------------------------------------

setlocal EnableExtensions

set "VMRUN=C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe"
set "VMX=C:\Users\Kentl\macos-vm\sonoma\Sonoma.vmx"
set "LOCAL_SCRIPT=%~dp0guest-enable-ssh.sh"
set "GUEST_SCRIPT=/tmp/guest-enable-ssh.sh"
set "GUEST_OUT=/tmp/guest-enable-ssh.out"
set "HOST_OUT=%~dp0..\shots\guest-enable-ssh.out.txt"
if "%MACOS_GUEST_KEYFILE%"=="" set "MACOS_GUEST_KEYFILE=%USERPROFILE%\.ssh\id_ed25519.pub"

if not exist "%VMRUN%" (
  echo [FAIL] vmrun not found at "%VMRUN%"
  exit /b 2
)
if not exist "%VMX%" (
  echo [FAIL] vmx not found at "%VMX%"
  exit /b 2
)
if not exist "%LOCAL_SCRIPT%" (
  echo [FAIL] guest script not found at "%LOCAL_SCRIPT%"
  exit /b 2
)

if "%MACOS_GUEST_USER%"=="" goto :nocreds
if "%MACOS_GUEST_PASS%"=="" goto :nocreds
goto :havecreds

:nocreds
echo [FAIL] MACOS_GUEST_USER and/or MACOS_GUEST_PASS are not set.
echo.
echo         Refusing to run. Calling vmrun guest ops without -gu/-gp does not
echo         fail fast -- it blocks on an interactive "Guest user:" prompt and
echo         hangs whatever shell called it.
echo.
echo         Set them for this shell only, then re-run:
echo             set MACOS_GUEST_USER=your_short_name
echo             set MACOS_GUEST_PASS=your_password
echo             tools\guest-enable-ssh-via-tools.cmd
exit /b 3

:havecreds

REM --- resolve the host public key BEFORE touching the guest --------------------
REM Read the first line of the .pub file. Doing this here means a bad path is
REM reported before any guest state changes, and the whole run stays offline
REM until step 2.
set "GUEST_KEY="
if exist "%MACOS_GUEST_KEYFILE%" (
  for /f "usebackq delims=" %%K in ("%MACOS_GUEST_KEYFILE%") do (
    if not defined GUEST_KEY set "GUEST_KEY=%%K"
  )
)
if defined GUEST_KEY (
  echo [KEY] will authorise from "%MACOS_GUEST_KEYFILE%"
  echo       %GUEST_KEY%
) else (
  echo [KEY] no usable public key at "%MACOS_GUEST_KEYFILE%".
  echo       Continuing with PASSWORD-ONLY auth. Key auth is NOT being set up.
  echo       Override with:  set MACOS_GUEST_KEYFILE=C:\path\to\key.pub
)
echo.

echo === STEP 1: VMware Tools state ===
call :toolsguard "step 1 (initial gate)"
if errorlevel 1 (
  echo [FAIL] Tools are not running. Guest ops unavailable.
  echo        'unknown' can also mean the guest is still booting -- re-check in a minute.
  exit /b 4
)
echo [OK] Tools running.
echo.

echo === STEP 2: prove the credentials authenticate ===
call :toolsguard "step 2 (authenticate)"
if errorlevel 1 exit /b 10
REM Cheap, read-only, non-mutating. If the password is wrong we find out HERE,
REM before we have copied or executed anything in the guest.
"%VMRUN%" -gu "%MACOS_GUEST_USER%" -gp "%MACOS_GUEST_PASS%" runProgramInGuest "%VMX%" /usr/bin/true
set "AUTHRC=%ERRORLEVEL%"
REM DO NOT use "if errorlevel 1" on a vmrun call. vmrun exits -1 -- a NEGATIVE
REM code -- when it rejects credentials. Measured 2026-08-10 by
REM tools/_errorlevel-repro.cmd: vmrun printed "Invalid user name or password",
REM the captured %ERRORLEVEL% was [-1], and "if errorlevel 1" DID NOT FIRE,
REM because "if errorlevel N" means "errorlevel >= N" and -1 is not >= 1.
REM Consequence before this fix: a bogus-credential run printed "[OK]
REM Credentials authenticate" on the line immediately after vmrun's rejection,
REM and every later step reported [OK] too. Comparing against 0 catches
REM negative AND positive failure codes.
if not "%AUTHRC%"=="0" (
  REM Distinguish a real auth failure from the daemon dropping mid-call. If Tools
  REM went away, the credentials are UNJUDGED -- saying "login failed" here would
  REM send the operator hunting a password that was never wrong.
  call :toolsguard "step 2 (post-failure recheck)"
  if errorlevel 1 (
    echo [FAIL] Tools dropped DURING the authentication attempt.
    echo        The credentials were NOT judged -- this is not an auth failure.
    echo        Re-run once checkToolsState reports 'running' again.
    exit /b 10
  )
  echo [FAIL] Guest login failed for user "%MACOS_GUEST_USER%".
  echo        Tools were still running, so this is a genuine credential rejection.
  echo        Check the SHORT name ^(not the full display name^) and the password.
  echo        Nothing was changed in the guest.
  exit /b 5
)
echo [OK] Credentials authenticate as "%MACOS_GUEST_USER%".
echo.

echo === STEP 3: copy the enable script into the guest ===
call :toolsguard "step 3 (copy in)"
if errorlevel 1 exit /b 10
"%VMRUN%" -gu "%MACOS_GUEST_USER%" -gp "%MACOS_GUEST_PASS%" CopyFileFromHostToGuest "%VMX%" "%LOCAL_SCRIPT%" "%GUEST_SCRIPT%"
set "COPYRC=%ERRORLEVEL%"
REM Compare against 0, not "errorlevel 1" -- see the note at step 2: vmrun
REM signals credential rejection with -1, which "if errorlevel 1" misses.
if not "%COPYRC%"=="0" (
  echo [FAIL] CopyFileFromHostToGuest failed. vmrun exit = %COPYRC%
  echo        A negative code means vmrun refused the call itself.
  exit /b 6
)
echo [OK] copied -^> %GUEST_SCRIPT%
echo.

echo === STEP 4: run it as root ===
call :toolsguard "step 4 (sudo run)"
if errorlevel 1 exit /b 10
REM sudo -S reads the password from stdin, because there is no TTY here.
REM Everything is teed to %GUEST_OUT% so we can pull the real output back even
REM when the exit code alone would tell us nothing useful.
REM $1 is the authorized_keys entry. Single-quoted in the guest shell so the
REM spaces in "ssh-ed25519 AAAA... comment" stay one argument. An empty
REM %GUEST_KEY% degrades to '' , which guest-enable-ssh.sh treats as "no key".
"%VMRUN%" -gu "%MACOS_GUEST_USER%" -gp "%MACOS_GUEST_PASS%" runScriptInGuest "%VMX%" /bin/sh "printf '%%s\n' '%MACOS_GUEST_PASS%' | /usr/bin/sudo -S /bin/sh %GUEST_SCRIPT% '%GUEST_KEY%' > %GUEST_OUT% 2>&1; echo \"SCRIPT_EXIT=$?\" >> %GUEST_OUT%"
set "RUNRC=%ERRORLEVEL%"
echo vmrun runScriptInGuest exit = %RUNRC%
if not "%RUNRC%"=="0" (
  echo [WARN] non-zero exit for the sudo run. A NEGATIVE code here means vmrun
  echo        itself refused the call -- -1 = credentials rejected -- so the
  echo        guest script never ran at all. Step 6 verifies independently;
  echo        do not report success or failure from this line alone.
)
echo.

echo === STEP 5: pull the guest-side output back to the host ===
call :toolsguard "step 5 (copy out)"
if errorlevel 1 exit /b 10
"%VMRUN%" -gu "%MACOS_GUEST_USER%" -gp "%MACOS_GUEST_PASS%" CopyFileFromGuestToHost "%VMX%" "%GUEST_OUT%" "%HOST_OUT%"
set "OUTRC=%ERRORLEVEL%"
REM Compare against 0 -- "if errorlevel 1" misses vmrun's negative codes.
if not "%OUTRC%"=="0" (
  echo [WARN] could not copy %GUEST_OUT% back; relying on verification below.
) else (
  echo ---- guest output ----
  type "%HOST_OUT%"
  echo ----------------------
)
echo.

echo === STEP 6: INDEPENDENT VERIFICATION ===
call :toolsguard "step 6 (verify)"
if errorlevel 1 (
  echo [FAIL] Tools dropped before verification could run.
  echo        The enable script may well have succeeded -- that is UNPROVEN, not failed.
  echo        Re-run once Tools are back before reporting any outcome either way.
  exit /b 10
)
REM Do not trust step 4's exit code. Ask the guest three separate questions.
"%VMRUN%" -gu "%MACOS_GUEST_USER%" -gp "%MACOS_GUEST_PASS%" runScriptInGuest "%VMX%" /bin/sh "{ echo '--- getremotelogin ---'; /usr/sbin/systemsetup -getremotelogin 2>&1; echo '--- launchctl ---'; /bin/launchctl print system/com.openssh.sshd 2>&1 | head -5; /bin/launchctl list 2>&1 | grep -i ssh; echo '--- listening on 22 ---'; /usr/sbin/netstat -an 2>/dev/null | grep '\.22 ' | grep -i listen; echo '--- ip ---'; /sbin/ifconfig en0 2>&1 | grep 'inet '; } > /tmp/ssh-verify.out 2>&1"
set "GENRC=%ERRORLEVEL%"
REM Capture it. Until 2026-08-13 this call's exit code was DISCARDED: the copy on
REM the next line ran immediately, so the %ERRORLEVEL% captured below belonged to
REM the COPY and never to the generation. A generation that did not run still let
REM the copy succeed -- against a /tmp/ssh-verify.out left behind by an EARLIER
REM run -- and step 6, whose entire purpose is to not trust step 4, would have
REM reported that stale file as this run's independent evidence. The host-side
REM staleness guard at the end of this step cannot see it: the file it checks was
REM genuinely written by this run's copy.
if not "%GENRC%"=="0" (
  echo [FAIL] could not run the verification script in the guest. vmrun exit = %GENRC%
  exit /b 7
)
"%VMRUN%" -gu "%MACOS_GUEST_USER%" -gp "%MACOS_GUEST_PASS%" CopyFileFromGuestToHost "%VMX%" /tmp/ssh-verify.out "%~dp0..\shots\ssh-verify.out.txt"
set "VERRC=%ERRORLEVEL%"
REM Compare against 0 -- "if errorlevel 1" misses vmrun's negative codes.
if not "%VERRC%"=="0" (
  echo [FAIL] could not retrieve verification output. vmrun exit = %VERRC%
  exit /b 7
)
REM A STALE ssh-verify.out.txt from an earlier run would otherwise be re-read by
REM the findstr checks below and reported as this run's evidence.
if not exist "%~dp0..\shots\ssh-verify.out.txt" (
  echo [FAIL] verification file does not exist after a reported-successful copy.
  exit /b 7
)
echo ---- verification ----
type "%~dp0..\shots\ssh-verify.out.txt"
echo ----------------------
echo.

findstr /i /c:"Remote Login: On" "%~dp0..\shots\ssh-verify.out.txt" >nul
if errorlevel 1 (
  echo [FAIL] systemsetup does not report "Remote Login: On". SSH is NOT enabled.
  exit /b 8
)
findstr /i /c:"LISTEN" "%~dp0..\shots\ssh-verify.out.txt" >nul
if errorlevel 1 (
  echo [WARN] Remote Login reports On, but no LISTEN socket on port 22 was seen.
  echo        Report this as "enabled but unproven", not as done.
  exit /b 9
)

echo [PASS] Remote Login is On AND port 22 is listening in the guest.
echo        Host-side reachability is a separate question -- the guest is on
echo        vmxnet3/NAT, so test from the host with the en0 address printed above.
exit /b 0

REM ---------------------------------------------------------------------------
REM :toolsguard <label>
REM
REM Returns 0 if checkToolsState reports 'running' RIGHT NOW, 1 otherwise.
REM Called before every guest op because the daemon has been observed going
REM status 1 -> 0 with guestInitiated=1 within 18 seconds of coming up on this
REM VM. The <label> is echoed on failure so the log says WHICH step lost Tools.
REM ---------------------------------------------------------------------------
:toolsguard
set "TOOLSTATE="
REM Do NOT put %VMRUN% inside a FOR body. Its path is
REM "C:\Program Files (x86)\..." and cmd.exe treats the parentheses of (x86)
REM as command-grouping metacharacters before quote processing, aborting the
REM WHOLE script with "\VMware\VMware was unexpected at this time." (observed
REM 2026-08-10, exit 255, reproduced in tools/_paren-repro.cmd). The old unit
REM test tools/_toolsguard-harness.cmd could not catch this: it stubbed VMRUN
REM with %TEMP%\_fake-vmrun.cmd, a path with no spaces and no parentheses.
REM Redirect to a file, then read the FILE -- validated in tools/_paren-repro-b.cmd.
"%VMRUN%" checkToolsState "%VMX%" > "%TEMP%\_toolsguard_state.txt" 2>&1
for /f "usebackq delims=" %%S in ("%TEMP%\_toolsguard_state.txt") do set "TOOLSTATE=%%S"
del "%TEMP%\_toolsguard_state.txt" >nul 2>&1
if /i "%TOOLSTATE%"=="running" exit /b 0
echo [TOOLS] checkToolsState = "%TOOLSTATE%" at %~1 -- expected 'running'.
exit /b 1
