/*
 * pickup.c - read the current layout off the screen. See pickup.h.
 *
 * The geometry is not reimplemented here: rel_from_pixels() does the
 * normalization and app_id_from_window() builds the identifier, both of which
 * already have to be right for `wac place` and `wac run`. Doing the arithmetic
 * again in this file would be a second chance to get it wrong, and the two
 * answers would only diverge on the screens nobody tests on.
 */

#include "pickup.h"

#include "../core/app_id.h"
#include "../core/monitor.h"
#include "../core/relcoord.h"
#include "../core/win_enum.h"
#include "../util/log.h"

/*
 * Enumeration scratch.
 *
 * A WindowInfo set is a few hundred KB - each record carries a title, a class
 * name and an executable path - and TRAPS.md #11 is a 26 MB struct
 * on the stack. Static storage, touched from the UI thread only.
 */
static WindowInfo         s_wins[WAC_WIN_MAX];
static MonitorFingerprint s_mons[WAC_MON_MAX];

BOOL pickup_layout(RuleSet *rules, PickupSummary *out)
{
    PickupSummary sum;
    ZeroMemory(&sum, sizeof(sum));
    if (out != NULL) {
        ZeroMemory(out, sizeof(*out));
    }

    if (rules == NULL) {
        return FALSE;
    }

    const int mc = monitor_enumerate(s_mons, WAC_MON_MAX);
    if (mc <= 0) {
        return FALSE;
    }

    const int n = win_enum_top(s_wins, WAC_WIN_MAX, WIN_ENUM_DEFAULT);
    sum.scanned = n;
    if (n <= 0) {
        return FALSE;
    }

    /*
     * NOTE (2026-09-23): a temporary fwprintf block used to sit here, from the
     * investigation into "wac pick never returns". That conclusion was wrong -
     * wac.exe is a WINDOWS-subsystem program, so the shell does not wait for
     * it, and what looked like a hang was the main window opening afterwards.
     * The diagnostics are gone;
     * what remains is that this function is still not verified end to end.
     */
    for (int i = 0; i < n; ++i) {
        const WindowInfo *w = &s_wins[i];

        AppId app;
        ZeroMemory(&app, sizeof(app));
        if (!app_id_from_window(w, &app) || app_id_empty(&app)) {
            /* No identity means no rule could ever match this window again.
             * Counted rather than logged per window: a screen full of them
             * would otherwise produce a log nobody reads. */
            sum.skipped++;
            continue;
        }

        /*
         * Which monitor? The centre decides; a centre outside every monitor
         * falls back to the primary rather than dropping the window. Identical
         * to rule_apply_all(), and that is the point: a layout picked up here
         * must land in the same place when it is applied again.
         */
        POINT centre;
        centre.x = (w->rect.left + w->rect.right) / 2;
        centre.y = (w->rect.top + w->rect.bottom) / 2;

        int mi = monitor_at_point(s_mons, mc, (int)centre.x, (int)centre.y);
        if (mi < 0) {
            mi = monitor_primary_index(s_mons, mc);
        }

        RECT mon;
        ZeroMemory(&mon, sizeof(mon));
        if (!monitor_rect(s_mons, mc, mi, &mon)) {
            sum.skipped++;
            continue;
        }

        RelRect rel;
        ZeroMemory(&rel, sizeof(rel));

        if (w->is_zoomed) {
            /*
             * Maximized: record the whole monitor, in fractions, and do not
             * keep the measured pixels.
             *
             * MEASURED in stage 4: GetWindowRect on a maximized window returns
             * 2582x1622 on a 2560x1600 screen - the work area plus the
             * invisible resize border. Storing that would shrink the window by
             * about 1.4% on every later apply, and the user would have no way
             * to tell why. "Maximized" already has a representation in this
             * model, so it does not need a new field.
             */
            rel.x = 0.0f;
            rel.y = 0.0f;
            rel.w = 1.0f;
            rel.h = 1.0f;
            sum.maximized++;
        } else if (!rel_from_pixels(&w->rect, &mon, &rel)) {
            sum.skipped++;
            continue;
        }

        /* rules_upsert returns the index of the new or updated rule, or -1. */
        const int idx = rules_upsert(rules, &app, &rel, NULL);
        if (idx < 0) {
            sum.skipped++;
            continue;
        }

        /*
         * Acceptance item 11: a picked-up preset also learns the size the
         * window really is, so the placeholder box can say how big it will be
         * before this program has ever placed it.
         *
         * Written to last_w/last_h and NOT to zone.abs_w/abs_h: those two decide
         * WHICH of section 5.2's size rules the preset follows, and a fresh
         * measurement written there would silently change what the preset means
         * the first time it ran.
         *
         * A maximized window reports the monitor, for the same reason the zone
         * above does: GetWindowRect on a maximized window includes the invisible
         * resize border (2582x1622 on a 2560x1600 screen), and storing that
         * would make the placeholder box claim a size no window ever has.
         */
        Rule *r = rules_at_mut(rules, idx);
        if (r != NULL) {
            if (w->is_zoomed) {
                r->last_w = (int)(mon.right - mon.left);
                r->last_h = (int)(mon.bottom - mon.top);
            } else {
                r->last_w = (int)(w->rect.right - w->rect.left);
                r->last_h = (int)(w->rect.bottom - w->rect.top);
            }
        }

        sum.picked++;
    }

    log_info(L"pickup: %d scanned, %d picked (%d maximized), %d skipped",
             sum.scanned, sum.picked, sum.maximized, sum.skipped);

    if (out != NULL) {
        *out = sum;
    }
    return TRUE;
}
