/*
 * t03_uipi.c - stage 1, experiment 4
 *
 * PURPOSE
 *   Establish the permission boundary the product must live with, and
 *   confirm that "permission denied" is DISTINGUISHABLE from other
 *   failures. The product runs as asInvoker on purpose, so it cannot move
 *   windows of elevated processes; src/core/win_place.c must return
 *   PLACE_DENIED for those and tell the user, rather than failing
 *   silently or retrying.
 *
 * WHY THIS MATTERS FOR THE PLAN
 *   If SetWindowPos on an elevated window returned success-but-no-effect,
 *   the product could not report the problem honestly. This program
 *   determines which it is.
 *
 * HOW TO RUN
 *     t03_uipi.exe              sweep every visible top-level window
 *     t03_uipi.exe --title "记事??
 *
 *   Run it BOTH as a standard user and as administrator, and compare.
 *   With at least one elevated process running (Task Manager is easiest:
 *   it is always elevated), the standard-user run must show DENIED for it.
 *
 *   To create an elevated target from a standard shell:
 *     runas /user:Administrator notepad      (or use "Run as administrator")
 *
 * WHAT TO RECORD (into docs/P0验证记录.md, experiment 4)
 *   - whether the failure is silent or returns a distinct error code
 *   - the exact error code and its decoded meaning
 *   - whether the window moved anyway
 *
 * BUILD
 *   tests\build_tests.cmd t03_uipi
 */

#include "test_common.h"

#define MAX_WINDOWS 256

/*
 * Move a window by a few pixels with SWP_NOACTIVATE, then put it back.
 *
 * Note we deliberately do NOT use SetWindowPos' success flag alone as the
 * verdict: the whole point of this test is to find out whether a failure
 * is detectable from the return value, from GetLastError, from the
 * observed geometry, or from none of the above.
 */
typedef enum {
    MOVE_OK = 0,
    MOVE_FAILED_API,
    MOVE_SILENT_NOEFFECT,   /* API said success but geometry unchanged */
    MOVE_DENIED_MOVED_ANYWAY
} MoveVerdict;

static MoveVerdict try_move(HWND hwnd, int dx, int dy, DWORD *out_err,
                            RECT *out_before, RECT *out_after)
{
    *out_err = 0;
    GetWindowRect(hwnd, out_before);

    SetLastError(0);   /* make GetLastError meaningful */
    BOOL ok = SetWindowPos(hwnd, NULL, dx, dy, 0, 0,
                           SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    *out_err = ok ? 0 : GetLastError();

    GetWindowRect(hwnd, out_after);

    int moved = (out_after->left != out_before->left) ||
                (out_after->top  != out_before->top);

    if (!ok)  return moved ? MOVE_DENIED_MOVED_ANYWAY : MOVE_FAILED_API;
    return moved ? MOVE_OK : MOVE_SILENT_NOEFFECT;
}

static int g_elevated = -1;   /* -1 unknown, 0 no, 1 yes */

/*
 * Determine whether THIS process is elevated.
 * Opening our own token with TOKEN_QUERY and reading TokenElevation is the
 * documented way; it avoids shelling out to `whoami /groups`.
 */
static int detect_elevation(void)
{
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return -1;

    TOKEN_ELEVATION elev;
    DWORD size = sizeof(elev);
    int result = -1;
    if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &size)) {
        result = elev.TokenIsElevated ? 1 : 0;
    }
    CloseHandle(token);
    return result;
}

static int g_denied = 0, g_ok = 0, g_silent = 0, g_other = 0;

