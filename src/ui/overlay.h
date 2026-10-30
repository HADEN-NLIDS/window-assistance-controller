/*
 * overlay.h - the full-screen translucent sheet a workspace is arranged on.
 *
 * WHAT IT IS
 *
 *   One WS_POPUP window covering the rcMonitor of one monitor: the whole
 *   screen including the taskbar area, not the work area. It is drawn with
 *   LWA_ALPHA rather than per-pixel composition, and it deliberately does NOT
 *   capture what is behind it. The user sees their real windows through the
 *   wash, which is the whole point: the overlay is a coordinate system laid
 *   over reality, not a screenshot of it. Capturing the screen would cost a
 *   full-screen BitBlt plus a scale on every entry, against the memory and
 *   first-frame budgets.
 *
 * WHY THIS ONE TAKES FOCUS WHEN t05's DID NOT
 *
 *   tests/t05_topmost.c asserts that its overlay never becomes the foreground
 *   window, and it is right to. That overlay appears because a test asked for
 *   it and must not steal typing from whatever the user was doing.
 *
 *   This one is the opposite case. The user pressed a button to enter edit
 *   mode, and edit mode needs the keyboard: Esc to leave, Tab to move between
 *   regions. A WS_EX_NOACTIVATE window receives none of those - it cannot even
 *   see them. So this window does take focus, and hands focus back to the
 *   window it took it from when the session ends.
 *
 *   That is a decision, not an inconsistency, and the two cases are the two
 *   halves of the same rule: never take focus the user did not offer.
 *
 * WHAT IS DELIBERATELY ABSENT
 *
 *   No WS_EX_TRANSPARENT - the editor must receive mouse drags to draw
 *   regions. The way out is the exit key, the toolbar and the fuse, not a
 *   click that falls through to whatever is underneath.
 *
 *   No timer, no animation, no periodic redraw. Nothing repaints unless
 *   something changed, so an editor left open while the user thinks costs
 *   0.0% CPU like everything else in this program.
 *
 *   No per-monitor fan-out yet: this creates one overlay on one monitor, which
 *   is what task 6.1 asks for and what the single-monitor development machine
 *   can verify. Task 6.6 decides whether other monitors get an outline-only
 *   sheet or nothing at all.
 */

#ifndef WAC_OVERLAY_H
#define WAC_OVERLAY_H

#include <windows.h>

#include "../core/relcoord.h"   /* RelRect, for the drop callback */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Why an edit session ended. The caller reports this, because "the overlay
 * disappeared" is not an acceptable thing to leave a user guessing about -
 * especially when the exit key is a four-key combination they may have hit by
 * accident.
 */
typedef enum {
    OVERLAY_EXIT_NONE = 0,   /* still running */
    OVERLAY_EXIT_HOTKEY,     /* the global exit key */
    OVERLAY_EXIT_ESCAPE,     /* Esc inside the overlay */
    OVERLAY_EXIT_TOOLBAR,    /* the [exit] button */
    OVERLAY_EXIT_FUSE,       /* the safety fuse expired */
    OVERLAY_EXIT_SHUTDOWN,   /* the program itself is going away */
    OVERLAY_EXIT_FAILED      /* the window could not be created at all */
} OverlayExit;

const wchar_t *overlay_exit_name(OverlayExit why);

typedef struct Overlay Overlay;

/*
 * Called when the overlay decides the session is over, on the main thread,
 * from inside the overlay's own message handling.
 *
 * The callback must NOT call overlay_destroy(): this is invoked while the
 * window still exists and its message is being processed. Returning from it
 * leaves the overlay in place; the caller ends the session afterwards, on its
 * own terms, by calling overlay_destroy().
 */
typedef void (*OverlayExitFn)(OverlayExit why, void *ctx);

/*
 * Called when the user finishes renaming the thing being edited, on the main
 * thread, from inside the overlay's message handling.
 *
 * The overlay holds the name so it can draw it; it does NOT store it. Deciding
 * what a name means - which workspace, whether the answer goes to disk - is the
 * caller's business, the same division the exit callback follows.
 *
 * `new_name` is never NULL and never empty. Typing an empty name and saving is
 * treated as "no change": the design says leaving without saving discards the
 * edit, and an empty box is closer to that than to a rename.
 */
typedef void (*OverlayRenameFn)(const wchar_t *new_name, void *ctx);

/*
 * What the name box shows, and where a finished rename goes.
 *
 * `name` may be NULL or empty - a session that is not editing a named thing
 * draws an empty box. The callback may be NULL, in which case the box is still
 * shown but typing into it changes nothing that outlives the session.
 *
 * Call after overlay_create(), before overlay_show().
 */
void overlay_set_name(Overlay *o, const wchar_t *name,
                      OverlayRenameFn on_rename, void *ctx);

