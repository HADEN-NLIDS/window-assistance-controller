/*
 * toolbar.h - the edit toolbar: where its buttons are, and which one was hit.
 *
 * WHY THIS IS NOT A CHILD WINDOW
 *
 *   docs/分段式编程计划.md specifies a WS_CHILD sub-window with its own
 *   WM_PAINT, about 200 lines. That does not work in this design. The overlay
 *   is a WS_EX_LAYERED window, and a layered window is composited as a single
 *   unit - child windows inside it are not drawn. A sub-window toolbar would
 *   be invisible, and the symptom would read as "the toolbar is missing"
 *   rather than "the architecture cannot express this".
 *
 *   So the toolbar is geometry, not a window. This module answers two pure
 *   questions - where do the buttons go, and which one is under this point -
 *   and overlay.c draws them and routes the mouse. That is less code than a
 *   sub-window, it cannot disagree with what is on screen, and the hit testing
 *   becomes testable with no window at all.
 *
 *   This is a deviation from the plan, recorded in docs/进度记录.md.
 *
 * WHAT THE LABELS ARE NOT HERE FOR
 *
 *   The captions come from the string table via toolbar_label_id(), which
 *   returns a StrId rather than the text. That keeps this module linkable into
 *   tests that have no i18n, and keeps the layout independent of which
 *   language is loaded - the button widths are fixed, so a longer translation
 *   cannot shift the buttons out from under the mouse.
 */

#ifndef WAC_TOOLBAR_H
#define WAC_TOOLBAR_H

#include <windows.h>

#include "i18n.h"   /* StrId - the type only; no string table is linked */

