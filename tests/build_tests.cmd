@echo off
REM ============================================================
REM  Window Assistance Controller - build all stage-1 test programs
REM
REM  Usage:  tests\build_tests.cmd            (build all)
REM          tests\build_tests.cmd t02_move   (build one)
REM
REM  Output: tests\out\<name>.exe   (console subsystem)
REM
REM  These are throwaway measurement tools for stage 1. They are built
REM  WITHOUT the product manifest and as CONSOLE apps, because they must
REM  print measurements; see TRAPS.md.
REM
REM  NOTE: no setlocal here - build\env.cmd must be able to modify this
REM  shell's environment.
REM ============================================================
setlocal EnableExtensions EnableDelayedExpansion

set "WAC_ROOT=%~dp0.."
pushd "%WAC_ROOT%" || (echo [tests] ERROR: cannot enter %WAC_ROOT% & exit /b 1)

call "%~dp0..\build\env.cmd"
if errorlevel 1 (echo [tests] ERROR: no MSVC environment & popd & exit /b 1)

set "OUTDIR=%WAC_ROOT%\tests\out"
if not exist "%OUTDIR%" mkdir "%OUTDIR%" >nul 2>nul

REM ---- flags -------------------------------------------------
REM  Console subsystem, no manifest. /utf-8 keeps the Chinese comments
REM  and any Chinese string literals from tripping warning C4819.
set "CFLAGS=/nologo /TC /O1 /MT /GS /W4 /WX /utf-8 /Zc:preprocessor /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS"
set "CFLAGS=%CFLAGS% /I"%WAC_ROOT%\tests""
set "LDFLAGS=/NOLOGO /SUBSYSTEM:CONSOLE /OPT:REF /OPT:ICF"
REM  System libraries every test program may need. Listed explicitly:
REM  raw link.exe resolves nothing automatically.
REM  shcore.lib is included for GetDpiForMonitor, used by t01/t06.
set "LIBS=kernel32.lib user32.lib gdi32.lib shell32.lib shlwapi.lib advapi32.lib ole32.lib oleaut32.lib uuid.lib dwmapi.lib comctl32.lib shcore.lib"

REM ---- pick targets ------------------------------------------
set "TARGETS="
if not "%~1"=="" (
    set "TARGETS=%~1"
) else (
    for %%f in ("%WAC_ROOT%\tests\t*.c") do set "TARGETS=!TARGETS! %%~nf"
)

set "OK=0"
set "FAILED="

