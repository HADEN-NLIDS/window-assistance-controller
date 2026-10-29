/*
 * appindex.c - see the header for what this is and what it refuses to do.
 *
 * The three readers below are the only OS-touching code in the module. Every
 * one of them opens KEY_READ and closes what it opened; nothing here writes a
 * registry value, and nothing here reads a file other than the cache this
 * program itself wrote.
 */

#include "appindex.h"

#include "config.h"
#include "../util/log.h"
#include "../util/strutil.h"   /* str_to_wide / wide_to_str for the cache */

#include <objbase.h>           /* CoInitializeEx / CoCreateInstance */
#include <shlobj.h>            /* SHGetFolderPathW, IShellLinkW */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ------------------------------------------------------------------ */
/* The array                                                           */
/* ------------------------------------------------------------------ */

struct AppIndex {
    AppIndexItem *items;
    int           count;
    int           cap;
};

AppIndex *appindex_create(void)
{
    AppIndex *idx = (AppIndex *)calloc(1, sizeof(AppIndex));
    return idx;
}

void appindex_destroy(AppIndex *idx)
{
    if (idx == NULL) return;
    free(idx->items);
    free(idx);
}

int appindex_count(const AppIndex *idx)
{
    return (idx != NULL) ? idx->count : 0;
}

const AppIndexItem *appindex_at(const AppIndex *idx, int index)
{
    if (idx == NULL || index < 0 || index >= idx->count) return NULL;
    return &idx->items[index];
}

