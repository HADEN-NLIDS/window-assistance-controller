/*
 * win_enum.c - see win_enum.h for what this is for and why the filter has
 * the shape it has.
 *
 * Read-only with respect to other applications: nothing in this file sends a
 * message, moves a window, or changes any state. It opens processes only to
 * ask for their image path, with PROCESS_QUERY_LIMITED_INFORMATION, which
 * works across the elevation boundary.
 *
 * The one non-obvious API choice is GetWindowTextW. It looks like it should
 * be SendMessageTimeoutW(WM_GETTEXT) to avoid hanging on an unresponsive
 * process, but GetWindowTextW is documented to return the cached caption
 * without sending WM_GETTEXT when the target belongs to another process -
 * "so that applications do not become unresponsive". It is the safe call,
 * and it must NOT be replaced with a direct SendMessage.
 */

#include "win_enum.h"

#include <dwmapi.h>
#include <string.h>
#include <wchar.h>

/* ------------------------------------------------------------------ */
/* Reasons                                                             */
/* ------------------------------------------------------------------ */

const wchar_t *win_filter_reason_name(WinFilterReason r)
{
    switch (r) {
    case WIN_OK:             return L"accepted";
    case WIN_DEAD:           return L"handle is not a window";
    case WIN_NOT_TOPLEVEL:   return L"owned window (dialog or popup)";
    case WIN_NOT_VISIBLE:    return L"not visible";
    case WIN_TOOLWINDOW:     return L"WS_EX_TOOLWINDOW";
    case WIN_NOACTIVATE:     return L"WS_EX_NOACTIVATE";
    case WIN_NO_RECT:        return L"GetWindowRect failed";
    case WIN_TOO_SMALL:      return L"smaller than the minimum";
    case WIN_EMPTY_TITLE:    return L"empty caption";
    case WIN_CLOAKED:        return L"DWM-cloaked";
    case WIN_OWN_PROCESS:    return L"belongs to this program";
    default:                 return L"(unknown)";
    }
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* DWM cloaked state. dwmapi is linked, so no LoadLibrary dance is needed
 * here; the call is wrapped anyway because a failure is not an error - a
 * machine with DWM composition off simply reports "not cloaked". */
static int is_cloaked(HWND hwnd)
{
    BOOL cloaked = FALSE;
    HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (SUCCEEDED(hr) && cloaked) return 1;
    return 0;
}

static WinFilterReason classify_ex(HWND hwnd, DWORD *out_pid, int skip_own)
{
    DWORD pid = 0;

    if (!IsWindow(hwnd)) {
        if (out_pid) *out_pid = 0;
        return WIN_DEAD;
    }

    GetWindowThreadProcessId(hwnd, &pid);
    if (out_pid) *out_pid = pid;

    if (GetWindow(hwnd, GW_OWNER) != NULL) return WIN_NOT_TOPLEVEL;
    if (!IsWindowVisible(hwnd))            return WIN_NOT_VISIBLE;

    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return WIN_TOOLWINDOW;
    if (ex & WS_EX_NOACTIVATE) return WIN_NOACTIVATE;

    RECT r;
    if (!GetWindowRect(hwnd, &r)) return WIN_NO_RECT;
    if ((r.right - r.left) < WIN_MIN_W || (r.bottom - r.top) < WIN_MIN_H) return WIN_TOO_SMALL;

    if (GetWindowTextLengthW(hwnd) == 0) return WIN_EMPTY_TITLE;

    if (is_cloaked(hwnd)) return WIN_CLOAKED;

    if (skip_own && pid == GetCurrentProcessId()) return WIN_OWN_PROCESS;

    return WIN_OK;
}

WinFilterReason win_classify(HWND hwnd, DWORD *out_pid)
{
    return classify_ex(hwnd, out_pid, 1);
}

/* ------------------------------------------------------------------ */
/* Attributes                                                          */
/* ------------------------------------------------------------------ */

BOOL win_fill_info(HWND hwnd, WindowInfo *out, unsigned flags)
{
    if (out == NULL) return FALSE;

    ZeroMemory(out, sizeof(*out));

    if (!IsWindow(hwnd)) return FALSE;

    out->hwnd = hwnd;
    GetWindowThreadProcessId(hwnd, &out->pid);

    out->style    = GetWindowLongPtrW(hwnd, GWL_STYLE);
    out->ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    /* A failed GetWindowRect leaves the rectangle zeroed rather than
     * aborting: a window with no rectangle is still worth reporting, and
     * the zeroed rect makes it obvious in a listing. */
    if (!GetWindowRect(hwnd, &out->rect)) {
        out->rect.left = out->rect.top = out->rect.right = out->rect.bottom = 0;
    }

    out->is_visible = IsWindowVisible(hwnd) ? 1 : 0;
    out->is_iconic  = IsIconic(hwnd) ? 1 : 0;
    out->is_zoomed  = IsZoomed(hwnd) ? 1 : 0;
    out->is_cloaked = is_cloaked(hwnd);
    out->is_own     = (out->pid == GetCurrentProcessId()) ? 1 : 0;

    /* The caption is always read: WIN_ENUM_NEED_TITLE decides whether an
     * empty one is a reason to REJECT the window, not whether it is worth
     * reporting. A listing that hides captions cannot explain itself. */
    GetWindowTextW(hwnd, out->title, WAC_TITLE_MAX);

    GetClassNameW(hwnd, out->class_name, WAC_CLASS_MAX);

    if (flags & WIN_ENUM_NEED_EXE) {
        win_exe_path(out->pid, out->exe_path, WAC_EXE_MAX);
    }

    return TRUE;
}

BOOL win_exe_path(DWORD pid, wchar_t *out, size_t cch)
{
    if (out == NULL || cch < 2) return FALSE;
    out[0] = L'\0';
    if (pid == 0) return FALSE;

    /*
     * PROCESS_QUERY_LIMITED_INFORMATION rather than PROCESS_QUERY_INFORMATION:
     * the limited right is granted across the elevation boundary and for most
     * protected-ish processes, so an elevated application still reports its
     * name instead of looking like an unknown program. It is also the right
     * that costs nothing to ask for.
     */
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == NULL) return FALSE;

    /* QueryFullProcessImageNameW wants the buffer size WITHOUT room for the
     * terminator: cch characters of storage hold cch-1 characters plus NUL.
     * Passing cch made a maximum-length path fail instead of truncating. */
    DWORD n = (DWORD)(cch - 1);
    BOOL ok = QueryFullProcessImageNameW(h, 0, out, &n);
    CloseHandle(h);

    if (!ok) {
        out[0] = L'\0';
        return FALSE;
    }
    return TRUE;
}

