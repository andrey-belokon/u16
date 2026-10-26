@echo off
rem call.bat - build all examples in bare\, universal\ and monitor\ with c.bat: .s, .lst, .map
rem and the image (.bin for the bare machine, .cx for the monitor, both for universal examples).
rem monitor\mon.b16 is a library of the monitor examples, not a program. A summary at the end.
setlocal
chcp 65001 > nul
set "HERE=%~dp0"
set /a OK=0, BAD=0

for /r "%HERE%." %%i in (*.b16) do (
    if /i not "%%~nxi"=="mon.b16" call :one "%%i"
)

echo.
echo built: %OK%, failed: %BAD%
pause
if %BAD% gtr 0 exit /b 1
exit /b 0

:one
call "%HERE%c.bat" "%~1"
if errorlevel 1 (set /a BAD+=1) else (set /a OK+=1)
exit /b 0
