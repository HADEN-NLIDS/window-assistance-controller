/*
 * t15_framemath.c - unit tests for the maximised-window geometry in frame.c
 *
 * THE BUG THIS FILE EXISTS FOR
 *
 *   A self-drawn frame reports "no non-client area" so the caption can be
 *   painted as normal content. On a maximised window that is wrong: Windows
 *   makes the window rectangle one frame thickness LARGER than the work area on
 *   every side, and letting the client area follow it pushes the visible
 *   content past the screen edge and over the taskbar.
 *
 *   Measured on this machine at 150% scaling (dpi 144):
 *       SM_CXSIZEFRAME 5 + SM_CXPADDEDBORDER 6 = 11 px per side
 *       work area 2560x1600 -> window rectangle (-11,-11)-(2571,1611)
 *
 *   The fix is NOT to shrink ptMaxSize: probed with tests/t16_maxprobe.c, a
 *   compensation of 11 px produced a 2538x1578 window, 22 px SMALLER than the
 *   work area, because Windows uses a requested size verbatim once it differs
 *   from its default. The fix is to keep the request at the work area and clamp
 *   the CLIENT area to it in WM_NCCALCSIZE.
 *
 * WHAT THIS PROGRAM ASSERTS
 *
 *   For a set of constructed monitors, the two pure functions must compose with
 *   the measured platform behaviour into a client area that is exactly the work
 *   area. simulate_system() encodes the platform rule; if Windows ever changes
 *   it, these tests fail loudly instead of the window silently covering the
 *   taskbar again.
 *
 *   This machine's taskbar auto-hides, so work area == monitor and the overlap
 *   cannot be observed here at all - which is exactly why the arithmetic lives
 *   in pure functions and the interesting cases below are constructed.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t15_framemath
 *   tests\out\t15_framemath.exe --no-pause
 */

#include "test_common.h"

#include "../src/ui/frame.h"

static int g_pass, g_fail;

static void ok(const wchar_t *what, int cond)
{
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls", what);
    }
}

static void rect_str(const RECT *r, wchar_t *out, int cch)
{
    _snwprintf_s(out, cch, _TRUNCATE, L"(%d,%d)-(%d,%d) %dx%d",
                 r->left, r->top, r->right, r->bottom,
                 r->right - r->left, r->bottom - r->top);
}

static BOOL rect_eq(const RECT *a, const RECT *b)
{
    return (a->left == b->left && a->top == b->top &&
            a->right == b->right && a->bottom == b->bottom) ? TRUE : FALSE;
}

/*
 * The measured platform rule: ptMaxPosition is relative to the monitor's
 * top-left, and the frame thickness is added around the whole rectangle.
 */
static RECT simulate_system(const RECT *monitor, const POINT *pos,
                            const POINT *size, int border)
{
    RECT r;
    r.left   = monitor->left + pos->x - border;
    r.top    = monitor->top  + pos->y - border;
    r.right  = monitor->left + pos->x + size->x + border;
    r.bottom = monitor->top  + pos->y + size->y + border;
    return r;
}

/*
 * One case: the composed result - window rectangle, then clamped client area -
 * must put the client area exactly on the work area.
 */
static void check_case(const wchar_t *what, const RECT *work, const RECT *monitor,
                       int border)
{
    POINT pos, size;
    ZeroMemory(&pos, sizeof(pos));
    ZeroMemory(&size, sizeof(size));

    frame_compute_maximized_rect(work, monitor, &pos, &size);

    const RECT win = simulate_system(monitor, &pos, &size, border);
    const RECT cli = frame_maximized_client_rect(&win, work, border);

    /* The client area in screen coordinates: the window's true origin is the
     * rectangle Windows reports plus the invisible border. */
    RECT client_screen;
    client_screen.left   = win.left + border + cli.left;
    client_screen.top    = win.top  + border + cli.top;
    client_screen.right  = win.left + border + cli.right;
    client_screen.bottom = win.top  + border + cli.bottom;

    wchar_t want_s[80], got_s[80], win_s[80];
    rect_str(work, want_s, 80);
    rect_str(&client_screen, got_s, 80);
    rect_str(&win, win_s, 80);

    if (rect_eq(&client_screen, work)) {
        g_pass++;
        T_KV(L"case", L"%ls -> req(%d,%d %dx%d) win %ls -> client %ls",
             what, pos.x, pos.y, size.x, size.y, win_s, got_s);
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls", what);
        t_out_line(L"        want client %ls", want_s);
        t_out_line(L"        got  client %ls", got_s);
    }
}

