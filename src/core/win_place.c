/*
 * win_place.c - see win_place.h for the contract and the measured evidence
 * behind it.
 *
 * The three interesting decisions in this file:
 *
 *   1. Every request ends with GetWindowRect. The return value of
 *      SetWindowPos is recorded for diagnostics and then ignored, because a
 *      TRUE from it does not mean the window obeyed.
 *
 *   2. Un-maximizing is done with SetWindowPlacement(SW_SHOWNOACTIVATE)
 *      rather than ShowWindow(SW_RESTORE), because SW_RESTORE is documented
 *      to ACTIVATE the window and would steal focus. ShowWindow is kept only
 *      as a fallback for the case where the window ignores the placement
 *      call, and that case is reported through the same read-back as
 *      everything else.
 *
 *   3. Two retries exist, one for each half of the request. A window that
 *      accepts a move but not a resize is a real, common category (stage 1
 *      measured it), so retrying the move on its own is worthwhile. The
 *      mirror case - the window moved correctly but came back the wrong
 *      size, which is what a cross-DPI move does to it - is retried with the
 *      size alone. A window that refuses the move gets PLACE_DENIED and no
 *      retry at all.
 *
 * All locals are declared at the top and assigned later. Every early exit is
 * a `goto done`, and a goto that jumps over an initialiser is warning C4533 -
 * which this project builds with /WX, so it is an error here.
 */

#include "win_place.h"

#include "win_enum.h"

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

const wchar_t *win_place_result_name(PlaceResult r)
{
    switch (r) {
    case PLACE_OK:                  return L"PLACE_OK";
    case PLACE_MOVED_ONLY:          return L"PLACE_MOVED_ONLY";
    case PLACE_CLAMPED:             return L"PLACE_CLAMPED";
    case PLACE_DENIED:              return L"PLACE_DENIED";
    case PLACE_GONE:                return L"PLACE_GONE";
    case PLACE_SKIPPED_FULLSCREEN:  return L"PLACE_SKIPPED_FULLSCREEN";
    case PLACE_SKIPPED_MINIMIZED:   return L"PLACE_SKIPPED_MINIMIZED";
    case PLACE_BAD_REQUEST:         return L"PLACE_BAD_REQUEST";
    default:                        return L"(unknown result)";
    }
}

BOOL win_place_result_ok(PlaceResult r)
{
    return (r == PLACE_OK || r == PLACE_MOVED_ONLY || r == PLACE_CLAMPED) ? TRUE : FALSE;
}

