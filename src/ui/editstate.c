/*
 * editstate.c - the edit session and its single release point.
 *
 * See editstate.h for why this module exists. The short version: overlay.c
 * owns a window, this owns a MODE, and every resource that lives only while
 * that mode is active is destroyed in exactly one function below.
 */

#include "editstate.h"

#include "../util/log.h"

/*
 * One session at a time.
 *
 * A file-scope singleton rather than a handle passed around, because the whole
 * point of release_edit_resources() is that there is exactly one of these and
 * exactly one place that cleans it up. A caller able to hold two of them is a
 * caller able to leak one.
 */
static struct {
    BOOL        active;
    Overlay    *overlay;
    OverlayExit reason;
    int         fuse_seconds;
    BOOL        fuse_armed;
    /*
     * The id SetTimer ACTUALLY returned, which is not necessarily the one that
     * was requested. KillTimer has to be given the real one: killing the
     * requested id when the system allocated a different one leaves the timer
     * alive, and a live thread timer holds a USER object until the process
     * ends.
     *
     * MEASURED 2026-09-22: exactly one USER object per edit session, found by
     * tests/t51_release.c splitting a cycle into stages (create+show gained 11
     * objects, destroy returned 10) and by comparing against t50_overlay,
     * which drives overlay.c directly and leaked one object in fifty cycles
     * rather than fifty.
     */
    UINT_PTR    fuse_timer_id;
    int         entries;
    int         exits;
} g_edit;

BOOL edit_is_active(void)
{
    return g_edit.active;
}

void edit_set_name(const wchar_t *name, OverlayRenameFn on_rename, void *ctx)
{
    /*
     * Passed straight through to the overlay, which owns the box. The session
     * deliberately keeps no copy: the overlay is the only thing that draws the
     * name and the only thing that can change it, so a second copy here could
     * only ever disagree with it.
     */
    if (!g_edit.active || g_edit.overlay == NULL) return;
    overlay_set_name(g_edit.overlay, name, on_rename, ctx);
}

void edit_set_sidebar(const OverlaySidebarSource *src, OverlayDropFn on_drop,
                      void *ctx)
{
    /*
     * Straight through to the overlay, like edit_set_name() above - and for
     * the same reason: the overlay is the only thing that draws the list, so a
     * copy kept here could only ever disagree with it.
     *
     * Called more than once per session on purpose: the list may still be
     * indexing when the session starts, and this is how the finished one
     * arrives.
     */
    if (!g_edit.active || g_edit.overlay == NULL) return;
    overlay_set_sidebar(g_edit.overlay, src, on_drop, ctx);
}

void edit_set_slots(const OverlaySlotSource *src)
{
    /* Straight through, like the two above - the overlay is the only thing
     * that draws the boxes. */
    if (!g_edit.active || g_edit.overlay == NULL) return;
    overlay_set_slots(g_edit.overlay, src);
}

void edit_set_taskbar_guide(BOOL on)
{
    if (!g_edit.active || g_edit.overlay == NULL) return;
    overlay_set_taskbar_guide(g_edit.overlay, on);
}

BOOL edit_enter(HINSTANCE hinst, int monitor_index, int fuse_seconds)
{
    if (g_edit.active) {
        log_info(L"edit: refused - a session is already running");
        return FALSE;
    }

    int fuse = (fuse_seconds > 0) ? fuse_seconds : EDIT_FUSE_DEFAULT_SECONDS;
    if (fuse < EDIT_FUSE_MIN_SECONDS) {
        fuse = EDIT_FUSE_MIN_SECONDS;
    }

    /*
     * No exit callback is passed. The session asks the overlay how it ended
     * rather than being pushed at, which keeps every decision about ending a
     * session in edit_poll() - one path, not four.
     */
    Overlay *o = overlay_create(hinst, monitor_index, NULL, NULL);
    if (o == NULL) {
        log_info(L"edit: refused - the overlay could not be created");
        return FALSE;
    }

    if (!overlay_show(o)) {
        /* Created but not showable. Release it here rather than leaving a
         * hidden overlay owned by a session that never started. */
        overlay_destroy(o);
        log_info(L"edit: refused - the overlay could not be shown");
        return FALSE;
    }

    g_edit.overlay      = o;
    g_edit.reason       = OVERLAY_EXIT_NONE;
    g_edit.fuse_seconds = fuse;
    g_edit.active       = TRUE;
    g_edit.entries++;

    /*
     * Arm the fuse last, so a session that failed to start cannot leave a
     * timer running against nothing.
     */
    const UINT_PTR timer = SetTimer(NULL, EDIT_FUSE_TIMER_ID,
                                    (UINT)fuse * 1000u, NULL);
    if (timer != 0) {
        /* Keep the id the system handed back, not the one asked for. */
        g_edit.fuse_timer_id = timer;
        g_edit.fuse_armed    = TRUE;
    } else {
        /*
         * Not fatal, but it has to be said. Without the fuse the user has one
         * fewer way out, and the ones that remain are the ones that need a
         * working keyboard - which is precisely the thing most likely to be
         * unavailable when a full-screen window is involved.
         */
        log_info(L"edit: WARNING the fuse could not be armed");
    }

    log_info(L"edit: session %d started on monitor %d, fuse %d s",
             g_edit.entries, monitor_index, fuse);
    return TRUE;
}

