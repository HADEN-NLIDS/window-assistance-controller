/*
 * winwatch.h - hear about windows appearing instead of looking for them.
 *
 * WHY THIS EXISTS
 *
 *   Stage 4 could only place a window it had started itself: `wac place
 *   --launch` knows when to look because it caused the process. The thing the
 *   user actually complained about is the other half - they double-click an
 *   icon themselves, and want THAT window put where the preset says. Nothing
 *   is watching at that moment.
 *
 *   The obvious answer, polling the window list on a timer, is ruled out twice
 *   over: it breaks the "idle CPU 0.0%" promise the whole design leans on, and
 *   a program that sweeps the process list several times a second is exactly
 *   what a security product is built to notice. Measured: a workspace run was
 *   flagged by Huorong on the user's machine, and that sweep was the reason.
 *
 *   So: SetWinEventHook, WINEVENT_OUTOFCONTEXT. The system calls US when a
 *   window is created or shown. No injection, no polling, no callback into
 *   another process.
 *
 * WHAT THE CALLBACK IS ALLOWED TO DO
 *
 *   Almost nothing. It runs on whatever thread raised the event, and every
 *   millisecond it spends is a millisecond that thread's own program is
 *   blocked. It may not open a process handle, read the window's geometry, or
 *   touch the configuration. It posts a message and returns.
 *
 *   Everything real - which process, whether a preset matches, where the
 *   window goes - happens on the main thread, which is also the only thread
 *   allowed to place a window.
 *
 * DE-DUPLICATION
 *
 *   One window appearing produces several events (create, show, and often more
 *   than one of each as child objects settle). The claim table below is how
 *   "one window, one placement" is enforced.
 */

#ifndef WAC_WINWATCH_H
#define WAC_WINWATCH_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start listening. `notify` receives `msg` once per candidate window, with the
 * HWND in wParam.
 *
 * WINEVENT_SKIPOWNPROCESS is set: this program's own windows are none of its
 * business, and hooking them would make the main window place itself.
 *
 * Returns FALSE when the hook could not be installed - the caller says so
 * rather than silently offering a feature that is not running.
 */
BOOL winwatch_start(HWND notify, UINT msg);

/* Stop listening. Safe to call when not running. */
void winwatch_stop(void);

BOOL winwatch_is_running(void);

/*
 * "Have I already dealt with this window?"
 *
 * TRUE means it is in the recent table and should be ignored. FALSE records it
 * and returns FALSE, so the caller proceeds exactly once per window.
 *
 * The table is small and fixed: a window that disappears and comes back is a
 * different window as far as placement is concerned, and only the last few
 * dozen matter anyway.
 */
BOOL winwatch_claim(HWND hwnd);

#ifdef __cplusplus
}
#endif

#endif /* WAC_WINWATCH_H */
