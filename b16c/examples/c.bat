@echo off
rem c.bat file.b16 - build one example next to its source:
rem   b16c.exe     -> file.s   (with -s: source lines as comments)
rem   cpu16asm.exe -> image by the group the example belongs to (its directory):
rem     bare\       file.bin          run on the bare machine: cpu16 file.bin
rem     monitor\    file.cx           copy to a volume as /bin/file.cx, run from the shell
rem     universal\  file.bin, file.cx one image for both ways of running
rem   plus the listing file.lst and the memory map file.map.
rem b16c.exe and cpu16asm.exe are expected next to this file. Examples for the monitor also get
rem the monitor interface (..\..\monitor\user\lib, ..\..\monitor\kernel) as include paths.
rem Messages are ASCII. Exit code: 0 - built, 1 - compiler or assembler error, 2 - bad usage.
setlocal

if "%~1"=="" (
    echo usage: %~nx0 file.b16
    exit /b 2
)
if not exist "%~1" (
    echo %~nx0: %~1 not found
    exit /b 2
)
if /i not "%~x1"==".b16" (
    echo %~nx0: %~1 is not a .b16 file
    exit /b 2
)

set "HERE=%~dp0"
set "B16C=%HERE%..\b16c.exe"
set "ASM=%HERE%..\..\cpu16\tools\cpu16asm.exe"
if not exist "%B16C%" (
    echo %~nx0: compiler not found: %B16C%
    exit /b 1
)
if not exist "%ASM%" (
    echo %~nx0: assembler not found: %ASM%
    exit /b 1
)

rem --- paths: b16c gets forward slashes, as tools\b16build.bat of the monitor does
for %%r in ("%HERE%..") do set "R=%%~fr"
for %%m in ("%HERE%..\..\monitor") do set "MON=%%~fm"
set "SRC=%~f1"
set "OUT=%~dpn1"
set "SRCF=%SRC:\=/%"
set "OUTF=%OUT:\=/%"
set "RF=%R:\=/%"
set "MONF=%MON:\=/%"

rem --- group: name of the directory of the source; anything else (asmlink\) is universal
set "D=%~dp1"
for %%d in ("%D:~0,-1%") do set "G=%%~nxd"
if /i "%G%"=="bare" (set "G=bare") else if /i "%G%"=="monitor" (set "G=monitor") else set "G=universal"

set "INC=-I "%RF%/lib""
if "%G%"=="monitor" set "INC=%INC% -I "%MONF%/user/lib" -I "%MONF%/kernel""

rem --- compile
"%B16C%" -s %INC% %B16FLAGS% -o "%OUTF%.s" "%SRCF%"
if errorlevel 1 (
    echo FAIL %G%\%~n1: compiler
    exit /b 1
)

rem --- assemble: an executable CX16 image (cpu16asm --exec)
set "EXT=bin"
if "%G%"=="monitor" set "EXT=cx"
"%ASM%" --exec -l "%OUT%.lst" -m "%OUT%.map" -o "%OUT%.%EXT%" "%OUT%.s"
if errorlevel 1 (
    echo FAIL %G%\%~n1: assembler
    exit /b 1
)
if "%G%"=="universal" (
    copy /y "%OUT%.bin" "%OUT%.cx" > nul
    if errorlevel 1 (
        echo FAIL %G%\%~n1: cannot write %~n1.cx
        exit /b 1
    )
    echo ok   %G%\%~n1: %~n1.bin, %~n1.cx
) else (
    echo ok   %G%\%~n1: %~n1.%EXT%
)

endlocal
exit /b 0
