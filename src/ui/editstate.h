/*
 * editstate.h - one edit session, and the single place its resources die.
 *
 * WHY THIS IS SEPARATE FROM overlay.c
 *
 *   overlay.c owns a window; that is all it should own. An edit session is
 *   more than a window. It is a mode the program is in, with a fuse that has
 *   to be armed, an outcome that has to be reported exactly once, and - from
 *   stage 8 - an icon cache and thumbnail registrations as well.
 *
 *   This is the most important design of stage 6:
 *   EVERY resource that exists only while edit mode is active is released in
 *   release_edit_resources(), and nowhere else. The reason is the memory
 *   budget. A brush or an icon that survives one edit session is invisible; a
 *   hundred of them is not, and by then nobody can tell which of four exit
 *   paths forgot it.
 *
 *   tests/t51_release.c is the gate that turns that from an intention into a
 *   measurement: fifty entries, GDI and USER handle growth within ten, or
 *   stage 7 does not start.
 *
 * THE FOUR WAYS OUT (task 6.8)
 *
 *   1. the global exit key - stage 2's hotkey, owned by the main window rather
 *      than by the overlay, so it keeps working while the overlay is up;
 *   2. Esc and Ctrl+Q - handled inside overlay.c, which is why the overlay
 *      takes focus (see overlay.h);
 *   3. the toolbar's exit button - task 6.3, not here yet;
 *   4. the fuse - this file. Entering edit mode arms a timer; when it expires
 *      the session ends on its own.
 *
 *   The fuse is the one that matters when everything else has gone wrong. It
 *   is the only exit that does not depend on the keyboard, on the message loop
 *   being responsive, or on the user knowing anything at all. Ten minutes is
 *   long enough to arrange a workspace and short enough that being stuck is
 *   bounded rather than open-ended.
 *
 *   The watchdog - an independent thread that force-destroys the overlay if
 *   the main loop stops pumping entirely - is a fifth mechanism and lives in
 *   core/watchdog.c. It is not implemented yet.
 */

#ifndef WAC_EDITSTATE_H
#define WAC_EDITSTATE_H

#include <windows.h>

#include "overlay.h"    /* OverlayExit, overlay_exit_name */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The fuse.
 *
 * Ten minutes by default. The floor exists because a fuse shorter than the
 * time it takes to arrange a workspace is not a safety net, it is an
 * interruption - and a user who has been interrupted by it once will turn it
 * off, which is the worst outcome for a safety mechanism.
 */
#define EDIT_FUSE_DEFAULT_SECONDS 600
#define EDIT_FUSE_MIN_SECONDS      30

/*
 * Timer id for the fuse.
 *
 * A thread timer (SetTimer(NULL, ...)), so its WM_TIMER arrives with hwnd ==
 * NULL on the main queue and edit_handle_message() can recognise it. It is
 * deliberately not a timer on the overlay window: that would need the overlay
 * to know about session policy, which is exactly the coupling this file exists
 * to avoid.
 */
#define EDIT_FUSE_TIMER_ID 2

/*
 * Begin an edit session on `monitor_index`.
 *
 * fuse_seconds is clamped up to EDIT_FUSE_MIN_SECONDS; pass 0 for the default.
 *
 * Returns FALSE - without changing any state - when a session is already
 * running, when the monitor does not exist, or when the overlay could not be
 * created. A refused entry is a reportable outcome, not a partial one: the
 * caller must never end up in edit mode with nothing on screen.
 */
BOOL edit_enter(HINSTANCE hinst, int monitor_index, int fuse_seconds);

/*
 * Ask the running session to end.
 *
 * Does nothing when no session is running. The session does NOT end here: the
 * overlay records the reason, and edit_poll() ends it on the next message, so
 * that every exit path - Esc, hotkey, fuse - converges on one place. Calling
 * this twice does not change the first reason.
 */
void edit_request_exit(OverlayExit why);

/*
 * Main-loop hooks. Call both once per iteration, in this order.
 *
 * edit_handle_message() consumes the fuse's WM_TIMER, and returns TRUE when
 * the message was the session's rather than the program's.
 * edit_poll() ends the session once the overlay has decided it is over, and
 * returns TRUE when a session ended during this call - which is the moment to
 * tell the user what happened and why.
 */
BOOL edit_handle_message(const MSG *msg);
BOOL edit_poll(void);

BOOL edit_is_active(void);

/*
 * Tell the running session what the name box shows, and where a finished
 * rename goes.
 *
 * Called after edit_enter() succeeded. Kept separate from edit_enter() because
 * the session does not care what is being named - it is the caller that knows
 * whether this is a workspace, a window preset, or nothing at all yet - and a
 * parameter that only one caller ever fills in is a parameter the next caller
 * passes wrongly.
 *
 * Does nothing when no session is running.
 */
void edit_set_name(const wchar_t *name, OverlayRenameFn on_rename, void *ctx);

/*
 * Hand the running session its application list, and where a drop goes
 * (stage 8, task 2b).
 *
 * Same reasoning as edit_set_name(): the session does not care what the list
 * is or where it came from, and the callbacks' own state belongs to the
 * caller. Called after edit_enter() succeeded, and AGAIN whenever the list
 * changes - a list that finishes indexing while the panel is already open
 * lands through this call, which is the whole reason it is separate from
 * edit_enter().
 *
 * Does nothing when no session is running.
 */
void edit_set_sidebar(const OverlaySidebarSource *src, OverlayDropFn on_drop,
                      void *ctx);

/*
 * Hand the running session the windows this workspace is meant to place
 * (stage 8, acceptance items 8 and 9).
 *
 * Same contract as edit_set_sidebar(): called when the session starts and again
 * whenever the list changes - a preset edited elsewhere, or one dragged in from
 * the sidebar, must show up on the canvas without restarting the session.
 *
 * Does nothing when no session is running.
 */
void edit_set_slots(const OverlaySlotSource *src);

/*
 * END THE SESSION. The only place edit-mode resources are released.
 *
 * Safe to call when nothing is running, and safe to call twice. Stage 8 adds
 * the icon cache and thumbnail registrations to this function; anything that
 * lives only while edit mode is active belongs in here and nowhere else.
 */
void edit_release_resources(void);

/* How the last session ended, or OVERLAY_EXIT_NONE if none has. */
OverlayExit edit_exit_reason(void);

/* ------------------------------------------------------------------ */
/* Diagnostics for t51_release                                         */
/* ------------------------------------------------------------------ */

/* Overlays this session still owns. Must be 0 outside an active session. */
int  edit_live_overlays(void);

HWND edit_overlay_hwnd(void);

/* Sessions entered and released since start-up. Both must climb together. */
int  edit_entries(void);
int  edit_exits(void);

/* The fuse actually armed, for reporting. 0 when none is armed. */
int  edit_fuse_seconds(void);

#ifdef __cplusplus
}
#endif

#endif /* WAC_EDITSTATE_H */
