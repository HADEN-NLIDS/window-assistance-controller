/*
 * t02_move.c - stage 1, experiment 3
 *
 * PURPOSE
 *   Decide the tolerance threshold used by the read-back verification in
 *   src/core/win_place.c, and confirm that "does this window accept
 *   resizing?" can be answered by writing a size and reading it back
 *   rather than by maintaining a hard-coded list of applications.
 *
 *   It also measures the WM_GETMINMAXINFO floor: the smallest size a
 *   window will actually accept, which becomes RelRect.min_w / min_h.
 *
 * HOW TO RUN
 *     t02_move.exe                          list candidate windows
 *     t02_move.exe --title "记事本          test the first title match
 *     t02_move.exe --app notepad.exe        test the first process match
 *     t02_move.exe --index 2                pick the Nth listed window
 *     t02_move.exe --title X --size-only    skip the position test
 *
 *   Test at least: Notepad, a Chromium browser, Calculator, and a
 *   properties dialog (固定尺寸).
 *
 * WHAT TO RECORD (into docs/P0验证记录.md, experiment 3)
 *   - max deviation in px across all cases  -> becomes tolerance_px
 *   - which windows refused resizing        -> confirms the probe design
 *   - the measured minimum accepted size
 *
 * BUILD
 *   tests\build_tests.cmd t02_move
 */

#include "test_common.h"

#include <tlhelp32.h>   /* CreateToolhelp32Snapshot: enumerate processes by name */

#define MAX_WINDOWS 256

static HWND  g_target = NULL;
static RECT  g_original;

/*
 * Read-back tolerance, in pixels.
 *
 * MEASURED (2026-09-19, 2560x1600 @150%, PerMonitorV2): Notepad accepted
 * every requested size with dx=dy=dw=dh=0, and so did Edge once it had been
 * restored from maximised. The deviation is not "small", it is zero - under
 * PerMonitorV2 there is no DPI scaling term in the round trip at all, and
 * GetWindowRect includes the DWM frame, so request and read-back agree
 * exactly.
 *
 * 8 px is therefore generous headroom rather than a measured requirement.
 * It is kept because a tighter value buys nothing: the only thing it would
 * change is whether a window that refuses resizing gets reported as
 * MOVED_ONLY (true) or CLAMPED (a lie). The cost of being generous is that
 * a window sitting a few pixels off its requested size is treated as
 * compliant - acceptable, since the user's intent is "roughly this area".
 */
static int   g_tolerance = 8;
static int   g_size_only = 0;
static int   g_show_all = 0;   /* --all: list rejected windows too */
static int   g_show_shell = 0; /* --all --shell: include shell plumbing */
static int   g_no_pause = 0;   /* --no-pause: do not wait for a key */
static int   g_move_only = 0;  /* --move-only: test repositioning only */
static int   g_open_log = 0;   /* --open-log: show the log when finished */

static void print_window_list(TWindowList *list)
{
    T_SEC(L"Candidate windows");
    t_out_line(L"  %-3ls %-9ls %-24ls %-22ls %ls",
               L"#", L"pid", L"process", L"rect", L"title");
    for (int i = 0; i < list->count; ++i) {
        TWindowInfo *w = &list->items[i];
        wchar_t rect[32];
        _snwprintf_s(rect, 32, _TRUNCATE, L"%ld,%ld %ldx%ld",
                     w->rect.left, w->rect.top,
                     w->rect.right - w->rect.left,
                     w->rect.bottom - w->rect.top);
        t_out_line(L"  %-3d %-9lu %-24ls %-22ls %ls",
                   i, (unsigned long)w->pid, t_process_name(w->pid), rect,
                   t_window_title(w->hwnd));
    }
}

/* Windows the user is unlikely to be looking for. Kept short and matched
   case-insensitively; the point is to remove the noise that explorer.exe
   and the shell generate, not to be exhaustive. */
static int is_shell_noise(const wchar_t *proc)
{
    static const wchar_t *noise[] = {
        L"explorer.exe", L"ShellExperienceHost.exe", L"SearchHost.exe",
        L"StartMenuExperienceHost.exe", L"TextInputHost.exe",
        L"ApplicationFrameHost.exe", L"dwm.exe", L"csrss.exe",
        L"winlogon.exe", L"RuntimeBroker.exe", L"SearchApp.exe",
    };
    for (int i = 0; i < (int)(sizeof(noise) / sizeof(noise[0])); ++i) {
        if (!_wcsicmp(proc, noise[i])) return 1;
    }
    return 0;
}

/* Enumerate EVERY top-level window and say why each one was accepted or
   rejected. Exists because a window the user could plainly see on screen
   (a UU accelerator panel) never appeared in the candidate list, and the
   filter gave no clue as to why. */