int win_place_default_tolerance(void)
{
    return 2;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int abs_int(int v) { return v < 0 ? -v : v; }

static int max_int(int a, int b) { return a > b ? a : b; }

static int rect_w(const RECT *r) { return (int)(r->right - r->left); }
static int rect_h(const RECT *r) { return (int)(r->bottom - r->top); }

static BOOL rect_ok(const RECT *r)
{
    return (r != NULL && r->right > r->left && r->bottom > r->top) ? TRUE : FALSE;
}

static void get_rect(HWND hwnd, RECT *out)
{
    ZeroMemory(out, sizeof(*out));
    GetWindowRect(hwnd, out);
}

/* How far apart two rectangles are, per axis. */
static int pos_delta(const RECT *want, const RECT *got)
{
    return max_int(abs_int((int)(got->left - want->left)),
                   abs_int((int)(got->top  - want->top)));
}

static int size_delta(const RECT *want, const RECT *got)
{
    return max_int(abs_int(rect_w(got) - rect_w(want)),
                   abs_int(rect_h(got) - rect_h(want)));
}

/*
 * Undo a maximized state without taking focus.
 *
 * SetWindowPlacement with SW_SHOWNOACTIVATE restores the window and leaves
 * the foreground alone, which is exactly what is needed. ShowWindow(SW_RESTORE)
 * is only reached when the placement call did not take effect - it is
 * documented to activate the window, so it is a last resort, and the report's
 * fg_before/fg_after pair will show that it happened.
 *
 * The stored rcNormalPosition is carried through unchanged: the point is to
 * leave the maximized STATE, not to relocate the window before the caller's
 * rectangle is applied.
 */
static void unmaximize_quietly(HWND hwnd)
{
    WINDOWPLACEMENT wp;
    ZeroMemory(&wp, sizeof(wp));
    wp.length = sizeof(wp);

    if (GetWindowPlacement(hwnd, &wp)) {
        if (wp.showCmd == SW_SHOWMAXIMIZED || wp.showCmd == SW_SHOWMINIMIZED) {
            wp.showCmd = SW_SHOWNOACTIVATE;
            wp.flags   = 0;
            SetWindowPlacement(hwnd, &wp);
        }
    }

    if (IsZoomed(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
}

/* The rectangle of the monitor a window is on, so full-screen detection has
 * something to compare against. */
static BOOL window_monitor_rect(HWND hwnd, RECT *out)
{
    HMONITOR hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;

    if (hm == NULL) return FALSE;

    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);

    if (!GetMonitorInfoW(hm, &mi)) return FALSE;
    *out = mi.rcMonitor;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* The operation                                                       */
/* ------------------------------------------------------------------ */

PlaceResult win_place_apply(const PlaceRequest *req, PlaceReport *out)
{
    PlaceReport local;
    RECT        mon;
    int         tolerance;
    int         want_x, want_y, want_w, want_h;
    UINT        base_flags;
    UINT        flags;
    BOOL        ok;

    ZeroMemory(&local, sizeof(local));
    ZeroMemory(&mon, sizeof(mon));

    /*
     * Sampled into `local` as well as into `out`.
     *
     * The first version set only out->fg_before, and then the copy at the
     * `done` label replaced the whole structure with `local` - whose
     * fg_before was never assigned. Every report therefore claimed the
     * foreground window had been 0x0 and had changed, on every run, for every
     * window. A diagnostic that always reports the same alarming thing is
     * worse than no diagnostic: it trains the reader to ignore it.
     */
    local.fg_before = GetForegroundWindow();

    if (out != NULL) {
        ZeroMemory(out, sizeof(*out));
        out->fg_before = local.fg_before;
    }

    /* ---- validate ------------------------------------------------ */

    if (req == NULL || req->hwnd == NULL) {
        local.result = PLACE_BAD_REQUEST;
        goto done;
    }
    if (!rect_ok(&req->target)) {
        local.result = PLACE_BAD_REQUEST;
        goto done;
    }
    if (!win_alive(req->hwnd)) {
        local.result = PLACE_GONE;
        goto done;
    }

    local.requested = req->target;

    /* ---- skip decisions, before touching anything ----------------- */

    /*
     * The minimized check comes FIRST, ahead of the full-screen check: a
     * minimized window reports its restored rectangle, so testing that
     * against the monitor would produce nonsense.
     *
     * Deliberately no restore. The user minimized it; a layout must not undo
     * that behind their back. The caller records the miss and can offer to
     * handle it later.
     */
    if (IsIconic(req->hwnd)) {
        local.result = PLACE_SKIPPED_MINIMIZED;
        get_rect(req->hwnd, &local.before);
        local.after = local.before;
        goto done;
    }

    if (window_monitor_rect(req->hwnd, &mon) &&
        win_is_fullscreen_exclusive(req->hwnd, &mon)) {
        local.result = PLACE_SKIPPED_FULLSCREEN;
        get_rect(req->hwnd, &local.before);
        local.after = local.before;
        goto done;
    }

    /*
     * Maximized is NOT a skip: a maximized window can be placed, it just has
     * to be un-maximized first. This is the one place where the tool changes
     * a state the user set, and it is unavoidable - Windows ignores position
     * requests for a maximized window.
     */
    if (IsZoomed(req->hwnd)) {
        unmaximize_quietly(req->hwnd);
        if (!win_alive(req->hwnd)) {
            local.result = PLACE_GONE;
            goto done;
        }
    }

    get_rect(req->hwnd, &local.before);

    tolerance = (req->tolerance_px > 0) ? req->tolerance_px
                                        : win_place_default_tolerance();

    want_x = (int)req->target.left;
    want_y = (int)req->target.top;
    want_w = rect_w(&req->target);
    want_h = rect_h(&req->target);

    /*
     * SWP_NOACTIVATE on every call, without exception. The whole feature is
     * worthless if applying a layout interrupts typing. SWP_NOOWNERZORDER
     * keeps an owned popup from being re-stacked, and SWP_NOZORDER leaves the
     * stacking order to the user.
     */
    base_flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;

    /* ---- attempt 1: move and, if allowed, resize ------------------ */

    flags = base_flags;
    if (!req->allow_resize) flags |= SWP_NOSIZE;

    SetLastError(0);
    ok = SetWindowPos(req->hwnd, NULL, want_x, want_y,
                      req->allow_resize ? want_w : 0,
                      req->allow_resize ? want_h : 0,
                      flags);

    local.last_error = ok ? 0 : GetLastError();

    if (!ok) {
        if (local.last_error == ERROR_ACCESS_DENIED) {
            /*
             * UIPI. The target runs at a higher integrity level, and no
             * amount of retrying changes that. Reported, never worked
             * around: a normal process driving an elevated one is exactly
             * what the integrity mechanism exists to prevent, and defeating
             * it is out of scope by design (see docs/安全设计.md).
             */
            local.result = PLACE_DENIED;
            get_rect(req->hwnd, &local.after);
            goto done;
        }
        if (!win_alive(req->hwnd)) {
            local.result = PLACE_GONE;
            goto done;
        }
        /* Some other failure with the window still alive: fall through to
         * the read-back, which is what actually decides the outcome. */
    }

    if (!win_alive(req->hwnd)) {
        local.result = PLACE_GONE;
        goto done;
    }

    get_rect(req->hwnd, &local.after);

    local.pos_error  = pos_delta(&req->target, &local.after);
    local.size_error = size_delta(&req->target, &local.after);
    local.did_resize = (rect_w(&local.after) != rect_w(&local.before) ||
                        rect_h(&local.after) != rect_h(&local.before)) ? TRUE : FALSE;

    /* ---- attempt 2: the move on its own --------------------------- */

    /*
     * Reached when a resize was requested and the position is still wrong.
     * The size change and the move are one request, so a window that rejects
     * the size may have rejected both. Retrying with SWP_NOSIZE separates the
     * two: if the move now succeeds, the window is the "move only" category
     * that stage 1 measured, and the caller should stop asking it to resize.
     */
    if (req->allow_resize && local.pos_error > tolerance) {
        SetLastError(0);
        ok = SetWindowPos(req->hwnd, NULL, want_x, want_y, 0, 0,
                          base_flags | SWP_NOSIZE);
        if (!ok) local.last_error = GetLastError();

        if (!win_alive(req->hwnd)) {
            local.result = PLACE_GONE;
            goto done;
        }

        get_rect(req->hwnd, &local.after);
        local.pos_error  = pos_delta(&req->target, &local.after);
        local.size_error = size_delta(&req->target, &local.after);
    }

    /* ---- attempt 3: the size alone, after a cross-DPI move -------- */

    /*
     * Reached when the window IS where it was asked to be but came back a
     * different size. Moving a window onto a monitor with a different DPI
     * makes Windows rescale it (WM_DPICHANGED): the size request was applied
     * in the OLD DPI context, so 600x400 asked for on a 150% monitor lands as
     * 900x600. That rescale happens once, during the move, so asking again -
     * now that the window already lives on the target monitor - lands
     * exactly.
     *
     * Measured 2026-10-08 on the mixed-DPI pair (2560x1440 @100% primary +
     * 2560x1600 @150%): a first call asking for 600x400 gave 900x600, a
     * second identical call gave 600x400. The size follows the DPI RATIO, in
     * both directions: moving back to the 100% monitor, 800x600 landed as
     * 533x400.
     *
     * This is NOT a property of the target window. It reproduces with plain
     * SetWindowPos from a separate process, under both PerMonitorV2 and
     * unaware thread contexts, and the same numbers came back for Notepad and
     * for this program's own PerMonitorV2 window.
     *
     * did_resize is required for the same reason it is used below: a window
     * that never changed size is refusing to resize, which is the
     * PLACE_MOVED_ONLY category, and asking it a second time would not change
     * that. Taken only when the size is still wrong, so the ordinary
     * same-DPI path issues no extra call at all.
     */
    if (req->allow_resize && local.did_resize &&
        local.pos_error <= tolerance && local.size_error > tolerance) {
        SetLastError(0);
        ok = SetWindowPos(req->hwnd, NULL, want_x, want_y, want_w, want_h,
                          base_flags);
        if (!ok) local.last_error = GetLastError();

        if (!win_alive(req->hwnd)) {
            local.result = PLACE_GONE;
            goto done;
        }

        get_rect(req->hwnd, &local.after);
        local.pos_error  = pos_delta(&req->target, &local.after);
        local.size_error = size_delta(&req->target, &local.after);
    }

    /* ---- decide from the read-back, not from the return value ----- */

    if (local.pos_error > tolerance) {
        /*
         * The window is alive and did not move. With a successful API call
         * and no ERROR_ACCESS_DENIED this is rare; it is reported as denied
         * because the observable outcome is the same - this process is not
         * allowed to move that window - and the advice to the user is
         * identical.
         */
        local.result = PLACE_DENIED;
        goto done;
    }

    if (!req->allow_resize || local.size_error <= tolerance) {
        local.result = PLACE_OK;
        goto done;
    }

    /*
     * Position is right, size is not.
     *
     * did_resize tells the two sub-cases apart: a window that never changed
     * size at all is refusing to be resized and should be locked, whereas one
     * that changed to a different size enforces its own limits and is worth
     * asking again next time.
     */
    local.result = local.did_resize ? PLACE_CLAMPED : PLACE_MOVED_ONLY;

done:
    if (out != NULL) {
        *out = local;
        out->fg_after = GetForegroundWindow();
    }
    return local.result;
}
