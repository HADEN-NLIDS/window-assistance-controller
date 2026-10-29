/*
 * t72_placeholder.c - unit tests for src/ui/placeholder.c
 *
 * WHY THIS FILE CAN EXIST AT ALL
 *
 *   A placeholder box is drawn by overlay.c on a real screen, but WHERE it goes
 *   and what fits inside it is arithmetic - and that arithmetic is what goes
 *   wrong on a screen nobody tested on: a box in the corner, a box too small
 *   for its own label, a zone that projects half off the edge.
 *
 *   placeholder.c calls no Win32 function (see placeholder.h), so this test
 *   links no window, needs no screen, and passes on any machine.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t72_placeholder
 *   tests\out\t72_placeholder.exe --no-pause
 */

#include "test_common.h"

#include "../src/ui/placeholder.h"

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

static RelRect z4(float x, float y, float w, float h)
{
    RelRect z;
    ZeroMemory(&z, sizeof(z));
    z.x = x; z.y = y; z.w = w; z.h = h;
    return z;
}

static POINT p2(int x, int y)
{
    POINT p;
    p.x = x; p.y = y;
    return p;
}

/* Fractions and their round trips go through float, so a pixel either way is
 * the arithmetic, not a bug - the same tolerance t10_relcoord uses. */
static void ok_near(const wchar_t *what, float got, float want)
{
    const float d = got - want;
    if (d < 0.0005f && d > -0.0005f) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls  (got %.5f, want %.5f)", what, got, want);
    }
}

static void ok_near_int(const wchar_t *what, int got, int want, int tol)
{
    const int d = (got > want) ? (got - want) : (want - got);
    if (d <= tol) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls  (got %d, want %d +/-%d)", what, got, want, tol);
    }
}

/* The canvas the edit overlay works in: one 2560x1440 screen below a 64-px
 * toolbar strip, which is what overlay.c passes down. */
static RECT area_std(void) { return r4(0, 64, 2560, 1440); }

/* ------------------------------------------------------------------ */

static void test_metrics(void)
{
    T_SEC(L"1. metrics: one dpi in, every length out");

    PlaceholderMetrics m;
    placeholder_metrics(96, &m);
    ok_int(L"padding 8", m.pad, 8);
    ok_int(L"icon 20", m.icon, 20);
    ok_int(L"line 20", m.line_h, 20);

    PlaceholderMetrics m150;
    placeholder_metrics(144, &m150);
    ok_int(L"150%: padding 12", m150.pad, 12);
    ok_int(L"150%: icon 30", m150.icon, 30);
    ok_int(L"150%: line 30", m150.line_h, 30);

    PlaceholderMetrics bad;
    placeholder_metrics(0, &bad);
    ok_int(L"dpi 0 falls back to 96", bad.icon, 20);
    placeholder_metrics(100000, &bad);
    ok_int(L"an absurd dpi falls back to 96", bad.icon, 20);

    placeholder_metrics(96, NULL);      /* must not crash */
    ok(L"NULL metrics output is ignored", 1);
}

static void test_rect(void)
{
    T_SEC(L"2. a zone becomes a box, on this screen");

    const RECT area = area_std();       /* 2560 x 1376 */

    RECT b;
    RelRect zone = z4(0.0f, 0.0f, 0.5f, 1.0f);
    ok(L"the left half has a box", placeholder_rect(&zone, &area, &b));
    ok_int(L"from the left edge", b.left, 0);
    ok_int(L"below the toolbar", b.top, 64);
    ok_int(L"half the width", b.right, 1280);
    ok_int(L"down to the bottom", b.bottom, 1440);

    zone = z4(0.5f, 0.0f, 0.5f, 0.5f);
    ok(L"the top-right quarter has a box", placeholder_rect(&zone, &area, &b));
    ok_int(L"starting at the middle", b.left, 1280);
    ok_int(L"half the height", b.bottom - b.top, 688);

    /* A zone that runs off the canvas is CLAMPED for drawing. The zone itself
     * still says what gets placed - this only decides what is on screen, and a
     * box you cannot see is worse than one that is trimmed. */
    zone = z4(0.9f, 0.9f, 0.5f, 0.5f);
    ok(L"an off-screen zone still draws", placeholder_rect(&zone, &area, &b));
    ok_int(L"clamped at the right edge", b.right, 2560);
    ok_int(L"clamped at the bottom", b.bottom, 1440);
    ok(L"and it is still a real box", b.right > b.left && b.bottom > b.top);

    /* Degenerate input is refused rather than guessed at. */
    zone = z4(0.0f, 0.0f, 0.0f, 0.0f);
    ok(L"a zero-sized zone has no box", !placeholder_rect(&zone, &area, &b));
    zone = z4(0.5f, 0.5f, 0.5f, 0.5f);
    ok(L"NULL area refused", !placeholder_rect(&zone, NULL, &b));
    ok(L"NULL output refused", !placeholder_rect(&zone, &area, NULL));
    ok(L"NULL zone refused", !placeholder_rect(NULL, &area, &b));
    const RECT empty = r4(10, 10, 10, 10);
    ok(L"an empty canvas refused", !placeholder_rect(&zone, &empty, &b));
}

