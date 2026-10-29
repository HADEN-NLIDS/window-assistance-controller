/*
 * frame.h - the window frame we draw ourselves.
 *
 * WHY NOT THE SYSTEM TITLE BAR
 *
 *   The system caption cannot be themed: its colours come from the user's
 *   Windows theme, its height is fixed, and it cannot host the layout this
 *   interface needs (a navigation strip that is part of the window, not a
 *   menu bar). Drawing the caption is also the prerequisite for the rounded
 *   corners and themed colours planned for the polish pass - those cannot be
 *   applied to a caption Windows owns.
 *
 * WHAT IT COSTS
 *
 *   Everything the system did for free now has to be done by hand: dragging,
 *   the eight resize edges, double-click to maximise, the system menu, and -
 *   the classic - keeping a maximised window from covering the taskbar.
 *   docs/架构与数据结构.md section 1.4 lists them; this file implements them.
 *
 * WHAT THE WINDOW PROCEDURE MUST DO
 *
 *   case WM_NCCALCSIZE:    return 0            (client area == whole window)
 *   case WM_NCHITTEST:     return frame_hit_test(...)
 *   case WM_GETMINMAXINFO: frame_adjust_min_max_info(...)
 *   case WM_NCMOUSEMOVE:   frame_set_hot(frame_button_at(...))
 *   case WM_NCMOUSELEAVE:  frame_set_hot(FRAME_BTN_NONE)
 *   case WM_PAINT:         frame_paint(...)    (clipped to the caption)
 *   case WM_NCACTIVATE:    repaint the caption
 *   case WM_NCRBUTTONUP:   show the system menu
 *
 *   Returning the real HTMINBUTTON / HTMAXBUTTON / HTCLOSE codes (rather than
 *   HTCAPTION plus hand-rolled click handling) is what keeps the platform
 *   behaviours working: DefWindowProc turns those into WM_SYSCOMMAND, so
 *   minimising, maximising, restoring and closing all behave like the system
 *   window even though the pixels are ours.
 */

#ifndef WAC_FRAME_H
#define WAC_FRAME_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Caption buttons. The values double as the "hot" index. */
#define FRAME_BTN_NONE   0
#define FRAME_BTN_MIN    1
#define FRAME_BTN_MAX    2
#define FRAME_BTN_CLOSE  3

typedef struct {
    HWND  hwnd;
    HICON icon;
    HFONT font;

    int   dpi;
    int   height;      /* caption height, physical pixels */
    int   btn_width;   /* width of one caption button */
    int   border;      /* resize grip thickness */
    int   icon_size;
    int   pad;         /* left padding before the icon */

    int   hot;         /* FRAME_BTN_* currently under the cursor */
} Frame;

/*
 * Attach the frame to a window and compute the metrics for its DPI. Call once
 * after the window exists; call frame_refresh_metrics() again on WM_DPICHANGED.
 */
void frame_attach(Frame *f, HWND hwnd, HICON icon, HFONT font);
void frame_refresh_metrics(Frame *f);

/* Caption height in physical pixels. Zero before frame_attach(). */
int frame_height(const Frame *f);

/*
 * Hit test in SCREEN coordinates. Returns an HT* code: HTCAPTION, HTCLIENT,
 * HTMINBUTTON, HTMAXBUTTON, HTCLOSE, or one of the eight resize codes.
 *
 * A maximised window reports no resize border - dragging its edge must not
 * resize it, which is also what the system does.
 */
LRESULT frame_hit_test(Frame *f, POINT pt_screen);

/* Caption button under the point (screen coordinates), or FRAME_BTN_NONE. */
int frame_button_at(const Frame *f, POINT pt_screen);

/*
 * Record which button the cursor is over. Returns TRUE when the value changed,
 * i.e. when the caption needs repainting - so the caller can avoid repainting
 * on every mouse move.
 */
BOOL frame_set_hot(Frame *f, int button);

/*
 * Draw the caption. The caller is expected to have selected the clip region
 * (BeginPaint already does) - this paints the top frame_height pixels.
 */
void frame_paint(const Frame *f, HDC dc, const wchar_t *title, BOOL active);

/*
 * Pure geometry for a maximised window.
 *
 * TWO MEASURED FACTS DRIVE THIS (probed with tests/t16_maxprobe.c)
 *
 *   1. Windows makes a maximised window's rectangle one frame thickness
 *      LARGER than the work area on every side: with ptMaxSize set to the work
 *      area (2560x1600) the resulting window rect is (-11,-11)-(2571,1611),
 *      i.e. 2582x1622 at 150% scaling (SM_CXSIZEFRAME 5 + SM_CXPADDEDBORDER 6).
 *
 *   2. Asking for anything SMALLER than that default in WM_GETMINMAXINFO makes
 *      Windows use the requested rectangle verbatim - no border is added. So
 *      "compensating" ptMaxSize by the border does not cancel fact 1, it just
 *      shrinks the window: measured, a compensation of 11 produced a
 *      2538x1578 window, 22 px SMALLER than the work area.
 *
 * The consequence is that ptMaxPosition/ptMaxSize must ask for the work area
 * itself, and the CLIENT area is then clamped to the work area in
 * WM_NCCALCSIZE. The window rectangle sticks out past the screen edge by one
 * border; the visible content stops exactly at the work area, so a maximised
 * window never covers the taskbar.
 *
 * frame_maximized_client_rect() performs that clamp. It is a pure function so
 * the arithmetic can be tested against constructed monitors - including one
 * with a visible taskbar, which this machine cannot produce because its
 * taskbar auto-hides (tests/t15_framemath.c).
 */

/* ptMaxPosition / ptMaxSize for a maximised window: the work area, expressed
 * relative to the monitor's top-left, which is what MINMAXINFO expects. */
void frame_compute_maximized_rect(const RECT *work, const RECT *monitor,
                                  POINT *out_pos, POINT *out_size);

/*
 * The client rectangle, in WINDOW coordinates, for a maximised window.
 *
 * window_rect is what WM_NCCALCSIZE receives in rgrc[0] - it already includes
 * the invisible resize border Windows adds, which is why the true window
 * origin is window_rect.left + border and not window_rect.left.
 */
RECT frame_maximized_client_rect(const RECT *window_rect, const RECT *work,
                                 int border);

/*
 * Fill in the maximised size and position so a maximised window covers the
 * monitor's WORK area, not the whole monitor. Without this the window hides
 * the taskbar - the single most common mistake in a hand-drawn frame.
 * Also sets the minimum tracking size.
 */
void frame_adjust_min_max_info(const Frame *f, MINMAXINFO *mmi);

/* Show the system menu at the given screen point (right-click on the caption). */
void frame_show_system_menu(Frame *f, POINT pt_screen);

#ifdef __cplusplus
}
#endif

#endif /* WAC_FRAME_H */
