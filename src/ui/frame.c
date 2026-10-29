/*
 * frame.c - drawing and behaviour of the self-drawn window frame.
 *
 * The caption is part of the CLIENT area: WM_NCCALCSIZE returns 0, so every
 * pixel of the window belongs to us and the caption is painted in WM_PAINT
 * like any other content. That is what makes the themed colours planned for
 * the polish pass possible at all.
 *
 * Windows 10 1607+ provides GetDpiForWindow and GetSystemMetricsForDpi. The
 * project targets Windows 10 1809 and later (res/wac.manifest), so they are
 * called directly rather than resolved through GetProcAddress.
 */

#include "frame.h"

/* The system's own close-button red, so the hover state looks familiar. */
#define CLR_CLOSE_HOT   RGB(232, 17, 35)
#define CLR_CLOSE_INK   RGB(255, 255, 255)

/* ------------------------------------------------------------------ */
/* Small drawing helpers                                               */
/* ------------------------------------------------------------------ */

static void fill_rect(HDC dc, const RECT *r, COLORREF color)
{
    const HBRUSH b = CreateSolidBrush(color);
    if (b == NULL) return;
    FillRect(dc, r, b);
    DeleteObject(b);
}

static void draw_line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color)
{
    const HPEN p = CreatePen(PS_SOLID, 1, color);
    if (p == NULL) return;

    const HGDIOBJ old = SelectObject(dc, p);
    MoveToEx(dc, x1, y1, NULL);
    LineTo(dc, x2, y2);
    SelectObject(dc, old);
    DeleteObject(p);
}

static void draw_rect_outline(HDC dc, const RECT *r, COLORREF color)
{
    const HPEN     p  = CreatePen(PS_SOLID, 1, color);
    const HBRUSH   br = (HBRUSH)GetStockObject(NULL_BRUSH);
    if (p == NULL) return;

    const HGDIOBJ oldp = SelectObject(dc, p);
    const HGDIOBJ oldb = SelectObject(dc, br);

    /* Rectangle draws the outline only when the brush is NULL_BRUSH, and it
     * excludes the right/bottom edge - which is exactly the convention the
     * metrics below use. */
    Rectangle(dc, r->left, r->top, r->right, r->bottom);

    SelectObject(dc, oldb);
    SelectObject(dc, oldp);
    DeleteObject(p);
}

/* ------------------------------------------------------------------ */
/* Metrics                                                             */
/* ------------------------------------------------------------------ */

static int dpi_of(HWND hwnd)
{
    const UINT dpi = GetDpiForWindow(hwnd);
    return (dpi == 0) ? 96 : (int)dpi;
}

void frame_refresh_metrics(Frame *f)
{
    if (f == NULL || f->hwnd == NULL) return;

    f->dpi       = dpi_of(f->hwnd);
    f->height    = GetSystemMetricsForDpi(SM_CYCAPTION,       (UINT)f->dpi);

    /*
     * SM_CXSIZE, NOT SM_CXSMSIZE.
     *
     *   SM_CXSIZE    width of a caption button on a normal title bar
     *   SM_CXSMSIZE  width of a button on a SMALL caption - the thin strip a
     *                tool window gets
     *
     * They are not close: on this machine at 144 dpi SM_CXSIZE is 54 while
     * SM_CXSMSIZE is 33. Using the small-caption metric made the three buttons
     * 33x34 px - near enough to square to be reported as "square buttons
     * appearing next to the real ones", and narrow enough that the click
     * targets no longer lined up with where a Windows user expects them.
     */
    f->btn_width = GetSystemMetricsForDpi(SM_CXSIZE,          (UINT)f->dpi);

    f->border    = GetSystemMetricsForDpi(SM_CXSIZEFRAME,     (UINT)f->dpi)
                 + GetSystemMetricsForDpi(SM_CXPADDEDBORDER,  (UINT)f->dpi);
    f->icon_size = GetSystemMetricsForDpi(SM_CXSMICON,        (UINT)f->dpi);
    f->pad       = MulDiv(8, f->dpi, 96);

    /* A zero here would collapse the caption and make the window undraggable;
     * fall back to sane values rather than trusting the system blindly. */
    if (f->height    <= 0) f->height    = MulDiv(30, f->dpi, 96);
    if (f->btn_width <= 0) f->btn_width = MulDiv(45, f->dpi, 96);
    if (f->border    <= 0) f->border    = MulDiv(4,  f->dpi, 96);
    if (f->icon_size <= 0) f->icon_size = MulDiv(16, f->dpi, 96);
}

