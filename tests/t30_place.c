/*
 * t30_place.c - window placement: find, place, verify.
 *
 * This is the stage-4 workhorse. It is a console program on purpose: the
 * placement path has to be run dozens of times while it is being tuned, and
 * a GUI would add a build, a launch and a click to every iteration.
 *
 * WHAT IT CAN DO
 *
 *   --list                 every top-level window, with the reason each
 *                          rejected one was rejected
 *   --probe                what a candidate window will and will not accept,
 *                          without touching it
 *   --place                move (and optionally resize) a window that is
 *                          already open
 *   --launch               start a program, wait for its REAL window to
 *                          appear, then place that
 *
 * The --launch path is the one the product exists for: "some windows have no
 * position memory and always open dead centre". Everything about the wait
 * loop below - ignoring splash screens, waiting for the geometry to settle,
 * falling back to the largest window - exists because launching the first
 * window that shows up puts the wrong window in the right place.
 *
 * WHY --list SHOWS REJECTED WINDOWS
 *
 *   A filter that silently drops windows cannot be debugged. When a user can
 *   see a window on screen and the tool says it is not there, the only useful
 *   answer is "here is that window, and here is the rule that rejected it".
 *   So the listing never hides anything, and --all is not needed to see the
 *   noise.
 *
 * EXAMPLES
 *
 *   t30_place.exe --list
 *   t30_place.exe --launch notepad.exe --zone 0,0,0.5,1
 *   t30_place.exe --app notepad.exe --zone 0.5,0,0.5,1 --move-only
 *   t30_place.exe --pid 8124 --zone 0,0,1,1 --monitor 0
 *   t30_place.exe --probe --app Code.exe
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t30_place
 *   tests\out\t30_place.exe --help
 */

#include "test_common.h"

#include <stdlib.h>
#include <wchar.h>

#include "../src/core/app_id.h"
#include "../src/core/monitor.h"
#include "../src/core/relcoord.h"
#include "../src/core/win_enum.h"
#include "../src/core/win_place.h"
#include "../src/model/config.h"
#include "../src/model/rule.h"
#include "../src/model/store.h"

/* ------------------------------------------------------------------ */
/* Static storage                                                      */
/*                                                                     */
/* A WindowInfo is about 1.3 KB, so 128 of them is ~170 KB. That does   */
/* not belong on a 1 MB stack - the project already lost a day to a     */
/* 26 MB struct on the stack (TRAPS.md #11).                 */
/* ------------------------------------------------------------------ */

static WindowInfo g_all[WAC_WIN_MAX];
static WindowInfo g_hits[WAC_WIN_MAX];
static MonitorFingerprint g_mons[WAC_MON_MAX];
static int g_mon_count;

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    int   do_list;
    int   do_list_all;
    int   do_probe;
    int   do_place;
    int   do_launch;
    int   do_rules;        /* list the stored rules */
    int   do_apply;        /* apply every matching rule to every open window */
    int   do_remember;     /* save the placement as a rule */
    wchar_t forget_name[RULE_LABEL_MAX];
    int   move_only;
    int   no_pause;
    int   log_all;
    int   wait_ms;

    int   monitor_index;      /* -1 = follow the window */
    int   have_zone;
    float zx, zy, zw, zh;

    AppId sel;
    wchar_t launch_cmd[512];
    wchar_t launch_exe[WAC_EXE_MAX];   /* file name derived from launch_cmd */
} Options;