BOOL appindex_add(AppIndex *idx, const wchar_t *name, const wchar_t *exe_path,
                  AppIndexSource source)
{
    if (idx == NULL || name == NULL || name[0] == L'\0') return FALSE;

    if (idx->count == idx->cap) {
        const int ncap = (idx->cap > 0) ? (idx->cap * 2) : 64;
        AppIndexItem *grown = (AppIndexItem *)realloc(
            idx->items, (size_t)ncap * sizeof(AppIndexItem));
        if (grown == NULL) return FALSE;
        idx->items = grown;
        idx->cap = ncap;
    }

    AppIndexItem *it = &idx->items[idx->count++];
    ZeroMemory(it, sizeof(*it));

    wcsncpy_s(it->name, APPINDEX_NAME_MAX, name, _TRUNCATE);
    if (exe_path != NULL) {
        wcsncpy_s(it->exe_path, APPINDEX_PATH_MAX, exe_path, _TRUNCATE);
    }
    it->source = source;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Dedup and sort - pure                                               */
/* ------------------------------------------------------------------ */

static int cmp_by_name(const void *a, const void *b)
{
    const AppIndexItem *x = (const AppIndexItem *)a;
    const AppIndexItem *y = (const AppIndexItem *)b;
    return _wcsicmp(x->name, y->name);
}

/* Drop entry j, keeping the array dense. */
static void drop_item(AppIndex *idx, int j)
{
    idx->items[j] = idx->items[idx->count - 1];
    idx->count--;
}

int appindex_dedup(AppIndex *idx)
{
    if (idx == NULL || idx->count <= 1) return 0;

    const int before = idx->count;

    /*
     * Same NAME, twice. This is the common case: a program appears in
     * Uninstall (a product name) and in the Start Menu (a shortcut name) with
     * the same text. The entry that can be LAUNCHED wins, because the other
     * one can only be displayed.
     */
    qsort(idx->items, (size_t)idx->count, sizeof(AppIndexItem), cmp_by_name);

    for (int i = 0; i < idx->count; ++i) {
        int j = i + 1;
        while (j < idx->count && _wcsicmp(idx->items[i].name, idx->items[j].name) == 0) {
            const BOOL i_has = (idx->items[i].exe_path[0] != L'\0');
            const BOOL j_has = (idx->items[j].exe_path[0] != L'\0');

            if (!i_has && j_has) {
                /* Keep j's path on i's slot, then drop j. */
                wcsncpy_s(idx->items[i].exe_path, APPINDEX_PATH_MAX,
                          idx->items[j].exe_path, _TRUNCATE);
                idx->items[i].source = idx->items[j].source;
                drop_item(idx, j);
            } else {
                /* Both launchable, or neither: one entry is enough. */
                drop_item(idx, j);
            }
        }
    }

    /*
     * Same EXECUTABLE, different names ("Notepad" vs "notepad.exe"). O(n^2),
     * but n is a few hundred and this runs once per scan, off the UI thread.
     */
    for (int i = 0; i < idx->count; ++i) {
        if (idx->items[i].exe_path[0] == L'\0') continue;

        for (int j = i + 1; j < idx->count; ) {
            if (idx->items[j].exe_path[0] != L'\0' &&
                _wcsicmp(idx->items[i].exe_path, idx->items[j].exe_path) == 0) {
                /* The longer name is usually the friendlier one ("Notepad"
                 * beats "notepad.exe"), so it wins the label. */
                if (wcslen(idx->items[j].name) > wcslen(idx->items[i].name)) {
                    wcsncpy_s(idx->items[i].name, APPINDEX_NAME_MAX,
                              idx->items[j].name, _TRUNCATE);
                }
                drop_item(idx, j);
            } else {
                ++j;
            }
        }
    }

    /* Sorting again: dropping entries above may have moved an unsorted one
     * into the slot we already passed. */
    qsort(idx->items, (size_t)idx->count, sizeof(AppIndexItem), cmp_by_name);

    return before - idx->count;
}

/* ------------------------------------------------------------------ */
/* Source 1: App Paths (an exe path per key name)                      */
/* ------------------------------------------------------------------ */

static void strip_exe_suffix(wchar_t *name)
{
    const size_t n = wcslen(name);
    if (n > 4 && _wcsicmp(name + n - 4, L".exe") == 0) name[n - 4] = L'\0';
}

static int scan_app_paths_view(AppIndex *idx, HKEY hive, REGSAM view)
{
    static const wchar_t *kPath =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths";

    HKEY root = NULL;
    if (RegOpenKeyExW(hive, kPath, 0,
                      (REGSAM)(KEY_READ | view), &root) != ERROR_SUCCESS) {
        return 0;
    }

    int added = 0;

    for (DWORD i = 0; ; ++i) {
        wchar_t sub[512];
        DWORD cch = (DWORD)(sizeof(sub) / sizeof(sub[0]));

        const LONG r = RegEnumKeyExW(root, i, sub, &cch, NULL, NULL, NULL, NULL);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) break;

        HKEY k = NULL;
        if (RegOpenKeyExW(root, sub, 0, (REGSAM)(KEY_READ | view), &k) != ERROR_SUCCESS) {
            continue;
        }

        /* The default value is the full path of the executable. */
        wchar_t path[APPINDEX_PATH_MAX];
        DWORD cb = (DWORD)sizeof(path);
        DWORD type = 0;

        if (RegQueryValueExW(k, NULL, NULL, &type, (BYTE *)path, &cb) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ) &&
            path[0] != L'\0') {
            /* A few register "%ProgramFiles%\..." rather than the literal
             * path; expanding is harmless when there is nothing to expand. */
            wchar_t expanded[APPINDEX_PATH_MAX];
            if (ExpandEnvironmentStringsW(path, expanded, APPINDEX_PATH_MAX) > 0) {
                wcsncpy_s(path, APPINDEX_PATH_MAX, expanded, _TRUNCATE);
            }

            wchar_t name[APPINDEX_NAME_MAX];
            wcsncpy_s(name, APPINDEX_NAME_MAX, sub, _TRUNCATE);
            strip_exe_suffix(name);
            if (appindex_add(idx, name, path, APPINDEX_FROM_APPPATHS)) added++;
        }

        RegCloseKey(k);
    }

    RegCloseKey(root);
    return added;
}