static void test_one(HWND hwnd, int verbose)
{
    if (!t_is_app_window(hwnd)) return;

    DWORD pid = t_window_pid(hwnd);
    const wchar_t *proc = t_process_name(pid);
    const wchar_t *title = t_window_title(hwnd);

    /* Some windows vanish mid-test; that is not a permission result. */
    if (!IsWindow(hwnd)) return;

    DWORD err = 0;
    RECT before, after;
    MoveVerdict v = try_move(hwnd, 0, 0, &err, &before, &after);   /* no-op move */

    /* A no-op move proves nothing; retry with a real 1px shift. */
    if (v == MOVE_SILENT_NOEFFECT) {
        v = try_move(hwnd, before.left + 1, before.top, &err, &before, &after);
        /* put it back */
        SetWindowPos(hwnd, NULL, before.left, before.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    const wchar_t *verdict;
    switch (v) {
    case MOVE_OK:                  verdict = L"OK";                 g_ok++;     break;
    case MOVE_FAILED_API:          verdict = L"DENIED(api)";        g_denied++; break;
    case MOVE_SILENT_NOEFFECT:     verdict = L"NO-EFFECT(silent)";  g_silent++; break;
    case MOVE_DENIED_MOVED_ANYWAY: verdict = L"ERR+MOVED";          g_other++;  break;
    default:                       verdict = L"?";                              break;
    }

    if (v == MOVE_OK && !verbose) return;   /* keep the report short */

    t_out_line(L"[WIN.%-16ls] pid=%-7lu %-24ls err=%-6lu \"%ls\"",
            verdict, (unsigned long)pid, proc, (unsigned long)err, title);

    if (v == MOVE_FAILED_API && err) {
        static DWORD last_reported = 0;
        if (err != last_reported) {
            t_winerr(L"SetWindowPos", err);
            T_NOTE(L"^ decode shown once per distinct error code");
            last_reported = err;
        }
    }
}

typedef struct { int verbose; } SweepCtx;

static BOOL CALLBACK sweep_proc(HWND hwnd, LPARAM lp)
{
    test_one(hwnd, ((SweepCtx *)lp)->verbose);
    return TRUE;
}

int wmain(int argc, wchar_t **argv)
{
    /* Crash filter + file log first: a console that flashes and closes
       takes its output with it. See TRAPS.md. */
    t_diag_init(L"t03_uipi");
    t_log_mirror(1);   /* full report goes to the log by default */

    t_console_init();

    const wchar_t *want_title = L"";
    int verbose = 0;
    for (int i = 1; i < argc; ++i) {
        if      (!wcscmp(argv[i], L"--title") && i + 1 < argc) want_title = argv[++i];
        else if (!wcscmp(argv[i], L"--verbose")) verbose = 1;
        else if (!wcscmp(argv[i], L"--help")) {
            t_out_line(L"usage: t03_uipi.exe [--title T] [--verbose]");
            return 0;
        }
    }

    T_TITLE(L"t03_uipi - window control permission boundary");
    T_KV(L"run_at", L"%ls", t_now_string());

    t_set_dpi_awareness();

    g_elevated = detect_elevation();
    T_KV(L"this_process_elevated", L"%ls",
         g_elevated == 1 ? L"YES (administrator)" :
         g_elevated == 0 ? L"NO (standard user)" : L"UNKNOWN (token query failed)");

    if (g_elevated == 1) {
        T_WARN(L"You are running elevated. Every window below will look movable,");
        T_WARN(L"which does NOT reflect how the shipped product behaves.");
        T_WARN(L"Re-run this program from a NON-elevated console for the real answer.");
    }

    T_SEC(L"Reading the results");
    T_NOTE(L"OK              - window moved, as expected for same-integrity targets");
    T_NOTE(L"DENIED(api)     - SetWindowPos returned FALSE with a distinct error code");
    T_NOTE(L"NO-EFFECT       - API returned success but the window did not move");
    T_NOTE(L"ERR+MOVED       - API failed yet the window moved (should not happen)");
    T_NOTE(L"");
    T_NOTE(L"The product needs DENIED(api) to be the normal failure mode, so it can");
    T_NOTE(L"report PLACE_DENIED honestly. If NO-EFFECT dominates instead, the");
    T_NOTE(L"product must detect refusal by comparing geometry, not by the return value.");

    T_SEC(L"Sweep");

    if (*want_title) {
        T_KV(L"filter", L"title contains \"%ls\"", want_title);
    } else {
        T_KV(L"filter", L"none - every visible top-level window is tried");
        T_NOTE(L"Only non-OK results are printed unless --verbose is given.");
    }

    t_out_line(L"\n%-24ls %-9ls %-24ls %-9ls %ls",
            L"verdict", L"pid", L"process", L"err", L"title");

    if (*want_title) {
        /* Reuse the shared enumerator, then apply the title filter. */
        TWindowList list;
        int n = t_enum_app_windows(&list, MAX_WINDOWS);
        for (int i = 0; i < n; ++i) {
            if (t_contains_i(t_window_title(list.items[i].hwnd), want_title)) {
                sweep_proc(list.items[i].hwnd, (LPARAM)&(SweepCtx){ verbose });
            }
        }
        free(list.items);
    } else {
        SweepCtx ctx = { verbose };
        EnumWindows(sweep_proc, (LPARAM)&ctx);
    }

    T_SEC(L"Summary");
    T_KV(L"movable_ok", L"%d", g_ok);
    T_KV(L"denied_with_error", L"%d", g_denied);
    T_KV(L"silent_no_effect", L"%d", g_silent);
    T_KV(L"failed_but_moved", L"%d", g_other);

    T_SEC(L"Verdict");
    if (g_elevated == 1) {
        T_WARN(L"Run was elevated - re-run from a standard console to get a valid result.");
    } else if (g_denied > 0 && g_silent == 0) {
        T_KV(L"diagnosis", L"PASS - denial is reported through SetWindowPos + GetLastError");
        T_NOTE(L"win_place.c can rely on the return value to produce PLACE_DENIED.");
    } else if (g_denied > 0 && g_silent > 0) {
        T_KV(L"diagnosis", L"MIXED - some refusals are silent");
        T_NOTE(L"win_place.c should verify geometry after every SetWindowPos and");
        T_NOTE(L"classify a no-change result as refusal, regardless of the return value.");
    } else if (g_silent > 0) {
        T_KV(L"diagnosis", L"SILENT REFUSAL - the return value is not trustworthy");
        T_WARN(L"win_place.c MUST compare geometry to detect refusal.");
    } else {
        T_KV(L"diagnosis", L"no denials observed");
        T_NOTE(L"Make sure an elevated process was running (Task Manager) and that");
        T_NOTE(L"this test itself ran WITHOUT elevation. Note: Task Manager's window");
        T_NOTE(L"may be a child of a broker process, so also try an elevated Notepad.");
    }

    t_pause_if_console();
    t_log_close();
    return 0;
}