static void test_fit(void)
{
    T_SEC(L"3. what fits inside a box");

    PlaceholderMetrics m;
    placeholder_metrics(96, &m);

    /* Big box: everything has room. */
    const RECT big = r4(0, 64, 1280, 1440);
    RECT ir, nr, sr;

    ok(L"a half-screen box is worth drawing", placeholder_is_visible(&big));
    ok(L"the icon fits", placeholder_icon_rect(&m, &big, &ir));
    ok_int(L"in the top-left corner, after the padding", ir.left, 8);
    ok_int(L"on the box's top row", ir.top, 72);
    ok_int(L"one icon square", ir.right - ir.left, 20);

    ok(L"the name fits", placeholder_name_rect(&m, &big, &nr));
    ok_int(L"to the right of the icon", nr.left, 36);
    ok_int(L"on the icon's row, so they line up", nr.top, ir.top);
    ok_int(L"and the same height", nr.bottom - nr.top, ir.bottom - ir.top);
    ok_int(L"reaching the right padding", nr.right, 1272);

    ok(L"the size readout fits", placeholder_size_rect(&m, &big, &sr));
    ok_int(L"under the icon", sr.top, 94);
    ok_int(L"as wide as the box's padding allows", sr.right, 1272);
    ok_int(L"one line tall", sr.bottom - sr.top, 20);

    /* Exactly at the floor: still drawn. */
    const RECT floor_box = r4(0, 0, PLACEHOLDER_MIN_W, PLACEHOLDER_MIN_H);
    ok(L"a box exactly at the minimum is drawn",
       placeholder_is_visible(&floor_box));

    /* One pixel under it: not drawn. A scribble is not a smaller box. */
    const RECT too_small = r4(0, 0, PLACEHOLDER_MIN_W - 1, PLACEHOLDER_MIN_H);
    ok(L"one pixel narrower is not", !placeholder_is_visible(&too_small));

    /* A short box keeps the name and trims the readout, rather than drawing
     * the size over its own border. */
    const RECT squat = r4(0, 0, 200, 48);
    ok(L"a squat box is still drawn", placeholder_is_visible(&squat));
    ok(L"its name fits", placeholder_name_rect(&m, &squat, &nr));
    ok(L"its readout fits too", placeholder_size_rect(&m, &squat, &sr));
    ok_int(L"but is trimmed at the bottom", sr.bottom, 40);
    ok(L"and is still not empty", sr.bottom > sr.top);

    /* Too short for even the icon: nothing is claimed to fit. */
    const RECT sliver = r4(0, 0, 200, 20);
    ok(L"a 20-px-tall box draws nothing", !placeholder_icon_rect(&m, &sliver, &ir));

    /* NULL and empty are refused throughout. */
    ok(L"NULL metrics refused", !placeholder_icon_rect(NULL, &big, &ir));
    ok(L"NULL box refused", !placeholder_icon_rect(&m, NULL, &ir));
    ok(L"NULL output refused", !placeholder_icon_rect(&m, &big, NULL));
    const RECT empty = r4(5, 5, 5, 5);
    ok(L"an empty box refused", !placeholder_is_visible(&empty));
    ok(L"and has no name line", !placeholder_name_rect(&m, &empty, &nr));
}

/* ------------------------------------------------------------------ */

