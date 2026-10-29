/*
 * relcoord.h - normalized coordinates <-> physical pixels.
 *
 * This module exists on its own because coordinate math is where this
 * program is most likely to go subtly wrong, and it is the one part that
 * can be tested without a window, a monitor, or a message loop. Keep it
 * free of Win32 calls beyond the RECT/POINT/SIZE types.
 *
 * THE COORDINATE MODEL
 *
 *   A zone is stored as a fraction of the TARGET MONITOR's rcMonitor, not
 *   of the whole virtual desktop:
 *
 *       x = 0.5  means "half way across monitor N"
 *       w = 0.5  means "half of monitor N's width"
 *
 *   Storing monitor-relative fractions is what makes a preset portable:
 *   it survives a resolution change, and it means something even when the
 *   machine it is loaded on has a different monitor layout.
 *
 *   rcMonitor (not rcWork) is the reference because rcWork changes when the
 *   taskbar moves or auto-hides, which would silently shift every stored
 *   position.
 *
 * MEASURED (2026-09-19, 2560x1600 @150%, PerMonitorV2):
 *   Under PerMonitorV2 there is no DPI scaling term in the round trip -
 *   GetWindowRect and SetWindowPos already speak physical pixels. Verified
 *   by asking Windows for a 400x300 window and reading back exactly
 *   400x300. So no scale factor appears anywhere in this file, by design.
 */

#ifndef WAC_RELCOORD_H
#define WAC_RELCOORD_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A zone: where a window should sit on its monitor.
 *
 * All four fractions are relative to that monitor's rcMonitor and normally
 * lie in [0,1]. Values outside that range are legal on input (a window may
 * deliberately hang off the edge) but rel_clamp_to_monitor() will pull them
 * back in when the caller asks for it.
 */
typedef struct {
    float x, y;              /* top-left corner, as a fraction */
    float w, h;              /* size, as a fraction */

    int   min_w, min_h;      /* smallest size accepted by the target, pixels */
    int   abs_w, abs_h;      /* measured window size, pixels - THE baseline
                              * for rel_resolve_size(); it is the only field
                              * here that records the window's real shape */
    int   ref_w;             /* width of the monitor abs_w/abs_h were measured
                              * on; 0 = unknown. Written by rel_from_pixels()
                              * and rel_seed_absolute_sizes(), so a caller can
                              * never forget to pass it and silently lose the
                              * window's shape. */
    unsigned lock_size : 1;  /* window refused resizing: move only, never resize */
} RelRect;

/* Smallest zone this module will produce, in normalized units. Guards
 * against a preset that would otherwise become a one-pixel sliver. */
#define REL_MIN_FRACTION 0.02f

/*
 * A monitor width change smaller than this fraction is treated as noise
 * rather than as a different screen. Taskbar auto-hide, a DPI rounding
 * difference and a KVM that re-panels a display with a few pixels missing all
 * move the numbers slightly, and reacting to them would make windows visibly
 * jump for no reason.
 *
 * Beyond the band the monitor is assumed to be a genuinely different one and
 * a recorded window size follows it proportionally.
 *
 * PUBLIC because the same question - "is this still the same panel?" - is
 * also asked when a workspace slot's monitor binding is evaluated
 * (ws_slot_evaluate_monitor). Two hard-coded 0.20f values would drift apart,
 * and then the two features would disagree about what a different panel is.
 */
#define REL_JITTER_FRACTION 0.20f

/*
 * Convert a zone into pixels on a given monitor.
 *
 * rc_monitor is the target monitor's rcMonitor. out_pt receives the
 * top-left corner and out_size the size, both in physical pixels.
 *
 * Returns FALSE and leaves the outputs untouched when:
 *   - either pointer is NULL
 *   - the monitor has zero or negative width/height (division by zero)
 *   - the resulting size would be smaller than REL_MIN_FRACTION of the
 *     monitor, in which case the result cannot be trusted to be usable
 *
 * The result is clamped to the monitor and to rel->min_w/min_h, so a caller
 * can always use it directly without further checking.
 *
 * WHICH SIZE FUNCTION DO I WANT?
 *
 *   This one sizes a ZONE: because w and h are fractions of their own axes,
 *   the result takes the shape the panel gives it. That is right for
 *   anything the user described as a share of the screen - "the left half",
 *   a workspace slot, a cell of the fake desktop - and it is why a "quarter
 *   of the screen" is a tall cell on a portrait panel.
 *
 *   It is WRONG for restoring a window the user placed and sized by hand:
 *   those fractions came from measuring that window, so feeding them back
 *   only reproduces its shape while the panel keeps the shape it had. Use
 *   rel_resolve_size() for that; it works from abs_w/abs_h and keeps the
 *   window's own aspect ratio.
 *
 *   Mixing the two is the bug this file's history keeps re-learning: a
 *   2560x1440 window whose zone read 1.0 x 1.0 came back as a 1440x2560
 *   shape (1296x2304) on a portrait panel, because the fractions - not the
 *   measurement - were used to size it.
 */