#ifdef __cplusplus
extern "C" {
#endif

/* Strip height. overlay.c reserves exactly this much at the top of the screen.
 *
 * This is the BASE value, measured in physical pixels at 96 dpi. Everything in
 * this header is a 96-dpi number: see the DPI note below. */
#define TOOLBAR_HEIGHT 64

/* ------------------------------------------------------------------ */
/* DPI                                                                 */
/* ------------------------------------------------------------------ */

/*
 * WHY THIS EXISTS, AND HOW IT WAS MEASURED
 *
 *   Every length in this module was originally a fixed physical pixel count,
 *   chosen on a 27" 2560x1440 panel (109 ppi). The stage-7 review measured what
 *   that means on the other monitor of the development pair - a 16"
 *   2560x1600 laptop panel at 150% scaling, i.e. 189 ppi: the SAME 22-px
 *   caption is 5.1 mm tall on the desktop panel and 2.96 mm on the laptop
 *   panel, **1.7x smaller**, while every other control on that screen grows
 *   1.5x. The strip and its panels therefore read as cramped and small exactly
 *   where the user's own UI is large.
 *
 *   The fix is the one Windows itself uses: scale the geometry and the fonts by
 *   dpi/96. The numbers below keep their 96-dpi values; toolbar_px() converts.
 *
 *   The scale is set ONCE per overlay, before anything is laid out, and the
 *   geometry functions read it. It is one number for one process (only one
 *   overlay exists at a time), which is why it is module state and not another
 *   parameter on six functions - the same arrangement i18n_init() uses for the
 *   language.
 */
void toolbar_set_dpi(int dpi);

/* The dpi currently in effect (96 when nobody has called the setter). */
int toolbar_dpi(void);

/* A 96-dpi length in physical pixels at the current dpi. */
int toolbar_px(int base);

/* The strip's height at the current dpi: what overlay.c must reserve. */
int toolbar_strip_h(void);

typedef enum {
    TB_EXIT = 0,      /* leave edit mode                  */
    TB_RESET,         /* clear every region               */
    TB_APPLY,         /* write the regions out as presets */
    TB_LAYOUT,        /* open the layout panel: the four templates and
                       * custom, as a grid of thumbnails. It replaces the five
                       * buttons those used to be - the strip had grown to nine
                       * of them, and a set of mutually exclusive choices is
                       * what a panel is for (requested by the user 2026-10-09) */
    TB_PICK,          /* pick up the current layout       */
    TB_COUNT
} ToolbarButton;

typedef struct {
    RECT          rect;
    ToolbarButton id;
    BOOL          separator_before;   /* the rule the plan draws between groups */
    BOOL          enabled;
} ToolbarItem;

/*
 * Lay the buttons out inside `strip`, in overlay client coordinates.
 *
 * Pure: no window, no DC, no globals, so a test can pass any rectangle and
 * check the arithmetic.
 *
 * Returns the number of items written, or 0 when the strip is too narrow to
 * hold even the exits; the caller then draws nothing rather than drawing
 * buttons that overlap or run off the edge.
 *
 * `enabled` is taken from `mask`, a bitmask of 1u << ToolbarButton. Buttons
 * whose feature does not exist yet are laid out but disabled, so the operator
 * sees the shape of the toolbar and cannot press something that does nothing -
 * the same rule that kept stage 5's listener switch greyed out until it worked.
 */
int toolbar_layout(const RECT *strip, unsigned mask,
                   ToolbarItem *out, int cap);

/*
 * Which button is at `pt`?
 *
 * Returns TB_COUNT for a gap, a separator, a disabled button, or a point
 * outside every button - a button that cannot be used must not answer as if it
 * could.
 */
ToolbarButton toolbar_hit_test(const ToolbarItem *items, int count, POINT pt);

/* The i18n id of a button's caption. */
StrId toolbar_label_id(ToolbarButton b);

/* Stable name for logs and status lines. Not translated: logs are not UI. */
const wchar_t *toolbar_button_name(ToolbarButton b);

/* The width a button is drawn at. Exposed so overlay.c and this module cannot
 * disagree about it. */
int toolbar_button_width(ToolbarButton b);

/* ------------------------------------------------------------------ */
/* The name box (renaming lives inside edit mode, by design)           */
/* ------------------------------------------------------------------ */

/*
 * The name box, and the two buttons that appear beside it while the user is
 * typing. This is the design recorded in docs/手册.md section 8.4: the name of
 * the thing being edited is shown at the LEFT end of the strip, the action
 * buttons stay right-aligned, and the two ends meet in the middle.
 *
 * The box is ALWAYS drawn - it shows the current name, which is how the user
 * tells two workspaces apart once the overlay covers the screen. Only the two
 * buttons come and go, and their space is reserved whether they are drawn or
 * not: the design requires the box NOT to move when the user clicks into it,
 * because a text box that jumps under the cursor is a text box that loses the
 * click that was meant to open it.
 */
typedef struct {
    RECT name;      /* the box itself                                    */
    RECT save;      /* [save]   - reserved always, drawn while editing   */
    RECT cancel;    /* [cancel] - reserved always, drawn while editing   */
    BOOL valid;     /* FALSE when the strip cannot hold the box          */
} ToolbarNameBox;

/* Narrowest box that is still worth drawing. The strip needs this much spare
 * room to the left of the buttons before the box appears at all. */
#define TOOLBAR_NAME_MIN_W 120

/*
 * Lay out the name box (and the two reserved buttons) to the left of `items`.
 *
 * Pure, like toolbar_layout(): `items`/`count` are that function's output, and
 * the box takes the space between the left margin and the first button.
 *
 * The box has no fixed width - it takes what is left after the two buttons are
 * reserved. That is what "on a narrow screen the box gives way to the left"
 * means in practice: the BOX shortens, the buttons never move.
 *
 * Returns FALSE, writing nothing, when less than TOOLBAR_NAME_MIN_W would be
 * left for the box. The caller then draws no box at all, which is better than
 * one drawn across the buttons.
 */
BOOL toolbar_name_layout(const RECT *strip, const ToolbarItem *items, int count,
                         ToolbarNameBox *out);

/* Width reserved for each of the two buttons. Exposed so overlay.c draws them
 * at the size this module placed them. */
int toolbar_name_button_width(void);

/* ------------------------------------------------------------------ */
/* The layout panel (what TB_LAYOUT opens)                             */
/* ------------------------------------------------------------------ */

/*
 * A two-column grid of thumbnails, one per SnapTemplate, in enum order - so
 * the item index IS the template and no mapping table is needed anywhere.
 *
 * The geometry lives here, beside the button it hangs from, and stays pure:
 * a panel near the edge of a narrow screen has to land somewhere sensible, and
 * that is arithmetic a test can check without a screen (same reason the strip
 * arithmetic lives here rather than in overlay.c).
 *
 * The panel's POSITION comes from popup_place_below(); this module only sizes
 * it and says where the items sit inside it.
 */
/* The numbers in this header are 96-dpi BASES; toolbar.c scales them. */
#define TB_PANEL_COLS   2
#define TB_PANEL_CELL_W 130
#define TB_PANEL_CELL_H 104
#define TB_PANEL_PAD    10

/* The size a panel with `item_count` items needs. Never reports 0 for a
 * positive count: an empty panel would be invisible and still swallow clicks. */
void toolbar_panel_size(int item_count, int *out_w, int *out_h);

/*
 * Rectangle of item `index`, in whatever coordinates `panel` is given in.
 * FALSE for an out-of-range index or a NULL argument.
 */
BOOL toolbar_panel_item_rect(const RECT *panel, int item_count, int index, RECT *out);

/*
 * The part of an item the thumbnail is drawn in - the item minus the caption
 * strip at the bottom. overlay.c draws the thumbnail here and the label under
 * it, from one rectangle, so the two cannot disagree.
 */
BOOL toolbar_panel_thumb_rect(const RECT *panel, int item_count, int index, RECT *out);

/* Which item is at `pt`, or -1. */
int toolbar_panel_hit(const RECT *panel, int item_count, POINT pt);

/* Height of the caption strip at the bottom of an item.
 *
 * 32, not 26. MEASURED 2026-10-09, reported by the user looking at the screen:
 * at 26 the strip left 22 px for the text after the 4 px gap above it, and the
 * bottom of a CJK glyph - whose line box is taller than the Latin one - was
 * clipped by roughly a tenth of its height. No assertion noticed, because none
 * of them looks at a font; the screenshot that would have shown it was examined
 * at a third of its size. */
#define TB_PANEL_CAPTION_H 32

/* ------------------------------------------------------------------ */
/* The monitor box (top centre) and its dropdown                       */
/* ------------------------------------------------------------------ */

/*
 * Which monitor the edit overlay covers, shown in the MIDDLE of the strip and
 * clickable to move the overlay to another one (requested by the user
 * 2026-10-09).
 *
 * It sits between the name box on the left and the buttons on the right - the
 * only piece of the strip that was empty - and it GIVES WAY to both rather
 * than overlapping either: on a narrow screen it narrows, and when even that
 * is not enough it is left out entirely. A control drawn across another one is
 * worse than a missing control, which is the rule the name box already
 * follows.
 */
#define TOOLBAR_MON_W      220
#define TOOLBAR_MON_H       28
#define TOOLBAR_MON_ITEM_H  32
#define TOOLBAR_MON_PAD      4

/*
 * The box in the strip. `nb` may be NULL or invalid (no name box on this
 * screen) and `items`/`count` may be empty. Returns FALSE when what is left in
 * the middle is too small to be worth drawing.
 */
BOOL toolbar_monitor_box(const RECT *strip, const ToolbarItem *items, int count,
                         const ToolbarNameBox *nb, RECT *out);

/* Height of a dropdown holding `item_count` monitors, padding included. */
int toolbar_monitor_list_height(int item_count);

/* The box's own width and height at the current dpi. overlay.c draws it with
 * these rather than with the 96-dpi macros, so the drawn box and the hit-test
 * rectangle cannot disagree on a scaled monitor. */
int toolbar_monitor_w(void);
int toolbar_monitor_h(void);

/*
 * Rectangle of item `index` inside the dropdown panel, which popup.c has
 * already placed. One column, one row per monitor.
 */
BOOL toolbar_monitor_item_rect(const RECT *panel, int item_count, int index,
                               RECT *out);

/* Which monitor is at `pt` in an open dropdown, or -1. */
int toolbar_monitor_hit(const RECT *panel, int item_count, POINT pt);

#ifdef __cplusplus
}
#endif

#endif /* WAC_TOOLBAR_H */
