/*
 * sidebar.c - see sidebar.h for what this module owns and what it does not.
 *
 * No Win32 calls in this file beyond the RECT/POINT types and MulDiv: that is
 * what lets t70_sidebar link it with no window, no message loop and no
 * user32. RectIsEmpty and PtInRect are user32 exports, so the two helpers at
 * the top exist rather than being borrowed - same promise popup.c makes.
 */

#include "sidebar.h"

#include <wctype.h>   /* towlower, for the search box's matching */

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
 * Half-open on purpose, the same reason popup.c uses it: a point on the edge
 * shared by two rectangles belongs to exactly one of them, so "the row or the
 * body" and "the body or the edge strip" can never both answer yes.
 */
static BOOL rect_contains(const RECT *r, POINT pt)
{
    return (pt.x >= r->left && pt.x < r->right &&
            pt.y >= r->top  && pt.y < r->bottom) ? TRUE : FALSE;
}

static BOOL intersect_rect(const RECT *a, const RECT *b, RECT *out)
{
    if (a == NULL || b == NULL || out == NULL) return FALSE;

    out->left   = (a->left   > b->left)   ? a->left   : b->left;
    out->top    = (a->top    > b->top)    ? a->top    : b->top;
    out->right  = (a->right  < b->right)  ? a->right  : b->right;
    out->bottom = (a->bottom < b->bottom) ? a->bottom : b->bottom;

    if (rect_empty(out)) return FALSE;
    return TRUE;
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ------------------------------------------------------------------ */
/* Metrics                                                             */
/* ------------------------------------------------------------------ */

void sidebar_metrics(int dpi, SidebarMetrics *out)
{
    if (out == NULL) return;

    /* Same guard as toolbar_set_dpi(): a bad dpi must not be able to produce a
     * panel wider than the monitor it slides over. */
    if (dpi < 48 || dpi > 480) dpi = 96;

    out->edge_w      = MulDiv(SIDEBAR_EDGE_W,      dpi, 96);
    out->body_w      = MulDiv(SIDEBAR_BODY_W,      dpi, 96);
    out->pad         = MulDiv(SIDEBAR_PAD,         dpi, 96);
    out->row_h       = MulDiv(SIDEBAR_ROW_H,       dpi, 96);
    out->header_h    = MulDiv(SIDEBAR_HEADER_H,    dpi, 96);
    out->filter_h    = MulDiv(SIDEBAR_FILTER_H,    dpi, 96);
    out->icon        = MulDiv(SIDEBAR_ICON,        dpi, 96);
    out->scrollbar_w = MulDiv(SIDEBAR_SCROLLBAR_W, dpi, 96);
    out->ghost_w     = MulDiv(SIDEBAR_GHOST_W,     dpi, 96);
    out->handle_w    = MulDiv(SIDEBAR_HANDLE_W,    dpi, 96);
    out->handle_h    = MulDiv(SIDEBAR_HANDLE_H,    dpi, 96);

    /*
     * Floors, so that a tiny dpi cannot produce a control that is there but
     * unusable - an icon that does not fit its row is drawn clipped, and a
     * zero-width edge strip can never be hit by the pointer.
     */
    if (out->edge_w < 1)      out->edge_w = 1;
    if (out->scrollbar_w < 2) out->scrollbar_w = 2;
    if (out->row_h < out->icon + 4) out->row_h = out->icon + 4;
    /* The ghost must at least hold the icon it is standing in for. */
    if (out->ghost_w < out->icon + out->pad * 2) out->ghost_w = out->icon + out->pad * 2;

    /* The handle must be visibly wider than the strip it sticks out of, or it
     * is not a signpost - it is the same strip with a different name. */
    if (out->handle_w < out->edge_w * 2) out->handle_w = out->edge_w * 2;
    if (out->handle_h < out->row_h * 2)  out->handle_h = out->row_h * 2;
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

void sidebar_init(SidebarState *st)
{
    if (st == NULL) return;

    /*
     * Zero is a valid closed state - SIDEBAR_CLOSED is 0, progress 0 - so a
     * state that has never been touched needs no special case anywhere below.
     * hover_row and press_row are 0 after ZeroMemory, and 0 is a legal row
     * index, so they are set to -1 explicitly rather than left to zero.
     */
    ZeroMemory(st, sizeof(*st));
    st->hover_row = -1;
    st->press_row = -1;
    st->drag_row  = -1;
}

int sidebar_progress(const SidebarState *st)
{
    return (st != NULL) ? st->progress : 0;
}

SidebarPhase sidebar_phase(const SidebarState *st)
{
    return (st != NULL) ? st->phase : SIDEBAR_CLOSED;
}

BOOL sidebar_is_animating(const SidebarState *st)
{
    if (st == NULL) return FALSE;
    return (st->phase == SIDEBAR_OPENING || st->phase == SIDEBAR_CLOSING) ? TRUE : FALSE;
}

BOOL sidebar_is_open(const SidebarState *st)
{
    return (st != NULL && st->phase == SIDEBAR_OPEN) ? TRUE : FALSE;
}

/* Progress `dt` milliseconds into a slide from `from` to `to`. */
static int progress_at(int from, int to, unsigned long long dt)
{
    if (dt >= SIDEBAR_SLIDE_MS) return to;
    return from + (int)(((long long)(to - from) * (long long)dt) / SIDEBAR_SLIDE_MS);
}

void sidebar_set_open(SidebarState *st, BOOL want_open, unsigned long long now_ms)
{
    if (st == NULL) return;

    if (want_open) {
        /* Already going out, or out. Repeating the request must not restart the
         * slide: this is called on every mouse move, and restarting would make
         * a panel that never finishes arriving. */
        if (st->phase == SIDEBAR_OPENING || st->phase == SIDEBAR_OPEN) return;

        st->from_progress = st->progress;
        st->phase_ms      = now_ms;
        st->phase         = (st->progress >= SIDEBAR_PROGRESS_MAX) ? SIDEBAR_OPEN
                                                                   : SIDEBAR_OPENING;
        return;
    }

    /*
     * A close request during a drag is ignored - the header says why: the
     * pointer is off the panel for the whole trip to the canvas, and the panel
     * closing mid-drag would take the item with it.
     */
    if (st->dragging) return;

    if (st->phase == SIDEBAR_CLOSING || st->phase == SIDEBAR_CLOSED) return;

    st->from_progress = st->progress;
    st->phase_ms      = now_ms;
    st->phase         = (st->progress <= 0) ? SIDEBAR_CLOSED : SIDEBAR_CLOSING;
}

BOOL sidebar_advance(SidebarState *st, unsigned long long now_ms)
{
    if (st == NULL) return FALSE;

    /* A timestamp that went backwards (a clock change, a wrapped tick count)
     * is treated as "no time has passed" rather than as a jump. */
    const unsigned long long dt = (now_ms > st->phase_ms) ? (now_ms - st->phase_ms) : 0;

    switch (st->phase) {
    case SIDEBAR_OPENING: {
        const int want = progress_at(st->from_progress, SIDEBAR_PROGRESS_MAX, dt);

        /* Monotonic. Progress is computed from the clock, so a timestamp that
         * did not advance - or went backwards - would otherwise recompute the
         * same or an earlier value and make the panel jump BACK. The panel may
         * not retreat while it is coming out. */
        if (want > st->progress) st->progress = want;

        if (dt >= SIDEBAR_SLIDE_MS) {
            st->progress = SIDEBAR_PROGRESS_MAX;
            st->phase    = SIDEBAR_OPEN;
            return FALSE;
        }
        return TRUE;
    }

    case SIDEBAR_CLOSING: {
        const int want = progress_at(st->from_progress, 0, dt);

        if (want < st->progress) st->progress = want;

        if (dt >= SIDEBAR_SLIDE_MS) {
            st->progress = 0;
            st->phase    = SIDEBAR_CLOSED;
            return FALSE;
        }
        return TRUE;
    }

    default:
        /* Settled - nothing to animate, so the caller may stop its timer. */
        return FALSE;
    }
}

/* ------------------------------------------------------------------ */
/* Hover, press, drag                                                  */
/* ------------------------------------------------------------------ */

void sidebar_note_hover(SidebarState *st, int row)
{
    if (st == NULL) return;
    st->hover_row = (row >= 0) ? row : -1;
}

int sidebar_hover_row(const SidebarState *st)
{
    return (st != NULL) ? st->hover_row : -1;
}

void sidebar_press(SidebarState *st, int row)
{
    if (st == NULL) return;
    st->press_row = (row >= 0) ? row : -1;
}

int sidebar_pressed_row(const SidebarState *st)
{
    return (st != NULL) ? st->press_row : -1;
}

void sidebar_release_press(SidebarState *st)
{
    if (st == NULL) return;
    st->press_row = -1;
}

void sidebar_drag_begin(SidebarState *st, POINT pt)
{
    if (st == NULL) return;

    /* A drag is always the continuation of a press. Starting one without a
     * pressed row would let a stray mouse move carry an application the user
     * never picked up. */
    if (st->press_row < 0) return;

    st->dragging  = TRUE;
    st->drag_row  = st->press_row;
    st->drag_pt   = pt;
    st->hover_row = -1;   /* the pointer left the row it started on */
}

BOOL sidebar_drag_move(SidebarState *st, POINT pt)
{
    if (st == NULL || !st->dragging) return FALSE;

    if (pt.x == st->drag_pt.x && pt.y == st->drag_pt.y) return FALSE;
    st->drag_pt = pt;
    return TRUE;
}

void sidebar_drag_end(SidebarState *st)
{
    if (st == NULL) return;

    st->dragging = FALSE;
    st->drag_row = -1;
    st->press_row = -1;
}

BOOL sidebar_is_dragging(const SidebarState *st)
{
    return (st != NULL && st->dragging) ? TRUE : FALSE;
}

int sidebar_drag_row(const SidebarState *st)
{
    return (st != NULL) ? st->drag_row : -1;
}

BOOL sidebar_drag_ghost(const SidebarState *st, const SidebarMetrics *m, RECT *out)
{
    if (st == NULL || m == NULL || out == NULL) return FALSE;
    if (!st->dragging) return FALSE;

    /* Centred on the pointer, so the thing being carried is under the cursor
     * rather than beside it - the drop point is the cursor, and a ghost that
     * trailed behind it would point at the wrong place.
     *
     * Wide enough for a NAME (see SIDEBAR_GHOST_W): the ghost is how the user
     * knows which of forty rows they picked up. */
    const int w = m->ghost_w;
    const int h = m->row_h;

    out->left   = st->drag_pt.x - w / 2;
    out->top    = st->drag_pt.y - h / 2;
    out->right  = out->left + w;
    out->bottom = out->top + h;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Scrolling                                                           */
/* ------------------------------------------------------------------ */

BOOL sidebar_rows_rect(const SidebarMetrics *m, const RECT *area, RECT *out)
{
    if (m == NULL || area == NULL || out == NULL) return FALSE;
    if (rect_empty(area)) return FALSE;

    RECT r;
    r.left   = area->left;
    r.right  = area->right;
    /* The header, then the search box, then the rows (2026-10-10). */
    r.top    = area->top + m->pad + m->header_h + m->filter_h;
    r.bottom = area->bottom - m->pad;

    if (rect_empty(&r)) return FALSE;

    *out = r;
    return TRUE;
}

BOOL sidebar_header_rect(const SidebarState *st, const SidebarMetrics *m,
                         const RECT *area, RECT *out)
{
    if (st == NULL || m == NULL || area == NULL || out == NULL) return FALSE;
    if (rect_empty(area)) return FALSE;

    RECT body;
    if (!sidebar_body_rect(st, m, area, &body)) return FALSE;

    out->left   = body.left + m->pad;
    out->right  = body.right - m->pad;
    out->top    = area->top + m->pad;
    out->bottom = out->top + m->header_h;   /* exactly one header tall */

    if (rect_empty(out)) return FALSE;
    return TRUE;
}

BOOL sidebar_filter_rect(const SidebarState *st, const SidebarMetrics *m,
                         const RECT *area, RECT *out)
{
    if (st == NULL || m == NULL || area == NULL || out == NULL) return FALSE;
    if (rect_empty(area)) return FALSE;

    /* Laid out in the BODY's rectangle, like the rows, so the box slides with
     * the panel instead of staying behind at the screen edge. */
    RECT head;
    if (!sidebar_header_rect(st, m, area, &head)) return FALSE;

    out->left   = head.left;
    out->right  = head.right;
    out->top    = head.bottom;             /* starts where the header ends */
    out->bottom = out->top + m->filter_h;

    if (rect_empty(out)) return FALSE;
    if (out->bottom > area->bottom - m->pad) return FALSE;
    return TRUE;
}

BOOL sidebar_matches(const wchar_t *name, const wchar_t *filter)
{
    if (name == NULL) return FALSE;
    if (filter == NULL || filter[0] == L'\0') return TRUE;

    const size_t fl = wcslen(filter);

    /* Case-insensitive substring. Written out rather than borrowed from
     * shlwapi's StrStrIW so that this module keeps its promise of linking
     * nothing - and it is eight lines. */
    for (const wchar_t *p = name; *p != L'\0'; ++p) {
        size_t i = 0;
        while (i < fl && p[i] != L'\0' &&
               towlower((wint_t)p[i]) == towlower((wint_t)filter[i])) {
            ++i;
        }
        if (i == fl) return TRUE;
    }
    return FALSE;
}

int sidebar_visible_rows(const SidebarMetrics *m, const RECT *area)
{
    RECT rows;
    if (m == NULL || m->row_h <= 0) return 0;
    if (!sidebar_rows_rect(m, area, &rows)) return 0;

    /*
     * Rounded UP. A row that is only partly inside the rows area is still
     * drawn (clipped) and still hit-testable, so counting whole rows only
     * would make the bottom row visible but not clickable. Drawing, hit
     * testing and the scroll range all read this one number.
     */
    const int h = rows.bottom - rows.top;
    return (h + m->row_h - 1) / m->row_h;
}

int sidebar_max_scroll(int total_rows, int visible_rows)
{
    if (total_rows <= 0 || visible_rows <= 0) return 0;
    return (total_rows > visible_rows) ? (total_rows - visible_rows) : 0;
}

int sidebar_scroll(const SidebarState *st)
{
    return (st != NULL) ? st->scroll : 0;
}

void sidebar_scroll_to(SidebarState *st, int first, int total_rows, int visible_rows)
{
    if (st == NULL) return;

    const int max_first = sidebar_max_scroll(total_rows, visible_rows);
    st->scroll = clamp_int(first, 0, max_first);
}

void sidebar_scroll_by(SidebarState *st, int rows, int total_rows, int visible_rows)
{
    if (st == NULL) return;
    sidebar_scroll_to(st, st->scroll + rows, total_rows, visible_rows);
}

void sidebar_ensure_visible(SidebarState *st, int row, int total_rows, int visible_rows)
{
    if (st == NULL || row < 0) return;
    if (visible_rows <= 0) return;

    if (row < st->scroll) {
        sidebar_scroll_to(st, row, total_rows, visible_rows);
    } else if (row >= st->scroll + visible_rows) {
        sidebar_scroll_to(st, row - visible_rows + 1, total_rows, visible_rows);
    } else {
        /* Already visible: clamp anyway, because the window may have shrunk
         * since this was last set - a stored scroll past the end shows blank
         * rows under the last item, which reads as "the list ended early". */
        sidebar_scroll_to(st, st->scroll, total_rows, visible_rows);
    }
}

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

BOOL sidebar_edge_rect(const SidebarMetrics *m, const RECT *area, RECT *out)
{
    if (m == NULL || area == NULL || out == NULL) return FALSE;
    if (rect_empty(area)) return FALSE;

    const int area_w = area->right - area->left;
    const int w      = (m->edge_w < area_w) ? m->edge_w : area_w;

    out->left   = area->right - w;
    out->right  = area->right;
    out->top    = area->top;
    out->bottom = area->bottom;
    return TRUE;
}

BOOL sidebar_handle_rect(const SidebarMetrics *m, const RECT *area, RECT *out)
{
    if (m == NULL || area == NULL || out == NULL) return FALSE;
    if (rect_empty(area)) return FALSE;

    const int area_w = area->right - area->left;
    const int area_h = area->bottom - area->top;

    const int w = (m->handle_w < area_w) ? m->handle_w : area_w;
    const int h = (m->handle_h < area_h) ? m->handle_h : area_h;
    if (w <= 0 || h <= 0) return FALSE;

    /* Vertically centred: the middle of the right edge is where a hand goes
     * looking for a drawer, and it is the one place on a full-height strip
     * that is not near a corner the user is aiming at for something else. */
    const int cy = area->top + area_h / 2;

    out->left   = area->right - w;
    out->right  = area->right;
    out->top    = cy - h / 2;
    out->bottom = out->top + h;

    if (out->top < area->top) {
        out->top    = area->top;
        out->bottom = out->top + h;
    }
    if (out->bottom > area->bottom) {
        out->bottom = area->bottom;
        out->top    = out->bottom - h;
    }

    return TRUE;
}

BOOL sidebar_body_rect(const SidebarState *st, const SidebarMetrics *m,
                       const RECT *area, RECT *out)
{
    if (st == NULL || m == NULL || area == NULL || out == NULL) return FALSE;
    if (rect_empty(area)) return FALSE;
    if (st->progress <= 0) return FALSE;

    /* Never wider than the screen it slides over: a panel wider than the
     * monitor would push its own left edge off screen and nothing could be
     * read from it. */
    const int area_w = area->right - area->left;
    const int body_w = (m->body_w < area_w) ? m->body_w : area_w;
    if (body_w <= 0) return FALSE;

    /* Rounded UP: at progress 1 the panel must already be one pixel out, or
     * the first few frames of every slide would draw nothing at all. */
    int vis_w = (int)(((long long)body_w * st->progress +
                       SIDEBAR_PROGRESS_MAX - 1) / SIDEBAR_PROGRESS_MAX);
    vis_w = clamp_int(vis_w, 1, body_w);

    /*
     * The panel slides in from the right edge: at progress 0 it is entirely
     * past area.right, at full progress it ends exactly on it. The content is
     * laid out in THIS rectangle, so during the slide its right edge is PAST
     * the screen - the rows are revealed by the screen edge, not pushed by it.
     */
    out->left   = area->right - vis_w;
    out->right  = out->left + body_w;
    out->top    = area->top;
    out->bottom = area->bottom;
    return TRUE;
}

BOOL sidebar_body_visible_rect(const SidebarState *st, const SidebarMetrics *m,
                               const RECT *area, RECT *out)
{
    if (out == NULL) return FALSE;

    RECT body;
    if (!sidebar_body_rect(st, m, area, &body)) return FALSE;
    return intersect_rect(&body, area, out);
}

BOOL sidebar_row_rect(const SidebarState *st, const SidebarMetrics *m,
                      const RECT *area, int row, RECT *out)
{
    if (st == NULL || m == NULL || area == NULL || out == NULL) return FALSE;
    if (row < 0 || row < st->scroll) return FALSE;

    RECT rows, body;
    if (!sidebar_rows_rect(m, area, &rows)) return FALSE;
    if (!sidebar_body_rect(st, m, area, &body)) return FALSE;

    /*
     * The same count the hit test uses, so a row can never be drawn but not
     * clickable or the other way round - the two answers come from one number
     * (see sidebar_visible_rows).
     */
    const int index = row - st->scroll;
    if (index < 0 || index >= sidebar_visible_rows(m, area)) return FALSE;

    const int top = rows.top + index * m->row_h;
    if (top >= rows.bottom) return FALSE;      /* below the last visible row */

    out->left   = body.left + m->pad;
    out->right  = body.right - m->pad;
    out->top    = top;
    out->bottom = top + m->row_h;

    /* The last row is often cut by the bottom of the rows area, and the
     * rectangle handed back is the part that is actually on screen: the caller
     * fills it without asking again, so it must not reach into the padding
     * below or up into the header. */
    if (out->top    < rows.top)    out->top    = rows.top;
    if (out->bottom > rows.bottom) out->bottom = rows.bottom;

    if (rect_empty(out)) return FALSE;
    return TRUE;
}

BOOL sidebar_scrollbar_rects(const SidebarState *st, const SidebarMetrics *m,
                             const RECT *area, int total_rows, int visible_rows,
                             RECT *track, RECT *thumb)
{
    if (st == NULL || m == NULL || area == NULL) return FALSE;
    if (track == NULL && thumb == NULL) return FALSE;

    /* Nothing to scroll: a scrollbar on a list that fits is a control that
     * does nothing, which is the shape trap 24 warns about. */
    if (visible_rows <= 0 || total_rows <= visible_rows) return FALSE;

    RECT rows, body;
    if (!sidebar_rows_rect(m, area, &rows)) return FALSE;
    if (!sidebar_body_rect(st, m, area, &body)) return FALSE;

    RECT t;
    t.left   = body.right - m->pad - m->scrollbar_w;
    t.right  = body.right - m->pad;
    t.top    = rows.top;
    t.bottom = rows.bottom;
    if (rect_empty(&t)) return FALSE;

    if (track != NULL) *track = t;

    if (thumb != NULL) {
        const int track_h = t.bottom - t.top;
        const int max_first = sidebar_max_scroll(total_rows, visible_rows);

        int thumb_h = MulDiv(track_h, visible_rows, total_rows);
        /* A thumb shorter than a few pixels cannot be grabbed, and one taller
         * than its track cannot move - both floors keep it draggable. */
        const int min_h = (m->pad > 0) ? m->pad * 2 : 4;
        thumb_h = clamp_int(thumb_h, min_h, track_h);

        int top = t.top;
        if (max_first > 0) {
            const int travel = track_h - thumb_h;
            top += MulDiv(travel, st->scroll, max_first);
        }

        thumb->left   = t.left;
        thumb->right  = t.right;
        thumb->top    = top;
        thumb->bottom = top + thumb_h;
    }

    return TRUE;
}

int sidebar_scroll_from_thumb(const RECT *track, int thumb_h,
                              int total_rows, int visible_rows, int y)
{
    if (track == NULL) return 0;

    const int max_first = sidebar_max_scroll(total_rows, visible_rows);
    if (max_first <= 0) return 0;

    const int track_h = track->bottom - track->top;
    const int travel  = track_h - thumb_h;
    if (travel <= 0) return 0;

    /* The thumb is centred on the pointer, which is what the user expects when
     * they grab it - and the clamping makes dragging past either end stop at
     * the end instead of scrolling into blank space. */
    const int pos = clamp_int(y - thumb_h / 2 - track->top, 0, travel);
    return clamp_int(MulDiv(max_first, pos, travel), 0, max_first);
}

/* ------------------------------------------------------------------ */
/* Dropping an item on the canvas                                      */
/* ------------------------------------------------------------------ */

BOOL sidebar_drop_zone(const RECT *area, POINT pt, float want_w, float want_h,
                       RelRect *out)
{
    if (area == NULL || out == NULL) return FALSE;
    if (rect_empty(area)) return FALSE;
    if (!(want_w > 0.0f) || !(want_h > 0.0f)) return FALSE;   /* also rejects NaN */

    /* A zone bigger than the canvas cannot be centred on anything; it is the
     * whole canvas, and the clamp below is what says so. */
    if (want_w > 1.0f) want_w = 1.0f;
    if (want_h > 1.0f) want_h = 1.0f;

    const int w = area->right - area->left;
    const int h = area->bottom - area->top;

    int px_w = (int)(want_w * (float)w + 0.5f);
    int px_h = (int)(want_h * (float)h + 0.5f);
    if (px_w < 1) px_w = 1;
    if (px_h < 1) px_h = 1;

    int left = pt.x - px_w / 2;
    int top  = pt.y - px_h / 2;

    if (left + px_w > area->right)  left = area->right  - px_w;
    if (top  + px_h > area->bottom) top  = area->bottom - px_h;
    if (left < area->left)          left = area->left;
    if (top  < area->top)           top  = area->top;

    ZeroMemory(out, sizeof(*out));
    out->x     = (float)(left - area->left) / (float)w;
    out->y     = (float)(top  - area->top)  / (float)h;
    out->w     = (float)px_w / (float)w;
    out->h     = (float)px_h / (float)h;
    out->min_w = 1;
    out->min_h = 1;
    /* abs_w / abs_h stay 0: this is a fraction zone. Section 5.2 - the two
     * halves of the rule are chosen by whether those two are zero, and there
     * is no measured window here to put in them. */
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Hit testing                                                         */
/* ------------------------------------------------------------------ */

static SidebarHit hit_of(SidebarHitKind kind, int row)
{
    SidebarHit h;
    h.kind = kind;
    h.row  = row;
    return h;
}

SidebarHit sidebar_hit(const SidebarState *st, const SidebarMetrics *m,
                       const RECT *area, int total_rows, POINT pt)
{
    if (st == NULL || m == NULL || area == NULL) return hit_of(SIDEBAR_HIT_NONE, -1);
    if (rect_empty(area) || total_rows < 0)  return hit_of(SIDEBAR_HIT_NONE, -1);

    /*
     * The BODY first, then the edge strip. During the slide the panel covers
     * part of the strip, and the covered part belongs to the panel (trap 33:
     * before using a rectangle, ask whose it is). Points outside the VISIBLE
     * part of the body are never the body, however far the panel has slid: a
     * click on invisible panel is a click on the canvas.
     */
    RECT vis;
    if (sidebar_body_visible_rect(st, m, area, &vis) && rect_contains(&vis, pt)) {

        RECT rows_r;
        const BOOL have_rows = sidebar_rows_rect(m, area, &rows_r);
        const int visible    = sidebar_visible_rows(m, area);

        /* The scrollbar sits inside the row area's right edge, so it is asked
         * before the rows are - otherwise the row under it would win. */
        if (visible > 0 && total_rows > visible) {
            RECT track;
            if (sidebar_scrollbar_rects(st, m, area, total_rows, visible, &track, NULL) &&
                rect_contains(&track, pt)) {
                return hit_of(SIDEBAR_HIT_SCROLLBAR, -1);
            }
        }

        /* The search box, between the header and the rows. */
        RECT fr;
        const BOOL have_filter = sidebar_filter_rect(st, m, area, &fr);
        if (have_filter && rect_contains(&fr, pt)) {
            return hit_of(SIDEBAR_HIT_FILTER, -1);
        }

        if (have_rows && rect_contains(&rows_r, pt)) {
            const int index = (pt.y - rows_r.top) / m->row_h;
            const int row   = st->scroll + index;

            if (index >= 0 && index < visible && row >= 0 && row < total_rows) {
                return hit_of(SIDEBAR_HIT_ROW, row);
            }
            /* Below the last item: background, not a row that does not exist. */
            return hit_of(SIDEBAR_HIT_BODY, -1);
        }

        /* Everything above the search box is the header line. */
        const int header_end = have_filter ? fr.top
                                           : (have_rows ? rows_r.top : area->bottom);
        if (pt.y >= area->top && pt.y < header_end) {
            return hit_of(SIDEBAR_HIT_HEADER, -1);
        }

        return hit_of(SIDEBAR_HIT_BODY, -1);
    }

    /*
     * The strip's own rectangles, the HANDLE first: it is wider than the strip
     * and the two overlap by construction, so asking the strip first would make
     * the handle's outer half answer as the strip. Same behaviour either way
     * today, but the two are different things to the user and the difference
     * should exist in the code before it is needed.
     */
    RECT handle;
    if (sidebar_handle_rect(m, area, &handle) && rect_contains(&handle, pt)) {
        return hit_of(SIDEBAR_HIT_HANDLE, -1);
    }

    RECT edge;
    if (sidebar_edge_rect(m, area, &edge) && rect_contains(&edge, pt)) {
        return hit_of(SIDEBAR_HIT_EDGE, -1);
    }

    return hit_of(SIDEBAR_HIT_NONE, -1);
}

BOOL sidebar_hit_keeps_open(SidebarHit hit)
{
    return (hit.kind != SIDEBAR_HIT_NONE) ? TRUE : FALSE;
}
