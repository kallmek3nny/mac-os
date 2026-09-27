@echo off
REM ---------------------------------------------------------------------------
REM guest-enable-ssh-kenny.cmd
REM
REM Thin wrapper over guest-enable-ssh-via-tools.cmd that removes the ONLY
REM remaining unknown from the operator's hands.
REM
REM WHY THIS EXISTS
REM ---------------
REM t-396740 asked the operator for BOTH the macOS guest account short name and
REM the password. The short name is no longer unknown. On 2026-08-10 the full
REM 8,589,934,592-byte live guest memory image
REM   C:\Users\Kentl\macos-vm\sonoma\564d3ffb-d11c-edd5-265a-225898e79f2b.vmem
REM was scanned end to end:
REM
REM   /Users/kenny/Library                                    x 2106
REM   /Users/kenny/Library/{Preferences,Containers,App Support,Caches} x 631
REM   /Users/kenny/Movies                                     x   20
REM   /Users/<not-kenny>/Library/{Preferences,Containers,...} x    0
REM
REM Bare /Users/<name> hits for aliu(9), rdimac(4), eric(3), cloud(5) are Apple
REM BUILD-MACHINE paths baked into shipped binaries -- they carry no runtime
REM subpaths. Only an account that has actually logged in leaves thousands of
REM resident Library/Preferences references. Exactly one name does.
REM
REM So MACOS_GUEST_USER is hardcoded to 'kenny' here and the operator supplies
REM only the password.
REM
REM WHAT IS NOT PROVEN
REM ------------------
REM That 'kenny' can AUTHENTICATE. Short name and password are separate facts.
REM Do NOT try to confirm the short name from vmrun's error text: measured on
REM 2026-08-10, both of these produce the BYTE-IDENTICAL message at exit 127:
REM
REM   vmrun -gu kenny           -gp <wrong>  ->  Invalid user name or password
REM   vmrun -gu zzznosuchuser9c -gp <wrong>  ->  Invalid user name or password
REM
REM The error is not a username oracle. If this wrapper fails to authenticate,
REM the password is the thing to question first, and repeated guessing risks
REM tripping the guest account lockout.
REM
REM NETWORK CAVEAT (does NOT affect this script)
REM --------------------------------------------
REM The host currently cannot reach the guest IP 192.168.255.129 at all --
REM ProtonVPN's kill switch (IsKillSwitchEnabled=true, KillSwitchMode=Standard,
REM no LAN-allow key) refuses every off-host private destination with WSAEACCES.
REM Proof it is not a VMware fault: the PHYSICAL Wi-Fi gateway 192.168.0.1:80
REM fails identically, while 8.8.8.8:53 and the host's own 192.168.255.1:445
REM connect fine.
REM
REM This script is UNAFFECTED: vmrun guest ops ride the VMCI/backdoor channel to
REM vmware-vmx.exe on the host and never touch TCP/IP. But the post-run SSH
REM verification FROM the host will fail for that unrelated reason. To actually
REM ssh in afterwards, enable 'Allow LAN connections' in the ProtonVPN client or
REM disconnect the VPN.
REM
REM USAGE
REM -----
REM   tools\guest-enable-ssh-kenny.cmd <password>
REM
REM or, to keep the password out of your command history / this file's argv:
REM
REM   set /p MACOS_GUEST_PASS=password:
REM   tools\guest-enable-ssh-kenny.cmd
REM ---------------------------------------------------------------------------

setlocal

set "MACOS_GUEST_USER=kenny"

if not "%~1"=="" set "MACOS_GUEST_PASS=%~1"

if "%MACOS_GUEST_PASS%"=="" (
    echo [FAIL] No password supplied.
    echo.
    echo         MACOS_GUEST_USER is already set to 'kenny' by this wrapper --
    echo         that half is settled from guest memory evidence. Only the
    echo         password is missing.
    echo.
    echo         Pass it as an argument:
    echo             tools\guest-enable-ssh-kenny.cmd yourpassword
    echo.
    echo         Or set it first, to keep it out of argv:
    echo             set /p MACOS_GUEST_PASS=password:
    echo             tools\guest-enable-ssh-kenny.cmd
    echo.
    endlocal
    exit /b 3
)

echo [INFO] MACOS_GUEST_USER=kenny  ^(from live-guest-memory evidence, 2026-08-10^)
echo [INFO] MACOS_GUEST_PASS=^<supplied, %COMPUTERNAME% session^>
echo [INFO] Delegating to guest-enable-ssh-via-tools.cmd ...
echo.

REM NO ARGUMENTS ARE FORWARDED, DELIBERATELY.
REM guest-enable-ssh-via-tools.cmd reads MACOS_GUEST_USER / MACOS_GUEST_PASS from
REM the ENVIRONMENT only -- it never looks at %%1. An earlier revision called it
REM with %%* , which forwarded the password as an argument the callee ignores,
REM placing the cleartext password on a second process command line for zero
REM benefit. setlocal above already scopes both variables to this run, so the
REM child inherits them through the environment and nothing else needs passing.
call "%~dp0guest-enable-ssh-via-tools.cmd"
set "RC=%ERRORLEVEL%"

echo.
echo [INFO] guest-enable-ssh-via-tools.cmd exited with %RC%
if not "%RC%"=="0" (
    echo [HINT] If this was an authentication failure, question the PASSWORD, not
    echo        the short name -- vmrun returns the same error for a wrong password
    echo        on a real account as it does for a nonexistent account.
)

endlocal ^& exit /b %RC%
