/*
 * rule_apply.h - "put everything back where it belongs", once.
 *
 * WHY THIS IS ITS OWN MODULE
 *
 *   Two callers need exactly this operation: `wac apply` on the command line
 *   and the [Apply all] button in the window. Before this file existed the
 *   command line had its own copy, which meant the GUI would have needed a
 *   second one - and two copies of "match a window to a preset, work out the
 *   target rectangle, place it" is how the two paths start disagreeing.
 *
 *   The loop is written once here; the CLI passes a callback that prints, the
 *   window passes NULL and reads the summary.
 *
 * WHAT IT DOES NOT DO
 *
 *   It does not load or save anything. The rule set and the monitor set belong
 *   to the caller, which is also what keeps this testable: hand it a rule set
 *   and it works the same way with or without a configuration file.
 */

#ifndef WAC_RULE_APPLY_H
#define WAC_RULE_APPLY_H

#include <windows.h>

#include "../model/rule.h"
#include "win_enum.h"
#include "win_place.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int scanned;   /* top-level windows examined */
    int matched;   /* windows a preset matched */
    int placed;    /* moved successfully (PLACE_OK / MOVED_ONLY / CLAMPED) */
    int denied;    /* the target runs elevated - Windows refused the move */
    int failed;    /* the target rectangle could not be worked out, or another
                    * non-permission failure */
    int pinned_gone; /* the preset names a screen and that screen is not
                      * connected, so the window was deliberately left where it
                      * is. Counted apart from `failed`: nothing went wrong, a
                      * screen is missing, and the user has to plug it back in
                      * (or clear the pin) - see rule.h. */
} RuleApplySummary;

/*
 * Per-window report, for callers that want to say what happened to each one.
 *
 * target is NULL when the target rectangle could not be computed, in which
 * case res is PLACE_BAD_REQUEST and rep is NULL. ctx is passed through
 * untouched.
 */
typedef void (*RuleApplyFn)(const WindowInfo *w, const Rule *r,
                            const RECT *target, PlaceResult res,
                            const PlaceReport *rep, void *ctx);

/*
 * What to sweep, and where the windows go.
 *
 * rule_apply_all() is the common case (everything, wherever the window is).
 * The window's [Move to the primary monitor] button needs one preset and one
 * fixed monitor, and it needs them WITHOUT a second copy of the loop: the
 * matching cascade, the target-rectangle arithmetic and the read-back all have
 * to stay in one place, or the two paths start disagreeing about what
 * "applying a preset" means (see the note at the top of this file).
 */
typedef struct {
    /*
     * NULL sweeps every preset. Otherwise only windows that match THIS preset
     * id are touched.
     *
     * Matching is still rules_match()'s "first preset that claims the window
     * wins": a window that belongs to an earlier preset is left alone even if
     * the named one would also match it. That is the same semantic `wac apply`
     * has, and it is what keeps "move the selected preset's windows" from
     * dragging along windows the user thinks of as belonging somewhere else.
     */
    const char *only_id;

    /*
     * -1 leaves each window on its own monitor (the default, and what every
     * existing caller wants). Any other value is an index into the monitor set
     * this function enumerates, and every matched window is placed there using
     * its preset's zone.
     *
     * The index is validated: out of range means "no such monitor", reported
     * as failure rather than silently falling back to the window's own - a
     * button that says "primary" must not quietly do something else.
     */
    int         monitor;

    BOOL        move_only;
} RuleApplyOptions;

/*
 * The loop behind both entry points. `opt` may be NULL for the defaults
 * (every preset, each window's own monitor, resize allowed).
 *
 * Same return contract as rule_apply_all().
 */
BOOL rule_apply_ex(const RuleSet *rules, const RuleApplyOptions *opt,
                   RuleApplyFn cb, void *ctx, RuleApplySummary *out);

/*
 * Put every open window that matches a preset back where the preset says.
 *
 * move_only forces "move but do not resize" for every window, which is what
 * `wac apply --move-only` asks for; a preset whose window refused resizing
 * (zone.lock_size) is move-only regardless.
 *
 * Returns FALSE when there is nothing to do - no presets, or no windows - so
 * the caller can say "no presets to apply" instead of "0 placed", which are
 * different situations for a user who just created their first preset.
 *
 * out receives the counts; it is optional.
 */
BOOL rule_apply_all(const RuleSet *rules, BOOL move_only,
                    RuleApplyFn cb, void *ctx, RuleApplySummary *out);

/*
 * Put ONE window where ONE preset says.
 *
 * This exists for the resident listener (winwatch.c). When a window appears
 * because the user started the program themselves there is no set to sweep -
 * there is one hwnd and one preset that already matched it. Enumerating every
 * window on the desktop at that moment would be both slower and wrong, since
 * the whole point is to touch only the window that just appeared and leave the
 * user's existing layout alone.
 *
 * The target rectangle is worked out exactly as in rule_apply_all - the
 * monitor comes from the window's centre, the preset's zone is resolved
 * against it - so a window placed by the listener lands where `wac apply`
 * would have put it. Both paths share the pure geometry in rule.c.
 *
 * out_target is optional and receives the computed rectangle.
 *
 * Returns the same PlaceResult vocabulary as rule_apply_all. PLACE_BAD_REQUEST
 * covers "the window is gone", "no monitor" and "the zone does not resolve"
 * alike: the caller is a background listener with no user waiting to be told
 * the difference.
 */
PlaceResult rule_apply_one(HWND hwnd, const Rule *r, RECT *out_target);

#ifdef __cplusplus
}
#endif

#endif /* WAC_RULE_APPLY_H */
