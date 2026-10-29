/*
 * t52_popup.c - unit tests for src/ui/popup.c
 *
 * WHY THIS FILE CAN EXIST AT ALL
 *
 *   The overlay's pop-up panels are drawn by overlay.c and driven by the mouse,
 *   and neither of those can be tested without a window. What CAN be tested is
 *   where a panel lands and what a point means once it is open - and those are
 *   exactly the parts that go wrong on someone else's screen rather than on the
 *   developer's:
 *
 *     - a panel near the right edge of a narrow monitor
 *     - a panel taller than the space under its button
 *     - a click that lands on the shared edge of the anchor and the panel
 *
 *   popup.c is free of Win32 calls by design (see popup.h), so this test links
 *   no window, pumps no messages, and passes on any machine.
 *
 * WHAT IT DOES NOT COVER
 *
 *   Whether the click that CLOSES a panel is correctly stopped from reaching
 *   the canvas is a property of overlay.c's message handler, not of this
 *   module - so it is not here, and it cannot be: it needs a real mouse and a
 *   real window. That check is done by screenshot (docs/手册.md section 8.4's
 *   lesson, third time).
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t52_popup
 *   tests\out\t52_popup.exe --no-pause
 */

#include "test_common.h"

#include "../src/ui/popup.h"

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

static void ok_int(const wchar_t *what, int got, int want)
{
    if (got == want) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls  (got %d, want %d)", what, got, want);
    }
}

static RECT r4(int l, int t, int r, int b)
{
    RECT x;
    x.left = l; x.top = t; x.right = r; x.bottom = b;
    return x;
}

static POINT p2(int x, int y)
{
    POINT p;
    p.x = x; p.y = y;
    return p;
}

/* ------------------------------------------------------------------ */

static void test_state(void)
{
    T_SEC(L"1. opening and closing");

    PopupState st;
    popup_init(&st);
    ok(L"a fresh state is closed", !popup_is_open(&st));
    ok_int(L"and names no panel", (int)popup_current(&st), (int)POPUP_NONE);

    const RECT anchor = r4(100, 10, 200, 50);
    const RECT panel  = r4(100, 50, 340, 230);
    popup_open(&st, POPUP_LAYOUT, &anchor, &panel);
    ok(L"it opens", popup_is_open(&st));
    ok_int(L"with the id asked for", (int)popup_current(&st), (int)POPUP_LAYOUT);
    ok_int(L"and the panel rectangle given", st.panel.right, 340);

    /* Replacing: opening a second panel must not leave the first one's id. */
    popup_open(&st, POPUP_MONITOR, &anchor, &panel);
    ok_int(L"opening another replaces it", (int)popup_current(&st), (int)POPUP_MONITOR);

    /*
     * ...nor the first one's RECTANGLE, and that is load-bearing.
     *
     * The paint path dispatches on the id and hands the open panel's rectangle
     * to the matching drawer - so a state that kept the old rectangle would
     * draw the new panel's contents into the old panel's place.
     *
     * MEASURED 2026-10-09, the hard way: overlay.c used to ask only "is a panel
     * open" before drawing the layout grid, so with the monitor dropdown up the
     * whole 2-column grid was painted into the dropdown's 220x72 rectangle -
     * the 左右半屏 / 三等分 captions and the tops of the thumbnails stuck out
     * under the list. Found by cropping the screenshot at native size; the
     * geometry assertions above could not see it, because every rectangle
     * involved was individually legal.
     */
    const RECT panel2 = r4(600, 50, 820, 122);
    popup_open(&st, POPUP_MONITOR, &anchor, &panel2);
    ok_int(L"and its rectangle comes with it", st.panel.left, 600);
    ok_int(L"the old rectangle is gone", st.panel.right, 820);

    /* An empty rectangle is refused rather than opened. A panel with no area is
     * invisible AND swallows clicks, which reads as "the mouse stopped
     * working". */
    const RECT empty = r4(10, 10, 10, 10);
    popup_open(&st, POPUP_LAYOUT, &empty, &panel);
    ok(L"an empty anchor is refused", !popup_is_open(&st));
    popup_open(&st, POPUP_LAYOUT, &anchor, &empty);
    ok(L"an empty panel is refused", !popup_is_open(&st));

    popup_open(&st, POPUP_NONE, &anchor, &panel);
    ok(L"opening POPUP_NONE is a close", !popup_is_open(&st));

    /* Closing keeps the rectangles: the caller repaints where the panel WAS,
     * and clearing them would leave the old panel on screen until something
     * else forced a full repaint. */
    popup_open(&st, POPUP_MONITOR, &anchor, &panel);
    popup_close(&st);
    ok(L"close closes", !popup_is_open(&st));
    ok_int(L"but the panel rectangle survives for the repaint", st.panel.right, 340);

    /* Safe on NULL throughout - overlay.c calls these from paths where the
     * overlay may already be gone. */
    popup_init(NULL);
    popup_close(NULL);
    ok(L"NULL state is not open", !popup_is_open(NULL));
    ok_int(L"NULL state hit-tests to NONE", (int)popup_hit(NULL, p2(0, 0)), (int)POPUP_HIT_NONE);
}