for %%t in (!TARGETS!) do (
    if exist "%WAC_ROOT%\tests\%%t.c" (
        echo [tests] building %%t ...

        REM  t00_build uses wWinMain and exists to mirror the product's
        REM  GUI subsystem + embedded manifest, so it must be built as a
        REM  WINDOWS app. Every other test prints measurements and needs
        REM  the CONSOLE subsystem.
        set "SUB=/SUBSYSTEM:CONSOLE"
        set "MANIFEST="
        if /i "%%t"=="t00_build" (
            set "SUB=/SUBSYSTEM:WINDOWS"
            set "MANIFEST=/MANIFEST:EMBED /MANIFESTINPUT:"%WAC_ROOT%\res\wac.manifest""
        )

        REM  Unit tests link against the product source they exercise, so
        REM  they are compiled together with it. A test that only includes
        REM  a header would not actually test the implementation.
        set "EXTRASRC="
        if /i "%%t"=="t10_relcoord"      set "EXTRASRC="%WAC_ROOT%\src\core\relcoord.c""
        if /i "%%t"=="t11_store"         set "EXTRASRC="%WAC_ROOT%\src\model\store.c""
        if /i "%%t"=="t14_i18nfmt"       set "EXTRASRC="%WAC_ROOT%\src\ui\i18n.c""
        if /i "%%t"=="t12_uiformat"      set "EXTRASRC="%WAC_ROOT%\src\ui\uiformat.c" "%WAC_ROOT%\src\ui\preview.c" "%WAC_ROOT%\src\ui\pagelist.c""
        if /i "%%t"=="t13_navstack"      set "EXTRASRC="%WAC_ROOT%\src\ui\nav.c""
        if /i "%%t"=="t15_framemath"     set "EXTRASRC="%WAC_ROOT%\src\ui\frame.c""
        if /i "%%t"=="t05_topmost"       set "EXTRASRC="%WAC_ROOT%\src\core\proc_launch.c""
        if /i "%%t"=="t16_maxprobe"      set "EXTRASRC="%WAC_ROOT%\src\ui\frame.c""
        if /i "%%t"=="t17_sysinfo"       set "EXTRASRC="%WAC_ROOT%\src\util\sysinfo.c""
        if /i "%%t"=="t18_workspace"     set "EXTRASRC="%WAC_ROOT%\src\model\workspace.c" "%WAC_ROOT%\src\model\store.c" "%WAC_ROOT%\src\core\monitor.c" "%WAC_ROOT%\src\util\strutil.c""
        if /i "%%t"=="t19_proclaunch"    set "EXTRASRC="%WAC_ROOT%\src\core\proc_launch.c""
        if /i "%%t"=="t23_runstate"      set "EXTRASRC="%WAC_ROOT%\src\model\runstate.c" "%WAC_ROOT%\src\model\config.c" "%WAC_ROOT%\src\util\strutil.c""
        if /i "%%t"=="t24_appconfig"   set "EXTRASRC="%WAC_ROOT%\src\appconfig.c" "%WAC_ROOT%\src\model\store.c" "%WAC_ROOT%\src\model\workspace.c" "%WAC_ROOT%\src\core\monitor.c" "%WAC_ROOT%\src\ui\page_settings.c" "%WAC_ROOT%\src\ui\i18n.c" "%WAC_ROOT%\src\util\strutil.c""
        if /i "%%t"=="t22_workspacerun"  set "EXTRASRC="%WAC_ROOT%\src\model\workspace_run.c" "%WAC_ROOT%\src\model\workspace.c" "%WAC_ROOT%\src\model\rule.c" "%WAC_ROOT%\src\model\store.c" "%WAC_ROOT%\src\model\config.c" "%WAC_ROOT%\src\core\proc_launch.c" "%WAC_ROOT%\src\core\launch.c" "%WAC_ROOT%\src\core\win_place.c" "%WAC_ROOT%\src\core\win_enum.c" "%WAC_ROOT%\src\core\app_id.c" "%WAC_ROOT%\src\core\monitor.c" "%WAC_ROOT%\src\core\relcoord.c" "%WAC_ROOT%\src\util\strutil.c""
        if /i "%%t"=="t30_place" set "EXTRASRC="%WAC_ROOT%\src\core\win_enum.c" "%WAC_ROOT%\src\core\win_place.c" "%WAC_ROOT%\src\core\app_id.c" "%WAC_ROOT%\src\core\monitor.c" "%WAC_ROOT%\src\core\relcoord.c" "%WAC_ROOT%\src\model\store.c" "%WAC_ROOT%\src\model\rule.c" "%WAC_ROOT%\src\model\config.c" "%WAC_ROOT%\src\util\strutil.c""
        if /i "%%t"=="t20_monitor_enum"  set "EXTRASRC="%WAC_ROOT%\src\core\monitor.c""
        if /i "%%t"=="t50_overlay"       set "EXTRASRC="%WAC_ROOT%\src\ui\overlay.c" "%WAC_ROOT%\src\ui\popup.c" "%WAC_ROOT%\src\ui\sidebar.c" "%WAC_ROOT%\src\ui\iconcache.c" "%WAC_ROOT%\src\ui\placeholder.c" "%WAC_ROOT%\src\ui\snapgrid.c" "%WAC_ROOT%\src\ui\toolbar.c" "%WAC_ROOT%\src\ui\i18n.c" "%WAC_ROOT%\src\core\monitor.c" "%WAC_ROOT%\src\util\log.c" "%WAC_ROOT%\src\model\config.c" "%WAC_ROOT%\src\util\strutil.c""
        if /i "%%t"=="t21_monitor_match" set "EXTRASRC="%WAC_ROOT%\src\core\monitor.c""
        if /i "%%t"=="t51_release"       set "EXTRASRC="%WAC_ROOT%\src\ui\editstate.c" "%WAC_ROOT%\src\ui\overlay.c" "%WAC_ROOT%\src\ui\popup.c" "%WAC_ROOT%\src\ui\sidebar.c" "%WAC_ROOT%\src\ui\iconcache.c" "%WAC_ROOT%\src\ui\placeholder.c" "%WAC_ROOT%\src\ui\snapgrid.c" "%WAC_ROOT%\src\ui\toolbar.c" "%WAC_ROOT%\src\ui\i18n.c" "%WAC_ROOT%\src\core\monitor.c" "%WAC_ROOT%\src\util\log.c" "%WAC_ROOT%\src\model\config.c" "%WAC_ROOT%\src\util\strutil.c""
        if /i "%%t"=="t52_popup"         set "EXTRASRC="%WAC_ROOT%\src\ui\popup.c""
        if /i "%%t"=="t25_appindex"      set "EXTRASRC="%WAC_ROOT%\src\core\appindex.c" "%WAC_ROOT%\src\model\config.c" "%WAC_ROOT%\src\util\log.c" "%WAC_ROOT%\src\util\strutil.c""
        if /i "%%t"=="t70_sidebar"       set "EXTRASRC="%WAC_ROOT%\src\ui\sidebar.c""
        if /i "%%t"=="t71_iconcache"     set "EXTRASRC="%WAC_ROOT%\src\ui\iconcache.c""
        if /i "%%t"=="t72_placeholder"   set "EXTRASRC="%WAC_ROOT%\src\ui\placeholder.c""
        if /i "%%t"=="t73_pickup" set "EXTRASRC="%WAC_ROOT%\src\model\pickup.c" "%WAC_ROOT%\src\core\win_enum.c" "%WAC_ROOT%\src\core\app_id.c" "%WAC_ROOT%\src\core\monitor.c" "%WAC_ROOT%\src\core\relcoord.c" "%WAC_ROOT%\src\model\rule.c" "%WAC_ROOT%\src\model\store.c" "%WAC_ROOT%\src\model\config.c" "%WAC_ROOT%\src\util\log.c" "%WAC_ROOT%\src\util\strutil.c""
        if /i "%%t"=="t60_displaychange" set "EXTRASRC="%WAC_ROOT%\src\core\displaywatch.c" "%WAC_ROOT%\src\core\monitor.c""

        cl %CFLAGS% /I"%WAC_ROOT%\src" /I"%WAC_ROOT%\src\core" /I"%WAC_ROOT%\src\model" "%WAC_ROOT%\tests\%%t.c" !EXTRASRC! /Fo"%OUTDIR%\\" /Fe"%OUTDIR%\%%t.exe" /link %LDFLAGS% !SUB! !MANIFEST! %LIBS% >nul 2>nul
        if errorlevel 1 (
            echo [tests]   FAILED: %%t
            set "FAILED=!FAILED! %%t"
        ) else (
            for %%e in ("%OUTDIR%\%%t.exe") do echo [tests]   ok  %%t.exe  %%~ze bytes
            set /a OK+=1
        )
    ) else (
        echo [tests]   SKIP: %%t.c not found
    )
)

echo.
echo [tests] built !OK! program^(s^)
if not "!FAILED!"=="" (
    echo [tests] FAILED:!FAILED!
    echo.
    echo [tests] Re-run a single target WITHOUT output suppression to see errors:
    echo [tests]   tests\build_tests.cmd t02_move
    popd
    exit /b 1
)

echo [tests] output: %OUTDIR%

REM  .obj files are intermediate only; keep the output directory to exes
REM  and the per-run .log / .crash.txt diagnostic files.
del "%OUTDIR%\*.obj" >nul 2>nul

popd
exit /b 0
