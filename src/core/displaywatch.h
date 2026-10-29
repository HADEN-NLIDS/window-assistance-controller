/*
 * displaywatch.h - noticing that the monitor topology changed.
 *
 * WHAT PROBLEM THIS SOLVES
 *
 *   Windows broadcasts WM_DISPLAYCHANGE whenever the display configuration
 *   moves: a resolution change, a monitor plugged or unplugged, a driver
 *   reset, a KVM switch, waking from sleep. Stage 7 has to react to those
 *   events, and the reaction must not be "re-evaluate everything, immediately,
 *   once per message" - because a single driver reset produces a BURST of
 *   them, and because WM_DISPLAYCHANGE is a BROADCAST.
 *
 *   A broadcast is a SendMessage(HWND_BROADCAST, ...). That means the sender -
 *   the display settings app, the driver, the shell - BLOCKS inside its own
 *   SendMessage until every top-level window has returned from its handler.
 *   Anything expensive in that handler is felt by the user as a frozen dialog.
 *   This module therefore splits the work in two:
 *
 *     the handler        record the time, arm/reset one timer, return
 *     the settled pass   enumerate monitors, compare, re-evaluate every slot
 *
 *   The split is not an optimisation. It is the reason the handler is allowed
 *   to exist at all: enumerating monitors reads each monitor's EDID out of the
 *   registry, and doing that inside a broadcast handler stalls the very dialog
 *   the user is dragging.
 *
 * WHY THE DEBOUNCE IS A PURE STATE MACHINE
 *
 *   "Did the topology stop moving?" is arithmetic on timestamps, so it is
 *   written as arithmetic on timestamps: no timer is created here, no message
 *   is pumped, and `now` is a parameter. t60_displaychange checks 300 ms,
 *   500 ms, a three-message burst, and a burst that keeps being extended -
 *   cases that cannot be produced on demand by unplugging a cable.
 *
 *   The window, the timer and the log all live in main.c; this file decides
 *   only WHEN the settled pass is due and WHAT changed when it runs.
 *
 * WHAT IT DELIBERATELY DOES NOT DO
 *
 *   It does not write configuration. Stage 7's first rule is that a monitor
 *   going away is an ENVIRONMENT state, not a user intent: the slots are
 *   re-evaluated and reported, and nothing is persisted until the user says so
 *   (the grace period and the conflict prompt are task 7.2/7.3).
 */

#ifndef WAC_DISPLAYWATCH_H
#define WAC_DISPLAYWATCH_H

#include <windows.h>

#include "monitor.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How long the topology has to stay still before it is believed.
 *
 * A driver reset or a mode switch delivers several WM_DISPLAYCHANGE messages
 * over a few hundred milliseconds. Reacting to the first one means acting on a
 * half-built topology - and then acting again on the next one.
 */
#define DISPLAYWATCH_DEBOUNCE_MS 500

/* ------------------------------------------------------------------ */
/* The debounce                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    BOOL  pending;          /* a change was seen and has not been consumed */
    DWORD last_change_ms;   /* when the most recent one arrived          */
    int   messages;         /* how many arrived for this pending change  */
    int   settled;          /* diagnostics: settled passes that have run  */
} DisplayWatch;

void displaywatch_init(DisplayWatch *dw);

/*
 * A WM_DISPLAYCHANGE arrived at `now_ms` (GetTickCount, so it wraps with the
 * system uptime - comparisons are differences, never absolute values).
 *
 * Rearms the window: a message that arrives while one is already pending
 * extends the quiet period rather than shortening it.
 */
void displaywatch_note_change(DisplayWatch *dw, DWORD now_ms);

/* Has the topology been quiet for DISPLAYWATCH_DEBOUNCE_MS? */
BOOL displaywatch_due(const DisplayWatch *dw, DWORD now_ms);

/*
 * How long until it will be due; 0 when nothing is pending or it already is.
 *
 * The timer is armed with THIS value rather than with the constant, so a
 * burst cannot make the pass run early: the timer and the decision to run
 * cannot disagree, because they read the same number.
 */
DWORD displaywatch_ms_until_due(const DisplayWatch *dw, DWORD now_ms);

/*
 * Consume the pending change. Returns FALSE when there was nothing pending -
 * which is also what a second call returns, so the caller cannot run the
 * settled pass twice for one change.
 */
BOOL displaywatch_take(DisplayWatch *dw);

/* ------------------------------------------------------------------ */
/* What changed                                                        */
/* ------------------------------------------------------------------ */

/*
 * The four ways a topology can differ, counted rather than flagged: the log
 * line and the future conflict prompt both need "how many", and a single
 * `changed` boolean would throw that away.
 */
