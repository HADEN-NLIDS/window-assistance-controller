/*
 * pickup.h - read a layout off the screen instead of drawing one (task 6.7).
 *
 * WHY THIS IS THE ONE WORTH BUILDING FIRST
 *
 *   分段式编程计划.md calls it the highest-value feature of stage 6, and the
 *   reason is that it removes the hard part of the whole program. Arranging a
 *   workspace by hand means deciding, for every program, which region it ought
 *   to occupy - a design task the user has to do in the abstract, on a screen
 *   that is already full of windows. Picking the layout up means they arrange
 *   their windows the way they always do, once, and the program writes down
 *   what they did.
 *
 *   "Design an arrangement" becomes "show me the one you already use."
 *
 * WHAT IT DOES NOT DO
 *
 *   It does not save. Writing the file belongs to whoever owns the rule set and
 *   the config file, and keeping it out of here means this can be tested against
 *   an in-memory RuleSet with no disk involved.
 *
 *   It does not ask which windows. Everything win_enum_top() considers an
 *   application window is taken; the filters (visible, titled, not a tool
 *   window, not ours) are the same ones `wac list` uses, so what gets picked up
 *   is exactly what the program can later place.
 */

#ifndef WAC_PICKUP_H
#define WAC_PICKUP_H

#include <windows.h>

#include "rule.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int scanned;    /* top-level windows examined                        */
    int skipped;    /* could not become a rule (no app id, no rectangle) */
    int picked;     /* rules written, new or updated                     */
    int maximized;  /* among the picked: recorded as the whole monitor   */
} PickupSummary;

/*
 * Turn the windows that are on screen right now into rules.
 *
 * Each window is normalized against the monitor its centre falls on - the same
 * rule rule_apply_all() uses - so a layout picked up here and applied later
 * lands back where it started, including across monitors.
 *
 * A maximized window is recorded as the whole monitor (0,0,1,1) rather than by
 * its measured pixels. GetWindowRect on a maximized window returns the work
 * area plus the invisible resize border, measured in stage 4 at 2582x1622 on a
 * 2560x1600 screen; storing that would make every later apply shrink the window
 * by about 1.4%, for reasons the user could not see. "Maximized" is expressible
 * in the existing model, so it needs no new field.
 *
 * Returns FALSE when there is nothing to do - no presets table, no monitors, or
 * no windows - so the caller can say "nothing to pick up" rather than "0 picked
 * up", which are different situations.
 *
 * out receives the counts; it is optional.
 */
BOOL pickup_layout(RuleSet *rules, PickupSummary *out);

#ifdef __cplusplus
}
#endif

#endif /* WAC_PICKUP_H */