static int scan_app_paths(AppIndex *idx)
{
    /*
     * All the views a program can register itself in:
     *   HKLM          a machine-wide install
     *   HKLM\WOW6432  the same, from a 32-bit installer
     *   HKCU          a per-user install - easy to forget, and measured here
     *                 as the difference between a usable list and a thin one
     */
    int added = scan_app_paths_view(idx, HKEY_LOCAL_MACHINE, 0);
    added += scan_app_paths_view(idx, HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY);
    added += scan_app_paths_view(idx, HKEY_CURRENT_USER, 0);
    return added;
}

/* ------------------------------------------------------------------ */
/* Source 1b: HKCR\Applications (the same idea, a different tree)      */
/* ------------------------------------------------------------------ */

/*
 * "\"C:\path\app.exe\" %1" - the launch command as Windows stores it. Only
 * the program itself is wanted: everything after it is arguments.
 */
static BOOL exe_from_command_line(const wchar_t *cmd, wchar_t *out, size_t cch)
{
    if (cmd == NULL || out == NULL || cch == 0) return FALSE;
    out[0] = L'\0';

    const wchar_t *p = cmd;
    while (*p == L' ' || *p == L'\t') ++p;
    if (*p == L'\0') return FALSE;

    wchar_t buf[APPINDEX_PATH_MAX];

    if (*p == L'"') {
        ++p;
        const wchar_t *end = wcschr(p, L'"');
        if (end == NULL) return FALSE;
        const size_t len = (size_t)(end - p);
        if (len == 0 || len >= APPINDEX_PATH_MAX) return FALSE;
        wcsncpy_s(buf, APPINDEX_PATH_MAX, p, len);
    } else {
        const wchar_t *end = wcschr(p, L' ');
        const size_t len = (end != NULL) ? (size_t)(end - p) : wcslen(p);
        if (len == 0 || len >= APPINDEX_PATH_MAX) return FALSE;
        wcsncpy_s(buf, APPINDEX_PATH_MAX, p, len);
    }

    /* Values may carry %ProgramFiles% and friends. */
    wchar_t expanded[APPINDEX_PATH_MAX];
    if (ExpandEnvironmentStringsW(buf, expanded, APPINDEX_PATH_MAX) > 0) {
        wcsncpy_s(buf, APPINDEX_PATH_MAX, expanded, _TRUNCATE);
    }

    const size_t n = wcslen(buf);
    if (n <= 4 || _wcsicmp(buf + n - 4, L".exe") != 0) return FALSE;

    wcsncpy_s(out, cch, buf, _TRUNCATE);
    return TRUE;
}

/*
 * HKCR\Applications has one key per executable, named after the file, with the
 * launch command under shell\open\command. It carries the same kind of fact as
 * App Paths and catches programs that never registered there - measured on this
 * machine it is a large part of what makes the list launchable at all.
 *
 * HKCR is the merged HKLM+HKCU view of the same data, so one pass covers both.
 * KEY_READ, nothing written, same privacy line as the other registry readers.
 */
static int scan_applications(AppIndex *idx)
{
    HKEY root = NULL;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"Applications", 0, KEY_READ, &root)
            != ERROR_SUCCESS) {
        return 0;
    }

    int added = 0;

    for (DWORD i = 0; ; ++i) {
        wchar_t sub[512];
        DWORD cch = (DWORD)(sizeof(sub) / sizeof(sub[0]));

        const LONG r = RegEnumKeyExW(root, i, sub, &cch, NULL, NULL, NULL, NULL);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) break;

        /* The tree also holds COM registrations (ProgIDs); only executables
         * are of any use here. */
        const size_t n = wcslen(sub);
        if (n <= 4 || _wcsicmp(sub + n - 4, L".exe") != 0) continue;

        wchar_t cmd_key[600];
        _snwprintf_s(cmd_key, 600, _TRUNCATE, L"%ls\\shell\\open\\command", sub);

        HKEY k = NULL;
        if (RegOpenKeyExW(root, cmd_key, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;

        wchar_t cmd[1024];
        DWORD cb = (DWORD)sizeof(cmd);
        DWORD type = 0;

        if (RegQueryValueExW(k, NULL, NULL, &type, (BYTE *)cmd, &cb) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            wchar_t exe[APPINDEX_PATH_MAX];
            if (exe_from_command_line(cmd, exe, APPINDEX_PATH_MAX)) {
                wchar_t name[APPINDEX_NAME_MAX];
                wcsncpy_s(name, APPINDEX_NAME_MAX, sub, _TRUNCATE);
                strip_exe_suffix(name);
                if (appindex_add(idx, name, exe, APPINDEX_FROM_APPPATHS)) added++;
            }
        }

        RegCloseKey(k);
    }

    RegCloseKey(root);
    return added;
}