static BOOL CALLBACK dump_all_proc(HWND hwnd, LPARAM lp)
{
    int *counts = (int *)lp;

    TFilterReason r = t_classify_window(hwnd);
    counts[r]++;

    if (r == TFILTER_OK) return TRUE;   /* already in the normal listing */

    DWORD pid = t_window_pid(hwnd);
    const wchar_t *proc = t_process_name(pid);

    /* Skip the shell's own invisible plumbing unless it is asked for. */
    if (is_shell_noise(proc) && !g_show_shell) {
        counts[TFILTER_CLOAKED + 1]++;   /* extra slot: "hidden shell noise" */
        return TRUE;
    }

    RECT wr = {0,0,0,0};
    GetWindowRect(hwnd, &wr);

    wchar_t cls[128];
    cls[0] = L'\0';
    GetClassNameW(hwnd, cls, 128);

    LONG_PTR ex  = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    LONG_PTR st  = GetWindowLongPtrW(hwnd, GWL_STYLE);

    t_out_line(L"");
    t_out_line(L"  hwnd 0x%p  pid %lu  %ls", (void *)hwnd,
               (unsigned long)pid, proc);
    t_out_line(L"    reason ...... %ls", t_filter_reason_name(r));
    t_out_line(L"    rect ........ %ld,%ld %ldx%ld",
               wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top);
    t_out_line(L"    title ....... \"%ls\"", t_window_title(hwnd));
    t_out_line(L"    class ....... \"%ls\"", cls);
    t_out_line(L"    style ....... 0x%08lX  exstyle 0x%08lX",
               (unsigned long)st, (unsigned long)ex);
    t_out_line(L"    visible ..... %ls   owner ....... 0x%p",
               IsWindowVisible(hwnd) ? L"yes" : L"no",
               (void *)GetWindow(hwnd, GW_OWNER));

    return TRUE;
}

/*
 * Ask the window for its minimum accepted size by shrinking it until it
 * stops complying. Delegating this to the window itself is what makes the
 * approach robust across arbitrary applications.
 *
 * Returns 1 when the measurement is meaningful and 0 when it is not.
 *
 * The distinction matters: on a window that denies access, every
 * SetWindowPos fails, the binary search never converges, and the function
 * happily returns the window's current size as its "minimum". That reads
 * as a real measurement but is not one - measured on a UU accelerator
 * window, it produced "min_accepted_size = 1500x1032", which was simply
 * the size the window already had.
 */
