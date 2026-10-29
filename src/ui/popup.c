/*
 * popup.c - see popup.h for what this module owns and what it deliberately
 * does not.
 *
 * No Win32 calls in this file beyond the RECT/POINT types: that is what lets
 * t52_popup link it with no window, no message loop and no user32. SetRectEmpty
 * and PtInRect are user32 exports, so the two helpers below exist rather than
 * being borrowed - they are three lines each and they keep the promise.
 */

#include "popup.h"

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* A rectangle with no area in either axis. */
static BOOL rect_empty(const RECT *r)
{
    return (r == NULL || r->right <= r->left || r->bottom <= r->top) ? TRUE : FALSE;
}

/*
 * Half-open containment: [left,right) x [top,bottom).
 *
 * Half-open on purpose. A point on the edge shared by two rectangles belongs
 * to exactly one of them, so "is it the panel or the anchor" can never answer
 * both, and a panel placed exactly below an anchor does not steal the anchor's
 * bottom row of pixels.
 */
static BOOL rect_contains(const RECT *r, POINT pt)
{
    return (pt.x >= r->left && pt.x < r->right &&
            pt.y >= r->top  && pt.y < r->bottom) ? TRUE : FALSE;
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

void popup_init(PopupState *st)
{
    if (st == NULL) return;

    /* Zero is a valid closed state, and POPUP_NONE is 0 - so a popup that has
     * never been opened needs no special case anywhere below. */
    ZeroMemory(st, sizeof(*st));
}

BOOL popup_is_open(const PopupState *st)
{
    return (st != NULL && st->id != POPUP_NONE) ? TRUE : FALSE;
}

PopupId popup_current(const PopupState *st)
{
    return (st != NULL) ? st->id : POPUP_NONE;
}

void popup_open(PopupState *st, PopupId id, const RECT *anchor, const RECT *panel)
{
    if (st == NULL) return;

    /*
     * Refuse rather than open something unusable. A panel with no area would
     * be invisible AND would still swallow the next click, which reads to the
     * user as "the program stopped responding to the mouse".
     */
    if (id == POPUP_NONE || id >= POPUP_COUNT) { popup_close(st); return; }
    if (rect_empty(anchor) || rect_empty(panel)) { popup_close(st); return; }

    st->id     = id;
    st->anchor = *anchor;
    st->panel  = *panel;
}

void popup_close(PopupState *st)
{
    if (st == NULL) return;

    /*
     * Only the id is cleared. The rectangles are left as they were so that a
     * caller which repaints "the place the panel used to be" still has the
     * right rectangle after closing - clearing them would make the old panel
     * stay on screen until something else forced a full repaint.
     */
    st->id = POPUP_NONE;
}

/* ------------------------------------------------------------------ */
/* Hit testing and hover                                               */
/* ------------------------------------------------------------------ */

PopupHit popup_hit(const PopupState *st, POINT pt)
{
    if (!popup_is_open(st)) return POPUP_HIT_NONE;

    /* The panel first: while it is open it covers the anchor in the layout
     * that matters, and the two can never overlap anyway - so this order is a
     * property of the code rather than of where the caller placed things. */
    if (rect_contains(&st->panel, pt))  return POPUP_HIT_PANEL;
    if (rect_contains(&st->anchor, pt)) return POPUP_HIT_ANCHOR;

    return POPUP_HIT_OUTSIDE;
}

BOOL popup_keep_hover(const PopupState *st, POINT pt)
{
    if (!popup_is_open(st)) return FALSE;

    /* Anchor OR panel. This is the whole rule the design asks for: the panel
     * goes away once the pointer has left both, and NOT on the way between
     * them - which is the only path a user ever takes to reach it. */
    return (rect_contains(&st->panel, pt) ||
            rect_contains(&st->anchor, pt)) ? TRUE : FALSE;
}

/* ------------------------------------------------------------------ */
/* Placement                                                           */
/* ------------------------------------------------------------------ */

BOOL popup_place_below(const RECT *anchor, int want_w, int want_h,
                       const RECT *screen, RECT *out)
{
    if (anchor == NULL || screen == NULL || out == NULL) return FALSE;
    if (want_w <= 0 || want_h <= 0) return FALSE;
    if (rect_empty(anchor) || rect_empty(screen)) return FALSE;

    const int screen_w = screen->right - screen->left;
    const int screen_h = screen->bottom - screen->top;

    /* Clamp to the screen first. A panel bigger than the screen cannot be
     * placed meaningfully, and clipping it would hide the very part the user
     * opened it for. */
    int w = (want_w < screen_w) ? want_w : screen_w;
    int h = (want_h < screen_h) ? want_h : screen_h;

    /* Below the anchor, aligned to its left edge - so the panel reads as
     * belonging to the button rather than floating somewhere. */
    int x = anchor->left;
    int y = anchor->bottom;

    /* Slide back inside instead of clipping. */
    if (x + w > screen->right) x = screen->right - w;
    if (x < screen->left)      x = screen->left;

    if (y + h > screen->bottom) {
        /* No room below: flip above the anchor. The toolbar is at the top of
         * the screen in today's layout, so this cannot happen yet - but a
         * placement rule that only works in one layout is a rule that has to
         * be rewritten the first time something moves. */
        y = anchor->top - h;
        if (y < screen->top) y = screen->top;
    }

    out->left   = x;
    out->top    = y;
    out->right  = x + w;
    out->bottom = y + h;
    return TRUE;
}