/* ------------------------------------------------------------------ */
/* Source 2: Uninstall (a product name, sometimes an icon path)        */
/* ------------------------------------------------------------------ */

/*
 * "C:\path\app.exe,0" or "C:\path\app.exe" - the icon reference carries an
 * optional index after a comma, and only the .exe part is useful as a launch
 * path. Anything that is not an .exe (an .ico, a .dll) is refused rather than
 * trimmed into something that does not run.
 */
static BOOL exe_from_icon_ref(const wchar_t *ref, wchar_t *out, size_t cch)
{
    if (ref == NULL || out == NULL || cch == 0) return FALSE;
    out[0] = L'\0';

    wchar_t buf[APPINDEX_PATH_MAX];
    wcsncpy_s(buf, APPINDEX_PATH_MAX, ref, _TRUNCATE);

    /* Strip the trailing ",<index>" if there is one. */
    wchar_t *comma = wcsrchr(buf, L',');
    if (comma != NULL) *comma = L'\0';

    /* Strip surrounding quotes. */
    wchar_t *p = buf;
    while (*p == L' ') ++p;
    const size_t n = wcslen(p);
    if (n >= 2 && p[0] == L'"' && p[n - 1] == L'"') {
        p[n - 1] = L'\0';
        ++p;
    }

    if (wcslen(p) <= 4 || _wcsicmp(p + wcslen(p) - 4, L".exe") != 0) return FALSE;

    wcsncpy_s(out, cch, p, _TRUNCATE);
    return TRUE;
}

static int scan_uninstall_view(AppIndex *idx, HKEY hive, const wchar_t *subkey)
{
    HKEY root = NULL;
    if (RegOpenKeyExW(hive, subkey, 0, KEY_READ, &root) != ERROR_SUCCESS) return 0;

    int added = 0;

    for (DWORD i = 0; ; ++i) {
        wchar_t sub[512];
        DWORD cch = (DWORD)(sizeof(sub) / sizeof(sub[0]));

        const LONG r = RegEnumKeyExW(root, i, sub, &cch, NULL, NULL, NULL, NULL);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) break;

        HKEY k = NULL;
        if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;

        wchar_t name[APPINDEX_NAME_MAX];
        DWORD cb = (DWORD)sizeof(name);
        DWORD type = 0;
        name[0] = L'\0';

        const BOOL have_name =
            (RegQueryValueExW(k, L"DisplayName", NULL, &type, (BYTE *)name, &cb)
                 == ERROR_SUCCESS) &&
            (type == REG_SZ) && name[0] != L'\0';

        /* SystemComponent entries are the machine's own plumbing: drivers,
         * runtimes, update packages. Listing them would bury the programs the
         * user actually recognises. */
        DWORD sys_component = 0;
        DWORD cb2 = (DWORD)sizeof(sys_component);
        if (RegQueryValueExW(k, L"SystemComponent", NULL, NULL,
                             (BYTE *)&sys_component, &cb2) == ERROR_SUCCESS &&
            sys_component != 0) {
            RegCloseKey(k);
            continue;
        }

        if (have_name) {
            wchar_t icon[APPINDEX_PATH_MAX];
            DWORD cb3 = (DWORD)sizeof(icon);
            DWORD type3 = 0;
            icon[0] = L'\0';

            wchar_t exe[APPINDEX_PATH_MAX];
            const BOOL have_exe =
                (RegQueryValueExW(k, L"DisplayIcon", NULL, &type3, (BYTE *)icon, &cb3)
                     == ERROR_SUCCESS) &&
                (type3 == REG_SZ) &&
                exe_from_icon_ref(icon, exe, APPINDEX_PATH_MAX);

            if (appindex_add(idx, name, have_exe ? exe : NULL,
                             APPINDEX_FROM_UNINSTALL)) {
                added++;
            }
        }

        RegCloseKey(k);
    }

    RegCloseKey(root);
    return added;
}

