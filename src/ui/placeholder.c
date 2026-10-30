/*
 * placeholder.c - see placeholder.h for what these boxes are and why the
 * arithmetic lives outside overlay.c.
 *
 * No Win32 calls beyond the RECT type and MulDiv: that is what lets
 * tests/t72_placeholder.c link it with no window and no screen.
 */

#include "placeholder.h"

static BOOL rect_empty(const RECT *r)
{
    return (r == NULL || r->right <= r->left || r->bottom <= r->top) ? TRUE : FALSE;
}

void placeholder_metrics(int dpi, PlaceholderMetrics *out)
{
    if (out == NULL) return;

    /* The same guard the toolbar and the sidebar apply: a dpi Windows never
     * reports must not be able to produce a label taller than the screen. */
    if (dpi < 48 || dpi > 480) dpi = 96;

    out->pad    = MulDiv(PLACEHOLDER_PAD,    dpi, 96);
    out->icon   = MulDiv(PLACEHOLDER_ICON,   dpi, 96);
    out->line_h = MulDiv(PLACEHOLDER_LINE_H, dpi, 96);
    out->grip   = MulDiv(PLACEHOLDER_GRIP,   dpi, 96);

    if (out->pad < 1)         out->pad = 1;
    if (out->icon < 8)        out->icon = 8;
    if (out->line_h < 10)     out->line_h = 10;
    if (out->grip < 6)        out->grip = 6;
}

BOOL placeholder_rect(const RelRect *zone, const RECT *area, RECT *out)
{
    if (zone == NULL || area == NULL || out == NULL) return FALSE;
    if (rect_empty(area)) return FALSE;

    const int w = area->right - area->left;
    const int h = area->bottom - area->top;

    out->left   = area->left + (LONG)(zone->x * (float)w);
    out->top    = area->top  + (LONG)(zone->y * (float)h);
    out->right  = out->left + (LONG)(zone->w * (float)w);
    out->bottom = out->top  + (LONG)(zone->h * (float)h);

    if (rect_empty(out)) return FALSE;

    /* Kept inside the canvas: a box hanging off the edge is one the user cannot
     * see, and the zone it came from is still what gets placed - this only
     * decides what is drawn. */
    if (out->left   < area->left)   out->left   = area->left;
    if (out->top    < area->top)    out->top    = area->top;
    if (out->right  > area->right)  out->right  = area->right;
    if (out->bottom > area->bottom) out->bottom = area->bottom;

    if (rect_empty(out)) return FALSE;
    return TRUE;
}

BOOL placeholder_is_visible(const RECT *box)
{
    if (rect_empty(box)) return FALSE;
    return ((box->right - box->left) >= PLACEHOLDER_MIN_W &&
            (box->bottom - box->top) >= PLACEHOLDER_MIN_H) ? TRUE : FALSE;
}

/*
 * The top of the label block: the box's own top, unless the caller's floor is
 * lower. One helper for the three rows so that the icon, the name and the
 * size can never slide down by different amounts.
 */
static int label_top(const PlaceholderMetrics *m, const RECT *box, int min_top)
{
    const int top = box->top + m->pad;
    return (top < min_top) ? min_top : top;
}

BOOL placeholder_icon_rect(const PlaceholderMetrics *m, const RECT *box,
                           int min_top, RECT *out)
{
    if (m == NULL || box == NULL || out == NULL) return FALSE;
    if (rect_empty(box)) return FALSE;

    out->left   = box->left + m->pad;
    out->top    = label_top(m, box, min_top);
    out->right  = out->left + m->icon;
    out->bottom = out->top  + m->icon;

    /* It has to fit, or the caller draws a clipped icon and a label pushed
     * somewhere it cannot be read. The box was already checked for size by
     * placeholder_is_visible(); this guards the degenerate metrics case. */
    if (out->right > box->right - m->pad) return FALSE;
    if (out->bottom > box->bottom - m->pad) return FALSE;
    return TRUE;
}

