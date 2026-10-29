/*
 * win_enum.h - top-level window enumeration and attribute capture.
 *
 * WHAT THIS IS FOR
 *
 *   Placing a window needs three things before anything can be moved:
 *
 *     1. find the window         - EnumWindows, filtered hard enough that a
 *                                  splash screen or a tooltip cannot be
 *                                  mistaken for the application
 *     2. know whose window it is - the process and its executable, so a rule
 *                                  can be attached to an app rather than to a
 *                                  window handle that dies with the process
 *     3. know what it can do     - style bits, minimized/maximized state, and
 *                                  whether Windows considers it cloaked
 *
 *   This module answers all three and NOTHING else. It never moves a window;
 *   that is win_place.c. Keeping the read side separate is what makes the
 *   dangerous side testable: every filter below can be exercised by pointing
 *   it at windows that are already on screen.
 *
 * WHY THE FILTER IS SO PICKY
 *
 *   "Take the first window that appears with the right process id" is wrong
 *   for most real applications. Chromium, Electron, VS Code, Office and most
 *   Java apps create a small hidden or off-screen window first, then the real
 *   one; many show a splash screen. Moving the first match produces a window
 *   that jumps to the target zone and then vanishes.
 *
 *   So the filter rejects in a fixed order and REPORTS WHICH RULE REJECTED
 *   EACH WINDOW. A filter that silently drops windows is undebuggable: the
 *   user can see the window on screen and the tool insists it is not there.
 *   t30_place prints the rejected set with reasons for exactly this reason.
 *
 *   The rules, in the order they are applied, are documented on
 *   win_classify() below.
 *
 * NO ALLOCATION
 *
 *   The caller supplies the array, as with monitor_enumerate(). A WindowInfo
 *   is about 1.3 KB, so a 128-entry set is roughly 170 KB - far too much for
 *   a stack frame in a program that must stay small, and the reason this API
 *   takes a pointer instead of returning a structure by value.
 */

#ifndef WAC_WIN_ENUM_H
#define WAC_WIN_ENUM_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAC_TITLE_MAX  256
#define WAC_CLASS_MAX  128
#define WAC_EXE_MAX    260      /* MAX_PATH */

/*
 * How many windows one scan can report.
 *
 * MEASURED (2026-09-20, Windows 11, one monitor, ordinary desktop): 128 was
 * NOT enough. A scan with no filters hit the cap and reported "possibly
 * truncated". Most of those were invisible helper windows - a single
 * Electron app contributed a dozen, the shell contributed more - so the
 * count says nothing about how many applications are running.
 *
 * 256 covers that machine with headroom. The placement path filters first
 * and typically keeps under twenty, so this ceiling matters mainly to the
 * diagnostic listing; callers that enumerate unfiltered should still check
 * for a return value equal to the cap.
 */
#define WAC_WIN_MAX    256

/* ------------------------------------------------------------------ */
/* Window record                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    HWND      hwnd;
    DWORD     pid;
    LONG_PTR  style;
    LONG_PTR  ex_style;
    RECT      rect;            /* as reported by GetWindowRect, physical px */

    int       is_visible;
    int       is_iconic;       /* minimized */
    int       is_zoomed;       /* maximized */
    int       is_cloaked;      /* DWM says it is not really on screen */
    int       is_own;          /* belongs to this process - never place these */

    wchar_t   title[WAC_TITLE_MAX];
    wchar_t   class_name[WAC_CLASS_MAX];
    wchar_t   exe_path[WAC_EXE_MAX];   /* full path; empty when unavailable */
} WindowInfo;

/* ------------------------------------------------------------------ */
/* Rejection reasons                                                   */
/* ------------------------------------------------------------------ */

/*
 * Why a top-level window was not treated as an application window.
 *
 * Reported rather than discarded. The order below IS the order the rules are
 * applied, and the order matters: a cloaked window is often also invisible,
 * and reporting "not visible" when the real reason is "cloaked" sends the
 * reader down the wrong path.
 */
