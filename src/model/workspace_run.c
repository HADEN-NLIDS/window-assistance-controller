/*
 * workspace_run.c - see workspace_run.h.
 */

#include "workspace_run.h"

#include "rule.h"

#include "../core/launch.h"
#include "../core/win_place.h"
#include "../util/strutil.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void set_note(SlotRunReport *r, const wchar_t *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(r->note, 160, _TRUNCATE, fmt, args);
    va_end(args);
}

static void trace_line(WorkspaceRunTrace trace, void *ctx,
                       const wchar_t *fmt, ...)
{
    if (trace == NULL) return;

    wchar_t line[320];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(line, 320, _TRUNCATE, fmt, args);
    va_end(args);

    trace(ctx, line);
}

const wchar_t *slot_run_result_name(SlotRunResult r)
{
    switch (r) {
    case RUN_OK:           return L"OK";
    case RUN_SKIPPED:      return L"SKIPPED";
    case RUN_NOT_FOUND:    return L"NOT_FOUND";
    case RUN_TIMEOUT:      return L"TIMEOUT";
    case RUN_DENIED:       return L"DENIED";
    case RUN_PLACE_FAILED: return L"PLACE_FAILED";
    case RUN_MONITOR_GONE: return L"MONITOR_GONE";
    default:               return L"?";
    }
}

/*
 * Place one window according to an effective rule.
 *
 * WHICH MONITOR, and why this changed on 2026-10-10:
 *
 *   A slot that records a monitor sends its window THERE - that is what
 *   "the workspace decides which screen" means, and it is what the manual has
 *   promised all along ("workstations decide where it opens"). Until this date
 *   the recorded monitor was only ever used to SKIP a slot whose panel was
 *   gone; the placement itself asked Windows where the window happened to be
 *   and applied the fractions there. The visible result was a window that
 *   moved a little and stayed on the wrong screen - reported by the user as
 *   "it only changed the position, it did not move to the screen I chose".
 *
 *   An UNBOUND slot keeps the old behaviour on purpose: a workspace written
 *   before stage 7, and a preset that never named a screen, must keep meaning
 *   "the left half of wherever the window is".
 */
static SlotRunResult place_one(HWND hwnd, const Rule *effective,
                               const MonitorFingerprint *mons, int mon_count,
                               int bound_index, PlaceReport *out)
{
    RECT mon_rect;
    ZeroMemory(&mon_rect, sizeof(mon_rect));

    /*
     * Which screen, in this order:
     *
     *   1. what the SLOT recorded - the more specific statement of the two,
     *   2. else what the PRESET is pinned to (rule.h),
     *   3. else wherever Windows opened the window - the pre-stage-7 rule.
     *
     * A slot whose screen is gone never reaches this function: start_slot()
     * reports RUN_MONITOR_GONE before the program is even started. So
     * bound_index >= 0 already means "that screen is connected".
     */
    int target_index = bound_index;

    if (target_index < 0) {
        int pinned = -1;
        const RulePinState pin = rule_pinned_monitor(effective, mons, mon_count, &pinned);

        if (pin == RULE_PIN_FOUND) {
            target_index = pinned;
        } else if (pin == RULE_PIN_GONE) {
            /* The preset names a screen and it is not here. Left alone rather
             * than placed somewhere the user did not ask for - the same
             * decision the other placement paths make. */
            return RUN_MONITOR_GONE;
        }
    }

    if (target_index >= 0 &&
        monitor_rect(mons, mon_count, target_index, &mon_rect)) {
        /* A screen was named and it is connected: the window goes there,
         * wherever Windows decided to open it. */
    } else {
        const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        ZeroMemory(&mi, sizeof(mi));
        mi.cbSize = sizeof(mi);

        if (!GetMonitorInfoW(mon, &mi)) return RUN_PLACE_FAILED;
        mon_rect = mi.rcMonitor;
    }

    RECT target;
    if (!rule_target_rect(effective, &mon_rect, &target)) {
        return RUN_PLACE_FAILED;
    }

    PlaceRequest req;
    ZeroMemory(&req, sizeof(req));
    req.hwnd         = hwnd;
    req.target       = target;
    req.allow_resize = effective->zone.lock_size ? FALSE : TRUE;
    req.tolerance_px = 0;              /* 0 selects the default */

    const PlaceResult r = win_place_apply(&req, out);

    if (win_place_result_ok(r)) return RUN_OK;
    if (r == PLACE_DENIED)      return RUN_DENIED;

    /* Everything else - a minimized window left alone, an exclusive
     * full-screen window, a window that vanished - is a placement that did
     * not happen. `win_place_result_name` puts the specific reason in the
     * note, so the report never collapses them into one word. */
    return RUN_PLACE_FAILED;
}

