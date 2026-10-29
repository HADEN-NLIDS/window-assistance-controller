/*
 * t12_uiformat.c - unit tests for src/ui/uiformat.c
 *
 * The preset editor takes a zone as TEXT. Everything a user might reasonably
 * type - "50", "50%", "50,5", " 50.5 ", or a typo - goes through these
 * functions, and a wrong answer here becomes a saved preset that puts a window
 * somewhere the user did not ask for.
 *
 * The tests therefore concentrate on the cases where a lenient parser would
 * quietly produce a NUMBER instead of refusing: "50abc" must not become 50,
 * "23,5" must not become 23, and "" must not become 0.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t12_uiformat
 *   tests\out\t12_uiformat.exe --no-pause
 */

#include "test_common.h"

/* fabs() lives here. Omitting this is TRAPS.md #9: an implicitly
 * declared fabs() is assumed to return int, so a comparison of two identical
 * doubles still reports a difference. The first run of this very file failed
 * 20 assertions that way - "got 0.500000, want 0.500000" and FAIL. */
#include <math.h>

#include "../src/ui/uiformat.h"
#include "../src/ui/preview.h"
#include "../src/ui/pagelist.h"

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

static void ok_str(const wchar_t *what, const wchar_t *got, const wchar_t *want)
{
    if (got != NULL && want != NULL && wcscmp(got, want) == 0) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls  (got \"%ls\", want \"%ls\")",
                   what, got ? got : L"(null)", want ? want : L"(null)");
    }
}

/* Parse and compare against a fraction, within a tolerance. */
static void ok_parsed(const wchar_t *input, double want)
{
    double got = -1.0;
    const BOOL did = uiformat_parse_percent(input, &got);

    if (did && fabs(got - want) < 1.0e-9) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: parse \"%ls\"  (ok=%d, got %.6f, want %.6f)",
                   input, (int)did, got, want);
    }
}

static void ok_rejected(const wchar_t *input)
{
    double got = 12345.0;
    const BOOL did = uiformat_parse_percent(input, &got);

    if (!did && got == 12345.0) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: \"%ls\" should be refused (ok=%d, got %.6f)",
                   input, (int)did, got);
    }
}

/* ------------------------------------------------------------------ */

static void test_parse_percent(void)
{
    T_SEC(L"percent parsing - accepted");

    ok_parsed(L"0",      0.0);
    ok_parsed(L"50",     0.5);
    ok_parsed(L"100",    1.0);
    ok_parsed(L"23.5",   0.235);
    ok_parsed(L"23,5",   0.235);      /* comma decimal separator */
    ok_parsed(L"50%",    0.5);
    ok_parsed(L"  50  ", 0.5);        /* surrounding spaces */
    ok_parsed(L"50 %",   0.5);
    ok_parsed(L"+50",    0.5);
    ok_parsed(L".5",     0.005);      /* ".5" is 0.5 PERCENT, not 50% */
    ok_parsed(L"0.1",    0.001);
    ok_parsed(L"99.9",   0.999);
}

static void test_parse_percent_rejected(void)
{
    T_SEC(L"percent parsing - refused (a lenient parser would guess here)");

    ok_rejected(L"");
    ok_rejected(L"   ");
    ok_rejected(L"%");
    ok_rejected(L".");
    ok_rejected(L"+");
    ok_rejected(L"-50");              /* negative: not a screen position */
    ok_rejected(L"50abc");            /* trailing junk must not be ignored */
    ok_rejected(L"abc");
    ok_rejected(L"50.5.5");
    ok_rejected(L"101");              /* past the right edge */
    ok_rejected(L"-1");
    ok_rejected(L"1e5");              /* exponent notation is not offered */
    ok_rejected(L"50%%");
    ok_rejected(NULL);
}

static void test_parse_leaves_output_alone(void)
{
    T_SEC(L"refusal must not write a zero");

    /* Keeping the previous value is what lets the editor leave a bad field
     * untouched instead of silently storing 0 - which would be a valid zone
     * and therefore impossible to notice. */
    double v = 0.42;
    ok(L"refused input reports FALSE", !uiformat_parse_percent(L"oops", &v));
    ok(L"and the old value survives", fabs(v - 0.42) < 1.0e-9);
}