static int scan_uninstall(AppIndex *idx)
{
    static const wchar_t *kUninstall =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

    int added = scan_uninstall_view(idx, HKEY_LOCAL_MACHINE, kUninstall);
    added += scan_uninstall_view(idx, HKEY_CURRENT_USER, kUninstall);
    return added;
}

/* ------------------------------------------------------------------ */
/* Source 3: the Start Menu listing (names only)                       */
/* ------------------------------------------------------------------ */

#define STARTMENU_MAX_DEPTH 4

/*
 * The target of a shortcut, or FALSE.
 *
 * THIS OPENS ONE FILE, and that is a deliberate, user-approved change to what
 * this program reads. Until 2026-10-10 the Start Menu was listed by NAME only,
 * which left 328 of 413 entries that could be displayed but never launched -
 * and a side bar whose purpose is "make a preset for a program that is not
 * running yet" is useless if most of its rows do nothing. The privacy
 * statement now says so in as many words (manual §1.3): the shortcut's TARGET
 * PATH is read; the file's contents are not, and nothing is written.
 *
 * IShellLink is the system's own reader for this format - no parsing of .lnk
 * bytes by hand, no shell extension of our own. It is COM, so the calling
 * thread must have an apartment; appindex_scan() sets one up.
 *
 * A shortcut pointing at anything that is not an .exe (a document, a folder, a
 * URL) is reported as "no path" rather than trimmed into something that would
 * fail to start.
 */
static BOOL resolve_lnk(const wchar_t *lnk_path, wchar_t *out, size_t cch)
{
    if (lnk_path == NULL || out == NULL || cch == 0) return FALSE;
    out[0] = L'\0';

    IShellLinkW *link = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IShellLinkW, (void **)&link))) {
        return FALSE;
    }

    BOOL ok = FALSE;
    IPersistFile *file = NULL;

    if (SUCCEEDED(link->lpVtbl->QueryInterface(link, &IID_IPersistFile,
                                               (void **)&file))) {
        if (SUCCEEDED(file->lpVtbl->Load(file, lnk_path, STGM_READ))) {
            wchar_t target[APPINDEX_PATH_MAX];
            WIN32_FIND_DATAW fd;
            ZeroMemory(&fd, sizeof(fd));
            target[0] = L'\0';

            if (SUCCEEDED(link->lpVtbl->GetPath(link, target, APPINDEX_PATH_MAX,
                                                &fd, SLGP_UNCPRIORITY)) &&
                target[0] != L'\0') {
                const size_t n = wcslen(target);
                if (n > 4 && _wcsicmp(target + n - 4, L".exe") == 0) {
                    wcsncpy_s(out, cch, target, _TRUNCATE);
                    ok = TRUE;
                }
            }
        }
        file->lpVtbl->Release(file);
    }

    link->lpVtbl->Release(link);
    return ok;
}

static int scan_start_menu_dir(AppIndex *idx, const wchar_t *dir, int depth)
{
    if (depth > STARTMENU_MAX_DEPTH) return 0;

    wchar_t pattern[MAX_PATH];
    _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%ls\\*", dir);

    WIN32_FIND_DATAW fd;
    ZeroMemory(&fd, sizeof(fd));

    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int added = 0;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }

        wchar_t child[MAX_PATH];
        _snwprintf_s(child, MAX_PATH, _TRUNCATE, L"%ls\\%ls", dir, fd.cFileName);

        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            added += scan_start_menu_dir(idx, child, depth + 1);
            continue;
        }

        /* The NAME of the shortcut, and - since 2026-10-10 - its TARGET, so
         * the entry can actually be launched. See resolve_lnk(). */
        const size_t n = wcslen(fd.cFileName);
        if (n <= 4 || _wcsicmp(fd.cFileName + n - 4, L".lnk") != 0) continue;

        wchar_t name[APPINDEX_NAME_MAX];
        wcsncpy_s(name, APPINDEX_NAME_MAX, fd.cFileName, _TRUNCATE);
        name[n - 4] = L'\0';

        wchar_t target[APPINDEX_PATH_MAX];
        const BOOL have_target = resolve_lnk(child, target, APPINDEX_PATH_MAX);

        if (appindex_add(idx, name, have_target ? target : NULL,
                         APPINDEX_FROM_STARTMENU)) {
            added++;
        }

    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return added;
}