/* The name currently in the box. Never NULL (possibly empty). */
const wchar_t *overlay_name(const Overlay *o);

/*
 * The application list the sidebar shows (stage 8, task 2b).
 *
 * The overlay does NOT take a copy of the list. It asks for a name only for
 * the rows it is about to draw - forty-odd of them - so a list of a thousand
 * costs what a list of forty costs, and a list that finishes arriving while
 * the panel is already open is picked up without rebuilding anything.
 *
 * The callbacks are the caller's: the overlay never decides what an
 * application is, or where the list came from.
 */
typedef struct {
    void *ctx;

    /* How many rows there are to show. */
    int   count;

    /* TRUE while the list is still being built on a worker thread. The panel
     * says so rather than showing an empty list, because "nothing is
     * installed" and "not ready yet" are different answers. */
    BOOL  indexing;

    /* Row `index`'s display name. Never NULL; may be empty. Called only with
     * 0 <= index < count. */
    const wchar_t *(*name_at)(void *ctx, int index);

    /* Row `index`'s executable path, or an empty string when there is none. A
     * row that is listed but cannot be started keeps its place and says so,
     * rather than disappearing - the same rule the [More...] picker follows. */
    const wchar_t *(*path_at)(void *ctx, int index);
} OverlaySidebarSource;

/*
 * Called when the user drops an application from the sidebar onto the canvas.
 *
 * `exe_path` is that application's executable, `zone` the normalized rectangle
 * it was dropped on. The overlay writes NOTHING: what a drop means - a new
 * preset, or a move for one that already exists - is the caller's business,
 * exactly as deciding what a rename means is.
 */
typedef void (*OverlayDropFn)(const wchar_t *exe_path, const RelRect *zone,
                              void *ctx);

/*
 * Hand the session its application list, and where a drop goes.
 *
 * May be called at ANY time, including while the overlay is up with the list
 * already showing: that is how a list that finishes indexing lands on screen.
 * `src` is copied, so the caller's structure need not outlive the call - but
 * the callbacks must keep working until the next call, or until the overlay is
 * destroyed.
 *
 * A NULL `src`, or one whose name_at is NULL, removes the list, and the
 * sidebar then does not open at all: an edge strip that opens an empty panel
 * is a control promising something it cannot deliver.
 */
void overlay_set_sidebar(Overlay *o, const OverlaySidebarSource *src,
                         OverlayDropFn on_drop, void *drop_ctx);

/*
 * One placeholder: a window this workspace is meant to place (stage 8,
 * acceptance items 8 and 9).
 *
 * The editor used to draw anonymous regions - a rectangle meant "a window goes
 * here" with nothing saying which window or how big it really is. A placeholder
 * is that rectangle with an identity, so the user recognises their layout
 * instead of inferring it from shapes.
 */
typedef struct {
    char    rule_id[64];    /* which preset this box belongs to */
    wchar_t name[128];      /* what to show on the box                    */
    wchar_t path[260];      /* the executable, for the icon; may be empty */
    RelRect zone;           /* where it goes, normalized                  */
    int     last_w;         /* the size it was last placed at, or 0       */
    int     last_h;
} OverlaySlot;

/*
 * Where the placeholders come from, and where an edit goes.
 *
 * Borrowed for the duration of each call, exactly like the sidebar's rows -
 * the caller keeps ownership.
 */
typedef struct {
    void *ctx;
    int   count;

    /* May return NULL for an index it cannot describe; that row is skipped. */
    const OverlaySlot *(*slot_at)(void *ctx, int index);

    /*
     * The user finished moving or resizing box `index`, and this is the zone it
     * now describes.
     *
     * The overlay stores NOTHING and writes NOTHING: a drag is a proposal, and
     * whether it is kept - in memory only, in the file, not at all - is the
     * caller's decision. Same division the rename and drop callbacks follow.
     *
     * NULL makes the boxes display-only: they are drawn and cannot be dragged.
     */
    void (*slot_moved)(void *ctx, int index, const RelRect *zone);

    /*
     * The toolbar's [Apply] was pressed.
     *
     * Which is the caller's cue to write the boxes it has been handed out as
     * presets - the overlay does not know what a preset is. NULL leaves the
     * button greyed out, which is what it has been since edit mode existed.
     */
    void (*apply)(void *ctx);

    /*
     * The toolbar's [Reset] was pressed: throw the boxes away and load them
     * again.
     *
     * With the draft model in main.c this is one reload - and it is the answer
     * to "I dragged something and I want it back", which would otherwise mean
     * leaving edit mode and coming back in.
     *
     * NULL leaves the button greyed out.
     */
    void (*reset)(void *ctx);

    /*
     * The toolbar's [Pick up this layout] was pressed.
     *
     * The caller's cue to read the layout off the screen and store it - which
     * is the one thing this overlay cannot do for itself, because the windows
     * it would be reading are the ones underneath it.
     *
     * NULL leaves the button greyed out, which is what it has been since edit
     * mode existed: `wac pick` never wrote the file, and for a long time nobody
     * knew why (a verb whitelist was missing the word, so the command never
     * ran at all).
     */
    void (*pick)(void *ctx);

    /*
     * The user asked to remove box `index` (Delete while it is selected).
     *
     * The overlay only says WHICH box; whether that means "take it out of this
     * workspace" or something else is the caller's decision, the same division
     * slot_moved() follows. NULL leaves Delete doing nothing.
     */
    void (*deleted)(void *ctx, int index);
} OverlaySlotSource;