static void usage(void)
{
    t_out_line(L"t30_place - window placement tool");
    t_out_line(L"");
    t_out_line(L"  --list                     list the windows placement would use");
    t_out_line(L"  --list-all                 list every top-level window, with the reason");
    t_out_line(L"                             each rejected one was rejected");
    t_out_line(L"  --probe                    report what a window accepts, change nothing");
    t_out_line(L"  --place                    place an already-open window (default with a selector)");
    t_out_line(L"  --launch \"<command>\"       start a program, wait for its window, place it");
    t_out_line(L"");
    t_out_line(L"  --app <name|path>          select by executable");
    t_out_line(L"  --pid <n>                  select by process id");
    t_out_line(L"  --hwnd <0x...>             select by window handle");
    t_out_line(L"  --class <name>             add a window class requirement");
    t_out_line(L"  --title <substring>        add a caption substring requirement");
    t_out_line(L"");
    t_out_line(L"  --zone x,y,w,h             normalized target zone (default 0,0,0.5,1)");
    t_out_line(L"  --monitor <n>              target monitor index (default: the window's own)");
    t_out_line(L"  --move-only                never change the size");
    t_out_line(L"  --wait <ms>                window wait budget for --launch (default 15000)");
    t_out_line(L"");
    t_out_line(L"  --rules                    list the stored placement rules");
    t_out_line(L"  --forget <name>            delete a rule (by name, exe or id)");
    t_out_line(L"  --apply                    apply every matching rule to every open window");
    t_out_line(L"  --remember                 save this placement as a rule");
    t_out_line(L"");
    t_out_line(L"  With --remember and no --zone, the window's CURRENT position and size");
    t_out_line(L"  are recorded, so it will come back exactly as you left it.");
    t_out_line(L"  With --zone, the rule is a share of the screen instead: 0,0,0.5,1 is");
    t_out_line(L"  always the left half, on any monitor.");
    t_out_line(L"");
    t_out_line(L"  --no-pause                 exit immediately");
    t_out_line(L"  --log-all                  mirror the report into t30_place.log");
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

static int parse_floats(const wchar_t *s, float *a, float *b, float *c, float *d)
{
    return (swscanf_s(s, L"%f,%f,%f,%f", a, b, c, d) == 4) ? 1 : 0;
}

static int parse_args(int argc, wchar_t **argv, Options *o)
{
    ZeroMemory(o, sizeof(*o));
    o->monitor_index = -1;
    o->wait_ms       = 15000;

    for (int i = 1; i < argc; ++i) {
        const wchar_t *a = argv[i];

        if (_wcsicmp(a, L"--list") == 0)        o->do_list = 1;
        else if (_wcsicmp(a, L"--rules") == 0)  o->do_rules = 1;
        else if (_wcsicmp(a, L"--apply") == 0)  o->do_apply = 1;
        else if (_wcsicmp(a, L"--remember") == 0) o->do_remember = 1;
        else if (_wcsicmp(a, L"--forget") == 0 && i + 1 < argc) {
            wcsncpy_s(o->forget_name, RULE_LABEL_MAX, argv[++i], _TRUNCATE);
        }
        else if (_wcsicmp(a, L"--list-all") == 0) { o->do_list = 1; o->do_list_all = 1; }
        else if (_wcsicmp(a, L"--probe") == 0)  o->do_probe = 1;
        else if (_wcsicmp(a, L"--place") == 0)  o->do_place = 1;
        else if (_wcsicmp(a, L"--move-only") == 0) o->move_only = 1;
        else if (_wcsicmp(a, L"--no-pause") == 0)  o->no_pause = 1;
        else if (_wcsicmp(a, L"--log-all") == 0)   o->log_all = 1;
        else if (_wcsicmp(a, L"--help") == 0 || _wcsicmp(a, L"-h") == 0) { usage(); return 1; }

        else if (_wcsicmp(a, L"--launch") == 0 && i + 1 < argc) {
            wcsncpy_s(o->launch_cmd, 512, argv[++i], _TRUNCATE);
            o->do_launch = 1;
        }
        else if (_wcsicmp(a, L"--app") == 0 && i + 1 < argc) {
            app_id_set_exe(&o->sel, argv[++i]);
        }
        else if (_wcsicmp(a, L"--pid") == 0 && i + 1 < argc) {
            o->sel.pid = (DWORD)wcstoul(argv[++i], NULL, 10);
        }
        else if (_wcsicmp(a, L"--hwnd") == 0 && i + 1 < argc) {
            o->sel.hwnd = (HWND)(ULONG_PTR)_wcstoui64(argv[++i], NULL, 0);
        }
        else if (_wcsicmp(a, L"--class") == 0 && i + 1 < argc) {
            wcsncpy_s(o->sel.class_name, WAC_CLASS_MAX, argv[++i], _TRUNCATE);
        }
        else if (_wcsicmp(a, L"--title") == 0 && i + 1 < argc) {
            wcsncpy_s(o->sel.title_sub, WAC_TITLE_MAX, argv[++i], _TRUNCATE);
        }
        else if (_wcsicmp(a, L"--zone") == 0 && i + 1 < argc) {
            if (!parse_floats(argv[++i], &o->zx, &o->zy, &o->zw, &o->zh)) {
                t_out_line(L"[error] --zone wants x,y,w,h in normalized units");
                return -1;
            }
            o->have_zone = 1;
        }
        else if (_wcsicmp(a, L"--monitor") == 0 && i + 1 < argc) {
            o->monitor_index = (int)wcstol(argv[++i], NULL, 10);
        }
        else if (_wcsicmp(a, L"--wait") == 0 && i + 1 < argc) {
            o->wait_ms = (int)wcstol(argv[++i], NULL, 10);
            if (o->wait_ms < 100) o->wait_ms = 100;
        }
        else {
            t_out_line(L"[warn] unknown argument ignored: %ls", a);
        }
    }

    /*
     * --launch deliberately does NOT invent a default zone.
     *
     * It used to: with no --zone it set 0,0,0.5,1 so a bare
     * "--launch notepad.exe" did something visible. The moment stored rules
     * existed, that default SILENTLY SHADOWED them - have_zone was set, so
     * the rule lookup was never reached at all.
     *
     * And because the invented default happened to equal the rule that had
     * been stored, the end-to-end run printed exactly the right rectangle
     * while the rule was ignored from start to finish. A wrong answer that
     * coincides with the right one is the hardest kind to notice; the run
     * only proved that TWO different code paths agreed, not that the rule
     * path worked.
     *
     * With no zone and no matching rule the program now says so instead.
     */
    return 0;
}

/*
 * Pull the executable name out of a launch command line.
 *
 * This is what makes --launch work for PACKAGED applications. Measured
 * (2026-09-20): `--launch notepad.exe` started pid 19672, which then handed
 * off - the window that appeared belonged to a DIFFERENT process. Windows
 * Notepad, Calculator, Photos and every other packaged app work this way, and
 * so do applications that re-exec themselves. Waiting on the returned pid
 * therefore waits forever, which is exactly what happened.
 *
 * The name is matched case-insensitively against the window's executable, so
 * "notepad.exe" finds "Notepad.exe".
 */
static void derive_exe_name(const wchar_t *cmd, wchar_t *out, size_t cch)
{
    const wchar_t *p;
    const wchar_t *end;
    wchar_t token[512];
    size_t len;

    if (out == NULL || cch == 0) return;
    out[0] = L'\0';
    if (cmd == NULL) return;

    p = cmd;
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
    if (len == 0 || len >= 512) return;

    wcsncpy_s(token, 512, p, len);
    wcsncpy_s(out, cch, win_file_name(token), _TRUNCATE);
}

/* ------------------------------------------------------------------ */
/* Launch snapshot                                                     */
/*                                                                     */
/* Everything on screen before the launch. A window that is already     */
/* open cannot be the one the launch created, and this is what stops    */
/* "open a second document" from re-placing the first one.              */
/* ------------------------------------------------------------------ */

#define SNAP_MAX WAC_WIN_MAX
static HWND g_snap[SNAP_MAX];
static int  g_snap_count;

static void snapshot_windows(void)
{
    /*
     * The same narrow filter the wait loop uses, not an unfiltered scan.
     * MEASURED: this desktop has over 256 top-level windows, so an
     * unfiltered snapshot fills its array and silently stops recording -
     * and every window it failed to record would later look "new". With the
     * filter the snapshot is about twenty handles, so nothing is missed.
     */
    int n = win_enum_top(g_all, WAC_WIN_MAX,
                         WIN_ENUM_VISIBLE_ONLY | WIN_ENUM_SKIP_TOOLWIN |
                         WIN_ENUM_SKIP_SMALL | WIN_ENUM_NEED_TITLE);
    g_snap_count = 0;
    for (int i = 0; i < n && g_snap_count < SNAP_MAX; ++i) {
        g_snap[g_snap_count++] = g_all[i].hwnd;
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
/* Listing                                                             */
/* ------------------------------------------------------------------ */

static void print_window_line(int index, const WindowInfo *w, WinFilterReason why)
{
    const RECT *r = &w->rect;

    t_out_line(L"[%2d] %-8ls hwnd=0x%p pid=%-6lu %dx%d at (%d,%d)",
               index,
               win_filter_reason_name(why),
               (void *)w->hwnd,
               (unsigned long)w->pid,
               (int)(r->right - r->left), (int)(r->bottom - r->top),
               (int)r->left, (int)r->top);

    t_out_line(L"       exe   : %ls", w->exe_path[0] ? w->exe_path : L"(unavailable)");
    t_out_line(L"       class : %ls", w->class_name);
    t_out_line(L"       title : %ls", w->title[0] ? w->title : L"(empty)");

    t_out_line(L"       state : %ls%ls%ls",
               w->is_visible ? L"visible " : L"HIDDEN ",
               w->is_iconic ? L"minimized " : (w->is_zoomed ? L"maximized " : L"normal "),
               w->is_cloaked ? L"cloaked" : L"");
}

static void do_list(int show_all)
{
    T_SEC(L"top-level windows");

    /*
     * No filters at all, then classify each one. Enumerating with the
     * placement filter would hide exactly the windows this listing exists to
     * explain.
     *
     * MEASURED (2026-09-20): an ordinary desktop of one monitor holds over
     * 256 top-level windows, of which 12 are application windows. Almost all
     * the rest are invisible helpers - an Electron app contributes a dozen on
     * its own. So the unfiltered listing is a wall of noise by default and
     * the accepted set is shown first; --list-all prints everything.
     */
    int n = win_enum_top(g_all, WAC_WIN_MAX, WIN_ENUM_NEED_EXE | WIN_ENUM_NEED_TITLE);

    int accepted = 0, rejected = 0;
    for (int i = 0; i < n; ++i) {
        DWORD pid = 0;
        WinFilterReason why = win_classify(g_all[i].hwnd, &pid);
        if (why == WIN_OK) accepted++; else rejected++;
    }

    t_out_line(L"scanned %d top-level window(s): %d application window(s), %d rejected",
               n, accepted, rejected);

    if (n >= WAC_WIN_MAX) {
        t_out_line(L"  !! the scan hit its cap of %d, so some windows were never examined",
                   WAC_WIN_MAX);
    }
    t_out_line(L"");

    for (int i = 0; i < n; ++i) {
        DWORD pid = 0;
        WinFilterReason why = win_classify(g_all[i].hwnd, &pid);
        if (!show_all && why != WIN_OK) continue;
        print_window_line(i, &g_all[i], why);
    }

    t_out_line(L"");

    if (!show_all && rejected > 0) {
        t_out_line(L"re-run with --list-all to see all %d windows and the reason each", n);
        t_out_line(L"rejected one was rejected");
    }
}

/* ------------------------------------------------------------------ */
/* Selection                                                           */
/* ------------------------------------------------------------------ */

static int find_matches(const AppId *sel, int need_exe)
{
    unsigned flags = WIN_ENUM_DEFAULT;
    if (!need_exe) flags &= ~WIN_ENUM_NEED_EXE;

    int n = win_enum_top(g_all, WAC_WIN_MAX, flags);
    int k = 0;

    for (int i = 0; i < n; ++i) {
        if (app_id_match(sel, &g_all[i])) {
            if (k < WAC_WIN_MAX) g_hits[k] = g_all[i];
            k++;
        }
    }
    return k;
}

/* ------------------------------------------------------------------ */
/* The launch wait loop                                                */
/* ------------------------------------------------------------------ */

/*
 * Score a candidate main window.
 *
 * Bigger is better, and a window that is neither minimized nor maximized
 * beats one that is: an application that restores its own window state is
 * still starting up.
 *
 * Returns 0 for a window that cannot be the main window.
 */
static LONGLONG candidate_score(const WindowInfo *w)
{
    if (!w->is_visible) return 0;
    if (w->is_cloaked)  return 0;
    if (w->ex_style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return 0;

    LONG cw = w->rect.right - w->rect.left;
    LONG ch = w->rect.bottom - w->rect.top;

    /*
     * The splash-screen threshold. A splash is typically a few hundred
     * pixels; a real main window is bigger. This is a heuristic and it is
     * deliberately paired with the settle test below rather than relied on
     * alone.
     */
    if (cw < 200 || ch < 150) return 0;

    LONGLONG area = (LONGLONG)cw * (LONGLONG)ch;
    if (w->is_iconic || w->is_zoomed) area /= 4;   /* less likely to be the main one */
    return area;
}

/*
 * What counts as the window we are waiting for.
 *
 * Three ways to narrow it, combined with AND:
 *
 *   pid        the process CreateProcessW returned - correct when the
 *              program creates its own window, which most do
 *   exe_name   the executable's file name - necessary when it does NOT,
 *              which is every packaged app
 *   only_new   ignore anything already on screen at launch time - necessary
 *              when the program is already running and this launch only
 *              opened another window
 */
typedef struct {
    DWORD          pid;        /* 0 = any process */
    const wchar_t *exe_name;   /* NULL = any executable */
    int            only_new;   /* skip windows from the launch snapshot */
} WaitFilter;

/* Best candidate matching the filter, right now. */
static HWND best_matching(const WaitFilter *wf, RECT *out_rect, LONGLONG *out_score)
{
    /*
     * Filter in the enumeration rather than afterwards. An unfiltered scan of
     * this machine returns over 256 windows and opens a process handle for
     * each one when the executable path is wanted - and this runs every 50 ms
     * for up to fifteen seconds. Narrowing first makes the per-tick cost
     * roughly twenty handles instead of hundreds.
     */
    unsigned flags = WIN_ENUM_VISIBLE_ONLY | WIN_ENUM_SKIP_TOOLWIN |
                     WIN_ENUM_SKIP_SMALL | WIN_ENUM_NEED_TITLE;
    if (wf->exe_name != NULL) flags |= WIN_ENUM_NEED_EXE;

    int n = win_enum_top(g_all, WAC_WIN_MAX, flags);
    HWND best = NULL;
    LONGLONG best_score = 0;

    for (int i = 0; i < n; ++i) {
        const WindowInfo *w = &g_all[i];

        if (wf->pid != 0 && w->pid != wf->pid) continue;
        if (wf->only_new && !is_new_window(w->hwnd)) continue;

        if (wf->exe_name != NULL) {
            if (w->exe_path[0] == L'\0') continue;
            if (_wcsicmp(win_file_name(w->exe_path), wf->exe_name) != 0) continue;
        }

        LONGLONG s = candidate_score(w);
        if (s > best_score) {
            best_score = s;
            best = w->hwnd;
            if (out_rect) *out_rect = w->rect;
        }
    }

    if (out_score) *out_score = best_score;
    return best;
}

/*
 * Wait for a process to put its real main window on screen.
 *
 * THE SPLASH PROBLEM
 *
 *   Most applications show something before they show the window the user
 *   means. Chromium and Electron create hidden or off-screen windows first;
 *   VS Code, Office and most Java applications show a splash. Placing the
 *   first window that appears moves the splash, which then disappears, and
 *   the real window opens wherever it wanted to - which looks exactly like
 *   the tool not working.
 *
 * THE SETTLE TEST
 *
 *   A candidate must therefore persist AND keep the same rectangle for
 *   SETTLE_MS before it is accepted. A splash that is about to be replaced
 *   either vanishes (so it stops being a candidate) or is resized by the
 *   application as it finishes loading (so its rectangle changes). Either
 *   way it fails to settle.
 *
 *   This is a heuristic, not a guarantee, and the timeout path below says so
 *   rather than pretending otherwise.
 */
#define SETTLE_MS 300

static HWND wait_for_main_window(const WaitFilter *wf, int budget_ms)
{
    const DWORD start = GetTickCount();
    DWORD settle_start = 0;
    HWND settled = NULL;
    RECT settle_rect;
    ZeroMemory(&settle_rect, sizeof(settle_rect));

    t_out_line(L"  waiting up to %d ms for a window that settles for %d ms",
               budget_ms, SETTLE_MS);

    for (;;) {
        RECT r;
        LONGLONG score = 0;
        HWND h = best_matching(wf, &r, &score);
        DWORD now = GetTickCount();

        if (h == NULL) {
            settle_start = 0;
            settled = NULL;
        } else if (h != settled) {
            /* A new best candidate: start its settle clock. */
            settled = h;
            settle_rect = r;
            settle_start = now;
            t_out_line(L"  candidate 0x%p  %dx%d at (%d,%d) - settling",
                       (void *)h,
                       (int)(r.right - r.left), (int)(r.bottom - r.top),
                       (int)r.left, (int)r.top);
        } else if (r.left   != settle_rect.left  || r.top    != settle_rect.top ||
                   r.right  != settle_rect.right || r.bottom != settle_rect.bottom) {
            /* It moved or resized: still starting up. Restart the clock. */
            t_out_line(L"  candidate 0x%p changed geometry - settling again", (void *)h);
            settle_rect = r;
            settle_start = now;
        } else if (now - settle_start >= SETTLE_MS) {
            t_out_line(L"  settled: 0x%p  %dx%d at (%d,%d)",
                       (void *)h,
                       (int)(r.right - r.left), (int)(r.bottom - r.top),
                       (int)r.left, (int)r.top);
            return h;
        }

        if (now - start >= (DWORD)budget_ms) break;
        Sleep(50);
    }

    /*
     * Timed out with no settled window.
     *
     * The fallback the plan calls for: take the largest visible window the
     * process owns, even though it never settled. A window that is present
     * but slow to settle is a far better outcome than "no window found",
     * and the caller is told which path was taken.
     */
    RECT r;
    LONGLONG score = 0;
    HWND h = best_matching(wf, &r, &score);
    if (h != NULL) {
        t_out_line(L"  !! budget exhausted; falling back to the largest visible window");
        t_out_line(L"     0x%p  %dx%d at (%d,%d)",
                   (void *)h,
                   (int)(r.right - r.left), (int)(r.bottom - r.top),
                   (int)r.left, (int)r.top);
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* Target rectangle                                                    */
/* ------------------------------------------------------------------ */

/*
 * Work out the physical rectangle a zone means on a monitor.
 *
 * Uses rel_to_pixels(), which maps each fraction to its OWN axis. That is
 * correct here and only here: --zone is a share of the screen, so "the left
 * half" must be the left half of whatever panel the window is on. Restoring
 * a window's remembered SIZE is a different question with a different
 * function - rel_resolve_size() - and using the wrong one is what reshaped a
 * 16:9 window into 9:16 on a portrait panel (see docs/架构与数据结构.md 2.1.1).
 */
static BOOL zone_to_rect(const Options *o, const RECT *mrect, RECT *out)
{
    RelRect z;
    POINT pt;
    SIZE sz;

    if (o == NULL || mrect == NULL || out == NULL) return FALSE;

    ZeroMemory(&z, sizeof(z));
    z.x = o->zx; z.y = o->zy; z.w = o->zw; z.h = o->zh;
    z.min_w = 1; z.min_h = 1;

    if (!rel_to_pixels(&z, mrect, &pt, &sz)) return FALSE;

    out->left   = pt.x;
    out->top    = pt.y;
    out->right  = pt.x + sz.cx;
    out->bottom = pt.y + sz.cy;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Stored rules                                                        */
/* ------------------------------------------------------------------ */

/*
 * Rules live in the SAME file as everything else, through the same store:
 * atomic replace, rotating backups, a checksum. A rules file is not a
 * second-class citizen kept off to one side - losing it costs the user
 * exactly as much as losing a workspace list.
 */
static void print_rule(int index, const Rule *r)
{
    wchar_t app[WAC_EXE_MAX + WAC_CLASS_MAX + 32];

    app[0] = L'\0';
    app_id_describe(&r->app, app, ARRAYSIZE(app));

    t_out_line(L"  [%2d] %-20ls %ls", index,
               r->label[0] ? r->label : L"(no label)", app);
    t_out_line(L"       zone : x=%.4f y=%.4f w=%.4f h=%.4f",
               (double)r->zone.x, (double)r->zone.y,
               (double)r->zone.w, (double)r->zone.h);

    if (r->zone.abs_w > 0 && r->zone.abs_h > 0) {
        t_out_line(L"       size : remembered %dx%d px (measured on a %d px wide monitor)",
                   r->zone.abs_w, r->zone.abs_h, r->zone.ref_w);
        t_out_line(L"              - keeps the window's own shape, shrunk only if it");
        t_out_line(L"                no longer fits the panel");
    } else {
        t_out_line(L"       size : a share of the screen - follows the panel's shape");
    }

    t_out_line(L"       id   : %hs    enabled: %ls", r->id,
               r->enabled ? L"yes" : L"no");
}

static void do_rules_list(const RuleSet *rules)
{
    wchar_t path[MAX_PATH];

    T_SEC(L"stored placement rules");

    if (config_file(path, MAX_PATH)) t_out_line(L"  file : %ls", path);
    t_out_line(L"  count: %d of %d", rules_count(rules), RULE_MAX);
    t_out_line(L"");

    if (rules_count(rules) == 0) {
        t_out_line(L"  (none yet)");
        t_out_line(L"");
        t_out_line(L"  record one with:");
        t_out_line(L"    t30_place.exe --app notepad.exe --zone 0,0,0.5,1 --remember");
        t_out_line(L"    t30_place.exe --app notepad.exe --remember      (keeps where it is now)");
        return;
    }

    for (int i = 0; i < rules_count(rules); ++i) {
        print_rule(i, rules_at(rules, i));
        t_out_line(L"");
    }
}

static int do_forget(RuleSet *rules, const wchar_t *name)
{
    int idx = rules_find_by_name(rules, name);

    T_SEC(L"forget a rule");

    if (idx < 0) {
        t_out_line(L"[error] no rule matches \"%ls\"", name);
        t_out_line(L"        run --rules to see the list");
        return 1;
    }

    print_rule(idx, rules_at(rules, idx));
    t_out_line(L"");

    if (!rules_remove(rules, idx)) {
        t_out_line(L"[error] could not remove it");
        return 1;
    }

    if (!rules_save(rules)) {
        t_out_line(L"[error] could not write the config file");
        return 1;
    }

    t_out_line(L"  removed; %d rule(s) left", rules_count(rules));
    return 0;
}

/*
 * Apply every matching rule to every open window.
 *
 * This is "put my desktop back the way it was" - the operation a user wants
 * after a monitor change, a reboot, or a session of dragging things around.
 *
 * A window already in the right place is still passed through win_place_apply:
 * the read-back is what decides, and skipping it would mean guessing.
 */
static int do_apply(const Options *o, const RuleSet *rules)
{
    int n, matched = 0, ok = 0;

    T_SEC(L"apply stored rules");

    n = win_enum_top(g_all, WAC_WIN_MAX, WIN_ENUM_DEFAULT);
    t_out_line(L"  scanning %d application window(s) against %d rule(s)",
               n, rules_count(rules));
    t_out_line(L"");

    for (int i = 0; i < n; ++i) {
        int ri = rules_match(rules, &g_all[i]);
        if (ri < 0) continue;

        const Rule *r = rules_at(rules, ri);
        RECT mon, target;
        POINT pt;
        SIZE sz;

        matched++;

        /*
         * The monitor the window is on NOW. A rule stores a share of a
         * screen, not an identity for one, so the same rule does the right
         * thing on whichever monitor the window happens to be.
         */
        pt.x = (g_all[i].rect.left + g_all[i].rect.right) / 2;
        pt.y = (g_all[i].rect.top  + g_all[i].rect.bottom) / 2;

        int mi = monitor_at_point(g_mons, g_mon_count, (int)pt.x, (int)pt.y);
        if (mi < 0) mi = monitor_primary_index(g_mons, g_mon_count);

        ZeroMemory(&mon, sizeof(mon));
        ZeroMemory(&target, sizeof(target));
        ZeroMemory(&sz, sizeof(sz));

        if (!monitor_rect(g_mons, g_mon_count, mi, &mon)) continue;
        if (!rule_target_rect(r, &mon, &target)) continue;

        t_out_line(L"  [%ls]  %ls", r->label[0] ? r->label : L"rule", g_all[i].title);
        t_out_line(L"       pid %lu on monitor %d -> (%d,%d)-(%d,%d) %dx%d",
                   (unsigned long)g_all[i].pid, mi,
                   (int)target.left, (int)target.top,
                   (int)target.right, (int)target.bottom,
                   (int)(target.right - target.left),
                   (int)(target.bottom - target.top));

        PlaceRequest req;
        ZeroMemory(&req, sizeof(req));
        req.hwnd         = g_all[i].hwnd;
        req.target       = target;
        req.allow_resize = o->move_only ? FALSE : (r->zone.lock_size ? FALSE : TRUE);

        PlaceReport rep;
        PlaceResult res = win_place_apply(&req, &rep);

        t_out_line(L"       -> %ls   pos error %d px, size error %d px",
                   win_place_result_name(res), rep.pos_error, rep.size_error);

        if (win_place_result_ok(res)) ok++;
    }

    t_out_line(L"");
    t_out_line(L"  %d window(s) matched, %d placed", matched, ok);

    return (matched == 0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Placement                                                           */
/* ------------------------------------------------------------------ */

static void report_place(const PlaceReport *rep)
{
    T_SEC(L"placement report");

    t_out_line(L"  result     : %ls", win_place_result_name(rep->result));
    t_out_line(L"  requested  : (%d,%d)-(%d,%d)  %dx%d",
               (int)rep->requested.left, (int)rep->requested.top,
               (int)rep->requested.right, (int)rep->requested.bottom,
               (int)(rep->requested.right - rep->requested.left),
               (int)(rep->requested.bottom - rep->requested.top));
    t_out_line(L"  before     : (%d,%d)-(%d,%d)  %dx%d",
               (int)rep->before.left, (int)rep->before.top,
               (int)rep->before.right, (int)rep->before.bottom,
               (int)(rep->before.right - rep->before.left),
               (int)(rep->before.bottom - rep->before.top));
    t_out_line(L"  after      : (%d,%d)-(%d,%d)  %dx%d",
               (int)rep->after.left, (int)rep->after.top,
               (int)rep->after.right, (int)rep->after.bottom,
               (int)(rep->after.right - rep->after.left),
               (int)(rep->after.bottom - rep->after.top));

    t_out_line(L"  pos error  : %d px", rep->pos_error);
    t_out_line(L"  size error : %d px", rep->size_error);
    t_out_line(L"  did resize : %ls", rep->did_resize ? L"yes" : L"no");

    if (rep->last_error != 0) {
        t_out_line(L"  last error : %lu", (unsigned long)rep->last_error);
    }

    /*
     * The focus check is part of the report, not a separate mode: stealing
     * focus is the failure mode that would make this feature unusable in
     * practice, and it must be visible on every single run.
     */
    t_out_line(L"  focus      : before=0x%p after=0x%p  %ls",
               (void *)rep->fg_before, (void *)rep->fg_after,
               (rep->fg_before == rep->fg_after) ? L"UNCHANGED (correct)"
                                                 : L"CHANGED");

    switch (rep->result) {
    case PLACE_OK:
        t_out_line(L"  note       : position and size both as requested");
        break;
    case PLACE_MOVED_ONLY:
        t_out_line(L"  note       : the window refuses to resize - record lock_size=1");
        t_out_line(L"               so it is never asked again");
        break;
    case PLACE_CLAMPED:
        t_out_line(L"  note       : the window accepted a different size; it enforces");
        t_out_line(L"               its own limits. Not worth locking.");
        break;
    case PLACE_DENIED:
        t_out_line(L"  note       : Windows refused. The usual cause is that the target");
        t_out_line(L"               runs elevated (UIPI) and this program does not.");
        t_out_line(L"               Run this program elevated to place those windows.");
        break;
    case PLACE_SKIPPED_MINIMIZED:
        t_out_line(L"  note       : the window is minimized and was left that way on");
        t_out_line(L"               purpose - a layout must not restore what the user hid");
        break;
    case PLACE_SKIPPED_FULLSCREEN:
        t_out_line(L"  note       : exclusive full-screen window, left alone");
        break;
    default:
        break;
    }
}

/*
 * Place one window at an already-computed rectangle.
 *
 * Deciding WHERE is the caller's job: a --zone and a stored rule are two
 * different questions (a share of the screen versus a remembered pixel size)
 * and answering them in two places is how the two answers drift apart. This
 * function only moves things and reports what happened.
 */
static int place_one(const Options *o, HWND hwnd, const wchar_t *label,
                     const RECT *target)
{
    t_out_line(L"");
    t_out_line(L"target     : %ls", label);
    t_out_line(L"window     : 0x%p", (void *)hwnd);

    /*
     * Focus baseline.
     *
     * Sampled HERE rather than trusting the value win_place_apply() captures
     * in its first microseconds: a console process started from a script can
     * see GetForegroundWindow() return NULL simply because the desktop is
     * still handing over, and that NULL then reads as "the focus changed" when
     * nothing of the sort happened. Two samples, spaced out, mean something.
     */
    HWND fg1 = GetForegroundWindow();
    Sleep(60);
    HWND fg2 = GetForegroundWindow();
    t_out_line(L"foreground baseline : 0x%p (re-checked 0x%p)", (void *)fg1, (void *)fg2);

    PlaceRequest req;
    ZeroMemory(&req, sizeof(req));
    req.hwnd         = hwnd;
    req.target       = *target;
    req.allow_resize = o->move_only ? FALSE : TRUE;
    req.tolerance_px = 0;    /* the default */

    PlaceReport rep;
    PlaceResult r = win_place_apply(&req, &rep);

    HWND fg3 = GetForegroundWindow();
    t_out_line(L"foreground after    : 0x%p  %ls", (void *)fg3,
               (fg3 == fg2) ? L"UNCHANGED - focus was not stolen"
                            : L"CHANGED - investigate");

    report_place(&rep);

    return win_place_result_ok(r) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Probe                                                               */
/* ------------------------------------------------------------------ */

static void probe_one(HWND hwnd, const WindowInfo *w)
{
    RECT mon;
    ZeroMemory(&mon, sizeof(mon));
    BOOL have_mon = FALSE;

    HMONITOR hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (hm != NULL) {
        MONITORINFO mi;
        ZeroMemory(&mi, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hm, &mi)) {
            mon = mi.rcMonitor;
            have_mon = TRUE;
        }
    }

    t_out_line(L"  hwnd        : 0x%p", (void *)hwnd);
    t_out_line(L"  pid         : %lu", (unsigned long)w->pid);
    t_out_line(L"  rect        : (%d,%d)-(%d,%d)  %dx%d",
               (int)w->rect.left, (int)w->rect.top,
               (int)w->rect.right, (int)w->rect.bottom,
               (int)(w->rect.right - w->rect.left),
               (int)(w->rect.bottom - w->rect.top));
    t_out_line(L"  style       : 0x%08lX   exstyle: 0x%08lX",
               (unsigned long)(w->style & 0xFFFFFFFFu),
               (unsigned long)(w->ex_style & 0xFFFFFFFFu));
    t_out_line(L"  WS_THICKFRAME (says resizable): %ls",
               (w->style & WS_THICKFRAME) ? L"yes" : L"no");
    t_out_line(L"  WS_CAPTION                    : %ls",
               (w->style & WS_CAPTION) ? L"yes" : L"no");
    t_out_line(L"  visible/minimized/maximized   : %d / %d / %d",
               w->is_visible, w->is_iconic, w->is_zoomed);
    t_out_line(L"  cloaked                       : %d", w->is_cloaked);

    if (have_mon) {
        t_out_line(L"  on monitor  : (%d,%d)-(%d,%d)",
                   (int)mon.left, (int)mon.top, (int)mon.right, (int)mon.bottom);
        t_out_line(L"  full-screen exclusive         : %ls",
                   win_is_fullscreen_exclusive(hwnd, &mon) ? L"YES (would be skipped)"
                                                           : L"no");
    }

    /*
     * The style bit is reported as a HINT and labelled as one. Stage 1
     * measured a window with WS_THICKFRAME that refused to resize, so the
     * only way to know is to try and read back - which is what --place does.
     */
    t_out_line(L"  would be skipped               : %ls",
               (w->is_iconic || (have_mon && win_is_fullscreen_exclusive(hwnd, &mon)))
                   ? L"yes" : L"no");
    t_out_line(L"  NOTE: WS_THICKFRAME is necessary but NOT sufficient. Stage 1");
    t_out_line(L"        measured a window with the bit set that still refused.");
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    Options o;

    t_diag_init(L"t30_place");
    t_console_init();

    {
        /* 1 = --help was printed, -1 = a bad argument. Neither may fall
         * through into the placement path. */
        int pr = parse_args(argc, argv, &o);
        if (pr != 0) {
            if (!o.no_pause) t_pause();
            return (pr > 0) ? 0 : 1;
        }
    }
    if (o.log_all) t_log_mirror(1);

    t_set_dpi_awareness();

    T_TITLE(L"t30: window placement");
    t_out_line(L"time: %ls   dpi-awareness: %ls", t_now_string(), t_dpi_awareness_name());

    g_mon_count = monitor_enumerate(g_mons, WAC_MON_MAX);
    t_out_line(L"monitors: %d", g_mon_count);

    if (o.do_list) {
        do_list(o.do_list_all);
        if (!o.no_pause) t_pause();
        return 0;
    }

    /* ---- stored rules ---------------------------------------------- */

    /* ~70 KB, so static rather than a stack frame - see rule.h. */
    static RuleSet rules;
    rules_load(&rules);

    if (o.do_rules) {
        do_rules_list(&rules);
        if (!o.no_pause) t_pause();
        return 0;
    }

    if (o.forget_name[0] != L'\0') {
        int frc = do_forget(&rules, o.forget_name);
        if (!o.no_pause) t_pause();
        return frc;
    }

    if (o.do_apply) {
        int arc = do_apply(&o, &rules);
        if (!o.no_pause) t_pause();
        return arc;
    }

    /* ---- launch --------------------------------------------------- */

    DWORD launched_pid = 0;

    if (o.do_launch) {
        T_SEC(L"launch");

        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));

        /* CreateProcessW may modify the command line buffer, so it must be
         * writable - a string literal here would crash. */
        wchar_t cmd[512];
        wcsncpy_s(cmd, 512, o.launch_cmd, _TRUNCATE);

        /*
         * Photograph the desktop BEFORE starting anything. Whatever the
         * launch puts on screen is new by definition, and this is the only
         * reliable way to tell "the window we just caused" from "a window
         * that was already open" when the program is one the user already
         * has running.
         */
        snapshot_windows();
        t_out_line(L"  snapshot: %d window(s) already open", g_snap_count);
        t_out_line(L"  command: %ls", cmd);

        if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                            CREATE_UNICODE_ENVIRONMENT, NULL, NULL, &si, &pi)) {
            T_LASTERR(L"CreateProcessW");
            t_out_line(L"[error] could not start the program");
            if (!o.no_pause) t_pause();
            return 1;
        }

        launched_pid = pi.dwProcessId;
        t_out_line(L"  started: pid=%lu", (unsigned long)launched_pid);

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        derive_exe_name(o.launch_cmd, o.launch_exe, WAC_EXE_MAX);
        t_out_line(L"  program: %ls",
                   o.launch_exe[0] ? o.launch_exe : L"(name not recognised in the command)");
    }

    /* ---- select --------------------------------------------------- */

    if (app_id_empty(&o.sel) && !o.do_launch) {
        t_out_line(L"");
        t_out_line(L"[error] no target selected - pass --app, --pid, --hwnd or --launch");
        t_out_line(L"");
        usage();
        if (!o.no_pause) t_pause();
        return 1;
    }

    wchar_t desc[512];
    if (o.do_launch && o.launch_exe[0] != L'\0') {
        wcsncpy_s(desc, 512, o.launch_exe, _TRUNCATE);
    } else {
        app_id_describe(&o.sel, desc, 512);
    }

    T_SEC(L"selection");
    t_out_line(L"  select : %ls", desc);

    HWND target = NULL;
    WindowInfo target_info;
    ZeroMemory(&target_info, sizeof(target_info));

    if (o.sel.hwnd != NULL) {
        /*
         * An explicit handle names ONE window, so it is used directly.
         *
         * It must NOT go through the wait loop below: that searches by
         * process id and returns the process's LARGEST window, which is a
         * different question. With two documents open it would quietly place
         * the other one, and the report would look perfectly successful.
         */
        if (!win_alive(o.sel.hwnd)) {
            t_out_line(L"[error] that window handle is no longer valid");
            if (!o.no_pause) t_pause();
            return 1;
        }
        target = o.sel.hwnd;
        t_out_line(L"  using the given handle directly (no wait needed)");

        if (!win_fill_info(target, &target_info, WIN_ENUM_NEED_EXE)) {
            t_out_line(L"[error] the window vanished before it could be read");
            if (!o.no_pause) t_pause();
            return 1;
        }
    } else if (o.do_launch || o.sel.pid != 0) {
        /*
         * A process - or just a program - was named, but not a window, so
         * wait for one to appear and settle.
         */
        WaitFilter wf;
        ZeroMemory(&wf, sizeof(wf));

        if (o.do_launch && o.launch_exe[0] != L'\0') {
            /*
             * Keyed on the executable name, NOT on the pid CreateProcessW
             * returned. Measured (2026-09-20): `notepad.exe` returned pid
             * 19672, that process handed off and exited, and the window
             * belonged to a different process entirely - so waiting on the
             * pid timed out after fifteen seconds while a Notepad window sat
             * on screen the whole time. Every packaged application works
             * this way (Calculator, Photos, the Store apps), as do programs
             * that re-exec themselves.
             */
            wf.exe_name = o.launch_exe;
            wf.only_new = 1;
            t_out_line(L"  key: a NEW window of %ls (the launched pid is not reliable)",
                       o.launch_exe);
        } else {
            wf.pid = o.sel.pid;
            t_out_line(L"  key: a window of pid %lu", (unsigned long)wf.pid);
        }

        if (wf.pid == 0 && wf.exe_name == NULL) {
            t_out_line(L"[error] could not determine what to wait for");
            if (!o.no_pause) t_pause();
            return 1;
        }

        target = wait_for_main_window(&wf, o.wait_ms);

        if (target == NULL) {
            t_out_line(L"[error] no window appeared within %d ms", o.wait_ms);
            t_out_line(L"        the process may have exited, or it shows no top-level window");
            if (!o.no_pause) t_pause();
            return 1;
        }

        if (!win_fill_info(target, &target_info, WIN_ENUM_NEED_EXE)) {
            t_out_line(L"[error] the window vanished before it could be read");
            if (!o.no_pause) t_pause();
            return 1;
        }
    } else {
        int n = find_matches(&o.sel, 1);

        t_out_line(L"  matches: %d", n);

        if (n == 0) {
            t_out_line(L"[error] no window matches that rule");
            t_out_line(L"        run --list to see every window and why each one was");
            t_out_line(L"        accepted or rejected");
            if (!o.no_pause) t_pause();
            return 1;
        }

        for (int i = 0; i < n; ++i) {
            t_out_line(L"  [%d] hwnd=0x%p pid=%lu  %ls",
                       i, (void *)g_hits[i].hwnd,
                       (unsigned long)g_hits[i].pid,
                       g_hits[i].title);
        }

        if (n > 1) {
            t_out_line(L"");
            t_out_line(L"[note] %d windows match; using the first. Add --title or --class", n);
            t_out_line(L"       to narrow it down, or --hwnd to name one exactly.");
        }

        target = g_hits[0].hwnd;
        target_info = g_hits[0];
    }

    if (o.do_probe) {
        T_SEC(L"probe (nothing is changed)");
        probe_one(target, &target_info);
        if (!o.no_pause) t_pause();
        return 0;
    }

    /* ---- which monitor, and where does the window go -------------- */

    RECT cur;
    ZeroMemory(&cur, sizeof(cur));
    if (!GetWindowRect(target, &cur)) {
        t_out_line(L"[error] GetWindowRect failed for the target window");
        if (!o.no_pause) t_pause();
        return 1;
    }

    int mon_index = o.monitor_index;
    if (mon_index < 0) {
        int cx = (int)((cur.left + cur.right) / 2);
        int cy = (int)((cur.top  + cur.bottom) / 2);
        mon_index = monitor_at_point(g_mons, g_mon_count, cx, cy);
        if (mon_index < 0) {
            mon_index = monitor_primary_index(g_mons, g_mon_count);
            t_out_line(L"[warn] the window is on no known monitor; using the primary");
        }
    }

    RECT mon_rect;
    ZeroMemory(&mon_rect, sizeof(mon_rect));
    if (!monitor_rect(g_mons, g_mon_count, mon_index, &mon_rect)) {
        t_out_line(L"[error] monitor %d is not available", mon_index);
        if (!o.no_pause) t_pause();
        return 1;
    }

    RECT    target_rect;
    RelRect remember;
    int     have_remember = 0;
    int     rule_index = -1;

    ZeroMemory(&target_rect, sizeof(target_rect));
    ZeroMemory(&remember, sizeof(remember));

    if (o.have_zone) {
        if (!zone_to_rect(&o, &mon_rect, &target_rect)) {
            t_out_line(L"[error] cannot convert the zone to pixels on monitor %d", mon_index);
            if (!o.no_pause) t_pause();
            return 1;
        }

        if (o.do_remember) {
            /*
             * A ZONE rule: the fractions are the whole answer, so abs_w and
             * abs_h stay zero and the window takes the PANEL's shape. That is
             * what "the left half" means - and it is the opposite of what
             * --remember without a zone records.
             */
            remember.x = o.zx;
            remember.y = o.zy;
            remember.w = o.zw;
            remember.h = o.zh;
            remember.min_w = 1;
            remember.min_h = 1;
            have_remember = 1;
        }
    } else {
        rule_index = rules_match(&rules, &target_info);

        if (rule_index < 0 && o.do_remember) {
            /*
             * Nothing stored yet and --remember was given: the target is
             * simply where the window already is.
             *
             * This is the ONLY way to create a remembered-size rule, because
             * that kind of rule records the window's own pixel size - so the
             * user has to put the window where they want it first, and then
             * say "remember this". Recording it without moving anything is
             * exactly what that sentence means.
             */
            t_out_line(L"");
            t_out_line(L"  no rule yet - recording the window exactly where it is now");
            target_rect = cur;

            if (rel_from_pixels(&cur, &mon_rect, &remember)) {
                remember.min_w = 0;
                remember.min_h = 0;
                have_remember = 1;
            }
        } else if (rule_index < 0) {
            t_out_line(L"");
            t_out_line(L"[error] no --zone was given, and no stored rule matches this window");
            t_out_line(L"        place it once:   --zone 0,0,0.5,1");
            t_out_line(L"        or record one:   --remember");
            if (!o.no_pause) t_pause();
            return 1;
        } else {
            const Rule *r = rules_at(&rules, rule_index);
            t_out_line(L"");
            t_out_line(L"  rule   : [%d] %ls", rule_index,
                       r->label[0] ? r->label : L"(no label)");

            if (!rule_target_rect(r, &mon_rect, &target_rect)) {
                t_out_line(L"[error] the rule produced no usable rectangle");
                if (!o.no_pause) t_pause();
                return 1;
            }

            if (o.do_remember) {
                /*
                 * Re-recording takes the window's CURRENT rectangle, not the
                 * rule's. Running --remember again means "remember where it
                 * is now", which is how a user adjusts a rule: drag the
                 * window somewhere better, then say remember.
                 */
                if (rel_from_pixels(&cur, &mon_rect, &remember)) {
                    remember.min_w = 0;
                    remember.min_h = 0;
                    have_remember = 1;
                }
            }
        }
    }

    t_out_line(L"");
    t_out_line(L"monitor    : %d  (%d,%d)-(%d,%d)",
               mon_index, (int)mon_rect.left, (int)mon_rect.top,
               (int)mon_rect.right, (int)mon_rect.bottom);

    int rc = place_one(&o, target, desc, &target_rect);

    /* ---- remember -------------------------------------------------- */

    if (o.do_remember && have_remember) {
        AppId app;

        T_SEC(L"remember");

        if (!app_id_from_window(&target_info, &app)) {
            t_out_line(L"[error] this window's executable could not be read, so no rule");
            t_out_line(L"        could be built for it (a protected process?)");
        } else {
            wchar_t shown[WAC_EXE_MAX + 32];
            app_id_describe(&app, shown, ARRAYSIZE(shown));
            t_out_line(L"  app  : %ls", shown);

            int idx = rules_upsert(&rules, &app, &remember, NULL);
            if (idx < 0) {
                t_out_line(L"[error] the rule list is full (%d), or the app names nothing",
                           RULE_MAX);
            } else if (!rules_save(&rules)) {
                t_out_line(L"[error] could not write the config file");
            } else {
                t_out_line(L"  saved:");
                print_rule(idx, rules_at(&rules, idx));
            }
        }
    }

    T_SEC(L"verdict");
    t_out_line(L"  %ls", rc == 0 ? L"PASS - the window ended up where it was asked to be"
                                 : L"see the report above for what happened instead");

    if (!o.no_pause) t_pause();
    return rc;
}
