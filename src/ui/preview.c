/*
 * preview.c - the zone preview control.
 *
 * A plain child window that paints itself in WM_PAINT. No owner-draw
 * notification, no subclassing, no bitmaps - the whole picture is a few filled
 * rectangles, which is exactly as much as the job needs.
 */

#include "preview.h"

#include <stdlib.h>   /* calloc / free */

/* ------------------------------------------------------------------ */
/* Pure geometry                                                       */
/* ------------------------------------------------------------------ */

BOOL preview_monitor_rect(int ctrl_w, int ctrl_h, int mon_w, int mon_h,
                          int pad, RECT *out)
{
    if (out == NULL) return FALSE;

    ZeroMemory(out, sizeof(*out));

    if (mon_w <= 0 || mon_h <= 0) { mon_w = 16; mon_h = 10; }
    if (pad < 0) pad = 0;

    const int avail_w = ctrl_w - 2 * pad;
    const int avail_h = ctrl_h - 2 * pad;
    if (avail_w <= 0 || avail_h <= 0) return FALSE;

    /*
     * Fit the monitor's shape inside the control, whichever axis runs out
     * first. Computing both scales and taking the smaller one is what keeps a
     * 2560x1600 panel from being drawn as a square in a wide control - and a
     * 1440x2560 portrait panel from being drawn as a landscape one.
     */
    const int by_w = (int)((long long)avail_w * mon_h / mon_w);
    int w, h;

    if (by_w <= avail_h) {
        w = avail_w;
        h = by_w;
    } else {
        h = avail_h;
        w = (int)((long long)avail_h * mon_w / mon_h);
    }

    if (w <= 0 || h <= 0) return FALSE;

    out->left   = pad + (avail_w - w) / 2;
    out->top    = pad + (avail_h - h) / 2;
    out->right  = out->left + w;
    out->bottom = out->top + h;
    return TRUE;
}

BOOL preview_zone_rect(const RECT *monitor, double x, double y, double w, double h,
                       RECT *out)
{
    if (monitor == NULL || out == NULL) return FALSE;

    ZeroMemory(out, sizeof(*out));

    const double mon_w = (double)(monitor->right - monitor->left);
    const double mon_h = (double)(monitor->bottom - monitor->top);
    if (mon_w <= 0.0 || mon_h <= 0.0) return FALSE;

    /* NaN and out-of-range values are clamped rather than rejected: the
     * preview's job is to SHOW what the numbers mean, and refusing to draw
     * would leave the user guessing. */
    if (x != x) x = 0.0;
    if (y != y) y = 0.0;
    if (w != w) w = 0.0;
    if (h != h) h = 0.0;

    if (x < 0.0) x = 0.0;
    if (y < 0.0) y = 0.0;
    if (w < 0.0) w = 0.0;
    if (h < 0.0) h = 0.0;
    if (x > 1.0) x = 1.0;
    if (y > 1.0) y = 1.0;
    if (w > 1.0) w = 1.0;
    if (h > 1.0) h = 1.0;

    out->left   = monitor->left + (LONG)(x * mon_w + 0.5);
    out->top    = monitor->top  + (LONG)(y * mon_h + 0.5);
    out->right  = monitor->left + (LONG)((x + w) * mon_w + 0.5);
    out->bottom = monitor->top  + (LONG)((y + h) * mon_h + 0.5);

    /* Keep it inside the monitor outline and at least one pixel wide, so a
     * degenerate zone is still visible rather than invisible. */
    if (out->left   < monitor->left)   out->left   = monitor->left;
    if (out->top    < monitor->top)    out->top    = monitor->top;
    if (out->right  > monitor->right)  out->right  = monitor->right;
    if (out->bottom > monitor->bottom) out->bottom = monitor->bottom;

    if (out->right  <= out->left) out->right  = out->left + 1;
    if (out->bottom <= out->top)  out->bottom = out->top + 1;

    if (out->right > monitor->right)  out->right  = monitor->right;
    if (out->bottom > monitor->bottom) out->bottom = monitor->bottom;

    return TRUE;
}

/* ------------------------------------------------------------------ */
/* The control                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    double x, y, w, h;
    /*
     * The window's real size as fractions of the same monitor, or 0 when there
     * is nothing to draw. 0 is the normal state: it means "this window can be
     * resized", so the zone is the whole story (see preview_set_real_size).
     */
    double real_w, real_h;
    int    mon_w, mon_h;
} PreviewData;

static const wchar_t *k_class = L"WacPreview";

