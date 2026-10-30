/*
 * t31_splash.c - the splash-screen filter in launch_score_candidate().
 *
 * WHAT IT PROVES
 *
 *   A splash screen, a tool window or a helper frame must never be mistaken
 *   for the main window; a real window must win by area; and a minimized or
 *   maximized window must be quartered rather than discarded.
 *
 *   Those are the rules the launch wait loop in launch.c relies on. They were
 *   pulled into a pure function (src/core/launch_score.c) so they can be
 *   pinned down here without a live window - the sizes and flags below are
 *   the same ones a splash screen actually reports.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t31_splash
 *   tests\out\t31_splash.exe --no-pause
 */

#include "test_common.h"

#include "../src/core/launch_score.h"

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

static void ok_ll(const wchar_t *what, LONGLONG got, LONGLONG want)
{
    if (got == want) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls  (got %lld, want %lld)", what, got, want);
    }
}

/* ------------------------------------------------------------------ */
/* A window with everything a main window needs, then one field changed */
/*                                                                    */
/* WindowInfo is about 1.3 KB, so it lives in static storage - the     */
/* project already lost a day to a large struct on the stack          */
/* (TRAPS.md #11).                                                     */
/* ------------------------------------------------------------------ */

static WindowInfo g_w;

static void make(int l, int t, int r, int b)
{
    ZeroMemory(&g_w, sizeof(g_w));
    g_w.rect.left   = l;
    g_w.rect.top    = t;
    g_w.rect.right  = r;
    g_w.rect.bottom = b;
    g_w.is_visible  = 1;
}

/* ------------------------------------------------------------------ */

static void test_filters(void)
{
    T_SEC(L"what can never be the main window");

    ok_ll(L"NULL", launch_score_candidate(NULL), 0);

    make(0, 0, 1000, 800);
    g_w.is_visible = 0;
    ok_ll(L"not visible", launch_score_candidate(&g_w), 0);

    make(0, 0, 1000, 800);
    g_w.is_cloaked = 1;
    ok_ll(L"cloaked by DWM", launch_score_candidate(&g_w), 0);

    make(0, 0, 1000, 800);
    g_w.ex_style = WS_EX_TOOLWINDOW;
    ok_ll(L"WS_EX_TOOLWINDOW", launch_score_candidate(&g_w), 0);

    make(0, 0, 1000, 800);
    g_w.ex_style = WS_EX_NOACTIVATE;
    ok_ll(L"WS_EX_NOACTIVATE", launch_score_candidate(&g_w), 0);

    make(0, 0, 1000, 800);
    g_w.ex_style = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
    ok_ll(L"both styles set together", launch_score_candidate(&g_w), 0);
}

static void test_size_boundary(void)
{
    T_SEC(L"the size floor (200 x 150)");

    make(0, 0, 199, 151);
    ok_ll(L"199 x 151 - one px too narrow", launch_score_candidate(&g_w), 0);

    make(0, 0, 200, 150);
    ok_ll(L"200 x 150 - exactly at the floor", launch_score_candidate(&g_w), 30000);

    make(0, 0, 201, 149);
    ok_ll(L"201 x 149 - one px too short", launch_score_candidate(&g_w), 0);

    make(0, 0, 600, 150);
    ok_ll(L"600 x 150 - wide but at the height floor",
          launch_score_candidate(&g_w), 90000);

    make(0, 0, 150, 600);
    ok_ll(L"150 x 600 - tall but below the width floor",
          launch_score_candidate(&g_w), 0);

    make(0, 0, 0, 0);
    ok_ll(L"zero-sized window", launch_score_candidate(&g_w), 0);

    make(0, 0, -100, -100);
    ok_ll(L"negative-sized window", launch_score_candidate(&g_w), 0);
}

static void test_area(void)
{
    T_SEC(L"a larger window wins by area");

    make(0, 0, 800, 600);
    ok_ll(L"800 x 600", launch_score_candidate(&g_w), 480000);

    make(0, 0, 2560, 1600);
    ok_ll(L"2560 x 1600 - the largest this machine reports",
          launch_score_candidate(&g_w), 4096000);

    /* The point of the score: a main window beats a bigger splash. */
    WindowInfo splash;
    ZeroMemory(&splash, sizeof(splash));
    splash.is_visible = 1;
    splash.rect.right = 1024;
    splash.rect.bottom = 768;
    const LONGLONG main_score = launch_score_candidate(&g_w);
    const LONGLONG splash_score = launch_score_candidate(&splash);
    ok(L"a 2560x1600 window outscores a 1024x768 one",
       main_score > splash_score);
}

static void test_minimized_and_maximized(void)
{
    T_SEC(L"minimized / maximized are quartered, not discarded");

    make(0, 0, 800, 600);
    g_w.is_iconic = 1;
    ok_ll(L"minimized 800 x 600", launch_score_candidate(&g_w), 120000);

    make(0, 0, 800, 600);
    g_w.is_zoomed = 1;
    ok_ll(L"maximized 800 x 600", launch_score_candidate(&g_w), 120000);

    /*
     * A window cannot really be both, but the scoring must not divide twice if
     * it ever is: /4 means "still starting up", and two flags are one reason.
     */
    make(0, 0, 800, 600);
    g_w.is_iconic = 1;
    g_w.is_zoomed = 1;
    ok_ll(L"both flags set - quartered once", launch_score_candidate(&g_w), 120000);

    /* And a normal window must still beat its own minimized self. */
    make(0, 0, 800, 600);
    const LONGLONG normal = launch_score_candidate(&g_w);
    g_w.is_iconic = 1;
    ok(L"a normal window outscores the same window minimized",
       normal > launch_score_candidate(&g_w));
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t31_splash");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t31_splash - the splash-screen filter (launch_score_candidate)");
    T_KV(L"run_at", L"%ls", t_now_string());
    T_NOTE(L"A splash screen is rejected by visibility, cloaking, tool-window");
    T_NOTE(L"styles or size; a real window wins by area; minimized and");
    T_NOTE(L"maximized windows are quartered because they are still starting.");

    test_filters();
    test_size_boundary();
    test_area();
    test_minimized_and_maximized();

    T_SEC(L"Result");
    T_KV(L"passed", L"%d", g_pass);
    T_KV(L"failed", L"%d", g_fail);

    if (g_fail == 0) T_KV(L"VERDICT", L"PASS - all %d assertions hold", g_pass);
    else             T_KV(L"VERDICT", L"FAIL - %d assertion(s) broken", g_fail);

    if (!no_pause) t_pause();
    t_log_close();
    return (g_fail == 0) ? 0 : 1;
}