/*
 * Reference resolution: slot + preset -> one rule.
 *
 * A slot whose preset_id names a rule that no longer exists has lost half of
 * itself. That is reported as SKIPPED with the id in the note rather than
 * silently treated as standalone - the user's next question would be "why did
 * nothing open", and the answer has to be in the report.
 */
static BOOL resolve_slot(const Workspace *w, int i, const RuleSet *rules,
                         Rule *effective, BOOL *out_orphan)
{
    const WorkspaceSlot *slot = &w->slots[i];
    const Rule *preset = NULL;

    *out_orphan = FALSE;

    if (slot->preset_id[0] != '\0') {
        /* The slot stores the id as narrow text because that is what goes in
         * the INI key; the lookup takes a wide name, so it is converted here
         * rather than asking rule.c to carry a second string type. */
        wchar_t wide_id[WSS_PRESET_ID_MAX];
        if (str_to_wide(slot->preset_id, wide_id, WSS_PRESET_ID_MAX)) {
            const int found = rules_find_by_name(rules, wide_id);
            if (found >= 0) preset = rules_at(rules, found);
        }

        if (preset == NULL) {
            *out_orphan = TRUE;
            return FALSE;
        }
    }

    return ws_slot_effective(slot, preset, effective);
}

/* ------------------------------------------------------------------ */
/* One slot                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    Launch launch;
    Rule   effective;
    BOOL   started;
    /*
     * Which monitor this slot's window goes to: an index into the batch's
     * monitor set, or -1 for an unbound slot. Filled in by start_slot() from
     * the slot's recorded fingerprint and used by finish_slot() - the whole
     * point being that the placement no longer asks Windows where the window
     * happens to be (2026-10-10).
     */
    int    bound_monitor;
} SlotRun;

static int slot_timeout(const WorkspaceSlot *s)
{
    return (s->wait_timeout_ms > 0) ? s->wait_timeout_ms : WSS_WAIT_DEFAULT_MS;
}

/*
 * Start one slot. Returns TRUE when a process was started - the window still
 * has to be found afterwards.
 */
static BOOL start_slot(const Workspace *w, int i, const RuleSet *rules,
                       const MonitorFingerprint *mons, int mon_count,
                       SlotRun *run, SlotRunReport *rep, LaunchGroup *group,
                       WorkspaceRunTrace trace, void *ctx)
{
    const WorkspaceSlot *slot = &w->slots[i];
    BOOL orphan = FALSE;

    /*
     * Stage 7: is the monitor this slot was recorded on still connected?
     *
     * Checked BEFORE the executable is started, on purpose: starting a program
     * the workspace then cannot place leaves a window wherever Windows felt
     * like putting it, which is worse than not starting it at all.
     *
     * An UNBOUND slot - which is every slot written before stage 7 - always
     * evaluates to READY, so this block does nothing at all for a
     * configuration that predates the feature.
     */
    /*
     * The recorded monitor is RETURNED here, not merely checked: place_one()
     * needs its index to send the window there, which is what "the workspace
     * decides the screen" has always meant on paper (2026-10-10).
     * An unbound slot returns -1 and keeps the "wherever the window is"
     * behaviour.
     */
    if (ws_slot_evaluate_monitor(slot, mons, mon_count,
                                 &run->bound_monitor) == SLOT_ORPHANED) {
        rep->result = RUN_MONITOR_GONE;
        set_note(rep, L"its monitor is not connected");
        trace_line(trace, ctx, L"slot %d: MONITOR_GONE (%ls)", i, rep->note);
        return FALSE;
    }

    if (!resolve_slot(w, i, rules, &run->effective, &orphan)) {
        rep->result = RUN_SKIPPED;
        if (orphan) {
            set_note(rep, L"preset \"%hs\" no longer exists", slot->preset_id);
        } else {
            set_note(rep, L"no executable to start");
        }
        trace_line(trace, ctx, L"slot %d: SKIPPED (%ls)", i, rep->note);
        return FALSE;
    }

    if (run->effective.app.exe_path[0] == L'\0') {
        /* A preset that knows the program only by FILE NAME cannot be
         * started: there is no path to run. Reported, not guessed at. */
        rep->result = RUN_SKIPPED;
        set_note(rep, L"only the file name \"%ls\" is known",
                 run->effective.app.exe_name);
        trace_line(trace, ctx, L"slot %d: SKIPPED (%ls)", i, rep->note);
        return FALSE;
    }

    LaunchSpec spec;
    ZeroMemory(&spec, sizeof(spec));
    spec.exe_path = run->effective.app.exe_path;
    spec.args     = (slot->launch_args[0] != L'\0') ? slot->launch_args : NULL;
    spec.kind     = slot->launch_kind;
    spec.out_pid  = &rep->pid;

    /* The window search needs the file name of what is ABOUT to start, and a
     * snapshot that predates it - taken once for the whole workspace. */
    launch_prepare(&run->launch, spec.exe_path);

    HANDLE process = NULL;
    if (!launch_app(&spec, &process)) {
        rep->result = RUN_NOT_FOUND;
        set_note(rep, L"could not start \"%ls\"", spec.exe_path);
        trace_line(trace, ctx, L"slot %d: NOT_FOUND (%ls)", i, rep->note);
        return FALSE;
    }

    /*
     * Ownership of the handle moves to the group, or it is closed here. A
     * shell launch may hand back nothing at all - the program is running but
     * this process cannot end it later, which the close path reports by
     * counting fewer processes than slots.
     */
    if (process != NULL) {
        if (group != NULL) {
            launch_group_add(group, process, rep->pid);
        } else {
            CloseHandle(process);
        }
    }

    run->started = TRUE;
    trace_line(trace, ctx, L"slot %d: started pid=%lu", i, (unsigned long)rep->pid);
    return TRUE;
}