BOOL rel_to_pixels(const RelRect *rel, const RECT *rc_monitor,
                   POINT *out_pt, SIZE *out_size);

/*
 * Convert an actual window rectangle on a given monitor back into a zone.
 *
 * Used when recording a layout: "put the window where it is now".
 * abs_w/abs_h are filled from the rectangle so a later resolution change
 * has a reference size to fall back on. The other fields of out_rel are
 * left untouched (caller owns min_w/min_h/lock_size).
 *
 * Returns FALSE on NULL pointers or a degenerate monitor.
 */
BOOL rel_from_pixels(const RECT *window, const RECT *rc_monitor, RelRect *out_rel);

/*
 * Pull a zone back inside [0,1] and enforce the minimum size.
 *
 * Applied when loading a config that may have been hand-edited, and after
 * a resolution change that made a stored zone degenerate.
 *
 * The minimum size is expressed in pixels but the zone is normalized, so
 * this needs the monitor to know what "min_w pixels" means in fractions.
 * Pass the current monitor; when min_w/min_h are zero the check is skipped.
 */
BOOL rel_clamp_to_monitor(RelRect *rel, const RECT *rc_monitor);

/*
 * Fill in default absolute sizes when the zone has never been applied.
 *
 * A brand-new zone has no measured size. Rather than leave abs_w/abs_h at
 * zero (which would make later clamping meaningless), derive them from the
 * fractions and the current monitor.
 */
void rel_seed_absolute_sizes(RelRect *rel, const RECT *rc_monitor);

/*
 * Decide the pixel size to request for this zone on this monitor.
 *
 * This is the rule that makes a preset survive a resolution change. It works
 * on ONE pair of numbers - the baseline - and never scales the two axes by
 * different amounts:
 *
 *   1. If abs_w/abs_h were ever measured, THEY are the baseline. They came
 *      from a real window, so they carry its real shape.
 *   2. Only when there is no measurement (a brand-new preset) are the
 *      fractions used, each against its own axis.
 *   3. rel->ref_w, when known, is compared with the current monitor: a
 *      change under 20% is jitter (taskbar auto-hide, a DPI rounding
 *      difference) and the baseline is kept verbatim. A larger change means
 *      a different screen, and the baseline follows it by a single factor
 *      applied to BOTH axes.
 *   4. The ceiling is the monitor ITSELF, also applied as a single factor to
 *      both axes, so a window that does not fit is shrunk without being
 *      reshaped. A stated min_w/min_h outranks the ceiling.
 *
 *      It was 90% until 2026-09-20. That was wrong: an ordinary half-screen
 *      window (1280x1600 on a 2560x1600 panel) came back 10% smaller every
 *      time it was restored, because the shrunken rectangle silently became
 *      the request. See relcoord.c for the full measurement.
 *
 * There is deliberately no `ref_monitor_width` parameter. It used to be one,
 * and a caller that passed 0 silently lost the window's shape by falling back
 * to the fractions; the reference belongs next to the measurement it
 * describes, where rel_from_pixels() can maintain it.
 *
 * ASPECT RATIO IS PRESERVED BY CONSTRUCTION.
 *
 *   Exactly one factor multiplies both axes on every path, so no path can
 *   reshape a window:
 *
 *     - a 16:9 window measured on a landscape screen stays 16:9 when the
 *       same preset is applied to a PORTRAIT screen - it is shrunk to fit,
 *       not rotated into 9:16;
 *     - a panel that is neither 16:9 nor 16:10 (7:5, 3:2, 4:3, 32:9) is
 *       handled by the same arithmetic, because nothing here reads the
 *       panel's own aspect ratio;
 *     - the fraction path is the deliberate exception, and it is not a
 *       window's shape: "the left half" must mean the left half on a panel
 *       of any shape.
 *
 *   Both of the first two points were real bugs, both measured on a
 *   1440x2560 panel where a 2560x1440 window came back as 1296x2304:
 *
 *     - the ceiling was computed per axis, so the height passed through
 *       untouched while only the width was cut;
 *     - an unknown ref_monitor_width fell through to the fraction path, and
 *       for a window that filled its screen those fractions are 1.0 x 1.0,
 *       so the baseline became the new panel's shape.
 *
 *   See t10_relcoord's portrait section, which pins both.
 *
 *   A caller that genuinely wants a window reshaped to some other ratio must
 *   express that as a new zone, not by relying on the clamp.
 */
void rel_resolve_size(const RelRect *rel, const RECT *rc_monitor,
                      SIZE *out_size);

#ifdef __cplusplus
}
#endif

#endif /* WAC_RELCOORD_H */