static void test_format_percent(void)
{
    T_SEC(L"percent formatting");

    wchar_t buf[64];

    uiformat_format_percent(0.5, buf, 64);      ok_str(L"0.5 -> 50",     buf, L"50");
    uiformat_format_percent(1.0, buf, 64);      ok_str(L"1.0 -> 100",    buf, L"100");
    uiformat_format_percent(0.0, buf, 64);      ok_str(L"0.0 -> 0",      buf, L"0");
    uiformat_format_percent(0.235, buf, 64);    ok_str(L"0.235 -> 23.5", buf, L"23.5");
    uiformat_format_percent(0.001, buf, 64);    ok_str(L"0.001 -> 0.1",  buf, L"0.1");
    uiformat_format_percent(-5.0, buf, 64);     ok_str(L"negative clamps to 0", buf, L"0");
    uiformat_format_percent(9.0, buf, 64);      ok_str(L"over 1 clamps to 100", buf, L"100");
}

static void test_round_trip(void)
{
    T_SEC(L"round trip: format then parse gives the same zone back");

    /* The editor does this on every save: show the percentage, read it back.
     * A mismatch here means the window lands somewhere else the next time. */
    const double samples[] = { 0.0, 0.001, 0.1, 0.235, 0.5, 0.75, 0.999, 1.0 };

    for (int i = 0; i < (int)(sizeof(samples) / sizeof(samples[0])); ++i) {
        wchar_t buf[64];
        uiformat_format_percent(samples[i], buf, 64);

        double back = -1.0;
        const BOOL did = uiformat_parse_percent(buf, &back);

        wchar_t what[96];
        _snwprintf_s(what, 96, _TRUNCATE, L"round trip %.4f via \"%ls\"", samples[i], buf);

        /* One decimal on the way out, so a value that is not a round tenth
         * comes back within half of that - which is why the tolerance is 0.001
         * (a tenth of a percent), not zero. */
        ok(what, did && fabs(back - samples[i]) < 0.001);
    }
}

static void test_parse_pixels(void)
{
    T_SEC(L"pixel parsing");

    int v = -1;

    ok(L"\"1280\" accepted", uiformat_parse_pixels(L"1280", 10000, &v));
    ok_int(L"value is 1280", v, 1280);

    ok(L"spaces tolerated", uiformat_parse_pixels(L"  800  ", 10000, &v));
    ok_int(L"value is 800", v, 800);

    ok(L"\"0\" accepted", uiformat_parse_pixels(L"0", 10000, &v));
    ok_int(L"value is 0", v, 0);

    ok(L"over max refused", !uiformat_parse_pixels(L"99999", 10000, &v));
    ok(L"negative refused", !uiformat_parse_pixels(L"-5", 10000, &v));
    ok(L"decimal refused",  !uiformat_parse_pixels(L"12.5", 10000, &v));
    ok(L"junk refused",     !uiformat_parse_pixels(L"12px", 10000, &v));
    ok(L"empty refused",    !uiformat_parse_pixels(L"", 10000, &v));
    ok(L"NULL refused",     !uiformat_parse_pixels(NULL, 10000, &v));

    /* 9 digits of pure digits must not overflow the accumulator. */
    ok(L"absurd length refused", !uiformat_parse_pixels(L"1234567890123", 0, &v));
}

static void test_format_frac(void)
{
    T_SEC(L"normalized readout");

    wchar_t buf[64];
    uiformat_format_frac(0.235, 4, buf, 64);  ok_str(L"0.2350", buf, L"0.2350");
    uiformat_format_frac(0.5,   4, buf, 64);  ok_str(L"0.5000", buf, L"0.5000");
    uiformat_format_frac(1.0,   2, buf, 64);  ok_str(L"1.00",   buf, L"1.00");
    uiformat_format_frac(0.0,   0, buf, 64);  ok_str(L"0",      buf, L"0");
}

static void test_zone_valid(void)
{
    T_SEC(L"zone validation");

    ok(L"full screen is valid",       uiformat_zone_valid(0.0, 0.0, 1.0, 1.0));
    ok(L"left half is valid",         uiformat_zone_valid(0.0, 0.0, 0.5, 1.0));
    ok(L"right half is valid",        uiformat_zone_valid(0.5, 0.0, 0.5, 1.0));
    ok(L"bottom right quarter valid", uiformat_zone_valid(0.5, 0.5, 0.5, 0.5));

    ok(L"zero width refused",   !uiformat_zone_valid(0.0, 0.0, 0.0, 1.0));
    ok(L"zero height refused",  !uiformat_zone_valid(0.0, 0.0, 1.0, 0.0));
    ok(L"negative x refused",   !uiformat_zone_valid(-0.1, 0.0, 0.5, 1.0));
    ok(L"x past 1 refused",     !uiformat_zone_valid(1.1, 0.0, 0.5, 1.0));
    ok(L"hanging off the right refused", !uiformat_zone_valid(0.8, 0.0, 0.5, 1.0));
    ok(L"hanging off the bottom refused", !uiformat_zone_valid(0.0, 0.8, 1.0, 0.5));

    /* Exactly touching the edge is fine: "the right half" is x=0.5 w=0.5. */
    ok(L"touching the right edge is valid",  uiformat_zone_valid(0.5, 0.0, 0.5, 1.0));
    ok(L"touching the bottom edge is valid", uiformat_zone_valid(0.0, 0.5, 1.0, 0.5));
}