/* Wait for the started slot's window and place it. */
static void finish_slot(SlotRun *run, SlotRunReport *rep, int timeout_ms,
                        const MonitorFingerprint *mons, int mon_count,
                        WorkspaceRunTrace trace, void *ctx, int index)
{
    if (!run->started) return;
    run->started = FALSE;

    const HWND hwnd = launch_wait(&run->launch, timeout_ms, NULL, NULL);

    if (hwnd == NULL) {
        rep->result = RUN_TIMEOUT;
        set_note(rep, L"no window within %d ms", timeout_ms);
        trace_line(trace, ctx, L"slot %d: TIMEOUT", index);
        return;
    }

    rep->hwnd = hwnd;

    PlaceReport place;
    const SlotRunResult placed = place_one(hwnd, &run->effective,
                                           mons, mon_count,
                                           run->bound_monitor, &place);

    rep->result = placed;

    if (placed == RUN_OK) {
        set_note(rep, L"placed %ld,%ld %ldx%ld",
                 (long)place.after.left, (long)place.after.top,
                 (long)(place.after.right - place.after.left),
                 (long)(place.after.bottom - place.after.top));
    } else if (placed == RUN_DENIED) {
        set_note(rep, L"Windows refused (error %lu): the target runs elevated",
                 (unsigned long)place.last_error);
    } else if (placed == RUN_MONITOR_GONE) {
        /* Only reachable through the PRESET's pin: a slot whose own screen is
         * gone is caught before the program is started. */
        set_note(rep, L"the screen this preset is pinned to is not connected");
    } else {
        set_note(rep, L"placement failed (%ls)",
                 win_place_result_name(place.result));
    }

    trace_line(trace, ctx, L"slot %d: %ls - %ls", index,
               slot_run_result_name(rep->result), rep->note);
}

/* ------------------------------------------------------------------ */
/* The workspace                                                       */
/* ------------------------------------------------------------------ */

static void fill_totals(WorkspaceRunReport *rep)
{
    rep->ok = rep->denied = rep->failed = rep->skipped = 0;
    rep->monitor_gone = 0;

    for (int i = 0; i < rep->count; ++i) {
        switch (rep->slots[i].result) {
        case RUN_OK:           rep->ok++;           break;
        case RUN_DENIED:       rep->denied++;       break;
        case RUN_SKIPPED:      rep->skipped++;      break;
        case RUN_MONITOR_GONE: rep->monitor_gone++; break;
        default:               rep->failed++;       break;
        }
    }
}

