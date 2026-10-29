/*
 * toolbar.c - toolbar geometry. See toolbar.h for why this is not a window.
 *
 * Everything here is a pure function of its arguments: no globals, no DC, no
 * window handles. That is what lets tests/t50_overlay exercise the hit testing
 * and the layout arithmetic without a screen, and it is the reason the buttons
 * cannot end up somewhere other than where the mouse believes they are.
 */

#include "toolbar.h"

/* Button height. */
#define BUTTON_H_BASE 36
/* Button height at the current dpi. */
#define BUTTON_H S(BUTTON_H_BASE)

/* ------------------------------------------------------------------ */
/* DPI                                                                 */
/* ------------------------------------------------------------------ */

/*
 * The scale in effect. 96 = the numbers below, unchanged - which is what keeps
 * every existing test assertion valid, since the test machine's primary panel
 * is 100%.
 */
static int g_dpi = 96;

void toolbar_set_dpi(int dpi)
{
    /* Anything outside the range Windows actually reports is treated as 96:
     * a bad dpi must not be able to make the strip taller than the screen. */
    g_dpi = (dpi >= 48 && dpi <= 480) ? dpi : 96;
}

int toolbar_dpi(void)
{
    return g_dpi;
}

int toolbar_px(int base)
{
    return MulDiv(base, g_dpi, 96);
}

int toolbar_strip_h(void)
{
    return toolbar_px(TOOLBAR_HEIGHT);
}

/* Every length in this file goes through here. */
#define S(x) toolbar_px(x)

/* The header's bases, at the current dpi. The macros keep their plain values
 * in the header so that every includer sees a number, not a call into this
 * file's private state. */
#define TB_PANEL_PAD_S       S(TB_PANEL_PAD)
#define TB_PANEL_CELL_W_S    S(TB_PANEL_CELL_W)
#define TB_PANEL_CELL_H_S    S(TB_PANEL_CELL_H)
#define TB_PANEL_CAPTION_H_S S(TB_PANEL_CAPTION_H)
#define TOOLBAR_MON_W_S      S(TOOLBAR_MON_W)
#define TOOLBAR_MON_H_S      S(TOOLBAR_MON_H)
#define TOOLBAR_MON_ITEM_H_S S(TOOLBAR_MON_ITEM_H)
#define TOOLBAR_MON_PAD_S    S(TOOLBAR_MON_PAD)

/* Left and right margin of the whole strip, the gap between buttons, and the
 * extra space a group separator needs. 96-dpi values. */
#define MARGIN_X S(16)
#define GAP_X    S(10)
#define SEP_X    S(18)

/*
 * Button widths, in physical pixels.
 *
 * Fixed rather than measured from the caption. Measuring would need a DC, and
 * that would make the layout depend on whichever font happened to be selected
 * when the paint ran - so the buttons could move between two paints of the
 * same state. It would also let a longer translation push the last button off
 * the edge of the screen.
 *
 * KNOWN LIMITATION: these are not scaled by the monitor DPI. At 150% they read
 * as small but usable; on a high-DPI display the strip wants a scale factor
 * passed in. Recorded rather than guessed at, because the development machine
 * has one monitor and guessing a scaling rule from it would be inventing
 * evidence. The widest caption ("拾取当前布局" / "Pick up this layout") sets
 * the width of TB_PICK.
 */
static const int k_width[TB_COUNT] = {
    130,   /* TB_EXIT    */
     90,   /* TB_RESET   */
     90,   /* TB_APPLY   */
    130,   /* TB_LAYOUT  */
    170    /* TB_PICK    */
};

/*
 * Which buttons open a new group, by button id. Read against k_order below,
 * which is the order they actually appear in.
 *
 * The strip is now
 *   | [exit] [reset] [apply] | [layout] | [pick]
 * and the rule tells the operator that the middle group replaces the regions
 * wholesale while the right-hand button reads them off the screen.
 *
 * The layout button is its own group because it opens a PANEL rather than
 * acting on the canvas immediately - a button whose press does not change what
 * is underneath it should not look like the ones that do.
 */
static const BOOL k_sep[TB_COUNT] = {
    FALSE,  /* TB_EXIT    */
    FALSE,  /* TB_RESET   */
    TRUE,   /* TB_APPLY   - layout | actions */
    TRUE,   /* TB_LAYOUT  - pick | layout    */
    FALSE   /* TB_PICK    */
};

