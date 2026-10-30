/*
 * watchdog.h - an independent thread that cleans up the overlay when the UI
 * thread stops pumping messages.
 *
 * WHY A THREAD
 *   Everything else in this program runs on the UI thread: the message loop,
 *   the edit session, the overlay's own painting. If that thread blocks, no
 *   code in it can notice - so the check has to live on a thread of its own.
 *
 * WHEN IT RUNS
 *   Only while an edit session is active: created by watchdog_start() and
 *   joined by watchdog_stop(). The idle process therefore keeps its three
 *   threads and its 0.000 s CPU time; the extra thread exists only while the
 *   user is in edit mode, which is not an idle state.
 *
 * WHAT IT DOES ON A STALL
 *   DestroyWindow cannot be called across threads, and a window cannot be
 *   hidden while its own thread is frozen either. So the guard posts an
 *   asynchronous hide plus WM_CLOSE; both land the moment the UI thread pumps
 *   again, and watchdog_fired() tells the caller that the guard, not the user,
 *   ended the session.
 */

#ifndef WAC_WATCHDOG_H
#define WAC_WATCHDOG_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* No heartbeat for this long means the UI thread is stuck. */
#define WATCHDOG_TIMEOUT_MS 5000

/* How often the guard checks. It sleeps in WaitForSingleObject between checks. */
#define WATCHDOG_POLL_MS    500

/* Start guarding `overlay`. FALSE if the thread or its event cannot be made. */
BOOL watchdog_start(HWND overlay);

/* End the guard. Safe to call when it is not running. */
void watchdog_stop(void);

/* Heartbeat: the UI thread calls this once per message-loop turn. */
void watchdog_tick(void);

/* TRUE once the guard has fired for the current session; cleared by start(). */
BOOL watchdog_fired(void);

/*
 * The boundary rule, exposed so it can be asserted without making the UI
 * thread stall: fire only when the gap is STRICTLY greater than the timeout,
 * and never when the clock appears to have moved backwards.
 */
BOOL watchdog_should_fire(ULONGLONG now, ULONGLONG last_tick, DWORD timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* WAC_WATCHDOG_H */
