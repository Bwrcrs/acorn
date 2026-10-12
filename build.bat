@echo off
REM ============================================================
REM  Acorn build script
REM
REM  NOTE: keep this file pure ASCII. Do NOT put Chinese (or any
REM  non-ASCII) characters in a .bat file. cmd.exe reads batch
REM  files using the system OEM codepage (GBK on this machine),
REM  but this file is saved as UTF-8. The 3 bytes of a Chinese
REM  character get mis-decoded and can swallow the following
REM  newline or caret, which corrupts line boundaries and makes
REM  cmd try to execute fragments of comments. Chinese docs live
REM  in README.md instead, where UTF-8 is handled correctly.
REM  (Learned the hard way in the Nova project.)
REM
REM  No cmake / make on this machine, so we call g++ directly.
REM
REM  NOTE: `g++ -o acorn.exe src\*.cpp` does NOT work -- cmd does
REM  not expand wildcards (that is a Unix shell habit) and MinGW
REM  g++ does not either, so it receives the literal string
REM  "src\*.cpp" and fails with "Invalid argument". Hence the
REM  for-loop below that builds the file list explicitly.
REM
REM  Notably NOT passed: -static-libstdc++ / -static-libgcc.
REM  TDM-GCC already links libstdc++ statically by default (a
REM  TDM-specific deviation from stock MinGW-w64), so those flags
REM  are redundant here.
REM
REM  Flags:
REM    -std=c++17      C++17
REM    -Wall -Wextra   all warnings; zero warnings is a good habit
REM    -g              debug info for stepping in a debugger
REM    -finput-charset / -fexec-charset   read and emit UTF-8, so
REM                    Chinese string literals survive (pairs with
REM                    SetConsoleOutputCP in main.cpp)
REM ============================================================

REM ---- always run from the directory this script lives in ----
cd /d "%~dp0"

setlocal enabledelayedexpansion

set OUT=acorn.exe

REM ---- pick optimization level ----
REM   build.bat release
set OPT=-g
set NOPAUSE=0
for %%a in (%*) do (
    if /i "%%a"=="release" set OPT=-O2 -DNDEBUG
    if /i "%%a"=="nopause" set NOPAUSE=1
)

REM ---- collect sources ----
REM enabledelayedexpansion lets us use !SRCS! (evaluated on the
REM spot). With %SRCS% the whole for-loop body would be expanded
REM once up front, when SRCS is still empty -- classic cmd trap.
set SRCS=
for %%f in (src\*.cpp) do set SRCS=!SRCS! %%f

if "!SRCS!"=="" (
    echo [FAIL] no .cpp files found under src\
    exit /b 1
)

echo [BUILD] g++ -std=c++17 -Wall -Wextra !OPT! -o %OUT%
echo.

g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter !OPT! ^
    -finput-charset=UTF-8 -fexec-charset=UTF-8 ^
    -o %OUT% !SRCS!

if errorlevel 1 (
    echo.
    echo [FAIL] compile errors, see above
    exit /b 1
)

echo.
echo [OK] built %OUT%

REM ---- keep the window open when double-clicked ----
if "!NOPAUSE!"=="0" pause

endlocal
