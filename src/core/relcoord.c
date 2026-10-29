/*
 * relcoord.c - normalized coordinates <-> physical pixels.
 *
 * See relcoord.h for the coordinate model. This file is deliberately free
 * of any Win32 call beyond the RECT/POINT/SIZE types, so tests/t10_relcoord
 * can link and run it without a window or a message loop.
 *
 * Why no scaling factor appears anywhere here: under PerMonitorV2,
 * GetWindowRect and SetWindowPos already speak physical pixels (verified in
 * stage-1 experiment 2 by asking for 400x300 and reading back exactly
 * 400x300). Introducing a DPI term would double-apply the scaling.
 *
 * THAT HOLDS ONLY WHILE THE WINDOW STAYS ON ONE DPI. Moving a window onto a
 * monitor with a different DPI makes Windows rescale the window itself
 * (WM_DPICHANGED), so the SIZE lands as request x (new DPI / old DPI).
 * Measured 2026-10-08 on the 100% + 150% pair: 600x400 -> 900x600, and
 * 800x600 going back the other way -> 533x400. The POSITION is unaffected -
 * it lands exactly, on every run. This is not a property of the target
 * window: plain SetWindowPos from an unrelated process reproduces it under
 * both PerMonitorV2 and unaware thread contexts.
 *
 * A scaling factor here would therefore be wrong twice over - it would
 * double-apply on the ordinary same-DPI path and still not describe the
 * cross-DPI one. win_place.c absorbs the rescale by asking for the size a
 * second time, once the window already lives on the target monitor.
 */

#include "relcoord.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Round half away from zero.
 *
 * Not floorf(x + 0.5f): 0.5f * 1920 computed in float is 960.0f exactly,
 * but intermediate products such as 0.3333f * 2400 land a hair below the
 * .5 boundary and floor(x+0.5) then rounds the wrong way. lroundf rounds
 * half away from zero and behaves predictably for negative coordinates,
 * which matter because a monitor left of the primary has a negative origin.
 */
static int round_half_away(float v)
{
    return (int)lroundf(v);
}

/* A monitor rectangle is usable only if it has a positive extent. */
static int monitor_ok(const RECT *rc)
{
    return rc != NULL && rc->right > rc->left && rc->bottom > rc->top;
}

