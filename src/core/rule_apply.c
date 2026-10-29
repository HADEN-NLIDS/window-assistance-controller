/*
 * rule_apply.c - the shared "apply every preset" loop.
 *
 * See the header for why this exists. The logic is the same one `wac apply`
 * has always used; it was moved here so the window can use it too.
 */

#include "rule_apply.h"

#include "monitor.h"

#include <string.h>   /* _stricmp */

/*
 * Enumeration scratch.
 *
 * A WindowInfo set is a few hundred KB (256 slots, each carrying a title, a
 * class name and an executable path), and min_w/min_h lessons have been learnt
 * the hard way in this project - TRAPS.md #11 is a 26 MB struct on
 * the stack. Static storage, touched from the UI thread and the CLI thread
 * only, never concurrently.
 */
static WindowInfo         s_wins[WAC_WIN_MAX];
static MonitorFingerprint s_mons[WAC_MON_MAX];

BOOL rule_apply_ex(const RuleSet *rules, const RuleApplyOptions *opt,
                   RuleApplyFn cb, void *ctx, RuleApplySummary *out)
{
    RuleApplySummary summary;
    ZeroMemory(&summary, sizeof(summary));
    if (out != NULL) ZeroMemory(out, sizeof(*out));

    if (rules == NULL || rules_count(rules) <= 0) return FALSE;

    const char *only_id  = (opt != NULL) ? opt->only_id  : NULL;
    const int   want_mon = (opt != NULL) ? opt->monitor  : -1;
    const BOOL  move_only = (opt != NULL) ? opt->move_only : FALSE;

    const int mon_count = monitor_enumerate(s_mons, WAC_MON_MAX);
    const int n = win_enum_top(s_wins, WAC_WIN_MAX, WIN_ENUM_DEFAULT);
    summary.scanned = n;

    if (n <= 0) return FALSE;

    for (int i = 0; i < n; ++i) {
        const WindowInfo *w = &s_wins[i];

        const int ri = rules_match(rules, w);
        if (ri < 0) continue;

        const Rule *r = rules_at(rules, ri);
        if (r == NULL) continue;

        /* One preset asked for: skip the windows that belong to the others.
         * Checked AFTER the match, so the window-to-preset attribution is
         * still rules_match()'s decision and not a second rule of our own. */
        if (only_id != NULL && _stricmp(r->id, only_id) != 0) continue;

        summary.matched++;

        /*
         * Which monitor the window goes to.
         *
         * By default its centre decides; if the centre is outside every
         * monitor (a window dragged half off-screen), fall back to the primary
         * one rather than skipping the window. A caller that named a monitor
         * gets that monitor - and an index that does not exist is a failure,
         * not a silent fallback: a button that says "primary" must not quietly
         * do something else.
         */
        int mi = want_mon;
        if (mi < 0) {
            /*
             * A preset may name a screen. That answer comes FIRST, and it beats
             * the window's own position: sending the window there is the whole
             * point of pinning it (2026-10-10).
             *
             * A caller that named a screen explicitly (want_mon >= 0 - the
             * [Move to the selected monitor] button) still wins over the stored
             * pin: what the user just chose outranks what was stored earlier.
             */
            int pinned = -1;
            const RulePinState pin = rule_pinned_monitor(r, s_mons, mon_count, &pinned);

            if (pin == RULE_PIN_FOUND) {
                mi = pinned;
            } else if (pin == RULE_PIN_GONE) {
                /* It named a screen and that screen is not here. Deliberately
                 * NOT a fallback: quietly using another screen is the
                 * behaviour this feature exists to remove. */
                summary.pinned_gone++;
                if (cb != NULL) cb(w, r, NULL, PLACE_BAD_REQUEST, NULL, ctx);
                continue;
            } else {
                POINT centre;
                centre.x = (w->rect.left + w->rect.right) / 2;
                centre.y = (w->rect.top  + w->rect.bottom) / 2;

                mi = monitor_at_point(s_mons, mon_count, (int)centre.x, (int)centre.y);
                if (mi < 0) mi = monitor_primary_index(s_mons, mon_count);
            }
        } else if (mi >= mon_count) {
            summary.failed++;
            if (cb != NULL) cb(w, r, NULL, PLACE_BAD_REQUEST, NULL, ctx);
            continue;
        }

        RECT mon, target;
        ZeroMemory(&mon, sizeof(mon));
        ZeroMemory(&target, sizeof(target));

        if (!monitor_rect(s_mons, mon_count, mi, &mon) ||
            !rule_target_rect(r, &mon, &target)) {
            summary.failed++;
            if (cb != NULL) {
                cb(w, r, NULL, PLACE_BAD_REQUEST, NULL, ctx);
            }
            continue;
        }

        PlaceRequest req;
        ZeroMemory(&req, sizeof(req));
        req.hwnd         = w->hwnd;
        req.target       = target;
        /* A window that refused resizing before must only be moved now: asking
         * again would produce a "size differs" report the user cannot act on.
         * --move-only forces the same for every window. */
        req.allow_resize = (move_only || r->zone.lock_size) ? FALSE : TRUE;

        PlaceReport rep;
        ZeroMemory(&rep, sizeof(rep));
        const PlaceResult res = win_place_apply(&req, &rep);

        if (win_place_result_ok(res)) {
            summary.placed++;
        } else if (res == PLACE_DENIED) {
            summary.denied++;
        } else {
            summary.failed++;
        }

        if (cb != NULL) cb(w, r, &target, res, &rep, ctx);
    }

    if (out != NULL) *out = summary;
    return TRUE;
}

