/*
 * displaywatch.c - the debounce and the topology diff.
 *
 * NO WIN32 CALLS except the two typedefs windows.h provides. Everything here
 * is arithmetic on numbers the caller supplies, which is what makes the
 * awkward cases testable: t60_displaychange builds bursts, wrapped tick
 * counts, renamed monitors and resolution-only changes by hand, because
 * unplugging a cable does not produce them on demand.
 *
 * See displaywatch.h for why the work is split between a cheap message handler
 * and a settled pass.
 */

#include "displaywatch.h"

#include <wchar.h>   /* wcscmp */

/* ------------------------------------------------------------------ */
/* The debounce                                                        */
/* ------------------------------------------------------------------ */

void displaywatch_init(DisplayWatch *dw)
{
    if (dw == NULL) return;

    dw->pending        = FALSE;
    dw->last_change_ms = 0;
    dw->messages       = 0;
    dw->settled        = 0;
}

void displaywatch_note_change(DisplayWatch *dw, DWORD now_ms)
{
    if (dw == NULL) return;

    dw->pending        = TRUE;
    dw->last_change_ms = now_ms;
    dw->messages++;
}

/*
 * Unsigned subtraction, so the comparison survives GetTickCount wrapping at
 * 49.7 days. `now - last` is the elapsed time even when the counter wrapped in
 * between - that is the documented reason these values are only ever compared
 * as differences.
 */
BOOL displaywatch_due(const DisplayWatch *dw, DWORD now_ms)
{
    if (dw == NULL || !dw->pending) return FALSE;

    return (DWORD)(now_ms - dw->last_change_ms) >= DISPLAYWATCH_DEBOUNCE_MS;
}

DWORD displaywatch_ms_until_due(const DisplayWatch *dw, DWORD now_ms)
{
    if (dw == NULL || !dw->pending) return 0;
    if (displaywatch_due(dw, now_ms)) return 0;

    return (DWORD)DISPLAYWATCH_DEBOUNCE_MS - (DWORD)(now_ms - dw->last_change_ms);
}