/* ------------------------------------------------------------------ */

#define BORDER_150  11   /* measured: SM_CXSIZEFRAME 5 + SM_CXPADDEDBORDER 6 */

static void test_taskbar_bottom(void)
{
    T_SEC(L"taskbar at the bottom (1600 tall, 48 px taskbar)");

    const RECT mon  = { 0, 0, 2560, 1600 };
    const RECT work = { 0, 0, 2560, 1552 };

    check_case(L"150% scaling", &work, &mon, BORDER_150);
    check_case(L"100% scaling", &work, &mon, 8);
    check_case(L"no border at all", &work, &mon, 0);
}

static void test_taskbar_left(void)
{
    T_SEC(L"taskbar on the left");

    const RECT mon  = { 0, 0, 2560, 1600 };
    const RECT work = { 80, 0, 2560, 1600 };

    check_case(L"80 px taskbar on the left", &work, &mon, BORDER_150);
}

static void test_taskbar_top(void)
{
    T_SEC(L"taskbar at the top");

    const RECT mon  = { 0, 0, 2560, 1600 };
    const RECT work = { 0, 48, 2560, 1600 };

    check_case(L"48 px taskbar at the top", &work, &mon, BORDER_150);
}

static void test_autohide(void)
{
    T_SEC(L"taskbar auto-hidden (work area == monitor)");

    const RECT mon = { 0, 0, 2560, 1600 };

    check_case(L"work area equals the monitor", &mon, &mon, BORDER_150);
}

static void test_secondary_monitor(void)
{
    T_SEC(L"secondary monitor (non-zero origin)");

    /* A monitor to the right of the primary one, with its own taskbar space at
     * the bottom - virtual-desktop coordinates. */
    const RECT mon  = { 2560, 0, 4480, 1440 };
    const RECT work = { 2560, 0, 4480, 1392 };

    check_case(L"right-hand monitor", &work, &mon, BORDER_150);

    /* A monitor ABOVE the primary one: negative y in the virtual desktop. */
    const RECT mon2  = { 0, -1080, 1920, 0 };
    const RECT work2 = { 0, -1080, 1920, -48 };

    check_case(L"monitor above the primary", &work2, &mon2, BORDER_150);
}

static void test_degenerate(void)
{
    T_SEC(L"degenerate input");

    const RECT mon  = { 0, 0, 20, 20 };
    const RECT work = { 0, 0, 20, 20 };

    POINT pos, size;
    ZeroMemory(&pos, sizeof(pos));
    ZeroMemory(&size, sizeof(size));

    frame_compute_maximized_rect(&work, &mon, &pos, &size);
    ok(L"size stays non-negative", size.x >= 0 && size.y >= 0);

    /* NULL arguments must be survivable, not fatal. */
    frame_compute_maximized_rect(NULL, &mon, &pos, &size);
    frame_compute_maximized_rect(&work, NULL, &pos, &size);
    frame_compute_maximized_rect(&work, &mon, NULL, &size);
    frame_compute_maximized_rect(&work, &mon, &pos, NULL);

    const RECT cli = frame_maximized_client_rect(NULL, &work, 8);
    ok(L"client rect degrades to 0 with a NULL window rect",
       cli.left == 0 && cli.top == 0 && cli.right == 0 && cli.bottom == 0);

    const RECT cli2 = frame_maximized_client_rect(&mon, NULL, 8);
    ok(L"client rect degrades to 0 with a NULL work area",
       cli2.left == 0 && cli2.top == 0 && cli2.right == 0 && cli2.bottom == 0);

    ok(L"NULL arguments do not crash", 1);
}