BOOL placeholder_name_rect(const PlaceholderMetrics *m, const RECT *box,
                           int min_top, RECT *out)
{
    if (m == NULL || box == NULL || out == NULL) return FALSE;
    if (rect_empty(box)) return FALSE;

    out->left   = box->left + m->pad + m->icon + m->pad;
    out->right  = box->right - m->pad;
    out->top    = label_top(m, box, min_top);
    out->bottom = out->top + m->icon;   /* the icon's row: the two line up */

    if (rect_empty(out)) return FALSE;
    if (out->right <= out->left) return FALSE;
    if (out->bottom > box->bottom - m->pad) return FALSE;
    return TRUE;
}

BOOL placeholder_size_rect(const PlaceholderMetrics *m, const RECT *box,
                           int min_top, RECT *out)
{
    if (m == NULL || box == NULL || out == NULL) return FALSE;
    if (rect_empty(box)) return FALSE;

    /* Same left edge as the name line, so the two rows of text line up even
     * when there is no icon to fill the left slot. */
    out->left   = box->left + m->pad + m->icon + m->pad;
    out->right  = box->right - m->pad;
    out->top    = label_top(m, box, min_top) + m->icon + 2;
    out->bottom = out->top + m->line_h;

    if (rect_empty(out)) return FALSE;

    /* The readout may be cut by the bottom of the box - a short box shows the
     * name and loses the size, rather than drawing it over the border. */
    if (out->bottom > box->bottom - m->pad) {
        out->bottom = box->bottom - m->pad;
    }
    if (rect_empty(out)) return FALSE;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Editing                                                             */
/* ------------------------------------------------------------------ */

BOOL placeholder_grip_rect(const PlaceholderMetrics *m, const RECT *box, RECT *out)
{
    if (m == NULL || box == NULL || out == NULL) return FALSE;
    if (rect_empty(box)) return FALSE;

    /*
     * A grip never takes more than a third of its box. On a box the size of a
     * business card, a full-size grip would cover the name and the size - the
     * two things the box exists to show - and the user would be resizing
     * something they can no longer read.
     */
    int g = m->grip;
    const int bw = box->right - box->left;
    const int bh = box->bottom - box->top;
    if (g > bw / 3) g = bw / 3;
    if (g > bh / 3) g = bh / 3;
    if (g <= 0) return FALSE;

    out->right  = box->right;
    out->bottom = box->bottom;
    out->left   = out->right - g;
    out->top    = out->bottom - g;
    return TRUE;
}

BOOL placeholder_close_rect(const PlaceholderMetrics *m, const RECT *box,
                            int min_top, RECT *out)
{
    if (m == NULL || box == NULL || out == NULL) return FALSE;
    if (rect_empty(box)) return FALSE;

    int g = m->grip;
    const int bw = box->right - box->left;
    const int bh = box->bottom - box->top;
    if (g > bw / 3) g = bw / 3;
    if (g > bh / 3) g = bh / 3;
    if (g <= 0) return FALSE;

    out->left   = box->right - g;
    out->right  = box->right;
    out->top    = box->top;
    out->bottom = box->top + g;

    /* A box dragged to the very top would put the button under the toolbar,
     * which owns the click there (overlay.c). Move it down instead - a button
     * that cannot be clicked is not a button. */
    if (out->top < min_top) {
        const int dy = min_top - out->top;
        out->top    += dy;
        out->bottom += dy;
        if (out->bottom > box->bottom) return FALSE;
    }
    return TRUE;
}

/* Half-open containment, the same rule the rest of the project uses. */
static BOOL rect_contains(const RECT *r, POINT pt)
{
    return (pt.x >= r->left && pt.x < r->right &&
            pt.y >= r->top  && pt.y < r->bottom) ? TRUE : FALSE;
}

PlaceholderHit placeholder_hit(const RECT *boxes, int count,
                               const PlaceholderMetrics *m, int min_top, POINT pt)
{
    PlaceholderHit none;
    none.kind  = PLACEHOLDER_HIT_NONE;
    none.index = -1;

    if (boxes == NULL || m == NULL || count <= 0) return none;

    /* Top of the stack first: the boxes were drawn in array order, so the last
     * one drawn is the one the user can see and reach. */
    for (int i = count - 1; i >= 0; --i) {
        RECT close;
        if (placeholder_close_rect(m, &boxes[i], min_top, &close) &&
            rect_contains(&close, pt)) {
            PlaceholderHit h;
            h.kind  = PLACEHOLDER_HIT_CLOSE;
            h.index = i;
            return h;
        }

        RECT grip;
        if (placeholder_grip_rect(m, &boxes[i], &grip) && rect_contains(&grip, pt)) {
            PlaceholderHit h;
            h.kind  = PLACEHOLDER_HIT_GRIP;
            h.index = i;
            return h;
        }

        if (rect_contains(&boxes[i], pt)) {
            PlaceholderHit h;
            h.kind  = PLACEHOLDER_HIT_BODY;
            h.index = i;
            return h;
        }
    }

    return none;
}

/* Keep `r` inside `area` by translating it, never by shrinking it. */
static void clamp_inside(RECT *r, const RECT *area)
{
    const int w = r->right - r->left;
    const int h = r->bottom - r->top;

    if (r->left < area->left) r->left = area->left;
    if (r->top  < area->top)  r->top  = area->top;
    if (r->left + w > area->right)  r->left = area->right - w;
    if (r->top  + h > area->bottom) r->top  = area->bottom - h;

    /* A box bigger than the canvas cannot be kept inside it; the top-left
     * corner is the best answer available. */
    if (r->left < area->left) r->left = area->left;
    if (r->top  < area->top)  r->top  = area->top;

    r->right  = r->left + w;
    r->bottom = r->top  + h;
}

BOOL placeholder_move_rect(const RECT *box, int dx, int dy,
                           const RECT *area, RECT *out)
{
    if (box == NULL || area == NULL || out == NULL) return FALSE;
    if (rect_empty(box) || rect_empty(area)) return FALSE;

    *out = *box;
    out->left   += dx;
    out->top    += dy;
    out->right  += dx;
    out->bottom += dy;

    clamp_inside(out, area);
    return TRUE;
}

BOOL placeholder_resize_rect(const RECT *box, POINT pt, const RECT *area,
                             int min_w, int min_h, RECT *out)
{
    if (box == NULL || area == NULL || out == NULL) return FALSE;
    if (rect_empty(box) || rect_empty(area)) return FALSE;

    if (min_w < 1) min_w = 1;
    if (min_h < 1) min_h = 1;

    *out = *box;
    out->right  = pt.x;
    out->bottom = pt.y;

    /* The top-left corner is the anchor: it does not move, whoever is dragging
     * the other corner. */
    if (out->right < out->left + min_w)   out->right = out->left + min_w;
    if (out->bottom < out->top + min_h)   out->bottom = out->top + min_h;
    if (out->right > area->right)         out->right = area->right;
    if (out->bottom > area->bottom)       out->bottom = area->bottom;

    if (rect_empty(out)) return FALSE;
    return TRUE;
}

BOOL placeholder_zone_from_rect(const RECT *box, const RECT *area, RelRect *out)
{
    if (box == NULL || area == NULL || out == NULL) return FALSE;
    if (rect_empty(box) || rect_empty(area)) return FALSE;

    const int w = area->right - area->left;
    const int h = area->bottom - area->top;

    ZeroMemory(out, sizeof(*out));
    out->x = (float)(box->left   - area->left) / (float)w;
    out->y = (float)(box->top    - area->top)  / (float)h;
    out->w = (float)(box->right  - box->left)  / (float)w;
    out->h = (float)(box->bottom - box->top)   / (float)h;

    if (out->x < 0.0f) out->x = 0.0f;
    if (out->y < 0.0f) out->y = 0.0f;
    if (out->w < 0.0f) out->w = 0.0f;
    if (out->h < 0.0f) out->h = 0.0f;
    if (out->x > 1.0f) out->x = 1.0f;
    if (out->y > 1.0f) out->y = 1.0f;
    if (out->w > 1.0f) out->w = 1.0f;
    if (out->h > 1.0f) out->h = 1.0f;

    /* The minimum a drawn zone carries, the same value sidebar_drop_zone()
     * produces: the placement path expects it. */
    out->min_w = 1;
    out->min_h = 1;
    return TRUE;
}
