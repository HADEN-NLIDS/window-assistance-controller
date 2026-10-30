/*
 * t74_layout.c - unit tests for src/ui/snapgrid.c: the 1+2 and 2+1 templates,
 * and the rule that picks the region a dragged box would fill.
 *
 * WHY THIS FILE CAN EXIST AT ALL
 *
 *   Both are arithmetic on a monitor size. snapgrid_build() produces the
 *   regions a template describes, and snapgrid_pick() answers "which of these
 *   is the pointer nearest to" - the check that makes "drag a box to the edge,
 *   release, it fills the region" predictable instead of a surprise during a
 *   drag.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t74_layout
 *   tests\out\t74_layout.exe --no-pause
 */

#include "test_common.h"

#include "../src/ui/snapgrid.h"

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

static void ok_near(const wchar_t *what, float got, float want)
{
    const float d = (got > want) ? (got - want) : (want - got);
    if (d < 0.0005f) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls  (got %.4f, want %.4f)", what, got, want);
    }
}

static POINT p2(int x, int y)
{
    POINT p;
    p.x = x;
    p.y = y;
    return p;
}

static RelRect z4(float x, float y, float w, float h)
{
    RelRect z;
    ZeroMemory(&z, sizeof(z));
    z.x = x; z.y = y; z.w = w; z.h = h;
    return z;
}

static void check_region(const wchar_t *what, const RelRect *got,
                         float x, float y, float w, float h)
{
    ok_near(what, got->x, x);
    ok_near(what, got->y, y);
    ok_near(what, got->w, w);
    ok_near(what, got->h, h);
}

/* ------------------------------------------------------------------ */
/* The templates                                                       */
/* ------------------------------------------------------------------ */

static void test_one_two(void)
{
    T_SEC(L"1+2: one full-height column on the left, two regions on the right");

    RelRect out[SNAP_MAX_REGIONS];
    const int n = snapgrid_build(SNAP_ONE_TWO, 0, 1000, 1000, out, SNAP_MAX_REGIONS);
    ok_int(L"three regions", n, 3);
    if (n == 3) {
        check_region(L"left column", &out[0], 0.0f, 0.0f, 0.5f, 1.0f);
        check_region(L"top right", &out[1], 0.5f, 0.0f, 0.5f, 0.5f);
        check_region(L"bottom right", &out[2], 0.5f, 0.5f, 0.5f, 0.5f);
    }

    /* With a gap, the column still spans the whole height: the gap between the
     * two right-hand cells is inside the right column, not across the left. */
    const int g = snapgrid_build(SNAP_ONE_TWO, 8, 1000, 1000, out, SNAP_MAX_REGIONS);
    ok_int(L"three regions with a gap", g, 3);
    if (g == 3) {
        ok_near(L"left column height reaches the bottom", out[0].y + out[0].h, 1.0f);
        ok_near(L"left column width loses the gap", out[0].w, 0.496f);
        ok_near(L"right cells leave the gap", out[2].y - (out[1].y + out[1].h),
                (float)8 / 1000.0f);
    }

    /* A monitor smaller than the cap: same answer, and never a partial write. */
    const int small = snapgrid_build(SNAP_ONE_TWO, 0, 0, 0, out, SNAP_MAX_REGIONS);
    ok_int(L"degenerate monitor gives no regions", small, 0);

    /* cap too small: refuse, rather than write half the layout. */
    const int capped = snapgrid_build(SNAP_ONE_TWO, 0, 1000, 1000, out, 2);
    ok_int(L"cap below three refuses", capped, 0);
}

