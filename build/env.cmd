@echo off
REM ============================================================
REM  Window Assistance Controller - MSVC x64 environment setup
REM
REM  Usage:  call build\env.cmd
REM  Effect: initializes the x64 MSVC environment IN THE CALLER's
REM          cmd.exe, so cl.exe / link.exe work afterwards.
REM
REM  Design rule: KEEP THIS SCRIPT SIMPLE. Three things matter:
REM
REM   1. NO setlocal / endlocal PAIR, and no `setlocal` at all
REM      (except the one-line toggle form, which is not used here).
REM      Measured on Windows 11 22631:
REM        setlocal & set VAR=x & endlocal   ->  VAR is empty after.
REM      So bracketing VsDevCmd.bat in a setlocal would silently roll
REM      back everything it sets, and `call build\env.cmd` would
REM      appear to succeed while changing nothing.
REM
REM   2. AVOID the variable %ProgramFiles(x86)% - its NAME contains
REM      parentheses, and expanding it inside a for/if block breaks
REM      cmd's parser with "\Microsoft was unexpected at this time."
REM      This is why the script uses %ProgramW6432% and
REM      %ProgramFiles% and simply tries both locations via an
REM      if-defined check instead of building the path inside a block.
REM
REM   3. VsDevCmd.bat is called through pushd/popd, because the
REM      install path may itself contain parentheses.
REM
REM  If this script ever misbehaves, use the CMake build path instead
REM  (see INSTRUCTION_MANUAL.md) - it has none of these constraints.
REM ============================================================

REM ---- already initialized? -----------------------------------
if defined VCINSTALLDIR (
    where cl.exe >nul 2>nul
    if not errorlevel 1 (
        echo [env] MSVC environment already active: %VCINSTALLDIR%
        exit /b 0
    )
)

REM ---- collect candidate Visual Studio roots ------------------
REM  vswhere gives the authoritative answer when present.
REM  NOTE: `if exist ... for /f ... 2>nul` written on ONE line is wrong -
REM  cmd parses the trailing 2>nul as literal arguments to the for command
REM  and the redirection silently breaks the whole statement. The
REM  `if exist` must stay on its own line, with usebackq + inner quotes.
set "WAC_VSDIR="
set "WAC_VSWHERE=%ProgramW6432%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%WAC_VSWHERE%" set "WAC_VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%WAC_VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%WAC_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "WAC_VSDIR=%%i"
)

REM  Fallback: probe the usual roots one by one. The year list is NOT
REM  hard-coded to a single version on purpose - the installed VS changes
REM  when the machine is updated (this dev machine moved from VS 18
REM  BuildTools to VS 2022 Community, which broke the first version of
REM  this script). Newer years are probed first.
if not defined WAC_VSDIR call :try "%ProgramW6432%\Microsoft Visual Studio\2026\Community"
if not defined WAC_VSDIR call :try "%ProgramW6432%\Microsoft Visual Studio\2026\BuildTools"
if not defined WAC_VSDIR call :try "%ProgramW6432%\Microsoft Visual Studio\2022\Community"
if not defined WAC_VSDIR call :try "%ProgramW6432%\Microsoft Visual Studio\2022\Professional"
if not defined WAC_VSDIR call :try "%ProgramW6432%\Microsoft Visual Studio\2022\Enterprise"
if not defined WAC_VSDIR call :try "%ProgramW6432%\Microsoft Visual Studio\2022\BuildTools"
if not defined WAC_VSDIR call :try "%ProgramW6432%\Microsoft Visual Studio\2022\Preview"
if not defined WAC_VSDIR call :try "%ProgramFiles%\Microsoft Visual Studio\2022\Community"
if not defined WAC_VSDIR call :try "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"

REM  Last resort: the x86 root, via a helper subroutine so the
REM  parenthesised variable name is never expanded inside a block.
if not defined WAC_VSDIR call :try_x86

if not defined WAC_VSDIR (
    echo [env] ERROR: Visual Studio C++ build tools not found.
    echo [env] Install the "Desktop development with C++" workload, or run
    echo [env] this script from a Visual Studio Developer Command Prompt.
    exit /b 1
)

echo [env] Visual Studio: %WAC_VSDIR%

REM ---- initialize x64 environment ----------------------------
pushd "%WAC_VSDIR%\Common7\Tools"
call VsDevCmd.bat -arch=amd64 -host_arch=amd64 -no_logo
set "WAC_VSRC=%ERRORLEVEL%"
popd

if not "%WAC_VSRC%"=="0" (
    echo [env] ERROR: VsDevCmd.bat returned %WAC_VSRC%
    set "WAC_VSDIR="
    set "WAC_VSWHERE="
    set "WAC_VSRC="
    exit /b 1
)

where cl.exe >nul 2>nul
if errorlevel 1 (
    echo [env] ERROR: cl.exe not found on PATH after VsDevCmd.
    echo [env] The C++ build tools may be incompletely installed.
    set "WAC_VSDIR="
    set "WAC_VSWHERE="
    set "WAC_VSRC="
    exit /b 1
)

echo [env] OK - x64 MSVC environment ready
for /f "delims=" %%i in ('where cl.exe 2^>nul') do echo [env] cl.exe: %%i

REM ---- drop our own temporaries, keep VsDevCmd's variables ----
set "WAC_VSDIR="
set "WAC_VSWHERE="
set "WAC_VSRC="
exit /b 0

REM ============================================================
REM  Subroutines
REM ============================================================

REM  :try <candidate-dir>  - accept it if VsDevCmd.bat is there.
:try
if defined WAC_VSDIR exit /b 0
if exist "%~1\Common7\Tools\VsDevCmd.bat" set "WAC_VSDIR=%~1"
exit /b 0

REM  :try_x86 - probe the 32-bit program-files root. The variable is
REM  read on its own line so its parenthesised name is never expanded
REM  inside a parenthesised block. Newer VS releases install to the
REM  64-bit Program Files root, so this path is rarely needed.
:try_x86
if defined WAC_VSDIR exit /b 0
set "WAC_X86ROOT=%ProgramFiles(x86)%"
if not defined WAC_X86ROOT exit /b 0
call :try "%WAC_X86ROOT%\Microsoft Visual Studio\2022\BuildTools"
call :try "%WAC_X86ROOT%\Microsoft Visual Studio\2022\Community"
call :try "%WAC_X86ROOT%\Microsoft Visual Studio\18\BuildTools"
call :try "%WAC_X86ROOT%\Microsoft Visual Studio\18\Community"
call :try "%WAC_X86ROOT%\Microsoft Visual Studio\17\BuildTools"
call :try "%WAC_X86ROOT%\Microsoft Visual Studio\17\Community"
set "WAC_X86ROOT="
exit /b 0
