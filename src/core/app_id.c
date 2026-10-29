/*
 * app_id.c - see app_id.h for the identity model and the precedence rules.
 *
 * All comparisons are case-insensitive with the "invariant" locale behaviour
 * of _wcsicmp, which is what the default Windows filesystem and window class
 * names both expect.
 */

#include "app_id.h"

#include <wchar.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Equality where an empty value means "unset", never "matches empty". */
static BOOL eq_set(const wchar_t *a, const wchar_t *b)
{
    if (a == NULL || a[0] == L'\0') return FALSE;
    if (b == NULL) return FALSE;
    return (_wcsicmp(a, b) == 0) ? TRUE : FALSE;
}

static BOOL contains_i(const wchar_t *hay, const wchar_t *needle)
{
    if (hay == NULL || needle == NULL) return FALSE;

    size_t n = wcslen(needle);
    if (n == 0) return FALSE;

    /* Walk every starting position rather than using wcsstr, which is
     * case-sensitive. The titles being searched are short. */
    for (const wchar_t *p = hay; *p != L'\0'; ++p) {
        if (_wcsnicmp(p, needle, n) == 0) return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */

BOOL app_id_empty(const AppId *id)
{
    if (id == NULL) return TRUE;

    if (id->exe_name[0]   != L'\0') return FALSE;
    if (id->exe_path[0]   != L'\0') return FALSE;
    if (id->class_name[0] != L'\0') return FALSE;
    if (id->title_sub[0]  != L'\0') return FALSE;
    if (id->pid           != 0)     return FALSE;
    if (id->hwnd          != NULL)  return FALSE;

    return TRUE;
}

BOOL app_id_from_window(const WindowInfo *w, AppId *out)
{
    if (w == NULL || out == NULL) return FALSE;

    ZeroMemory(out, sizeof(*out));

    if (w->exe_path[0] == L'\0') {
        /*
         * Deliberately no fallback to the pid. A rule built from a pid alone
         * looks like it works - it matches right now - and then silently
         * matches nothing, or worse, a different program, after a restart.
         * The caller is told instead.
         */
        return FALSE;
    }

    /*
     * exe_name, NOT exe_path - and this is deliberate.
     *
     * MEASURED (2026-09-20): the Windows 11 Notepad runs from
     *
     *   ...\WindowsApps\Microsoft.WindowsNotepad_11.2607.14.0_x64__8wekyb3d8bbwe\Notepad\Notepad.exe
     *
     * and that version number changes on every Store update. A rule pinned
     * to the full path would stop matching after an update: silently, and at
     * the worst possible moment, because the user is not thinking about this
     * program when their editor updates itself.
     *
     * The file name plus the window class identifies an application in
     * practice. A caller that genuinely needs to separate two installs of the
     * same program sets exe_path explicitly through app_id_set_exe().
     */
    wcsncpy_s(out->exe_name, WAC_EXE_MAX, win_file_name(w->exe_path), _TRUNCATE);
    wcsncpy_s(out->class_name, WAC_CLASS_MAX, w->class_name, _TRUNCATE);
    out->exe_path[0] = L'\0';

    /* Title and pid are NOT copied: they would make the rule match only the
     * window it was built from, which is the opposite of what a stored rule
     * is for. Both remain available to a caller that wants them. */
    out->pid  = 0;
    out->hwnd = NULL;

    return TRUE;
}

void app_id_set_exe(AppId *id, const wchar_t *exe_or_name)
{
    if (id == NULL || exe_or_name == NULL) return;

    const wchar_t *slash = wcsrchr(exe_or_name, L'\\');
    if (slash == NULL) slash = wcsrchr(exe_or_name, L'/');

    if (slash != NULL) {
        wcsncpy_s(id->exe_path, WAC_EXE_MAX, exe_or_name, _TRUNCATE);
        wcsncpy_s(id->exe_name, WAC_EXE_MAX, slash + 1, _TRUNCATE);
    } else {
        /* A bare name: leave exe_path empty so the path is not compared, and
         * any install of this program matches. */
        id->exe_path[0] = L'\0';
        wcsncpy_s(id->exe_name, WAC_EXE_MAX, exe_or_name, _TRUNCATE);
    }
}

/* ------------------------------------------------------------------ */
/* Matching                                                            */
/* ------------------------------------------------------------------ */

BOOL app_id_match(const AppId *rule, const WindowInfo *w)
{
    if (rule == NULL || w == NULL) return FALSE;
    if (app_id_empty(rule)) return FALSE;

    /* Exact handles first: the caller has already identified one window. */
    if (rule->hwnd != NULL) return (rule->hwnd == w->hwnd) ? TRUE : FALSE;
    if (rule->pid != 0)     return (rule->pid == w->pid) ? TRUE : FALSE;

    /*
     * A rule that names an executable requires the window's executable to be
     * KNOWN. Without this, a window whose path could not be read (protected
     * process) would have an empty exe_path, and an empty string must never
     * be treated as satisfying a non-empty rule.
     */
    if (rule->exe_path[0] != L'\0') {
        if (!eq_set(rule->exe_path, w->exe_path)) return FALSE;
    } else if (rule->exe_name[0] != L'\0') {
        if (w->exe_path[0] == L'\0') return FALSE;
        if (!eq_set(rule->exe_name, win_file_name(w->exe_path))) return FALSE;
    }

    if (rule->class_name[0] != L'\0') {
        if (!eq_set(rule->class_name, w->class_name)) return FALSE;
    }

    if (rule->title_sub[0] != L'\0') {
        if (!contains_i(w->title, rule->title_sub)) return FALSE;
    }

    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Description                                                         */
/* ------------------------------------------------------------------ */

int app_id_describe(const AppId *id, wchar_t *out, size_t cch)
{
    if (out == NULL || cch == 0) return 0;
    out[0] = L'\0';
    if (id == NULL) return 0;

    wchar_t buf[WAC_EXE_MAX + WAC_CLASS_MAX + WAC_TITLE_MAX + 64];
    buf[0] = L'\0';

    /* Precedence mirrors app_id_match(), so the text says which field will
     * actually decide the match rather than listing everything. */
    if (id->hwnd != NULL) {
        _snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, L"hwnd=0x%p", (void *)id->hwnd);
    } else if (id->pid != 0) {
        _snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, L"pid=%lu", (unsigned long)id->pid);
    } else {
        size_t used = 0;
        buf[0] = L'\0';

        if (id->exe_path[0] != L'\0') {
            _snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, L"%ls", id->exe_path);
        } else if (id->exe_name[0] != L'\0') {
            _snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, L"%ls", id->exe_name);
        }
        used = wcslen(buf);

        if (id->class_name[0] != L'\0' && used + 12 < ARRAYSIZE(buf)) {
            _snwprintf_s(buf + used, ARRAYSIZE(buf) - used, _TRUNCATE,
                         L"%lsclass=%ls", used ? L" " : L"", id->class_name);
            used = wcslen(buf);
        }
        if (id->title_sub[0] != L'\0' && used + 12 < ARRAYSIZE(buf)) {
            _snwprintf_s(buf + used, ARRAYSIZE(buf) - used, _TRUNCATE,
                         L"%lstitle~\"%ls\"", used ? L" " : L"", id->title_sub);
        }

        if (buf[0] == L'\0') {
            wcsncpy_s(buf, ARRAYSIZE(buf), L"(empty rule)", _TRUNCATE);
        }
    }

    wcsncpy_s(out, cch, buf, _TRUNCATE);
    return (int)wcslen(out);
}
