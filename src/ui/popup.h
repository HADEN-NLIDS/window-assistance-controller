/*
 * popup.h - the pop-up panels the overlay can show: their state and geometry.
 *
 * WHY THIS IS NOT A WINDOW, AND NOT PART OF overlay.c
 *
 *   The overlay is a WS_EX_LAYERED window, so a child window inside it is not
 *   drawn at all - the same constraint that made the toolbar geometry instead
 *   of a sub-window (see the top of toolbar.h). A pop-up panel therefore has
 *   to be drawn by overlay.c, like everything else that appears on the
 *   overlay.
 *
 *   What it does NOT have to be is overlay.c's business. Two panels are
 *   already planned - [Layout format] above the canvas and the monitor
 *   dropdown in the toolbar - and stage 8 adds a slide-out sidebar that needs
 *   the same three answers. Those three answers are:
 *
 *     where does the panel go?
 *     is the pointer still on it?
 *     has the pointer left it?
 *
 *   Writing them three times is how three features end up disagreeing about
 *   what "the pointer left" means.
 *
 *   So this module owns the STATE and the ARITHMETIC and nothing else. It does
 *   not know what a toolbar is or what a monitor is, it never draws, and it
 *   calls no Win32 function - a test drives it with rectangles and points, no
 *   window required. That is the same reason relcoord.c is free of Win32
 *   calls, and it is what makes the tricky part (a panel near the screen edge)
 *   testable.
 *
 * THE HIT-TEST ORDER IS NOT HERE
 *
 *   The overlay carries several kinds of interactive thing, and the order they
 *   are asked in is a property of the OVERLAY rather than of any one panel:
 *
 *     1. an open pop-up panel - and a click OUTSIDE it closes it; that click
 *        must NOT fall through to the canvas, where it would start drawing a
 *        region (the overlay starts a drag for any press below the toolbar)
 *     2. toolbar buttons
 *     3. the name box, including [save]/[cancel] while typing
 *     4. the sidebar's edge strip and body                (stage 8)
 *     5. the canvas - free-hand region dragging
 *
 *   popup_hit() answers line 1 only. Keeping it to that is what stops this
 *   module from having to know about the toolbar, and it leaves the table
 *   above as the single place the order is written down.
 */

#ifndef WAC_POPUP_H
#define WAC_POPUP_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which panel is open. POPUP_NONE means "none", so a zeroed PopupState is a
 * closed one - the state a freshly created overlay is in. */
typedef enum {
    POPUP_NONE = 0,
    POPUP_LAYOUT,     /* [Layout format]: the grid of layout thumbnails */
    POPUP_MONITOR,    /* the monitor dropdown in the toolbar */
    POPUP_COUNT
} PopupId;

/*
 * One open panel.
 *
 * Two rectangles, and both matter:
 *
 *   anchor  the button that opened it. The pointer sitting on the anchor also
 *           keeps the panel open, which is what lets the user move from the
 *           button down onto the panel without it vanishing on the way.
 *   panel   the panel itself.
 *
 * The caller computes both; this module never derives one from the other,
 * because only the caller knows how big its own content is.
 */
typedef struct {
    PopupId id;
    RECT    anchor;
    RECT    panel;
} PopupState;

/*
 * What a point means while a panel is open.
 *
 * POPUP_HIT_OUTSIDE is the interesting one: the caller must CLOSE the panel
 * and consume the click. Letting it continue would reach the canvas handler
 * and draw a region under the panel the user was aiming at.
 */
typedef enum {
    POPUP_HIT_NONE = 0,   /* no panel is open */
    POPUP_HIT_PANEL,      /* inside the panel */
    POPUP_HIT_ANCHOR,     /* on the button that opened it - a second click
                           * there closes it again */
    POPUP_HIT_OUTSIDE     /* open, but the point is on neither */
} PopupHit;

/* A zeroed state is a closed one. Safe on NULL. */
void popup_init(PopupState *st);

BOOL   popup_is_open(const PopupState *st);
PopupId popup_current(const PopupState *st);

/*
 * Open `id` with these rectangles, or replace whatever was open.
 *
 * Refuses - leaving the state closed - when either rectangle is empty, which
 * is what a degenerate layout produces. An open panel with no area would be
 * invisible and would swallow clicks, so it is better not to open one.
 */
void popup_open(PopupState *st, PopupId id, const RECT *anchor, const RECT *panel);

/* Close, keeping the caller's other state alone. Safe to call when closed. */
void popup_close(PopupState *st);

/*
 * Which part of the open panel `pt` is on. Safe on NULL, and on a closed
 * state (returns POPUP_HIT_NONE).
 *
 * Uses a half-open test - [left,right) x [top,bottom) - so a point on the
 * shared edge of two rectangles belongs to exactly one of them. PtInRect()
 * would do the same, but it is a user32 export and this module deliberately
 * links nothing.
 */
PopupHit popup_hit(const PopupState *st, POINT pt);

/*
 * Should the panel stay open for this pointer position?
 *
 * TRUE while the pointer is on the anchor OR the panel. That is the whole
 * hover rule the design asks for: "it disappears once the mouse has left both
 * the button and the panel". The caller calls this on every WM_MOUSEMOVE and
 * closes the panel when it returns FALSE.
 */
BOOL popup_keep_hover(const PopupState *st, POINT pt);

/*
 * Where a panel of this size goes when it opens under `anchor`.
 *
 * Below the anchor and aligned to its left edge, then slid back inside
 * `screen` if that would run off the right or bottom edge; if there is no room
 * below at all it flips above the anchor. Pure arithmetic - no window, no
 * screen query - so every one of those cases can be asserted in a test
 * instead of being discovered on someone's narrow portrait monitor.
 *
 * The size is clamped to the screen first: a panel larger than the screen
 * cannot be placed meaningfully, and clipping it would hide the part the user
 * needs.
 *
 * Returns FALSE, writing nothing, for a NULL argument or a degenerate screen.
 */
BOOL popup_place_below(const RECT *anchor, int want_w, int want_h,
                       const RECT *screen, RECT *out);

#ifdef __cplusplus
}
#endif

#endif /* WAC_POPUP_H */