static void test_editing(void)
{
    T_SEC(L"4. moving and resizing a box");

    PlaceholderMetrics m;
    placeholder_metrics(96, &m);
    const RECT area = area_std();              /* (0,64)-(2560,1440) */
    const RECT box  = r4(200, 200, 600, 500);  /* 400 x 300 */

    /* ---- the grip ---- */
    RECT g;
    ok(L"the grip exists", placeholder_grip_rect(&m, &box, &g));
    ok_int(L"at the right edge", g.right, 600);
    ok_int(L"at the bottom edge", g.bottom, 500);
    ok_int(L"14 px square", g.right - g.left, 14);
    ok_int(L"and as tall as it is wide", g.bottom - g.top, 14);

    /* A box too small to hold a full grip: it shrinks rather than covering the
     * name and the size - the two things the box exists to show. */
    const RECT tiny = r4(0, 0, 30, 24);
    ok(L"a small box still has a grip", placeholder_grip_rect(&m, &tiny, &g));
    ok(L"but it takes at most a third of it", g.right - g.left <= 30 / 3);

    /* ---- what is under the pointer ---- */
    const RECT boxes[3] = {
        r4(0, 0, 100, 100),      /* 0 */
        r4(200, 200, 400, 400),  /* 1 */
        r4(50, 50, 150, 150)     /* 2 - drawn last, so it is on top */
    };
    PlaceholderHit h;

    h = placeholder_hit(boxes, 3, &m, p2(20, 20));
    ok_int(L"inside box 0 only", (int)h.kind, (int)PLACEHOLDER_HIT_BODY);
    ok_int(L"and it names that box", h.index, 0);

    h = placeholder_hit(boxes, 3, &m, p2(75, 75));
    ok_int(L"where two overlap, the top one wins", h.index, 2);

    h = placeholder_hit(boxes, 3, &m, p2(145, 145));
    ok_int(L"the grip is asked before the body", (int)h.kind, (int)PLACEHOLDER_HIT_GRIP);
    ok_int(L"and it belongs to that box", h.index, 2);

    h = placeholder_hit(boxes, 3, &m, p2(900, 900));
    ok_int(L"empty canvas is nothing", (int)h.kind, (int)PLACEHOLDER_HIT_NONE);
    ok_int(L"with no index", h.index, -1);

    ok_int(L"no boxes at all is nothing",
           (int)placeholder_hit(NULL, 0, &m, p2(0, 0)).kind,
           (int)PLACEHOLDER_HIT_NONE);

    /* ---- moving ---- */
    RECT moved;
    ok(L"it moves", placeholder_move_rect(&box, 50, -30, &area, &moved));
    ok_int(L"by the delta", moved.left, 250);
    ok_int(L"and keeps its size", moved.right - moved.left, 400);

    /* Past the edge it stops, rather than vanishing or being refused - which
     * is what every window on the desktop does. */
    ok(L"a huge move still lands somewhere",
       placeholder_move_rect(&box, -99999, -99999, &area, &moved));
    ok_int(L"pinned to the left edge", moved.left, area.left);
    ok_int(L"pinned to the top edge", moved.top, area.top);
    ok_int(L"and it kept its size", moved.right - moved.left, 400);

    ok(L"and the other way too",
       placeholder_move_rect(&box, 99999, 99999, &area, &moved));
    ok_int(L"pinned to the right edge", moved.right, area.right);
    ok_int(L"pinned to the bottom", moved.bottom, area.bottom);

    /* ---- resizing ---- */
    RECT resized;
    ok(L"it resizes", placeholder_resize_rect(&box, p2(800, 700), &area, 40, 30, &resized));
    ok_int(L"the left edge does not move", resized.left, 200);
    ok_int(L"nor the top", resized.top, 200);
    ok_int(L"the right follows the pointer", resized.right, 800);
    ok_int(L"and so does the bottom", resized.bottom, 700);

    ok(L"a drag back past the corner still resizes",
       placeholder_resize_rect(&box, p2(0, 0), &area, 40, 30, &resized));
    ok_int(L"but not below the minimum width", resized.right - resized.left, 40);
    ok_int(L"nor the minimum height", resized.bottom - resized.top, 30);

    /* ---- and back to a zone ---- */
    RelRect z;
    ok(L"a box becomes a zone", placeholder_zone_from_rect(&box, &area, &z));
    ok_near(L"x is the fraction across", z.x, 200.0f / 2560.0f);
    ok_near(L"y counts from the canvas top, not the screen top",
            z.y, (200.0f - 64.0f) / 1376.0f);
    ok_near(L"w is the fraction wide", z.w, 400.0f / 2560.0f);
    ok_near(L"h is the fraction tall", z.h, 300.0f / 1376.0f);
    ok_int(L"with the minimum the placement path expects", z.min_w, 1);

    /*
     * The round trip. A zone that is drawn and read back must come out the same
     * box: if it did not, dragging a box would move it by a pixel every time,
     * and a layout would drift while the user "did not change anything".
     */
    RECT again;
    ok(L"the zone draws back to a box", placeholder_rect(&z, &area, &again));
    ok_near_int(L"the same left", again.left, box.left, 1);
    ok_near_int(L"the same top", again.top, box.top, 1);
    ok_near_int(L"the same right", again.right, box.right, 1);
    ok_near_int(L"the same bottom", again.bottom, box.bottom, 1);

    /* Degenerate input is refused rather than guessed at. */
    ok(L"NULL box refused", !placeholder_move_rect(NULL, 1, 1, &area, &moved));
    ok(L"NULL area refused", !placeholder_move_rect(&box, 1, 1, NULL, &moved));
    ok(L"NULL output refused", !placeholder_move_rect(&box, 1, 1, &area, NULL));
    ok(L"a degenerate zone has no box",
       !placeholder_zone_from_rect(&box, &area, NULL));
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t72_placeholder");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t72_placeholder - the placeholder boxes (projection, fit, labels)");
    T_KV(L"run_at", L"%ls", t_now_string());
    T_NOTE(L"No window is created: placeholder.c calls no Win32 function, so the");
    T_NOTE(L"awkward cases - a zone falling off the canvas, a box too small for");
    T_NOTE(L"its own label - are assertions here instead of surprises on screen.");

    test_metrics();
    test_rect();
    test_fit();
    test_editing();

    T_SEC(L"Result");
    T_KV(L"passed", L"%d", g_pass);
    T_KV(L"failed", L"%d", g_fail);

    if (g_fail == 0) T_KV(L"VERDICT", L"PASS - all %d assertions hold", g_pass);
    else             T_KV(L"VERDICT", L"FAIL - %d assertion(s) broken", g_fail);

    if (!no_pause) t_pause();
    t_log_close();
    return (g_fail == 0) ? 0 : 1;
}
