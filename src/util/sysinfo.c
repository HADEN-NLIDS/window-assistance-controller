/*
 * sysinfo.c - see sysinfo.h.
 */

#include "sysinfo.h"

/*
 * WIN32_LEAN_AND_MEAN (set for the whole build) excludes shellapi.h, so
 * ShellExecuteW has to be asked for explicitly. shlobj.h is not excluded and
 * brings in the CSIDL enumeration used below - the same one config.c uses for
 * %APPDATA%, so both paths resolve through one API family.
 */
#include <shlobj.h>
#include <shellapi.h>

BOOL sysinfo_is_elevated(void)
{
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return FALSE;
    }

    TOKEN_ELEVATION elevation;
    ZeroMemory(&elevation, sizeof(elevation));

    DWORD size = (DWORD)sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation,
                                        &elevation, (DWORD)sizeof(elevation),
                                        &size);
    CloseHandle(token);

    return (ok && elevation.TokenIsElevated != 0) ? TRUE : FALSE;
}

BOOL sysinfo_startup_folder(wchar_t *out, size_t cch)
{
    if (out == NULL || cch == 0) return FALSE;

    out[0] = L'\0';

    /*
     * SHGetFolderPathW takes NO size argument: it documents that the buffer
     * must hold MAX_PATH characters and writes accordingly. (Measured by
     * tests/t17_sysinfo.c, which passed a 4-character buffer and watched it
     * get written past the end.) So the call always fills a local buffer of
     * that size, and the result is copied out afterwards where a short
     * destination can be reported instead of overrun.
     */
    wchar_t path[MAX_PATH];
    path[0] = L'\0';

    /*
     * CSIDL_STARTUP rather than the literal %APPDATA%\Microsoft\Windows\...
     * path: a profile can be redirected, and the shell is the only thing that
     * knows where the folder really is. This is the same call config.c makes
     * for the config directory.
     */
    if (SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, 0, path) != S_OK) return FALSE;
    if (path[0] == L'\0') return FALSE;

    /*
     * A destination that cannot hold the path is a failure, not a truncated
     * success: half a folder path would be handed to the shell, which would
     * then report its own error somewhere the user cannot connect to this
     * call. Nothing usable is left behind either.
     */
    if (wcsncpy_s(out, cch, path, _TRUNCATE) != 0) {
        out[0] = L'\0';
        return FALSE;
    }

    return TRUE;
}

BOOL sysinfo_open_folder(HWND owner, const wchar_t *path)
{
    if (path == NULL || path[0] == L'\0') return FALSE;

    const HINSTANCE r = ShellExecuteW(owner, L"open", path, NULL, NULL,
                                      SW_SHOWNORMAL);

    /* ShellExecuteW returns a value that only means anything above 32; the
     * comparison is the documented contract, not a threshold picked here. */
    return ((INT_PTR)r > 32) ? TRUE : FALSE;
}