void frame_attach(Frame *f, HWND hwnd, HICON icon, HFONT font)
{
    if (f == NULL) return;

    ZeroMemory(f, sizeof(*f));
    f->hwnd = hwnd;
    f->icon = icon;
    f->font = font;
    f->hot  = FRAME_BTN_NONE;

    frame_refresh_metrics(f);
}

int frame_height(const Frame *f)
{
    return (f == NULL) ? 0 : f->height;
}

/* ------------------------------------------------------------------ */
/* Hit testing                                                         */
/* ------------------------------------------------------------------ */

int frame_button_at(const Frame *f, POINT pt)
{
    if (f == NULL || f->hwnd == NULL || f->btn_width <= 0) return FRAME_BTN_NONE;

    RECT rc;
    if (!GetWindowRect(f->hwnd, &rc)) return FRAME_BTN_NONE;

    if (pt.y < rc.top || pt.y >= rc.top + f->height) return FRAME_BTN_NONE;
    if (pt.x < rc.left || pt.x >= rc.right) return FRAME_BTN_NONE;

    /* Close is rightmost, then maximise, then minimise. */
    if (pt.x >= rc.right - f->btn_width)     return FRAME_BTN_CLOSE;
    if (pt.x >= rc.right - 2 * f->btn_width) return FRAME_BTN_MAX;
    if (pt.x >= rc.right - 3 * f->btn_width) return FRAME_BTN_MIN;

    return FRAME_BTN_NONE;
}

LRESULT frame_hit_test(Frame *f, POINT pt)
{
    if (f == NULL || f->hwnd == NULL) return HTCLIENT;

    RECT rc;
    if (!GetWindowRect(f->hwnd, &rc)) return HTCLIENT;

    const BOOL maximized = IsZoomed(f->hwnd);

    /*
     * Resize borders. A maximised window reports none: dragging its edge must
     * not resize it, and that is also what the system does.
     */
    if (!maximized && f->border > 0) {
        const BOOL left   = (pt.x <  rc.left   + f->border);
        const BOOL right  = (pt.x >= rc.right  - f->border);
        const BOOL top    = (pt.y <  rc.top    + f->border);
        const BOOL bottom = (pt.y >= rc.bottom - f->border);

        if (top    && left)  return HTTOPLEFT;
        if (top    && right) return HTTOPRIGHT;
        if (bottom && left)  return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left)            return HTLEFT;
        if (right)           return HTRIGHT;
        if (top)             return HTTOP;
        if (bottom)          return HTBOTTOM;
    }

    /*
     * Caption - the WHOLE strip, buttons included, reports HTCAPTION.
     *
     * Returning the real HTMINBUTTON / HTMAXBUTTON / HTCLOSE codes hands those
     * three buttons to the system. It then draws its own set of them in
     * WM_NCPAINT - and on a window that reports no non-client area they land in
     * the wrong place, showing up as stray square buttons that appear on click
     * and never go away (reported from testing; returning 0 from WM_NCPAINT
     * alone did NOT stop it, because the system is entitled to paint a
     * non-client area it believes it owns).
     *
     * Claiming the whole strip as a caption removes that entitlement. The
     * buttons are still recognised by frame_button_at(), and the window
     * procedure routes clicks on them in WM_NCLBUTTONDOWN.
     *
     * Cost, accepted deliberately: no system tooltips over the buttons, and no
     * Windows 11 Snap Layouts popup on the maximise button.
     */
    if (pt.y >= rc.top && pt.y < rc.top + f->height &&
        pt.x >= rc.left && pt.x < rc.right) {
        return HTCAPTION;
    }

    return HTCLIENT;
}

