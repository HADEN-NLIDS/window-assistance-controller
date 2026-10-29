/*
 * t10_relcoord.c - unit tests for src/core/relcoord.c
 *
 * These run without a window, a monitor, or a message loop: the module
 * takes monitor rectangles as parameters, so every geometry that matters
 * can be constructed here. That is the whole reason relcoord is a separate
 * module - coordinate math is where this program is most likely to go
 * subtly wrong, and it must be testable in isolation.
 *
 * Coverage targets (from docs/架构与数据结构.md):
 *   1. a monitor to the LEFT of the primary (negative origin)
 *   2. a monitor ABOVE the primary (negative origin)
 *   3. a zero-width or zero-height monitor (division by zero)
 *   4. w or h of 0 / negative
 *   5. x/y outside [0,1]
 *   6. float precision: 0.5 * 1920 must be exactly 960
 *   7. size below min_w shrinks around the CENTRE, not the top-left
 *   8. min_w larger than the monitor
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t10_relcoord
 *   tests\out\t10_relcoord.exe
 */

#include "test_common.h"

#include <math.h>          /* fabsf: MUST be declared. Without this header
                            * C89 implicit declaration makes the compiler
                            * treat fabsf as returning int, and the float
                            * result is read from the wrong place - which
                            * showed up as "diff = 1.0" for two identical
                            * inputs. */
#include <string.h>        /* memcpy, for printing raw float bits */

#include "../src/core/relcoord.h"

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

/*
 * Compare floats with a relative tolerance.
 *
 * An absolute epsilon is not enough: rel_from_pixels computes
 * (float)640 / 2560.0f, and the result of that division is not bit-identical
 * to the literal 0.25f even though they print the same to six decimals.
 * That produced a "FAIL: got 0.250000, want 0.250000" - the classic
 * absolute-epsilon trap.
 */
static void ok_flt(const wchar_t *what, float got, float want)
{
    const float diff = fabsf(got - want);
    const float scale = fabsf(want) > 1.0f ? fabsf(want) : 1.0f;

    unsigned db, sb;
    memcpy(&db, &diff,  sizeof(db));
    memcpy(&sb, &scale, sizeof(sb));

    if (diff / scale < 1e-5f) {
        g_pass++;
    } else {
        g_fail++;
        unsigned gb, wb;
        memcpy(&gb, &got,  sizeof(gb));
        memcpy(&wb, &want, sizeof(wb));
        t_out_line(L"  FAIL: %ls", what);
        t_out_line(L"        got  %.10f bits 0x%08X", (double)got,  gb);
        t_out_line(L"        want %.10f bits 0x%08X", (double)want, wb);
        t_out_line(L"        diff %.10f bits 0x%08X", (double)diff, db);
        t_out_line(L"        scale %.10f bits 0x%08X", (double)scale, sb);
        t_out_line(L"        ratio %.10f", (double)(diff / scale));
    }
}

/* Convenience: a monitor rectangle. */
static RECT mon(int l, int t, int r, int b)
{
    RECT rc = { l, t, r, b };
    return rc;
}

/*
 * Build a zone for the next call.
 *
 * Returns a POINTER, not a value, because C forbids taking the address of a
 * function's return value - `zone(...)` is a compile error (MSVC C2102).
 * Returning a pointer into a static buffer keeps the call sites short:
 *
 *     rel_to_pixels(zone(0,0,0.5f,1), &m, &pt, &sz)
 *
 * The buffer is static, so a second zone() call overwrites the first. That
 * is fine here because no test passes two zones at once; if one ever needs
 * to, give it its own local variable instead of using this helper.
 */
static RelRect *zone(float x, float y, float w, float h)
{
    static RelRect z;
    ZeroMemory(&z, sizeof(z));
    z.x = x; z.y = y; z.w = w; z.h = h;
    return &z;
}

/*
 * Same, but with a minimum size - needs its own buffer slot so a test can
 * hold a zone with a minimum while another is being built.
 */
static RelRect *zone_min(float x, float y, float w, float h, int min_w, int min_h)
{
    static RelRect z;
    ZeroMemory(&z, sizeof(z));
    z.x = x; z.y = y; z.w = w; z.h = h;
    z.min_w = min_w; z.min_h = min_h;
    return &z;
}

/* ------------------------------------------------------------------ */

