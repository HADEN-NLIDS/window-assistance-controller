/*
 * overlay.c - the edit overlay window. See overlay.h for the reasoning.
 *
 * Resource discipline is the theme of this file. Everything GDI it creates is
 * created once, in overlay_create(), and released in overlay_destroy(), and
 * the module counts its own objects so tests/t51_release.c can ask it directly
 * rather than guessing. Task 6.9's rule is that entering and leaving edit mode
 * fifty times must not grow the handle count; a module that cannot state how
 * many objects it holds cannot honour that.
 */

#include "overlay.h"
#include "toolbar.h"    /* the strip's geometry; toolbar.h says why it is not a window */
#include "i18n.h"       /* i18n_text, for the button captions */
#include "snapgrid.h"   /* template region geometry (task 6.4) */
#include "popup.h"      /* the layout panel: state, hover rule, placement */
#include "sidebar.h"    /* the slide-out application list (stage 8, task 2b) */
#include "iconcache.h"  /* the rows' icons, with a ceiling (stage 8, task 2b) */
#include "placeholder.h"/* the boxes for the windows a workspace places (stage 8) */

#include <stdio.h>      /* _snwprintf_s */
#include <stdlib.h>     /* calloc / free                                            */
                        /*
                         * That include is not decoration. Without it MSVC assumes
                         * calloc returns int, truncates the 64-bit pointer to 32
                         * bits on x64, and the first write through the result is an
                         * access violation. C4013 is only a warning, and
                         * tests/build_tests.cmd compiles with /WX-, so the build
                         * stays green while every run dies at 0xC0000005.
                         * MEASURED 2026-09-22: exactly that, on the first
                         * overlay_create of t50_overlay --loop 1.
                         */

#include "../core/monitor.h"
#include "../util/log.h"

#define WAC_OVERLAY_CLASS L"WacEditOverlay"

/* The wash. 150/255 keeps real windows readable through it, which is the
 * property the whole design depends on - the user must be able to see what
 * they are arranging. */
#define OVERLAY_ALPHA   150
#define OVERLAY_BG_R    15
#define OVERLAY_BG_G    30
#define OVERLAY_BG_B    60

/* Reserved strip at the top for the toolbar (task 6.3). toolbar.h owns the
 * height so the drawn strip and the hit-test rectangles cannot disagree. */
#define TOOLBAR_H toolbar_strip_h()   /* DPI-scaled: 64 at 96 dpi, 96 at 144 */

/*
 * Longest name the box will hold, including the terminator.
 *
 * Deliberately not WS_NAME_MAX from model/workspace.h: the overlay does not
 * know what it is editing (that is the whole point of the callback design), so
 * it must not depend on the workspace model. 128 matches the workspace name
 * limit with room to spare.
 */
#define OVERLAY_NAME_MAX 128

/* Longest search string the box will hold, including the terminator. Past this
 * the extra characters are simply not part of the filter - a search string
 * longer than this does not describe anything installed. */
#define OVERLAY_FILTER_MAX 64

/*
 * Which buttons are live.
 *
 * The four templates and the exit button, as of task 6.4. Pick-up (6.7),
 * apply and reset still have nothing behind them and stay greyed: a button that
 * can be pressed but does nothing is the same broken promise as a menu item for
 * a key that does not exist. They are laid out and drawn disabled so the
 * operator sees the shape of the toolbar without being able to press a lie.
 *
 * SNAP_CUSTOM is enabled as a template choice even though drawing regions by
 * hand arrives in 6.5, because selecting it now shows an empty canvas - which
 * is exactly what "custom, nothing drawn yet" should look like.
 *
 * 2026-10-09: the five template buttons became ONE, [Layout format]. Five
 * mutually exclusive choices were taking two thirds of the strip, and they are
 * a choice rather than an action - which is what a panel is for. The button
 * opens the panel; the templates themselves are still all live (the panel
 * lists every SnapTemplate).
 */
#define TOOLBAR_ENABLED_MASK ((1u << TB_EXIT)   | \
                              (1u << TB_LAYOUT) | \
                              (1u << TB_PICK)   | \
                              (1u << TB_APPLY)  | \
                              (1u << TB_RESET))

/*
 * Z-order maintenance (task 6.6).
 *
 * MEASURED 2026-09-22, on the development machine (32 threads), using t05's
 * identical SetWindowPos(HWND_TOPMOST) call:
 *
 *     2000 ms interval   15.6 ms of CPU per 10 s   0.156% of one core
 *     1000 ms interval   46.9 ms of CPU per 10 s   0.469% of one core
 *
 * The plan's worry - "high-rate re-raising visibly raises CPU" - is right in
 * direction (1 s costs three times what 2 s does) and negligible in size. Two
 * seconds it is.
 *
 * The idle-CPU budget is not involved either way: this timer lives on the
 * overlay window and dies with it, and the overlay only exists while the user
 * is in edit mode. Being in edit mode is not being idle.
 */
#define OVERLAY_TOPMOST_TIMER_ID 1
#define OVERLAY_TOPMOST_MS       2000

/*
 * How much of the toolbar strip's own colour it keeps while the pointer is
 * elsewhere (2026-10-10, user decision).
 *
 * The strip used to be solid always, and a region dragged up under it was
 * invisible there - which is why the drag was clamped below the strip in the
 * first place. With the clamp gone the strip has to show what is under it.
 *
 * MEASURED, first attempt: lowering the strip's per-pixel ALPHA does not do
 * this. The alpha pass blends the finished picture with the DESKTOP, and the
 * strip had already painted over the region in the buffer - so the region
 * stayed hidden and only the wallpaper came through. The mixing has to happen
 * in the buffer, over what is already there: see blend_rect().
 *
 * 72: the strip stays clearly a strip, and a 1-px bright border under it reads
 * as (48,65,95) against its own (8,16,34).
 */
#define OVERLAY_BAR_MIX 72

/*
 * The sidebar's slide timer (stage 8, task 2b).
 *
 * It exists ONLY while the panel is moving: sidebar_advance() says whether
 * there is another frame to draw, and the timer is killed the moment it says
 * no. An editor sitting open with the panel parked - out or away - has no
 * timer at all, which is what keeps the idle-CPU budget at 0.0%.
 *
 * 16 ms is one frame at 60 Hz. The slide is SIDEBAR_SLIDE_MS long, so a whole
 * slide is about nine frames.
 */
#define OVERLAY_SIDEBAR_TIMER_ID 3
#define OVERLAY_SIDEBAR_FRAME_MS 16

/*
 * The opening text's one-shot timer (2026-10-11, user decision).
 *
 * MEASURED, from a screenshot of a real session: the "edit mode - monitor N"
 * line and the two Esc hints were moved into the CENTRE of the canvas by
 * section 8.30, to stop them colliding with a full-height box's own label in
 * the top left corner. The centre turned out to be no safer - a box spanning
 * the middle of the screen put its contents underneath them, which is exactly
 * the collision 8.30 set out to avoid, just somewhere else.
 *
 * There is no safe place to park text on a canvas that covers the whole screen,
 * so the text does not stay. It is shown once as edit mode opens and then gets
 * out of the way. Leaving is still discoverable afterwards: the [exit] button
 * sits on the toolbar for the whole session.
 *
 * ONE SHOT, killed in WM_TIMER. It must never become a repeating timer, or the
 * idle-CPU budget (section 1.2) pays forever for a line of text nobody is
 * reading. t51_release's fifty-cycle gate is what would notice a missed kill -
 * that is precisely how the Z-order timer was caught (81 GDI / 42 USER left
 * behind; see overlay_destroy()).
 *
 * FIVE SECONDS, not two (user decision, 2026-10-11, after actually using it):
 * two was long enough to MISS. Open the panel, look at it, and the text has
 * already gone before it has been read - and the whole point of showing it is
 * that it gets read. Five is still over quickly enough that it never becomes
 * part of the furniture.
 */
#define OVERLAY_INTRO_TIMER_ID 2
#define OVERLAY_INTRO_MS       5000

/*
 * The zone a dropped application gets, as a fraction of the canvas.
 *
 * Half the width and the full height: the same starting point `create_preset_
 * from_path()` uses for a program that is not running, because a drop says
 * WHERE and the editor is where the size gets decided. Two different defaults
 * for the same question would be one more thing to explain.
 */
#define SIDEBAR_DROP_W 0.5f
#define SIDEBAR_DROP_H 1.0f

/* How far the pointer must travel with the button down before a press on a row
 * becomes a DRAG. The system's own threshold, not a guessed number: it is what
 * every list on the desktop uses, and it is already scaled for the machine's
 * mouse. */
#define SIDEBAR_DRAG_SLOP_X GetSystemMetrics(SM_CXDRAG)
#define SIDEBAR_DRAG_SLOP_Y GetSystemMetrics(SM_CYDRAG)

struct Overlay {
    HWND          hwnd;
    HINSTANCE     hinst;
    int           monitor_index;
    RECT          rect;              /* physical pixels, rcMonitor */
    int           dpi;               /* GetDpiForWindow at create / after a move */
    HWND          prev_foreground;   /* focus to hand back on exit */
    OverlayExitFn on_exit;
    void         *ctx;
    OverlayExit   reason;
    BOOL          destroying;

    /*
     * Toolbar geometry, computed once in overlay_create().
     *
     * It cannot change afterwards: the overlay is exactly the size of the
     * monitor and the widths are fixed, so recomputing it per paint would be
     * work that always produces the same answer. Holding it also means the
     * rectangles the mouse is tested against are the same ones that were
     * drawn - the classic way a custom toolbar goes wrong is two pieces of
     * code each deriving the layout slightly differently.
     */
    ToolbarItem   items[TB_COUNT];
    int           item_count;
    ToolbarButton hover;        /* TB_COUNT means none */
    ToolbarButton pressed;      /* TB_COUNT means none */

    /*
     * Whether the pointer is on the strip. The strip is solid while it is, and
     * faded otherwise so that a region dragged up under it stays visible
     * (2026-10-10, user decision - see OVERLAY_BAR_FADED).
     */
    BOOL          top_hover;

    /*
     * The name box (stage 6: renaming lives inside edit mode, by design).
     *
     * Geometry beside the toolbar's, and computed once for the same reason: the
     * strip is fixed and the widths are fixed. The two buttons are placed even
     * while they are not drawn - that is what keeps the box still when the user
     * clicks into it, which the design requires.
     */
    ToolbarNameBox  name_box;
    wchar_t         name[OVERLAY_NAME_MAX];
    OverlayRenameFn on_rename;      /* may be NULL: the box is then display-only */
    void           *rename_ctx;
    HWND            name_edit;      /* the top-level EDIT while typing, else NULL */
    BOOL            name_editing;
    int             name_hover;     /* 0 = [save], 1 = [cancel], -1 = neither */
    WNDPROC         name_edit_old;  /* the EDIT's own procedure, before ours */

    /*
     * The regions of the current template, normalized, and which template made
     * them. Recomputed only when the template changes - the geometry does not
     * depend on anything else, and a full 3x3 layout recomputed per paint would
     * be work whose answer never differs.
     */
    RelRect       regions[SNAP_MAX_REGIONS];
    int           region_count;
    SnapTemplate  tpl;

    /*
     * The layout panel (2026-10-09).
     *
     * [Layout format] opens a two-column grid of template thumbnails instead of
     * the five separate buttons the strip used to carry. popup owns where it is
     * and whether it is open; panel_hover is the item under the pointer, or -1.
     *
     * They are separate because "open" and "something hovered" are different
     * states: the panel opens with nothing under the pointer, and the pointer
     * can leave it while it stays open on the way to the button.
     */
    PopupState    popup;
    int           panel_hover;

    /*
     * The monitor box in the middle of the strip, and the list its dropdown
     * offers (2026-10-09).
     *
     * The list is CACHED rather than enumerated per paint: enumerating reads
     * every monitor's EDID out of the registry, and a paint happens on every
     * mouse move. It is refreshed where the screen can change - at create, and
     * after a move.
     */
    RECT                mon_box;
    BOOL                mon_box_valid;
    wchar_t             mon_name[128];   /* what the box shows */
    MonitorFingerprint  mons[WAC_MON_MAX];
    int                 mon_count;

    /*
     * The application sidebar (stage 8, task 2b).
     *
     * sidebar      the state machine: how far out the panel is, what is
     *              scrolled to, what is hovered, what is being dragged.
     * sb_metrics   every length, scaled once from this overlay's dpi. NOT a
     *              module global like toolbar.c keeps: two modules each
     *              holding a scale factor is how trap 36 happened.
     * sb_area      the client area MINUS the strip, which is the only thing
     *              sidebar.c is told about the screen it slides over.
     * sb_src       the rows, borrowed from the caller - see overlay.h.
     */
    SidebarState         sidebar;
    SidebarMetrics       sb_metrics;
    RECT                 sb_area;
    OverlaySidebarSource sb_src;
    OverlayDropFn        sb_drop;
    void                *sb_drop_ctx;

    /*
     * The rows' icons, with a ceiling (stage 8, task 2b).
     *
     * Owned here rather than in editstate.c because the paint path is the only
     * thing that asks for an icon, and because release_resources() is already
     * the one place this module gives its handles back - t51_release is the
     * gate that keeps that true.
     *
     * NOT built in build_resources(): that pair is rebuilt whenever the
     * overlay moves to another monitor, and an icon does not depend on the
     * screen. Its SIZE does, so a move empties the cache instead.
     */
    IconCache           *icons;

    /*
     * The search box (2026-10-10).
     *
     * `sb_filter` is an array of INDICES into the source rows, and NULL when
     * the box is empty - which is not an optimization: it is the difference
     * between "draw source row 5" and "draw the fifth row that matched".
     * Everything that used to use a row number directly now goes through
     * sb_row_at().
     *
     * `sb_edit` is the top-level EDIT window the user actually types into,
     * placed exactly over the drawn box. The overlay cannot host a child
     * control (a layered window is composited as one unit) and text it paints
     * itself never reaches the IME - so a Chinese program name could not be
     * typed into a self-drawn box at all. Same constraint, same answer as the
     * name box.
     */
    wchar_t  sb_filter_text[OVERLAY_FILTER_MAX];
    int     *sb_filter;
    int      sb_filter_count;
    HWND     sb_edit;
    WNDPROC  sb_edit_old;

    /*
     * The placeholders: one box per preset this workspace is meant to place
     * (stage 8, acceptance items 8 and 9).
     *
     * Borrowed from the caller, like the sidebar's rows, and asked for afresh
     * on every paint: the list is one entry per preset, and reading it every
     * time means a preset changed elsewhere shows up here without any
     * bookkeeping to get out of step.
     *
     * ph_boxes records what was drawn, so the alpha pass does not have to
     * recompute it - the same arrangement the drag ghost uses.
     */
    OverlaySlotSource    slot_src;
    PlaceholderMetrics   ph_metrics;
    RECT                 ph_boxes[PLACEHOLDER_MAX_SHOWN];
    int                  ph_count;

    /*
     * The box being dragged or resized, and the one under the pointer.
     *
     * ph_start is the box as it was when the button went down and ph_now is
     * where the drag has taken it: the difference between the two is what tells
     * a real edit from a click, and a drag that is abandoned mid-way can be
     * undone by simply not reporting it.
     */
    int                  ph_hover;      /* box under the pointer, or -1 */
    int                  ph_drag;       /* box being dragged, or -1     */
    BOOL                 ph_resizing;
    POINT                ph_origin;
    RECT                 ph_start;
    RECT                 ph_now;
    BOOL                 ph_short_reported;  /* "some boxes did not fit", said once */
    POINT                sb_press_pt;     /* where the button went down on a row */
    BOOL                 sb_scroll_drag;  /* dragging the scrollbar thumb */
    BOOL                 sb_timer;        /* the slide timer is running */
    BOOL                 intro;           /* the opening text is still up */
    BOOL                 sb_suppress;     /* Esc closed the panel; see sidebar_pointer_moved() */

    /*
     * What the drag is drawing right now, recorded by draw_sidebar_drag() and
     * used by the alpha pass that follows it.
     *
     * The alpha pass runs over the FINISHED picture (GDI does not write the
     * alpha byte), so it cannot ask the drag where it was - and recomputing
     * the arithmetic a second time is how two answers to one question start to
     * differ. The drawer writes them down; the alpha pass reads them.
     */
    RECT                 sb_ghost;
    BOOL                 sb_ghost_valid;
    RECT                 sb_preview;
    BOOL                 sb_preview_valid;

    /*
     * Free-hand region drawing (task 6.5).
     *
     * The candidate lines are collected when the regions change, not on every
     * mouse move: they depend only on the regions and the screen size, and a
     * single drag produces hundreds of moves.
     */
    SnapLines     lines;
    BOOL          drag_active;
    POINT         drag_origin;
    RECT          drag;          /* kept normalized: left<right, top<bottom */

    /* How often something climbed above us (task 6.6). Reported, not acted on:
     * a contested Z-order is a fact about the machine, and the number is the
     * only way to tell "nothing ever tried" from "we never noticed". */
    int           reasserts;

    HDC     mem_dc;                  /* back buffer, created once */
    HBITMAP mem_bmp;                 /* a 32bpp DIB section, not a compatible bitmap */
    HGDIOBJ mem_old_bmp;
    void   *bits;                    /* its pixels, for the alpha pass */
    int     stride;                  /* bytes per row */

    HFONT   font_big;
    HFONT   font_small;
    HBRUSH  brush_bg;
    HBRUSH  brush_bar;
    HPEN    pen_border;
    HPEN    pen_grid;
};

/*
 * The name box's four operations.
 *
 * Declared here because overlay_proc()'s mouse handling uses them and they are
 * defined further down, beside the rest of the name box code. C would otherwise
 * assume they return int, which is exactly the trap that made `calloc` return a
 * truncated pointer in this file once already (TRAPS.md #30).
 */
static void name_edit_close(Overlay *o);
static void name_edit_commit(Overlay *o);
static void name_edit_cancel(Overlay *o);
static BOOL name_edit_begin(Overlay *o);

/*
 * The layout panel, for the same reason: the paint path draws it and the mouse
 * paths open, hover and close it, all before the definitions further down.
 */
static void apply_template(Overlay *o, SnapTemplate tpl);
static void layout_panel_open(Overlay *o);
static void layout_panel_close(Overlay *o);
static void layout_panel_click(Overlay *o, POINT pt);
static void draw_layout_panel(Overlay *o, const RECT *panel);

/* The monitor box and its dropdown. Same reason: the paint path draws them and
 * the mouse paths open, hover and choose, all before the definitions. */
static void refresh_monitor_box(Overlay *o);
static void monitor_panel_open(Overlay *o);
static void monitor_panel_close(Overlay *o);
static void monitor_panel_click(Overlay *o, POINT pt);
static void draw_monitor_box(Overlay *o);
static void draw_monitor_panel(Overlay *o, const RECT *panel);

/*
 * The sidebar (stage 8, task 2b). Same reason again: the paint path draws it,
 * the mouse paths drive it, the timer advances it and overlay_create lays it
 * out - all of which appear before the definitions further down.
 */
static BOOL sidebar_enabled(Overlay *o);
static int  sb_visible_count(Overlay *o);
static int  sb_row_at(Overlay *o, int visible_index);
static void sb_apply_filter(Overlay *o);
static int  sidebar_rows(Overlay *o);
static void sidebar_layout(Overlay *o);
static void sidebar_timer_sync(Overlay *o);
static void sidebar_invalidate(Overlay *o);
static void sidebar_pointer_moved(Overlay *o, POINT pt);
static void draw_sidebar(Overlay *o);
static void draw_sidebar_drag(Overlay *o);
static void draw_placeholders(Overlay *o);

/* The search box's EDIT window. Same reason: the mouse paths open and close it
 * long before its definitions. */
static BOOL filter_edit_begin(Overlay *o);
static void filter_edit_close(Overlay *o);
static void filter_edit_changed(Overlay *o);

/* ------------------------------------------------------------------ */
/* Module-level counters                                              */
/* ------------------------------------------------------------------ */

static int g_gdi_count;
static int g_created;
static int g_destroyed;

int overlay_gdi_objects(void)     { return g_gdi_count; }
int overlay_created_count(void)   { return g_created; }
int overlay_destroyed_count(void) { return g_destroyed; }

