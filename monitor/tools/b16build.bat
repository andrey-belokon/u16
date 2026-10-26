@echo off
rem b16build.bat - Windows version of tools/b16build.sh: build a B16 program for the CPU16 monitor.
rem
rem   b16build.bat path\prog.b16 [b16c options]      -> <prog>.bin, <prog>.map, <prog>.lst, <prog>.s
rem                                                       in the same directory as prog.b16
rem
rem Put this file into monitor\tools\ (next to b16build.sh). Image parameters (option max_top,
rem option min_free) and assembly modules (include asm; sys.b16 adds sys.asm) are set in the
rem source itself; include paths: source directory, user\lib, kernel (abi.b16). Environment variables:
rem   B16C      path to b16c.exe        (default: monitor\..\b16c\b16c.exe)
rem   CPU16ASM  path to cpu16asm.exe    (default: monitor\..\cpu16\build\cpu16asm.exe)
rem   B16FLAGS  extra b16c options
rem Example:  tools\b16build.bat ..\b16c\examples\tetris.b16
rem              (creates ..\b16c\examples\tetris.bin and the other files next to tetris.b16)
rem Messages are ASCII on purpose: cmd.exe and UTF-8 do not mix well.

setlocal EnableExtensions

if "%~1"=="" (
    echo usage: %~nx0 path\prog.b16 [b16c options]
    exit /b 2
)
if not exist "%~f1" (
    echo %~nx0: source file not found: %~f1
    exit /b 2
)

rem --- paths (no trailing backslash: a backslash before a closing quote would escape it)
for %%I in ("%~dp0..") do set "D=%%~fI"
for %%I in ("%~dp1.") do set "S=%%~fI"
set "SRC=%~f1"
set "N=%~n1"
set "O=%S%"

rem --- the same paths with forward slashes: b16c writes them into the .s file (.include, .line),
rem     and the assembler would read "\R" or "\t" in a backslash path as an escape sequence
set "DF=%D:\=/%"
set "SF=%S:\=/%"
set "SRCF=%SRC:\=/%"
set "OF=%O:\=/%"

if not defined B16C set "B16C=%D%\..\b16c\b16c.exe"
if not defined CPU16ASM set "CPU16ASM=%D%\..\cpu16\tools\cpu16asm.exe"

if not exist "%B16C%" (
    echo %~nx0: b16c not found: %B16C%
    echo Build it:  cd b16c ^&^& g++ -x c++ -O2 -o b16c.exe b16c.c   or set B16C
    exit /b 1
)
if not exist "%CPU16ASM%" (
    echo %~nx0: cpu16asm not found: %CPU16ASM%
    echo Build it:  cd cpu16 ^&^& mkdir build ^&^& g++ -std=c++17 -O2 -o build\cpu16asm.exe tools\cpu16asm.cpp   or set CPU16ASM
    exit /b 1
)

rem --- the remaining arguments go to b16c as they are
set "REST="
shift
:collect
if "%~1"=="" goto collected
set "REST=%REST% %1"
shift
goto collect
:collected

"%B16C%" -Werror -I "%SF%" -I "%DF%/user/lib" -I "%DF%/kernel" %B16FLAGS% %REST% "%SRCF%" -o "%OF%/%N%.s"
if errorlevel 1 exit /b 1

"%CPU16ASM%" --exec -m "%O%\%N%.map" -l "%O%\%N%.lst" -o "%O%\%N%.cx" "%O%\%N%.s"
if errorlevel 1 exit /b 1

endlocal
exit /b 0