typedef struct {
    BOOL same;          /* nothing differed - the message was not for us */
    int  old_count;
    int  new_count;
    int  removed;       /* was there, is not now                       */
    int  added;         /* is there, was not                           */
    int  renamed;       /* same panel, different \\.\DISPLAYn           */
    int  geometry_only; /* same panel, resolution or origin moved      */
} DisplayDiff;

/*
 * Compare two monitor sets.
 *
 * Identity is the device name first and the EDID name second - the two levels
 * that survive their respective operations (measured on real hardware).
 * GEOMETRY IS NOT AN IDENTITY HERE, and
 * that is the whole point of the `geometry_only` count: a resolution change
 * must be reported as a change, while monitor_match() is free to treat equal
 * geometry as evidence when it is looking for a slot's monitor.
 *
 * PURE: both sets are parameters. Returns FALSE on NULL input or a bad count.
 */
BOOL displaywatch_diff(const MonitorFingerprint *old_set, int old_count,
                       const MonitorFingerprint *new_set, int new_count,
                       DisplayDiff *out);

/* ------------------------------------------------------------------ */
/* Live plumbing (a window, so it is in main.c's domain)               */
/* ------------------------------------------------------------------ */

/*
 * The listener window must be a hidden TOP-LEVEL window, not a message-only
 * one. WM_DISPLAYCHANGE is broadcast to top-level windows only, and a
 * message-only window (HWND_MESSAGE) is documented as receiving no broadcasts
 * and cannot even be found by FindWindow. MEASURED 2026-10-09 with
 * t60_displaychange: one synthetic SendMessage(HWND_BROADCAST,
 * WM_DISPLAYCHANGE) reached a hidden top-level window and did not reach a
 * message-only window in the same process.
 *
 * main.c creates that window (WS_POPUP, WS_EX_TOOLWINDOW, never shown, zero
 * sized) and owns the timer; this module owns the arithmetic.
 */

/* ------------------------------------------------------------------ */
/* The grace period (task 7.2)                                         */
/* ------------------------------------------------------------------ */

/*
 * How long a broken topology is given to come back before the program accepts
 * it as the new reality.
 *
 * Display changes are mostly TRANSIENT. A KVM switch, a driver reset, a
 * resolution change, a monitor waking up, a cable nudged - each one produces a
 * topology that is missing a monitor for a moment and then has it again.
 * Reacting to the first snapshot means telling the user "your preset is
 * unusable" about a screen that is already coming back, and doing it again the
 * next time they touch the cable. Sixty seconds of silence absorbs all of it.
 *
 * Nothing is written either way: the grace period only decides what the
 * program SAYS and which state it settles on. See stage 7's first rule.
 */
#define DISPLAYWATCH_GRACE_MS 60000

/*
 * The grace state machine - pure, like the debounce above and for the same
 * reason: the case that matters ("the monitor came back 30 seconds later") is
 * not something a desk can arrange on demand.
 *
 * The deadline does NOT move. A topology that stays broken keeps counting down
 * from the moment it first broke; only a recovery clears it. That is the
 * opposite of the debounce, on purpose: there, the question is "has it stopped
 * moving", here it is "has it been gone long enough to mean something".
 */
typedef struct {
    BOOL  armed;
    DWORD armed_ms;    /* when the topology first went unusable */
    int   orphans;     /* slots unusable at that moment             */
    int   expired;     /* diagnostics: grace periods that ran out   */
    int   recovered;   /* diagnostics: recoveries inside the window */
} DisplayGrace;

void displaywatch_grace_init(DisplayGrace *g);

/*
 * Start waiting. Does nothing when a wait is already running - the first
 * deadline is the one that counts.
 */
void displaywatch_grace_arm(DisplayGrace *g, DWORD now_ms, int orphans);

/* The topology came back. Returns FALSE when nothing was being waited for. */
BOOL displaywatch_grace_clear(DisplayGrace *g);

/*
 * The wait ran out. Returns FALSE when nothing was being waited for.
 *
 * Distinct from clear() on purpose: the counters and the log have to be able
 * to tell "it came back in time" from "it did not", and a single stop()
 * would erase that difference at exactly the place that reports it.
 */
BOOL displaywatch_grace_expire(DisplayGrace *g);

BOOL displaywatch_grace_armed(const DisplayGrace *g);

/* Has the wait run out? FALSE when nothing is armed. */
BOOL displaywatch_grace_due(const DisplayGrace *g, DWORD now_ms);

/* Time left, 0 when nothing is armed or it has run out. Wraps like the rest. */
DWORD displaywatch_grace_ms_left(const DisplayGrace *g, DWORD now_ms);

/* How long the wait has been running - for the log line that says what
 * happened and when. 0 when nothing is armed. */
DWORD displaywatch_grace_elapsed_ms(const DisplayGrace *g, DWORD now_ms);

#ifdef __cplusplus
}
#endif

#endif /* WAC_DISPLAYWATCH_H */