const wchar_t *overlay_exit_name(OverlayExit why)
{
    switch (why) {
    case OVERLAY_EXIT_NONE:     return L"none";
    case OVERLAY_EXIT_HOTKEY:   return L"exit key";
    case OVERLAY_EXIT_ESCAPE:   return L"escape";
    case OVERLAY_EXIT_TOOLBAR:  return L"toolbar";
    case OVERLAY_EXIT_FUSE:     return L"safety fuse";
    case OVERLAY_EXIT_SHUTDOWN: return L"program shutdown";
    case OVERLAY_EXIT_FAILED:   return L"could not start";
    }
    return L"unknown";
}

static Overlay *from_hwnd(HWND hwnd)
{
    return (Overlay *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                            */
/* ------------------------------------------------------------------ */

/* Every object this module owns is created exactly here. */
/*
 * The dpi every length in this overlay is scaled by.
 *
 * MUST BE CALLED BEFORE ANY GEOMETRY EXISTS. The first version of the scaling
 * set it inside build_resources(), which overlay_create() calls AFTER
 * toolbar_layout() - so on a 150% monitor the buttons were laid out with
 * 96-dpi rectangles while the captions were drawn with 150% fonts: the four
 * captions ran into each other with no gaps, and hovering [Layout format]
 * answered for the wrong rectangle, so the panel never opened. MEASURED
 * 2026-10-09 by screenshotting the overlay on the 150% panel.
 */
static void apply_overlay_dpi(Overlay *o)
{
    o->dpi = (int)GetDpiForWindow(o->hwnd);
    toolbar_set_dpi(o->dpi);

    /*
     * The sidebar's lengths are scaled from the SAME number, in one call, so
     * the strip and the panel cannot end up at two different scales - which is
     * trap 36, and it cost a screenshot to find the first time.
     */
    sidebar_metrics(o->dpi, &o->sb_metrics);

    /* ...and the placeholders' lengths from the same number again. */
    placeholder_metrics(o->dpi, &o->ph_metrics);
}

static void build_resources(Overlay *o)
{
    /*
     * One screen DC for both the memory DC and the DIB section, released once
     * at the end. (The first version took it twice and shadowed the variable;
     * /W4 /WX turned that into a build failure, which is the warning earning
     * its keep.)
     */
    HDC screen = GetDC(NULL);

    if (screen != NULL) {
        o->mem_dc = CreateCompatibleDC(screen);
    }

    if (o->mem_dc != NULL) {
        g_gdi_count++;
        const int w = (int)(o->rect.right - o->rect.left);
        const int h = (int)(o->rect.bottom - o->rect.top);

        /*
         * A 32bpp DIB section, not CreateCompatibleBitmap.
         *
         * MEASURED 2026-09-22, reported by the user with a screenshot: with
         * SetLayeredWindowAttributes + LWA_ALPHA the window is blended as one
         * unit, so the toolbar captions were 59% opaque too and unreadable
         * against a dark wallpaper. The wash has to be faint and the controls
         * have to be solid, and those are two different alphas in one window -
         * which needs the pixels themselves.
         *
         * Top-down (negative height) so row 0 is the top row, matching the
         * coordinates GDI draws in.
         */
        BITMAPINFO bmi;
        ZeroMemory(&bmi, sizeof(bmi));
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = w;
        bmi.bmiHeader.biHeight      = -h;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void *bits = NULL;
        if (screen != NULL) {
            o->mem_bmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS,
                                          &bits, NULL, 0);
        }

        if (o->mem_bmp != NULL) {
            g_gdi_count++;
            o->bits        = bits;
            o->stride      = w * 4;
            o->mem_old_bmp = SelectObject(o->mem_dc, o->mem_bmp);
        }
    }

    if (screen != NULL) {
        ReleaseDC(NULL, screen);
    }

    /* The fonts scale with the strip, or a 96-dpi caption would be clipped
     * inside a 144-dpi box (measured 2026-10-09). */
    o->font_big = CreateFontW(-toolbar_px(40), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
    o->font_small = CreateFontW(-toolbar_px(22), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, L"Segoe UI");
    o->brush_bg  = CreateSolidBrush(RGB(OVERLAY_BG_R, OVERLAY_BG_G, OVERLAY_BG_B));
    o->brush_bar = CreateSolidBrush(RGB(8, 16, 34));
    o->pen_border = CreatePen(PS_SOLID, 4, RGB(255, 200, 0));
    o->pen_grid   = CreatePen(PS_SOLID, 1, RGB(70, 90, 130));

    if (o->font_big != NULL)   g_gdi_count++;
    if (o->font_small != NULL) g_gdi_count++;
    if (o->brush_bg != NULL)   g_gdi_count++;
    if (o->brush_bar != NULL)  g_gdi_count++;
    if (o->pen_border != NULL) g_gdi_count++;
    if (o->pen_grid != NULL)   g_gdi_count++;
}

static void release_resources(Overlay *o)
{
    if (o->mem_dc != NULL) {
        if (o->mem_old_bmp != NULL) {
            SelectObject(o->mem_dc, o->mem_old_bmp);
            o->mem_old_bmp = NULL;
        }
        DeleteDC(o->mem_dc);
        o->mem_dc = NULL;
        g_gdi_count--;
    }
    if (o->mem_bmp != NULL) {
        DeleteObject(o->mem_bmp);
        o->mem_bmp = NULL;
        g_gdi_count--;
    }
    if (o->font_big != NULL)   { DeleteObject(o->font_big);   o->font_big = NULL;   g_gdi_count--; }
    if (o->font_small != NULL) { DeleteObject(o->font_small); o->font_small = NULL; g_gdi_count--; }
    if (o->brush_bg != NULL)   { DeleteObject(o->brush_bg);   o->brush_bg = NULL;   g_gdi_count--; }
    if (o->brush_bar != NULL)  { DeleteObject(o->brush_bar);  o->brush_bar = NULL;  g_gdi_count--; }
    if (o->pen_border != NULL) { DeleteObject(o->pen_border); o->pen_border = NULL; g_gdi_count--; }
    if (o->pen_grid != NULL)   { DeleteObject(o->pen_grid);   o->pen_grid = NULL;   g_gdi_count--; }

    /* The pixels belonged to the DIB, which is gone. Dropping the pointer stops
     * a later paint writing through it. */
    o->bits   = NULL;
    o->stride = 0;
}

/* ---- per-pixel alpha ------------------------------------------------
 *
 * MEASURED 2026-09-22, reported by the user with a screenshot: with one
 * window-wide alpha via SetLayeredWindowAttributes, the toolbar captions were
 * exactly as see-through as the background wash - 59% - and were unreadable
 * against a dark wallpaper. The wash has to be faint and the controls have to
 * be solid, and those are two different alphas in one window. That is only
 * possible per pixel.
 *
 * GDI leaves the alpha byte of a 32bpp DIB untouched, which is what makes this
 * cheap: draw the whole picture in colour first, then stamp alpha over the
 * finished result, region by region.
 * -------------------------------------------------------------------- */
static void alpha_rect(Overlay *o, const RECT *r, BYTE a)
{
    if (o->bits == NULL || r == NULL) {
        return;
    }

    const int w = (int)(o->rect.right - o->rect.left);
    const int h = (int)(o->rect.bottom - o->rect.top);

    const int top    = (r->top    < 0) ? 0 : (int)r->top;
    const int left   = (r->left   < 0) ? 0 : (int)r->left;
    const int bottom = (r->bottom > h) ? h : (int)r->bottom;
    const int right  = (r->right  > w) ? w : (int)r->right;

    for (int y = top; y < bottom; ++y) {
        BYTE *row = (BYTE *)o->bits + (size_t)y * (size_t)o->stride;
        for (int x = left; x < right; ++x) {
            row[x * 4 + 3] = a;
        }
    }
}

static void alpha_all(Overlay *o, BYTE a)
{
    RECT full;
    full.left   = 0;
    full.top    = 0;
    full.right  = o->rect.right - o->rect.left;
    full.bottom = o->rect.bottom - o->rect.top;
    alpha_rect(o, &full, a);
}

/*
 * Mix `color` into the pixels already in `r`.
 *
 * NOT FillRect, and not the alpha pass either. A fill REPLACES what is under
 * it; the alpha pass blends the finished picture with the DESKTOP. Neither can
 * show a region through a panel drawn over it - which is the whole reason this
 * function exists (the toolbar strip, 2026-10-10).
 *
 * `percent` is how much of `color` ends up in the result, so 100 is a plain
 * fill and 0 changes nothing. The alpha byte is left alone: what this decides
 * is what the pixel LOOKS like, not how much of the desktop shows through it.
 */
static void blend_rect(Overlay *o, const RECT *r, COLORREF color, int percent)
{
    if (o->bits == NULL || r == NULL || percent <= 0) return;
    if (percent > 100) percent = 100;

    const int w = (int)(o->rect.right - o->rect.left);
    const int h = (int)(o->rect.bottom - o->rect.top);

    const int top    = (r->top    < 0) ? 0 : (int)r->top;
    const int left   = (r->left   < 0) ? 0 : (int)r->left;
    const int bottom = (r->bottom > h) ? h : (int)r->bottom;
    const int right  = (r->right  > w) ? w : (int)r->right;

    const int  keep = 100 - percent;
    const BYTE cr   = GetRValue(color);
    const BYTE cg   = GetGValue(color);
    const BYTE cb   = GetBValue(color);

    for (int y = top; y < bottom; ++y) {
        BYTE *row = (BYTE *)o->bits + (size_t)y * (size_t)o->stride;
        for (int x = left; x < right; ++x) {
            BYTE *px = row + (size_t)x * 4;     /* the DIB is B, G, R, A */
            px[0] = (BYTE)((px[0] * keep + cb * percent) / 100);
            px[1] = (BYTE)((px[1] * keep + cg * percent) / 100);
            px[2] = (BYTE)((px[2] * keep + cr * percent) / 100);
        }
    }
}

/*
 * Paint the whole sheet into the back buffer.
 *
 * Called only from WM_PAINT, which is only raised when something actually
 * changed. There is no timer behind this: an editor sitting open while the
 * user decides where a window goes must cost nothing.
 */
static void paint_back_buffer(Overlay *o)
{
    if (o->mem_dc == NULL || o->mem_bmp == NULL) {
        return;
    }

    const int w = (int)(o->rect.right - o->rect.left);
    const int h = (int)(o->rect.bottom - o->rect.top);
    RECT full = { 0, 0, w, h };

    /* The wash. LWA_ALPHA blends the whole window, so this is a flat fill -
     * cheap, and the reason a screen capture is unnecessary. */
    FillRect(o->mem_dc, &full, o->brush_bg);

    /* The current template's regions, or a plain quarter grid when none has
     * been chosen. drawn with the DC brush and pen so that no region costs a
     * GDI object: this repeats for every repaint. */
    HGDIOBJ old_pen = SelectObject(o->mem_dc, o->pen_grid);

    if (o->region_count > 0) {
        for (int i = 0; i < o->region_count; ++i) {
            RECT px;
            px.left   = (LONG)(o->regions[i].x * (float)w);
            px.top    = (LONG)(o->regions[i].y * (float)h);
            px.right  = px.left + (LONG)(o->regions[i].w * (float)w);
            px.bottom = px.top  + (LONG)(o->regions[i].h * (float)h);

            SetDCBrushColor(o->mem_dc, RGB(42, 74, 128));
            FillRect(o->mem_dc, &px, (HBRUSH)GetStockObject(DC_BRUSH));

            SetDCPenColor(o->mem_dc, RGB(150, 190, 255));
            SelectObject(o->mem_dc, GetStockObject(DC_PEN));
            SelectObject(o->mem_dc, GetStockObject(NULL_BRUSH));
            Rectangle(o->mem_dc, px.left, px.top, px.right, px.bottom);
        }
    } else {
        /* No template chosen yet: quarter lines are enough reference to judge
         * "half the screen" by eye while the user decides. */
        for (int i = 1; i < 4; ++i) {
            const int x = w * i / 4;
            const int y = h * i / 4;
            MoveToEx(o->mem_dc, x, 0, NULL);
            LineTo(o->mem_dc, x, h);
            MoveToEx(o->mem_dc, 0, y, NULL);
            LineTo(o->mem_dc, w, y);
        }
    }

    /* ---- the placeholders, over the regions ----
     *
     * Drawn here rather than beside the regions because they are the layer the
     * user compares against: what they are drawing goes on top, and what the
     * workspace already says stays visible underneath it.
     */
    draw_placeholders(o);

    /*
     * The rectangle being dragged, with its size while it is still being
     * decided.
     *
     * The numbers are the point of the 6.5 exercise. 12-pixel snapping is only
     * trustworthy if the user can see what it produced: a region that quietly
     * came out 1279 wide instead of 1280 shows up much later, as a one-pixel
     * seam between two windows that nobody can explain.
     */
    if (o->drag_active) {
        SetDCBrushColor(o->mem_dc, RGB(70, 120, 190));
        FillRect(o->mem_dc, &o->drag, (HBRUSH)GetStockObject(DC_BRUSH));

        SetDCPenColor(o->mem_dc, RGB(255, 235, 150));
        SelectObject(o->mem_dc, GetStockObject(DC_PEN));
        SelectObject(o->mem_dc, GetStockObject(NULL_BRUSH));
        Rectangle(o->mem_dc, o->drag.left, o->drag.top,
                  o->drag.right, o->drag.bottom);

        wchar_t size[96];
        _snwprintf_s(size, 96, _TRUNCATE, L"%ld x %ld",
                     (long)(o->drag.right - o->drag.left),
                     (long)(o->drag.bottom - o->drag.top));

        HFONT old_drag = (HFONT)SelectObject(o->mem_dc, o->font_small);
        SetBkMode(o->mem_dc, TRANSPARENT);
        SetTextColor(o->mem_dc, RGB(255, 255, 255));

        SIZE sz;
        ZeroMemory(&sz, sizeof(sz));
        GetTextExtentPoint32W(o->mem_dc, size, (int)wcslen(size), &sz);
        TextOutW(o->mem_dc,
                 (o->drag.left + o->drag.right) / 2 - sz.cx / 2,
                 (o->drag.top + o->drag.bottom) / 2 - sz.cy / 2,
                 size, (int)wcslen(size));

        SelectObject(o->mem_dc, old_drag);
    }

    /* ---- the toolbar strip ----
     *
     * Drawn with the DC_BRUSH and DC_PEN stock objects rather than brushes of
     * its own. SetDCBrushColor changes what DC_BRUSH paints in, so the whole
     * toolbar costs zero GDI objects - which matters because this paint runs
     * on every hover change, and a per-paint CreateSolidBrush would be a leak
     * waiting for someone to forget a DeleteObject.
     */
    RECT bar = { 0, 0, w, TOOLBAR_H };

    /*
     * The strip is MIXED with what is under it, not filled over it.
     *
     * Full strength while the pointer is on it or while a region is being
     * drawn (the user is placing an edge up there and must be able to see it);
     * OVERLAY_BAR_MIX percent the rest of the time, so that a region dragged
     * up under the strip stays visible. The buttons, the name box and the
     * monitor box are all filled AFTER this and stay solid - only the strip's
     * background gives way.
     *
     * Driven by the pointer, not by a clock: nothing here needs a timer, and a
     * full-screen window that repaints on its own schedule is exactly what the
     * idle-CPU budget forbids.
     */
    const BOOL bar_solid = (o->top_hover || o->drag_active) ? TRUE : FALSE;
    blend_rect(o, &bar, RGB(8, 16, 34), bar_solid ? 100 : OVERLAY_BAR_MIX);

    if (o->item_count > 0) {
        HFONT old_tb = (HFONT)SelectObject(o->mem_dc, o->font_small);
        SetBkMode(o->mem_dc, TRANSPARENT);

        for (int i = 0; i < o->item_count; ++i) {
            const ToolbarItem *it = &o->items[i];

            if (it->separator_before) {
                /* The vertical rule between groups, drawn at the left edge of
                 * the gap the layout left for it and shifted into it. */
                const int sx = it->rect.left - 9;
                SetDCPenColor(o->mem_dc, RGB(70, 90, 130));
                SelectObject(o->mem_dc, GetStockObject(DC_PEN));
                MoveToEx(o->mem_dc, sx, it->rect.top, NULL);
                LineTo(o->mem_dc, sx, it->rect.bottom);
            }

            COLORREF face = RGB(30, 48, 82);
            COLORREF text = RGB(215, 225, 245);
            if (!it->enabled) {
                /* Greyed: visible, obviously unavailable. */
                face = RGB(22, 30, 44);
                text = RGB(95, 105, 125);
            } else if (o->pressed == it->id) {
                face = RGB(16, 26, 46);
            } else if (o->hover == it->id) {
                face = RGB(46, 72, 120);
            }

            SetDCBrushColor(o->mem_dc, face);
            FillRect(o->mem_dc, &it->rect, (HBRUSH)GetStockObject(DC_BRUSH));

            SetDCPenColor(o->mem_dc, it->enabled ? RGB(90, 120, 180)
                                                 : RGB(40, 50, 66));
            SelectObject(o->mem_dc, GetStockObject(DC_PEN));
            SelectObject(o->mem_dc, GetStockObject(NULL_BRUSH));
            Rectangle(o->mem_dc, it->rect.left, it->rect.top,
                      it->rect.right, it->rect.bottom);

            const wchar_t *cap = i18n_text(toolbar_label_id(it->id));
            SIZE sz;
            ZeroMemory(&sz, sizeof(sz));
            GetTextExtentPoint32W(o->mem_dc, cap, (int)wcslen(cap), &sz);
            SetTextColor(o->mem_dc, text);
            TextOutW(o->mem_dc,
                     it->rect.left + ((it->rect.right - it->rect.left) - sz.cx) / 2,
                     it->rect.top + ((it->rect.bottom - it->rect.top) - sz.cy) / 2,
                     cap, (int)wcslen(cap));
        }

        SelectObject(o->mem_dc, old_tb);
    }

    /*
     * The name box, to the left of the buttons.
     *
     * It reads as a text field: a darker fill than the strip, and a brighter
     * border while the user is typing in it. The name is painted here even
     * while typing - the real EDIT window sits exactly on top of the box, so
     * the two never show at once, and keeping the browse-state text correct
     * means nothing has to be repainted specially when the box closes.
     */
    if (o->name_box.valid) {
        HFONT old_nb = (HFONT)SelectObject(o->mem_dc, o->font_small);

        SetBkMode(o->mem_dc, TRANSPARENT);

        SetDCBrushColor(o->mem_dc, o->name_editing ? RGB(20, 30, 52)
                                                   : RGB(12, 18, 30));
        FillRect(o->mem_dc, &o->name_box.name, (HBRUSH)GetStockObject(DC_BRUSH));

        SetDCPenColor(o->mem_dc, o->name_editing ? RGB(130, 170, 235)
                                                 : RGB(60, 80, 118));
        SelectObject(o->mem_dc, GetStockObject(DC_PEN));
        SelectObject(o->mem_dc, GetStockObject(NULL_BRUSH));
        Rectangle(o->mem_dc, o->name_box.name.left, o->name_box.name.top,
                  o->name_box.name.right, o->name_box.name.bottom);

        RECT txt = o->name_box.name;
        txt.left  += 10;
        txt.right -= 10;
        SetTextColor(o->mem_dc, RGB(225, 235, 255));
        DrawTextW(o->mem_dc, o->name, -1, &txt,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                  DT_NOPREFIX);

        /*
         * The two buttons are drawn only while typing - but their rectangles
         * were reserved by the layout either way. That is what keeps the box
         * from moving when the user clicks into it, which the design requires.
         */
        if (o->name_editing) {
            static const StrId k_box_ids[2] = { STR_BTN_SAVE, STR_SETTINGS_CANCEL };
            const RECT *pair[2];

            pair[0] = &o->name_box.save;
            pair[1] = &o->name_box.cancel;

            for (int i = 0; i < 2; ++i) {
                const RECT *r = pair[i];
                const wchar_t *cap = i18n_text(k_box_ids[i]);
                SIZE sz;

                SetDCBrushColor(o->mem_dc,
                                (o->name_hover == i) ? RGB(46, 72, 120)
                                                     : RGB(30, 48, 82));
                FillRect(o->mem_dc, r, (HBRUSH)GetStockObject(DC_BRUSH));

                SetDCPenColor(o->mem_dc, RGB(90, 120, 180));
                SelectObject(o->mem_dc, GetStockObject(DC_PEN));
                SelectObject(o->mem_dc, GetStockObject(NULL_BRUSH));
                Rectangle(o->mem_dc, r->left, r->top, r->right, r->bottom);

                ZeroMemory(&sz, sizeof(sz));
                GetTextExtentPoint32W(o->mem_dc, cap, (int)wcslen(cap), &sz);
                SetTextColor(o->mem_dc, RGB(215, 225, 245));
                TextOutW(o->mem_dc,
                         r->left + ((r->right - r->left) - sz.cx) / 2,
                         r->top + ((r->bottom - r->top) - sz.cy) / 2,
                         cap, (int)wcslen(cap));
            }
        }

        SelectObject(o->mem_dc, old_nb);
    }

    /* Border last, so it sits above the grid. */
    SelectObject(o->mem_dc, o->pen_border);
    SelectObject(o->mem_dc, GetStockObject(NULL_BRUSH));
    Rectangle(o->mem_dc, 2, 2, w - 2, h - 2);
    SelectObject(o->mem_dc, old_pen);

    SetBkMode(o->mem_dc, TRANSPARENT);

    /* ---- centre: what this is, and how to get out ----
     *
     * MEASURED 2026-10-10, on the first screenshot that had a placeholder box
     * on the left half: the "editing a workspace" line used to sit at the top
     * left, which is exactly where a full-height box puts its own label - two
     * pieces of text in one place, and the box's name was the one that lost.
     *
     * MEASURED AGAIN 2026-10-11, and the second reading overrules the first:
     * the centre is no safer. A box spanning the middle of the screen puts its
     * contents under this text, so the collision 8.30 was avoiding simply moved
     * house. A canvas that covers the whole screen has nowhere safe to park a
     * permanent label.
     *
     * So it is not permanent any more - it is shown once, for
     * OVERLAY_INTRO_MS, and then the canvas is the user's alone. The one
     * failure this design must never have is a user who cannot work out how to
     * leave (an exit key that only exists in the documentation is an exit key
     * that does not exist), and that is still
     * covered: the [exit] button is on the toolbar for the whole session, and
     * it is there before, during and after these few seconds.
     */
    HGDIOBJ old_font = SelectObject(o->mem_dc, o->font_small);

    if (o->intro) {
        wchar_t line1[160];
        _snwprintf_s(line1, 160, _TRUNCATE, i18n_text(STR_EDIT_OVERLAY_TITLE),
                     o->monitor_index,
                     (int)(o->rect.right - o->rect.left),
                     (int)(o->rect.bottom - o->rect.top));

        SetTextColor(o->mem_dc, RGB(255, 220, 120));
        SelectObject(o->mem_dc, o->font_big);
        SIZE l1;
        ZeroMemory(&l1, sizeof(l1));
        GetTextExtentPoint32W(o->mem_dc, line1, (int)wcslen(line1), &l1);
        TextOutW(o->mem_dc, (w - l1.cx) / 2, h / 2 - 40, line1, (int)wcslen(line1));

        SetTextColor(o->mem_dc, RGB(235, 240, 255));
        SelectObject(o->mem_dc, o->font_small);

        const wchar_t *hints[] = {
            i18n_text(STR_EDIT_OVERLAY_HINT1),
            i18n_text(STR_EDIT_OVERLAY_HINT2),
        };
        int y = h / 2 + 4;
        for (int i = 0; i < (int)(sizeof(hints) / sizeof(hints[0])); ++i) {
            SIZE sz;
            ZeroMemory(&sz, sizeof(sz));
            GetTextExtentPoint32W(o->mem_dc, hints[i], (int)wcslen(hints[i]), &sz);
            TextOutW(o->mem_dc, (w - sz.cx) / 2, y, hints[i], (int)wcslen(hints[i]));
            y += sz.cy + 8;
        }
    }

    /* ---- bottom: live geometry, so the numbers can be trusted later ---- */
    SetTextColor(o->mem_dc, RGB(150, 170, 210));
    wchar_t line3[160];
    _snwprintf_s(line3, 160, _TRUNCATE, i18n_text(STR_EDIT_OVERLAY_RECT_FMT),
                 (int)o->rect.left, (int)o->rect.top,
                 (int)o->rect.right, (int)o->rect.bottom);
    TextOutW(o->mem_dc, 24, h - 40, line3, (int)wcslen(line3));

    SelectObject(o->mem_dc, old_font);

    /*
     * The alpha pass, over the finished picture.
     *
     * Order matters. GDI does not write the alpha byte of a 32bpp DIB, so
     * stamping alpha per element while drawing would not work - a later fill
     * would write colour over it and the byte would keep whatever value it had.
     * Draw everything first, then decide how see-through each region is.
     */
    /*
     * The monitor box in the middle of the strip, then whichever pop-up is
     * open on top of everything. Both are drawn BEFORE the alpha pass below so
     * that it can make them solid in one go - the order the toolbar uses.
     */
    /*
     * The sidebar goes on next: over the canvas, under the strip's pop-ups.
     * A panel the user opened is on top of everything, and the sidebar is a
     * surface they work with rather than something they opened.
     */
    draw_sidebar(o);
    draw_sidebar_drag(o);

    draw_monitor_box(o);

    /*
     * WHICH panel is open is decided HERE, once, and each drawer is handed the
     * rectangle it may paint into.
     *
     * MEASURED 2026-10-09, found by cropping a screenshot of the open monitor
     * dropdown at native size: draw_layout_panel() used to ask only "is a
     * panel open", so with the DROPDOWN up it drew the whole layout grid into
     * the dropdown's own rectangle - the title row of 左右半屏 / 三等分 and the
     * tops of the thumbnails stuck out below the list. The panel was never
     * wrong about its own geometry; it was asked to draw against the other
     * panel's. Passing the rectangle in makes that impossible to write, and
     * the switch is the only place the two can be confused.
     */
    switch (popup_current(&o->popup)) {
    case POPUP_LAYOUT:  draw_layout_panel(o, &o->popup.panel);   break;
    case POPUP_MONITOR: draw_monitor_panel(o, &o->popup.panel);  break;
    default:            break;
    }

    alpha_all(o, OVERLAY_ALPHA);

    /*
     * The toolbar is solid, always.
     *
     * What makes a region under it visible is not this alpha but the MIXING
     * done when the strip was painted (blend_rect): the strip's own colour and
     * the region's are already combined in the buffer by the time the alpha
     * pass runs. Lowering the alpha here instead would only let the DESKTOP
     * through - measured 2026-10-10, and the region stayed hidden.
     *
     * The controls inside the strip have to stay readable either way, and a
     * control the user cannot read is not a control.
     */
    RECT bar_alpha = { 0, 0, w, TOOLBAR_H };
    alpha_rect(o, &bar_alpha, 255);

    /*
     * The panel is solid for the same reason the toolbar is: it holds choices,
     * and a choice the user cannot read is not a choice. It also sits on top of
     * the canvas, where 59% over a dark wallpaper is unreadable.
     */
    if (popup_is_open(&o->popup)) {
        RECT panel_alpha = o->popup.panel;
        alpha_rect(o, &panel_alpha, 255);
    }

    /* The monitor box holds a control the user reads and clicks, so it is solid
     * for the same reason the toolbar is. */
    if (o->mon_box_valid) {
        RECT mb = o->mon_box;
        alpha_rect(o, &mb, 255);
    }

    /*
     * The centre instructions get a band of their own, nearly solid. They are
     * the one thing on this screen that must never be missed: a user who
     * cannot read how to leave is the failure this whole design is arranged to
     * prevent. The band is 235 rather than 255 so real windows still show
     * through it, just not enough to swallow the text.
     */
    RECT hint_band = { 0, h / 2 - 30, w, h / 2 + 100 };
    alpha_rect(o, &hint_band, 235);

    /*
     * The sidebar is solid for the same reason the toolbar is: it holds names
     * the user has to read. The strip is solid too - a grip nobody can see is
     * not an invitation.
     */
    if (sidebar_enabled(o)) {
        RECT sb_edge;
        if (sidebar_edge_rect(&o->sb_metrics, &o->sb_area, &sb_edge)) {
            alpha_rect(o, &sb_edge, 255);
        }

        /* The handle sticks OUT of the strip, so it needs its own pass - the
         * strip's rectangle does not cover it. */
        RECT sb_handle;
        if (sidebar_handle_rect(&o->sb_metrics, &o->sb_area, &sb_handle)) {
            alpha_rect(o, &sb_handle, 255);
        }

        RECT sb_vis;
        if (sidebar_body_visible_rect(&o->sidebar, &o->sb_metrics, &o->sb_area,
                                      &sb_vis)) {
            alpha_rect(o, &sb_vis, 255);
        }

        /* What is being carried, and where it would land: nearly solid, like
         * the region readout - feedback at 59% over a dark wallpaper is
         * feedback nobody can read. */
        if (o->sb_ghost_valid)   alpha_rect(o, &o->sb_ghost, 235);
        if (o->sb_preview_valid) alpha_rect(o, &o->sb_preview, 235);
    }

    /*
     * The placeholder boxes are nearly solid for the same reason the sidebar
     * is: they carry text - a name and a size - and text at 59% over a dark
     * wallpaper is text nobody can read.
     */
    for (int i = 0; i < o->ph_count; ++i) {
        alpha_rect(o, &o->ph_boxes[i], 235);
    }

    /*
     * The drag rectangle goes solid too. Its size readout is the one piece of
     * live feedback in this mode, and feedback at 59% against a dark wallpaper
     * is feedback nobody can read - the same mistake the toolbar captions had.
     */
    if (o->drag_active) {
        alpha_rect(o, &o->drag, 235);
    }
}

/* ------------------------------------------------------------------ */
/* Getting the picture onto the screen                                */
/* ------------------------------------------------------------------ */

/*
 * Push the back buffer to the screen as a layered frame.
 *
 * Not BitBlt: with per-pixel alpha the whole surface goes at once, and the
 * alpha byte of each pixel decides how much of what is underneath shows
 * through.
 */
static void push_frame(Overlay *o)
{
    if (o->hwnd == NULL || o->mem_dc == NULL) {
        return;
    }

    HDC screen = GetDC(NULL);
    if (screen == NULL) {
        return;
    }

    POINT dst;
    dst.x = o->rect.left;
    dst.y = o->rect.top;
    POINT src;
    src.x = 0;
    src.y = 0;
    SIZE size;
    size.cx = o->rect.right - o->rect.left;
    size.cy = o->rect.bottom - o->rect.top;

    BLENDFUNCTION bf;
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = 255;   /* the per-pixel alpha decides */
    bf.AlphaFormat         = AC_SRC_ALPHA;

    (void)UpdateLayeredWindow(o->hwnd, screen, &dst, &size,
                              o->mem_dc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
}

/* Draw the frame, then show it. */
static void render(Overlay *o)
{
    paint_back_buffer(o);
    push_frame(o);
}

/* ------------------------------------------------------------------ */
/* Exit                                                               */
/* ------------------------------------------------------------------ */

/*
 * Record the reason and tell the owner. Does NOT destroy the window: this runs
 * inside message handling, and destroying a window from inside its own message
 * is how the rest of that message ends up running against freed state.
 */
static void report_exit(Overlay *o, OverlayExit why)
{
    if (o->reason != OVERLAY_EXIT_NONE) {
        return;         /* first reason wins; a fuse must not overwrite an Esc */
    }
    o->reason = why;
    log_info(L"overlay: exit requested (%ls)", overlay_exit_name(why));

    if (o->on_exit != NULL) {
        o->on_exit(why, o->ctx);
    }
}

/* ------------------------------------------------------------------ */
/* Window procedure                                                   */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Overlay *o = from_hwnd(hwnd);

    switch (msg) {
    case WM_NCCREATE: {
        const CREATESTRUCTW *cs = (const CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return TRUE;
    }

    case WM_ERASEBKGND:
        return 1;               /* the back buffer covers every pixel */

    case WM_PAINT: {
        /*
         * UpdateLayeredWindow, not BitBlt.
         *
         * With per-pixel alpha the system no longer blends the window at a
         * fixed ratio: the pixels themselves say how opaque they are, and the
         * whole surface is pushed to the screen at once. BeginPaint/EndPaint
         * still have to be paired or the message loop waits for a paint it
         * believes is unfinished, but nothing is drawn through hdc - the
         * picture goes over the top of it.
         */
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        (void)hdc;

        if (o != NULL) {
            render(o);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    /*
     * ---- toolbar mouse ----
     *
     * Only the strip is invalidated on a hover or press change. Repainting the
     * whole overlay on every mouse move would push a 2560x1600 back buffer
     * through GDI at pointer speed, against the "no redraw unless something
     * changed" rule this program is built on.
     */
    case WM_MOUSEMOVE: {
        if (o == NULL) break;

        POINT pt;
        pt.x = (int)(short)LOWORD(lp);
        pt.y = (int)(short)HIWORD(lp);

        /*
         * The strip is solid while the pointer is on it and faded otherwise -
         * see the alpha pass. Repainted only when that changes, so crossing
         * the line costs one strip repaint rather than one per mouse move.
         */
        {
            const BOOL in_top = (pt.y < TOOLBAR_H) ? TRUE : FALSE;
            if (in_top != o->top_hover) {
                o->top_hover = in_top;
                RECT bar = { 0, 0, o->rect.right - o->rect.left, TOOLBAR_H };
                InvalidateRect(hwnd, &bar, FALSE);
            }
        }

        /*
         * An open panel first: whether it survives this move has to be decided
         * before anything else looks at the pointer, because a panel that
         * outlives the pointer leaving it is a panel that never goes away.
         *
         * The rule is popup.h's: the pointer on the BUTTON or on the PANEL
         * keeps it up. That one rule is what lets the user travel from the
         * button down onto the panel - the only path anyone takes to reach it.
         */
        /*
         * Whichever pop-up is open - and each one closes by its own rule.
         *
         * The layout panel is HOVER-opened, so it is hover-closed (popup.h's
         * rule). The monitor dropdown is CLICK-opened, so it stays until a
         * click elsewhere or Esc: closing it on mouse-out would make it
         * unreachable, because it hangs BELOW the box whose edge the pointer
         * has to cross to get to it.
         */
        if (popup_current(&o->popup) == POPUP_LAYOUT) {
            if (!popup_keep_hover(&o->popup, pt)) {
                layout_panel_close(o);
            } else {
                const int item = toolbar_panel_hit(&o->popup.panel, (int)SNAP_COUNT, pt);
                if (item != o->panel_hover) {
                    o->panel_hover = item;
                    RECT pr = o->popup.panel;
                    InvalidateRect(hwnd, &pr, FALSE);
                }
            }
        } else if (popup_current(&o->popup) == POPUP_MONITOR) {
            /* Only the highlight follows the pointer here. */
            const int item = toolbar_monitor_hit(&o->popup.panel, o->mon_count, pt);
            if (item != o->panel_hover) {
                o->panel_hover = item;
                RECT pr = o->popup.panel;
                InvalidateRect(hwnd, &pr, FALSE);
            }
        }

        /*
         * The sidebar, before the region drag below: while an application is
         * being carried out of the list, every mouse move belongs to that drag
         * and to nothing else.
         */
        if (sidebar_is_dragging(&o->sidebar)) {
            if (sidebar_drag_move(&o->sidebar, pt)) {
                /* The ghost follows the pointer, so the whole screen is dirty -
                 * one repaint per move, the same cost the region drag below
                 * already pays. */
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        /*
         * A press on a row becomes a DRAG once the pointer has travelled far
         * enough. The threshold is the system's own (SM_CXDRAG/SM_CYDRAG), not
         * a guessed number, so a small tremor of the hand stays a click.
         */
        if (sidebar_pressed_row(&o->sidebar) >= 0) {
            const int dx = (pt.x > o->sb_press_pt.x) ? (pt.x - o->sb_press_pt.x)
                                                     : (o->sb_press_pt.x - pt.x);
            const int dy = (pt.y > o->sb_press_pt.y) ? (pt.y - o->sb_press_pt.y)
                                                     : (o->sb_press_pt.y - pt.y);
            if (dx > SIDEBAR_DRAG_SLOP_X || dy > SIDEBAR_DRAG_SLOP_Y) {
                sidebar_drag_begin(&o->sidebar, pt);
                sidebar_invalidate(o);
                return 0;
            }
        }

        /*
         * The placeholder boxes: a drag in progress owns the pointer until the
         * button comes up, and otherwise the box under the pointer is
         * highlighted so the user can see what a press would grab.
         */
        if (o->ph_drag >= 0) {
            if (o->ph_resizing) {
                (void)placeholder_resize_rect(&o->ph_start, pt, &o->sb_area,
                                              PLACEHOLDER_MIN_W, PLACEHOLDER_MIN_H,
                                              &o->ph_now);
            } else {
                (void)placeholder_move_rect(&o->ph_start,
                                            pt.x - o->ph_origin.x,
                                            pt.y - o->ph_origin.y,
                                            &o->sb_area, &o->ph_now);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        if (o->slot_src.slot_at != NULL && o->ph_count > 0) {
            const PlaceholderHit ph = placeholder_hit(o->ph_boxes, o->ph_count,
                                                      &o->ph_metrics, pt);
            const int want = (ph.kind == PLACEHOLDER_HIT_NONE) ? -1 : ph.index;
            if (want != o->ph_hover) {
                o->ph_hover = want;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }

        /*
         * Slide the panel out or back.
         *
         * AFTER the region drag, so that drawing a rectangle across the screen
         * does not make the panel jump in and out as the pointer crosses the
         * right edge on its way past it.
         */
        if (!o->drag_active) {
            sidebar_pointer_moved(o, pt);
        }

        /*
         * A drag in progress takes priority over hover. Both are handled in this
         * same window, and without this an ordinary hover would keep
         * invalidating the strip while the user was drawing a rectangle.
         */
        if (o->drag_active) {
            const int w = (int)(o->rect.right - o->rect.left);
            const int h = (int)(o->rect.bottom - o->rect.top);

            /* Normalized as it is drawn, so dragging up-left works the same as
             * down-right. Rebuilt from the origin every move rather than
             * accumulated, which is what keeps a drag that changes direction
             * from collapsing to nothing. */
            RECT r;
            r.left   = (pt.x < o->drag_origin.x) ? pt.x : o->drag_origin.x;
            r.top    = (pt.y < o->drag_origin.y) ? pt.y : o->drag_origin.y;
            r.right  = (pt.x > o->drag_origin.x) ? pt.x : o->drag_origin.x;
            r.bottom = (pt.y > o->drag_origin.y) ? pt.y : o->drag_origin.y;

            /*
             * A region MAY reach the very top of the screen (2026-10-10, user
             * decision).
             *
             * It used to be clamped below the strip, on the argument that a
             * region drawn under the toolbar could never be clicked again.
             * That argument is about nothing: regions are not clickable at all,
             * they are only drawn. What the clamp actually did was stop the
             * user drawing "the top half of the screen" by hand.
             *
             * The strip still owns those pixels for INPUT, and it now fades
             * while the pointer is elsewhere, so the part of a region under it
             * remains visible.
             */
            if (r.left   < 0)      r.left = 0;
            if (r.right  > w)      r.right = w;
            if (r.bottom > h)      r.bottom = h;

            snapgrid_snap_rect(&r, &o->lines, SNAP_THRESHOLD_PX);

            if (r.left != o->drag.left || r.top != o->drag.top ||
                r.right != o->drag.right || r.bottom != o->drag.bottom) {
                o->drag = r;
                /* The whole screen: the old rectangle has to disappear as well
                 * as the new one appear. */
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        /*
         * The name box's two buttons, while they are up. Their hover state is
         * kept apart from the toolbar's: they belong to a different control
         * with a different lifetime, and one shared variable would leave a
         * highlight behind on whichever of them was under the pointer when the
         * box closed.
         */
        if (o->name_editing) {
            int nb = -1;
            if (PtInRect(&o->name_box.save, pt))   nb = 0;
            if (PtInRect(&o->name_box.cancel, pt)) nb = 1;
            if (nb != o->name_hover) {
                o->name_hover = nb;
                RECT bar = { 0, 0, o->rect.right - o->rect.left, TOOLBAR_H };
                InvalidateRect(hwnd, &bar, FALSE);
            }
        }

        if (o->item_count <= 0) break;

        const ToolbarButton hit = toolbar_hit_test(o->items, o->item_count, pt);
        if (hit != o->hover) {
            o->hover = hit;
            RECT bar = { 0, 0, o->rect.right - o->rect.left, TOOLBAR_H };
            InvalidateRect(hwnd, &bar, FALSE);
        }

        /*
         * Hovering [Layout format] opens the panel. It is NOT a click - that is
         * the design the user asked for (2026-10-09), and the first version got
         * it wrong: the templates are a set of choices to BROWSE, and hiding
         * them behind a press hides what the tool can do.
         *
         * By definition the pointer is on the anchor here, so the hover rule
         * keeps the panel up until the pointer leaves BOTH the button and the
         * panel - which is exactly what makes the trip down onto a thumbnail
         * safe.
         */
        if (hit == TB_LAYOUT && !popup_is_open(&o->popup)) {
            layout_panel_open(o);
        }

        /* Without this the hover state sticks to the last button the pointer
         * crossed, and the toolbar looks like it is still tracking a mouse
         * that left the screen. */
        TRACKMOUSEEVENT tme;
        ZeroMemory(&tme, sizeof(tme));
        tme.cbSize    = sizeof(tme);
        tme.dwFlags   = TME_LEAVE;
        tme.hwndTrack = hwnd;
        (void)TrackMouseEvent(&tme);
        return 0;
    }

    /*
     * The wheel scrolls the list, and only when the pointer is over it - the
     * wheel belongs to whatever the pointer is on, and this window covers the
     * whole screen. Three rows a notch: one per notch would take 300 notches to
     * cross a long list.
     */
    case WM_MOUSEWHEEL: {
        if (o == NULL || !sidebar_enabled(o)) break;

        POINT pt;
        if (!GetCursorPos(&pt)) break;
        (void)ScreenToClient(hwnd, &pt);

        const SidebarHit sh = sidebar_hit(&o->sidebar, &o->sb_metrics, &o->sb_area,
                                          sb_visible_count(o), pt);
        if (sh.kind != SIDEBAR_HIT_ROW && sh.kind != SIDEBAR_HIT_BODY &&
            sh.kind != SIDEBAR_HIT_HEADER && sh.kind != SIDEBAR_HIT_SCROLLBAR) {
            break;
        }

        const int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        if (steps == 0) return 0;

        sidebar_scroll_by(&o->sidebar, -steps * 3, sb_visible_count(o),
                          sidebar_visible_rows(&o->sb_metrics, &o->sb_area));
        sidebar_invalidate(o);
        return 0;
    }

    /*
     * The search box's notifications.
     *
     * EN_CHANGE goes to the PARENT - this window - and NOT to the EDIT itself.
     * MEASURED 2026-10-10 on the first run of probe_search.ps1: subclassing the
     * control does not let it see its own notifications, so the filter never
     * ran and the list never narrowed - while everything else about the box
     * worked, which is exactly how a "the search does nothing" report looks.
     */
    case WM_COMMAND:
        if (o != NULL && o->sb_edit != NULL && (HWND)lp == o->sb_edit &&
            HIWORD(wp) == EN_CHANGE) {
            filter_edit_changed(o);
            return 0;
        }
        break;

    case WM_MOUSELEAVE:
        if (o != NULL) {
            BOOL dirty = FALSE;
            if (o->hover != TB_COUNT) {
                o->hover = TB_COUNT;
                dirty = TRUE;
            }
            if (o->name_hover != -1) {
                o->name_hover = -1;
                dirty = TRUE;
            }
            if (dirty) {
                RECT bar = { 0, 0, o->rect.right - o->rect.left, TOOLBAR_H };
                InvalidateRect(hwnd, &bar, FALSE);
            }
        }
        return 0;

    case WM_LBUTTONDOWN: {
        if (o == NULL) break;

        POINT pt;
        pt.x = (int)(short)LOWORD(lp);
        pt.y = (int)(short)HIWORD(lp);

        /*
         * An open panel comes FIRST - before the canvas, before the buttons.
         *
         * This is the ordering rule popup.h writes down, and it is
         * load-bearing: the panel hangs BELOW the toolbar, which is exactly
         * the region where a press starts a free-hand region drag. Without
         * this test, clicking a layout thumbnail would draw a rectangle across
         * the screen instead of choosing a layout.
         *
         * Anywhere else - the canvas, another button, even the button that
         * opened it - closes the panel, and the click is CONSUMED. Letting it
         * through would be the same bug in a different place.
         */
        if (popup_is_open(&o->popup)) {
            const PopupHit ph    = popup_hit(&o->popup, pt);
            const PopupId  which = popup_current(&o->popup);

            if (ph == POPUP_HIT_PANEL) {
                if (which == POPUP_LAYOUT)       layout_panel_click(o, pt);
                else if (which == POPUP_MONITOR) monitor_panel_click(o, pt);
                return 0;
            }

            if (ph == POPUP_HIT_ANCHOR && which == POPUP_LAYOUT) {
                /*
                 * The layout button opens its panel by HOVERING, so closing it
                 * on a click there would only make the next mouse move open it
                 * again - a flicker, and a click that appears to do nothing.
                 * The monitor box is click-opened, so a second click on IT does
                 * close its list (the branch below).
                 */
                return 0;
            }

            /* Anywhere else - a second click on the monitor box, the canvas,
             * another button - closes it, and the click is CONSUMED. Letting it
             * through would reach the canvas handler below and start drawing a
             * region under the panel the user was aiming at. */
            if (which == POPUP_LAYOUT)       layout_panel_close(o);
            else if (which == POPUP_MONITOR) monitor_panel_close(o);
            return 0;
        }

        /*
         * The monitor box: its list opens on a CLICK, not on hover. That is
         * what the user asked for ("显示器选择功能用下拉栏实现", 2026-10-09) and
         * what a dropdown does - a list that appeared whenever the pointer
         * crossed the middle of the strip would be in the way constantly.
         */
        if (o->mon_box_valid && PtInRect(&o->mon_box, pt)) {
            monitor_panel_open(o);
            return 0;
        }

        /*
         * A click anywhere but on the search box ends the search.
         *
         * The EDIT window receives the clicks that land on IT, so anything
         * arriving here is by definition somewhere else - which is the rule
         * every search field on the desktop follows.
         */
        if (o->sb_edit != NULL) {
            const SidebarHit fh = sidebar_hit(&o->sidebar, &o->sb_metrics,
                                              &o->sb_area, sb_visible_count(o), pt);
            if (fh.kind != SIDEBAR_HIT_FILTER) {
                filter_edit_close(o);
            }
        }

        /*
         * The sidebar: line 4 of the order popup.h writes down, and it has to
         * be asked BEFORE the canvas below - because the canvas starts a region
         * drag for any press under the strip, which is exactly where the panel
         * is. Without this, pressing a row would draw a rectangle instead.
         */
        if (sidebar_enabled(o)) {
            const int  total   = sb_visible_count(o);
            const int  visible = sidebar_visible_rows(&o->sb_metrics, &o->sb_area);
            const SidebarHit sh = sidebar_hit(&o->sidebar, &o->sb_metrics,
                                              &o->sb_area, total, pt);

            switch (sh.kind) {
            case SIDEBAR_HIT_HANDLE:
            case SIDEBAR_HIT_EDGE:
                /* The strip and its handle are the way in, so a press on either
                 * opens the panel - they are not hover-only zones, or they
                 * could not be used with a pen or a touchpad tap. */
                sidebar_set_open(&o->sidebar, TRUE, GetTickCount64());
                sidebar_timer_sync(o);
                sidebar_invalidate(o);
                return 0;

            case SIDEBAR_HIT_ROW:
                sidebar_press(&o->sidebar, sh.row);
                o->sb_press_pt = pt;
                (void)SetCapture(hwnd);   /* the drag may leave the window */
                sidebar_invalidate(o);
                return 0;

            case SIDEBAR_HIT_SCROLLBAR: {
                RECT track, thumb;
                if (sidebar_scrollbar_rects(&o->sidebar, &o->sb_metrics, &o->sb_area,
                                            total, visible, &track, &thumb)) {
                    sidebar_scroll_to(&o->sidebar,
                        sidebar_scroll_from_thumb(&track, thumb.bottom - thumb.top,
                                                  total, visible, pt.y),
                        total, visible);
                }
                /* Follow the pointer from here: grabbing the thumb and having
                 * it jump once is not dragging. */
                o->sb_scroll_drag = TRUE;
                (void)SetCapture(hwnd);
                sidebar_invalidate(o);
                return 0;
            }

            case SIDEBAR_HIT_FILTER:
                (void)filter_edit_begin(o);
                return 0;

            case SIDEBAR_HIT_HEADER:
            case SIDEBAR_HIT_BODY:
                /* The panel's own background. Consumed: a press here must not
                 * reach the canvas and start a region under the panel. */
                return 0;

            default:
                break;   /* not on the sidebar - the canvas gets it */
            }
        }

        /*
         * The placeholder boxes, before the canvas.
         *
         * A press on a box moves it; a press on the empty canvas draws a new
         * region. That single distinction is the whole of "change what is
         * there" versus "add something" in this editor, so it is decided here
         * and nowhere else.
         */
        if (o->slot_src.slot_at != NULL && o->ph_count > 0) {
            const PlaceholderHit ph = placeholder_hit(o->ph_boxes, o->ph_count,
                                                      &o->ph_metrics, pt);
            if (ph.kind != PLACEHOLDER_HIT_NONE && ph.index >= 0) {
                o->ph_drag     = ph.index;
                o->ph_resizing = (ph.kind == PLACEHOLDER_HIT_GRIP) ? TRUE : FALSE;
                o->ph_origin   = pt;
                o->ph_start    = o->ph_boxes[ph.index];
                o->ph_now      = o->ph_start;
                (void)SetCapture(hwnd);   /* the drag may leave the window */
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
        }

        /*
         * Below the strip: begin drawing a region (task 6.5).
         *
         * SetCapture matters here. Without it, letting go outside the window -
         * which is easy to do on a full-screen overlay - would leave the drag
         * stuck on, and the next mouse move anywhere would keep resizing a
         * rectangle nobody is holding.
         */
        if (pt.y >= TOOLBAR_H) {
            o->drag_active = TRUE;
            o->drag_origin = pt;
            o->drag.left   = pt.x;
            o->drag.top    = pt.y;
            o->drag.right  = pt.x;
            o->drag.bottom = pt.y;
            (void)SetCapture(hwnd);
            return 0;
        }

        /*
         * The name box first, before the toolbar buttons.
         *
         * It is drawn to the left of them, so the two hit tests cannot both
         * match - but testing it first makes that a property of the code rather
         * than of the current layout.
         */
        if (o->name_box.valid) {
            if (o->name_editing) {
                if (PtInRect(&o->name_box.save, pt)) {
                    name_edit_commit(o);
                    return 0;
                }
                if (PtInRect(&o->name_box.cancel, pt)) {
                    name_edit_cancel(o);
                    return 0;
                }
            } else if (PtInRect(&o->name_box.name, pt)) {
                (void)name_edit_begin(o);
                return 0;
            }
        }

        if (o->item_count <= 0) break;

        const ToolbarButton hit = toolbar_hit_test(o->items, o->item_count, pt);
        if (hit != TB_COUNT) {
            o->pressed = hit;
            RECT bar = { 0, 0, o->rect.right - o->rect.left, TOOLBAR_H };
            InvalidateRect(hwnd, &bar, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (o == NULL) break;

        POINT pt;
        pt.x = (int)(short)LOWORD(lp);
        pt.y = (int)(short)HIWORD(lp);

        /*
         * A placeholder drag, first: it owns the pointer until the button comes
         * up, exactly like the sidebar's item drag.
         *
         * Only a REAL change is reported. A press and release that moved
         * nothing is a click, and a click on a box is not an edit - reporting
         * it would rewrite the config file for a gesture the user did not make.
         */
        if (o->ph_drag >= 0) {
            const int  idx   = o->ph_drag;
            const RECT final = o->ph_now;

            o->ph_drag = -1;
            (void)ReleaseCapture();

            if (final.left != o->ph_start.left || final.top    != o->ph_start.top ||
                final.right != o->ph_start.right || final.bottom != o->ph_start.bottom) {
                RelRect zone;
                if (placeholder_zone_from_rect(&final, &o->sb_area, &zone) &&
                    o->slot_src.slot_moved != NULL) {
                    log_info(L"overlay: box %d -> %.4f,%.4f %.4fx%.4f", idx,
                             zone.x, zone.y, zone.w, zone.h);
                    o->slot_src.slot_moved(o->slot_src.ctx, idx, &zone);
                }
            }

            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        /* Dragging the scrollbar thumb: follow the pointer, then let go. */
        if (o->sb_scroll_drag) {
            if (sidebar_enabled(o)) {
                RECT track, thumb;
                const int total   = sb_visible_count(o);
                const int visible = sidebar_visible_rows(&o->sb_metrics, &o->sb_area);
                if (sidebar_scrollbar_rects(&o->sidebar, &o->sb_metrics, &o->sb_area,
                                            total, visible, &track, &thumb)) {
                    sidebar_scroll_to(&o->sidebar,
                        sidebar_scroll_from_thumb(&track, thumb.bottom - thumb.top,
                                                  total, visible, pt.y),
                        total, visible);
                }
            }
            o->sb_scroll_drag = FALSE;
            (void)ReleaseCapture();
            sidebar_invalidate(o);
            return 0;
        }

        /*
         * An application carried out of the list and let go.
         *
         * The drop is only honoured over the CANVAS: released back on the panel
         * or on the strip it is a cancelled drag, not a drop somewhere the user
         * cannot see. The overlay still writes nothing - it reports where, and
         * what that means is the caller's business (overlay.h).
         */
        if (sidebar_is_dragging(&o->sidebar)) {
            const int row = sidebar_drag_row(&o->sidebar);
            sidebar_drag_end(&o->sidebar);
            (void)ReleaseCapture();

            const SidebarHit sh = sidebar_hit(&o->sidebar, &o->sb_metrics,
                                              &o->sb_area, sb_visible_count(o), pt);
            const BOOL on_canvas = (sh.kind == SIDEBAR_HIT_NONE && pt.y >= TOOLBAR_H);

            if (on_canvas && row >= 0 && row < sb_visible_count(o)) {
                /* The row the user sees is a VISIBLE row; the search box may
                 * have put a different source row behind it. */
                const int src = sb_row_at(o, row);
                const wchar_t *path = (src >= 0)
                                    ? o->sb_src.path_at(o->sb_src.ctx, src) : NULL;
                RelRect zone;

                if (path != NULL && path[0] != L'\0' &&
                    sidebar_drop_zone(&o->sb_area, pt, SIDEBAR_DROP_W,
                                      SIDEBAR_DROP_H, &zone)) {
                    log_info(L"sidebar: dropped row %d at %ld,%ld", src,
                             (long)pt.x, (long)pt.y);
                    if (o->sb_drop != NULL) {
                        o->sb_drop(path, &zone, o->sb_drop_ctx);
                    }
                } else {
                    /* Listed but not launchable: said, not silently ignored -
                     * the row looks exactly like the ones that work. */
                    log_info(L"sidebar: row %d has no launchable path", src);
                }
            }

            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        /* A press on a row that never became a drag: a click. The press state
         * is cleared and nothing else happens - selecting a row is not an
         * action, and inventing one here would be a second way to create a
         * preset, with a different meaning from dropping it. */
        if (sidebar_pressed_row(&o->sidebar) >= 0) {
            sidebar_release_press(&o->sidebar);
            (void)ReleaseCapture();
            sidebar_invalidate(o);
            return 0;
        }

        /* End a region drag, if one was running. */
        if (o->drag_active) {
            o->drag_active = FALSE;
            (void)ReleaseCapture();

            const int w = (int)(o->rect.right - o->rect.left);
            const int h = (int)(o->rect.bottom - o->rect.top);
            const int dw = (int)(o->drag.right - o->drag.left);
            const int dh = (int)(o->drag.bottom - o->drag.top);

            /*
             * A click is not a rectangle. Without a floor, a stray click while
             * aiming at nothing would leave a three-pixel region on screen that
             * can never be selected to remove.
             */
            if (dw >= 40 && dh >= 40 && o->region_count < SNAP_MAX_REGIONS) {
                RelRect *r = &o->regions[o->region_count];
                r->x = (float)o->drag.left / (float)w;
                r->y = (float)o->drag.top / (float)h;
                r->w = (float)dw / (float)w;
                r->h = (float)dh / (float)h;
                o->region_count++;

                /* Hand-drawn regions ARE the custom template, whatever was
                 * selected before: drawing one is how the user says so. */
                o->tpl = SNAP_CUSTOM;

                snapgrid_collect_lines(o->regions, o->region_count, w, h,
                                       &o->lines);

                log_info(L"overlay: region %d drawn %dx%d at %ld,%ld",
                         o->region_count, dw, dh,
                         (long)o->drag.left, (long)o->drag.top);
            }

            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        if (o->item_count <= 0) break;

        const ToolbarButton was = o->pressed;
        o->pressed = TB_COUNT;
        if (was != TB_COUNT) {
            RECT bar = { 0, 0, o->rect.right - o->rect.left, TOOLBAR_H };
            InvalidateRect(hwnd, &bar, FALSE);
        }

        /* A press counts only if it is released over the same button. That is
         * what every other button on the desktop does, and it is also the only
         * way to cancel a press that was a mistake. */
        const ToolbarButton hit = toolbar_hit_test(o->items, o->item_count, pt);
        if (was == TB_COUNT || hit != was) {
            return 0;
        }

        log_info(L"overlay: toolbar button '%ls' clicked",
                 toolbar_button_name(hit));

        if (hit == TB_EXIT) {
            report_exit(o, OVERLAY_EXIT_TOOLBAR);
        } else if (hit == TB_APPLY) {
            /*
             * [Apply] writes the boxes out as presets - or rather, it TELLS the
             * caller to: the overlay does not know what a preset is, and this
             * division is what has kept every other decision about storage on
             * the other side of the boundary.
             */
            if (o->slot_src.apply != NULL) {
                log_info(L"overlay: [apply] pressed");
                o->slot_src.apply(o->slot_src.ctx);
            }
        } else if (hit == TB_RESET) {
            /*
             * [Reset] reloads the boxes from the caller. The overlay holds no
             * copy to restore, which is the same division every other callback
             * here follows - and it means "reset" cannot disagree with what the
             * session started with.
             */
            if (o->slot_src.reset != NULL) {
                log_info(L"overlay: [reset] pressed");
                o->slot_src.reset(o->slot_src.ctx);
            }
        } else if (hit == TB_PICK) {
            /*
             * [Pick up this layout] reads the screen INTO the store, which is
             * the opposite of [Apply] - and the overlay is the wrong side of
             * that boundary to do it: the windows worth picking up are the ones
             * underneath this one.
             *
             * This button sat greyed out for a long time because the command
             * line behind it silently did nothing
             * ("wac pick does not write the file, cause unknown"). The cause
             * turned out to be a whitelist missing the word "pick", so the
             * command was never executed at all.
             */
            if (o->slot_src.pick != NULL) {
                log_info(L"overlay: [pick] pressed");
                o->slot_src.pick(o->slot_src.ctx);
            }
        } else if (hit == TB_LAYOUT) {
            /*
             * The one button that opens something rather than acting at once.
             * The templates themselves are chosen inside the panel, which is
             * where the choice belongs - and where a thumbnail can show what
             * each one means without spending a caption on every button.
             */
            layout_panel_open(o);
        }
        /*
         * Every button on the strip is live now. The mask above is what the
         * toolbar draws from, and each handler below still checks its callback
         * for NULL - a caller that does not provide one gets a button that does
         * nothing rather than a crash, and main.c provides all of them.
         */
        return 0;
    }

    case WM_TIMER:
        /*
         * The opening text has had its five seconds. Take it down and repaint
         * once, so the rest of the session runs on a clean canvas. One shot -
         * no repeat is set, and the timer is killed here rather than left to
         * fire again.
         */
        if (wp == OVERLAY_INTRO_TIMER_ID && o != NULL) {
            (void)KillTimer(o->hwnd, OVERLAY_INTRO_TIMER_ID);
            o->intro = FALSE;
            if (o->hwnd != NULL) {
                InvalidateRect(o->hwnd, NULL, FALSE);
            }
            return 0;
        }

        /*
         * The sidebar's slide. One frame per tick, and the timer STOPS as soon
         * as there is no next frame - so a parked panel costs nothing, which is
         * what makes the slide affordable at all.
         */
        if (wp == OVERLAY_SIDEBAR_TIMER_ID && o != NULL) {
            if (sidebar_advance(&o->sidebar, GetTickCount64())) {
                sidebar_invalidate(o);
            } else {
                sidebar_timer_sync(o);      /* it says no: stop the timer */
                sidebar_invalidate(o);      /* ...but draw the frame it ended on */
            }
            return 0;
        }

        /* Z-order maintenance (task 6.6). Nothing repaints here: raising a
         * window does not change what it contains, and the measured cost above
         * is for the SetWindowPos call alone. */
        if (wp == OVERLAY_TOPMOST_TIMER_ID && o != NULL) {
            if (overlay_reassert_topmost(o)) {
                o->reasserts++;
            }
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (o == NULL) break;

        /* Esc, and Ctrl+Q for the muscle memory of anyone who closes things
         * that way. Both are checked here rather than registered as hotkeys:
         * a global Esc would swallow the key for every other program on the
         * machine, which is not a price this program is willing to charge. */
        if (wp == VK_ESCAPE) {
            /*
             * The order in popup.h: close what is open before leaving the mode.
             * An Esc that jumps straight out of edit mode while a panel is up
             * is the "this key is broken" complaint - and the layering already
             * has a precedent in this file (the name box takes Esc first while
             * the user is typing).
             */
            if (popup_is_open(&o->popup)) {
                /* Either list closes on Esc before edit mode does; which list
                 * is open decides which one (popup.h's ordering table). */
                if (popup_current(&o->popup) == POPUP_LAYOUT) layout_panel_close(o);
                else                                          monitor_panel_close(o);
                return 0;
            }

            /*
             * Then the sidebar - the next layer down in the same table. It is
             * dismissed rather than left to the pointer: without this, Esc would
             * either do nothing or drop the user all the way out of edit mode
             * while they were only trying to close the list.
             *
             * sb_suppress is what keeps the dismissal from being undone by the
             * next mouse move, since the pointer is usually still on the panel
             * the user just closed.
             */
            if (sidebar_progress(&o->sidebar) > 0) {
                sidebar_drag_end(&o->sidebar);
                sidebar_set_open(&o->sidebar, FALSE, GetTickCount64());
                sidebar_timer_sync(o);
                sidebar_invalidate(o);
                o->sb_suppress = TRUE;
                return 0;
            }

            report_exit(o, OVERLAY_EXIT_ESCAPE);
            return 0;
        }
        if (wp == 'Q' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            report_exit(o, OVERLAY_EXIT_ESCAPE);
            return 0;
        }
        break;

    case WM_CLOSE:
        /* Alt+F4 from the keyboard. Same path as everything else. */
        if (o != NULL) {
            report_exit(o, OVERLAY_EXIT_TOOLBAR);
            return 0;
        }
        break;

    case WM_DESTROY:
        if (o != NULL) {
            o->hwnd = NULL;
        }
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* The layout panel                                                    */
/* ------------------------------------------------------------------ */

/*
 * Apply a template to the canvas.
 *
 * One place, so that the rule "the regions changed, therefore everything is
 * repainted" cannot be remembered by one caller and forgotten by another - the
 * same reason the old button path had a single mapping function.
 */
static void apply_template(Overlay *o, SnapTemplate tpl)
{
    if (o == NULL || o->hwnd == NULL) return;
    if ((int)tpl < 0 || (int)tpl >= (int)SNAP_COUNT) return;

    o->tpl = tpl;
    o->region_count = snapgrid_build(tpl, SNAP_GAP_DEFAULT_PX,
                                     (int)(o->rect.right - o->rect.left),
                                     (int)(o->rect.bottom - o->rect.top),
                                     o->regions, SNAP_MAX_REGIONS);

    log_info(L"overlay: template '%ls' -> %d region(s)",
             snapgrid_name(tpl), o->region_count);

    /* The whole screen, not just the strip: the regions are what changed. */
    InvalidateRect(o->hwnd, NULL, FALSE);
}

static void layout_panel_open(Overlay *o)
{
    if (o == NULL || o->hwnd == NULL) return;

    /* The button it hangs from. Without one there is nothing to anchor to, and
     * a panel floating in the middle of the screen would be worse than none. */
    BOOL found = FALSE;
    RECT anchor;
    ZeroMemory(&anchor, sizeof(anchor));
    for (int i = 0; i < o->item_count; ++i) {
        if (o->items[i].id == TB_LAYOUT) {
            anchor = o->items[i].rect;
            found  = TRUE;
            break;
        }
    }
    if (!found) return;

    int want_w = 0, want_h = 0;
    toolbar_panel_size(SNAP_COUNT, &want_w, &want_h);

    /* The overlay's own rectangle is the screen it covers, and its client
     * origin is (0,0), so this is the screen in the same coordinates as the
     * button rectangles. */
    RECT screen;
    screen.left   = 0;
    screen.top    = 0;
    screen.right  = (int)(o->rect.right - o->rect.left);
    screen.bottom = (int)(o->rect.bottom - o->rect.top);

    RECT panel;
    if (!popup_place_below(&anchor, want_w, want_h, &screen, &panel)) return;

    popup_open(&o->popup, POPUP_LAYOUT, &anchor, &panel);
    o->panel_hover = -1;

    /* Full repaint: the panel covers canvas that has to be redrawn around it.
     * Opening happens on a click, so the cost is invisible. */
    InvalidateRect(o->hwnd, NULL, FALSE);
}

static void layout_panel_close(Overlay *o)
{
    if (o == NULL || o->hwnd == NULL) return;
    if (popup_current(&o->popup) != POPUP_LAYOUT) return;

    popup_close(&o->popup);
    o->panel_hover = -1;

    /* Full repaint for the same reason as opening: whatever the panel covered
     * has to come back, and a partial repaint would need this function to know
     * what that was. */
    InvalidateRect(o->hwnd, NULL, FALSE);
}

static void layout_panel_click(Overlay *o, POINT pt)
{
    if (o == NULL) return;

    const int item = toolbar_panel_hit(&o->popup.panel, SNAP_COUNT, pt);
    if (item < 0 || item >= (int)SNAP_COUNT) return;

    /*
     * The panel lists the templates in SnapTemplate order, so the item index IS
     * the template. That is what replaced snapgrid_from_button(): there is no
     * mapping left to write down, and therefore none to drift.
     */
    layout_panel_close(o);
    apply_template(o, (SnapTemplate)item);
}

static void draw_layout_panel(Overlay *o, const RECT *panel)
{
    if (o == NULL || o->mem_dc == NULL || panel == NULL) return;

    /* Body and border, the same treatment the toolbar gets, so the two read as
     * the same kind of surface rather than as two different inventions. */
    SetDCBrushColor(o->mem_dc, RGB(18, 24, 38));
    FillRect(o->mem_dc, panel, (HBRUSH)GetStockObject(DC_BRUSH));

    SetDCPenColor(o->mem_dc, RGB(70, 92, 132));
    SelectObject(o->mem_dc, GetStockObject(DC_PEN));
    SelectObject(o->mem_dc, GetStockObject(NULL_BRUSH));
    Rectangle(o->mem_dc, panel->left, panel->top, panel->right, panel->bottom);

    HFONT old_font = (HFONT)SelectObject(o->mem_dc, o->font_small);
    SetBkMode(o->mem_dc, TRANSPARENT);

    for (int i = 0; i < (int)SNAP_COUNT; ++i) {
        RECT item, thumb;
        if (!toolbar_panel_item_rect(panel, (int)SNAP_COUNT, i, &item)) continue;
        if (!toolbar_panel_thumb_rect(panel, (int)SNAP_COUNT, i, &thumb)) continue;

        const BOOL hot = (i == o->panel_hover);

        SetDCBrushColor(o->mem_dc, hot ? RGB(38, 58, 96) : RGB(24, 32, 50));
        FillRect(o->mem_dc, &item, (HBRUSH)GetStockObject(DC_BRUSH));

        /*
         * The thumbnail is the TEMPLATE'S OWN regions, built for a screen the
         * size of the box. Drawing it from snapgrid_build() rather than by hand
         * is what stops the picture from disagreeing with what the template
         * actually does - and it makes SNAP_CUSTOM show an empty box, which is
         * exactly what "custom, nothing drawn yet" is.
         */
        RelRect cells[SNAP_MAX_REGIONS];
        const int tw = (int)(thumb.right - thumb.left);
        const int th = (int)(thumb.bottom - thumb.top);
        const int n  = snapgrid_build((SnapTemplate)i, 2, tw, th,
                                      cells, SNAP_MAX_REGIONS);

        for (int c = 0; c < n; ++c) {
            RECT box;
            box.left   = thumb.left + (int)(cells[c].x * (float)tw);
            box.top    = thumb.top  + (int)(cells[c].y * (float)th);
            box.right  = box.left   + (int)(cells[c].w * (float)tw);
            box.bottom = box.top    + (int)(cells[c].h * (float)th);
            if (box.right <= box.left || box.bottom <= box.top) continue;

            SetDCBrushColor(o->mem_dc, hot ? RGB(150, 190, 250)
                                           : RGB(112, 142, 192));
            FillRect(o->mem_dc, &box, (HBRUSH)GetStockObject(DC_BRUSH));
        }

        /* Caption under the thumbnail. The rectangle comes from the same item
         * the thumbnail did, so the two cannot overlap. */
        RECT cap = item;
        cap.top = thumb.bottom + 4;

        SetTextColor(o->mem_dc, hot ? RGB(240, 246, 255) : RGB(205, 216, 236));
        /*
         * DT_VCENTER, not DT_TOP. The caption strip is deliberately taller than
         * the line box (see TB_PANEL_CAPTION_H), and centring splits that slack
         * between the two edges instead of spending all of it above the text -
         * which is what pushed the descenders out of the rectangle.
         */
        DrawTextW(o->mem_dc, i18n_text(snapgrid_label_id((SnapTemplate)i)), -1,
                  &cap, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    }

    SelectObject(o->mem_dc, old_font);
}

/* ------------------------------------------------------------------ */
/* The monitor box and its dropdown                                    */
/* ------------------------------------------------------------------ */

/*
 * Cache the monitor list, the box's geometry, and the name it shows.
 *
 * Called where the screen can change - at create, and after a move. Never from
 * a paint: monitor_enumerate() reads every monitor's EDID out of the registry,
 * and a paint runs on every mouse move.
 */
static void refresh_monitor_box(Overlay *o)
{
    if (o == NULL) return;

    o->mon_count = monitor_enumerate(o->mons, WAC_MON_MAX);

    o->mon_name[0] = L'\0';
    if (o->monitor_index >= 0 && o->monitor_index < o->mon_count) {
        const MonitorFingerprint *m = &o->mons[o->monitor_index];
        const wchar_t *nm = (m->friendly_name[0] != L'\0') ? m->friendly_name
                                                           : m->device_name;
        _snwprintf_s(o->mon_name, 128, _TRUNCATE, i18n_text(STR_MONITOR_ITEM),
                     nm, m->width, m->height);
    }

    RECT strip;
    strip.left   = 0;
    strip.top    = 0;
    strip.right  = (int)(o->rect.right - o->rect.left);
    strip.bottom = TOOLBAR_H;

    o->mon_box_valid = toolbar_monitor_box(&strip, o->items, o->item_count,
                                           o->name_box.valid ? &o->name_box : NULL,
                                           &o->mon_box);
}

static void monitor_panel_open(Overlay *o)
{
    if (o == NULL || o->hwnd == NULL) return;
    if (!o->mon_box_valid || o->mon_count <= 0) return;

    const int want_h = toolbar_monitor_list_height(o->mon_count);
    const int want_w = (int)(o->mon_box.right - o->mon_box.left);

    RECT screen;
    screen.left   = 0;
    screen.top    = 0;
    screen.right  = (int)(o->rect.right - o->rect.left);
    screen.bottom = (int)(o->rect.bottom - o->rect.top);

    RECT panel;
    if (!popup_place_below(&o->mon_box, want_w, want_h, &screen, &panel)) return;

    popup_open(&o->popup, POPUP_MONITOR, &o->mon_box, &panel);
    o->panel_hover = -1;
    InvalidateRect(o->hwnd, NULL, FALSE);
}

static void monitor_panel_close(Overlay *o)
{
    if (o == NULL || o->hwnd == NULL) return;
    /* Its OWN list, not just "a list": a function named for one panel must
     * never close the other one. Same rule as layout_panel_close(). */
    if (popup_current(&o->popup) != POPUP_MONITOR) return;

    popup_close(&o->popup);
    o->panel_hover = -1;
    InvalidateRect(o->hwnd, NULL, FALSE);
}

static void monitor_panel_click(Overlay *o, POINT pt)
{
    if (o == NULL) return;

    const int item = toolbar_monitor_hit(&o->popup.panel, o->mon_count, pt);
    if (item < 0 || item >= o->mon_count) return;

    if (item == o->monitor_index) {
        /* The monitor it is already on: close, and do NOT report a move that
         * did not happen. A log line saying "moved" when nothing moved is the
         * kind of diagnostic that teaches a reader to ignore the log. */
        monitor_panel_close(o);
        return;
    }

    /* The move drops the pop-up state itself and rebuilds everything that
     * depends on the screen size; if it refuses, close the panel anyway so the
     * user is not left with a list that no longer applies. */
    if (!overlay_move_to_monitor(o, item)) {
        monitor_panel_close(o);
    }
}

static void draw_monitor_box(Overlay *o)
{
    if (o == NULL || o->mem_dc == NULL || !o->mon_box_valid) return;

    /* Reads as an ordinary dropdown: a field, a border, and a brighter border
     * while its list is open - the same language the name box uses. */
    const BOOL open = (popup_current(&o->popup) == POPUP_MONITOR);

    HFONT old = (HFONT)SelectObject(o->mem_dc, o->font_small);
    SetBkMode(o->mem_dc, TRANSPARENT);

    SetDCBrushColor(o->mem_dc, open ? RGB(20, 30, 52) : RGB(12, 18, 30));
    FillRect(o->mem_dc, &o->mon_box, (HBRUSH)GetStockObject(DC_BRUSH));

    SetDCPenColor(o->mem_dc, open ? RGB(130, 170, 235) : RGB(60, 80, 118));
    SelectObject(o->mem_dc, GetStockObject(DC_PEN));
    SelectObject(o->mem_dc, GetStockObject(NULL_BRUSH));
    Rectangle(o->mem_dc, o->mon_box.left, o->mon_box.top,
              o->mon_box.right, o->mon_box.bottom);

    RECT txt = o->mon_box;
    txt.left  += toolbar_px(10);
    txt.right -= toolbar_px(10);
    SetTextColor(o->mem_dc, RGB(225, 235, 255));
    DrawTextW(o->mem_dc, o->mon_name, -1, &txt,
              DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS |
              DT_NOPREFIX);

    SelectObject(o->mem_dc, old);
}

static void draw_monitor_panel(Overlay *o, const RECT *panel)
{
    if (o == NULL || o->mem_dc == NULL || panel == NULL) return;

    SetDCBrushColor(o->mem_dc, RGB(18, 24, 38));
    FillRect(o->mem_dc, panel, (HBRUSH)GetStockObject(DC_BRUSH));

    SetDCPenColor(o->mem_dc, RGB(70, 92, 132));
    SelectObject(o->mem_dc, GetStockObject(DC_PEN));
    SelectObject(o->mem_dc, GetStockObject(NULL_BRUSH));
    Rectangle(o->mem_dc, panel->left, panel->top, panel->right, panel->bottom);

    HFONT old = (HFONT)SelectObject(o->mem_dc, o->font_small);
    SetBkMode(o->mem_dc, TRANSPARENT);

    for (int i = 0; i < o->mon_count; ++i) {
        RECT item;
        if (!toolbar_monitor_item_rect(panel, o->mon_count, i, &item)) continue;

        const BOOL hot    = (i == o->panel_hover);
        const BOOL chosen = (i == o->monitor_index);

        SetDCBrushColor(o->mem_dc, hot ? RGB(38, 58, 96) : RGB(24, 32, 50));
        FillRect(o->mem_dc, &item, (HBRUSH)GetStockObject(DC_BRUSH));

        const MonitorFingerprint *m = &o->mons[i];
        const wchar_t *nm = (m->friendly_name[0] != L'\0') ? m->friendly_name
                                                           : m->device_name;
        wchar_t line[192];
        _snwprintf_s(line, 192, _TRUNCATE, i18n_text(STR_MONITOR_ITEM),
                     nm, m->width, m->height);

        RECT txt = item;
        txt.left  += toolbar_px(12);
        txt.right -= toolbar_px(12);

        /* The one in use is marked by colour rather than by a tick: a tick
         * needs a glyph the font may not have, and this list is two lines
         * long on the machine it was built for. */
        SetTextColor(o->mem_dc, chosen ? RGB(150, 220, 170)
                                       : (hot ? RGB(240, 246, 255)
                                              : RGB(205, 216, 236)));
        DrawTextW(o->mem_dc, line, -1, &txt,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
    }

    SelectObject(o->mem_dc, old);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* The sidebar (stage 8, task 2b)                                      */
/* ------------------------------------------------------------------ */

/*
 * Whether there is a list to show at all.
 *
 * Without one the strip is neither drawn nor hit-tested: an edge strip that
 * opens an empty panel is a control promising something it cannot deliver,
 * which is the shape trap 24 warns about.
 */
static BOOL sidebar_enabled(Overlay *o)
{
    return (o->sb_src.name_at != NULL) ? TRUE : FALSE;
}

/*
 * How many rows the panel shows: the whole list, or what is left of it after
 * the search box.
 */
static int sb_visible_count(Overlay *o)
{
    if (!sidebar_enabled(o)) return 0;
    if (o->sb_filter != NULL) return o->sb_filter_count;
    return o->sb_src.count;
}

/*
 * The SOURCE row behind visible row `visible_index`, or -1.
 *
 * Without a filter the two numbers are the same, which is exactly why this is
 * a function: one place decides, and the drawing, the hit test and the drop
 * all ask it rather than each doing the arithmetic for itself.
 */
static int sb_row_at(Overlay *o, int visible_index)
{
    if (visible_index < 0) return -1;

    if (o->sb_filter != NULL) {
        if (visible_index >= o->sb_filter_count) return -1;
        return o->sb_filter[visible_index];
    }

    if (!sidebar_enabled(o) || visible_index >= o->sb_src.count) return -1;
    return visible_index;
}

/*
 * Rebuild the visible-index array from the search box's text.
 *
 * An empty box means NO array at all, and every other function here is written
 * against that state: with sb_filter NULL, visible row N is source row N.
 */
static void sb_apply_filter(Overlay *o)
{
    if (o->sb_filter != NULL) {
        free(o->sb_filter);
        o->sb_filter = NULL;
    }
    o->sb_filter_count = 0;

    const int total = sidebar_enabled(o) ? o->sb_src.count : 0;

    /* The scroll position described the OLD list. Start the new one at the top
     * rather than leaving the panel in the middle of a list nobody has seen. */
    sidebar_scroll_to(&o->sidebar, 0, total,
                      sidebar_visible_rows(&o->sb_metrics, &o->sb_area));

    if (total <= 0 || o->sb_filter_text[0] == L'\0') return;

    int *map = (int *)calloc((size_t)total, sizeof(int));
    if (map == NULL) {
        /* Said, not hidden: the box would otherwise look broken - the text is
         * there and nothing is being filtered. */
        log_info(L"overlay: no memory for the search filter - showing everything");
        return;
    }

    int n = 0;
    for (int i = 0; i < total; ++i) {
        const wchar_t *name = o->sb_src.name_at(o->sb_src.ctx, i);
        if (sidebar_matches(name, o->sb_filter_text)) map[n++] = i;
    }

    /*
     * "Nothing matched" is NOT "no filter": the array is kept even when it is
     * empty, so the panel shows an empty result rather than falling back to all
     * 343 rows - which would look like the search box did nothing.
     */
    o->sb_filter       = map;
    o->sb_filter_count = n;

    log_info(L"sidebar: filter \"%ls\" matches %d of %d",
             o->sb_filter_text, n, total);
}

/*
 * The client area below the strip - the only thing sidebar.c is told about the
 * screen it slides over, which is what keeps that module from having to know a
 * toolbar exists.
 *
 * Recomputed when the geometry changes (create, and a move to another
 * monitor), never per paint.
 */
static void sidebar_layout(Overlay *o)
{
    o->sb_area.left   = 0;
    o->sb_area.top    = TOOLBAR_H;
    o->sb_area.right  = o->rect.right - o->rect.left;
    o->sb_area.bottom = o->rect.bottom - o->rect.top;
}

/*
 * Start or stop the slide timer so that it matches the state machine.
 *
 * This is the ONLY timer this file starts for the sidebar, and it is stopped
 * the moment there is no next frame to draw. A parked panel - out or away -
 * has no timer at all; that is the whole difference between an animation and a
 * repaint loop, and it is what the idle-CPU budget asks for.
 */
static void sidebar_timer_sync(Overlay *o)
{
    if (o->hwnd == NULL) return;

    const BOOL want = sidebar_is_animating(&o->sidebar);
    if (want == o->sb_timer) return;

    if (want) {
        o->sb_timer = (SetTimer(o->hwnd, OVERLAY_SIDEBAR_TIMER_ID,
                                OVERLAY_SIDEBAR_FRAME_MS, NULL) != 0) ? TRUE : FALSE;
        if (!o->sb_timer) {
            /* A timer that failed to start would leave the panel stuck half
             * way out with nothing to advance it - so it is said out loud
             * rather than left to look like a drawing bug. */
            log_info(L"overlay: the sidebar slide timer could not be started");
        }
    } else {
        KillTimer(o->hwnd, OVERLAY_SIDEBAR_TIMER_ID);
        o->sb_timer = FALSE;
    }
}

/*
 * Dirty the strip and the panel's full width.
 *
 * The whole slide happens inside this rectangle, so there is no reason to ask
 * for the whole screen - and asking for less keeps the invalidation honest
 * about what changed.
 */
static void sidebar_invalidate(Overlay *o)
{
    if (o->hwnd == NULL) return;

    RECT r = o->sb_area;
    r.left = o->sb_area.right - o->sb_metrics.body_w - 2;
    if (r.left < o->sb_area.left) r.left = o->sb_area.left;
    InvalidateRect(o->hwnd, &r, FALSE);
}

/*
 * The pointer moved: slide the panel out or back, and track the row under it.
 *
 * The rule is popup.h's, applied to this panel: the pointer on the STRIP or on
 * the PANEL keeps it out, and it goes away once it has left both. That one
 * rule is what makes the trip from the strip onto the list - the only path a
 * user ever takes to it - safe.
 */
static void sidebar_pointer_moved(Overlay *o, POINT pt)
{
    if (o->hwnd == NULL || !sidebar_enabled(o)) return;
    if (sidebar_is_dragging(&o->sidebar)) return;   /* a drag keeps it out */

    const SidebarHit hit = sidebar_hit(&o->sidebar, &o->sb_metrics, &o->sb_area,
                                       sb_visible_count(o), pt);
    const BOOL on = sidebar_hit_keeps_open(hit);

    /*
     * Esc dismissed the panel, and the pointer is probably still sitting on
     * it. Until it leaves, the pointer must not undo that - otherwise the
     * panel the user just closed comes straight back on the next mouse move,
     * and the key reads as broken.
     */
    if (o->sb_suppress) {
        if (!on) o->sb_suppress = FALSE;
        return;
    }

    sidebar_set_open(&o->sidebar, on, GetTickCount64());
    sidebar_timer_sync(o);

    const int row = (hit.kind == SIDEBAR_HIT_ROW) ? hit.row : -1;
    if (row != sidebar_hover_row(&o->sidebar)) {
        sidebar_note_hover(&o->sidebar, row);
    }

    sidebar_invalidate(o);
}

/*
 * The handle: the tab that says this edge does something.
 *
 * It REPLACED three 2-px dots on the strip (user report, 2026-10-10: "the
 * sidebar is not obvious enough - on first use you cannot find it"). Three
 * pixels of signpost on an 8-px strip is not a signpost: the strip itself is
 * only visible because it is slightly darker than the canvas, and the dots
 * were the same colour as the toolbar's captions. A 24-px tab with a
 * left-pointing arrow is a thing you can see from across the desk - and it
 * disappears under the panel the moment the panel arrives, so it never becomes
 * clutter.
 */
static void draw_sidebar_handle(Overlay *o, const RECT *h)
{
    SetDCBrushColor(o->mem_dc, RGB(34, 56, 96));
    FillRect(o->mem_dc, h, (HBRUSH)GetStockObject(DC_BRUSH));

    SetDCPenColor(o->mem_dc, RGB(120, 160, 225));
    SelectObject(o->mem_dc, GetStockObject(DC_PEN));
    SelectObject(o->mem_dc, GetStockObject(NULL_BRUSH));
    Rectangle(o->mem_dc, h->left, h->top, h->right, h->bottom);

    /* "It comes out this way." Drawn as a polygon rather than typed as a
     * glyph: a font that lacks the character would leave an empty box on the
     * one control that has to be understood at a glance. */
    const int w  = h->right - h->left;
    const int cy = (h->top + h->bottom) / 2;
    const int s  = (w >= 30) ? 7 : 5;
    const int cx = h->left + w / 2 + s / 2;

    POINT tri[3];
    tri[0].x = cx - s;  tri[0].y = cy;
    tri[1].x = cx + s;  tri[1].y = cy - (s + s / 2);
    tri[2].x = cx + s;  tri[2].y = cy + (s + s / 2);

    SetDCBrushColor(o->mem_dc, RGB(205, 225, 255));
    SetDCPenColor(o->mem_dc, RGB(205, 225, 255));
    SelectObject(o->mem_dc, GetStockObject(DC_BRUSH));
    SelectObject(o->mem_dc, GetStockObject(DC_PEN));
    Polygon(o->mem_dc, tri, 3);

    SelectObject(o->mem_dc, GetStockObject(NULL_BRUSH));
}

/*
 * The panel: background, header, rows, scrollbar.
 *
 * Only the VISIBLE part of the body is filled; the rest is off screen during
 * the slide, and the next frame would overwrite it anyway. The rows are laid
 * out in the FULL body rectangle (sidebar.h), so the list is revealed by the
 * screen edge rather than pushed by it - text that slid sideways under the
 * pointer would be unreadable exactly while the user is aiming at it.
 */
static void draw_sidebar(Overlay *o)
{
    if (!sidebar_enabled(o)) return;

    const SidebarMetrics *m = &o->sb_metrics;

    /*
     * The strip and its handle first: the body is drawn over whatever part of
     * either is under it, which is exactly what should happen once the panel
     * is out - the signpost has done its job and gets out of the way.
     */
    RECT edge;
    if (sidebar_edge_rect(m, &o->sb_area, &edge)) {
        FillRect(o->mem_dc, &edge, o->brush_bar);
    }

    RECT handle;
    if (sidebar_handle_rect(m, &o->sb_area, &handle)) {
        draw_sidebar_handle(o, &handle);
    }

    RECT vis, body;
    if (!sidebar_body_visible_rect(&o->sidebar, m, &o->sb_area, &vis)) return;
    if (!sidebar_body_rect(&o->sidebar, m, &o->sb_area, &body)) return;

    HFONT old_font = (HFONT)SelectObject(o->mem_dc, o->font_small);
    SetBkMode(o->mem_dc, TRANSPARENT);

    SetDCBrushColor(o->mem_dc, RGB(16, 24, 40));
    FillRect(o->mem_dc, &vis, (HBRUSH)GetStockObject(DC_BRUSH));

    /* A brighter lip on the left edge, so the panel reads as a surface laid
     * over the canvas rather than as a hole in it. */
    RECT lip;
    lip.left   = body.left;
    lip.right  = body.left + 2;
    lip.top    = body.top;
    lip.bottom = body.bottom;
    SetDCBrushColor(o->mem_dc, RGB(70, 100, 150));
    FillRect(o->mem_dc, &lip, (HBRUSH)GetStockObject(DC_BRUSH));

    RECT rows_rect;
    const BOOL have_rows = sidebar_rows_rect(m, &o->sb_area, &rows_rect);
    const int  total     = sb_visible_count(o);
    const int  visible   = sidebar_visible_rows(m, &o->sb_area);

    /* ---- the header: what this list is, and how much of it there is ----
     *
     * Straight from its own geometry, NOT "everything above the rows". That is
     * what it used to be, and it silently became TWO rows tall the moment the
     * search box was added between them: the caption was centred across both
     * and the box covered its lower half. The user saw it before any test did
     * (2026-10-10). `head` starts as the body so that a failure here - which
     * cannot happen, body_rect just succeeded - leaves it somewhere valid.
     */
    RECT head = body;
    (void)sidebar_header_rect(&o->sidebar, m, &o->sb_area, &head);

    SetTextColor(o->mem_dc, RGB(225, 235, 255));
    DrawTextW(o->mem_dc, i18n_text(STR_SIDEBAR_TITLE), -1, &head,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);

    wchar_t right_buf[64];
    const wchar_t *right;
    if (sidebar_is_dragging(&o->sidebar)) {
        right = i18n_text(STR_SIDEBAR_DROP_HINT);
    } else if (o->sb_src.indexing) {
        right = i18n_text(STR_SIDEBAR_INDEXING);
    } else {
        _snwprintf_s(right_buf, 64, _TRUNCATE,
                     i18n_text(STR_SIDEBAR_COUNT_FMT), total);
        right = right_buf;
    }
    SetTextColor(o->mem_dc, RGB(140, 160, 195));
    DrawTextW(o->mem_dc, right, -1, &head,
              DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);

    /* ---- the search box (2026-10-10) ----
     *
     * 343 programs cannot be found by scrolling. The box is drawn here and
     * TYPED INTO through a real EDIT window placed exactly over it (see
     * filter_edit_begin) - a layered window cannot host a control, and
     * self-painted text never reaches the IME.
     */
    RECT fr;
    if (sidebar_filter_rect(&o->sidebar, m, &o->sb_area, &fr)) {
        SetDCBrushColor(o->mem_dc, RGB(12, 18, 30));
        FillRect(o->mem_dc, &fr, (HBRUSH)GetStockObject(DC_BRUSH));

        SetDCPenColor(o->mem_dc, (o->sb_edit != NULL) ? RGB(130, 170, 235)
                                                      : RGB(60, 80, 118));
        SelectObject(o->mem_dc, GetStockObject(DC_PEN));
        SelectObject(o->mem_dc, GetStockObject(NULL_BRUSH));
        Rectangle(o->mem_dc, fr.left, fr.top, fr.right, fr.bottom);

        /*
         * The text is painted here as well, even while the EDIT is up: the two
         * never show at once (the EDIT sits exactly on top of the box), and
         * keeping the browse state right means nothing needs repainting
         * specially when it closes. The name box is arranged the same way, for
         * the same reason.
         */
        if (o->sb_edit == NULL) {
            RECT t = fr;
            t.left  += 8;
            t.right -= 8;

            if (o->sb_filter_text[0] != L'\0') {
                SetTextColor(o->mem_dc, RGB(225, 235, 255));
                DrawTextW(o->mem_dc, o->sb_filter_text, -1, &t,
                          DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
            } else {
                SetTextColor(o->mem_dc, RGB(120, 135, 165));
                DrawTextW(o->mem_dc, i18n_text(STR_SIDEBAR_SEARCH), -1, &t,
                          DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
            }
        }
    }

    /* ---- the rows ---- */
    if (have_rows && total > 0) {
        for (int i = 0; i < visible; ++i) {
            const int row = sidebar_scroll(&o->sidebar) + i;
            if (row >= total) break;

            RECT rr;
            if (!sidebar_row_rect(&o->sidebar, m, &o->sb_area, row, &rr)) break;

            const BOOL is_hover = (sidebar_hover_row(&o->sidebar) == row);
            const BOOL is_drag  = (sidebar_drag_row(&o->sidebar) == row);

            if (is_hover || is_drag) {
                SetDCBrushColor(o->mem_dc,
                                is_drag ? RGB(46, 76, 124) : RGB(30, 48, 82));
                FillRect(o->mem_dc, &rr, (HBRUSH)GetStockObject(DC_BRUSH));
            }

            const wchar_t *name = NULL;
            const wchar_t *path = NULL;
            {
                /* `row` is a VISIBLE row: with a filter on, the source row
                 * behind it is a different number. */
                const int src = sb_row_at(o, row);
                if (src >= 0) {
                    name = o->sb_src.name_at(o->sb_src.ctx, src);
                    path = o->sb_src.path_at(o->sb_src.ctx, src);
                }
            }
            if (name == NULL) name = L"";
            const BOOL can = (path != NULL && path[0] != L'\0');

            /*
             * The icon, into the space the geometry has been reserving for it
             * since sidebar.h was written (SIDEBAR_ICON). A row that has no
             * icon - a shortcut to a document, a program that has since been
             * uninstalled - simply draws without one: a missing icon is a
             * smaller problem than a list that will not draw.
             */
            if (can && o->icons != NULL) {
                HICON ic = iconcache_get(o->icons, path);
                if (ic != NULL) {
                    (void)DrawIconEx(o->mem_dc,
                                     rr.left + m->pad,
                                     rr.top + ((rr.bottom - rr.top) - m->icon) / 2,
                                     ic, m->icon, m->icon, 0, NULL, DI_NORMAL);
                }
            }

            /* The icon slot is reserved now and painted in the next batch, so
             * that the geometry does not move when the icons arrive. */
            RECT tx = rr;
            tx.left  += m->pad + m->icon + m->pad;
            tx.right -= m->pad;
            if (tx.right <= tx.left) continue;

            const wchar_t *status = i18n_text(can ? STR_APP_NOT_RUNNING
                                                  : STR_APP_UNLAUNCHABLE);
            SIZE st;
            ZeroMemory(&st, sizeof(st));
            GetTextExtentPoint32W(o->mem_dc, status, (int)wcslen(status), &st);

            RECT sr = tx;
            sr.left = tx.right - st.cx;
            SetTextColor(o->mem_dc, RGB(130, 148, 180));
            DrawTextW(o->mem_dc, status, -1, &sr,
                      DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);

            tx.right = sr.left - m->pad;
            if (tx.right <= tx.left) tx.right = tx.left + 1;

            SetTextColor(o->mem_dc, can ? RGB(230, 240, 255) : RGB(150, 162, 185));
            DrawTextW(o->mem_dc, name, -1, &tx,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
        }
    } else if (have_rows) {
        /* Neither a list nor an index in progress: said, not left blank. A
         * blank panel reads as "still loading" for as long as it is looked at. */
        SetTextColor(o->mem_dc, RGB(150, 165, 195));
        RECT er = rows_rect;
        DrawTextW(o->mem_dc, i18n_text(STR_SIDEBAR_EMPTY), -1, &er,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    }

    /* ---- the scrollbar, when there is somewhere to scroll ---- */
    RECT track, thumb;
    if (sidebar_scrollbar_rects(&o->sidebar, m, &o->sb_area, total, visible,
                                &track, &thumb)) {
        SetDCBrushColor(o->mem_dc, RGB(26, 36, 56));
        FillRect(o->mem_dc, &track, (HBRUSH)GetStockObject(DC_BRUSH));

        SetDCBrushColor(o->mem_dc, o->sb_scroll_drag ? RGB(120, 160, 220)
                                                     : RGB(78, 104, 148));
        FillRect(o->mem_dc, &thumb, (HBRUSH)GetStockObject(DC_BRUSH));
    }

    SelectObject(o->mem_dc, old_font);
}

/*
 * What is being carried, and where it would land.
 *
 * The landing rectangle is drawn BEFORE the button comes up, on purpose: a
 * drop that lands somewhere other than where the pointer suggested is a
 * surprise, and showing the rectangle is the cheapest way not to have one.
 */
static void draw_sidebar_drag(Overlay *o)
{
    o->sb_ghost_valid   = FALSE;
    o->sb_preview_valid = FALSE;

    if (!sidebar_enabled(o)) return;
    if (!sidebar_is_dragging(&o->sidebar)) return;

    const SidebarMetrics *m = &o->sb_metrics;
    HFONT old_font = (HFONT)SelectObject(o->mem_dc, o->font_small);
    SetBkMode(o->mem_dc, TRANSPARENT);

    const POINT pt = o->sidebar.drag_pt;
    const SidebarHit hit = sidebar_hit(&o->sidebar, m, &o->sb_area,
                                       sb_visible_count(o), pt);

    /* Over the canvas only: over the panel itself, or the strip, there is
     * nothing to drop onto and promising otherwise would be a lie. */
    if (hit.kind == SIDEBAR_HIT_NONE && pt.y >= TOOLBAR_H) {
        RelRect zone;
        if (sidebar_drop_zone(&o->sb_area, pt, SIDEBAR_DROP_W, SIDEBAR_DROP_H,
                              &zone)) {
            const int w = o->sb_area.right - o->sb_area.left;
            const int h = o->sb_area.bottom - o->sb_area.top;

            RECT zr;
            zr.left   = o->sb_area.left + (LONG)(zone.x * (float)w);
            zr.top    = o->sb_area.top  + (LONG)(zone.y * (float)h);
            zr.right  = zr.left + (LONG)(zone.w * (float)w);
            zr.bottom = zr.top  + (LONG)(zone.h * (float)h);

            SetDCBrushColor(o->mem_dc, RGB(70, 120, 190));
            FillRect(o->mem_dc, &zr, (HBRUSH)GetStockObject(DC_BRUSH));

            SetDCPenColor(o->mem_dc, RGB(255, 235, 150));
            SelectObject(o->mem_dc, GetStockObject(DC_PEN));
            SelectObject(o->mem_dc, GetStockObject(NULL_BRUSH));
            Rectangle(o->mem_dc, zr.left, zr.top, zr.right, zr.bottom);

            o->sb_preview       = zr;
            o->sb_preview_valid = TRUE;
        }
    }

    RECT ghost;
    if (sidebar_drag_ghost(&o->sidebar, m, &ghost)) {
        SetDCBrushColor(o->mem_dc, RGB(58, 92, 148));
        FillRect(o->mem_dc, &ghost, (HBRUSH)GetStockObject(DC_BRUSH));

        SetDCPenColor(o->mem_dc, RGB(190, 215, 255));
        SelectObject(o->mem_dc, GetStockObject(DC_PEN));
        SelectObject(o->mem_dc, GetStockObject(NULL_BRUSH));
        Rectangle(o->mem_dc, ghost.left, ghost.top, ghost.right, ghost.bottom);

        o->sb_ghost       = ghost;
        o->sb_ghost_valid = TRUE;

        const int row = sidebar_drag_row(&o->sidebar);
        if (row >= 0 && row < sb_visible_count(o)) {
            const int src = sb_row_at(o, row);
            const wchar_t *name = (src >= 0)
                                ? o->sb_src.name_at(o->sb_src.ctx, src) : L"";
            RECT t = ghost;
            t.left  += 4;
            t.right -= 4;
            SetTextColor(o->mem_dc, RGB(235, 244, 255));
            DrawTextW(o->mem_dc, name, -1, &t,
                      DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
        }
    }

    SelectObject(o->mem_dc, old_font);
}

/* ------------------------------------------------------------------ */
/* The placeholders                                                    */
/* ------------------------------------------------------------------ */

/*
 * One box per preset this workspace is meant to place, with its icon, its name
 * and its size (acceptance items 8 and 9).
 *
 * The regions and the placeholders answer different questions - "what am I
 * drawing" and "what does this workspace already say" - and they are drawn in
 * that order, so the one being edited is on top.
 *
 * The size line is the interesting one: it shows the PIXEL size the zone works
 * out to, and, when the window has been placed before, the size it was last
 * actually seen at. Those two differ exactly when the window refuses to be the
 * size the preset asks for - the case the user kept reporting, and the reason
 * "set a size, see nothing change" was so confusing.
 */
static void draw_placeholders(Overlay *o)
{
    o->ph_count = 0;
    if (o->slot_src.slot_at == NULL) return;

    const PlaceholderMetrics *pm = &o->ph_metrics;
    const int total = o->slot_src.count;

    HFONT old_font = (HFONT)SelectObject(o->mem_dc, o->font_small);
    SetBkMode(o->mem_dc, TRANSPARENT);

    for (int i = 0; i < total && o->ph_count < PLACEHOLDER_MAX_SHOWN; ++i) {
        const OverlaySlot *s = o->slot_src.slot_at(o->slot_src.ctx, i);
        if (s == NULL) continue;

        RECT box;
        if (!placeholder_rect(&s->zone, &o->sb_area, &box)) continue;

        /* A box being dragged is drawn where the pointer has taken it - the
         * stored zone is only updated when the button comes back up. */
        const BOOL is_drag = (o->ph_drag == i) ? TRUE : FALSE;
        if (is_drag) box = o->ph_now;

        if (!placeholder_is_visible(&box)) continue;

        const BOOL hot = (is_drag || o->ph_hover == i) ? TRUE : FALSE;
        o->ph_boxes[o->ph_count++] = box;

        SetDCBrushColor(o->mem_dc, hot ? RGB(36, 66, 98) : RGB(26, 48, 74));
        FillRect(o->mem_dc, &box, (HBRUSH)GetStockObject(DC_BRUSH));

        SetDCPenColor(o->mem_dc, hot ? RGB(190, 245, 215) : RGB(120, 200, 160));
        SelectObject(o->mem_dc, GetStockObject(DC_PEN));
        SelectObject(o->mem_dc, GetStockObject(NULL_BRUSH));
        Rectangle(o->mem_dc, box.left, box.top, box.right, box.bottom);

        /* The resize grip, shown only for the box the user is pointing at or
         * dragging: eight permanent handles on every box would be a control
         * panel, and this is a layout. */
        RECT grip;
        if (hot && placeholder_grip_rect(pm, &box, &grip)) {
            SetDCBrushColor(o->mem_dc, RGB(190, 245, 215));
            FillRect(o->mem_dc, &grip, (HBRUSH)GetStockObject(DC_BRUSH));
        }

        RECT ir;
        if (s->path[0] != L'\0' && o->icons != NULL &&
            placeholder_icon_rect(pm, &box, &ir)) {
            HICON ic = iconcache_get(o->icons, s->path);
            if (ic != NULL) {
                (void)DrawIconEx(o->mem_dc, ir.left, ir.top, ic,
                                 ir.right - ir.left, ir.bottom - ir.top,
                                 0, NULL, DI_NORMAL);
            }
        }

        RECT nr;
        if (placeholder_name_rect(pm, &box, &nr)) {
            SetTextColor(o->mem_dc, RGB(228, 245, 236));
            DrawTextW(o->mem_dc, s->name, -1, &nr,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
        }

        RECT sr;
        if (placeholder_size_rect(pm, &box, &sr)) {
            /* Taken from the BOX, not from the stored zone: while a drag is in
             * progress this is the only place the new size is visible, and it
             * is the feedback that makes resizing a box mean anything. */
            const int pw = box.right - box.left;
            const int ph = box.bottom - box.top;

            wchar_t txt[96];
            if (s->last_w > 0 && s->last_h > 0) {
                _snwprintf_s(txt, 96, _TRUNCATE,
                             i18n_text(STR_PLACEHOLDER_LAST_FMT),
                             pw, ph, s->last_w, s->last_h);
            } else {
                _snwprintf_s(txt, 96, _TRUNCATE,
                             i18n_text(STR_PLACEHOLDER_SIZE_FMT), pw, ph);
            }

            SetTextColor(o->mem_dc, RGB(168, 205, 190));
            DrawTextW(o->mem_dc, txt, -1, &sr,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
        }
    }

    /*
     * Said ONCE, not silently dropped and not once per frame - the rule this
     * project follows everywhere. On screen there is nowhere left to put the
     * ones that did not fit, so the log is where that answer lives.
     */
    if (o->ph_count < total && !o->ph_short_reported) {
        log_info(L"overlay: %d of %d preset box(es) are not drawn on this screen",
                 total - o->ph_count, total);
        o->ph_short_reported = TRUE;
    }

    /* An empty workspace and a broken one look identical on a blank canvas. */
    if (total == 0) {
        SetTextColor(o->mem_dc, RGB(190, 210, 200));
        RECT t = o->sb_area;
        t.left  += 24;
        t.right -= 24;
        t.top    = TOOLBAR_H + 56;
        t.bottom = t.top + 28;
        DrawTextW(o->mem_dc, i18n_text(STR_PLACEHOLDER_EMPTY), -1, &t,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
    }

    SelectObject(o->mem_dc, old_font);
}

static BOOL register_class(HINSTANCE hinst)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = overlay_proc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursor(NULL, IDC_CROSS);   /* arranging, not typing */
    wc.lpszClassName = WAC_OVERLAY_CLASS;

    if (RegisterClassExW(&wc) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        log_info(L"overlay: RegisterClassExW failed (%lu)", GetLastError());
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* The name box: typing a new name for whatever is being edited        */
/* ------------------------------------------------------------------ */

/*
 * Repaint the strip.
 *
 * The name box, its two buttons and the toolbar buttons all live in the top
 * TOOLBAR_H rows, so a single rectangle covers every change made up there.
 */
static void name_box_repaint(Overlay *o)
{
    RECT bar;

    if (o == NULL || o->hwnd == NULL) return;

    bar.left   = 0;
    bar.top    = 0;
    bar.right  = o->rect.right - o->rect.left;
    bar.bottom = TOOLBAR_H;
    InvalidateRect(o->hwnd, &bar, FALSE);
}

/*
 * The ONLY way the box's mode changes.
 *
 * State and repaint move together here, so "the state changed but nothing
 * repainted" cannot happen by omission: there is no other way to set the flag.
 *
 * MEASURED 2026-09-23, reported from a screenshot: entering typing mode set the
 * flag without invalidating, so [save] and [cancel] appeared only after the
 * user clicked somewhere else - which reads as "the buttons are slow" when only
 * the painting was missing. The geometry test could not see it; a person
 * looking at the screen could.
 */
static void name_box_set_editing(Overlay *o, BOOL editing)
{
    if (o == NULL || o->name_editing == editing) return;
    o->name_editing = editing;
    name_box_repaint(o);
}

/*
 * Close the EDIT window, if one is open, and put the keyboard back on the
 * overlay.
 *
 * This is the ONE place that window is destroyed. Commit, cancel, Esc and the
 * end of the session all come through here - which is what makes the resource
 * discipline checkable: if a name box ever survived a session, t51 says so.
 */
static void name_edit_close(Overlay *o)
{
    HWND ed;

    if (o == NULL || o->name_edit == NULL) return;

    ed = o->name_edit;
    o->name_edit  = NULL;   /* before DestroyWindow: no re-entry */
    o->name_hover = -1;

    /* Back to browse mode, and repainted, in one step. */
    name_box_set_editing(o, FALSE);

    /*
     * The overlay owns Esc for the session and cannot see the key while the
     * EDIT window has the keyboard, so hand it back before the window goes.
     */
    if (o->hwnd != NULL) {
        (void)SetFocus(o->hwnd);
    }
    DestroyWindow(ed);
}

/*
 * Take what the user typed.
 *
 * An empty box counts as "no change" rather than as a rename to nothing: the
 * design says abandoning the edit discards it, and an empty name is closer to
 * abandoning than to naming.
 */
static void name_edit_commit(Overlay *o)
{
    wchar_t buf[OVERLAY_NAME_MAX];

    if (o == NULL || o->name_edit == NULL) return;

    buf[0] = L'\0';
    GetWindowTextW(o->name_edit, buf, OVERLAY_NAME_MAX);

    /* Repaints on its way out - but with the OLD name still in place. */
    name_edit_close(o);

    if (buf[0] == L'\0' || wcscmp(buf, o->name) == 0) {
        return;   /* nothing changed, and the close already repainted */
    }

    wcsncpy_s(o->name, OVERLAY_NAME_MAX, buf, _TRUNCATE);
    log_info(L"overlay: name set to \"%ls\"", o->name);

    /*
     * The overlay stores nothing: it reports the new name and the caller
     * decides what it means - the same division the exit callback follows.
     * Called last, so a callback that tears the session down finds an overlay
     * that is already back in its browse state.
     */
    if (o->on_rename != NULL) {
        o->on_rename(o->name, o->rename_ctx);
    }

    /* Now that the name itself changed, paint it. */
    name_box_repaint(o);
}

/* Abandon the edit. The design is explicit: what was typed is discarded. */
static void name_edit_cancel(Overlay *o)
{
    if (o == NULL || o->name_edit == NULL) return;

    log_info(L"overlay: name edit cancelled, keeping \"%ls\"", o->name);

    /* Closing repaints, and puts the box back into browse mode. */
    name_edit_close(o);
}

/*
 * The EDIT window's procedure.
 *
 * Subclassed for two keys: Enter commits and Esc abandons. Without this the
 * only way out of the box would be clicking a button - and Esc, which the user
 * has just learned means "get me out of here", would do nothing at all, because
 * the overlay that owns it no longer has the keyboard.
 *
 * Subclassing a window this process created itself is ordinary. The rule that
 * forbids subclassing is about OTHER processes' windows.
 */
static LRESULT CALLBACK name_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Overlay *o = (Overlay *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (msg == WM_KEYDOWN && o != NULL) {
        if (wp == VK_RETURN) { name_edit_commit(o); return 0; }
        if (wp == VK_ESCAPE) { name_edit_cancel(o); return 0; }
    }

    if (o != NULL && o->name_edit_old != NULL) {
        return CallWindowProcW(o->name_edit_old, hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/*
 * Open the box for typing.
 *
 * A REAL EDIT window, not text painted into the overlay.
 *
 * Two reasons, and the second is the deciding one. A layered window is
 * composited as a single unit, so a child control inside the overlay would not
 * be drawn at all. And text the overlay paints itself never enters the IME
 * composition, so a Chinese name could not be typed into it - MEASURED as the
 * constraint on 2026-09-22.
 *
 * It is placed exactly over the drawn box, so the two agree pixel for pixel.
 */
static BOOL name_edit_begin(Overlay *o)
{
    const RECT *r;

    if (o == NULL || !o->name_box.valid) return FALSE;
    if (o->name_edit != NULL) return TRUE;      /* already typing */

    r = &o->name_box.name;

    o->name_edit = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"EDIT", o->name,
        WS_POPUP | WS_VISIBLE | ES_AUTOHSCROLL,
        o->rect.left + r->left, o->rect.top + r->top,
        r->right - r->left, r->bottom - r->top,
        o->hwnd, NULL, o->hinst, NULL);

    if (o->name_edit == NULL) {
        log_info(L"overlay: could not create the name box EDIT window");
        return FALSE;
    }

    SendMessageW(o->name_edit, WM_SETFONT, (WPARAM)o->font_small, TRUE);
    SendMessageW(o->name_edit, EM_SETSEL, 0, -1);   /* typing replaces it all */

    SetWindowLongPtrW(o->name_edit, GWLP_USERDATA, (LONG_PTR)o);
    o->name_edit_old = (WNDPROC)SetWindowLongPtrW(o->name_edit, GWLP_WNDPROC,
                                                  (LONG_PTR)name_edit_proc);

    o->name_hover = -1;

    /*
     * Typing mode, and the repaint that puts [save] and [cancel] on screen, in
     * one step - see name_box_set_editing(). Doing it here by hand is what was
     * missed the first time round.
     */
    name_box_set_editing(o, TRUE);

    (void)SetFocus(o->name_edit);

    log_info(L"overlay: name box opened for \"%ls\"", o->name);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* The search box's EDIT window                                        */
/* ------------------------------------------------------------------ */

/*
 * Close it, if it is open, and put the keyboard back on the overlay.
 *
 * The ONE place this window is destroyed: Enter, Esc, a click elsewhere, the
 * end of the session and overlay_destroy all come through here. That is the
 * same discipline the name box follows, and it is what lets t51_release prove
 * nothing survives a session.
 */
static void filter_edit_close(Overlay *o)
{
    HWND ed;

    if (o == NULL || o->sb_edit == NULL) return;

    ed = o->sb_edit;
    o->sb_edit = NULL;          /* before DestroyWindow: no re-entry */

    /*
     * The overlay owns Esc for the whole session and cannot see the key while
     * the EDIT has the keyboard, so hand it back before the window goes.
     */
    if (o->hwnd != NULL) {
        (void)SetFocus(o->hwnd);
    }
    DestroyWindow(ed);

    /* The drawn box shows the text from now on, and the border goes back to
     * its resting colour. */
    sidebar_invalidate(o);
}

/* Rebuild the visible rows from what is in the box right now. */
static void filter_edit_changed(Overlay *o)
{
    wchar_t buf[OVERLAY_FILTER_MAX];

    if (o == NULL || o->sb_edit == NULL) return;

    buf[0] = L'\0';
    GetWindowTextW(o->sb_edit, buf, OVERLAY_FILTER_MAX);

    if (wcscmp(buf, o->sb_filter_text) == 0) return;

    wcsncpy_s(o->sb_filter_text, OVERLAY_FILTER_MAX, buf, _TRUNCATE);
    sb_apply_filter(o);
    sidebar_invalidate(o);
}

/*
 * The EDIT's procedure.
 *
 * Enter keeps the result. Esc clears the text first and closes the box only
 * when it is already empty - "undo what I typed" is what Esc means in every
 * search field on the desktop, and closing on the first Esc would throw the
 * search away instead.
 *
 * The typing itself is NOT handled here: EN_CHANGE is delivered to the parent
 * (see the WM_COMMAND case in overlay_proc), so a subclass of the control
 * never sees it.
 */
static LRESULT CALLBACK filter_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Overlay *o = (Overlay *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (o != NULL && o->sb_edit == hwnd && msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            filter_edit_close(o);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            if (o->sb_filter_text[0] != L'\0') {
                SetWindowTextW(o->sb_edit, L"");
                filter_edit_changed(o);
                return 0;
            }
            filter_edit_close(o);
            return 0;
        }
    }

    if (o != NULL && o->sb_edit_old != NULL) {
        return CallWindowProcW(o->sb_edit_old, hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/*
 * Open the search box for typing.
 *
 * A REAL EDIT window, placed exactly over the drawn box - the same answer the
 * name box arrived at, for the same two reasons: the overlay is layered, so a
 * child control inside it is not painted at all; and text the overlay paints
 * itself never enters the IME composition, so a Chinese program name could not
 * be searched for.
 */
static BOOL filter_edit_begin(Overlay *o)
{
    RECT fr;

    if (o == NULL) return FALSE;
    if (o->sb_edit != NULL) return TRUE;        /* already typing */
    if (!sidebar_filter_rect(&o->sidebar, &o->sb_metrics, &o->sb_area, &fr)) {
        return FALSE;
    }

    o->sb_edit = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"EDIT", o->sb_filter_text,
        WS_POPUP | WS_VISIBLE | ES_AUTOHSCROLL,
        o->rect.left + fr.left, o->rect.top + fr.top,
        fr.right - fr.left, fr.bottom - fr.top,
        o->hwnd, NULL, o->hinst, NULL);

    if (o->sb_edit == NULL) {
        log_info(L"overlay: could not create the search box's EDIT window");
        return FALSE;
    }

    SendMessageW(o->sb_edit, WM_SETFONT, (WPARAM)o->font_small, TRUE);

    /*
     * The placeholder is the EDIT's own (EM_SETCUEBANNER, comctl32 6): the
     * overlay cannot paint behind a separate top-level window sitting in front
     * of it. Spelled out as a number rather than including commctrl.h for one
     * constant.
     */
    SendMessageW(o->sb_edit, 0x1501 /* EM_SETCUEBANNER */, TRUE,
                 (LPARAM)i18n_text(STR_SIDEBAR_SEARCH));

    SendMessageW(o->sb_edit, EM_SETSEL, 0, -1);     /* typing replaces it all */

    SetWindowLongPtrW(o->sb_edit, GWLP_USERDATA, (LONG_PTR)o);
    o->sb_edit_old = (WNDPROC)SetWindowLongPtrW(o->sb_edit, GWLP_WNDPROC,
                                                (LONG_PTR)filter_edit_proc);

    (void)SetFocus(o->sb_edit);
    sidebar_invalidate(o);
    return TRUE;
}

Overlay *overlay_create(HINSTANCE hinst, int monitor_index,
                        OverlayExitFn on_exit, void *ctx)
{
    if (hinst == NULL || monitor_index < 0) {
        return NULL;
    }
    if (!register_class(hinst)) {
        return NULL;
    }

    MonitorFingerprint mons[WAC_MON_MAX];
    const int n = monitor_enumerate(mons, WAC_MON_MAX);
    if (monitor_index >= n) {
        log_info(L"overlay: monitor %d does not exist (%d present)",
                 monitor_index, n);
        return NULL;
    }

    RECT mon;
    ZeroMemory(&mon, sizeof(mon));
    if (!monitor_rect(mons, n, monitor_index, &mon)) {
        log_info(L"overlay: monitor %d has no rectangle", monitor_index);
        return NULL;
    }

    Overlay *o = (Overlay *)calloc(1, sizeof(*o));
    if (o == NULL) {
        return NULL;
    }
    o->hinst         = hinst;
    o->monitor_index = monitor_index;
    o->rect          = mon;
    o->on_exit       = on_exit;
    o->ctx           = ctx;
    o->reason        = OVERLAY_EXIT_NONE;

    /*
     * WS_EX_NOACTIVATE is deliberately absent, and its absence is a fix.
     *
     * MEASURED 2026-09-22, reported by the user: Esc and Ctrl+Q did nothing
     * while the overlay was up and only the fuse could end the session.
     * WS_EX_NOACTIVATE does not merely decline to ask for focus - it makes the
     * window impossible to activate at all, whether by SetForegroundWindow or
     * by clicking on it. Nothing the user could do would have given this window
     * the keyboard.
     *
     * The comment in overlay.h used to claim SetForegroundWindow makes Esc work
     * despite WS_EX_NOACTIVATE. That was wrong, and this is the correction: a
     * mode the user opened on purpose needs the keyboard, and it has to be able
     * to take it. WS_EX_TOOLWINDOW stays, so it never appears in Alt+Tab.
     */
    o->hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        WAC_OVERLAY_CLASS, L"WAC edit overlay", WS_POPUP,
        mon.left, mon.top, mon.right - mon.left, mon.bottom - mon.top,
        NULL, NULL, hinst, o);
    if (o->hwnd == NULL) {
        log_info(L"overlay: CreateWindowExW failed (%lu)", GetLastError());
        free(o);
        return NULL;
    }

    /*
     * No SetLayeredWindowAttributes call here.
     *
     * MEASURED 2026-09-22, from the user's screenshot: it blends the whole
     * window at one ratio, which made the toolbar captions exactly as faint as
     * the background wash - 59% - and unreadable against a dark wallpaper. The
     * window stays WS_EX_LAYERED, because that is what UpdateLayeredWindow
     * requires, but the blending is now decided per pixel from the DIB: the
     * wash is faint while the controls are solid.
     *
     * The consequence is that the window has no defined appearance until the
     * first paint pushes a frame, which is why overlay_show() forces one.
     */
    if (o->hwnd == NULL) {
        free(o);
        return NULL;
    }

    /*
     * The dpi FIRST - before the strip, the buttons, the name box or the
     * monitor box are laid out, and before any font exists. See
     * apply_overlay_dpi() for what went wrong when this came second.
     */
    apply_overlay_dpi(o);

    /*
     * The sidebar's state and the area it slides over.
     *
     * sidebar_init() is not optional even though the structure was calloc'd: a
     * zeroed SidebarState is CLOSED, which is what we want, but 0 is also a
     * legal ROW number - so the hover and press fields have to be told "none"
     * explicitly or the first row starts out highlighted.
     */
    sidebar_init(&o->sidebar);
    sidebar_layout(o);

    /*
     * "No box" has to be SAID, not left at zero.
     *
     * MEASURED 2026-10-11, the first run of probe_edit_apply.ps1: the state is
     * calloc'd, so ph_drag started as 0 - and 0 is a perfectly good box index.
     * The first box was therefore drawn where "the drag has taken it", which is
     * a rectangle that has never been set, and it vanished from the canvas
     * while the second one drew normally. Same trap popup.c and sidebar_init()
     * each hit once: a zeroed struct is a valid state only if zero means
     * "nothing".
     */
    o->ph_hover = -1;
    o->ph_drag  = -1;

    /*
     * The icon cache, at the size the sidebar draws icons at on this screen.
     *
     * A failure here is NOT fatal: iconcache_get() answers NULL and the rows
     * draw without icons, which is a worse-looking list rather than a broken
     * one. Said out loud so that "the icons are missing" has a cause in the
     * log rather than being a mystery.
     */
    o->icons = iconcache_create(ICONCACHE_MAX, o->sb_metrics.icon,
                                iconcache_shell_load, iconcache_shell_free, NULL);
    if (o->icons == NULL) {
        log_info(L"overlay: no icon cache - the rows will draw without icons");
    }

    /*
     * Lay the toolbar out exactly once. The strip is the top TOOLBAR_H pixels
     * of the overlay in client coordinates, and the rectangles computed here
     * are the ones both the painter and the mouse use.
     */
    RECT strip;
    strip.left   = 0;
    strip.top    = 0;
    strip.right  = mon.right - mon.left;
    strip.bottom = TOOLBAR_H;
    o->item_count = toolbar_layout(&strip, TOOLBAR_ENABLED_MASK,
                                   o->items, TB_COUNT);
    if (o->item_count <= 0) {
        /* Reported, not worked around: a monitor too narrow for the toolbar
         * gets a screen with no buttons rather than overlapping ones. */
        log_info(L"overlay: the toolbar does not fit in %ld px",
                 (long)(mon.right - mon.left));
    } else if (!toolbar_name_layout(&strip, o->items, o->item_count,
                                    &o->name_box)) {
        /* The box is left out when it would run into the buttons. Reported for
         * the same reason the toolbar's own failure is: a screen missing
         * something should say so in the log rather than just look wrong. */
        log_info(L"overlay: no room for the name box beside the toolbar");
    }
    /* calloc leaves these at 0, which is a real button; TB_COUNT means none. */
    o->hover   = TB_COUNT;
    o->pressed = TB_COUNT;
    /* The same trap once more: 0 is [save] in the box's pair, so "neither"
     * has to be set explicitly or the save button starts out highlighted. */
    o->name_hover = -1;

    /* And once more: the opening text is UP until its timer fires, and calloc
     * would leave this at 0 - which reads as "never show it at all". */
    o->intro = TRUE;

    /* Start on an empty canvas. A template is something the user picks, and
     * defaulting to one would put regions on screen nobody asked for. */
    o->tpl          = SNAP_CUSTOM;
    o->region_count = 0;
    o->drag_active  = FALSE;
    snapgrid_collect_lines(o->regions, o->region_count,
                           (int)(mon.right - mon.left),
                           (int)(mon.bottom - mon.top),
                           &o->lines);

    /* The monitor box needs the toolbar and the name box to know what room is
     * left in the middle of the strip, so it comes after both of them. */
    refresh_monitor_box(o);

    build_resources(o);
    if (o->mem_dc == NULL || o->mem_bmp == NULL) {
        /* Without a back buffer every paint would flicker. Refuse rather than
         * show a broken editor. */
        log_info(L"overlay: no back buffer available");
        release_resources(o);
        DestroyWindow(o->hwnd);
        free(o);
        return NULL;
    }

    /*
     * The opening text's single shot, started last so that a failure anywhere
     * above can never leave a timer pointing at a half-built overlay.
     */
    (void)SetTimer(o->hwnd, OVERLAY_INTRO_TIMER_ID, OVERLAY_INTRO_MS, NULL);

    g_created++;
    log_info(L"overlay: created on monitor %d (%ld,%ld %ldx%ld)",
             monitor_index, (long)mon.left, (long)mon.top,
             (long)(mon.right - mon.left), (long)(mon.bottom - mon.top));
    return o;
}

/*
 * Move a running overlay to another monitor (2026-10-09, requested by the user).
 *
 * WHY TWO SetWindowPos CALLS, IN THIS ORDER
 *
 *   Moving a window onto a monitor with a different DPI makes Windows rescale
 *   it (WM_DPICHANGED): the size lands as request x (new DPI / old DPI), and
 *   the position is not affected. MEASURED 2026-10-08 on the 100% + 150% pair:
 *   asking for 600x400 gave 900x600, and asking again gave exactly 600x400.
 *
 *   win_place.c absorbs that with a second size request, and this does the same
 *   thing for the same reason: move first - the rescale happens here and its
 *   result is overwritten by the next call - then size. Without it the overlay
 *   would cover three quarters of the new screen on a mixed-DPI desk, with
 *   nothing in the log to explain it.
 *
 * THE BACK BUFFER IS REBUILT, NOT STRETCHED
 *
 *   It is a DIB section whose dimensions are the screen's, and rebuilding also
 *   keeps the per-pixel alpha exact - a stretched buffer would blur the 1:1
 *   pixel relationship the alpha pass depends on. release_resources() and
 *   build_resources() are the existing pair for that, and t51_release is the
 *   gate that keeps them balanced.
 *
 * WHAT SURVIVES AND WHAT IS RECOMPUTED
 *
 *   The regions are NORMALIZED, so they mean the same thing on the new screen
 *   and are kept as they are. What changes is their projection into pixels, so
 *   the snap lines are collected again - and the toolbar is laid out again,
 *   because the new screen may be a different width.
 */
BOOL overlay_move_to_monitor(Overlay *o, int monitor_index)
{
    if (o == NULL || o->hwnd == NULL || monitor_index < 0) return FALSE;

    MonitorFingerprint mons[WAC_MON_MAX];
    const int n = monitor_enumerate(mons, WAC_MON_MAX);
    if (monitor_index >= n) return FALSE;

    const int x = mons[monitor_index].virtual_x;
    const int y = mons[monitor_index].virtual_y;
    const int w = mons[monitor_index].width;
    const int h = mons[monitor_index].height;
    if (w <= 0 || h <= 0) return FALSE;

    /* Already there: no window calls, no repaint. */
    if (x == (int)o->rect.left && y == (int)o->rect.top &&
        w == (int)(o->rect.right - o->rect.left) &&
        h == (int)(o->rect.bottom - o->rect.top)) {
        return TRUE;
    }

    /*
     * The name box's EDIT is a separate top-level window positioned against the
     * old geometry. Close it rather than leave it floating over the screen the
     * user just left - and do it BEFORE the geometry changes, so it cannot be
     * re-placed against a rectangle that is about to be wrong.
     */
    name_edit_close(o);

    /* Step 1: move only. The DPI change happens here. */
    SetWindowPos(o->hwnd, HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);

    /* Step 2: size it, now that the window is already on the target monitor. */
    SetWindowPos(o->hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);

    o->rect.left     = x;
    o->rect.top      = y;
    o->rect.right    = x + w;
    o->rect.bottom   = y + h;
    o->monitor_index = monitor_index;

    /* A pop-up panel is positioned against the old screen; it goes with it. */
    popup_close(&o->popup);
    o->panel_hover = -1;

    /*
     * The monitor changed, so the dpi may have too. Set it before the
     * resources (fonts) and the geometry below, exactly as overlay_create()
     * does - this is the cross-dpi move that task 7.2's migration path takes.
     */
    apply_overlay_dpi(o);
    sidebar_layout(o);

    /*
     * Every icon in the cache was rasterized at the OLD screen's size, so they
     * all go back. Not rebuilt here: the next paint asks for the ones it needs,
     * and asking for forty is what the cache is for.
     */
    iconcache_clear(o->icons);

    /*
     * Park the sidebar. Its geometry moved with the screen, and the pointer is
     * still on the old one - a panel left half out with its strip on a monitor
     * nobody is looking at is worse than a panel that is simply away. The
     * SCROLL POSITION is kept: it is about the list, not about the screen.
     */
    {
        const unsigned long long now = GetTickCount64();
        sidebar_set_open(&o->sidebar, FALSE, now);
        (void)sidebar_advance(&o->sidebar, now + SIDEBAR_SLIDE_MS);
        sidebar_timer_sync(o);
    }

    /* The back buffer is the size of the screen, so it is rebuilt - not
     * stretched - against the rectangle the window has just been given. */
    release_resources(o);
    build_resources(o);

    /*
     * Toolbar geometry depends on the screen width and the name box on the
     * toolbar, so both are computed again - in the same order overlay_create()
     * uses, because that order is what the two of them assume.
     */
    RECT strip;
    strip.left   = 0;
    strip.top    = 0;
    strip.right  = w;
    strip.bottom = TOOLBAR_H;

    o->item_count = toolbar_layout(&strip, TOOLBAR_ENABLED_MASK, o->items, TB_COUNT);
    if (o->item_count > 0) {
        (void)toolbar_name_layout(&strip, o->items, o->item_count, &o->name_box);
    } else {
        /* A screen too narrow for the buttons: no box either. Reported by
         * toolbar_layout()'s own caller contract, not worked around here. */
        o->name_box.valid = FALSE;
    }

    /* Hover and press belonged to the old layout; a stale id would highlight
     * the wrong button on the new one. */
    o->hover       = TB_COUNT;
    o->pressed     = TB_COUNT;
    o->name_hover  = -1;
    o->drag_active = FALSE;

    /* The regions are normalized and survive the move; their projection does
     * not, so a template is rebuilt for the new panel. A hand-drawn layout
     * keeps its regions exactly as they were - they are the user's work. */
    if (o->tpl != SNAP_CUSTOM) {
        o->region_count = snapgrid_build(o->tpl, SNAP_GAP_DEFAULT_PX, w, h,
                                         o->regions, SNAP_MAX_REGIONS);
    }
    snapgrid_collect_lines(o->regions, o->region_count, w, h, &o->lines);

    /* The box now names a different screen, and the new one may be a different
     * width - so its geometry is recomputed as well. */
    refresh_monitor_box(o);

    InvalidateRect(o->hwnd, NULL, FALSE);
    render(o);

    log_info(L"overlay: moved to monitor %d (%d,%d %dx%d)",
             monitor_index, x, y, w, h);
    return TRUE;
}

BOOL overlay_show(Overlay *o)
{
    if (o == NULL || o->hwnd == NULL) {
        return FALSE;
    }

    /*
     * Remember who had the keyboard, then take it. See overlay.h for why this
     * overlay takes focus while tests/t05_topmost.c insists its own must not.
     */
    o->prev_foreground = GetForegroundWindow();

    /* SW_SHOW, not SW_SHOWNOACTIVATE: this window has to be able to become
     * active, or it can never receive a key. See overlay_create(). */
    ShowWindow(o->hwnd, SW_SHOW);
    SetWindowPos(o->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

    /*
     * Take the keyboard, with the thread-input attach Windows requires.
     *
     * MEASURED 2026-09-22, reported by the user: Esc and Ctrl+Q did nothing
     * while the overlay was up. Two causes, both fixed here and in
     * overlay_create(): WS_EX_NOACTIVATE made the window impossible to
     * activate at all, and SetForegroundWindow is refused outright when the
     * calling process is not already the foreground process - which is exactly
     * this case, since the user was working in another program when they asked
     * for edit mode.
     *
     * AttachThreadInput borrows the foreground thread's input state for the
     * duration of the call, making the request legitimate. It is the standard
     * technique, and unlike a keyboard hook it costs nothing and watches
     * nothing - WH_KEYBOARD_LL is ruled out outright.
     *
     * Failing is survivable now that WS_EX_NOACTIVATE is gone: a click on the
     * overlay also activates it. It is still recorded, because "the exit key
     * did not work" is the one silence this mode must never have.
     */
    const HWND  fg     = GetForegroundWindow();
    const DWORD my_tid = GetCurrentThreadId();
    const DWORD fg_tid = (fg != NULL) ? GetWindowThreadProcessId(fg, NULL) : 0;

    BOOL attached = FALSE;
    if (fg_tid != 0 && fg_tid != my_tid) {
        attached = AttachThreadInput(fg_tid, my_tid, TRUE);
    }

    (void)SetForegroundWindow(o->hwnd);
    (void)SetActiveWindow(o->hwnd);
    (void)SetFocus(o->hwnd);

    if (attached) {
        (void)AttachThreadInput(fg_tid, my_tid, FALSE);
    }

    if (GetForegroundWindow() != o->hwnd) {
        log_info(L"overlay: could not take the foreground - a click on the "
                 L"overlay will be needed before Esc works");
    }

    /*
     * Render directly instead of hoping for a WM_PAINT.
     *
     * MEASURED 2026-09-22, from a screenshot: after moving to per-pixel alpha
     * the overlay came up completely invisible. ShowWindow does not reliably
     * mark a layered window invalid, so UpdateWindow had no invalid region to
     * act on and no frame was ever pushed - and until one is, the window has
     * no appearance at all, because SetLayeredWindowAttributes is gone.
     *
     * Calling render() here removes the dependency on the message system
     * entirely: the first frame exists before this function returns.
     */
    InvalidateRect(o->hwnd, NULL, TRUE);
    UpdateWindow(o->hwnd);
    render(o);

    /*
     * The Z-order timer is deliberately NOT started here yet.
     *
     * MEASURED 2026-09-22: adding SetTimer(hwnd, ...) to this function took the
     * t51 gate from PASS (9 GDI / 1 USER after fifty edit sessions) to FAIL
     * (81 GDI / 42 USER). Killing the timer explicitly in overlay_destroy() did
     * not move those two numbers at all, so the cost is not the timer
     * surviving - it is this window owning one, and the exact mechanism is not
     * yet understood.
     *
     * What IS understood: the SetWindowPos itself is cheap. On a 32-thread
     * machine, re-asserting HWND_TOPMOST every two seconds costs 15.6 ms of CPU
     * per ten seconds, 0.16% of one core, and every one second costs three
     * times that. The rate was never the problem.
     *
     * So task 6.6 is NOT done, and this is where it stops until the mechanism
     * is found. The WM_TIMER case below remains and simply never fires.
     */
    (void)OVERLAY_TOPMOST_MS;

    log_info(L"overlay: shown (previous foreground 0x%p)",
             (void *)o->prev_foreground);
    return TRUE;
}

void overlay_request_exit(Overlay *o, OverlayExit why)
{
    if (o == NULL || o->hwnd == NULL) {
        return;
    }
    report_exit(o, why);
}

void overlay_destroy(Overlay *o)
{
    if (o == NULL || o->destroying) {
        return;
    }
    o->destroying = TRUE;

    /*
     * The name box's EDIT window goes first, and explicitly.
     *
     * DestroyWindow on the overlay would take it down as a side effect of
     * ownership - and a resource that dies as a side effect is one the leak gate
     * cannot attribute to anything. This is the single release point the design
     * asks for, in reverse order of creation.
     */
    name_edit_close(o);

    if (o->hwnd != NULL) {
        /*
         * Kill the Z-order timer explicitly, before the window goes.
         *
         * DestroyWindow is documented as cleaning up a window's timers, and
         * MEASURED 2026-09-22 it did not: adding this timer took the t51 gate
         * from PASS to FAIL, with 81 GDI and 42 USER objects left after fifty
         * edit sessions instead of 9 and 1. Killing the timer by hand puts the
         * numbers back, and the gate is what noticed - which is the whole
         * reason it exists.
         */
        (void)KillTimer(o->hwnd, OVERLAY_TOPMOST_TIMER_ID);

        /*
         * Same for the sidebar's slide timer, and for the same measured
         * reason: DestroyWindow does not reliably clean up a window's timers,
         * and this one is started and stopped while the user works - so an
         * edit session that ends mid-slide would leave it behind.
         */
        (void)KillTimer(o->hwnd, OVERLAY_SIDEBAR_TIMER_ID);
        o->sb_timer = FALSE;

        /*
         * And the opening text's one shot. It normally kills itself in
         * WM_TIMER after five seconds, so this is for the case where the user
         * leaves edit mode inside those five seconds - which, given that this
         * is a mode people open and immediately abandon, is not a rare path.
         */
        (void)KillTimer(o->hwnd, OVERLAY_INTRO_TIMER_ID);
        o->intro = FALSE;

        DestroyWindow(o->hwnd);
        o->hwnd = NULL;
    }

    /*
     * The icon handles go back before the GDI objects do. They are USER
     * objects rather than GDI ones, so release_resources() does not count
     * them - but they are this module's promise all the same, and one left
     * behind is one per edit session (tests/t51_release.c measures exactly
     * that, over fifty of them).
     */
    iconcache_destroy(o->icons);
    o->icons = NULL;

    /*
     * The search box's EDIT goes with it - through its one close function, so
     * the keyboard is handed back before the window it belongs to disappears.
     * The index array the filter builds is this module's memory, freed here.
     */
    filter_edit_close(o);
    if (o->sb_filter != NULL) {
        free(o->sb_filter);
        o->sb_filter = NULL;
        o->sb_filter_count = 0;
    }

    release_resources(o);

    /*
     * Hand the keyboard back. Only if the window we took it from is still
     * there - it may have been the very window the user was arranging, and it
     * may be gone by now.
     */
    if (o->prev_foreground != NULL && IsWindow(o->prev_foreground)) {
        (void)SetForegroundWindow(o->prev_foreground);
    }

    g_destroyed++;
    log_info(L"overlay: destroyed (exit=%ls, gdi now %d)",
             overlay_exit_name(o->reason), g_gdi_count);
    free(o);
}

int overlay_monitor_index(const Overlay *o)
{
    return (o != NULL) ? o->monitor_index : -1;
}

void overlay_set_name(Overlay *o, const wchar_t *name,
                      OverlayRenameFn on_rename, void *ctx)
{
    if (o == NULL) return;

    o->on_rename  = on_rename;
    o->rename_ctx = ctx;

    o->name[0] = L'\0';
    if (name != NULL) {
        wcsncpy_s(o->name, OVERLAY_NAME_MAX, name, _TRUNCATE);
    }

    if (o->hwnd != NULL) InvalidateRect(o->hwnd, NULL, FALSE);
}

const wchar_t *overlay_name(const Overlay *o)
{
    return (o != NULL) ? o->name : L"";
}

void overlay_set_sidebar(Overlay *o, const OverlaySidebarSource *src,
                         OverlayDropFn on_drop, void *drop_ctx)
{
    if (o == NULL) return;

    /*
     * The source is COPIED: the caller's structure is usually a local built
     * for this call, and the overlay reads the callbacks on every frame it
     * draws. What must outlive the call is the callbacks' own state, and that
     * is the caller's business - the same division overlay_set_name() uses.
     */
    if (src == NULL || src->name_at == NULL) {
        ZeroMemory(&o->sb_src, sizeof(o->sb_src));
        o->sb_drop     = NULL;
        o->sb_drop_ctx = NULL;

        /* No list: no panel either. Leaving one open over a canvas with
         * nothing to put on it would be a dead end. */
        sidebar_drag_end(&o->sidebar);
        sidebar_set_open(&o->sidebar, FALSE, GetTickCount64());
        (void)sidebar_advance(&o->sidebar, GetTickCount64() + SIDEBAR_SLIDE_MS);
        sidebar_timer_sync(o);
    } else {
        o->sb_src      = *src;
        o->sb_drop     = on_drop;
        o->sb_drop_ctx = drop_ctx;
    }

    /*
     * A list that arrived (or left) changes how far the panel may scroll. A
     * list that SHRANK must not leave the panel scrolled past its end, where
     * it would show blank rows under the last item and read as "the list
     * ended early".
     */
    /* A list that arrived (or left) changes how far the panel may scroll, and
     * the filter has to be applied to it - the new rows may match, or may not. */
    sb_apply_filter(o);
    sidebar_scroll_to(&o->sidebar, sidebar_scroll(&o->sidebar), sb_visible_count(o),
                      sidebar_visible_rows(&o->sb_metrics, &o->sb_area));

    if (o->hwnd != NULL) InvalidateRect(o->hwnd, NULL, FALSE);
}

void overlay_set_slots(Overlay *o, const OverlaySlotSource *src)
{
    if (o == NULL) return;

    /* Copied, like the sidebar's source: the caller's structure is usually a
     * local built for the call, and the overlay reads the callback on every
     * frame. What must outlive the call is the callback's own state. */
    if (src == NULL || src->slot_at == NULL) {
        ZeroMemory(&o->slot_src, sizeof(o->slot_src));
    } else {
        o->slot_src = *src;
    }

    /* The list just changed, so an index held from the previous one may point
     * at a different box. Let go of it rather than editing the wrong preset. */
    o->ph_drag  = -1;
    o->ph_hover = -1;
    o->ph_count = 0;
    o->ph_short_reported = FALSE;

    if (o->hwnd != NULL) InvalidateRect(o->hwnd, NULL, FALSE);
}

BOOL overlay_rect(const Overlay *o, RECT *out)
{
    if (o == NULL || out == NULL) {
        return FALSE;
    }
    *out = o->rect;
    return TRUE;
}

HWND overlay_hwnd(const Overlay *o)
{
    return (o != NULL) ? o->hwnd : NULL;
}

OverlayExit overlay_exit_reason(const Overlay *o)
{
    return (o != NULL) ? o->reason : OVERLAY_EXIT_NONE;
}

BOOL overlay_reassert_topmost(Overlay *o)
{
    if (o == NULL || o->hwnd == NULL) {
        return FALSE;
    }

    /* Cheap question first: is anything actually above us? Re-raising without
     * asking would move the window every time and count as "contested" every
     * time, which would make the number meaningless. */
    if (GetForegroundWindow() == o->hwnd) {
        return FALSE;
    }

    const HWND above = GetWindow(o->hwnd, GW_HWNDPREV);
    if (above == NULL) {
        return FALSE;       /* already the topmost window */
    }

    SetWindowPos(o->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return TRUE;
}