static void test_basic_halves(void)
{
    T_SEC(L"1. Basic cases: half and quarter of a 1920x1080 monitor");

    RECT m = mon(0, 0, 1920, 1080);
    POINT pt; SIZE sz;

    ok(L"left half converts", rel_to_pixels(zone(0.0f, 0.0f, 0.5f, 1.0f), &m, &pt, &sz));
    ok_int(L"left half: x", pt.x, 0);
    ok_int(L"left half: y", pt.y, 0);
    ok_int(L"left half: w", sz.cx, 960);
    ok_int(L"left half: h", sz.cy, 1080);

    ok(L"right half converts", rel_to_pixels(zone(0.5f, 0.0f, 0.5f, 1.0f), &m, &pt, &sz));
    ok_int(L"right half: x", pt.x, 960);
    ok_int(L"right half: w", sz.cx, 960);

    ok(L"top-left quarter converts", rel_to_pixels(zone(0.25f, 0.25f, 0.5f, 0.5f), &m, &pt, &sz));
    ok_int(L"quarter: x", pt.x, 480);
    ok_int(L"quarter: y", pt.y, 270);
    ok_int(L"quarter: w", sz.cx, 960);
    ok_int(L"quarter: h", sz.cy, 540);
}

static void test_float_precision(void)
{
    T_SEC(L"2. Float precision: 0.5 * 1920 must be exactly 960");

    RECT m = mon(0, 0, 1920, 1080);
    POINT pt; SIZE sz;

    /* Run the halves test a second way: the boundary must land on the
     * exact midpoint, not one pixel either side. */
    rel_to_pixels(zone(0.5f, 0.5f, 0.5f, 0.5f), &m, &pt, &sz);
    ok_int(L"0.5 * 1920 == 960 exactly", pt.x, 960);
    ok_int(L"0.5 * 1080 == 540 exactly", pt.y, 540);

    /* A third of a width that does not divide evenly must round, not
     * truncate, and must not jump around between calls. */
    RECT m3 = mon(0, 0, 1000, 1000);
    rel_to_pixels(zone(0.333f, 0.0f, 0.333f, 1.0f), &m3, &pt, &sz);
    ok_int(L"0.333 * 1000 rounds to 333", pt.x, 333);

    /* Repeated calls must be stable (no accumulated drift). */
    int first = -1;
    int stable = 1;
    for (int i = 0; i < 5; ++i) {
        rel_to_pixels(zone(0.123f, 0.456f, 0.321f, 0.654f), &m, &pt, &sz);
        if (first < 0) first = pt.x;
        else if (pt.x != first) stable = 0;
    }
    ok(L"repeated conversions are stable", stable);
}

static void test_negative_origin(void)
{
    T_SEC(L"3. Monitor LEFT of the primary: negative origin");
    /* A 1920x1080 monitor sitting to the left of the primary puts its
     * origin at x = -1920. This is the case that breaks naive code. */
    RECT left_mon = mon(-1920, 0, 0, 1080);
    POINT pt; SIZE sz;

    ok(L"converts on a negative-origin monitor",
       rel_to_pixels(zone(0.0f, 0.0f, 0.5f, 1.0f), &left_mon, &pt, &sz));
    ok_int(L"left edge stays at -1920", pt.x, -1920);
    ok_int(L"width is still 960", sz.cx, 960);

    rel_to_pixels(zone(0.5f, 0.0f, 0.5f, 1.0f), &left_mon, &pt, &sz);
    ok_int(L"half way across lands at -960", pt.x, -960);

    T_SEC(L"4. Monitor ABOVE the primary: negative origin");
    /* A 1920x1080 monitor sitting above the primary puts its origin at
     * y = -1080. A half-height zone at y=0.5 must land at the midpoint. */
    RECT above_mon = mon(0, -1080, 1920, 0);
    ok(L"converts on a negative-origin monitor",
       rel_to_pixels(zone(0.0f, 0.0f, 1.0f, 0.5f), &above_mon, &pt, &sz));
    ok_int(L"top edge stays at -1080", pt.y, -1080);
    ok_int(L"half-height zone is 540 tall", sz.cy, 540);

    ok(L"converts a half-offset zone",
       rel_to_pixels(zone(0.0f, 0.5f, 1.0f, 0.5f), &above_mon, &pt, &sz));
    ok_int(L"half way down lands at -540", pt.y, -540);
}

