/*
 * t00_build.c - stage 1, experiment 1
 *
 * PURPOSE
 *   Prove that the /MT build produces a genuinely self-contained exe:
 *   no CRT redistributable, correct architecture, manifest embedded.
 *   This program does nothing at runtime - its value is in the build
 *   artifacts, which you inspect with dumpbin / mt.
 *
 * BUILD
 *   Compiled as a WINDOWS subsystem app (see below), because the real
 *   product is a GUI app and this must mirror its subsystem and manifest
 *   settings exactly. That means it has no console output by design.
 *
 *   cl /nologo /TC /O1 /GL /MT /GS /Gy /Gw /GR- /EHs-c- /W4 /WX ^
 *      /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX t00_build.c ^
 *      /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS ^
 *      /MANIFEST:EMBED /MANIFESTINPUT:..\res\wac.manifest ^
 *      kernel32.lib user32.lib /OUT:t00_build.exe
 *
 *   Then run the checks in TRAPS.md (dumpbin /dependents,
 *   /headers, mt -inputresource). This program is intentionally NOT part
 *   of build_tests.cmd's default output filter - build it with the
 *   command above, or use build\build.cmd once src/ exists.
 */

#include <windows.h>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, PWSTR cmdline, int show)
{
    (void)hInstance; (void)hPrev; (void)cmdline; (void)show;

    /* Keep this tiny: the measurement is the build output, not this code.
       Every extra API call would pull in another import and muddy the
       dependency report we are trying to read. */
    MessageBoxW(NULL,
                L"t00_build - build artifact smoke test.\n\n"
                L"This program exists only to produce an exe whose\n"
                L"dependencies and manifest can be inspected.\n\n"
                L"See tests\\README.md for the dumpbin / mt commands.",
                L"WAC stage 1 - t00_build",
                MB_OK | MB_ICONINFORMATION);
    return 0;
}