const wchar_t *win_file_name(const wchar_t *path)
{
    if (path == NULL) return L"";
    const wchar_t *slash = wcsrchr(path, L'\\');
    return (slash != NULL) ? slash + 1 : path;
}

/* ------------------------------------------------------------------ */
/* Enumeration                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    WindowInfo *out;
    int         cap;
    int         count;
    unsigned    flags;
} EnumCtx;

static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lp)
{
    EnumCtx *ctx = (EnumCtx *)lp;
    const unsigned f = ctx->flags;

    if (ctx->count >= ctx->cap) return FALSE;   /* stop the enumeration */

    /* ---- filters, cheapest first --------------------------------- */
    if (f & WIN_ENUM_SKIP_OWN) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) return TRUE;
    }

    if (f & WIN_ENUM_SKIP_OWNED) {
        if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;
    }

    if (f & WIN_ENUM_VISIBLE_ONLY) {
        if (!IsWindowVisible(hwnd)) return TRUE;
    }

    if (f & WIN_ENUM_SKIP_TOOLWIN) {
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return TRUE;
    }

    if (f & WIN_ENUM_NEED_TITLE) {
        if (GetWindowTextLengthW(hwnd) == 0) return TRUE;
    }

    if (f & WIN_ENUM_SKIP_SMALL) {
        RECT r;
        if (!GetWindowRect(hwnd, &r)) return TRUE;
        if ((r.right - r.left) < WIN_MIN_W || (r.bottom - r.top) < WIN_MIN_H) return TRUE;
    }

    if (f & WIN_ENUM_SKIP_CLOAKED) {
        if (is_cloaked(hwnd)) return TRUE;
    }

    /* ---- capture -------------------------------------------------- */
    if (!win_fill_info(hwnd, &ctx->out[ctx->count], f)) return TRUE;
    ctx->count++;
    return TRUE;
}

int win_enum_top(WindowInfo *out, int cap, unsigned flags)
{
    if (out == NULL || cap <= 0) return 0;

    EnumCtx ctx;
    ctx.out   = out;
    ctx.cap   = cap;
    ctx.count = 0;
    ctx.flags = flags;

    EnumWindows(enum_proc, (LPARAM)&ctx);
    return ctx.count;
}

/* ------------------------------------------------------------------ */
/* State probes                                                        */
/* ------------------------------------------------------------------ */

BOOL win_is_fullscreen_exclusive(HWND hwnd, const RECT *rc_monitor)
{
    if (hwnd == NULL || rc_monitor == NULL) return FALSE;
    if (!IsWindow(hwnd)) return FALSE;

    /* The desktop and the shell's window cover the monitor exactly and are
     * not applications. Moving either would be catastrophic rather than
     * merely wrong. */
    if (hwnd == GetDesktopWindow()) return FALSE;
    if (hwnd == GetShellWindow())  return FALSE;

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);

    if (style & WS_CAPTION)    return FALSE;   /* has a title bar */
    if (style & WS_THICKFRAME) return FALSE;   /* resizable border */
    if (!(style & WS_POPUP))   return FALSE;   /* not a borderless top-level */

    RECT r;
    if (!GetWindowRect(hwnd, &r)) return FALSE;

    /*
     * EXACT cover, not "covers at least". A window that merely overlaps the
     * whole monitor - a maximized window with a hidden frame, or a borderless
     * window the user resized by hand - is an ordinary window that the user
     * may well want placed. Calling those full-screen would make the tool
     * silently refuse to work on exactly the windows it is most useful for.
     */
    if (r.left   != rc_monitor->left   || r.top    != rc_monitor->top ||
        r.right  != rc_monitor->right  || r.bottom != rc_monitor->bottom) {
        return FALSE;
    }

    return TRUE;
}

BOOL win_style_resizable(HWND hwnd)
{
    if (hwnd == NULL || !IsWindow(hwnd)) return FALSE;
    return (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME) ? TRUE : FALSE;
}

BOOL win_alive(HWND hwnd)
{
    return IsWindow(hwnd) ? TRUE : FALSE;
}
