/*
 * launch.c - see launch.h for why the search works the way it does.
 *
 * Everything here that could be called a heuristic is labelled as one, and
 * every heuristic has a fallback that reports what it did rather than
 * silently returning "no window".
 */

#include "launch.h"
#include "launch_score.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* ------------------------------------------------------------------ */
/* Static storage                                                      */
/*                                                                     */
/* A WindowInfo is about 1.3 KB, so 256 of them is ~330 KB. That does  */
/* not belong on a 1 MB stack - the project already lost a day to a 26 */
/* MB struct on the stack (TRAPS.md #11).                   */
/* ------------------------------------------------------------------ */

static WindowInfo g_scan[WAC_WIN_MAX];
static HWND       g_snap[WAC_WIN_MAX];
static int        g_snap_count;

/* ------------------------------------------------------------------ */
/* Tracing                                                             */
/* ------------------------------------------------------------------ */

static void tracef(LaunchTrace f, void *ctx, const wchar_t *fmt, ...)
{
    wchar_t buf[512];
    va_list ap;

    if (f == NULL) return;

    va_start(ap, fmt);
    _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    f(ctx, buf);
}

/* ------------------------------------------------------------------ */
/* Command line                                                        */
/* ------------------------------------------------------------------ */

void launch_derive_exe(const wchar_t *command, wchar_t *out, size_t cch)
{
    const wchar_t *p;
    const wchar_t *end;
    wchar_t token[WAC_LAUNCH_CMD_MAX];
    size_t len;

    if (out == NULL || cch == 0) return;
    out[0] = L'\0';
    if (command == NULL) return;

    p = command;
    while (*p == L' ' || *p == L'\t') ++p;

    if (*p == L'"') {
        ++p;
        end = wcschr(p, L'"');
        if (end == NULL) end = p + wcslen(p);
    } else {
        end = p;
        while (*end != L'\0' && *end != L' ' && *end != L'\t') ++end;
    }

    len = (size_t)(end - p);
    if (len == 0 || len >= WAC_LAUNCH_CMD_MAX) return;

    wcsncpy_s(token, WAC_LAUNCH_CMD_MAX, p, len);
    wcsncpy_s(out, cch, win_file_name(token), _TRUNCATE);
}

/* ------------------------------------------------------------------ */
/* Snapshot                                                            */
/* ------------------------------------------------------------------ */

/*
 * The same narrow filter the wait loop uses, not an unfiltered scan.
 *
 * MEASURED: an ordinary desktop holds over 256 top-level windows, most of
 * them invisible helpers. An unfiltered snapshot fills its array and silently
 * stops recording - and every window it failed to record would later look
 * "new". With the filter the snapshot is about twenty handles, so nothing is
 * missed.
 */
static void snapshot_windows(void)
{
    int n = win_enum_top(g_scan, WAC_WIN_MAX,
                         WIN_ENUM_VISIBLE_ONLY | WIN_ENUM_SKIP_TOOLWIN |
                         WIN_ENUM_SKIP_SMALL | WIN_ENUM_NEED_TITLE);

    g_snap_count = 0;
    for (int i = 0; i < n && g_snap_count < WAC_WIN_MAX; ++i) {
        g_snap[g_snap_count++] = g_scan[i].hwnd;
    }
}

static int is_new_window(HWND hwnd)
{
    for (int i = 0; i < g_snap_count; ++i) {
        if (g_snap[i] == hwnd) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Starting                                                            */
/* ------------------------------------------------------------------ */

void launch_snapshot_take(void)
{
    snapshot_windows();
}

void launch_prepare(Launch *l, const wchar_t *exe_path)
{
    if (l == NULL) return;

    ZeroMemory(l, sizeof(*l));
    if (exe_path == NULL) return;

    wcsncpy_s(l->command, WAC_LAUNCH_CMD_MAX, exe_path, _TRUNCATE);
    launch_derive_exe(exe_path, l->exe_name, WAC_EXE_MAX);
}

BOOL launch_start(Launch *l, const wchar_t *command)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t cmd[WAC_LAUNCH_CMD_MAX];

    if (l == NULL || command == NULL || command[0] == L'\0') return FALSE;

    ZeroMemory(l, sizeof(*l));
    wcsncpy_s(l->command, WAC_LAUNCH_CMD_MAX, command, _TRUNCATE);
    launch_derive_exe(command, l->exe_name, WAC_EXE_MAX);

    /*
     * Snapshot FIRST. Doing it after CreateProcessW would race with the
     * window appearing, and the window we are looking for would already be
     * in the snapshot - so nothing would ever look new.
     */
    snapshot_windows();

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    /* CreateProcessW may write into the command line buffer, so it must not
     * be a string literal. */
    wcsncpy_s(cmd, WAC_LAUNCH_CMD_MAX, command, _TRUNCATE);

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, NULL, NULL, &si, &pi)) {
        return FALSE;
    }

    l->pid = pi.dwProcessId;

    /*
     * Both handles are closed at once. The process handle is NOT kept: for a
     * packaged application the process just started is a launcher that exits
     * immediately, so waiting on it would report "finished" while the real
     * window is still coming up.
     */
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Waiting                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    const wchar_t *exe_name;   /* NULL = any executable */
    int            only_new;   /* skip windows from the pre-launch snapshot */
} WaitFilter;