static void test_two_one(void)
{
    T_SEC(L"2+1: two regions on the left, one full-height column on the right");

    RelRect out[SNAP_MAX_REGIONS];
    const int n = snapgrid_build(SNAP_TWO_ONE, 0, 1000, 1000, out, SNAP_MAX_REGIONS);
    ok_int(L"three regions", n, 3);
    if (n == 3) {
        check_region(L"top left", &out[0], 0.0f, 0.0f, 0.5f, 0.5f);
        check_region(L"bottom left", &out[1], 0.0f, 0.5f, 0.5f, 0.5f);
        check_region(L"right column", &out[2], 0.5f, 0.0f, 0.5f, 1.0f);
    }

    const int g = snapgrid_build(SNAP_TWO_ONE, 8, 1000, 1000, out, SNAP_MAX_REGIONS);
    ok_int(L"three regions with a gap", g, 3);
    if (g == 3) {
        ok_near(L"right column height reaches the bottom", out[2].y + out[2].h, 1.0f);
        ok_near(L"right column width loses the gap", out[2].w, 0.496f);
        ok_near(L"left cells leave the gap", out[1].y - (out[0].y + out[0].h),
                (float)8 / 1000.0f);
    }
}

static void test_names_and_labels(void)
{
    T_SEC(L"Names and labels");

    ok(L"1+2 name", wcscmp(snapgrid_name(SNAP_ONE_TWO), L"1+2") == 0);
    ok(L"2+1 name", wcscmp(snapgrid_name(SNAP_TWO_ONE), L"2+1") == 0);
    ok(L"custom has no regions", snapgrid_build(SNAP_CUSTOM, 0, 1000, 1000,
                                               NULL, 0) == 0);

    /* The panel lists templates in SnapTemplate order and asks each for its
     * caption; the two new ones must not fall back to the generic label. */
    ok(L"1+2 label", snapgrid_label_id(SNAP_ONE_TWO) == STR_TB_TPL_1P2);
    ok(L"2+1 label", snapgrid_label_id(SNAP_TWO_ONE) == STR_TB_TPL_2P1);
}

/* ------------------------------------------------------------------ */
/* Picking the region a dragged box would fill                         */
/* ------------------------------------------------------------------ */

static void test_edge_hit(void)
{
    T_SEC(L"Only a screen edge triggers the fill");

    const int W = 1000;
    const int H = 1000;

    ok(L"a point 10 px from the left edge",
       snapgrid_edge_hit(W, H, p2(10, 500), SNAP_EDGE_BAND_PX));
    ok(L"a point 10 px from the right edge",
       snapgrid_edge_hit(W, H, p2(990, 500), SNAP_EDGE_BAND_PX));
    ok(L"a point 10 px from the top edge",
       snapgrid_edge_hit(W, H, p2(500, 10), SNAP_EDGE_BAND_PX));
    ok(L"a point 10 px from the bottom edge",
       snapgrid_edge_hit(W, H, p2(500, 990), SNAP_EDGE_BAND_PX));

    /* The report that made this test exist: a pointer merely INSIDE a region
     * used to pick it. It must not. */
    ok(L"the middle of a region is not an edge",
       !snapgrid_edge_hit(W, H, p2(500, 500), SNAP_EDGE_BAND_PX));
    ok(L"100 px in is not an edge",
       !snapgrid_edge_hit(W, H, p2(100, 500), SNAP_EDGE_BAND_PX));
    ok(L"outside the screen is not its edge",
       !snapgrid_edge_hit(W, H, p2(-5, 500), SNAP_EDGE_BAND_PX));
    ok(L"a band of zero accepts nothing",
       !snapgrid_edge_hit(W, H, p2(0, 500), 0));
    ok(L"degenerate screen",
       !snapgrid_edge_hit(0, 0, p2(0, 0), SNAP_EDGE_BAND_PX));
}

