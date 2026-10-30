/*
 * snapgrid.c - template geometry. See snapgrid.h.
 *
 * Every function here is a pure function of its arguments, so the numbers can
 * be checked against a known screen without a monitor attached - which matters,
 * because "the left half" being 0.4999 instead of 0.5 is the kind of error that
 * only shows up as a one-pixel seam nobody can explain.
 */

#include "snapgrid.h"

#include "toolbar.h"   /* ToolbarButton ids, so the mapping lives in one place */

/*
 * One cell of a uniform grid.
 *
 * Written once and used by all four templates: halves is a 2x1 grid, thirds a
 * 3x1, and so on. Four separate layouts would be four chances to get the gap
 * arithmetic slightly different, and the difference would only be visible when
 * two templates were compared side by side.
 */
static void grid_cell(RelRect *r, int col, int row, int cols, int rows,
                      int gap, int sw, int sh)
{
    const float cw = (float)(sw - gap * (cols - 1)) / (float)cols;
    const float ch = (float)(sh - gap * (rows - 1)) / (float)rows;

    r->x = (float)(col * (cw + (float)gap)) / (float)sw;
    r->y = (float)(row * (ch + (float)gap)) / (float)sh;
    r->w = cw / (float)sw;
    r->h = ch / (float)sh;
}

int snapgrid_build(SnapTemplate tpl, int gap_px,
                   int screen_w, int screen_h,
                   RelRect *out, int cap)
{
    if (out == NULL || cap < 1 || screen_w <= 0 || screen_h <= 0) {
        return 0;
    }
    if (gap_px < 0) {
        gap_px = 0;
    }

    int cols = 0;
    int rows = 0;
    switch (tpl) {
    case SNAP_HALVES: cols = 2; rows = 1; break;
    case SNAP_THIRDS: cols = 3; rows = 1; break;
    case SNAP_2X2:    cols = 2; rows = 2; break;
    case SNAP_3X3:    cols = 3; rows = 3; break;

    /*
     * 1+2 and 2+1 are a 2x2 grid with one column merged: two regions on one
     * side, one on the other. They are the two arrangements a three-window
     * desk keeps coming back to, and neither is a uniform grid, which is why
     * they cannot be expressed by cols/rows alone.
     */
    case SNAP_ONE_TWO:
    case SNAP_TWO_ONE: {
        if (cap < 3) return 0;
        if (gap_px * 1 >= screen_w || gap_px * 1 >= screen_h) gap_px = 0;

        RelRect c[4];
        grid_cell(&c[0], 0, 0, 2, 2, gap_px, screen_w, screen_h);
        grid_cell(&c[1], 1, 0, 2, 2, gap_px, screen_w, screen_h);
        grid_cell(&c[2], 0, 1, 2, 2, gap_px, screen_w, screen_h);
        grid_cell(&c[3], 1, 1, 2, 2, gap_px, screen_w, screen_h);

        if (tpl == SNAP_ONE_TWO) {
            /* one full-height column on the left, two regions on the right */
            out[0] = c[0];
            out[0].h = (c[2].y + c[2].h) - c[0].y;
            out[1] = c[1];
            out[2] = c[3];
        } else {
            /* two regions on the left, one full-height column on the right */
            out[0] = c[0];
            out[1] = c[2];
            out[2] = c[1];
            out[2].h = (c[3].y + c[3].h) - c[1].y;
        }
        return 3;
    }

    case SNAP_CUSTOM:
    default:
        /* Custom regions are the user's to draw (task 6.5). Inventing a
         * default here would be answering a question the user is about to
         * answer themselves. */
        return 0;
    }

    if (cols * rows > cap) {
        return 0;
    }

    /*
     * Gaps that would consume the axis leave nothing to divide. Dropping them
     * to zero keeps the template usable at absurd window sizes instead of
     * producing zero-width regions that look like a rendering bug.
     */
    if (gap_px * (cols - 1) >= screen_w || gap_px * (rows - 1) >= screen_h) {
        gap_px = 0;
    }

    int n = 0;
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            grid_cell(&out[n], col, row, cols, rows, gap_px, screen_w, screen_h);
            n++;
        }
    }
    return n;
}

const wchar_t *snapgrid_name(SnapTemplate tpl)
{
    switch (tpl) {
    case SNAP_HALVES: return L"halves";
    case SNAP_THIRDS: return L"thirds";
    case SNAP_ONE_TWO: return L"1+2";
    case SNAP_TWO_ONE: return L"2+1";
    case SNAP_2X2:    return L"2x2";
    case SNAP_3X3:    return L"3x3";
    case SNAP_CUSTOM: return L"custom";
    default:          break;
    }
    return L"?";
}

StrId snapgrid_label_id(SnapTemplate tpl)
{
    switch (tpl) {
    case SNAP_HALVES: return STR_TB_TPL_HALVES;
    case SNAP_THIRDS: return STR_TB_TPL_THIRDS;
    case SNAP_ONE_TWO: return STR_TB_TPL_1P2;
    case SNAP_TWO_ONE: return STR_TB_TPL_2P1;
    case SNAP_2X2:    return STR_TB_TPL_2X2;
    case SNAP_3X3:    return STR_TB_TPL_3X3;
    case SNAP_CUSTOM: return STR_TB_CUSTOM;
    default:          break;
    }
    /* Not a fallback that can be reached with a valid template: the default
     * arm only fires for a value outside the enum, and the layout button's own
     * caption is the least misleading thing to show then. */
    return STR_TB_LAYOUT;
}