static void test_hit(void)
{
    T_SEC(L"2. what a point means while a panel is open");

    PopupState st;
    popup_init(&st);
    const RECT anchor = r4(100, 10, 200, 50);
    const RECT panel  = r4(100, 50, 340, 230);

    ok_int(L"closed: any point is NONE",
           (int)popup_hit(&st, p2(150, 100)), (int)POPUP_HIT_NONE);

    popup_open(&st, POPUP_LAYOUT, &anchor, &panel);

    ok_int(L"inside the panel", (int)popup_hit(&st, p2(150, 100)), (int)POPUP_HIT_PANEL);
    ok_int(L"on the anchor", (int)popup_hit(&st, p2(150, 30)), (int)POPUP_HIT_ANCHOR);
    ok_int(L"outside both", (int)popup_hit(&st, p2(1000, 800)), (int)POPUP_HIT_OUTSIDE);

    /* The shared edge: the anchor's bottom row is the panel's top row, and a
     * point there belongs to the PANEL only. Half-open rectangles are what
     * make that a property of the code rather than a coin flip. */
    ok_int(L"the shared edge belongs to the panel",
           (int)popup_hit(&st, p2(150, 50)), (int)POPUP_HIT_PANEL);

    /* Just outside the panel's right and bottom edges. */
    ok_int(L"one pixel right of the panel is outside",
           (int)popup_hit(&st, p2(340, 100)), (int)POPUP_HIT_OUTSIDE);
    ok_int(L"one pixel below the panel is outside",
           (int)popup_hit(&st, p2(150, 230)), (int)POPUP_HIT_OUTSIDE);

    /* Top-left corner is inside. */
    ok_int(L"the panel's top-left corner is inside",
           (int)popup_hit(&st, p2(100, 50)), (int)POPUP_HIT_PANEL);
}

static void test_hover(void)
{
    T_SEC(L"3. the hover rule: button OR panel keeps it up");

    PopupState st;
    popup_init(&st);
    const RECT anchor = r4(100, 10, 200, 50);
    const RECT panel  = r4(100, 50, 340, 230);
    popup_open(&st, POPUP_LAYOUT, &anchor, &panel);

    ok(L"on the panel - stays", popup_keep_hover(&st, p2(200, 120)));
    ok(L"on the anchor - stays", popup_keep_hover(&st, p2(150, 30)));

    /* The whole point of the rule: the path from the button down onto the
     * panel must not close it on the way. These are the pixels a real pointer
     * passes through. */
    ok(L"on the seam between them - stays", popup_keep_hover(&st, p2(150, 50)));

    ok(L"away from both - goes", !popup_keep_hover(&st, p2(900, 700)));
    ok(L"one pixel right of the panel - goes", !popup_keep_hover(&st, p2(340, 120)));
    ok(L"one pixel above the anchor - goes", !popup_keep_hover(&st, p2(150, 9)));

    ok(L"closed: nothing to keep", !popup_keep_hover(NULL, p2(150, 30)));
}