void edit_request_exit(OverlayExit why)
{
    if (!g_edit.active || g_edit.overlay == NULL) {
        return;
    }
    overlay_request_exit(g_edit.overlay, why);
}

BOOL edit_handle_message(const MSG *msg)
{
    if (!g_edit.active || msg == NULL) {
        return FALSE;
    }

    if (msg->message == WM_TIMER && msg->hwnd == NULL &&
        msg->wParam == (WPARAM)g_edit.fuse_timer_id) {
        log_info(L"edit: fuse expired after %d s", g_edit.fuse_seconds);
        overlay_request_exit(g_edit.overlay, OVERLAY_EXIT_FUSE);
        return TRUE;
    }
    return FALSE;
}

BOOL edit_poll(void)
{
    if (!g_edit.active || g_edit.overlay == NULL) {
        return FALSE;
    }

    const OverlayExit why = overlay_exit_reason(g_edit.overlay);
    if (why == OVERLAY_EXIT_NONE) {
        return FALSE;
    }

    /*
     * Recorded BEFORE releasing, because releasing frees the overlay that
     * knows the answer. The reason then outlives the session, which is the
     * point: the caller reads edit_exit_reason() afterwards to tell the user
     * what happened.
     */
    g_edit.reason = why;
    log_info(L"edit: session ended (%ls)", overlay_exit_name(why));

    edit_release_resources();
    return TRUE;
}

void edit_release_resources(void)
{
    /*
     * THE release point for edit mode.
     *
     * Everything that exists only while edit mode is active is torn down here,
     * in reverse order of creation, and nowhere else in the program. Stage 8
     * appends the icon cache and the thumbnail registrations to this function.
     * If you are adding a resource that lives and dies with edit mode, it goes
     * here - and tests/t51_release.c is what will notice if it does not.
     */

    if (g_edit.fuse_armed) {
        /*
         * Kill the id SetTimer returned, not the one that was requested. When
         * those differ, killing the requested id silently does nothing and the
         * timer outlives the session.
         */
        (void)KillTimer(NULL, g_edit.fuse_timer_id);
        g_edit.fuse_timer_id = 0;
        g_edit.fuse_armed    = FALSE;
    }

    if (g_edit.overlay != NULL) {
        /* overlay_destroy releases its own GDI objects and hands the keyboard
         * back to whichever window had it before. */
        overlay_destroy(g_edit.overlay);
        g_edit.overlay = NULL;
    }

    if (g_edit.active) {
        g_edit.exits++;
    }
    g_edit.active       = FALSE;
    g_edit.fuse_seconds = 0;

    /*
     * g_edit.reason is deliberately NOT cleared. It is read after the session
     * ends, which is the only moment it is worth anything.
     */
    log_info(L"edit: resources released (%d entered, %d released, %d overlay(s) still held)",
             g_edit.entries, g_edit.exits, edit_live_overlays());
}

OverlayExit edit_exit_reason(void)
{
    return g_edit.reason;
}

int edit_live_overlays(void)
{
    return (g_edit.overlay != NULL) ? 1 : 0;
}

HWND edit_overlay_hwnd(void)
{
    return (g_edit.overlay != NULL) ? overlay_hwnd(g_edit.overlay) : NULL;
}

int edit_entries(void)
{
    return g_edit.entries;
}

int edit_exits(void)
{
    return g_edit.exits;
}

int edit_fuse_seconds(void)
{
    return g_edit.fuse_seconds;
}