BOOL frame_set_hot(Frame *f, int button)
{
    if (f == NULL) return FALSE;
    if (f->hot == button) return FALSE;

    f->hot = button;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
/* ------------------------------------------------------------------ */

static void paint_button(const Frame *f, HDC dc, const RECT *r, int which)
{
    const BOOL hot      = (f->hot == which);
    const BOOL is_close = (which == FRAME_BTN_CLOSE);

    if (hot) {
        fill_rect(dc, r, is_close ? CLR_CLOSE_HOT : GetSysColor(COLOR_BTNSHADOW));
    }

    const COLORREF ink = (hot && is_close) ? CLR_CLOSE_INK
                                           : GetSysColor(COLOR_BTNTEXT);

    const int cx = (r->left + r->right) / 2;
    const int cy = (r->top + r->bottom) / 2;
    const int g  = MulDiv(10, f->dpi, 96) / 2;      /* half glyph size */

    switch (which) {
    case FRAME_BTN_MIN:
        draw_line(dc, cx - g, cy, cx + g, cy, ink);
        break;

    case FRAME_BTN_MAX:
        if (IsZoomed(f->hwnd)) {
            /* "Restore": two overlapping squares, the front one 2px down-left
             * and drawn with a notch so the two read as separate. */
            RECT back;
            back.left   = cx - g;
            back.top    = cy - g;
            back.right  = cx + g - 2;
            back.bottom = cy + g - 2;

            RECT front = back;
            front.left   += 3;
            front.top    += 3;
            front.right  += 3;
            front.bottom += 3;

            draw_line(dc, back.left + 3, back.top, back.right + 3, back.top, ink);
            draw_line(dc, back.right + 3, back.top, back.right + 3, back.bottom, ink);
            draw_rect_outline(dc, &front, ink);
        } else {
            RECT box;
            box.left   = cx - g;
            box.top    = cy - g;
            box.right  = cx + g;
            box.bottom = cy + g;
            draw_rect_outline(dc, &box, ink);
        }
        break;

    case FRAME_BTN_CLOSE:
        draw_line(dc, cx - g, cy - g, cx + g, cy + g, ink);
        draw_line(dc, cx + g, cy - g, cx - g, cy + g, ink);
        break;

    default:
        break;
    }
}

void frame_paint(const Frame *f, HDC dc, const wchar_t *title, BOOL active)
{
    if (f == NULL || f->hwnd == NULL || dc == NULL) return;

    RECT client;
    GetClientRect(f->hwnd, &client);

    RECT bar = client;
    bar.bottom = f->height;
    if (bar.bottom <= bar.top) return;

    fill_rect(dc, &bar, GetSysColor(COLOR_BTNFACE));

    /* ---- caption buttons (right to left) ------------------------- */
    RECT btn;
    btn.top    = bar.top;
    btn.bottom = bar.bottom;
    btn.right  = bar.right;

    btn.left = btn.right - f->btn_width;
    paint_button(f, dc, &btn, FRAME_BTN_CLOSE);

    btn.right = btn.left;
    btn.left  = btn.right - f->btn_width;
    paint_button(f, dc, &btn, FRAME_BTN_MAX);

    btn.right = btn.left;
    btn.left  = btn.right - f->btn_width;
    paint_button(f, dc, &btn, FRAME_BTN_MIN);

    /* ---- icon ---------------------------------------------------- */
    int text_left = bar.left + f->pad;

    if (f->icon != NULL && f->icon_size > 0) {
        const int iy = bar.top + (f->height - f->icon_size) / 2;
        DrawIconEx(dc, text_left, iy, f->icon, f->icon_size, f->icon_size,
                   0, NULL, DI_NORMAL);
        text_left += f->icon_size + f->pad;
    }

    /* ---- title --------------------------------------------------- */
    RECT tr = bar;
    tr.left  = text_left;
    tr.right = btn.left - f->pad;      /* btn.left is now the minimise button */
    if (tr.right <= tr.left) return;   /* window too narrow for any text */

    const int old_mode = SetBkMode(dc, TRANSPARENT);
    const COLORREF old_ink = SetTextColor(dc, active ? GetSysColor(COLOR_BTNTEXT)
                                                     : GetSysColor(COLOR_GRAYTEXT));
    const HGDIOBJ old_font = (f->font != NULL) ? SelectObject(dc, f->font) : NULL;

    DrawTextW(dc, title != NULL ? title : L"", -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (old_font != NULL) SelectObject(dc, old_font);
    SetTextColor(dc, old_ink);
    SetBkMode(dc, old_mode);
}

/* ------------------------------------------------------------------ */
/* Maximised geometry and the system menu                              */
/* ------------------------------------------------------------------ */

void frame_compute_maximized_rect(const RECT *work, const RECT *monitor,
                                  POINT *out_pos, POINT *out_size)
{
    if (work == NULL || monitor == NULL || out_pos == NULL || out_size == NULL) {
        return;
    }

    /*
     * Ask for the work area exactly. See the header for why NOT to inset it by
     * the frame thickness: measured on this machine, a compensation of 11 px
     * produced a 2538x1578 window instead of the intended 2560x1600, because
     * Windows uses a requested size verbatim once it differs from the default.
     */
    out_pos->x  = work->left - monitor->left;
    out_pos->y  = work->top  - monitor->top;
    out_size->x = work->right  - work->left;
    out_size->y = work->bottom - work->top;

    /* A pathologically small work area must not turn into a negative size. */
    if (out_size->x < 0) out_size->x = 0;
    if (out_size->y < 0) out_size->y = 0;
}

RECT frame_maximized_client_rect(const RECT *window_rect, const RECT *work,
                                 int border)
{
    RECT c;

    if (window_rect == NULL || work == NULL) {
        ZeroMemory(&c, sizeof(c));
        return c;
    }

    /*
     * The rectangle Windows hands to WM_NCCALCSIZE already contains the
     * invisible resize border, so the window's real origin is offset by it.
     * Measured: rgrc[0] was (-11,-11)-(2571,1611) while the window actually
     * sat at (0,0) - hence the "+ border" below. Dropping it shifts the whole
     * client area by one border and the content no longer lines up with the
     * work area.
     */
    const int origin_x = window_rect->left + border;
    const int origin_y = window_rect->top  + border;

    c.left   = work->left   - origin_x;
    c.top    = work->top    - origin_y;
    c.right  = work->right  - origin_x;
    c.bottom = work->bottom - origin_y;

    return c;
}

void frame_adjust_min_max_info(const Frame *f, MINMAXINFO *mmi)
{
    if (f == NULL || mmi == NULL) return;

    const HMONITOR mon = MonitorFromWindow(f->hwnd, MONITOR_DEFAULTTONEAREST);

    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);

    if (GetMonitorInfoW(mon, &mi)) {
        frame_compute_maximized_rect(&mi.rcWork, &mi.rcMonitor,
                                     &mmi->ptMaxPosition, &mmi->ptMaxSize);
    }

    /*
     * Minimum tracking size: small enough that the user can shrink the window
     * to watch how the lists behave when they need a scrollbar. The layout
     * copes - it skips a pass rather than drawing garbage when there is no room
     * left - and a floor that cannot be reached is worse than a small one.
     */
    mmi->ptMinTrackSize.x = MulDiv(520, f->dpi, 96);
    mmi->ptMinTrackSize.y = MulDiv(320, f->dpi, 96);
}

void frame_show_system_menu(Frame *f, POINT pt)
{
    if (f == NULL || f->hwnd == NULL) return;

    HMENU menu = GetSystemMenu(f->hwnd, FALSE);
    if (menu == NULL) return;

    /*
     * The system greys its own items before showing the menu (WM_INITMENU
     * handling inside DefWindowProc). We drive the menu ourselves, so the
     * state has to be set here - otherwise "Restore" looks available on a
     * window that is not maximised.
     */
    const BOOL zoomed = IsZoomed(f->hwnd);
    EnableMenuItem(menu, SC_RESTORE,  MF_BYCOMMAND | (zoomed ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, SC_SIZE,     MF_BYCOMMAND | (zoomed ? MF_GRAYED  : MF_ENABLED));
    EnableMenuItem(menu, SC_MAXIMIZE, MF_BYCOMMAND | (zoomed ? MF_GRAYED  : MF_ENABLED));
    EnableMenuItem(menu, SC_MINIMIZE, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(menu, SC_CLOSE,    MF_BYCOMMAND | MF_ENABLED);

    /* Required so the menu dismisses correctly when the user clicks away. */
    SetForegroundWindow(f->hwnd);

    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                    pt.x, pt.y, 0, f->hwnd, NULL);
    if (cmd != 0) {
        /* Posted, not sent: we are inside the window's own message handling
         * and a modal move/resize loop must not start nested inside it. */
        PostMessageW(f->hwnd, WM_SYSCOMMAND, (WPARAM)cmd, 0);
    }
}