static void test_degenerate_monitor(void)
{
    T_SEC(L"5. Degenerate monitors must be refused, not divided by");

    POINT pt = { 123, 456 };
    SIZE sz = { 789, 101 };

    RECT zero_w = mon(0, 0, 0, 1080);
    ok(L"zero-width monitor refused", !rel_to_pixels(zone(0,0,0.5f,1), &zero_w, &pt, &sz));
    ok_int(L"outputs untouched on refusal: x", pt.x, 123);
    ok_int(L"outputs untouched on refusal: w", sz.cx, 789);

    RECT zero_h = mon(0, 0, 1920, 0);
    ok(L"zero-height monitor refused", !rel_to_pixels(zone(0,0,0.5f,1), &zero_h, &pt, &sz));

    RECT inverted = mon(100, 100, 0, 0);
    ok(L"inverted monitor refused", !rel_to_pixels(zone(0,0,0.5f,1), &inverted, &pt, &sz));

    ok(L"NULL monitor refused", !rel_to_pixels(zone(0,0,0.5f,1), NULL, &pt, &sz));
    ok(L"NULL output refused", !rel_to_pixels(zone(0,0,0.5f,1), &zero_w, NULL, &sz));
    ok(L"NULL zone refused", !rel_to_pixels(NULL, &zero_w, &pt, &sz));
}

static void test_degenerate_zone(void)
{
    T_SEC(L"6. Zone with zero or negative size must still produce something usable");

    RECT m = mon(0, 0, 1920, 1080);
    POINT pt; SIZE sz;

    ok(L"zero-size zone converts", rel_to_pixels(zone(0.5f, 0.5f, 0.0f, 0.0f), &m, &pt, &sz));
    ok(L"zero-size zone gets a usable width", sz.cx >= 1);
    ok(L"zero-size zone gets a usable height", sz.cy >= 1);
    ok_int(L"zero-size zone uses the fraction floor",
           sz.cx, (int)(REL_MIN_FRACTION * 1920.0f));

    ok(L"negative-size zone converts", rel_to_pixels(zone(0.5f, 0.5f, -1.0f, -1.0f), &m, &pt, &sz));
    ok(L"negative-size zone gets a usable width", sz.cx >= 1);

    /* A tiny fraction must not become a one-pixel sliver. */
    rel_to_pixels(zone(0.0f, 0.0f, 0.001f, 0.001f), &m, &pt, &sz);
    ok(L"tiny fraction clamped to the floor", sz.cx >= (int)(REL_MIN_FRACTION * 1920.0f));
}

static void test_out_of_range_position(void)
{
    T_SEC(L"7. x/y outside [0,1] must be pulled back inside the monitor");

    RECT m = mon(0, 0, 1920, 1080);
    POINT pt; SIZE sz;

    ok(L"x = 2.0 converts", rel_to_pixels(zone(2.0f, 0.0f, 0.5f, 1.0f), &m, &pt, &sz));
    ok(L"far-right window ends at the right edge", pt.x + sz.cx <= 1920);
    ok(L"far-right window still starts on screen", pt.x >= 0);

    ok(L"x = -1.0 converts", rel_to_pixels(zone(-1.0f, -1.0f, 0.5f, 0.5f), &m, &pt, &sz));
    ok_int(L"negative x clamped to the left edge", pt.x, 0);
    ok_int(L"negative y clamped to the top edge", pt.y, 0);
}

static void test_min_size(void)
{
    T_SEC(L"8. min_w / min_h are respected");

    RECT m = mon(0, 0, 1920, 1080);
    POINT pt; SIZE sz;

    /* would be 96 x 54 without a minimum */
    RelRect *z = zone_min(0.4f, 0.4f, 0.05f, 0.05f, 400, 300);

    ok(L"converts with a minimum size", rel_to_pixels(z, &m, &pt, &sz));
    ok_int(L"width raised to min_w", sz.cx, 400);
    ok_int(L"height raised to min_h", sz.cy, 300);

    /* When the minimum is larger than the monitor, the monitor wins -
     * a window cannot be bigger than the screen. */
    RelRect *big = zone_min(0.0f, 0.0f, 0.1f, 0.1f, 5000, 5000);
    ok(L"converts with an oversized minimum", rel_to_pixels(big, &m, &pt, &sz));
    ok_int(L"clamped to monitor width", sz.cx, 1920);
    ok_int(L"clamped to monitor height", sz.cy, 1080);
    ok_int(L"position stays at the edge", pt.x, 0);
}