BOOL rule_apply_all(const RuleSet *rules, BOOL move_only,
                    RuleApplyFn cb, void *ctx, RuleApplySummary *out)
{
    RuleApplyOptions opt;
    ZeroMemory(&opt, sizeof(opt));
    opt.only_id   = NULL;        /* every preset */
    opt.monitor   = -1;          /* each window's own monitor */
    opt.move_only = move_only;

    return rule_apply_ex(rules, &opt, cb, ctx, out);
}

/*
 * The single-window entry point, for the resident listener.
 *
 * Deliberately a separate monitor buffer from s_mons rather than a shared one:
 * rule_apply_all is driven by a button, this is driven by a window event, and
 * the two could in principle interleave if a placement ever pumped messages.
 * The buffer is a few hundred bytes, so the clarity is free.
 */
static MonitorFingerprint s_one_mons[WAC_MON_MAX];

PlaceResult rule_apply_one(HWND hwnd, const Rule *r, RECT *out_target)
{
    if (hwnd == NULL || r == NULL || !IsWindow(hwnd)) {
        return PLACE_BAD_REQUEST;
    }

    /* The window's current rectangle, measured rather than remembered: by the
     * time the listener acts, the program has usually moved itself once or
     * twice already, and where the user's own program decided to open is what
     * picks the monitor. */
    RECT win;
    ZeroMemory(&win, sizeof(win));
    if (!GetWindowRect(hwnd, &win)) {
        return PLACE_BAD_REQUEST;
    }

    const int mon_count = monitor_enumerate(s_one_mons, WAC_MON_MAX);
    if (mon_count <= 0) {
        return PLACE_BAD_REQUEST;
    }

    /* Same rule as rule_apply_all: the centre decides, and a centre that is on
     * no monitor at all (a window opening partly off-screen) falls back to the
     * primary rather than being skipped.
     *
     * UNLESS the preset names a screen: then that screen decides, wherever the
     * window opened. This is the path a window takes when the USER starts the
     * program themselves (winwatch) - the case reported broken on 2026-10-10,
     * where the window kept the screen Windows chose for it. */
    int mi;
    int pinned = -1;
    const RulePinState pin = rule_pinned_monitor(r, s_one_mons, mon_count, &pinned);

    if (pin == RULE_PIN_FOUND) {
        mi = pinned;
    } else if (pin == RULE_PIN_GONE) {
        /* It says which screen, and that screen is not connected: leave the
         * window alone rather than moving it somewhere the user did not ask
         * for. The listener's caller reports this as a failed placement. */
        return PLACE_BAD_REQUEST;
    } else {
        POINT centre;
        centre.x = (win.left + win.right) / 2;
        centre.y = (win.top  + win.bottom) / 2;

        mi = monitor_at_point(s_one_mons, mon_count, (int)centre.x, (int)centre.y);
        if (mi < 0) mi = monitor_primary_index(s_one_mons, mon_count);
    }

    RECT mon, target;
    ZeroMemory(&mon, sizeof(mon));
    ZeroMemory(&target, sizeof(target));

    if (!monitor_rect(s_one_mons, mon_count, mi, &mon) ||
        !rule_target_rect(r, &mon, &target)) {
        return PLACE_BAD_REQUEST;
    }

    PlaceRequest req;
    ZeroMemory(&req, sizeof(req));
    req.hwnd         = hwnd;
    req.target       = target;
    /* A preset whose window refused resizing is move-only, exactly as in the
     * sweep: the listener must not be the one path that keeps asking. */
    req.allow_resize = r->zone.lock_size ? FALSE : TRUE;

    PlaceReport rep;
    ZeroMemory(&rep, sizeof(rep));
    const PlaceResult res = win_place_apply(&req, &rep);

    if (out_target != NULL) *out_target = target;
    return res;
}