BOOL displaywatch_take(DisplayWatch *dw)
{
    if (dw == NULL || !dw->pending) return FALSE;

    dw->pending  = FALSE;
    dw->messages = 0;
    dw->settled++;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* What changed                                                        */
/* ------------------------------------------------------------------ */

/*
 * Are these two entries the same physical panel?
 *
 * Device name first, then EDID name. Both names must be non-empty to match on
 * them: two monitors whose EDID could not be read both have an empty name, and
 * "empty equals empty" would pair up two unrelated panels - the same mistake
 * monitor_match() refuses to make.
 */
static BOOL same_panel(const MonitorFingerprint *a, const MonitorFingerprint *b)
{
    if (a->device_name[0] != L'\0' &&
        wcscmp(a->device_name, b->device_name) == 0) {
        return TRUE;
    }
    if (a->friendly_name[0] != L'\0' &&
        wcscmp(a->friendly_name, b->friendly_name) == 0) {
        return TRUE;
    }
    return FALSE;
}

static BOOL same_geometry(const MonitorFingerprint *a, const MonitorFingerprint *b)
{
    return (a->width     == b->width  &&
            a->height    == b->height &&
            a->virtual_x == b->virtual_x &&
            a->virtual_y == b->virtual_y) ? TRUE : FALSE;
}

/* Was this entry renamed rather than replaced? Same EDID name, new device. */
static BOOL renamed_panel(const MonitorFingerprint *old_one,
                          const MonitorFingerprint *new_one)
{
    if (old_one->friendly_name[0] == L'\0') return FALSE;

    return (wcscmp(old_one->friendly_name, new_one->friendly_name) == 0 &&
            wcscmp(old_one->device_name,   new_one->device_name)   != 0) ? TRUE : FALSE;
}

BOOL displaywatch_diff(const MonitorFingerprint *old_set, int old_count,
                       const MonitorFingerprint *new_set, int new_count,
                       DisplayDiff *out)
{
    if (out == NULL) return FALSE;
    if (old_count < 0 || new_count < 0) return FALSE;
    if (old_count > 0 && old_set == NULL) return FALSE;
    if (new_count > 0 && new_set == NULL) return FALSE;

    ZeroMemory(out, sizeof(*out));
    out->old_count = old_count;
    out->new_count = new_count;

    /*
     * Every old entry is looked up in the new set, and every new entry in the
     * old one. The two passes answer different questions - "did this one go
     * away" and "did this one appear" - and a set that lost one and gained
     * another has to report both, which one pass cannot do.
     */
    for (int i = 0; i < old_count; ++i) {
        int found = -1;
        for (int j = 0; j < new_count; ++j) {
            if (same_panel(&old_set[i], &new_set[j])) { found = j; break; }
        }

        if (found < 0) {
            out->removed++;
        } else if (!same_geometry(&old_set[i], &new_set[found])) {
            out->geometry_only++;
        }
    }

    for (int j = 0; j < new_count; ++j) {
        int found = -1;
        for (int i = 0; i < old_count; ++i) {
            if (same_panel(&new_set[j], &old_set[i])) { found = i; break; }
        }

        if (found >= 0) {
            if (renamed_panel(&old_set[found], &new_set[j])) out->renamed++;
        } else {
            out->added++;
        }
    }

    out->same = (out->removed == 0 && out->added == 0 &&
                 out->renamed == 0 && out->geometry_only == 0) ? TRUE : FALSE;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* The grace period                                                    */
/* ------------------------------------------------------------------ */

void displaywatch_grace_init(DisplayGrace *g)
{
    if (g == NULL) return;

    g->armed    = FALSE;
    g->armed_ms = 0;
    g->orphans  = 0;
    g->expired  = 0;
    g->recovered = 0;
}

void displaywatch_grace_arm(DisplayGrace *g, DWORD now_ms, int orphans)
{
    if (g == NULL) return;

    /* Already waiting: the deadline stays where it was. A monitor that
     * disappears, comes back for a second and disappears again is ONE outage
     * as far as the user is concerned. */
    if (g->armed) {
        g->orphans = orphans;   /* but the count it reports is the current one */
        return;
    }

    g->armed    = TRUE;
    g->armed_ms = now_ms;
    g->orphans  = orphans;
}

BOOL displaywatch_grace_clear(DisplayGrace *g)
{
    if (g == NULL || !g->armed) return FALSE;

    g->armed   = FALSE;
    g->orphans = 0;
    g->recovered++;
    return TRUE;
}

BOOL displaywatch_grace_expire(DisplayGrace *g)
{
    if (g == NULL || !g->armed) return FALSE;

    g->armed   = FALSE;
    g->orphans = 0;
    g->expired++;
    return TRUE;
}

BOOL displaywatch_grace_armed(const DisplayGrace *g)
{
    return (g != NULL && g->armed) ? TRUE : FALSE;
}

BOOL displaywatch_grace_due(const DisplayGrace *g, DWORD now_ms)
{
    if (g == NULL || !g->armed) return FALSE;

    return (DWORD)(now_ms - g->armed_ms) >= DISPLAYWATCH_GRACE_MS;
}

DWORD displaywatch_grace_ms_left(const DisplayGrace *g, DWORD now_ms)
{
    if (g == NULL || !g->armed) return 0;
    if (displaywatch_grace_due(g, now_ms)) return 0;

    return (DWORD)DISPLAYWATCH_GRACE_MS - (DWORD)(now_ms - g->armed_ms);
}

DWORD displaywatch_grace_elapsed_ms(const DisplayGrace *g, DWORD now_ms)
{
    if (g == NULL || !g->armed) return 0;

    return (DWORD)(now_ms - g->armed_ms);
}