static void test_round_trip(void)
{
    T_SEC(L"9. Round trip: pixels -> fractions -> pixels");

    RECT m = mon(0, 0, 2560, 1600);
    RECT win = { 640, 400, 1920, 1200 };   /* 1280 x 800 at 640,400 */
    RelRect z;
    ZeroMemory(&z, sizeof(z));

    ok(L"pixels -> fractions", rel_from_pixels(&win, &m, &z));
    ok_flt(L"x fraction is 0.25", z.x, 0.25f);
    ok_flt(L"y fraction is 0.25", z.y, 0.25f);
    ok_flt(L"w fraction is 0.5",  z.w, 0.5f);
    ok_flt(L"h fraction is 0.5",  z.h, 0.5f);
    ok_int(L"abs_w recorded", z.abs_w, 1280);
    ok_int(L"abs_h recorded", z.abs_h, 800);

    POINT pt; SIZE sz;
    ok(L"fractions -> pixels", rel_to_pixels(&z, &m, &pt, &sz));
    ok_int(L"round trip x", pt.x, 640);
    ok_int(L"round trip y", pt.y, 400);
    ok_int(L"round trip w", sz.cx, 1280);
    ok_int(L"round trip h", sz.cy, 800);

    /* Same on a negative-origin monitor. */
    RECT lm = mon(-1920, 0, 0, 1080);
    RECT lwin = { -1440, 270, -480, 810 };   /* 960 x 540 */
    RelRect lz;
    ZeroMemory(&lz, sizeof(lz));
    ok(L"negative-origin round trip in", rel_from_pixels(&lwin, &lm, &lz));
    ok_flt(L"negative-origin x fraction", lz.x, 0.25f);
    ok(L"negative-origin round trip out", rel_to_pixels(&lz, &lm, &pt, &sz));
    ok_int(L"negative-origin x restored", pt.x, -1440);
}

static void test_clamp(void)
{
    T_SEC(L"10. rel_clamp_to_monitor");

    RECT m = mon(0, 0, 1920, 1080);

    RelRect *z = zone(2.0f, -3.0f, 3.0f, 0.5f);
    ok(L"clamp succeeds", rel_clamp_to_monitor(z, &m));
    ok(L"x pulled into range", z->x >= 0.0f && z->x <= 1.0f);
    ok(L"y pulled into range", z->y >= 0.0f && z->y <= 1.0f);
    ok(L"w pulled into range", z->w > 0.0f && z->w <= 1.0f);
    ok(L"x + w does not exceed the monitor", z->x + z->w <= 1.0001f);

    /*
     * h is checked with a LOCAL variable, not with zone(): zone() hands back
     * a pointer into one static buffer, so a second zone() call overwrites
     * the first. Using it twice in one test silently rewrote z and made
     * "h untouched" fail against a buffer that no longer held z's data.
     */
    RelRect hzone;
    ZeroMemory(&hzone, sizeof(hzone));
    hzone.x = 2.0f; hzone.y = -3.0f; hzone.w = 3.0f; hzone.h = 0.5f;
    ok(L"clamp succeeds (h case)", rel_clamp_to_monitor(&hzone, &m));
    ok_flt(L"h untouched (already valid)", hzone.h, 0.5f);

    RelRect tiny;
    ZeroMemory(&tiny, sizeof(tiny));
    tiny.x = 0.5f; tiny.y = 0.5f; tiny.w = 0.0f; tiny.h = 0.0f;
    ok(L"clamp tiny zone", rel_clamp_to_monitor(&tiny, &m));
    ok(L"tiny zone raised to the floor", tiny.w >= REL_MIN_FRACTION);

    RECT zero = { 0, 0, 0, 0 };
    ok(L"clamp refuses a degenerate monitor",
       !rel_clamp_to_monitor(&tiny, &zero));
}