static HWND best_matching(const WaitFilter *wf, RECT *out_rect, LONGLONG *out_score)
{
    /*
     * NO WIN_ENUM_NEED_EXE HERE, and that is on purpose.
     *
     * Asking the enumerator for every window's executable path opens a process
     * handle for each one. This loop runs every 50 ms for up to twenty
     * seconds, so a single launch used to mean thousands of handle opens
     * against unrelated processes - which is exactly what a process scanner
     * looks like to a security product. Measured: a workspace run was flagged
     * by Huorong on the user's machine.
     *
     * Only the NEW windows need a name, and newness is decided by comparing
     * handles against an in-memory snapshot. So the filter runs first and the
     * (usually one or two) survivors are asked individually.
     *
     * The flags below still cut the enumeration down before any of this:
     * visible, not a tool window, has a title, not tiny.
     */
    const unsigned flags = WIN_ENUM_VISIBLE_ONLY | WIN_ENUM_SKIP_TOOLWIN |
                           WIN_ENUM_SKIP_SMALL | WIN_ENUM_NEED_TITLE;

    const int n = win_enum_top(g_scan, WAC_WIN_MAX, flags);
    HWND best = NULL;
    LONGLONG best_score = 0;

    for (int i = 0; i < n; ++i) {
        const WindowInfo *w = &g_scan[i];
        LONGLONG s;

        if (wf->only_new && !is_new_window(w->hwnd)) continue;

        if (wf->exe_name != NULL && wf->exe_name[0] != L'\0') {
            wchar_t path[MAX_PATH];
            if (!win_exe_path(w->pid, path, MAX_PATH)) continue;
            if (_wcsicmp(win_file_name(path), wf->exe_name) != 0) continue;
        }

        s = launch_score_candidate(w);
        if (s > best_score) {
            best_score = s;
            best = w->hwnd;
            if (out_rect != NULL) *out_rect = w->rect;
        }
    }

    if (out_score != NULL) *out_score = best_score;
    return best;
}

HWND launch_wait(Launch *l, int budget_ms, LaunchTrace trace, void *ctx)
{
    DWORD start;
    DWORD settle_start = 0;
    HWND  settled = NULL;
    RECT  settle_rect;
    WaitFilter wf;

    if (l == NULL) return NULL;
    if (budget_ms < 100) budget_ms = 100;

    l->hwnd = NULL;

    ZeroMemory(&settle_rect, sizeof(settle_rect));
    ZeroMemory(&wf, sizeof(wf));
    wf.exe_name = l->exe_name;
    wf.only_new = 1;

    tracef(trace, ctx, L"waiting up to %d ms for a NEW window of %ls that holds "
                       L"still for %d ms",
           budget_ms,
           (l->exe_name[0] != L'\0') ? l->exe_name : L"(any program)",
           WAC_LAUNCH_SETTLE_MS);

    start = GetTickCount();

    for (;;) {
        RECT r;
        LONGLONG score = 0;
        DWORD now;
        HWND h;

        ZeroMemory(&r, sizeof(r));
        h = best_matching(&wf, &r, &score);
        now = GetTickCount();

        if (h == NULL) {
            settle_start = 0;
            settled = NULL;
        } else if (h != settled) {
            /* A new best candidate: start its settle clock. */
            settled = h;
            settle_rect = r;
            settle_start = now;
            tracef(trace, ctx, L"candidate %p  %dx%d at (%d,%d) - settling",
                   (void *)h,
                   (int)(r.right - r.left), (int)(r.bottom - r.top),
                   (int)r.left, (int)r.top);
        } else if (r.left   != settle_rect.left  || r.top    != settle_rect.top ||
                   r.right  != settle_rect.right || r.bottom != settle_rect.bottom) {
            /* It moved or resized: still starting up. Restart the clock. */
            tracef(trace, ctx, L"candidate %p changed geometry - settling again",
                   (void *)h);
            settle_rect = r;
            settle_start = now;
        } else if (now - settle_start >= WAC_LAUNCH_SETTLE_MS) {
            tracef(trace, ctx, L"settled: %p  %dx%d at (%d,%d)",
                   (void *)h,
                   (int)(r.right - r.left), (int)(r.bottom - r.top),
                   (int)r.left, (int)r.top);
            l->hwnd = h;
            return h;
        }

        if (now - start >= (DWORD)budget_ms) break;

        /*
         * 150 ms, not 50.
         *
         * Every pass of this loop is a full EnumWindows sweep, and a security
         * product watching a program sweep the window list on a timer sees
         * scanning - which is how a workspace run got flagged by Huorong on the
         * user's machine. The settle window is 300 ms, so a coarser tick costs
         * at most 150 ms of extra latency before a window is accepted, and
         * nothing about which window wins.
         *
         * THE REAL FIX IS NOT A SLOWER TIMER, IT IS NO TIMER: SetWinEventHook
         * (stage 5, task 5.11) reacts to windows appearing instead of looking
         * for them. That needs a message loop, which this synchronous design
         * does not have yet - so the tick gets slower for now, and the
         * reasoning is written down rather than left as a magic number.
         */
        Sleep(150);
    }

    /*
     * Timed out with nothing settled. Take the largest candidate anyway: a
     * window that is present but slow to settle is a far better outcome than
     * "no window found", and the caller is told which path was taken.
     */
    {
        RECT r;
        LONGLONG score = 0;
        HWND h;

        ZeroMemory(&r, sizeof(r));
        h = best_matching(&wf, &r, &score);

        if (h != NULL) {
            tracef(trace, ctx, L"budget exhausted; falling back to the largest "
                               L"candidate: %p  %dx%d at (%d,%d)",
                   (void *)h,
                   (int)(r.right - r.left), (int)(r.bottom - r.top),
                   (int)r.left, (int)r.top);
            l->hwnd = h;
        } else {
            tracef(trace, ctx, L"no window matched - the program may have exited, "
                               L"or it shows no top-level window");
        }
        return h;
    }
}
