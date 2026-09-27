@echo off
REM Register one-shot Task Scheduler jobs that run _token-probe.py outside the
REM agent shell, so we can compare injection capability across token types.
REM
REM Background (2026-08-07): SendInput from the agent shell reports sent=1 err=0
REM and delivers nothing; SetCursorPos returns FALSE; BlockInput returns err=5.
REM
REM TokenIsRestricted=1 was briefly blamed. It is NOT the cause: a Task Scheduler
REM /IT /RL LIMITED process showed the SAME TokenIsRestricted=1 and the SAME
REM no-op. On an admin account every non-elevated process carries a UAC-filtered
REM (= restricted) token, so that flag is normal and proves nothing.
REM
REM Pass HIGHEST as %1 to register the elevated variant instead. Elevation is the
REM remaining discriminator: if an elevated process CAN inject, the blocker is
REM integrity/UIPI-shaped; if it also cannot, the block is host-wide (a thread
REM holding BlockInput(TRUE), or a low-level hook swallowing injected input) and
REM no token change will fix it.
REM
REM This lives in a .cmd file because the paths contain spaces AND schtasks /TR
REM needs its own nested quoting -- Git Bash mangles that into unrecognised
REM arguments every time.

set "PY=C:\Python314\python.exe"
set "PROBE=D:\nerd shit\other projects\mac os\tools\_token-probe.py"

set "LEVEL=LIMITED"
set "TASK=VmTypeProbe"
set "OUT=D:\nerd shit\other projects\mac os\shots\token-sched.txt"
if /i "%~1"=="HIGHEST" (
  set "LEVEL=HIGHEST"
  set "TASK=VmTypeProbeElev"
  set "OUT=D:\nerd shit\other projects\mac os\shots\token-elev.txt"
)

schtasks /Delete /TN %TASK% /F >nul 2>&1
schtasks /Create /TN %TASK% /SC ONCE /ST 00:00 /IT /RL %LEVEL% /F /TR "'%PY%' '%PROBE%' --out '%OUT%'"
if errorlevel 1 (
  echo CREATE-FAILED
  exit /b 1
)
schtasks /Run /TN %TASK%
exit /b %errorlevel%