/* ------------------------------------------------------------------ */

static void test_preview_geometry(void)
{
    T_SEC(L"preview geometry (the picture's arithmetic, not the drawing)");

    RECT m;

    /* A monitor with the same shape as the control fills it. */
    ok(L"16:10 monitor fits a 16:10 control",
       preview_monitor_rect(320, 200, 16, 10, 0, &m));
    ok_int(L"width is the control width", m.right - m.left, 320);
    ok_int(L"height keeps the ratio",     m.bottom - m.top, 200);

    /* A portrait panel inside a landscape control must be height-limited -
     * otherwise a 1440x2560 screen would be drawn as a landscape rectangle,
     * which is the exact confusion the preview exists to prevent. */
    ok(L"portrait monitor fits", preview_monitor_rect(320, 200, 9, 16, 0, &m));
    ok_int(L"portrait takes the full height", m.bottom - m.top, 200);
    ok(L"portrait comes out narrow", (m.right - m.left) < 200);

    /*
     * Padding insets the result, and the 16:10 shape then makes HEIGHT the
     * limiting axis: the usable area is 300x180, and 300x187 does not fit, so
     * the monitor comes out 288x180. (The first version of this assertion
     * expected 300 wide - the arithmetic was right and the test was wrong.)
     */
    preview_monitor_rect(320, 200, 16, 10, 10, &m);
    ok_int(L"padding shrinks the height",   m.bottom - m.top, 180);
    ok_int(L"width follows the 16:10 ratio", m.right - m.left, 288);

    /* No usable room is refused rather than producing a negative rectangle. */
    ok(L"tiny control is refused", !preview_monitor_rect(10, 10, 16, 10, 20, &m));
    ok(L"zero-sized control is refused", !preview_monitor_rect(0, 0, 16, 10, 0, &m));

    /* Zone mapping. */
    const RECT mon = { 0, 0, 200, 100 };
    RECT z;

    ok(L"left half maps", preview_zone_rect(&mon, 0.0, 0.0, 0.5, 1.0, &z));
    ok_int(L"left half starts at 0", z.left, 0);
    ok_int(L"left half ends at 100", z.right, 100);
    ok_int(L"full height", z.top, 0);
    ok_int(L"full height, bottom", z.bottom, 100);

    ok(L"right half maps", preview_zone_rect(&mon, 0.5, 0.0, 0.5, 1.0, &z));
    ok_int(L"right half starts at 100", z.left, 100);
    ok_int(L"right half ends at 200", z.right, 200);

    /* A zone hanging off the edge is clamped, so the picture stays inside the
     * control instead of escaping it. */
    ok(L"overflowing zone is clamped",
       preview_zone_rect(&mon, 0.8, 0.0, 0.5, 1.0, &z));
    ok_int(L"clamped to the right edge", z.right, 200);

    /* A degenerate zone still gets one pixel, otherwise the user sees nothing
     * at all and cannot tell "zero width" from "not drawn". */
    ok(L"zero-size zone is still drawn",
       preview_zone_rect(&mon, 0.5, 0.5, 0.0, 0.0, &z));
    ok(L"and it has width",  (z.right - z.left) >= 1);
    ok(L"and it has height", (z.bottom - z.top) >= 1);

    /* NaN must not reach the rectangle arithmetic.
     *
     * Built at run time on purpose: a literal 0.0/0.0 is a compile ERROR under
     * /WX (C2124, "divide by zero"), so the value has to come from a variable
     * the compiler cannot fold. */
    volatile double zero = 0.0;
    const double nan_val = zero / zero;

    const BOOL drew = preview_zone_rect(&mon, nan_val, 0.0, 0.5, 0.5, &z);
    ok(L"NaN zone does not crash and yields a sane rectangle",
       !drew || (z.right >= z.left && z.bottom >= z.top));
}

/*
 * The editor reports a window's REAL size as a share of the screen it is drawn
 * against ("1234x567 (48% x 35%)"). That is arithmetic on a measurement rather
 * than on something the user typed, so it lives here instead of in the window
 * code: round it wrong and the percentage contradicts the pixels printed right
 * next to it (2026-10-10, task B).
 */
