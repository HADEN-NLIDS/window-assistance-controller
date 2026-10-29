/*
 * winwatch.c - the SetWinEventHook listener. See winwatch.h for the reasoning.
 *
 * The shape of this file is dictated by one rule: the callback does as little
 * as it is possible to do. Everything that costs anything lives on the main
 * thread, behind a posted message.
 */

#include "winwatch.h"

/*
 * EVENT_OBJECT_SHOW is the one worth listening to.
 *
 * EVENT_OBJECT_CREATE fires before a window has been shown and often before it
 * has been given any geometry at all, so a placement attempt made then would
 * measure a window that is still being built. SHOW happens when there is
 * something on screen to measure. Windows raise SHOW more than once for the
 * same window during start-up, which is what the claim table is for.
 *
 * The range in SetWinEventHook is inclusive and must be ordered, so the pair
 * spans CREATE..SHOW and the callback keeps only SHOW.
 */
#define WATCH_EVENT_FIRST EVENT_OBJECT_CREATE
#define WATCH_EVENT_LAST  EVENT_OBJECT_SHOW

/*
 * How many windows are remembered as "already handled".
 *
 * Only recent history matters. A window that closes and is opened again is a
 * new window as far as placement goes, and sixty-four entries is far more than
 * the number of windows that can plausibly appear between two placements.
 */
#define WATCH_SEEN_MAX 64

static HWINEVENTHOOK g_hook;
static HWND          g_notify;
static UINT          g_msg;

static HWND g_seen[WATCH_SEEN_MAX];
static UINT g_seen_next;
static UINT g_seen_count;

/*
 * The callback.
 *
 * It runs on this process's main thread, delivered through that thread's
 * message queue - which is also why the caller must have a message loop, and
 * why the main window, not some worker, is what gets the message.
 *
 * There is deliberately no window query here. GetWindowLong, GetWindowRect and
 * friends all send messages to the window's owning thread, and doing that from
 * inside an event callback means blocking the very thread that is trying to
 * finish creating the window. That is how a "helpful" tool deadlocks someone
 * else's program. Post and return.
 */
static void CALLBACK watch_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                LONG id_object, LONG id_child,
                                DWORD thread_id, DWORD time)
{
    (void)hook;
    (void)thread_id;
    (void)time;

    /*
     * OBJID_WINDOW / CHILDID_SELF means the event is about the window itself.
     * Without this the callback also fires for every menu item, button, caret
     * and text change in every program on the machine - measured in the
     * thousands per second while the user drags across a browser page.
     */
    if (id_object != OBJID_WINDOW || id_child != CHILDID_SELF) {
        return;
    }
    if (hwnd == NULL || event != EVENT_OBJECT_SHOW) {
        return;
    }
    if (g_notify == NULL) {
        return;
    }

    /* The whole job. The exit code is dropped on purpose: if the queue is
     * full the window is simply not placed, which is the same outcome as the
     * hook not existing, and is not worth a dialog. */
    (void)PostMessageW(g_notify, g_msg, (WPARAM)hwnd, 0);
}

BOOL winwatch_start(HWND notify, UINT msg)
{
    if (g_hook != NULL) {
        return TRUE;
    }
    if (notify == NULL) {
        return FALSE;
    }

    g_notify = notify;
    g_msg    = msg;

    /*
     * WINEVENT_OUTOFCONTEXT: the callback runs in our own process and no code
     * of ours is loaded into any other program.
     *
     * WINEVENT_SKIPOWNPROCESS: our own windows are not candidates. Without it
     * the main window would show up as an event and be placed against whatever
     * preset happened to match it.
     *
     * idProcess and idThread are 0 so the hook covers every process on the
     * desktop; narrowing them further would mean knowing the pid before it
     * starts, which is the thing being asked about.
     */
    g_hook = SetWinEventHook(WATCH_EVENT_FIRST, WATCH_EVENT_LAST,
                             NULL, watch_proc,
                             0, 0,
                             WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (g_hook == NULL) {
        g_notify = NULL;
        g_msg    = 0;
        return FALSE;
    }
    return TRUE;
}

void winwatch_stop(void)
{
    if (g_hook != NULL) {
        (void)UnhookWinEvent(g_hook);
        g_hook = NULL;
    }
    g_notify = NULL;
    g_msg    = 0;
    /*
     * The claim table is deliberately NOT cleared. Stopping and starting the
     * listener is what toggling it in settings does, and forgetting the
     * windows already placed would mean every window on screen gets placed
     * again the moment it is switched off and on.
     */
}

BOOL winwatch_is_running(void)
{
    return g_hook != NULL;
}

BOOL winwatch_claim(HWND hwnd)
{
    UINT i;

    if (hwnd == NULL) {
        return TRUE;    /* nothing to do, and never a fresh claim */
    }

    for (i = 0; i < g_seen_count; i++) {
        if (g_seen[i] == hwnd) {
            return TRUE;
        }
    }

    g_seen[g_seen_next] = hwnd;
    g_seen_next = (g_seen_next + 1u) % WATCH_SEEN_MAX;
    if (g_seen_count < WATCH_SEEN_MAX) {
        g_seen_count++;
    }
    return FALSE;
}
