@echo off
REM ============================================================
REM  Window Assistance Controller - build script
REM  Output: build\wac.exe
REM  Requires: x64 MSVC toolchain (see INSTRUCTION_MANUAL.md)
REM ============================================================
setlocal EnableExtensions EnableDelayedExpansion

set "WAC_ROOT=%~dp0.."
pushd "%WAC_ROOT%" || (echo [build] ERROR: cannot enter %WAC_ROOT% & exit /b 1)

call "%~dp0env.cmd"
if errorlevel 1 (popd & exit /b 1)

set "OUTDIR=%WAC_ROOT%\build\obj"
if not exist "%OUTDIR%" mkdir "%OUTDIR%" >nul 2>nul

REM ---- clear stale object files ---------------------------------
REM  Every source is compiled on every run, so an .obj with no source
REM  behind it can only be left over from an earlier build - and the link
REM  step takes "%OUTDIR%\*.obj", so it would be linked in.
REM
REM  MEASURED: after src\ui\settings_dlg.c was deleted (stage 4.5 turned
REM  settings into a page) its object file was still there and still defined
REM  settings_to_store(), so the link failed with LNK2005. That was the lucky
REM  outcome. With symbol names that did not clash it would have linked
REM  silently and put deleted code back into the binary, which no compiler
REM  and no test would report.
if exist "%OUTDIR%\*.obj" del /q "%OUTDIR%\*.obj" >nul 2>nul
if exist "%OUTDIR%\*.res" del /q "%OUTDIR%\*.res" >nul 2>nul

REM ---- compiler flags -----------------------------------------
REM  /utf-8            source and string literals are UTF-8. Without it the
REM                    Chinese comments in src/ trigger C4819, which /WX
REM                    turns into a hard error.
REM  /Zc:preprocessor  conforming preprocessor; required for the variadic
REM                    logging macros to behave (see docs/架构与数据结构.md
REM                    pitfall B).
set "CFLAGS=/nologo /c /TC /O1 /GL /MT /GS /Gy /Gw /GR- /EHs-c- /W4 /WX"
set "CFLAGS=%CFLAGS% /utf-8 /Zc:preprocessor"
set "CFLAGS=%CFLAGS% /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE"

REM  Include roots, as RELATIVE paths with no quotes and no drive letter.
REM
REM  Headers are included by bare name from sibling directories
REM  (ui/mainwindow.c does #include "workspace.h", which lives in model/),
REM  so the compiler needs those directories on its search path.
REM
REM  Writing them as "%WAC_ROOT%\src\model" does not survive batch quoting:
REM  the variable already contains a space ("Window Assistance Controller"),
REM  and set "CFLAGS=... /I"%X%\a" ..." re-parses the embedded quotes, so the
REM  value gets chopped at the space and cl.exe is invoked with fragments
REM  like "sistance" as separate arguments.
REM
REM  Since the whole build runs with the repository root as the current
REM  directory (see the pushd below and `cd /d` before compiling), plain
REM  relative paths are unambiguous and need no quoting at all.
set "CFLAGS=%CFLAGS% /Isrc /Isrc\core /Isrc\model /Isrc\ui"

set "CFLAGS=%CFLAGS% /Fd"%OUTDIR%\wac.pdb" /Fo"%OUTDIR%\\""

REM ---- sources ------------------------------------------------
set "SOURCES="
for %%d in (core model ui util cli) do (
    if exist "%WAC_ROOT%\src\%%d" (
        for %%f in ("%WAC_ROOT%\src\%%d\*.c") do set "SOURCES=!SOURCES! "%%~ff""
    )
)
REM  Top-level sources: main.c, plus application-level modules that sit above
REM  ui/ and model/ because they coordinate both (appconfig.c). Listed by
REM  wildcard so adding one does not require editing this script - and so it
REM  cannot be silently left out of the build.
if exist "%WAC_ROOT%\src\*.c" (
    for %%f in ("%WAC_ROOT%\src\*.c") do set "SOURCES=!SOURCES! "%%~ff""
)

if "!SOURCES!"=="" (
    echo [build] No sources found under src\ - nothing to build yet.
    echo [build] This is expected until stage 2 of the plan.
    popd & exit /b 0
)

echo [build] Compiling...
REM  Compile from the repository root so the relative /I paths above resolve.
cd /d "%WAC_ROOT%" || (echo [build] FAILED: cannot enter %WAC_ROOT% & popd & exit /b 1)
cl %CFLAGS% !SOURCES!
if errorlevel 1 (echo [build] FAILED: compile & popd & exit /b 1)

REM ---- resources (optional) -----------------------------------
set "RES="
if exist "%WAC_ROOT%\res\wac.rc" (
    rc /nologo /fo "%OUTDIR%\wac.res" "%WAC_ROOT%\res\wac.rc"
    if errorlevel 1 (echo [build] FAILED: rc.exe & popd & exit /b 1)
    set "RES="%OUTDIR%\wac.res""
)

REM ---- system libraries ---------------------------------------
REM  All of these ship with Windows; nothing extra is redistributed.
REM  They MUST be listed explicitly: unlike the CMake path, a raw
REM  link.exe invocation resolves no libraries automatically, and the
REM  link fails with LNK2001 on the first user32/gdi32 symbol.
REM  uuid.lib carries the IID_/CLSID_ symbols the app index needs for
REM  IShellLink (reading a shortcut's target path); ole32 alone is not enough.
set "LIBS=kernel32.lib user32.lib gdi32.lib shell32.lib shlwapi.lib advapi32.lib ole32.lib oleaut32.lib uuid.lib dwmapi.lib comctl32.lib"

REM ---- link ---------------------------------------------------
echo [build] Linking...
link /NOLOGO /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /SUBSYSTEM:WINDOWS ^
     /MANIFEST:EMBED /MANIFESTINPUT:"%WAC_ROOT%\res\wac.manifest" ^
     "%OUTDIR%\*.obj" !RES! !LIBS! /OUT:"%WAC_ROOT%\build\wac.exe"
if errorlevel 1 (echo [build] FAILED: link & popd & exit /b 1)

echo [build] OK -^> build\wac.exe
for %%f in ("%WAC_ROOT%\build\wac.exe") do echo [build] Size: %%~zf bytes
popd
endlocal & exit /b 0