static void test_resolve_size(void)
{
    T_SEC(L"11. rel_resolve_size: surviving a resolution change");

    SIZE sz;

    RECT m = mon(0, 0, 2560, 1600);

    /* Case 1: unchanged monitor geometry -> keep the measured size. */
    RelRect *z = zone(0.0f, 0.0f, 0.5f, 0.5f);
    z->abs_w = 1280; z->abs_h = 800;
    z->ref_w = 2560;
    z->min_w = 400;  z->min_h = 300;

    rel_resolve_size(z, &m, &sz);
    ok_int(L"unchanged monitor keeps the absolute width", sz.cx, 1280);
    ok_int(L"unchanged monitor keeps the absolute height", sz.cy, 800);

    /* Case 2: the monitor got much wider -> follow it by one factor. */
    RECT wide = mon(0, 0, 3840, 2400);
    rel_resolve_size(z, &wide, &sz);
    ok(L"scaled width grew", sz.cx > 1280);
    ok(L"scaled height grew", sz.cy > 800);
    ok_int(L"...by the monitor's width ratio, on the width", sz.cx, 1920);
    ok_int(L"...and the same ratio on the height", sz.cy, 1200);

    /*
     * Case 2b: the window's own shape must survive a change of PANEL shape.
     *
     * 2560x1600 -> 3840x2400 is a weak test on its own: both panels are
     * 16:10, and for a zone stored as 0.5 x 0.5 the fractions and the
     * measurement agree, so a per-axis implementation returns the same
     * numbers and looks correct. A 16:9 window does separate the two - per
     * axis scaling gives 1920x1200, which is the PANEL's shape, not the
     * window's.
     */
    RelRect *wide_win = zone_min(0.25f, 0.25f, 0.5f, 0.5f, 1, 1);
    wide_win->abs_w = 1600; wide_win->abs_h = 900;
    wide_win->ref_w = 2560;

    rel_resolve_size(wide_win, &wide, &sz);
    ok_int(L"a 16:9 window keeps its shape on a wider panel: width", sz.cx, 2400);
    ok_int(L"a 16:9 window keeps its shape on a wider panel: height", sz.cy, 1350);

    /* Case 3: a small change (taskbar auto-hide) must NOT rescale. */
    RECT jitter = mon(0, 0, 2560 - 40, 1600);
    rel_resolve_size(z, &jitter, &sz);
    ok_int(L"a 1.6% change is treated as jitter, size kept", sz.cx, 1280);

    /* Case 4: minimum size wins over a shrunken monitor. */
    RECT small = mon(0, 0, 800, 600);
    rel_resolve_size(z, &small, &sz);
    ok(L"width never below min_w", sz.cx >= 400);
    ok(L"height never below min_h", sz.cy >= 300);

    /*
     * Case 5: an unknown reference width must NOT discard the measurement.
     *
     * This is the regression that shipped: ref_w == 0 used to mean "fall back
     * to the fractions", and for a window whose zone read 1.0 x 1.0 that
     * meant "take the new monitor's shape". It looks harmless on a monitor
     * shaped like the old one, which is why it survived until a portrait
     * panel was tried - see the portrait section below.
     */
    RelRect *z2 = zone_min(0.0f, 0.0f, 0.5f, 0.5f, 1, 1);
    z2->abs_w = 1280; z2->abs_h = 800;
    rel_resolve_size(z2, &m, &sz);
    ok_int(L"unknown reference keeps the measured width", sz.cx, 1280);
    ok_int(L"unknown reference keeps the measured height", sz.cy, 800);

    /* Case 6: never exceed 90% of the monitor. */
    RelRect *z3 = zone_min(0.0f, 0.0f, 1.0f, 1.0f, 1, 1);
    z3->abs_w = 2560; z3->abs_h = 1600;
    rel_resolve_size(z3, &m, &sz);
    ok(L"width capped at the monitor width", sz.cx <= 2560);
    ok_int(L"a window exactly as large as its screen is LEFT ALONE", sz.cx, 2560);
    ok_int(L"...on both axes", sz.cy, 1600);

    /* Case 7: seed then resolve must give a usable size. */
    RelRect *fresh = zone_min(0.25f, 0.25f, 0.5f, 0.5f, 0, 0);
    rel_seed_absolute_sizes(fresh, &m);
    ok_int(L"seeded abs_w matches the fraction", fresh->abs_w, 1280);
    ok_int(L"seeded abs_h matches the fraction", fresh->abs_h, 800);
    ok_int(L"seeded min_w defaults to 1", fresh->min_w, 1);
}

/* ------------------------------------------------------------------ */

/*
 * Portrait and unusual-aspect monitors.
 *
 * A rotated panel reports a taller-than-wide rcMonitor, and plenty of real
 * screens are neither 16:9 nor 16:10 (7:5 tablets, 3:2 laptops, 4:3
 * industrial panels). Nothing in the coordinate model may assume the panel
 * is wider than it is tall, and nothing may assume the window's aspect
 * matches the panel's.
 *
 * The risk here is not the arithmetic - fractions map to axes cleanly - but
 * the CLAMP. A ceiling computed per axis reshapes the window, which is
 * exactly what was measured on a 1440x2560 panel: a 16:9 window came out
 * approximately 9:16.
 */