/*
 * Display order, left to right.
 *
 * REQUESTED 2026-09-22 by the user: the strip is right-aligned and [exit] sits
 * at the far right, where every Windows window keeps the control that ends the
 * thing you are in. The overlay's top edge reads as a title bar - a full-width
 * band across the top of the screen - so hunting for the way out on its left is
 * the wrong habit to teach, whatever the reading order of the labels.
 */
static const ToolbarButton k_order[TB_COUNT] = {
    TB_PICK, TB_LAYOUT, TB_APPLY, TB_RESET, TB_EXIT
};

int toolbar_button_width(ToolbarButton b)
{
    if (b < 0 || b >= TB_COUNT) {
        return 0;
    }
    return S(k_width[b]);
}

StrId toolbar_label_id(ToolbarButton b)
{
    switch (b) {
    case TB_EXIT:       return STR_TB_EXIT;
    case TB_RESET:      return STR_TB_RESET;
    case TB_APPLY:      return STR_TB_APPLY;
    case TB_LAYOUT:     return STR_TB_LAYOUT;
    case TB_PICK:       return STR_TB_PICK;
    default:            break;
    }
    return STR_TB_EXIT;
}

const wchar_t *toolbar_button_name(ToolbarButton b)
{
    /* English and untranslated on purpose: these strings go into the log, and
     * a log that changes language with the UI is a log nobody can grep. */
    switch (b) {
    case TB_EXIT:       return L"exit";
    case TB_RESET:      return L"reset";
    case TB_APPLY:      return L"apply";
    case TB_LAYOUT:     return L"layout";
    case TB_PICK:       return L"pick";
    default:            break;
    }
    return L"?";
}

/*
 * Width the BUTTONS need at their natural spacing, margins included.
 *
 * Two callers need this, and they must not be confused with each other:
 * right-alignment starts the row here, and the "does the toolbar fit at all"
 * test adds the name box on top of it.
 *
 * MEASURED 2026-09-23, the hard way: folding the name box into this number
 * pushed the whole button row 130 px to the left and left a bare strip at the
 * right edge - exactly what right-alignment exists to prevent. A screenshot
 * from the user found it; the geometry assertions did not, because they only
 * checked that the parts do not OVERLAP, never that the row reaches the edge.
 */
static int buttons_width_needed(void)
{
    int total = MARGIN_X * 2;
    for (int i = 0; i < TB_COUNT; ++i) {
        if (k_sep[i]) {
            total += SEP_X;
        }
        total += S(k_width[i]) + GAP_X;
    }
    /* The last button is not followed by a gap. */
    return total - GAP_X;
}

/*
 * Everything the strip must be able to hold before it is worth drawing at all:
 * the buttons, plus the name box's minimum.
 *
 * The two buttons beside the box are deliberately NOT counted. Counting them
 * would make the whole toolbar disappear on a screen that could still show it,
 * and a missing toolbar is worse than a missing name box: the toolbar is the
 * only visible way out of edit mode. When the full set does not fit,
 * toolbar_name_layout() reports that and the box is left out while the buttons
 * stay where they are.
 */
static int strip_width_needed(void)
{
    return buttons_width_needed() + S(TOOLBAR_NAME_MIN_W) + GAP_X;
}

int toolbar_layout(const RECT *strip, unsigned mask, ToolbarItem *out, int cap)
{
    if (strip == NULL || out == NULL || cap < TB_COUNT) {
        return 0;
    }

    const int width = (int)(strip->right - strip->left);
    if (width < strip_width_needed()) {
        /*
         * Not enough room. Reporting zero is the honest answer: the caller
         * draws nothing, and the operator sees a screen with no toolbar rather
         * than one with buttons piled on top of each other or hanging off the
         * edge where they cannot be clicked.
         */
        return 0;
    }

    const int top = strip->top + ((int)(strip->bottom - strip->top) - BUTTON_H) / 2;

    /*
     * Right-aligned. The buttons are still walked left to right - so the
     * separator logic reads in display order - but the starting x is derived
     * from the right edge and the width the BUTTONS need. Deriving it from
     * strip_width_needed() instead is what left the bare strip described above.
     */
    const int content = buttons_width_needed() - MARGIN_X * 2;
    int x = (int)strip->right - MARGIN_X - content;

    for (int i = 0; i < TB_COUNT; ++i) {
        const ToolbarButton id = k_order[i];
        ToolbarItem *it = &out[i];

        if (k_sep[id]) {
            x += SEP_X;
        }

        it->id               = id;
        it->separator_before = k_sep[id];
        it->enabled          = ((mask >> id) & 1u) != 0u;
        it->rect.left        = x;
        it->rect.top         = top;
        it->rect.right       = x + S(k_width[id]);
        it->rect.bottom      = top + BUTTON_H;

        x = it->rect.right + GAP_X;
    }
    return TB_COUNT;
}