static void test_pick(void)
{
    T_SEC(L"Nearest region for the pointer");

    const int  W = 1000;
    const int  H = 1000;

    RelRect zones[3];
    zones[0] = z4(0.0f, 0.0f, 0.5f, 1.0f);   /* left column   */
    zones[1] = z4(0.5f, 0.0f, 0.5f, 0.5f);   /* top right     */
    zones[2] = z4(0.5f, 0.5f, 0.5f, 0.5f);   /* bottom right  */

    /* Inside wins over distance, whatever the distance would be. */
    ok_int(L"inside the left column", snapgrid_pick(zones, 3, W, H, p2(100, 500),
                                                    SNAP_FILL_MAX_FRAC), 0);
    ok_int(L"inside the top right", snapgrid_pick(zones, 3, W, H, p2(700, 200),
                                                  SNAP_FILL_MAX_FRAC), 1);
    ok_int(L"inside the bottom right", snapgrid_pick(zones, 3, W, H, p2(700, 700),
                                                     SNAP_FILL_MAX_FRAC), 2);

    /*
     * The user's example: a 360X box dragged to the upper part of the RIGHT
     * screen edge. The pointer is at the edge, in the top right region, so the
     * box goes there - 0.5 x 0.5 in the top right corner.
     */
    ok_int(L"dragged to the upper right edge", snapgrid_pick(zones, 3, W, H,
                                                             p2(999, 200),
                                                             SNAP_FILL_MAX_FRAC), 1);
    ok_int(L"dragged to the lower right edge", snapgrid_pick(zones, 3, W, H,
                                                             p2(999, 800),
                                                             SNAP_FILL_MAX_FRAC), 2);
    ok_int(L"dragged to the left edge", snapgrid_pick(zones, 3, W, H,
                                                      p2(1, 500),
                                                      SNAP_FILL_MAX_FRAC), 0);

    /*
     * The limit: 0.1 x the shorter side = 100 px. A pointer 100 px away still
     * counts; 101 px does not.
     */
    RelRect corner[1];
    corner[0] = z4(0.0f, 0.0f, 0.2f, 0.2f);   /* 0..200 in pixels */

    ok_int(L"100 px from the region still counts",
           snapgrid_pick(corner, 1, W, H, p2(299, 100), SNAP_FILL_MAX_FRAC), 0);
    ok_int(L"a point just outside the corner is inside the limit",
           snapgrid_pick(corner, 1, W, H, p2(200, 200), SNAP_FILL_MAX_FRAC), 0);
    ok_int(L"101 px away is too far",
           snapgrid_pick(corner, 1, W, H, p2(300, 100), SNAP_FILL_MAX_FRAC), -1);
    ok_int(L"far away is no pick",
           snapgrid_pick(corner, 1, W, H, p2(900, 900), SNAP_FILL_MAX_FRAC), -1);

    /* Degenerate inputs. */
    ok_int(L"no regions", snapgrid_pick(NULL, 0, W, H, p2(0, 0),
                                        SNAP_FILL_MAX_FRAC), -1);
    ok_int(L"degenerate screen", snapgrid_pick(zones, 3, 0, 0, p2(0, 0),
                                               SNAP_FILL_MAX_FRAC), -1);

    /* A zero limit means "inside only". */
    ok_int(L"zero limit refuses a point outside",
           snapgrid_pick(corner, 1, W, H, p2(300, 100), 0.0f), -1);
    ok_int(L"zero limit still accepts a point inside",
           snapgrid_pick(corner, 1, W, H, p2(100, 100), 0.0f), 0);
}

int wmain(int argc, wchar_t **argv)
{
    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t74_layout");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t74_layout - 1+2 / 2+1 geometry and the nearest-region rule");
    T_KV(L"run_at", L"%ls", t_now_string());
    T_NOTE(L"No window is created: both things under test are arithmetic on the");
    T_NOTE(L"rectangles they are handed.");

    test_one_two();
    test_two_one();
    test_names_and_labels();
    test_edge_hit();
    test_pick();

    T_SEC(L"Result");
    T_KV(L"passed", L"%d", g_pass);
    T_KV(L"failed", L"%d", g_fail);

    if (g_fail == 0) T_KV(L"VERDICT", L"PASS - all %d assertions hold", g_pass);
    else             T_KV(L"VERDICT", L"FAIL - %d assertion(s) broken", g_fail);

    if (!no_pause) t_pause();
    t_log_close();
    return (g_fail == 0) ? 0 : 1;
}