static int clamp_int(int v, int lo, int hi)
{
    if (lo > hi) hi = lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float clamp_float(float v, float lo, float hi)
{
    if (lo > hi) hi = lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ------------------------------------------------------------------ */
/* rel_to_pixels                                                       */
/* ------------------------------------------------------------------ */

BOOL rel_to_pixels(const RelRect *rel, const RECT *rc_monitor,
                   POINT *out_pt, SIZE *out_size)
{
    if (rel == NULL || out_pt == NULL || out_size == NULL) return FALSE;
    if (!monitor_ok(rc_monitor)) return FALSE;

    const int mon_w = rc_monitor->right - rc_monitor->left;
    const int mon_h = rc_monitor->bottom - rc_monitor->top;

    /* ---- size ---------------------------------------------------- */
    int w = round_half_away(rel->w * (float)mon_w);
    int h = round_half_away(rel->h * (float)mon_h);

    /* Never produce a sliver. REL_MIN_FRACTION of the monitor is the floor
     * unless the caller supplied a larger explicit minimum. */
    const int frac_floor_w = (int)(REL_MIN_FRACTION * (float)mon_w);
    const int frac_floor_h = (int)(REL_MIN_FRACTION * (float)mon_h);

    int floor_w = frac_floor_w;
    int floor_h = frac_floor_h;
    if (rel->min_w > floor_w) floor_w = rel->min_w;
    if (rel->min_h > floor_h) floor_h = rel->min_h;

    if (floor_w < 1) floor_w = 1;
    if (floor_h < 1) floor_h = 1;

    if (w < floor_w) w = floor_w;
    if (h < floor_h) h = floor_h;

    /* Do not let a zone exceed its monitor - a window larger than the
     * screen cannot be positioned meaningfully. */
    if (w > mon_w) w = mon_w;
    if (h > mon_h) h = mon_h;

    /* ---- position ------------------------------------------------ */
    int x = rc_monitor->left + round_half_away(rel->x * (float)mon_w);
    int y = rc_monitor->top  + round_half_away(rel->y * (float)mon_h);

    /*
     * Keep the window inside the monitor. When the size had to be grown
     * above the requested one, anchor the LEFT/TOP edge and let the right
     * edge extend - that matches what a user sees when they drag a window
     * to a corner, and it keeps the top-left anchor meaningful.
     */
    if (x + w > rc_monitor->right)  x = rc_monitor->right - w;
    if (y + h > rc_monitor->bottom) y = rc_monitor->bottom - h;
    if (x < rc_monitor->left) x = rc_monitor->left;
    if (y < rc_monitor->top)  y = rc_monitor->top;

    out_pt->x   = x;
    out_pt->y   = y;
    out_size->cx = w;
    out_size->cy = h;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* rel_from_pixels                                                     */
/* ------------------------------------------------------------------ */

BOOL rel_from_pixels(const RECT *window, const RECT *rc_monitor, RelRect *out_rel)
{
    if (window == NULL || out_rel == NULL) return FALSE;
    if (!monitor_ok(rc_monitor)) return FALSE;

    const float mon_w = (float)(rc_monitor->right - rc_monitor->left);
    const float mon_h = (float)(rc_monitor->bottom - rc_monitor->top);

    const int w = window->right - window->left;
    const int h = window->bottom - window->top;

    out_rel->x = ((float)(window->left - rc_monitor->left)) / mon_w;
    out_rel->y = ((float)(window->top  - rc_monitor->top))  / mon_h;
    out_rel->w = ((float)w) / mon_w;
    out_rel->h = ((float)h) / mon_h;

    /* Record the measured size so a later resolution change has a
     * reference, and so rel_resolve_size can prefer it. The monitor width
     * goes with it: a size without the screen it was measured on cannot be
     * interpreted six months later on a different machine. */
    out_rel->abs_w = w;
    out_rel->abs_h = h;
    out_rel->ref_w = (int)mon_w;

    return TRUE;
}

/* ------------------------------------------------------------------ */
/* rel_clamp_to_monitor                                                */
/* ------------------------------------------------------------------ */

BOOL rel_clamp_to_monitor(RelRect *rel, const RECT *rc_monitor)
{
    if (rel == NULL || !monitor_ok(rc_monitor)) return FALSE;

    const float mon_w = (float)(rc_monitor->right - rc_monitor->left);
    const float mon_h = (float)(rc_monitor->bottom - rc_monitor->top);

    /* Size first, so the position clamp below sees the final size. */
    rel->w = clamp_float(rel->w, REL_MIN_FRACTION, 1.0f);
    rel->h = clamp_float(rel->h, REL_MIN_FRACTION, 1.0f);

    /* Enforce a pixel-level minimum when one was measured. */
    if (rel->min_w > 0) {
        const float min_frac = (float)rel->min_w / mon_w;
        if (rel->w < min_frac) rel->w = min_frac;
    }
    if (rel->min_h > 0) {
        const float min_frac = (float)rel->min_h / mon_h;
        if (rel->h < min_frac) rel->h = min_frac;
    }

    if (rel->w > 1.0f) rel->w = 1.0f;
    if (rel->h > 1.0f) rel->h = 1.0f;

    /* Position: the window must start inside the monitor and must not run
     * off the far edge. */
    const float max_x = 1.0f - rel->w;
    const float max_y = 1.0f - rel->h;

    rel->x = clamp_float(rel->x, 0.0f, max_x < 0.0f ? 0.0f : max_x);
    rel->y = clamp_float(rel->y, 0.0f, max_y < 0.0f ? 0.0f : max_y);

    return TRUE;
}

/* ------------------------------------------------------------------ */
/* rel_seed_absolute_sizes                                             */
/* ------------------------------------------------------------------ */

void rel_seed_absolute_sizes(RelRect *rel, const RECT *rc_monitor)
{
    if (rel == NULL || !monitor_ok(rc_monitor)) return;

    const int mon_w = rc_monitor->right - rc_monitor->left;
    const int mon_h = rc_monitor->bottom - rc_monitor->top;

    if (rel->abs_w <= 0) {
        int w = round_half_away(rel->w * (float)mon_w);
        if (w < 1) w = 1;
        rel->abs_w = w;
    }
    if (rel->abs_h <= 0) {
        int h = round_half_away(rel->h * (float)mon_h);
        if (h < 1) h = 1;
        rel->abs_h = h;
    }
    if (rel->min_w <= 0) rel->min_w = 1;
    if (rel->min_h <= 0) rel->min_h = 1;

    /* The fabricated sizes describe this monitor, so they carry its width -
     * otherwise rel_resolve_size would read them as "measured somewhere
     * unknown" and could not follow a later screen change. */
    rel->ref_w = mon_w;
}

/* ------------------------------------------------------------------ */
/* rel_resolve_size                                                    */
/* ------------------------------------------------------------------ */

/* REL_JITTER_FRACTION now lives in relcoord.h: the workspace slot's monitor
 * binding asks the same "is this still the same panel?" question, and two
 * copies would drift apart. */

void rel_resolve_size(const RelRect *rel, const RECT *rc_monitor, SIZE *out_size)
{
    if (rel == NULL || out_size == NULL || !monitor_ok(rc_monitor)) return;

    const int mon_w = rc_monitor->right - rc_monitor->left;
    const int mon_h = rc_monitor->bottom - rc_monitor->top;

    int w, h;

    /* ---------------------------------------------------------------- */
    /* STEP 1: establish a baseline size, in pixels                     */
    /* ---------------------------------------------------------------- */

    if (rel->abs_w > 0 && rel->abs_h > 0) {
        /*
         * THE BASELINE IS THE SIZE THE USER ACTUALLY SAW.
         *
         * abs_w/abs_h were measured from a real window on a real screen, so
         * they carry the window's true shape. Everything below operates on
         * this one pair of numbers, which is what makes the shape survive a
         * change of panel.
         *
         * The previous version gated this branch on a ref_monitor_width
         * parameter being positive and, when the caller passed 0, fell
         * through to the fraction path - w = rel->w * mon_w,
         * h = rel->h * mon_h. For a window that filled its screen those
         * fractions are 1.0 x 1.0, so the baseline silently became "whatever
         * shape this monitor happens to be". On a 1440x2560 portrait panel a
         * 2560x1440 window therefore came back as 1296x2304 - a 16:9 window
         * standing on its head. Measured in t10.
         *
         * A missing reference is not a reason to throw the measurement
         * away: the measured size is meaningful on its own, so it is used
         * as-is.
         */
        w = rel->abs_w;
        h = rel->abs_h;

        /*
         * A reference width, when we have one, tells us whether the monitor
         * itself changed. A change beyond the jitter band means the machine
         * has a different screen, and a window that kept its pixel size on a
         * much larger panel would look lost in a corner - so it follows the
         * panel by ONE factor on BOTH axes.
         *
         * One factor is the whole point. Scaling the width by the monitor
         * width and the height by the monitor height is what reshapes a
         * window when the two differ, and the two always differ on a panel
         * whose aspect is not the window's.
         *
         * A missing reference (0) is not a reason to discard the
         * measurement: the measured size is meaningful on its own.
         */
        if (rel->ref_w > 0) {
            const float grow = (float)mon_w / (float)rel->ref_w;
            const float delta = fabsf(grow - 1.0f);

            if (delta >= REL_JITTER_FRACTION) {
                w = round_half_away((float)w * grow);
                h = round_half_away((float)h * grow);
            }
        }
    } else {
        /*
         * No measurement yet - a brand-new preset. Each fraction maps to its
         * OWN axis here, and only here: "the left half" must mean the left
         * half of a panel of any shape. This is the one place where letting
         * the axes scale independently is correct, because the fractions
         * were never a window's shape to begin with.
         */
        w = round_half_away(rel->w * (float)mon_w);
        h = round_half_away(rel->h * (float)mon_h);
    }

    /* ---------------------------------------------------------------- */
    /* STEP 2: floor, ceiling, floor again                              */
    /* ---------------------------------------------------------------- */

    int floor_w = rel->min_w > 0 ? rel->min_w : 1;
    int floor_h = rel->min_h > 0 ? rel->min_h : 1;
    if (floor_w < 1) floor_w = 1;
    if (floor_h < 1) floor_h = 1;

    if (w < floor_w) w = floor_w;
    if (h < floor_h) h = floor_h;

    /*
     * THE CEILING IS ONE SCALE FACTOR FOR BOTH AXES, AND IT IS THE PANEL.
     *
     * Computing ceil_w from the monitor width and ceil_h from the monitor
     * height and clamping each on its own reshapes the window whenever the
     * panel's aspect differs from the window's. Measured case: a 2560x1440
     * window on a 1440x2560 portrait panel gave ceil_w = 1296 and
     * ceil_h = 2304, so the height passed through untouched while the width
     * was cut - a 16:9 window became roughly 9:16, the exact opposite of the
     * user's intent.
     *
     * Taking the smaller of the two ratios and applying it to BOTH axes can
     * only ever shrink the window proportionally. It cannot flip or distort
     * the shape.
     *
     * WHY 100% AND NOT 90% (changed 2026-09-20)
     *
     * The ceiling used to be 0.9 of the panel. Measured consequence: a
     * window placed at 1280x1600 on a 2560x1600 screen - an ordinary
     * half-screen layout - and then remembered, came back as 1152x1440. It
     * was quietly shrunk by 10% on every restore, and the report still said
     * "position and size both as requested", because the shrunk rectangle
     * had already BECOME the request. The user's only clue was that their
     * window was the wrong size.
     *
     * A window as large as its monitor is legitimate; side-by-side layouts
     * are exactly that shape. Only a window that does NOT FIT should be
     * reduced, and "does not fit" means larger than the panel - so the
     * ceiling is the panel itself.
     *
     * The baseline is guaranteed >= 1 by the floor above, so the divisions
     * are safe.
     */
    const float max_scale_w = (float)mon_w / (float)w;
    const float max_scale_h = (float)mon_h / (float)h;

    float scale = 1.0f;
    if (max_scale_w < scale) scale = max_scale_w;
    if (max_scale_h < scale) scale = max_scale_h;
    if (scale < 0.0f) scale = 0.0f;

    if (scale < 1.0f) {
        w = round_half_away((float)w * scale);
        h = round_half_away((float)h * scale);
    }

    /* A stated minimum outranks the ceiling: a target that refuses to go
     * below min_w is better served by a window hanging off the panel edge
     * than by one the target will reject outright. */
    if (w < floor_w) w = floor_w;
    if (h < floor_h) h = floor_h;
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    out_size->cx = w;
    out_size->cy = h;
}