ToolbarButton toolbar_hit_test(const ToolbarItem *items, int count, POINT pt)
{
    if (items == NULL) {
        return TB_COUNT;
    }

    for (int i = 0; i < count; ++i) {
        if (!items[i].enabled) {
            /* A disabled button is not a button. Answering with its id would
             * let the caller act on a press the user could not have made. */
            continue;
        }
        if (PtInRect(&items[i].rect, pt)) {
            return items[i].id;
        }
    }
    return TB_COUNT;
}

/* ------------------------------------------------------------------ */
/* The name box                                                        */
/* ------------------------------------------------------------------ */

/*
 * [save] and [cancel] are narrower than the action buttons: two CJK glyphs and
 * their padding. Fixed for the same reason every other width here is fixed -
 * measuring text needs a DC, and then the geometry would depend on whichever
 * font the paint happened to select.
 */
#define NAME_BTN_W S(76)

int toolbar_name_button_width(void)
{
    return NAME_BTN_W;
}

BOOL toolbar_name_layout(const RECT *strip, const ToolbarItem *items, int count,
                         ToolbarNameBox *out)
{
    RECT name, save, cancel;

    if (strip == NULL || items == NULL || out == NULL || count <= 0) {
        return FALSE;
    }

    /*
     * Everything left of the first button is ours. toolbar_layout() walks
     * k_order, so items[0] is the leftmost button whatever that order becomes.
     */
    const int limit = items[0].rect.left - GAP_X;

    const int top    = strip->top + ((int)(strip->bottom - strip->top) - BUTTON_H) / 2;
    const int bottom = top + BUTTON_H;

    /*
     * Placed right to left: [cancel] [save] [name box]. Their positions depend
     * only on where the buttons are, never on whether the user is typing -
     * which is what keeps the box still when it is clicked, as the design
     * requires.
     */
    cancel.left   = limit - NAME_BTN_W;
    cancel.top    = top;
    cancel.right  = limit;
    cancel.bottom = bottom;

    save.left   = cancel.left - GAP_X - NAME_BTN_W;
    save.top    = top;
    save.right  = cancel.left - GAP_X;
    save.bottom = bottom;

    name.left   = strip->left + MARGIN_X;
    name.top    = top;
    name.right  = save.left - GAP_X;
    name.bottom = bottom;

    /*
     * The name box gives way to the monitor box in the middle (2026-10-09).
     *
     * Without this the box takes everything up to the buttons - MEASURED 1660
     * px wide on a 2560 screen (t50's old "[narrow box width] 1000 (was
     * 1660)") - and the middle of the strip is left with nothing, so the
     * monitor box is placed, found to have no room, and not drawn at all. The
     * user asked for the monitor box to be "in the middle of the top UI"; a
     * name box that filled the strip is what stopped that being possible.
     *
     * The name box is still the thing that shortens when space runs out - the
     * buttons never move and the monitor box keeps its width - so the priority
     * order is: buttons, monitor box, name box.
     */
    {
        const int mon_box_left = (strip->left + strip->right - TOOLBAR_MON_W_S) / 2;
        const int name_max     = mon_box_left - GAP_X;
        if (name.right > name_max) name.right = name_max;
    }

    if (name.right - name.left < S(TOOLBAR_NAME_MIN_W)) {
        /* Not enough room beside the buttons. Say so rather than draw a box
         * that overlaps them - the caller leaves the name out entirely. */
        return FALSE;
    }

    out->name   = name;
    out->save   = save;
    out->cancel = cancel;
    out->valid  = TRUE;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* The layout panel                                                    */
/* ------------------------------------------------------------------ */

void toolbar_panel_size(int item_count, int *out_w, int *out_h)
{
    /* At least one row: a panel for zero items is a degenerate case the caller
     * should not ask for, but returning 0 x 0 would make it invisible AND
     * click-swallowing, which is worse than a slightly oversized empty box. */
    const int rows = (item_count > 0)
                         ? (item_count + TB_PANEL_COLS - 1) / TB_PANEL_COLS
                         : 1;

    if (out_w != NULL) *out_w = TB_PANEL_PAD_S * 2 + TB_PANEL_COLS * TB_PANEL_CELL_W_S;
    if (out_h != NULL) *out_h = TB_PANEL_PAD_S * 2 + rows * TB_PANEL_CELL_H_S;
}

BOOL toolbar_panel_item_rect(const RECT *panel, int item_count, int index, RECT *out)
{
    if (panel == NULL || out == NULL) return FALSE;
    if (index < 0 || index >= item_count) return FALSE;

    /* Row-major: item 0 and 1 are the first row, left to right. */
    const int col = index % TB_PANEL_COLS;
    const int row = index / TB_PANEL_COLS;

    out->left   = panel->left + TB_PANEL_PAD_S + col * TB_PANEL_CELL_W_S;
    out->top    = panel->top  + TB_PANEL_PAD_S + row * TB_PANEL_CELL_H_S;
    out->right  = out->left + TB_PANEL_CELL_W_S;
    out->bottom = out->top  + TB_PANEL_CELL_H_S;
    return TRUE;
}

BOOL toolbar_panel_thumb_rect(const RECT *panel, int item_count, int index, RECT *out)
{
    if (!toolbar_panel_item_rect(panel, item_count, index, out)) return FALSE;

    /* Inset so neighbouring thumbnails do not touch, and stop above the
     * caption strip. Both the thumbnail and the caption come from this one
     * rectangle, which is what keeps them from overlapping. */
    const int inset = S(8);
    out->left   += inset;
    out->right  -= inset;
    out->top    += inset;
    out->bottom -= TB_PANEL_CAPTION_H_S;

    return (out->right > out->left && out->bottom > out->top) ? TRUE : FALSE;
}

int toolbar_panel_hit(const RECT *panel, int item_count, POINT pt)
{
    if (panel == NULL || item_count <= 0) return -1;

    for (int i = 0; i < item_count; ++i) {
        RECT r;
        if (toolbar_panel_item_rect(panel, item_count, i, &r) && PtInRect(&r, pt)) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* The monitor box and its dropdown                                    */
/* ------------------------------------------------------------------ */

BOOL toolbar_monitor_box(const RECT *strip, const ToolbarItem *items, int count,
                         const ToolbarNameBox *nb, RECT *out)
{
    if (strip == NULL || out == NULL) return FALSE;

    /* Centred on the strip first, then pulled back inside whatever the two
     * ends already occupy - the box gives way, they never move. */
    int left  = (strip->left + strip->right - TOOLBAR_MON_W_S) / 2;
    int right = left + TOOLBAR_MON_W_S;

    if (nb != NULL && nb->valid) {
        const int limit = nb->name.right + GAP_X;
        if (left < limit) left = limit;
    }
    if (items != NULL && count > 0) {
        /* items[0] is the leftmost button: k_order puts the strip's left end
         * first, so the box never has to look past one of them. */
        const int limit = items[0].rect.left - GAP_X;
        if (right > limit) right = limit;
    }

    const int outer_l = strip->left + MARGIN_X;
    const int outer_r = strip->right - MARGIN_X;
    if (left  < outer_l) left  = outer_l;
    if (right > outer_r) right = outer_r;

    /* Half the design width or nothing at all: a two-centimetre box would show
     * a truncated monitor name, which is worse than showing no box - the same
     * judgement toolbar_name_layout() makes about its own minimum. */
    if (right - left < TOOLBAR_MON_W_S / 2) return FALSE;

    const int top = strip->top + ((int)(strip->bottom - strip->top) - TOOLBAR_MON_H_S) / 2;
    out->left   = left;
    out->top    = top;
    out->right  = right;
    out->bottom = top + TOOLBAR_MON_H_S;
    return TRUE;
}

int toolbar_monitor_w(void) { return TOOLBAR_MON_W_S; }

int toolbar_monitor_h(void) { return TOOLBAR_MON_H_S; }

int toolbar_monitor_list_height(int item_count){
    if (item_count <= 0) return 0;
    return item_count * TOOLBAR_MON_ITEM_H_S + TOOLBAR_MON_PAD_S * 2;
}

BOOL toolbar_monitor_item_rect(const RECT *panel, int item_count, int index,
                               RECT *out)
{
    if (panel == NULL || out == NULL) return FALSE;
    if (index < 0 || index >= item_count) return FALSE;

    out->left   = panel->left;
    out->top    = panel->top + TOOLBAR_MON_PAD_S + index * TOOLBAR_MON_ITEM_H_S;
    out->right  = panel->right;
    out->bottom = out->top + TOOLBAR_MON_ITEM_H_S;
    return TRUE;
}

int toolbar_monitor_hit(const RECT *panel, int item_count, POINT pt)
{
    if (panel == NULL || item_count <= 0) return -1;

    for (int i = 0; i < item_count; ++i) {
        RECT r;
        if (toolbar_monitor_item_rect(panel, item_count, i, &r) && PtInRect(&r, pt)) {
            return i;
        }
    }
    return -1;
}