/*
 * Hand the session the windows this workspace is meant to place.
 *
 * May be called at any time, including while the overlay is up - the same
 * contract overlay_set_sidebar() has, and for the same reason: the caller's
 * list can change while the user is looking at it. A NULL `src`, or one whose
 * slot_at is NULL, removes the boxes.
 */
void overlay_set_slots(Overlay *o, const OverlaySlotSource *src);

/*
 * Draw the taskbar guide line and let placeholders snap to it.
 *
 * The taskbar's position is read when the overlay is created and again when it
 * moves to another monitor; this only turns the guide on or off - it is the
 * settings page's switch.
 */
void overlay_set_taskbar_guide(Overlay *o, BOOL on);

/*
 * Create the overlay on `monitor_index`.
 *
 * Returns NULL when the monitor does not exist, when the window class or the
 * window could not be created, or when there is no compatible DC for the
 * back buffer. A NULL return is a normal, reportable outcome: the caller says
 * so and does not enter edit mode, rather than entering a mode with nothing on
 * screen.
 *
 * The window is created hidden; call overlay_show() to put it up.
 */
Overlay *overlay_create(HINSTANCE hinst, int monitor_index,
                        OverlayExitFn on_exit, void *ctx);

/*
 * Show the overlay and take the keyboard.
 *
 * The window that currently has focus is remembered here and restored by
 * overlay_destroy(), so the user ends up back where they were.
 */
BOOL overlay_show(Overlay *o);

/* Hide and destroy everything the overlay owns. Safe on NULL, safe twice. */
void overlay_destroy(Overlay *o);

/*
 * End the session from outside the overlay's message handling - the global
 * hotkey arrives at the main window, not here.
 *
 * This asks the overlay to close rather than destroying it, so the outcome is
 * identical to the user pressing Esc and the exit reason is reported once, in
 * one place.
 */
void overlay_request_exit(Overlay *o, OverlayExit why);

/* Which monitor this overlay covers, or -1. */
int overlay_monitor_index(const Overlay *o);

/*
 * Move a running overlay to another monitor (2026-10-09).
 *
 * Returns FALSE for a NULL overlay, an unknown monitor, or a degenerate screen
 * rectangle - and the caller then keeps the screen it already has rather than
 * be left with a half-moved editor. Moving to the monitor it is already on
 * succeeds and does nothing.
 *
 * The SESSION IS NOT RESTARTED: the regions are normalized, so they mean the
 * same thing on the new screen. It is a move, not an exit-and-re-enter - which
 * is what makes it safe to do while the user is in the middle of arranging a
 * workspace. See overlay.c for why the window is moved in two steps.
 */
BOOL overlay_move_to_monitor(Overlay *o, int monitor_index);

/* The rectangle it covers, in physical pixels. FALSE if there is none. */
BOOL overlay_rect(const Overlay *o, RECT *out);

HWND overlay_hwnd(const Overlay *o);

/* How the session ended, or OVERLAY_EXIT_NONE while it is still running. */
OverlayExit overlay_exit_reason(const Overlay *o);

/*
 * Diagnostics for the resource-discipline test (task 6.9's t51_release).
 *
 * These exist so the numbers come from the module that owns the resources
 * rather than from the test re-deriving them, which is how a leak ends up
 * being counted correctly by neither.
 */
int overlay_gdi_objects(void);      /* objects this module currently holds */
int overlay_created_count(void);    /* overlays created since start-up */
int overlay_destroyed_count(void);  /* overlays destroyed since start-up */

/*
 * Re-assert HWND_TOPMOST if something has climbed above us.
 *
 * Called at a low rate by whoever owns the clock (task 6.6). Deliberately not
 * a timer inside this module: a full-screen window that wakes up on its own
 * schedule is exactly what the idle-CPU budget forbids.
 *
 * Returns TRUE when it actually had to raise the window, so the caller can
 * report how often the Z-order was contested.
 */
BOOL overlay_reassert_topmost(Overlay *o);

#ifdef __cplusplus
}
#endif

#endif /* WAC_OVERLAY_H */
