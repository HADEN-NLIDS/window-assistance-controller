/*
 * config.c - see config.h.
 *
 * Moved here verbatim from main.c so that the command-line tool and the main
 * window agree on one directory. The behaviour is unchanged; only the static
 * linkage went away.
 */

#include "config.h"

#include <shlobj.h>

#include "../util/strutil.h"

BOOL config_dir(wchar_t *out, size_t cch)
{
    wchar_t appdata[MAX_PATH];

    if (out == NULL || cch == 0) return FALSE;

    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata) != S_OK) {
        return FALSE;
    }

    if (!path_join(out, cch, appdata, L"WindowAssistanceController")) {
        return FALSE;
    }

    /*
     * CreateDirectoryW fails with ERROR_ALREADY_EXISTS on every run after the
     * first, which is success for our purposes. Any other failure means the
     * path exists as a FILE, or is not writable - both of which the caller
     * must hear about, because every later save would fail too.
     */
    if (!CreateDirectoryW(out, NULL)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
    }

    return TRUE;
}

BOOL config_file(wchar_t *out, size_t cch)
{
    wchar_t dir[MAX_PATH];

    if (out == NULL || cch == 0) return FALSE;
    if (!config_dir(dir, MAX_PATH)) return FALSE;

    return path_join(out, cch, dir, L"workspaces.ini");
}