static int measure_min_size(HWND hwnd, LONG *out_w, LONG *out_h)
{
    RECT r;
    if (!GetWindowRect(hwnd, &r)) { *out_w = 0; *out_h = 0; return 0; }

    LONG lo_w = 40, lo_h = 40;
    LONG hi_w = r.right - r.left;
    LONG hi_h = r.bottom - r.top;

    /*
     * Whether the window ever actually CHANGED size.
     *
     * The first version decided "was this request accepted?" purely by
     * comparing the read-back size against the requested one. That test is
     * vacuous whenever the window is already bigger than the request: the
     * window ignores the call, keeps its larger size, and `aw <= mid_w`
     * cannot hold - except it can, because the comparison is against the
     * REQUEST, not against the window's previous size.
     *
     * Measured on a UU accelerator window (1500x1032 fixed): the search
     * asked for 770x516, the window stayed 1500x1032, and the function
     * reported "min_accepted_size = 1500x1032" as if that were a result.
     * It was just the window's own size.
     *
     * The honest test is whether the size moved at all between iterations.
     */
    LONG prev_w = r.right - r.left;
    LONG prev_h = r.bottom - r.top;
    int  ever_changed = 0;

    /* Binary search: 8 iterations is enough to land within a few pixels. */
    for (int i = 0; i < 8; ++i) {
        LONG mid_w = (lo_w + hi_w) / 2;
        LONG mid_h = (lo_h + hi_h) / 2;

        SetLastError(0);
        BOOL ok = SetWindowPos(hwnd, NULL, 0, 0, mid_w, mid_h,
                               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        DWORD err = ok ? 0 : GetLastError();

        if (!ok && err == ERROR_ACCESS_DENIED) {
            /* Every attempt will fail the same way; stop immediately. */
            *out_w = prev_w;
            *out_h = prev_h;
            return 0;
        }

        RECT got;
        GetWindowRect(hwnd, &got);
        LONG aw = got.right - got.left;
        LONG ah = got.bottom - got.top;

        if (aw != prev_w || ah != prev_h) ever_changed = 1;

        if (aw <= mid_w + g_tolerance && ah <= mid_h + g_tolerance) {
            hi_w = aw; hi_h = ah;   /* accepted: try smaller */
        } else {
            lo_w = mid_w; lo_h = mid_h; /* refused: try bigger */
        }

        prev_w = aw;
        prev_h = ah;
    }

    *out_w = hi_w;
    *out_h = hi_h;

    /* No size was ever accepted: the reported value is not a measurement. */
    return ever_changed;
}

/*
 * MOVE-ONLY test.
 *
 * The real goal for "windows that always open centred and remember nothing"
 * is to reposition them - not to resize them. Those are separate
 * capabilities: a window can refuse one and allow the other, and the
 * earlier results show this is exactly what a UU accelerator window does
 * (MOVED_ONLY: position accepted, size refused).
 *
 * Resizing is also the part most likely to be protected: it visibly
 * restructures a window's content. Moving is far more often permitted.
 *
 * So this test asks the question that actually matters for the product:
 * with SWP_NOSIZE, can the window be repositioned even when resizing is
 * denied? If yes, the product can do its job under asInvoker and the whole
 * "do I need admin rights" question disappears for most targets.
 */
static void run_move_only(HWND hwnd)
{
    RECT orig;
    if (!GetWindowRect(hwnd, &orig)) {
        T_WARN(L"GetWindowRect failed; cannot run the move-only test.");
        return;
    }

    LONG w = orig.right - orig.left;
    LONG h = orig.bottom - orig.top;

    T_SEC(L"MOVE-ONLY: reposition without touching the size");
    T_NOTE(L"This is the capability the product actually needs for windows that");
    T_NOTE(L"always reopen centred. It uses SWP_NOSIZE, so the window's size is");
    T_NOTE(L"never requested - only its position.");
    T_NOTE(L"");
    T_KV(L"original_rect", L"%ld,%ld %ldx%ld", orig.left, orig.top, w, h);

    HMONITOR hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hm, &mi);

    LONG mw = mi.rcMonitor.right - mi.rcMonitor.left;

    /* Four positions: two corners and two offsets, all at the same size. */
    struct { const wchar_t *name; LONG x, y; } spots[] = {
        { L"top-left",      mi.rcMonitor.left + 20,      mi.rcMonitor.top + 20 },
        { L"top-right",     mi.rcMonitor.left + mw - w - 20, mi.rcMonitor.top + 20 },
        { L"centre",        mi.rcMonitor.left + (mw - w) / 2,
                            mi.rcMonitor.top + 40 },
        { L"bottom-left",   mi.rcMonitor.left + 20,
                            mi.rcMonitor.top + (mi.rcMonitor.bottom - mi.rcMonitor.top) - h - 20 },
    };

    int moved_ok = 0;
    int denied = 0;

    for (int i = 0; i < 4; ++i) {
        SetLastError(0);
        BOOL ok = SetWindowPos(hwnd, NULL, spots[i].x, spots[i].y, 0, 0,
                               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        DWORD err = ok ? 0 : GetLastError();

        RECT got;
        GetWindowRect(hwnd, &got);
        LONG dx = got.left - spots[i].x;
        LONG dy = got.top  - spots[i].y;
        LONG gw = got.right - got.left;
        LONG gh = got.bottom - got.top;

        int arrived = (labs(dx) <= g_tolerance && labs(dy) <= g_tolerance);
        int size_kept = (labs((gw - w)) <= g_tolerance && labs((gh - h)) <= g_tolerance);

        const wchar_t *verdict;
        if (!ok)                      verdict = L"DENIED  (call failed)";
        else if (arrived && size_kept) { verdict = L"MOVED   (position applied, size untouched)"; moved_ok++; }
        else if (arrived)              verdict = L"MOVED   (position applied, size also changed)";
        else                           verdict = L"IGNORED (call succeeded but it stayed put)";

        if (!ok) denied++;

        t_out_line(L"");
        t_out_line(L"  [%-12ls] -> %ld,%ld   want %ld,%ld",
                   spots[i].name, got.left, got.top, spots[i].x, spots[i].y);
        t_out_line(L"    delta ......... dx=%ld dy=%ld", dx, dy);
        t_out_line(L"    size after .... %ldx%ld  (was %ldx%ld, %ls)",
                   gw, gh, w, h, size_kept ? L"unchanged" : L"CHANGED");
        t_out_line(L"    verdict ....... %ls", verdict);
        if (!ok) {
            t_out_line(L"    error ......... %lu", (unsigned long)err);
        }
    }

    /* Put it back. */
    SetWindowPos(hwnd, NULL, orig.left, orig.top, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT back;
    GetWindowRect(hwnd, &back);

    T_SEC(L"MOVE-ONLY conclusion");
    T_KV(L"positions_applied", L"%d of 4", moved_ok);
    T_KV(L"calls_denied", L"%d of 4", denied);
    T_KV(L"restored", L"%ld,%ld (was %ld,%ld)  %ls",
         back.left, back.top, orig.left, orig.top,
         (back.left == orig.left && back.top == orig.top) ? L"exact" : L"differs");

    if (denied == 0 && moved_ok > 0) {
        T_KV(L"RESULT", L"PASS - the window CAN be repositioned");
        T_NOTE(L"");
        T_NOTE(L"This is the important answer: even if the window refuses to be");
        T_NOTE(L"resized, it accepts being MOVED. The product can therefore handle");
        T_NOTE(L"windows like this under normal permissions (asInvoker), with no");
        T_NOTE(L"administrator rights involved at all.");
    } else if (denied > 0) {
        T_KV(L"RESULT", L"DENIED - repositioning was refused too");
        T_NOTE(L"");
        T_NOTE(L"This is the case that would need higher privileges. Before");
        T_NOTE(L"concluding that, re-run this same test from an elevated console:");
        T_NOTE(L"if the denials disappear, it is a permission boundary (UIPI) and");
        T_NOTE(L"the product should tell the user so rather than request admin.");
    } else {
        T_KV(L"RESULT", L"INCONCLUSIVE - the call succeeded but nothing moved");
    }
}

/* One test case: request a size, read back, classify the outcome. */
static void run_case(HWND hwnd, const wchar_t *name, LONG x, LONG y, LONG w, LONG h)
{    RECT before;
    GetWindowRect(hwnd, &before);

    BOOL ok = SetWindowPos(hwnd, NULL, x, y, w, h,
                           SWP_NOZORDER | SWP_NOACTIVATE);
    DWORD err = ok ? 0 : GetLastError();

    RECT after;
    GetWindowRect(hwnd, &after);

    LONG got_x = after.left, got_y = after.top;
    LONG got_w = after.right - after.left;
    LONG got_h = after.bottom - after.top;

    LONG dx = labs(got_x - x);
    LONG dy = labs(got_y - y);
    LONG dw = labs(got_w - w);
    LONG dh = labs(got_h - h);

    /*
     * The verdict distinguishes three genuinely different situations that
     * the first version conflated. Measured on a UU accelerator window
     * (pid 1524): every case returned FALSE with GetLastError()=5
     * (ERROR_ACCESS_DENIED), and the old code still advised "set
     * lock_size=1 and only move" - which is exactly wrong. A window that
     * refuses to be touched at all is not a window whose size is fixed.
     */
    const wchar_t *verdict;
    const wchar_t *advice;

    if (!ok) {
        verdict = L"DENIED   (SetWindowPos failed)";
        if (err == ERROR_ACCESS_DENIED) {
            advice = L"the window refused to be moved AT ALL (access denied).\n"
                     L"                       Product must report PLACE_DENIED and tell the user.\n"
                     L"                       Do NOT set lock_size - that is for windows whose\n"
                     L"                       size is fixed, not for windows that reject control.";
        } else {
            advice = L"SetWindowPos failed for a reason other than access; see the error above.";
        }
    } else if (dw <= g_tolerance && dh <= g_tolerance &&
               dx <= g_tolerance && dy <= g_tolerance) {
        verdict = L"OK       (fully accepted)";
        advice  = NULL;
    } else if (dw > g_tolerance || dh > g_tolerance) {
        verdict = L"MOVED_ONLY (refused resize)";
        advice  = L"the call SUCCEEDED but the size did not change.\n"
                  L"                       Product must set lock_size=1 and only move it.";
    } else {
        verdict = L"CLAMPED  (position off only)";
        advice  = NULL;
    }

    t_out_line(L"");
    t_out_line(L"[CASE.%ls.requested] pos=%ld,%ld size=%ldx%ld", name, x, y, w, h);
    t_out_line(L"[CASE.%ls.got]       pos=%ld,%ld size=%ldx%ld", name, got_x, got_y, got_w, got_h);
    t_out_line(L"[CASE.%ls.delta]     dx=%ld dy=%ld dw=%ld dh=%ld", name, dx, dy, dw, dh);
    t_out_line(L"[CASE.%ls.verdict]   %ls", name, verdict);
    if (!ok) {
        t_out_line(L"[CASE.%ls.error]     SetWindowPos failed, GetLastError=%lu", name,
                   (unsigned long)err);
        {
            wchar_t msg[256];
            msg[0] = L'\0';
            FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                           msg, 256, NULL);
            for (wchar_t *p = msg; *p; ++p) { if (*p == L'\r' || *p == L'\n') { *p = L'\0'; break; } }
            t_out_line(L"[CASE.%ls.errtext]   %ls", name, msg);
        }
    }

    if (advice) {
        t_out_line(L"[CASE.%ls.note]      %ls", name, advice);
    }
}

/*
 * Search criteria for locate_proc. A struct rather than a pointer array:
 * passing a pid through a wchar_t* would rely on pointer-integer casts and
 * sign extension, which is a needless way to lose on a 64-bit build.
 */
typedef struct {
    const wchar_t *title;   /* substring, or NULL/empty to ignore */
    const wchar_t *app;     /* process-name substring, or NULL/empty */
    DWORD          pid;     /* 0 to ignore */
} FindSpec;

/*
 * Locate a window for testing.
 *
 * Matches on title, process name, and/or pid. Unlike the candidate listing
 * this does NOT apply the app-window filter, so a panel that the filter
 * rejects (tool palette, owned popup) can still be targeted deliberately -
 * which is the whole point of looking one up by pid.
 */
static BOOL CALLBACK find_proc(HWND hwnd, LPARAM lp)
{
    if (g_target) return FALSE;

    const FindSpec *spec = (const FindSpec *)lp;

    if (spec->pid) {
        if (t_window_pid(hwnd) == spec->pid) g_target = hwnd;
        return TRUE;
    }

    int title_hit = spec->title && *spec->title &&
                    t_contains_i(t_window_title(hwnd), spec->title);
    int app_hit   = spec->app && *spec->app &&
                    t_contains_i(t_process_name(t_window_pid(hwnd)), spec->app);

    if (title_hit || app_hit) g_target = hwnd;
    return TRUE;
}

typedef struct {
    HWND          hwnd;
    RECT          rect;
    TFilterReason reason;
    long          area;
} PidWin;

#define MAX_PID_WINS 64

typedef struct {
    DWORD  want_pid;
    PidWin wins[MAX_PID_WINS];
    int    count;
} PidDump;

static BOOL CALLBACK collect_pid_proc(HWND hwnd, LPARAM lp)
{
    PidDump *d = (PidDump *)lp;
    if (t_window_pid(hwnd) != d->want_pid) return TRUE;
    if (d->count >= MAX_PID_WINS) return FALSE;

    PidWin *w = &d->wins[d->count++];
    ZeroMemory(w, sizeof(*w));
    w->hwnd = hwnd;
    GetWindowRect(hwnd, &w->rect);
    w->reason = t_classify_window(hwnd);
    w->area = (long)(w->rect.right - w->rect.left) *
              (long)(w->rect.bottom - w->rect.top);
    if (w->area < 0) w->area = 0;
    return TRUE;
}

static int pidwin_cmp_desc(const void *a, const void *b)
{
    const PidWin *x = (const PidWin *)a;
    const PidWin *y = (const PidWin *)b;
    if (x->area < y->area) return 1;
    if (x->area > y->area) return -1;
    return 0;
}

static void print_pid_windows(DWORD want_pid)
{
    T_SEC(L"All top-level windows owned by pid %lu", (unsigned long)want_pid);

    static PidDump d;
    ZeroMemory(&d, sizeof(d));
    d.want_pid = want_pid;
    EnumWindows(collect_pid_proc, (LPARAM)&d);

    if (d.count == 0) {
        T_WARN(L"pid %lu owns no top-level windows.", (unsigned long)want_pid);
        T_WARN(L"Either the process has exited, or its UI lives in another");
        T_WARN(L"process. Check every process of that application - a single");
        T_WARN(L"app often runs many, and only one owns the visible window.");
        return;
    }

    qsort(d.wins, (size_t)d.count, sizeof(PidWin), pidwin_cmp_desc);

    t_out_line(L"  %d window(s), largest area first:", d.count);
    for (int i = 0; i < d.count; ++i) {
        PidWin *w = &d.wins[i];

        wchar_t cls[128];
        cls[0] = L'\0';
        GetClassNameW(w->hwnd, cls, 128);

        long rw = w->rect.right - w->rect.left;
        long rh = w->rect.bottom - w->rect.top;

        int off_screen = (w->rect.right  <= -30000 || w->rect.bottom <= -30000 ||
                          w->rect.left   >=  30000 || w->rect.top    >=  30000);

        t_out_line(L"");
        t_out_line(L"  [%d] hwnd 0x%p   %ldx%ld   visible %ls   %ls",
                   i + 1, (void *)w->hwnd, rw, rh,
                   IsWindowVisible(w->hwnd) ? L"yes" : L"NO",
                   (w->reason == TFILTER_OK) ? L"<- in candidate list"
                                             : L"<- filtered out");
        t_out_line(L"      class ....... \"%ls\"", cls);
        t_out_line(L"      title ....... \"%ls\"", t_window_title(w->hwnd));
        t_out_line(L"      rect ........ %ld,%ld %ldx%ld",
                   w->rect.left, w->rect.top, rw, rh);
        t_out_line(L"      on screen ... %ls",
                   off_screen ? L"NO - parked far off-screen" : L"yes");
        t_out_line(L"      filter ...... %ls", t_filter_reason_name(w->reason));
    }

    t_out_line(L"");
    t_out_line(L"  The window at the top is the largest one this process owns.");
    t_out_line(L"  If the window you can SEE is not listed here at all, it belongs");
    t_out_line(L"  to a DIFFERENT process of the same application - try other pids.");
    t_out_line(L"  Run: t02_move.exe --pid <pid>        to test that process.");
}

/*
 * Scan every process whose image name contains `app_substr`, and report the
 * windows each one owns.
 *
 * This is the answer to "I can see the window but no pid in Task Manager
 * obviously owns it": a single application routinely runs a dozen
 * processes, and only one of them owns the visible window. Probing pids
 * one at a time is tedious and error-prone, so do it in one pass.
 */
typedef struct {
    const wchar_t *app_substr;
    DWORD          pids[64];
    int            pid_count;
} AppScan;

static void app_scan_add(AppScan *s, DWORD pid)
{
    if (!pid) return;
    for (int i = 0; i < s->pid_count; ++i) {
        if (s->pids[i] == pid) return;
    }
    if (s->pid_count < 64) s->pids[s->pid_count++] = pid;
}

static BOOL CALLBACK app_scan_proc(HWND hwnd, LPARAM lp)
{
    AppScan *s = (AppScan *)lp;
    DWORD pid = t_window_pid(hwnd);
    if (!pid) return TRUE;
    if (!t_contains_i(t_process_name(pid), s->app_substr)) return TRUE;
    app_scan_add(s, pid);
    return TRUE;
}

/* Walk the process list so processes with no windows still get reported. */
static void app_scan_all_processes(AppScan *s)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(snap, &pe)) {
        do {
            if (t_contains_i(pe.szExeFile, s->app_substr)) app_scan_add(s, pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

static void scan_app_processes(const wchar_t *app_substr)
{
    static AppScan scan;
    ZeroMemory(&scan, sizeof(scan));
    scan.app_substr = app_substr;

    app_scan_all_processes(&scan);

    if (scan.pid_count == 0) {
        T_WARN(L"no running process matches \"%ls\".", app_substr);
        return;
    }

    t_out_line(L"  %d process(es) match. Each one's windows follow.", scan.pid_count);

    int total_windows = 0;
    for (int i = 0; i < scan.pid_count; ++i) {
        static PidDump d;
        ZeroMemory(&d, sizeof(d));
        d.want_pid = scan.pids[i];
        EnumWindows(collect_pid_proc, (LPARAM)&d);

        t_out_line(L"");
        t_out_line(L"  === pid %lu : %d top-level window(s) ===",
                   (unsigned long)scan.pids[i], d.count);
        total_windows += d.count;

        if (d.count == 0) {
            t_out_line(L"      (none - this process has no UI of its own)");
            continue;
        }

        qsort(d.wins, (size_t)d.count, sizeof(PidWin), pidwin_cmp_desc);
        for (int k = 0; k < d.count; ++k) {
            PidWin *w = &d.wins[k];
            wchar_t cls[128];
            cls[0] = L'\0';
            GetClassNameW(w->hwnd, cls, 128);

            long rw = w->rect.right - w->rect.left;
            long rh = w->rect.bottom - w->rect.top;
            int off_screen = (w->rect.right  <= -30000 || w->rect.bottom <= -30000 ||
                              w->rect.left   >=  30000 || w->rect.top    >=  30000);

            t_out_line(L"      [%d] %ldx%ld  visible %ls  %ls  class \"%ls\"",
                       k + 1, rw, rh,
                       IsWindowVisible(w->hwnd) ? L"yes" : L"NO",
                       off_screen ? L"OFF-SCREEN" : L"on-screen",
                       cls);
            t_out_line(L"          hwnd 0x%p  filter: %ls",
                       (void *)w->hwnd, t_filter_reason_name(w->reason));
        }
    }

    t_out_line(L"");
    t_out_line(L"  %d window(s) across %d process(es).", total_windows, scan.pid_count);
    t_out_line(L"  To test one:  t02_move.exe --pid <pid>");
    t_out_line(L"  Pick the pid whose window is the one you can see on screen.");
}


int wmain(int argc, wchar_t **argv)
{
    /* Crash filter + file log first: a console that flashes and closes
       takes its output with it. See TRAPS.md. */
    t_diag_init(L"t02_move");
    t_log_mirror(1);   /* full report goes to the log by default */

    t_console_init();

    const wchar_t *want_title = L"";
    const wchar_t *want_app   = L"";
    const wchar_t *want_scan_app = L"";
    int want_index = -1;
    DWORD want_pid = 0;
    int list_pid_only = 0;

    for (int i = 1; i < argc; ++i) {
        if      (!wcscmp(argv[i], L"--title") && i + 1 < argc) want_title = argv[++i];
        else if (!wcscmp(argv[i], L"--app")   && i + 1 < argc) want_app   = argv[++i];
        else if (!wcscmp(argv[i], L"--scan-app") && i + 1 < argc) want_scan_app = argv[++i];
        else if (!wcscmp(argv[i], L"--index") && i + 1 < argc) want_index = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--pid")   && i + 1 < argc) want_pid   = (DWORD)_wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--list-pid")) list_pid_only = 1;
        else if (!wcscmp(argv[i], L"--tolerance") && i + 1 < argc) g_tolerance = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--size-only")) g_size_only = 1;
        else if (!wcscmp(argv[i], L"--all")) g_show_all = 1;
        else if (!wcscmp(argv[i], L"--shell")) g_show_shell = 1;
        else if (!wcscmp(argv[i], L"--no-pause")) g_no_pause = 1;
        else if (!wcscmp(argv[i], L"--move-only")) g_move_only = 1;
        else if (!wcscmp(argv[i], L"--open-log")) g_open_log = 1;
        else if (!wcscmp(argv[i], L"--help")) {
            t_out_line(L"usage: t02_move.exe [--title T | --app A | --index N | --pid PID]");
            t_out_line(L"                    [--scan-app NAME] [--list-pid] [--tolerance PX]");
            t_out_line(L"                    [--size-only | --move-only] [--all] [--shell]");
            t_out_line(L"                    [--no-pause] [--open-log]");
            t_out_line(L"");
            t_out_line(L"  --move-only");
            t_out_line(L"              test only REPOSITIONING (SWP_NOSIZE), never resizing.");
            t_out_line(L"              This is what the product needs for windows that always");
            t_out_line(L"              reopen centred: move them, leave their size alone.");
            t_out_line(L"              Use it to find out whether a window that refuses");
            t_out_line(L"              resizing can still be moved under normal permissions.");
            t_out_line(L"  --scan-app NAME");
            t_out_line(L"              list every running process whose image name contains");
            t_out_line(L"              NAME, and the windows each one owns. Use this when");
            t_out_line(L"              you can see a window but do not know which pid owns");
            t_out_line(L"              it - one application often runs many processes.");
            t_out_line(L"  --pid N     test the window owned by process id N. Ignores the");
            t_out_line(L"              app-window filter, so rejected panels can be tested.");
            t_out_line(L"  --list-pid  with --pid, only list that process's windows and stop");
            t_out_line(L"  --all       also list top-level windows that were REJECTED as");
            t_out_line(L"              candidates, with the reason for each rejection");
            t_out_line(L"  --shell     with --all, include explorer/shell plumbing windows");
            t_out_line(L"  --no-pause  exit immediately instead of waiting for a key");
            return 0;
        }
    }

    T_TITLE(L"t02_move - window resize capability and read-back tolerance");
    T_KV(L"run_at", L"%ls", t_now_string());

    int v2 = t_set_dpi_awareness();
    T_KV(L"dpi_awareness_v2", L"%ls", v2 ? L"ok" : L"fallback (coordinates may be virtualized)");
    T_KV(L"tolerance_px", L"%d  (candidate threshold; refine after this run)", g_tolerance);

    /* ---------------- pick a target ---------------- */
    TWindowList list;
    int n = t_enum_app_windows(&list, MAX_WINDOWS);
    T_KV(L"app_window_count", L"%d", n);

    if (want_index >= 0) {
        if (want_index < n) g_target = list.items[want_index].hwnd;
        else T_WARN(L"--index %d out of range (0..%d)", want_index, n - 1);
    } else if (*want_title || *want_app) {
        FindSpec spec = { want_title, want_app, 0 };
        EnumWindows(find_proc, (LPARAM)&spec);
        if (!g_target) {
            T_WARN(L"no window matched. Use --title / --app with a substring,");
            T_WARN(L"or run without arguments to list candidates.");
        }
    } else if (want_pid) {
        /* --list-pid: just enumerate that process's windows and stop. */
        if (list_pid_only) {
            print_pid_windows(want_pid);
            free(list.items);
            if (!g_no_pause) t_pause_if_console();
            t_log_close();
    if (g_open_log) t_open_log_in_notepad();
            return 0;
        }

        FindSpec spec = { L"", L"", want_pid };
        EnumWindows(find_proc, (LPARAM)&spec);

        if (!g_target) {
            T_WARN(L"pid %lu owns no top-level window.", (unsigned long)want_pid);
            T_WARN(L"Run with --list-pid to see what that process actually owns.");
        } else {
            TFilterReason fr = t_classify_window(g_target);
            T_KV(L"pid_target_filter", L"%ls", t_filter_reason_name(fr));
            if (fr != TFILTER_OK) {
                T_NOTE(L"This window is NOT in the candidate list, but --pid targets it");
                T_NOTE(L"directly. That is intentional: the filter is a heuristic for");
                T_NOTE(L"listing, not a statement about whether the window can move.");
            }
        }
    } else if (*want_scan_app) {
        /* Scan every process matching the name and report its windows.
           Answers "which of this app's processes owns the window I can
           see?" in one pass instead of guessing pids one at a time. */
        T_SEC(L"Scanning every process whose name contains \"%ls\"", want_scan_app);
        scan_app_processes(want_scan_app);
        free(list.items);
        if (!g_no_pause) t_pause_if_console();
        t_log_close();
    if (g_open_log) t_open_log_in_notepad();
        return 0;
    }

    /* ---------------- optional: every rejected window ---------------- */
    if (g_show_all) {
        T_SEC(L"ALL top-level windows, including rejected ones");
        t_out_line(L"  Looking for anything you can see on screen but that the");
        t_out_line(L"  candidate list did not include.");

        int counts[TFILTER_CLOAKED + 2];
        ZeroMemory(counts, sizeof(counts));
        EnumWindows(dump_all_proc, (LPARAM)counts);

        t_out_line(L"");
        t_out_line(L"  -- rejection summary --");
        for (int r = 0; r <= TFILTER_CLOAKED; ++r) {
            if (counts[r] == 0) continue;
            t_out_line(L"    %-46ls %d", t_filter_reason_name((TFilterReason)r), counts[r]);
        }
        if (counts[TFILTER_CLOAKED + 1] > 0) {
            t_out_line(L"    %-46ls %d", L"explorer/shell plumbing (hidden here)", counts[TFILTER_CLOAKED + 1]);
            t_out_line(L"      re-run with --all --shell to see them");
        }
        t_out_line(L"");
        t_out_line(L"  A window is not necessarily broken just because it is listed");
        t_out_line(L"  as rejected: tool windows and dialogs are excluded on purpose.");
        t_out_line(L"  What matters is whether the exclusion reason makes sense for");
        t_out_line(L"  the window you were looking for.");
    }

    if (!g_target) {
        print_window_list(&list);
        t_out_line(L"");
        t_out_line(L"[NEXT] Re-run with one of:");
        t_out_line(L"       t02_move.exe --index <N>");
        t_out_line(L"       t02_move.exe --title <substring>");
        t_out_line(L"       t02_move.exe --app <exe name>");
        free(list.items);
        if (!g_no_pause) t_pause_if_console();
        t_log_close();
    if (g_open_log) t_open_log_in_notepad();
        return 0;
    }

    /* ---------------- identify the target ---------------- */
    GetWindowRect(g_target, &g_original);

    T_SEC(L"Target");
    T_KV(L"hwnd", L"0x%p", (void *)g_target);
    T_KV(L"pid", L"%lu", (unsigned long)t_window_pid(g_target));
    T_KV(L"process", L"%ls", t_process_name(t_window_pid(g_target)));
    T_KV(L"title", L"%ls", t_window_title(g_target));
    T_KV(L"original_rect", L"%ld,%ld %ldx%ld",
         g_original.left, g_original.top,
         g_original.right - g_original.left, g_original.bottom - g_original.top);

    LONG_PTR style = GetWindowLongPtrW(g_target, GWL_STYLE);
    T_KV(L"style_WS_THICKFRAME", L"%ls",
         (style & WS_THICKFRAME) ? L"present (claims to be resizable)" : L"absent (fixed size)");
    T_KV(L"style_WS_CAPTION", L"%ls", (style & WS_CAPTION) ? L"present" : L"absent");
    T_KV(L"is_maximized", L"%ls", IsZoomed(g_target) ? L"YES - product must SW_RESTORE first" : L"no");
    T_KV(L"is_minimized", L"%ls", IsIconic(g_target) ? L"YES - SetWindowPos will be ignored" : L"no");

    HMONITOR hm = MonitorFromWindow(g_target, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hm, &mi);
    T_KV(L"monitor_rcMonitor", L"%ld,%ld %ldx%ld",
         mi.rcMonitor.left, mi.rcMonitor.top,
         mi.rcMonitor.right - mi.rcMonitor.left,
         mi.rcMonitor.bottom - mi.rcMonitor.top);
    T_KV(L"monitor_rcWork", L"%ld,%ld %ldx%ld",
         mi.rcWork.left, mi.rcWork.top,
         mi.rcWork.right - mi.rcWork.left,
         mi.rcWork.bottom - mi.rcWork.top);

    /* ---------------- focus check ---------------- */
    T_SEC(L"Focus safety check");
    T_NOTE(L"SetWindowPos is called with SWP_NOACTIVATE throughout. The");
    T_NOTE(L"foreground window must not change because of this program.");
    t_report_foreground(L"foreground_before");

    /* ---------------- maximise / minimise handling ---------------- */
    /*
     * A maximised window ignores SetWindowPos entirely for on-screen
     * purposes: the call only rewrites the restore geometry, while
     * GetWindowRect keeps reporting the maximised rectangle (-11,-11
     * 2582x1622 on a 2560x1600 screen, i.e. the screen plus the invisible
     * DWM border). Testing such a window measures nothing and looks
     * exactly like "this window refuses to resize".
     *
     * The first version of this program did not handle that, and every
     * maximised window came out as MOVED_ONLY. Restore first, test, then
     * put the window back the way we found it.
     */
    int was_maximized = IsZoomed(g_target) ? 1 : 0;
    int was_minimized = IsIconic(g_target) ? 1 : 0;

    T_SEC(L"Window state before testing");
    T_KV(L"is_maximized", L"%ls", was_maximized ? L"YES" : L"no");
    T_KV(L"is_minimized", L"%ls", was_minimized ? L"YES" : L"no");

    if (was_maximized || was_minimized) {
        T_NOTE(L"Restoring it first. A maximised window ignores SetWindowPos for");
        T_NOTE(L"on-screen purposes, so testing it as-is measures nothing - it");
        T_NOTE(L"merely looks like the window refuses to resize.");
        ShowWindow(g_target, SW_RESTORE);
        Sleep(250);   /* let DWM finish the animation before measuring */
        GetWindowRect(g_target, &g_original);
        T_KV(L"rect_after_restore", L"%ld,%ld %ldx%ld",
             g_original.left, g_original.top,
             g_original.right - g_original.left, g_original.bottom - g_original.top);
    } else {
        T_NOTE(L"Already in the normal state - results will be meaningful.");
    }

    /* ---------------- size acceptance ---------------- */
    if (g_move_only) {
        run_move_only(g_target);
        free(list.items);

        T_SEC(L"RECORD THESE VALUES");
        T_NOTE(L"Report file: docs/P0-verification.md, experiment 3 (move-only part)");
        T_NOTE(L"");
        T_NOTE(L"This run answers the question the product actually depends on:");
        T_NOTE(L"can a window that refuses resizing still be MOVED?");
        T_NOTE(L"If yes, no administrator rights are needed for it.");
        if (!g_no_pause) t_pause_if_console();
        t_log_close();
    if (g_open_log) t_open_log_in_notepad();
        return 0;
    }

    T_SEC(L"Resize acceptance");

    LONG mw = mi.rcMonitor.right - mi.rcMonitor.left;
    LONG mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    LONG cx = mi.rcMonitor.left + mw / 4;
    LONG cy = mi.rcMonitor.top  + mh / 4;

    if (g_size_only) {
        run_case(g_target, L"h1", g_original.left, g_original.top, mw / 2, mh);
        run_case(g_target, L"h2", g_original.left, g_original.top, mw / 2 + 7, mh - 13);
    } else {
        run_case(g_target, L"full",  mi.rcMonitor.left, mi.rcMonitor.top, mw, mh);
        run_case(g_target, L"left",  mi.rcMonitor.left, mi.rcMonitor.top, mw / 2, mh);
        run_case(g_target, L"right", mi.rcMonitor.left + mw / 2, mi.rcMonitor.top, mw / 2, mh);
        run_case(g_target, L"quad",  cx, cy, mw / 2, mh / 2);
        run_case(g_target, L"odd",   mi.rcMonitor.left + 3, mi.rcMonitor.top + 5,
                                    mw / 3 + 11, mh / 3 + 7);
    }

    /* ---------------- minimum size ---------------- */
    T_SEC(L"Minimum accepted size (drives RelRect.min_w / min_h)");
    LONG min_w = 0, min_h = 0;
    int min_ok = measure_min_size(g_target, &min_w, &min_h);
    if (min_ok) {
        T_KV(L"min_accepted_size", L"%ldx%ld", min_w, min_h);
        T_NOTE(L"The product should store roughly these values as min_w / min_h,");
        T_NOTE(L"then clamp target sizes to at least this, or the window will");
        T_NOTE(L"silently keep a larger size than requested.");
    } else {
        T_KV(L"min_accepted_size", L"%ldx%ld  *** NOT A MEASUREMENT ***", min_w, min_h);
        T_WARN(L"No size was ever accepted, so this is just the window's current");
        T_WARN(L"size, not its minimum. Do NOT record it as min_w / min_h.");
        T_WARN(L"The earlier SetWindowPos denials explain why: this window cannot");
        T_WARN(L"be resized by this program at all.");
    }

    /* ---------------- restore ---------------- */
    T_SEC(L"Restore");
    SetWindowPos(g_target, NULL,
                 g_original.left, g_original.top,
                 g_original.right - g_original.left,
                 g_original.bottom - g_original.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    RECT restored;
    GetWindowRect(g_target, &restored);
    T_KV(L"restored_rect", L"%ld,%ld %ldx%ld",
         restored.left, restored.top,
         restored.right - restored.left, restored.bottom - restored.top);
    T_KV(L"restore_exact", L"%ls",
         (restored.left == g_original.left && restored.top == g_original.top &&
          restored.right == g_original.right && restored.bottom == g_original.bottom)
         ? L"yes" : L"NO - window refused to return to its original geometry");

    t_report_foreground(L"foreground_after");
    T_KV(L"focus_unchanged", L"%ls", L"compare the two foreground lines by eye");

    /* ---------------- put the window back as we found it ------------- */
    if (was_maximized) {
        ShowWindow(g_target, SW_MAXIMIZE);
        T_KV(L"state_restored", L"re-maximised");
    } else if (was_minimized) {
        ShowWindow(g_target, SW_MINIMIZE);
        T_KV(L"state_restored", L"re-minimised");
    } else {
        T_KV(L"state_restored", L"left in normal state");
    }

    free(list.items);

    /* ---------------- summary ---------------- */
    T_SEC(L"RECORD THESE VALUES");
    T_NOTE(L"Report file: docs/P0-verification.md, experiment 3");
    T_NOTE(L"");
    T_NOTE(L"1. max dw/dh among cases whose verdict was OK  -> tolerance_px");
    T_NOTE(L"2. which verdict this application produced      -> MOVED_ONLY means");
    T_NOTE(L"   the product must set lock_size=1 and only move it");
    T_NOTE(L"3. min_accepted_size -> becomes RelRect.min_w / min_h");
    T_NOTE(L"4. restore_exact must be yes");
    T_NOTE(L"");
    if (was_maximized) {
        T_NOTE(L"NOTE: this window was maximised when the test started, so it was");
        T_NOTE(L"restored first. That is why the numbers above are meaningful.");
    }
    T_NOTE(L"Remember: one run tests ONE application. Repeat for each app.");

    if (!g_no_pause) t_pause_if_console();
    t_log_close();
    if (g_open_log) t_open_log_in_notepad();
    return 0;
}