static int scan_start_menu(AppIndex *idx)
{
    int added = 0;

    /* Per-user first: a shortcut the user made themselves should not be
     * outweighed by the machine-wide one of the same name. */
    wchar_t dir[MAX_PATH];

    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, dir) == S_OK) {
        wchar_t progs[MAX_PATH];
        _snwprintf_s(progs, MAX_PATH, _TRUNCATE,
                     L"%ls\\Microsoft\\Windows\\Start Menu\\Programs", dir);
        added += scan_start_menu_dir(idx, progs, 0);
    }

    if (SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, dir) == S_OK) {
        wchar_t progs[MAX_PATH];
        _snwprintf_s(progs, MAX_PATH, _TRUNCATE,
                     L"%ls\\Microsoft\\Windows\\Start Menu\\Programs", dir);
        added += scan_start_menu_dir(idx, progs, 0);
    }

    return added;
}

/* ------------------------------------------------------------------ */

int appindex_scan(AppIndex *idx)
{
    if (idx == NULL) return 0;

    /*
     * Resolving shortcuts is COM work, so this thread needs an apartment.
     * S_OK means this call created it and CoUninitialize() is ours to make;
     * S_FALSE means the thread already had one and must keep it.
     */
    const HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    const BOOL must_uninit = (hr == S_OK);

    const int a  = scan_app_paths(idx);
    const int a2 = scan_applications(idx);
    const int b  = scan_uninstall(idx);
    const int c  = scan_start_menu(idx);

    log_info(L"appindex: scanned %d from App Paths, %d from Applications, "
             L"%d from Uninstall, %d from the Start Menu", a, a2, b, c);

    if (must_uninit) CoUninitialize();

    return a + a2 + b + c;
}

unsigned long long appindex_stamp(void)
{
    unsigned long long newest = 0;
    wchar_t dir[MAX_PATH];

    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, dir) == S_OK) {
        wchar_t progs[MAX_PATH];
        _snwprintf_s(progs, MAX_PATH, _TRUNCATE,
                     L"%ls\\Microsoft\\Windows\\Start Menu\\Programs", dir);

        WIN32_FILE_ATTRIBUTE_DATA fad;
        ZeroMemory(&fad, sizeof(fad));
        if (GetFileAttributesExW(progs, GetFileExInfoStandard, &fad)) {
            const unsigned long long t =
                ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32) |
                (unsigned long long)fad.ftLastWriteTime.dwLowDateTime;
            if (t > newest) newest = t;
        }
    }

    if (SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, dir) == S_OK) {
        wchar_t progs[MAX_PATH];
        _snwprintf_s(progs, MAX_PATH, _TRUNCATE,
                     L"%ls\\Microsoft\\Windows\\Start Menu\\Programs", dir);

        WIN32_FILE_ATTRIBUTE_DATA fad;
        ZeroMemory(&fad, sizeof(fad));
        if (GetFileAttributesExW(progs, GetFileExInfoStandard, &fad)) {
            const unsigned long long t =
                ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32) |
                (unsigned long long)fad.ftLastWriteTime.dwLowDateTime;
            if (t > newest) newest = t;
        }
    }

    /* 0 means "no reliable stamp" and makes every cache miss. */
    return newest;
}

/* ------------------------------------------------------------------ */
/* Cache                                                               */
/* ------------------------------------------------------------------ */