static void test_share_percent(void)
{
    T_SEC(L"a window's share of the screen");

    ok_int(L"half of 2560 is 50%", uiformat_share_percent(1280, 2560), 50);
    ok_int(L"all of it is 100%",   uiformat_share_percent(1600, 1600), 100);
    ok_int(L"a third is 33%",      uiformat_share_percent(853, 2560), 33);
    ok_int(L"nothing is 0%",       uiformat_share_percent(0, 2560), 0);

    /* The rounding boundary on both sides: 1267 and 1268 of 2560 are the two
     * whole pixels either side of 49.5%. */
    ok_int(L"just under the half rounds down", uiformat_share_percent(1267, 2560), 49);
    ok_int(L"just over the half rounds up",    uiformat_share_percent(1268, 2560), 50);

    /* A window larger than the screen is reported as such rather than capped:
     * "it does not fit" is exactly what the user needs to see. */
    ok_int(L"bigger than the screen is reported as is",
           uiformat_share_percent(3000, 2560), 117);

    /* No monitor to measure against: 0, and never a division by zero. */
    ok_int(L"an unknown monitor gives 0",  uiformat_share_percent(100, 0), 0);
    ok_int(L"a negative monitor gives 0",  uiformat_share_percent(100, -5), 0);
}

/* ------------------------------------------------------------------ */

static void test_pagelist_columns(void)
{
    T_SEC(L"list column widths (lifted out of mainwindow.c so they can be tested)");

    int out[8];

    /* Equal weights divide evenly. */
    {
        const int w[4] = { 1, 1, 1, 1 };
        ok(L"four equal columns fit", pagelist_column_widths(w, 4, 400, out));
        ok_int(L"first quarter",  out[0], 100);
        ok_int(L"second quarter", out[1], 100);
        ok_int(L"third quarter",  out[2], 100);
        ok_int(L"fourth quarter", out[3], 100);
    }

    /* Unequal weights are proportional. */
    {
        const int w[3] = { 1, 2, 1 };
        ok(L"1:2:1 fits", pagelist_column_widths(w, 3, 400, out));
        ok_int(L"narrow column", out[0], 100);
        ok_int(L"wide column",   out[1], 200);
        ok_int(L"narrow again",  out[2], 100);
    }

    /*
     * The property that matters most: the widths sum to EXACTLY the available
     * space. Rounding each column independently leaves a gap, and a list whose
     * columns do not fill it shows a horizontal scrollbar with nothing beyond
     * it - which is a bug this project has already shipped once.
     */
    {
        const int w[3] = { 1, 1, 1 };
        ok(L"thirds fit", pagelist_column_widths(w, 3, 100, out));
        ok_int(L"100/3 sums to exactly 100", out[0] + out[1] + out[2], 100);
        ok(L"the last column absorbs the remainder", out[2] >= out[0]);
    }

    /* The real preset-list weights at the default window width. */
    {
        const int w[5] = { 180, 240, 220, 180, 100 };
        ok(L"the preset columns fit", pagelist_column_widths(w, 5, 836, out));

        int sum = 0;
        for (int i = 0; i < 5; ++i) sum += out[i];
        ok_int(L"and they sum to the available width", sum, 836);
    }

    /* A very narrow list still produces a usable, non-negative split. */
    {
        const int w[4] = { 180, 360, 220, 130 };
        ok(L"a narrow list still fits", pagelist_column_widths(w, 4, 120, out));
        ok(L"no column goes negative", out[0] >= 0 && out[1] >= 0 && out[2] >= 0 && out[3] >= 0);
        ok_int(L"and the total is exact", out[0] + out[1] + out[2] + out[3], 120);
    }

    /* Degenerate input is refused and `out` is left alone. */
    {
        const int two[2]   = { 1, 1 };
        const int zero[2]  = { 1, 0 };
        const int neg[2]   = { 1, -5 };

        out[0] = -12345;

        ok(L"NULL weights refused", !pagelist_column_widths(NULL, 2, 100, out));
        ok(L"zero columns refused", !pagelist_column_widths(two, 0, 100, out));
        ok(L"no room refused",      !pagelist_column_widths(two, 2, 0, out));
        ok(L"NULL output refused",  !pagelist_column_widths(two, 2, 100, NULL));
        ok(L"a zero-weight column is refused", !pagelist_column_widths(zero, 2, 100, out));
        ok(L"a negative-weight column is refused", !pagelist_column_widths(neg, 2, 100, out));
        ok_int(L"a refused call writes nothing", out[0], -12345);
    }
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t12_uiformat");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t12_uiformat - editor text <-> number rules");
    T_KV(L"run_at", L"%ls", t_now_string());

    test_parse_percent();
    test_parse_percent_rejected();
    test_parse_leaves_output_alone();
    test_format_percent();
    test_round_trip();
    test_parse_pixels();
    test_format_frac();
    test_zone_valid();
    test_preview_geometry();
    test_share_percent();
    test_pagelist_columns();

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
