@echo off
REM ---------------------------------------------------------------------------
REM _fake-vmrun.cmd -- stub standing in for vmrun.exe in _toolsguard-harness.cmd.
REM
REM WHY THE DIRECTORY NAME IS UGLY
REM ------------------------------
REM This file deliberately lives in a directory whose name contains BOTH a space
REM and the parentheses of "(x86)", because the real program path is
REM   C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe
REM and those parentheses are exactly what broke the caller on 2026-08-10:
REM interpolating that path into a  for /f (' ... ')  body aborts the WHOLE
REM batch script with "\VMware\VMware was unexpected at this time." (exit 255).
REM
REM The PREVIOUS harness stubbed VMRUN as %TEMP%\_fake-vmrun.cmd -- no spaces,
REM no parentheses -- so it reported "5/5 PASS" against a caller that could not
REM run at all. A stub whose path is simpler than the real one cannot test the
REM real one. Do not "tidy" this directory name.
REM
REM MODES (read from the STUBMODE environment variable)
REM ---------------------------------------------------
REM   running / installed / unknown / <anything else>
REM        echo that word, exit 0 -- imitates checkToolsState output.
REM   EMPTY  produce NO output at all, exit 0 -- imitates a checkToolsState
REM          that printed nothing, which is what makes a stale %TOOLSTATE%
REM          dangerous in the caller.
REM   NEG    print vmrun's real credential-rejection line and exit -1. vmrun
REM          really does exit -1 (NEGATIVE) on rejected credentials; that is
REM          why "if errorlevel 1" never fires on it.
REM ---------------------------------------------------------------------------
setlocal EnableExtensions

if /i "%STUBMODE%"=="EMPTY" (
  endlocal
  exit /b 0
)

if /i "%STUBMODE%"=="NEG" (
  echo Error: Invalid user name or password for the guest OS
  endlocal
  exit /b -1
)

echo %STUBMODE%
endlocal
exit /b 0