static BOOL cache_path(wchar_t *out, size_t cch)
{
    wchar_t dir[MAX_PATH];
    if (!config_dir(dir, MAX_PATH)) return FALSE;
    return (_snwprintf_s(out, cch, _TRUNCATE, L"%ls\\appindex.ini", dir) > 0);
}

/*
 * The cache is PLAIN TEXT, not the shared store, and that is a measured
 * decision: the store caps a document at STORE_MAX_SECTIONS (256) while this
 * list has 343 entries on the development machine. The first version used the
 * store and silently kept only the first 256 - a TRUNCATED list carrying a
 * stamp that says it is current, which is worse than no cache at all.
 *
 * Format: three header lines, then one line per entry.
 *
 *     wac-appindex 1
 *     <stamp>
 *     <count>
 *     <source>\t<name>\t<path>
 *
 * A tab cannot occur in a file name, so splitting on tabs is safe. This is
 * derived data: if it is damaged or short, the caller rescans. That is also
 * why it needs neither the store's checksum nor its rotating backups.
 */

BOOL appindex_cache_load(AppIndex *idx, unsigned long long stamp)
{
    if (idx == NULL || stamp == 0) return FALSE;

    wchar_t path[MAX_PATH];
    if (!cache_path(path, MAX_PATH)) return FALSE;

    FILE *f = NULL;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == NULL) return FALSE;

    char line[APPINDEX_NAME_MAX * 4 + APPINDEX_PATH_MAX * 4 + 32];

    /* A file that does not start with our header is not ours. */
    if (fgets(line, (int)sizeof(line), f) == NULL ||
        strncmp(line, "wac-appindex 1", 14) != 0) {
        fclose(f);
        return FALSE;
    }

    unsigned long long stored = 0;
    if (fgets(line, (int)sizeof(line), f) == NULL ||
        sscanf_s(line, "%llu", &stored) != 1 ||
        stored != stamp) {
        fclose(f);
        return FALSE;
    }

    int count = 0;
    if (fgets(line, (int)sizeof(line), f) == NULL ||
        sscanf_s(line, "%d", &count) != 1 || count <= 0) {
        fclose(f);
        return FALSE;
    }

    int loaded = 0;

    while (loaded < count && fgets(line, (int)sizeof(line), f) != NULL) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        char *tab1 = strchr(line, '\t');
        if (tab1 == NULL) continue;
        *tab1 = '\0';

        char *tab2 = strchr(tab1 + 1, '\t');
        if (tab2 == NULL) continue;
        *tab2 = '\0';

        const int src = atoi(line);

        wchar_t name[APPINDEX_NAME_MAX];
        wchar_t exe[APPINDEX_PATH_MAX];

        str_to_wide(tab1 + 1, name, APPINDEX_NAME_MAX);
        if (tab2[1] != '\0') {
            str_to_wide(tab2 + 1, exe, APPINDEX_PATH_MAX);
        } else {
            exe[0] = L'\0';
        }

        if (name[0] == L'\0') continue;

        if (appindex_add(idx, name, (exe[0] != L'\0') ? exe : NULL,
                         (AppIndexSource)src)) {
            loaded++;
        }
    }

    fclose(f);

    /*
     * A SHORT cache is a WRONG cache: the stamp says it is current, so a list
     * missing rows would be handed over as the truth. Refusing it costs one
     * rescan; accepting it costs the user a program they cannot find.
     */
    if (loaded < count) {
        log_info(L"appindex: cache holds %d of %d entries - rescanning", loaded, count);
        idx->count = 0;
        return FALSE;
    }

    log_info(L"appindex: cache hit, %d entries", loaded);
    return TRUE;
}