static void test_placement(void)
{
    T_SEC(L"4. where the panel lands");

    const RECT screen = r4(0, 0, 2560, 1440);
    RECT out;

    /* The ordinary case: below the button, left edges aligned. */
    const RECT a1 = r4(100, 10, 220, 50);
    ok(L"placed", popup_place_below(&a1, 240, 180, &screen, &out));
    ok_int(L"x matches the anchor's left", out.left, 100);
    ok_int(L"y starts at the anchor's bottom", out.top, 50);
    ok_int(L"width as asked", out.right - out.left, 240);
    ok_int(L"height as asked", out.bottom - out.top, 180);

    /* Near the right edge the panel slides left rather than running off. */
    const RECT a2 = r4(2500, 10, 2560, 50);
    ok(L"placed near the right edge", popup_place_below(&a2, 240, 180, &screen, &out));
    ok_int(L"slid left to fit", out.right, 2560);
    ok_int(L"still the width asked for", out.left, 2320);

    /* Wider than the screen: clamped, not clipped. */
    ok(L"placed wider than the screen", popup_place_below(&a1, 3000, 100, &screen, &out));
    ok_int(L"clamped to the screen width", out.right - out.left, 2560);
    ok_int(L"and moved to the left edge", out.left, 0);

    /* Taller than the screen: same. */
    ok(L"placed taller than the screen", popup_place_below(&a1, 100, 2000, &screen, &out));
    ok_int(L"clamped to the screen height", out.bottom - out.top, 1440);

    /* No room below the anchor: it flips above, which cannot happen with the
     * toolbar at the top today but is what keeps the rule usable if it moves. */
    const RECT a3 = r4(100, 1300, 220, 1400);
    ok(L"placed with no room below", popup_place_below(&a3, 240, 180, &screen, &out));
    ok_int(L"flipped above the anchor", out.bottom, 1300);
    ok_int(L"keeping its height", out.bottom - out.top, 180);

    /* Degenerate input is refused rather than guessed at. */
    ok(L"NULL anchor refused", !popup_place_below(NULL, 10, 10, &screen, &out));
    ok(L"NULL screen refused", !popup_place_below(&a1, 10, 10, NULL, &out));
    ok(L"NULL output refused", !popup_place_below(&a1, 10, 10, &screen, NULL));
    ok(L"zero width refused", !popup_place_below(&a1, 0, 10, &screen, &out));
    ok(L"negative height refused", !popup_place_below(&a1, 10, -5, &screen, &out));

    const RECT degenerate_screen = r4(10, 10, 10, 10);
    ok(L"an empty screen refused",
       !popup_place_below(&a1, 10, 10, &degenerate_screen, &out));
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t52_popup");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t52_popup - overlay pop-up panels (geometry and hit testing)");
    T_KV(L"run_at", L"%ls", t_now_string());
    T_NOTE(L"No window is created and no message is pumped: popup.c calls no");
    T_NOTE(L"Win32 function, so the awkward cases - a panel at the screen edge,");
    T_NOTE(L"a click on the seam - can be asserted here instead of being");
    T_NOTE(L"discovered on someone else's monitor.");

    test_state();
    test_hit();
    test_hover();
    test_placement();

    T_SEC(L"Result");
    T_KV(L"passed", L"%d", g_pass);
    T_KV(L"failed", L"%d", g_fail);

    if (g_fail == 0) T_KV(L"VERDICT", L"PASS - all %d assertions hold", g_pass);
    else             T_KV(L"VERDICT", L"FAIL - %d assertion(s) broken", g_fail);

    if (!no_pause) t_pause();
    t_log_close();
    return (g_fail == 0) ? 0 : 1;
}