typedef enum {
    WIN_OK = 0,
    WIN_DEAD,             /* the handle is no longer a window at all */
    WIN_NOT_TOPLEVEL,     /* has an owner window: a dialog or a popup */
    WIN_NOT_VISIBLE,      /* IsWindowVisible is false */
    WIN_TOOLWINDOW,       /* WS_EX_TOOLWINDOW: palettes, floating toolbars */
    WIN_NOACTIVATE,       /* WS_EX_NOACTIVATE: never takes focus, not an app */
    WIN_NO_RECT,          /* GetWindowRect failed */
    WIN_TOO_SMALL,        /* below WIN_MIN_W x WIN_MIN_H */
    WIN_EMPTY_TITLE,      /* no caption text at all */
    WIN_CLOAKED,          /* DWM-cloaked: suspended UWP, other virtual desktop */
    WIN_OWN_PROCESS       /* this program's own window */
} WinFilterReason;

/* The smallest window that can plausibly be an application's main window.
 * Deliberately low: a small utility window is still a window the user may
 * want placed, and rejecting it would be the tool second-guessing the user. */
#define WIN_MIN_W 120
#define WIN_MIN_H 80

const wchar_t *win_filter_reason_name(WinFilterReason r);

/* ------------------------------------------------------------------ */
/* Classification                                                      */
/* ------------------------------------------------------------------ */

/*
 * Apply the filter rules to one window and say why it passed or failed.
 *
 * out_pid receives the owning process id (may be NULL); it is filled even
 * when the window is rejected, because a rejected window is exactly what a
 * "why is my window not listed" investigation needs to identify.
 *
 * Rules, in order - the first one that matches wins:
 *
 *   0. !IsWindow(hwnd)                    -> WIN_DEAD
 *
 *   1. GetWindow(hwnd, GW_OWNER) != NULL  -> WIN_NOT_TOPLEVEL
 *      An owned window is a dialog or a popup of another window. The user
 *      asked to place "the application", and placing its About box instead
 *      is worse than doing nothing.
 *
 *   2. !IsWindowVisible                   -> WIN_NOT_VISIBLE
 *      Hidden helper windows. Most toolkits create several per process.
 *
 *   3. WS_EX_TOOLWINDOW                   -> WIN_TOOLWINDOW
 *   4. WS_EX_NOACTIVATE                   -> WIN_NOACTIVATE
 *
 *   5. GetWindowRect fails                -> WIN_NO_RECT
 *
 *   6. smaller than WIN_MIN_W x WIN_MIN_H -> WIN_TOO_SMALL
 *      Not zero-sized: many real main windows are briefly tiny while they
 *      lay out, and a caller waiting for a window to appear must not latch
 *      onto that moment. The wait-and-recheck loop lives in win_place.c.
 *
 *   7. GetWindowTextLengthW == 0          -> WIN_EMPTY_TITLE
 *
 *   8. DWM cloaked                        -> WIN_CLOAKED
 *      A cloaked window is visible to the window manager but not to the
 *      user: a suspended UWP app, or a window on another virtual desktop.
 *      Moving it would appear to do nothing.
 *
 *   9. same process as the caller         -> WIN_OWN_PROCESS
 */
WinFilterReason win_classify(HWND hwnd, DWORD *out_pid);

/* ------------------------------------------------------------------ */
/* Enumeration                                                         */
/* ------------------------------------------------------------------ */

/*
 * Bit flags for win_enum_top().
 *
 * Zero means "no filtering at all": every top-level window is captured,
 * including 1x1 hidden helpers. That is what an investigation wants - a
 * filter that hides windows is impossible to debug when the user can see the
 * window on screen and the tool says it is not there.
 */
#define WIN_ENUM_VISIBLE_ONLY   0x0001u
#define WIN_ENUM_SKIP_OWNED     0x0002u
#define WIN_ENUM_SKIP_TOOLWIN   0x0004u
#define WIN_ENUM_SKIP_CLOAKED   0x0008u
#define WIN_ENUM_SKIP_OWN       0x0010u
#define WIN_ENUM_SKIP_SMALL     0x0020u
#define WIN_ENUM_NEED_EXE       0x0040u  /* open each process for its path */
#define WIN_ENUM_NEED_TITLE     0x0080u  /* require a non-empty caption */

