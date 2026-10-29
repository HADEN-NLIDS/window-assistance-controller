/*
 * rule.h - "this program always opens HERE", remembered.
 *
 * WHAT A RULE IS
 *
 *   One executable plus one place to put it. That is the whole feature the
 *   user asked for: some windows have no position memory and open dead centre
 *   every time, so the tool writes the memory down instead.
 *
 *     [rule:notepad]
 *     exe=notepad.exe
 *     x=0.0000  y=0.0000  w=0.5000  h=1.0000
 *     ref_w=2560
 *
 *   The zone is normalized against the target monitor's rcMonitor, the same
 *   model relcoord.h uses, so a rule survives a resolution change.
 *
 * TWO KINDS OF RULE, AND WHY THE DISTINCTION MATTERS
 *
 *   Which SIZE a rule means depends on how it was created, and getting this
 *   wrong is the bug that reshaped a 16:9 window into 9:16 on a portrait
 *   panel (see docs/架构与数据结构.md 2.1.1):
 *
 *     zone rule       --zone 0,0,0.5,1
 *                     "the left half of the screen". w and h are fractions of
 *                     their own axes, so the window takes the panel's shape.
 *                     This is right for a share of the screen.
 *
 *     remember rule   --remember, no zone
 *                     "put it back the size I made it". abs_w/abs_h carry the
 *                     window's real pixel size, so it keeps its OWN shape and
 *                     is only shrunk, by one factor on both axes, if it no
 *                     longer fits.
 *
 *   One field decides which: abs_w/abs_h. Zero means "no measured size, use
 *   the fractions"; non-zero means "the user has seen this size, keep it".
 *   There is deliberately no mode flag - a flag and the numbers could
 *   disagree, and two sources of truth is how the original bug happened.
 *
 * WHY RuleSet IS A VALUE TYPE WITH A SIZING WARNING
 *
 *   AppId carries four string fields (about 1.8 KB), so one Rule is roughly
 *   2.2 KB and a full set is about 70 KB. That is small enough to be a static
 *   or heap object and far too large for a comfortable stack frame in a
 *   program that must stay small - the project has already lost a day to a
 *   26 MB struct on the stack (TRAPS.md #11).
 *
 *   It is NOT opaque, unlike StoreDocument, because 70 KB does not overflow
 *   anything by merely existing and callers benefit from being able to index
 *   it. Declare it static or allocate it; do not put it in a small stack
 *   frame next to other large objects.
 */

#ifndef WAC_RULE_H
#define WAC_RULE_H

#include <windows.h>

#include "../core/app_id.h"
#include "../core/monitor.h"   /* MonitorFingerprint - the screen a preset may pin to */
#include "../core/relcoord.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RULE_ID_MAX     64
#define RULE_LABEL_MAX  128
#define RULE_MAX        32   /* distinct applications a user may pin */

typedef struct {
    char      id[RULE_ID_MAX];      /* stable key, used in the section name */
    AppId     app;                  /* what it matches */
    RelRect   zone;                 /* where it goes, normalized */
    wchar_t   label[RULE_LABEL_MAX];/* what the user calls it, optional */
    int       enabled;              /* 0 = keep it, do not apply it */

    /*
     * The screen this preset is PINNED to, or mon_set == FALSE for "wherever
     * the window turns up" - which is what every preset written before
     * 2026-10-10 means, and is still the default.
     *
     * The same three-level fingerprint a workspace slot keeps (device name ->
     * EDID name -> geometry), and for the same measured reason: none of the
     * three survives every display change on its own (§8.10).
     *
     * WHY: a user who moves a window to the other screen, then starts the
     * program again, gets it back wherever Windows felt like opening it -
     * because the fractions are applied relative to the window's OWN monitor.
     * Reported 2026-10-10 as "it only changed the position, it did not move to
     * the screen I chose". Pinning makes "this program always opens THERE"
     * possible, and the placement paths send the window there no matter which
     * screen it appeared on.
     */
    BOOL      mon_set;
    wchar_t   mon_device[WAC_MON_NAME_MAX];        /* "\\.\DISPLAYn" at pin time */
    wchar_t   mon_friendly[WAC_MON_FRIENDLY_MAX];  /* EDID name at pin time */
    int       mon_w, mon_h;                        /* resolution at pin time */
    int       mon_x, mon_y;                        /* virtual origin at pin time */

    /*
     * The size this window was LAST SEEN at when it was placed, in pixels, or
     * 0 for a preset that has never been placed (acceptance item 9: "an
     * application that has run before shows its measured size").
     *
     * NOT zone.abs_w / zone.abs_h, and that is the whole reason these two
     * exist: those fields DECIDE WHICH HALF of section 5.2's rule a preset
     * follows - zero means "follow the panel's shape", non-zero means "keep
     * the window's own shape". Writing a fresh measurement into them would
     * silently change what the preset means the first time it ran.
     *
     * This is a measurement and nothing else. It is drawn in the placeholder
     * box, and no placement path reads it.
     */
    int       last_w, last_h;
} Rule;