static PreviewData *data_of(HWND hwnd)
{
    return (PreviewData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static void paint(HWND hwnd, HDC dc)
{
    RECT client;
    GetClientRect(hwnd, &client);

    /* Face colour background, so the control sits on the dialog like the
     * labels around it rather than like a white box. */
    FillRect(dc, &client, GetSysColorBrush(COLOR_BTNFACE));

    const PreviewData *d = data_of(hwnd);
    const int pad = 6;

    RECT mon;
    if (!preview_monitor_rect(client.right - client.left,
                              client.bottom - client.top,
                              (d != NULL) ? d->mon_w : 16,
                              (d != NULL) ? d->mon_h : 10,
                              pad, &mon)) {
        return;   /* no room: leave the plain background */
    }

    /* Monitor outline: a window-coloured face inside a dark border. */
    FillRect(dc, &mon, GetSysColorBrush(COLOR_WINDOW));
    FrameRect(dc, &mon, GetSysColorBrush(COLOR_WINDOWFRAME));

    if (d == NULL) return;

    /* Taskbar: a strip along the bottom edge of the monitor, just enough to
     * make "the work area" visible - a window's zone is relative to the whole
     * monitor, and seeing the bar is how a user notices that. */
    RECT taskbar = mon;
    const int bar_h = (mon.bottom - mon.top) / 16;
    if (bar_h >= 2) {
        taskbar.top = taskbar.bottom - bar_h;
        taskbar.left += 1;
        taskbar.right -= 1;
        taskbar.bottom -= 1;
        FillRect(dc, &taskbar, GetSysColorBrush(COLOR_BTNSHADOW));
    }

    /* The zone itself. */
    RECT zone;
    if (!preview_zone_rect(&mon, d->x, d->y, d->w, d->h, &zone)) return;

    /* A translucent look is not worth a layered window here: a hatch brush
     * reads as "this is the window" and stays visible over both the face and
     * the taskbar strip. */
    HBRUSH fill = CreateHatchBrush(HS_BDIAGONAL, GetSysColor(COLOR_HIGHLIGHT));
    if (fill != NULL) {
        FillRect(dc, &zone, fill);
        DeleteObject(fill);
    }

    /* Two-pixel border: FrameRect is one pixel, and one pixel disappears
     * against the hatch at 150% scaling. */
    FrameRect(dc, &zone, GetSysColorBrush(COLOR_HIGHLIGHT));
    RECT inner = zone;
    InflateRect(&inner, -1, -1);
    if (inner.right > inner.left && inner.bottom > inner.top) {
        FrameRect(dc, &inner, GetSysColorBrush(COLOR_HIGHLIGHT));
    }

    /*
     * The window's own size, once the preset has learned it cannot be resized.
     *
     * Same top-left corner as the zone - the window is still placed there - but
     * its OWN width and height. Dashed and unfilled on purpose: the hatched
     * rectangle is what the preset asks for, this outline is what will actually
     * happen, and the gap between them is the point of drawing both.
     */
    if (d->real_w > 0.0 && d->real_h > 0.0) {
        RECT real;
        if (preview_zone_rect(&mon, d->x, d->y, d->real_w, d->real_h, &real)) {
            HPEN pen = CreatePen(PS_DOT, 1, GetSysColor(COLOR_WINDOWTEXT));
            if (pen != NULL) {
                HGDIOBJ old_pen   = SelectObject(dc, pen);
                HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, real.left, real.top, real.right, real.bottom);
                SelectObject(dc, old_brush);
                SelectObject(dc, old_pen);
                DeleteObject(pen);
            }
        }
    }
}

static LRESULT CALLBACK preview_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;   /* painted in WM_PAINT; erasing first only flickers */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NCDESTROY: {
        PreviewData *d = data_of(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        free(d);
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND preview_create(HWND parent, HINSTANCE hinst, int id)
{
    static BOOL registered = FALSE;

    if (!registered) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = preview_proc;
        wc.hInstance     = hinst;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = k_class;

        if (RegisterClassExW(&wc) == 0 &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return NULL;
        }
        registered = TRUE;
    }

    PreviewData *d = (PreviewData *)calloc(1, sizeof(PreviewData));
    if (d == NULL) return NULL;

    d->x = 0.0; d->y = 0.0; d->w = 0.5; d->h = 1.0;
    d->real_w = 0.0;
    d->real_h = 0.0;
    d->mon_w = 16;
    d->mon_h = 10;

    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, k_class, L"",
                             WS_CHILD | WS_VISIBLE,
                             0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, hinst, d);
    if (h == NULL) {
        free(d);
        return NULL;
    }
    return h;
}

void preview_set_zone(HWND preview, double x, double y, double w, double h)
{
    PreviewData *d = data_of(preview);
    if (d == NULL) return;

    d->x = x; d->y = y; d->w = w; d->h = h;
    InvalidateRect(preview, NULL, TRUE);
}

void preview_set_monitor(HWND preview, int mon_w, int mon_h)
{
    PreviewData *d = data_of(preview);
    if (d == NULL) return;

    if (mon_w > 0) d->mon_w = mon_w;
    if (mon_h > 0) d->mon_h = mon_h;
    InvalidateRect(preview, NULL, TRUE);
}

void preview_set_real_size(HWND preview, double w, double h)
{
    PreviewData *d = data_of(preview);
    if (d == NULL) return;

    /* Either axis <= 0 means "no second rectangle", so a caller that cannot
     * measure the window does not have to special-case its call. */
    d->real_w = (w > 0.0) ? w : 0.0;
    d->real_h = (h > 0.0) ? h : 0.0;
    InvalidateRect(preview, NULL, TRUE);
}
