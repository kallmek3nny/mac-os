@echo off
setlocal EnableExtensions
set "KID=%~dp0_nocall-child.cmd"
echo == case 1: invoke child WITHOUT call ==
"%KID%"
echo   PARENT resumed after no-call invocation
echo == case 2: invoke child WITH call ==
call "%KID%"
echo   PARENT resumed after call invocation
echo == done ==
endlocal
exit /b 0
