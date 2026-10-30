/*
 * snapgrid.h - the region templates a workspace is arranged from.
 *
 * WHAT THIS IS
 *
 *   Pure geometry. Given a template, a gap and a screen size, it produces the
 *   normalized rectangles the overlay draws and the user then assigns windows
 *   to. No window, no DC, no state - which is what lets the arithmetic be
 *   checked against known screen sizes without a screen.
 *
 * WHY THE GAP IS IN PIXELS
 *
 *   The plan specifies gap_px with a default of 8, and pixels are
 *   the right unit for it: a gap exists so that two touching windows still read
 *   as two windows, and that is a property of the eye at a given DPI, not a
 *   fraction of the monitor. A 1% gap on a 2560-wide screen would be 25 px, and
 *   on a laptop panel 13 px - the same number meaning two different things.
 *
 *   The regions themselves are normalized, because that is what a Rule stores;
 *   the conversion happens once, here, so that a template applied on one monitor
 *   can be re-applied on another and still mean "the left half".
 *
 * WHAT CUSTOM MEANS
 *
 *   snapgrid_build() returns 0 for SNAP_CUSTOM. That is not a stub: custom means
 *   the user draws the regions by hand, and inventing a default set for it would
 *   be this module deciding something the user is about to decide themselves.
 *   Task 6.5 supplies the drawing.
 */

#ifndef WAC_SNAPGRID_H
#define WAC_SNAPGRID_H

#include <windows.h>

#include "../core/relcoord.h"   /* RelRect */
#include "i18n.h"               /* StrId - the type only; no string table */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SNAP_HALVES = 0,   /* two columns            */
    SNAP_THIRDS,       /* three columns          */
    SNAP_ONE_TWO,      /* 1+2: left full height, right split top/bottom  */
    SNAP_TWO_ONE,      /* 2+1: left split top/bottom, right full height  */
    SNAP_2X2,          /* four quadrants         */
    SNAP_3X3,          /* nine cells             */
    SNAP_CUSTOM,       /* drawn by hand - see 6.5 */
    SNAP_COUNT
} SnapTemplate;

/* The plan's default, and the largest region count any built-in template makes. */
#define SNAP_GAP_DEFAULT_PX 8
#define SNAP_MAX_REGIONS    9

/*
 * Fill `out` with the template's regions, normalized against a monitor of
 * screen_w x screen_h physical pixels.
 *
 * Returns the number of regions written, or 0 when there are none to write:
 * an unknown template, SNAP_CUSTOM, a degenerate screen, or `cap` too small for
 * the template. The caller draws nothing rather than drawing a partial layout -
 * half a 3x3 grid would silently look like a design decision.
 *
 * gap_px is physical; it is dropped to 0 when the gaps alone would consume the
 * whole axis, because a template of zero-width regions is worse than no gaps.
 */
int snapgrid_build(SnapTemplate tpl, int gap_px,
                   int screen_w, int screen_h,
                   RelRect *out, int cap);

/* Stable name for logs. Not translated: logs are not UI. */
const wchar_t *snapgrid_name(SnapTemplate tpl);

/* ------------------------------------------------------------------ */
/* Snapping (task 6.5)                                                 */
/* ------------------------------------------------------------------ */

/*
 * How close an edge has to come before it is pulled onto a line.
 *
 * The plan says 12 px. Physical pixels, like the gap, because the tolerance is
 * about how steady a hand is rather than how big the monitor is.
 */
#define SNAP_THRESHOLD_PX 12

/* Screen edges (2 per axis) plus both edges of up to SNAP_MAX_REGIONS. */
#define SNAP_MAX_LINES (2 + SNAP_MAX_REGIONS * 2)

/*
 * The candidate lines an edge can snap to, in physical pixels.
 *
 * One collection serves all three of the plan's targets - screen edges, grid
 * lines and neighbouring region borders - because they are the same thing: a
 * coordinate worth aligning to. Keeping them in one list also means a template
 * and a hand-drawn region snap identically, which is what stops "custom" from
 * feeling like a different program.
 */
typedef struct {
    int x[SNAP_MAX_LINES];
    int count_x;
    int y[SNAP_MAX_LINES];
    int count_y;
} SnapLines;

/* Collect the screen edges and the edges of `count` normalized regions. */
void snapgrid_collect_lines(const RelRect *regions, int count,
                            int screen_w, int screen_h,
                            SnapLines *out);

/*
 * Pull each edge of `r` onto the nearest line within `threshold_px`.
 *
 * Each of the four edges moves independently: a rectangle being dragged by its
 * bottom-right corner should be able to align its right edge to a column
 * without its top being yanked somewhere else. That is what makes a drag feel
 * like it is helping rather than fighting.
 *
 * Pure: it reads `r` and `lines` and writes only `r`.
 */
void snapgrid_snap_rect(RECT *r, const SnapLines *lines, int threshold_px);

/*
 * The caption a template shows in the layout panel, as an i18n id.
 *
 * The panel used to be five toolbar buttons, each with its own caption, and
 * the mapping "button -> template" lived here so that the two could not drift
 * apart. The buttons are gone (2026-10-09): the panel lists the templates in
 * SnapTemplate order, so the item index IS the template and no mapping is
 * needed at all. The caption still is, and it stays here so that the panel has
 * exactly one place to ask for it.
 */
StrId snapgrid_label_id(SnapTemplate tpl);

/* ------------------------------------------------------------------ */
/* Filling a dragged box into the nearest region (task 7.3)            */
/* ------------------------------------------------------------------ */

/*
 * How far from a region the pointer may be and still count, as a fraction of
 * the SHORTER screen side.
 *
 * 0.1 was the user's number, with the reason given in their words: layouts
 * normally have at least one side against a screen edge, so a pointer that
 * touches an edge is either inside a region or one region-width away from it.
 */
#define SNAP_FILL_MAX_FRAC 0.1f

/*
 * How close to a screen edge the pointer must be for the fill to trigger.
 *
 * The user's rule: the edge itself is the trigger, like the system snap. 32 px
 * is the grab band - wide enough for a hand that means it, narrow enough that
 * waving over the middle of a region never counts. Measured complaint it
 * answers: "鼠标在移动到布局框内就会触发吸附" - a pointer merely INSIDE a
 * region used to pick it, which is not what an edge is.
 */
#define SNAP_EDGE_BAND_PX 32

/*
 * Is `pt` within `band_px` of an edge of the w x h screen?
 *
 * Pure. A point outside the screen is never at its edge.
 */
BOOL snapgrid_edge_hit(int screen_w, int screen_h, POINT pt, int band_px);

/*
 * Index of the region nearest to `pt`, or -1 when none is within
 * `max_frac` x the shorter screen side.
 *
 * The distance is measured to the rectangle itself (0 inside), not to its
 * centre: a wide region's centre can be farther from the pointer than a
 * narrow neighbour's edge, and "nearest" has to mean what the eye sees.
 *
 * Pure: every input is an argument, so the rule can be checked on any machine
 * without a screen.
 */
int snapgrid_pick(const RelRect *regions, int count,
                  int screen_w, int screen_h, POINT pt, float max_frac);

#ifdef __cplusplus
}
#endif

#endif /* WAC_SNAPGRID_H */