WorkspaceRunReport workspace_run(const Workspace *w, const RuleSet *rules,
                                 LaunchGroup *group,
                                 WorkspaceRunTrace trace, void *ctx)
{
    WorkspaceRunReport rep;
    ZeroMemory(&rep, sizeof(rep));

    if (w == NULL || rules == NULL) return rep;

    const int n = (w->slot_count < WSS_SLOT_MAX) ? w->slot_count : WSS_SLOT_MAX;
    rep.count = n;
    if (n == 0) return rep;

    /*
     * Static, not local: a SlotRun holds a Rule (about 2.2 KB) and there are
     * up to sixteen of them - the stack is the wrong place for that
     * (TRAPS.md #11).
     */
    static SlotRun runs[WSS_SLOT_MAX];

    /* One ordered slot makes the whole workspace sequential. */
    BOOL sequential = FALSE;
    for (int i = 0; i < n; ++i) {
        if (w->slots[i].order > 0) { sequential = TRUE; break; }
    }

    /*
     * ONE snapshot for the whole batch, before anything starts. Taking one per
     * program would put the earlier programs' windows into the later
     * snapshots, and those windows could never be found afterwards.
     */
    /*
     * Stage 7: ONE monitor enumeration for the whole batch. Doing it per slot
     * would re-read every monitor's EDID up to sixteen times, and the answer
     * cannot change while a workspace is being applied.
     *
     * About 3.5 KB on the stack: MonitorFingerprint is roughly 220 bytes and
     * WAC_MON_MAX is 16. Small enough for a frame, unlike the SlotRun array
     * below.
     */
    MonitorFingerprint mons[WAC_MON_MAX];
    const int mon_count = monitor_enumerate(mons, WAC_MON_MAX);

    launch_snapshot_take();

    if (sequential) {
        trace_line(trace, ctx, L"workspace \"%hs\": %d slot(s), sequential",
                   w->id, n);

        /*
         * Ascending order, with 0 meaning "no preference" and sorted last. A
         * selection pass rather than qsort: n is at most sixteen, and this
         * keeps the configured order between equal entries.
         */
        BOOL done[WSS_SLOT_MAX];
        for (int i = 0; i < n; ++i) done[i] = FALSE;

        for (int step = 0; step < n; ++step) {
            int next = -1;
            for (int i = 0; i < n; ++i) {
                if (done[i]) continue;
                if (next < 0) { next = i; continue; }

                const int a = (w->slots[i].order    > 0) ? w->slots[i].order    : 0x7FFF;
                const int b = (w->slots[next].order > 0) ? w->slots[next].order : 0x7FFF;
                if (a < b) next = i;
            }
            if (next < 0) break;
            done[next] = TRUE;

            SlotRun *run = &runs[next];
            ZeroMemory(run, sizeof(*run));

            if (!start_slot(w, next, rules, mons, mon_count, run,
                            &rep.slots[next], group, trace, ctx)) {
                continue;
            }

            /* Sequential means exactly this: the next program does not start
             * until this one's window is where it belongs. */
            finish_slot(run, &rep.slots[next], slot_timeout(&w->slots[next]),
                        mons, mon_count, trace, ctx, next);
        }
    } else {
        trace_line(trace, ctx, L"workspace \"%hs\": %d slot(s), parallel",
                   w->id, n);

        for (int i = 0; i < n; ++i) {
            ZeroMemory(&runs[i], sizeof(runs[i]));
            start_slot(w, i, rules, mons, mon_count, &runs[i], &rep.slots[i],
                       group, trace, ctx);
        }

        for (int i = 0; i < n; ++i) {
            finish_slot(&runs[i], &rep.slots[i], slot_timeout(&w->slots[i]),
                        mons, mon_count, trace, ctx, i);
        }
    }

    fill_totals(&rep);
    trace_line(trace, ctx,
               L"workspace \"%hs\": %d placed, %d denied, %d failed, %d skipped",
               w->id, rep.ok, rep.denied, rep.failed, rep.skipped);

    /* Only when it happened. An extra line on every ordinary run would be
     * noise, and the summary line above is what other tools match on. */
    if (rep.monitor_gone > 0) {
        trace_line(trace, ctx,
                   L"workspace \"%hs\": %d slot(s) skipped - monitor not connected",
                   w->id, rep.monitor_gone);
    }

    return rep;
}