/*
 * The counter-example: what the window looks like WITHOUT the client-area
 * clamp. This is not product code - it documents the measured failure so that
 * anyone tempted to delete the clamp can see exactly what it costs.
 */
static void test_unclamped_overlaps_taskbar(void)
{
    T_SEC(L"counter-example: no client-area clamp");

    const RECT mon  = { 0, 0, 2560, 1600 };
    const RECT work = { 0, 0, 2560, 1552 };

    POINT pos, size;
    ZeroMemory(&pos, sizeof(pos));
    ZeroMemory(&size, sizeof(size));
    frame_compute_maximized_rect(&work, &mon, &pos, &size);

    const RECT win = simulate_system(&mon, &pos, &size, BORDER_150);

    /* Without the clamp the client area is the whole window rectangle. */
    wchar_t s[80];
    rect_str(&win, s, 80);
    T_KV(L"unclamped client", L"%ls", s);
    T_KV(L"overhang", L"%d px past the work area at the bottom",
         win.bottom - work.bottom);

    ok(L"unclamped client is taller than the work area",
       (win.bottom - win.top) > (work.bottom - work.top));
    ok(L"unclamped client would cover the taskbar", win.bottom > work.bottom);

    /* And the earlier attempt: shrinking ptMaxSize instead of clamping the
     * client area. Measured with t16_maxprobe: once the requested size differs
     * from the platform default, Windows uses the requested RECTANGLE VERBATIM
     * and adds no border at all - so the window comes out 22 px SMALLER than the
     * work area instead of equal to it. */
    POINT shrunk_pos, shrunk_size;
    shrunk_pos.x  = pos.x + BORDER_150;
    shrunk_pos.y  = pos.y + BORDER_150;
    shrunk_size.x = size.x - 2 * BORDER_150;
    shrunk_size.y = size.y - 2 * BORDER_150;

    RECT win2;
    win2.left   = mon.left + shrunk_pos.x;
    win2.top    = mon.top  + shrunk_pos.y;
    win2.right  = mon.left + shrunk_pos.x + shrunk_size.x;
    win2.bottom = mon.top  + shrunk_pos.y + shrunk_size.y;

    rect_str(&win2, s, 80);
    T_KV(L"compensated window", L"%ls", s);
    T_KV(L"shortfall", L"%d px narrower than the work area",
         (work.right - work.left) - (win2.right - win2.left));

    ok(L"compensating ptMaxSize undershoots the work area",
       (win2.right - win2.left) < (work.right - work.left));
    ok(L"compensating ptMaxSize leaves a gap on every side",
       win2.left > work.left && win2.top > work.top);
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t15_framemath");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t15_framemath - maximised window geometry");
    T_KV(L"run_at", L"%ls", t_now_string());
    T_KV(L"border", L"%d px per side at 150%% scaling (measured)", BORDER_150);

    test_taskbar_bottom();
    test_taskbar_left();
    test_taskbar_top();
    test_autohide();
    test_secondary_monitor();
    test_degenerate();
    test_unclamped_overlaps_taskbar();

    T_SEC(L"Result");
    T_KV(L"passed", L"%d", g_pass);
    T_KV(L"failed", L"%d", g_fail);

    if (g_fail == 0) {
        T_KV(L"VERDICT", L"PASS - all %d assertions hold", g_pass);
    } else {
        T_KV(L"VERDICT", L"FAIL - %d assertion(s) broken", g_fail);
    }

    if (!no_pause) t_pause();

    t_log_close();
    return (g_fail == 0) ? 0 : 1;
}