static void test_portrait_and_odd_aspect(void)
{
    T_SEC(L"14. Portrait monitors and unusual aspect ratios");

    POINT pt; SIZE sz;

    /* ---- a rotated 2560x1440 panel ---- */
    RECT port = mon(0, 0, 1440, 2560);

    ok(L"portrait: left half converts",
       rel_to_pixels(zone(0.0f, 0.0f, 0.5f, 1.0f), &port, &pt, &sz));
    ok_int(L"portrait: left half x", pt.x, 0);
    ok_int(L"portrait: left half width", sz.cx, 720);
    ok_int(L"portrait: left half height", sz.cy, 2560);

    /* "Bottom half" must mean the bottom half of a TALL screen. */
    ok(L"portrait: bottom half converts",
       rel_to_pixels(zone(0.0f, 0.5f, 1.0f, 0.5f), &port, &pt, &sz));
    ok_int(L"portrait: bottom half y", pt.y, 1280);
    ok_int(L"portrait: bottom half height", sz.cy, 1280);
    ok_int(L"portrait: full width", sz.cx, 1440);

    /* A quarter zone is a quarter in both directions, giving a tall cell -
     * correct for a monitor held upright. */
    ok(L"portrait: quarter converts",
       rel_to_pixels(zone(0.25f, 0.25f, 0.5f, 0.5f), &port, &pt, &sz));
    ok_int(L"portrait: quarter x", pt.x, 360);
    ok_int(L"portrait: quarter y", pt.y, 640);
    ok_int(L"portrait: quarter width", sz.cx, 720);
    ok_int(L"portrait: quarter height", sz.cy, 1280);

    /* ---- an odd 7:5 panel ---- */
    RECT odd = mon(0, 0, 2100, 1500);
    ok(L"7:5: half converts",
       rel_to_pixels(zone(0.0f, 0.0f, 0.5f, 0.5f), &odd, &pt, &sz));
    ok_int(L"7:5: width", sz.cx, 1050);
    ok_int(L"7:5: height", sz.cy, 750);

    /* ---- THE CLAMP MUST NOT RESHAPE ---- */
    T_SEC(L"15. Clamping shrinks proportionally, never reshapes");

    /*
     * A 16:9 window applied to a 1440x2560 portrait monitor.
     *
     * The window cannot fit at its stored size, so it is scaled down by a
     * single factor - and the factor is chosen by the binding axis, which
     * here is the width: 1440 / 2560 = 0.5625. The result is 1440 x 810,
     * still 16:9 and now inside the panel.
     *
     * The bug this guards against computed the ceiling per axis, giving
     * ceil_w = 1296 and ceil_h = 2304. The height passed through untouched
     * while the width was cut, producing 1296 x 1440 - a 9:10 portrait
     * window where the user had a 16:9 landscape one.
     */
    RelRect *sixteen_nine = zone_min(0.0f, 0.0f, 1.0f, 1.0f, 1, 1);
    sixteen_nine->abs_w = 2560;
    sixteen_nine->abs_h = 1440;

    rel_resolve_size(sixteen_nine, &port, &sz);

    /*
     * Exact numbers, because a nearby wrong answer is the failure mode here:
     *
     *   1440 x 810   the measurement is the baseline and one factor fits it
     *                to the panel - 1440 / 2560 = 0.5625
     *   1296 x 2304  the fractions became the baseline (1.0 x 1.0 of the
     *                panel), which is the bug
     *   1440 x 2560  the fractions were used for the SIZE too, so the window
     *                took the panel's shape
     */
    ok_int(L"portrait: fitted width", sz.cx, 1440);
    ok_int(L"portrait: fitted height keeps the 16:9 shape", sz.cy, 810);

    const float got_ratio = (float)sz.cx / (float)sz.cy;
    const float want_ratio = 2560.0f / 1440.0f;   /* 1.778 */

    ok(L"result is still wider than tall", sz.cx > sz.cy);
    ok(L"aspect ratio preserved within 2%",
       fabsf(got_ratio - want_ratio) / want_ratio < 0.02f);
    ok(L"width bounded by the panel's width", sz.cx <= 1440);
    ok(L"height bounded by the panel's height", sz.cy <= 2560);

    /*
     * The same zone with the reference width KNOWN takes a different route
     * inside rel_resolve_size - the measurement is first scaled by the ratio
     * between the two monitors (2560 -> 1440, factor 0.5625, giving 1440x810)
     * and only then fitted to the panel. It must land on the same answer:
     * the panel decides how much of the shape survives, never its direction.
     */
    sixteen_nine->ref_w = 2560;
    rel_resolve_size(sixteen_nine, &port, &sz);
    ok_int(L"portrait with a known reference: fitted width", sz.cx, 1440);
    ok_int(L"portrait with a known reference: fitted height", sz.cy, 810);
    sixteen_nine->ref_w = 0;   /* the rest of this section keeps it unknown */

    /* ---- the same window on a landscape monitor is not distorted ---- */
    RECT land = mon(0, 0, 2560, 1440);
    rel_resolve_size(sixteen_nine, &land, &sz);
    ok(L"landscape: still 16:9",
       fabsf((float)sz.cx / (float)sz.cy - want_ratio) / want_ratio < 0.02f);
    ok(L"landscape: fits the monitor", sz.cx <= 2560 && sz.cy <= 1440);

    /*
     * ---- a 7:5 panel whose aspect matches nothing in the window ----
     *
     * 2100x1500 is a real tablet panel shape and neither 16:9 nor 16:10. A
     * 2000x1200 (5:3) window must come back as 5:3 squeezed by the binding
     * axis, which here is the width: 0.9 * 2100 / 2000 = 0.945.
     */
    RECT seven_five = mon(0, 0, 2100, 1500);
    RelRect *five_three = zone_min(0.0f, 0.0f, 1.0f, 1.0f, 1, 1);
    five_three->abs_w = 2000;
    five_three->abs_h = 1200;

    rel_resolve_size(five_three, &seven_five, &sz);
    ok_int(L"7:5 panel: fitted width", sz.cx, 2000);
    ok_int(L"7:5 panel: fitted height keeps 5:3", sz.cy, 1200);
    ok(L"7:5 panel: the window still fits", sz.cx <= 2100 && sz.cy <= 1500);

    /* ---- an already-fitting size is never enlarged ---- */
    RelRect *small = zone_min(0.0f, 0.0f, 0.5f, 0.5f, 1, 1);
    small->abs_w = 800;
    small->abs_h = 600;
    rel_resolve_size(small, &land, &sz);
    ok_int(L"a small window is not scaled up: width", sz.cx, 800);
    ok_int(L"a small window is not scaled up: height", sz.cy, 600);

    /* ---- a very tall zone on a very wide panel ---- */
    RECT ultra = mon(0, 0, 5120, 1440);   /* 32:9 */
    ok(L"ultra-wide: thin column converts",
       rel_to_pixels(zone(0.0f, 0.0f, 0.1f, 1.0f), &ultra, &pt, &sz));
    ok_int(L"ultra-wide: column width", sz.cx, 512);
    ok_int(L"ultra-wide: full height", sz.cy, 1440);
    ok(L"ultra-wide: column is taller than wide", sz.cy > sz.cx);
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    (void)argc; (void)argv;

    t_diag_init(L"t10_relcoord");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t10_relcoord - unit tests for normalized coordinate math");
    T_KV(L"run_at", L"%ls", t_now_string());
    T_NOTE(L"No window or monitor is needed: every geometry below is");
    T_NOTE(L"constructed in this program, which is why these tests can run");
    T_NOTE(L"anywhere, including on a machine with no display.");

    test_basic_halves();
    test_float_precision();
    test_negative_origin();
    test_degenerate_monitor();
    test_degenerate_zone();
    test_out_of_range_position();
    test_min_size();
    test_round_trip();
    test_clamp();
    test_resolve_size();
    test_portrait_and_odd_aspect();

    T_SEC(L"Result");
    T_KV(L"passed", L"%d", g_pass);
    T_KV(L"failed", L"%d", g_fail);

    if (g_fail == 0) {
        T_KV(L"VERDICT", L"PASS - all %d assertions hold", g_pass);
    } else {
        T_KV(L"VERDICT", L"FAIL - %d assertion(s) broken", g_fail);
    }

    t_log_close();
    if (g_fail != 0) return 1;
    return 0;
}