typedef struct {
    Rule items[RULE_MAX];
    int  count;
} RuleSet;

/* ------------------------------------------------------------------ */
/* Basics                                                              */
/* ------------------------------------------------------------------ */

void rules_clear(RuleSet *set);
int  rules_count(const RuleSet *set);

/* NULL when the index is out of range. */
const Rule *rules_at(const RuleSet *set, int index);
Rule       *rules_at_mut(RuleSet *set, int index);

/*
 * Add the rule for an application, or replace the zone of the one that is
 * already there.
 *
 * Replacing rather than appending is the point: telling the tool "notepad
 * goes here" twice must leave ONE rule for notepad, not two that fight over
 * the window on every launch.
 *
 * Rules are matched to each other by executable (full path when both have
 * one, otherwise file name), which is the same question app_id_match answers
 * for windows.
 *
 * Returns the index of the rule, or -1 when the set is full or the app names
 * nothing. An empty AppId is refused rather than stored: app_id_match treats
 * an empty rule as matching NOTHING, so storing one would produce a rule that
 * silently never fires.
 */
int rules_upsert(RuleSet *set, const AppId *app, const RelRect *zone,
                 const wchar_t *label);

/* Remove by index. Returns TRUE when something was removed. */
BOOL rules_remove(RuleSet *set, int index);

/* Find a rule by id, or by label when the id does not match. -1 when absent.
 * Used by the command line, where a user types a name rather than an index. */
int rules_find_by_name(const RuleSet *set, const wchar_t *name);

/*
 * Index of the first ENABLED rule whose app matches this window, or -1.
 *
 * The window's exe_path must be filled in, or only the class/title parts of a
 * rule can match - pass WIN_ENUM_NEED_EXE to win_enum_top().
 */
int rules_match(const RuleSet *set, const WindowInfo *w);

/* ------------------------------------------------------------------ */
/* Applying a rule - pure                                            */
/* ------------------------------------------------------------------ */

/*
 * The physical rectangle a rule means on a given monitor.
 *
 * Pure: no window calls, no I/O, no globals. That is what lets the placement
 * rules be tested without a screen, the same way monitor_match() is.
 *
 * Position always comes from the fractions - "where on the screen" is exactly
 * what a normalized coordinate means, and it must mean the left half on a
 * panel of any shape. Size comes from abs_w/abs_h when the rule remembers
 * one, and from the fractions when it does not.
 *
 * The result is clamped to the monitor: a remembered size that no longer fits
 * is scaled down rather than allowed to hang off the panel edge.
 *
 * Returns FALSE for NULL arguments or a degenerate monitor.
 */
BOOL rule_target_rect(const Rule *rule, const RECT *rc_monitor, RECT *out);

/* ------------------------------------------------------------------ */
/* Learning that a window cannot be resized - pure                     */
/* ------------------------------------------------------------------ */

/*
 * Remember that this preset's window refuses to be resized.
 *
 * The evidence is PLACE_MOVED_ONLY, and nothing else: win_place_apply() returns
 * it only when a resize was ALLOWED and the window came back the same size
 * (win_place.h). So seeing it means the window said no - a window that never
 * changed size is exactly the "move only" category stage 1 measured.
 *
 * Until 2026-10-10 nothing ever set lock_size: the field was read, written to
 * the file, honoured by both placement paths - and never learned. win_place.h
 * said "the caller should record lock_size = 1 so it stops asking"; no caller
 * did, so every sweep asked again and the user was never told why the size
 * they typed had no effect (2026-10-10, the UU accelerator report).
 *
 * Pure: no I/O, no windows. The caller decides when the file is written - and
 * the decision itself is testable without a window that really refuses to
 * resize, which only a third-party program provides (tests/t18_workspace.c).
 *
 * Returns TRUE only when THIS call set the flag: "we just learned something"
 * as opposed to "we already knew". Only the first deserves a save and a line
 * of explanation.
 */
BOOL rule_lock_size(RuleSet *set, const char *rule_id);

/*
 * Record the size a placed window was actually seen at (acceptance item 9).
 *
 * Kept OUT of zone.abs_w/abs_h on purpose - see the note on Rule.last_w in
 * this header: those two decide what the preset MEANS, and a fresh measurement
 * written into them would change the meaning the first time the window ran.
 *
 * Pure, like rule_lock_size(), and it returns TRUE only when the number
 * actually changed: "we just learned something" is what deserves a save and a
 * line of explanation.
 */