BOOL appindex_cache_save(const AppIndex *idx, unsigned long long stamp)
{
    if (idx == NULL || stamp == 0 || idx->count <= 0) return FALSE;

    wchar_t path[MAX_PATH];
    if (!cache_path(path, MAX_PATH)) return FALSE;

    FILE *f = NULL;
    if (_wfopen_s(&f, path, L"wb") != 0 || f == NULL) return FALSE;

    fprintf(f, "wac-appindex 1\n");
    fprintf(f, "%llu\n", stamp);
    fprintf(f, "%d\n", idx->count);

    int written = 0;

    for (int i = 0; i < idx->count; ++i) {
        char name[APPINDEX_NAME_MAX * 4];
        char exe[APPINDEX_PATH_MAX * 4];

        wide_to_str(idx->items[i].name, name, sizeof(name));
        exe[0] = '\0';
        if (idx->items[i].exe_path[0] != L'\0') {
            wide_to_str(idx->items[i].exe_path, exe, sizeof(exe));
        }

        if (fprintf(f, "%d\t%s\t%s\n", (int)idx->items[i].source, name, exe) < 0) break;
        written++;
    }

    fclose(f);

    /* The header claims a count. If the body came out shorter, the file is a
     * lie - delete it rather than leave something the loader must distrust. */
    if (written != idx->count) {
        log_info(L"appindex: cache write stopped at %d of %d entries - discarded",
                 written, idx->count);
        _wremove(path);
        return FALSE;
    }

    log_info(L"appindex: cache written (%d entries)", written);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* The worker thread                                                   */
/* ------------------------------------------------------------------ */

struct AppIndexJob {
    HANDLE    thread;
    HWND      notify_wnd;
    UINT      notify_msg;
    volatile LONG done;
    AppIndex *result;          /* handed over by appindex_job_take() */
    unsigned long elapsed_ms;
    DWORD     start_tick;
};

static DWORD WINAPI job_proc(LPVOID param)
{
    AppIndexJob *job = (AppIndexJob *)param;

    const unsigned long long stamp = appindex_stamp();

    AppIndex *idx = appindex_create();
    if (idx != NULL) {
        /* The cache first: on a machine where nothing changed since last time
         * this is the whole job, and it takes milliseconds. */
        if (!appindex_cache_load(idx, stamp)) {
            appindex_scan(idx);
            appindex_dedup(idx);
            appindex_cache_save(idx, stamp);
        }
    }

    job->result     = idx;
    job->elapsed_ms = GetTickCount() - job->start_tick;
    InterlockedExchange(&job->done, 1);

    if (job->notify_wnd != NULL) {
        PostMessageW(job->notify_wnd, job->notify_msg, 0, 0);
    }
    return 0;
}

AppIndexJob *appindex_job_start(HWND notify_wnd, UINT notify_msg)
{
    AppIndexJob *job = (AppIndexJob *)calloc(1, sizeof(AppIndexJob));
    if (job == NULL) return NULL;

    job->notify_wnd = notify_wnd;
    job->notify_msg = notify_msg;
    job->start_tick = GetTickCount();

    job->thread = CreateThread(NULL, 0, job_proc, job, 0, NULL);
    if (job->thread == NULL) {
        log_info(L"appindex: could not start the worker thread");
        free(job);
        return NULL;
    }

    return job;
}

AppIndex *appindex_job_take(AppIndexJob *job)
{
    if (job == NULL) return NULL;
    if (InterlockedCompareExchange(&job->done, 1, 1) == 0) return NULL;

    AppIndex *r = job->result;
    job->result = NULL;
    return r;
}

unsigned long appindex_job_elapsed_ms(const AppIndexJob *job)
{
    return (job != NULL) ? job->elapsed_ms : 0;
}

BOOL appindex_job_done(const AppIndexJob *job)
{
    if (job == NULL) return TRUE;
    return (InterlockedCompareExchange((volatile LONG *)&job->done, 1, 1) != 0)
               ? TRUE : FALSE;
}

void appindex_job_destroy(AppIndexJob *job)
{
    if (job == NULL) return;

    if (job->thread != NULL) {
        /* Waited for, never killed: the worker only reads the registry and the
         * Start Menu, so it always finishes - and a scan is bounded work, not
         * an open-ended loop. */
        WaitForSingleObject(job->thread, 15000);
        CloseHandle(job->thread);
    }

    if (job->result != NULL) appindex_destroy(job->result);
    free(job);
}
