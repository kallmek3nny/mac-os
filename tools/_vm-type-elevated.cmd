@echo off
REM _vm-type-elevated.cmd -- run vm-type.py with an ELEVATED token and capture
REM its output to a file.
REM
REM WHY THIS EXISTS (2026-08-07)
REM ----------------------------
REM Unelevated processes on this host cannot inject input. Measured, with the
REM cursor position as the post-condition -- never SendInput's return value:
REM
REM   SendInput     sent=1 err=0     cursor unmoved   DELIVERED=False
REM   mouse_event   err=0            cursor unmoved   DELIVERED=False
REM   SetCursorPos  ret=False err=0  cursor unmoved   DELIVERED=False
REM
REM The operator confirmed the converse by accepting an admin Python UAC prompt
REM and SEEING THE MOUSE MOVE. So elevation is the fix.
REM
REM Six other causes were blamed first and all six were wrong: bad coordinates
REM (a real bug, fixed, but not the blocker), UIPI between vmware.exe and
REM python.exe (both MEDIUM IL), a thread holding BlockInput(TRUE) (err=5 is
REM also what a UAC-filtered token returns), a restricted token (Task Scheduler
REM /IT reproduced TokenIsRestricted=1 and the same no-op), a non-interactive
REM window station (WinSta0, WSF_VISIBLE=1, desktop Default), and inline API
REM hooks (the ff25 prologues are normal user32->win32u forwarders).
REM
REM WHY A .CMD AND NOT A DIRECT Start-Process
REM -----------------------------------------
REM An elevated child is a NEW process with its own console -- it shares no
REM stdout with the launching shell, so `Start-Process -Verb RunAs python ...`
REM injects correctly and you see NOTHING. The redirection has to happen inside
REM the elevated process. That is this file: cmd redirects both streams to a log
REM the unelevated side can read.
REM
REM CORRECTION 2026-08-07 ~21:15Z (bold-wren-29): the line above used to read
REM "vm-type.py has no --out flag". It does now -- I added it earlier the same
REM day (tee over stdout+stderr, and it appends "exit : N" to the file so a
REM caller holding only the file can still tell success from failure).
REM
REM This wrapper is still the better entry point, and not redundant with --out:
REM   * it asserts elevation actually happened (`net session`) -- a dismissed
REM     UAC prompt otherwise yields a Medium-IL run whose "accepted by
REM     SendInput" count is meaningless;
REM   * it gates on _input-block-probe.py BEFORE injecting, so a non-delivering
REM     host aborts instead of producing a confident no-op;
REM   * it captures the probe's exit code and vm-type.py's separately;
REM   * it avoids Git Bash mangling the nested quoting of space-bearing paths.
REM
REM Use --out IN ADDITION when you want vm-type.py's own report isolated from
REM this log. Note the Start-Process quoting trap either way: -ArgumentList
REM joins elements with spaces WITHOUT quoting them, so '--text','echo hi'
REM reaches the child as "--text echo" plus a stray positional "hi" --
REM argparse exits 2 before --out is wired up, leaving NO file at all. Pass
REM '"echo hi"' with inner quotes. Exit 2 with no --out file is an
REM argument-parsing failure, not a window-targeting failure.
REM
REM Paths are also why this is a .cmd -- they contain spaces, and Git Bash
REM mangles the nested quoting every time.
REM
REM USAGE (from an unelevated shell -- this triggers the UAC prompt):
REM
REM   powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath \"D:\nerd shit\other projects\mac os\tools\_vm-type-elevated.cmd\" -ArgumentList '--window','Sonoma','--click'"
REM
REM then read the log:
REM
REM   cat "D:/nerd shit/other projects/mac os/shots/vm-type-elevated.log"
REM
REM The log is TRUNCATED on every run, so a stale success line can never be
REM mistaken for a fresh one.

set "PY=C:\Python314\python.exe"
set "TOOL=D:\nerd shit\other projects\mac os\tools\vm-type.py"
set "LOG=D:\nerd shit\other projects\mac os\shots\vm-type-elevated.log"

> "%LOG%" echo === _vm-type-elevated.cmd ===
>>"%LOG%" echo args: %*

REM Prove elevation actually happened. If this says NOT elevated, the UAC
REM prompt was dismissed and vm-type.py's "accepted by SendInput" count below
REM is meaningless -- at Medium integrity every event is accepted and discarded.
set "ELEV=unknown"
net session >nul 2>&1
if errorlevel 1 (
  set "ELEV=NOT ELEVATED"
  >>"%LOG%" echo elevation: NOT ELEVATED -- injection will silently no-op
) else (
  set "ELEV=ELEVATED"
  >>"%LOG%" echo elevation: ELEVATED
)

REM Delivery gate BEFORE the real injection. _input-block-probe.py exits 0 only
REM when an injected mouse move produces an OBSERVED cursor-position change.
>>"%LOG%" echo --- delivery probe ---
"%PY%" "D:\nerd shit\other projects\mac os\tools\_input-block-probe.py" >>"%LOG%" 2>&1
set "PROBE=%errorlevel%"
>>"%LOG%" echo probe exit: %PROBE%
if not "%PROBE%"=="0" (
  REM Report the MEASURED elevation state, never an assumed one. The previous
  REM wording here was the unconditional string "not delivering even elevated",
  REM which this file printed on 2026-08-07 21:00Z into a log whose own line 3
  REM read "NOT ELEVATED" -- the UAC prompt had been dismissed. A reader who
  REM trusted the abort line would have concluded elevation was ruled out when
  REM the run never tested it. Same class of error as reading SendInput's
  REM return value as delivery: asserting a condition instead of measuring it.
  >>"%LOG%" echo ABORTED: injection is not delivering at %ELEV%. Do NOT read
  >>"%LOG%" echo vm-type.py's "accepted by SendInput" count as delivery.
  if "%ELEV%"=="NOT ELEVATED" (
    >>"%LOG%" echo This run says nothing about elevated behaviour -- the UAC
    >>"%LOG%" echo prompt was dismissed. Re-run and ACCEPT it before diagnosing.
  ) else (
    >>"%LOG%" echo Elevation was confirmed and delivery still failed. Diagnose
    >>"%LOG%" echo before retrying.
  )
  exit /b 1
)

>>"%LOG%" echo --- vm-type.py ---
"%PY%" "%TOOL%" %* >>"%LOG%" 2>&1
set "RC=%errorlevel%"
>>"%LOG%" echo vm-type exit: %RC%
exit /b %RC%