/* ------------------------------------------------------------------ */
/* Snapping                                                            */
/* ------------------------------------------------------------------ */

static void push_line(int *arr, int *count, int value)
{
    if (*count >= SNAP_MAX_LINES) {
        return;
    }
    /*
     * Duplicates are harmless but waste the budget: a 3x3 grid whose cells share
     * borders would fill the list twice over with the same coordinates, and the
     * screen edges are usually already in it.
     */
    for (int i = 0; i < *count; ++i) {
        if (arr[i] == value) {
            return;
        }
    }
    arr[(*count)++] = value;
}

void snapgrid_collect_lines(const RelRect *regions, int count,
                            int screen_w, int screen_h,
                            SnapLines *out)
{
    if (out == NULL) {
        return;
    }
    out->count_x = 0;
    out->count_y = 0;

    /* The screen edges first: they are the targets that always exist, so a
     * user with no regions yet can still align to the edge of the display. */
    push_line(out->x, &out->count_x, 0);
    push_line(out->x, &out->count_x, screen_w);
    push_line(out->y, &out->count_y, 0);
    push_line(out->y, &out->count_y, screen_h);

    if (regions == NULL || count <= 0) {
        return;
    }

    for (int i = 0; i < count && i < SNAP_MAX_REGIONS; ++i) {
        push_line(out->x, &out->count_x, (int)(regions[i].x * (float)screen_w));
        push_line(out->x, &out->count_x,
                  (int)((regions[i].x + regions[i].w) * (float)screen_w));
        push_line(out->y, &out->count_y, (int)(regions[i].y * (float)screen_h));
        push_line(out->y, &out->count_y,
                  (int)((regions[i].y + regions[i].h) * (float)screen_h));
    }
}

/* Nearest line to `v`, or `v` itself when nothing is within the threshold. */
static int snap_value(int v, const int *lines, int count, int threshold)
{
    int best_dist = threshold + 1;
    int best_line = v;

    for (int i = 0; i < count; ++i) {
        const int d = (v > lines[i]) ? (v - lines[i]) : (lines[i] - v);
        if (d < best_dist) {
            best_dist = d;
            best_line = lines[i];
        }
    }
    return best_line;
}

void snapgrid_snap_rect(RECT *r, const SnapLines *lines, int threshold_px)
{
    if (r == NULL || lines == NULL) {
        return;
    }
    if (threshold_px < 0) {
        threshold_px = 0;
    }

    /*
     * The four edges are snapped independently, and deliberately not in an
     * order that could fight itself: an edge that has already been moved is not
     * re-examined, so a rectangle can align its left edge to a column and its
     * right edge to another without the first move changing the second's
     * answer.
     */
    r->left   = snap_value((int)r->left,   lines->x, lines->count_x, threshold_px);
    r->right  = snap_value((int)r->right,  lines->x, lines->count_x, threshold_px);
    r->top    = snap_value((int)r->top,    lines->y, lines->count_y, threshold_px);
    r->bottom = snap_value((int)r->bottom, lines->y, lines->count_y, threshold_px);
}

/* ------------------------------------------------------------------ */
/* Filling a dragged box into the nearest region (task 7.3)            */
/* ------------------------------------------------------------------ */

BOOL snapgrid_edge_hit(int screen_w, int screen_h, POINT pt, int band_px)
{
    if (screen_w <= 0 || screen_h <= 0) {
        return FALSE;
    }
    if (band_px < 0) {
        band_px = 0;
    }

    /* Outside the screen is not "at its edge": this is the point the user was
     * aiming with, and a point that left the desktop is a different move. */
    if (pt.x < 0 || pt.x >= screen_w || pt.y < 0 || pt.y >= screen_h) {
        return FALSE;
    }

    if (pt.x < band_px || pt.x >= screen_w - band_px) return TRUE;
    if (pt.y < band_px || pt.y >= screen_h - band_px) return TRUE;
    return FALSE;
}

int snapgrid_pick(const RelRect *regions, int count,
                  int screen_w, int screen_h, POINT pt, float max_frac)
{
    if (regions == NULL || count <= 0 || screen_w <= 0 || screen_h <= 0) {
        return -1;
    }
    if (max_frac < 0.0f) {
        max_frac = 0.0f;
    }

    const float  shorter = (float)((screen_w < screen_h) ? screen_w : screen_h);
    const double limit   = (double)max_frac * (double)shorter;
    const double limit2  = limit * limit;

    int    best   = -1;
    double best2  = 0.0;

    for (int i = 0; i < count; ++i) {
        const int l = (int)(regions[i].x * (float)screen_w);
        const int t = (int)(regions[i].y * (float)screen_h);
        const int r = (int)((regions[i].x + regions[i].w) * (float)screen_w);
        const int b = (int)((regions[i].y + regions[i].h) * (float)screen_h);

        int dx = 0;
        int dy = 0;
        if (pt.x < l)        dx = l - pt.x;
        else if (pt.x >= r)  dx = pt.x - (r - 1);
        if (pt.y < t)        dy = t - pt.y;
        else if (pt.y >= b)  dy = pt.y - (b - 1);

        const double d2 = (double)dx * (double)dx + (double)dy * (double)dy;
        if (best < 0 || d2 < best2) {
            best  = i;
            best2 = d2;
        }
        /* Inside a region: nothing can be closer. */
        if (d2 == 0.0) {
            break;
        }
    }

    if (best < 0) {
        return -1;
    }
    return (best2 <= limit2) ? best : -1;
}