/* What the placement path uses: real, user-visible application windows. */
#define WIN_ENUM_DEFAULT (WIN_ENUM_VISIBLE_ONLY | WIN_ENUM_SKIP_OWNED | \
                          WIN_ENUM_SKIP_TOOLWIN | WIN_ENUM_SKIP_CLOAKED | \
                          WIN_ENUM_SKIP_OWN | WIN_ENUM_SKIP_SMALL | \
                          WIN_ENUM_NEED_EXE | WIN_ENUM_NEED_TITLE)

/*
 * Scan top-level windows into `out`.
 *
 * Returns how many entries were written, which is at most `cap`. A return
 * value equal to `cap` means the list may have been truncated.
 *
 * The array must hold `cap` WindowInfo records. Pass WIN_ENUM_DEFAULT for
 * the placement path; pass 0 to capture everything and filter afterwards,
 * which is what an interactive "why is my window missing" listing wants.
 *
 * Order is whatever EnumWindows produces (z-order, topmost first). It is
 * stable enough for display but must never be relied on for identity - use
 * pid plus class name for that.
 */
int win_enum_top(WindowInfo *out, int cap, unsigned flags);

/*
 * Fill one WindowInfo from a handle. Used to re-read a window that was found
 * earlier, and by the wait loop that watches a window settle.
 *
 * Returns FALSE only when the handle is no longer a window; a window that
 * fails some filter is still described, with the reason available from
 * win_classify().
 */
BOOL win_fill_info(HWND hwnd, WindowInfo *out, unsigned flags);

/* ------------------------------------------------------------------ */
/* Process identity                                                    */
/* ------------------------------------------------------------------ */

/*
 * Full image path of a process, or FALSE when it cannot be read.
 *
 * Uses PROCESS_QUERY_LIMITED_INFORMATION, which succeeds on elevated
 * processes without elevation - reading the name is not a privileged act,
 * and failing here would make an elevated app look like an unknown one.
 *
 * Protected processes (anti-cheat, some DRM) refuse even that, and there is
 * no fallback: the function returns FALSE and the caller reports an unknown
 * application rather than guessing.
 */
BOOL win_exe_path(DWORD pid, wchar_t *out, size_t cch);

/* Just the file name part of a path. Returns a pointer INTO `path`, so the
 * caller must keep `path` alive; never NULL, and never past the end. */
const wchar_t *win_file_name(const wchar_t *path);

/* ------------------------------------------------------------------ */
/* State probes used by the placement path                             */
/* ------------------------------------------------------------------ */

/*
 * Is this a full-screen exclusive window?
 *
 * True when the window has no caption and no thick frame, covers its monitor
 * exactly, and is not the desktop. Games and video players report this, and
 * resizing one either does nothing or breaks its rendering, so the placement
 * path skips them and says so instead of fighting the application.
 *
 * rc_monitor is the monitor rectangle the window is on.
 */
BOOL win_is_fullscreen_exclusive(HWND hwnd, const RECT *rc_monitor);

/*
 * Does the window's style say it can be resized?
 *
 * Necessary but NOT sufficient: t03_uipi measured a window that has
 * WS_THICKFRAME and still refuses to resize, which is why stage 1's rule is
 * "read the rectangle back and compare" rather than "trust the style bits".
 * This function is a hint for reporting, never a decision.
 */
BOOL win_style_resizable(HWND hwnd);

/*
 * Is this window still alive? A named wrapper so the intent is visible at
 * the call site - a bare IsWindow() in the middle of a retry loop reads as
 * an accident.
 */
BOOL win_alive(HWND hwnd);

#ifdef __cplusplus
}
#endif

#endif /* WAC_WIN_ENUM_H */
