@echo off
REM
REM Run the host-side Python tool tests.
REM
REM These are separate from build-msvc.bat on purpose: that script tests the
REM kext's C++ decode logic and needs MSVC, CMake and Ninja. These need only
REM CPython and no third-party packages, so they run anywhere and are the one
REM gate that always works on a bare checkout.
REM
REM Usage (from anywhere):
REM   tests\run-python-tests.bat
REM

setlocal

cd /d "%~dp0.."

REM Capture the run to a log before printing it. 2026-08-11: a single
REM run reported "Ran 307 tests" + "FAILED (errors=1)" and four immediate
REM re-runs passed. The traceback was gone, because this script streamed
REM straight to the console and nothing kept a copy -- so the one artifact
REM that could have identified the fault did not exist. A transient failure
REM you cannot read is indistinguishable from a transient failure you
REM imagined, and the temptation is then to file it as flakiness.
REM
REM %RANDOM% rather than a fixed name on purpose: several terminals run this
REM gate concurrently in this project, and a fixed log path would have one
REM run's traceback overwritten by another run's pass. The path is printed
REM so the log is findable.
set "GATELOG=%TEMP%\macos-gpu-python-gate-%RANDOM%%RANDOM%.log"

REM unittest discover rather than a hardcoded file list. An earlier version of
REM the C++ wrapper ran one hardcoded exe and silently stopped running a suite
REM that had been added since; discovery avoids repeating that mistake here.
python -m unittest discover -s tests -p "test_*.py" -v > "%GATELOG%" 2>&1
set "GATERC=%errorlevel%"

type "%GATELOG%"

if not "%GATERC%"=="0" (
  echo.
  echo [FAIL] python gate exited %GATERC%
  echo        full output retained at: %GATELOG%
  echo        re-runs may pass; that log is the only record of THIS run.
  exit /b %GATERC%
)

REM Only delete on success. A passing run's log has no value; a failing
REM run's log is the whole point of writing one.
del "%GATELOG%" >nul 2>&1
exit /b 0
