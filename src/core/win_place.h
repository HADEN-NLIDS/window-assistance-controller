/*
 * win_place.h - move and resize one window, then verify it actually happened.
 *
 * THE RULE THIS FILE EXISTS TO ENFORCE
 *
 *   SetWindowPos returning TRUE means Windows accepted the request, NOT that
 *   the window ended up where it was asked to be. Stage 1 measured this
 *   directly (docs/P0验证记录.md, experiment 3):
 *
 *     - a window with WS_THICKFRAME refused to resize anyway;
 *     - an elevated process (UU accelerator) returned ERROR_ACCESS_DENIED for
 *       every attempt from a normal-privilege process, 4 out of 4;
 *     - the same window, once this process was elevated, MOVED 5 out of 5
 *       times and STILL did not resize.
 *
 *   So the style bits are a hint, the return value is a hint, and the only
 *   trustworthy answer comes from reading the rectangle back afterwards. Every
 *   path in win_place.c ends in a read-back, and the result enum describes
 *   what was observed rather than what was requested.
 *
 * WHAT IT WILL NOT DO
 *
 *   - It never calls ShowWindow() to make a minimized window appear. A user
 *     who minimized something did so on purpose; restoring it because a
 *     layout was applied would be the tool overriding the user.
 *   - It never retries after ERROR_ACCESS_DENIED. Retrying a permission
 *     failure cannot succeed, and a retry loop against an elevated process is
 *     indistinguishable from an attack. The result says PLACE_DENIED and the
 *     UI explains why.
 *   - It never sets the foreground window, and passes SWP_NOACTIVATE on every
 *     SetWindowPos call. Applying a layout must not steal focus from whatever
 *     the user is typing into. The report records the foreground window
 *     before and after so a caller can assert this held.
 */

#ifndef WAC_WIN_PLACE_H
#define WAC_WIN_PLACE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Request                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    HWND  hwnd;            /* the window to place */
    RECT  target;          /* desired rectangle, PHYSICAL pixels */

    /*
     * FALSE means "move only, do not touch the size". Set from a stored
     * lock_size flag: a window that has already refused a resize once will
     * refuse again, and asking again is what produces the visible jump-then-
     * snap-back the user sees.
     */
    BOOL  allow_resize;

    /* Maximum acceptable difference between requested and achieved, in
     * pixels. 0 selects win_place_default_tolerance(). */
    int   tolerance_px;
} PlaceRequest;

/* ------------------------------------------------------------------ */
/* Result                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    PLACE_OK = 0,                /* position and size both as requested */
    PLACE_MOVED_ONLY,            /* moved correctly, size unchanged: the window
                                  * refuses to resize. Caller should record
                                  * lock_size = 1 so it stops asking. */
    PLACE_CLAMPED,               /* moved, size differs but the window did
                                  * resize to something: it enforces its own
                                  * limits. Not worth locking. */
    PLACE_DENIED,                /* Windows refused: almost always an elevated
                                  * target process (UIPI). Needs the user. */
    PLACE_GONE,                  /* the window disappeared mid-operation */
    PLACE_SKIPPED_FULLSCREEN,    /* exclusive full-screen: left alone */
    PLACE_SKIPPED_MINIMIZED,     /* minimized: left alone on purpose */
    PLACE_BAD_REQUEST            /* NULL handle or a degenerate target rect */
} PlaceResult;

typedef struct {
    PlaceResult result;

    RECT  requested;        /* what was asked for */
    RECT  before;           /* where the window was */
    RECT  after;            /* where it ended up */

    int   pos_error;        /* max |dx|,|dy| between requested and after */
    int   size_error;       /* max |dw|,|dh| between requested and after */
    BOOL  did_resize;       /* the size actually changed at all */

    DWORD last_error;       /* GetLastError from the failing call, 0 if none */

    HWND  fg_before;        /* foreground window before, for focus checks */
    HWND  fg_after;
} PlaceReport;

/*
 * Apply a placement and report what actually happened.
 *
 * Always fills `out` (when non-NULL) before returning, so a caller can log
 * the before/after rectangles even for skipped windows. `out` is zeroed
 * first, so a NULL hwnd still produces a usable report.
 *
 * Never throws, never blocks for more than the OS calls themselves. A
 * cross-process SetWindowPos is synchronous and can therefore block if the
 * target is hung; that is inherent to the API, and the callers that must not
 * block (the launch wait loop) run it on their own schedule rather than in a
 * message handler.
 */
PlaceResult win_place_apply(const PlaceRequest *req, PlaceReport *out);

/* Stable English name for logs and debug output. Never NULL. */
const wchar_t *win_place_result_name(PlaceResult r);

/*
 * Is this result a success the caller may record as "the layout was applied"?
 *
 * True for PLACE_OK, PLACE_MOVED_ONLY and PLACE_CLAMPED - in all three the
 * window ended up where it was asked to be, even if the size settled
 * differently. False for skips and failures, which must be surfaced rather
 * than counted as done.
 */
BOOL win_place_result_ok(PlaceResult r);

/*
 * The read-back tolerance in physical pixels.
 *
 * Stage 1 measured a round trip of 400x300 -> 400x300 with a deviation of
 * ZERO under PerMonitorV2, so this could be 0. It is 2 to absorb the one
 * legitimate source of drift: a window whose non-client area is recomputed by
 * DWM between the set and the read. Anything larger would start hiding real
 * refusals.
 */
int win_place_default_tolerance(void);

#ifdef __cplusplus
}
#endif

#endif /* WAC_WIN_PLACE_H */
