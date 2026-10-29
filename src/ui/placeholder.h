/*
 * placeholder.h - the boxes the edit overlay draws for the windows a workspace
 * is meant to place, and the arithmetic behind them.
 *
 * WHAT A PLACEHOLDER IS
 *
 *   Until now the editor drew anonymous regions: a rectangle meant "a window
 *   goes here", with nothing saying WHICH window or how big it really is. A
 *   placeholder is that rectangle with an identity - an icon, a name, and a
 *   size - so the user can see their layout instead of inferring it from
 *   shapes (acceptance items 8 and 9).
 *
 *   The size shown is the PIXEL size the zone works out to on this screen, and
 *   optionally the size the window was last actually seen at (Rule.last_w /
 *   last_h). Those two differ exactly when the window refuses to be the size
 *   the preset asks for, which is the case the user kept reporting.
 *
 * WHY THIS IS A MODULE AND NOT PART OF overlay.c
 *
 *   Same reason sidebar.c and popup.c are: the arithmetic is what goes wrong on
 *   someone else's screen - a box in a corner, a box too small to hold its own
 *   label, a zone that lands half off the edge - and arithmetic in overlay.c
 *   cannot be tested at all. This module calls no Win32 function.
 */

#ifndef WAC_PLACEHOLDER_H
#define WAC_PLACEHOLDER_H

#include <windows.h>

#include "../core/relcoord.h"   /* RelRect - the type only */

#ifdef __cplusplus
extern "C" {
#endif

/* 96-dpi bases. */
#define PLACEHOLDER_PAD     8
#define PLACEHOLDER_ICON   20
#define PLACEHOLDER_LINE_H 20

/*
 * Below this the box is not drawn at all.
 *
 * A three-pixel rectangle with a 20-px icon in it is not a smaller version of
 * the same thing, it is a scribble: the label would be clipped to nothing and
 * the icon would cover the whole box. Saying nothing is better than saying
 * something unreadable.
 */
#define PLACEHOLDER_MIN_W  72
#define PLACEHOLDER_MIN_H  44

/*
 * How many are drawn.
 *
 * A workspace holds at most MW_SLOT_MAX slots, and the canvas is one screen:
 * past a dozen or so the boxes overlap into a solid wash and none of them can
 * be read. The caller reports how many were left out - the rule this project
 * follows everywhere ("said, not silently dropped").
 */
#define PLACEHOLDER_MAX_SHOWN 16

/*
 * The grip: a square in the box's bottom-right corner that resizes it.
 *
 * ONE grip, not eight. Eight handles on a box that may be 80 px wide is a
 * control panel, and the two things a user actually does here are "move it" and
 * "make it bigger" - the editor is for deciding a layout, and the precise
 * numbers can be typed on the main window's preset page afterwards.
 */
#define PLACEHOLDER_GRIP 14

typedef struct {
    int pad;
    int icon;
    int line_h;
    int grip;
} PlaceholderMetrics;

void placeholder_metrics(int dpi, PlaceholderMetrics *out);

/*
 * The box for `zone`, in `area`'s coordinates.
 *
 * The same projection overlay.c has always used for regions (fraction x the
 * screen), written down once so the placeholders and the regions cannot end up
 * disagreeing about where a half-screen rectangle is.
 *
 * FALSE for a NULL argument, an empty area, or a zone that projects to nothing.
 */
BOOL placeholder_rect(const RelRect *zone, const RECT *area, RECT *out);

/* Is this box big enough to say anything in? See PLACEHOLDER_MIN_W. */
BOOL placeholder_is_visible(const RECT *box);

/* The icon's square, in the box's top-left corner. FALSE when it does not fit. */
BOOL placeholder_icon_rect(const PlaceholderMetrics *m, const RECT *box, RECT *out);

/* The name's line: to the right of the icon, on the icon's row. */
BOOL placeholder_name_rect(const PlaceholderMetrics *m, const RECT *box, RECT *out);

/* The size readout: the full width under the icon row. */
BOOL placeholder_size_rect(const PlaceholderMetrics *m, const RECT *box, RECT *out);

/* ------------------------------------------------------------------ */
/* Editing a box: what is under the pointer, and where it goes next     */
/* ------------------------------------------------------------------ */

/* The resize grip, in the box's bottom-right corner. FALSE when the box is too
 * small to hold one without it covering the whole box. */
BOOL placeholder_grip_rect(const PlaceholderMetrics *m, const RECT *box, RECT *out);

typedef enum {
    PLACEHOLDER_HIT_NONE = 0,
    PLACEHOLDER_HIT_GRIP,   /* the resize handle of box `index` */
    PLACEHOLDER_HIT_BODY    /* the box itself - drag to move it   */
} PlaceholderHitKind;

typedef struct {
    PlaceholderHitKind kind;
    int                index;   /* -1 when kind is NONE */
} PlaceholderHit;

/*
 * What is under `pt`.
 *
 * Searched from the END of the array backwards, because that is the order the
 * boxes were drawn in: the one on top is the one the user sees, and it is the
 * one a click has to reach.
 *
 * A grip is asked before a body, and only for its own box - a grip that
 * answered for the box underneath would resize something the user is not
 * pointing at.
 */
PlaceholderHit placeholder_hit(const RECT *boxes, int count,
                               const PlaceholderMetrics *m, POINT pt);

/*
 * The box moved by (dx, dy), kept inside `area`.
 *
 * Clamped rather than refused: dragging a box past the edge of the screen and
 * having it simply stop there is what every window does, and a box that
 * vanished because the pointer went too far would be lost work.
 */
BOOL placeholder_move_rect(const RECT *box, int dx, int dy,
                           const RECT *area, RECT *out);

/*
 * The box resized so that its bottom-right corner is at `pt`.
 *
 * The top-left corner does not move, and the result never gets smaller than
 * `min_w` x `min_h` (nor bigger than `area`). One corner is enough: the layout
 * being decided is "this much of the screen", and a box that could also grow
 * leftwards would fight the grid it is being snapped to.
 */
BOOL placeholder_resize_rect(const RECT *box, POINT pt, const RECT *area,
                             int min_w, int min_h, RECT *out);

/*
 * Back to a fraction of the screen, for the caller to store.
 *
 * The exact inverse of placeholder_rect(), and it has to be the exact inverse:
 * a box that is dragged and dropped without moving must come back as the same
 * zone, or every edit would drift by a pixel or two. Values are clamped to
 * [0,1]; min_w/min_h come back as 1, which is what the placement path expects
 * from a zone the user drew.
 */
BOOL placeholder_zone_from_rect(const RECT *box, const RECT *area, RelRect *out);

#ifdef __cplusplus
}
#endif

#endif /* WAC_PLACEHOLDER_H */