BOOL rule_note_size(RuleSet *set, const char *rule_id, int w, int h);

/* ------------------------------------------------------------------ */
/* The screen a preset is pinned to - pure                             */
/* ------------------------------------------------------------------ */

typedef enum {
    RULE_PIN_NONE = 0,   /* not pinned: keep using the window's own monitor */
    RULE_PIN_FOUND,      /* pinned, and that screen is connected right now */
    RULE_PIN_GONE        /* pinned, but that screen is not connected */
} RulePinState;

/* TRUE when the preset names a screen at all. */
BOOL rule_is_pinned(const Rule *r);

/*
 * Copy a fingerprint into the preset's binding. fp == NULL clears it.
 *
 * Pure: the caller says WHICH monitor. Nothing here enumerates, so "pin to
 * this screen" and "unpin" are both testable without a desk full of hardware.
 */
void rule_bind_monitor(Rule *r, const MonitorFingerprint *fp);

/*
 * Resolve the binding against the current monitor set.
 *
 * *out_index receives the index into `cur` when the answer is RULE_PIN_FOUND,
 * and is set to -1 in every other case.
 *
 * The three answers are deliberately distinct: NONE means "no opinion, use the
 * window's own monitor", GONE means "the user named a screen and it is not
 * here" - and that one must NOT silently become a fallback, because quietly
 * using another screen is exactly the behaviour this feature removes.
 *
 * Pure, so the decision is testable with constructed monitors - the same
 * reason monitor_match() takes its set as a parameter.
 */
RulePinState rule_pinned_monitor(const Rule *r, const MonitorFingerprint *cur,
                                 int count, int *out_index);

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

/*
 * Section names are the contract:
 *
 *     [rule:<id>]
 *     exe=notepad.exe          file name; empty = do not compare
 *     exe_path=                full path; empty = compare by name only
 *     class=                   window class; empty = any
 *     title=                   caption substring; empty = any
 *     x, y, w, h               normalized zone, 4 decimals
 *     min_w, min_h             pixel floors
 *     abs_w, abs_h             remembered pixel size; 0 = use the fractions
 *     ref_w                    monitor width the size was measured on
 *     lock_size                1 = the window refuses resizing
 *     enabled                  1 = apply it
 *     label=                   display name, UTF-8
 *
 * Anything that does not match the pattern is ignored, so an unrelated or
 * future section cannot break loading - the same rule workspace.c follows.
 *
 * A section naming no executable and no class is SKIPPED rather than loaded,
 * because such a rule matches nothing and would be invisible dead weight in
 * the file.
 */
void rules_from_store(RuleSet *set, const void *store_doc);

/*
 * Write the set into a document, removing rule sections that are no longer
 * present. Other sections (meta, workspace:*, slot:*) are left untouched.
 */
void rules_to_store(const RuleSet *set, void *store_doc);

/* ------------------------------------------------------------------ */
/* The standard rule file                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    RULES_LOADED = 0,     /* read successfully */
    RULES_MODIFIED,       /* read successfully, but the file was last written by
                           * something other than this program - a hand edit.
                           * The contents ARE in use; the caller should say so. */
    RULES_MISSING,        /* no file yet - the normal first run, not an error */
    RULES_RECOVERED,      /* the file was damaged; a backup was loaded instead */
    RULES_UNAVAILABLE     /* no config directory, or the file could not be read */
} RulesLoadResult;

/*
 * Load the rule set from the standard config file.
 *
 * The result separates "nothing stored yet" from "could not read", which look
 * identical in the data and mean very different things to a user: one needs
 * no explanation at all, the other MUST be reported. Collapsing them into a
 * single boolean is how a corrupt file becomes "your rules are gone" with no
 * mention that anything went wrong.
 *
 * Always leaves `set` usable - empty when nothing could be read.
 */
RulesLoadResult rules_load(RuleSet *set);

/*
 * Write the rule set to the standard config file.
 *
 * Loads the file first and rewrites only the [rule:*] sections, so everything
 * the main window owns survives. Returns FALSE when the file could not be
 * written; the caller must say so rather than assume it worked, because the
 * user's next action is to close the program and expect the rule to be there.
 */
BOOL rules_save(const RuleSet *set);

/* ------------------------------------------------------------------ */
/* Naming                                                              */
/* ------------------------------------------------------------------ */

/*
 * Section-safe id from an executable name: "C:\...\Notepad.exe" -> "notepad".
 *
 * Lowercase ASCII letters, digits, '-' and '_' only, so the result can never
 * break the INI syntax or surprise someone editing the file by hand.
 * Collisions within the set get a numeric suffix.
 */
void rules_make_id(const RuleSet *set, const wchar_t *exe_name,
                   char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* WAC_RULE_H */
