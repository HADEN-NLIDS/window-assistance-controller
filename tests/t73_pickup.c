/*
 * t73_pickup.c - why does `wac pick` not write the file? (task 6.7, acceptance 11)
 *
 * THE QUESTION THIS EXISTS TO SETTLE
 *
 *   `wac pick` reads the current screen layout into presets. It has never been
 *   seen to write the config file, and the 2026-09-22 investigation left two
 *   candidates with no way to choose between them:
 *
 *     1. pickup_layout() returns FALSE  - no rules table, no monitors, no
 *        windows at all;
 *     2. it returns TRUE with nothing picked - every window was skipped.
 *
 *   It could not choose because the observation was unreliable: wac.exe is a
 *   WINDOWS-subsystem program, so it has no console and the explanation
 *   cmd_pick() prints (cli.c:543 and :548 say exactly which of the two it was)
 *   goes nowhere. MEASURED 2026-10-11: redirecting `wac pick` to a file gives
 *   exit code 0, empty stdout, empty stderr, and an unchanged config - a
 *   combination cmd_pick() cannot actually produce, since every failure path
 *   returns 1 and the success path always calls rules_save().
 *
 *   So this program is the observer that path could not be: a CONSOLE program
 *   that calls the same function and prints the four counters it fills in.
 *   It answers the question by construction rather than by inference.
 *
 * WHY IT IS WRAPPED IN A WATCHDOG
 *
 *   The window scan opens every process on the machine to read its image path
 *   (WIN_ENUM_NEED_EXE), and this machine has 256+ top-level windows. A hang
 *   here would be a console program sitting silent with nothing in its log to
 *   say where - so the work runs on its own thread, the main thread prints a
 *   heartbeat every second and names the stage it is in, and after
 *   WATCHDOG_SECONDS the program gives up and says so. A stuck test must never
 *   become a process nobody can account for.
 */

#include "test_common.h"

#include "core/app_id.h"
#include "core/monitor.h"
#include "core/win_enum.h"
#include "model/pickup.h"
#include "model/rule.h"

/* Each test program carries its own tiny assertion helper - test_common.h gives
 * the output functions (T_KV, T_NOTE, t_out_line) but not the counting. */
static int g_pass, g_fail;

static void ok(const wchar_t *what, int cond)
{
    if (cond) { g_pass++; }
    else { g_fail++; t_out_line(L"  FAIL: %ls", what); }
}

#define WATCHDOG_SECONDS 60

/* Where the worker is, for the heartbeat. Plain ints: written by one thread,
 * read by another, and a stale value only makes the message less precise. */
enum { STAGE_START = 0, STAGE_MONITORS, STAGE_SCAN, STAGE_IDENTITY, STAGE_PICKUP, STAGE_DONE };

typedef struct {
    int          stage;

    int          monitors;
    DWORD        monitors_ms;

    int          windows;
    DWORD        scan_ms;

    int          with_identity;      /* windows app_id_from_window accepts */
    int          without_identity;
    DWORD        identity_ms;

    PickupSummary sum;
    BOOL          picked_ok;
    DWORD         pickup_ms;

    RuleSet     *rules;
} PickupJob;

static const wchar_t *stage_name(int stage)
{
    switch (stage) {
    case STAGE_MONITORS: return L"enumerating monitors";
    case STAGE_SCAN:     return L"scanning top-level windows";
    case STAGE_IDENTITY: return L"reading window identities";
    case STAGE_PICKUP:   return L"pickup_layout()";
    case STAGE_DONE:     return L"done";
    default:             return L"starting";
    }
}

/*
 * Everything that can be slow lives here, so the watchdog can watch all of it.
 *
 * The identity pass is separate from pickup_layout() on purpose: pickup_layout()
 * counts skips but does not say WHY a window was skipped, and "no usable
 * identity" is the one skip reason that would make every window disappear.
 * Running the same call here, and counting, turns that from a guess into a
 * number.
 */
static DWORD WINAPI pickup_worker(LPVOID param)
{
    PickupJob *job = (PickupJob *)param;

    static MonitorFingerprint mons[WAC_MON_MAX];
    static WindowInfo         wins[WAC_WIN_MAX];   /* hundreds of KB: static (trap 11) */

    job->stage = STAGE_MONITORS;
    DWORD t0 = GetTickCount();
    job->monitors = monitor_enumerate(mons, WAC_MON_MAX);
    job->monitors_ms = GetTickCount() - t0;

    job->stage = STAGE_SCAN;
    t0 = GetTickCount();
    job->windows = win_enum_top(wins, WAC_WIN_MAX, WIN_ENUM_DEFAULT);
    job->scan_ms = GetTickCount() - t0;

    job->stage = STAGE_IDENTITY;
    t0 = GetTickCount();
    for (int i = 0; i < job->windows; ++i) {
        AppId app;
        ZeroMemory(&app, sizeof(app));
        if (app_id_from_window(&wins[i], &app) && !app_id_empty(&app)) {
            job->with_identity++;
        } else {
            job->without_identity++;
        }
    }
    job->identity_ms = GetTickCount() - t0;

    job->stage = STAGE_PICKUP;
    t0 = GetTickCount();
    job->picked_ok = pickup_layout(job->rules, &job->sum);
    job->pickup_ms = GetTickCount() - t0;

    job->stage = STAGE_DONE;
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    /*
     * PerMonitorV2, like the product and like t22/t25: without it any monitor
     * rectangle read back here is DPI-virtualised, and pickup_layout() decides
     * which monitor a window belongs to from exactly those rectangles.
     */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t73_pickup");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t73_pickup - can the current screen layout be read into presets?");
    T_KV(L"run_at", L"%ls", t_now_string());
    T_NOTE(L"Answers the 2026-09-22 question by calling pickup_layout() directly,");
    T_NOTE(L"instead of watching the config file from outside a GUI program that");
    T_NOTE(L"has no console to report through.");
    T_NOTE(L"Watchdog: the work runs on a thread; %d s is the limit.",
           WATCHDOG_SECONDS);

    /* ~70 KB of rules plus the arrays the worker keeps: static, never a stack
     * frame (TRAPS.md #11 is a 26 MB struct on the stack). */
    static RuleSet rules;
    rules_clear(&rules);

    static PickupJob job;
    ZeroMemory(&job, sizeof(job));
    job.rules = &rules;

    T_SEC(L"Running");

    HANDLE thread = CreateThread(NULL, 0, pickup_worker, &job, 0, NULL);
    if (thread == NULL) {
        T_KV(L"VERDICT", L"FAIL - could not start the worker thread");
        t_log_close();
        return 2;
    }

    int timed_out = 0;
    for (int s = 1; s <= WATCHDOG_SECONDS; ++s) {
        if (WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0) break;
        /* A heartbeat, so that a stall is visible while it is happening rather
         * than only as a program that never printed anything. */
        T_NOTE(L"  ... still running: %2d s  (%ls)", s, stage_name(job.stage));
        if (s == WATCHDOG_SECONDS) timed_out = 1;
    }

    if (timed_out) {
        T_KV(L"VERDICT", L"TIMED OUT after %d s in stage: %ls",
             WATCHDOG_SECONDS, stage_name(job.stage));
        T_NOTE(L"The stage above is where it stopped. Nothing was killed by");
        T_NOTE(L"force - the process is exiting, which is the safe way out of a");
        T_NOTE(L"thread that may hold a process handle.");
        t_log_close();
        ExitProcess(3);
    }

    CloseHandle(thread);
    T_KV(L"work took", L"%lu ms total",
         (unsigned long)(job.monitors_ms + job.scan_ms + job.identity_ms +
                         job.pickup_ms));

    T_SEC(L"What the scan saw");
    T_KV(L"monitors", L"%d  (%lu ms)", job.monitors,
         (unsigned long)job.monitors_ms);
    T_KV(L"windows enumerated", L"%d  (%lu ms)", job.windows,
         (unsigned long)job.scan_ms);
    T_KV(L"windows with an identity", L"%d", job.with_identity);
    T_KV(L"windows without one", L"%d  (%lu ms to decide)",
         job.without_identity, (unsigned long)job.identity_ms);

    T_SEC(L"pickup_layout()");
    T_KV(L"returned", L"%ls", job.picked_ok ? L"TRUE" : L"FALSE");
    T_KV(L"scanned", L"%d", job.sum.scanned);
    T_KV(L"picked", L"%d", job.sum.picked);
    T_KV(L"maximized", L"%d", job.sum.maximized);
    T_KV(L"skipped", L"%d", job.sum.skipped);
    T_KV(L"elapsed", L"%lu ms", (unsigned long)job.pickup_ms);
    T_KV(L"rules now in the set", L"%d", rules_count(&rules));

    /*
     * The verdict the 2026-09-22 investigation could not reach.
     *
     * It is reported, not asserted: what "should" happen depends on what is on
     * screen when this runs, and a test that fails because the machine happened
     * to have no windows open would be worse than useless. The assertions below
     * cover only what is true regardless of the desktop.
     */
    T_SEC(L"Answer");
    if (!job.picked_ok) {
        T_KV(L"why", L"pickup_layout() returned FALSE");
        T_NOTE(L"FALSE means one of: no rules table (not the case here), no");
        T_NOTE(L"monitors, or no windows at all. The counters above say which:");
        T_NOTE(L"  monitors == 0  -> the monitor enumeration found nothing");
        T_NOTE(L"  windows  == 0  -> win_enum_top() found nothing to pick up");
    } else if (job.sum.picked == 0) {
        T_KV(L"why", L"it ran, but every window was skipped");
        T_NOTE(L"Every window was skipped. The identity pass above separates the");
        T_NOTE(L"reasons: %d had an identity (so they were skipped later, by the",
               job.with_identity);
        T_NOTE(L"monitor lookup or by rules_upsert), %d had none.",
               job.without_identity);
    } else {
        T_KV(L"why", L"nothing - it works");
        T_NOTE(L"%d window(s) became presets. The config file would now be",
               job.sum.picked);
        T_NOTE(L"written by cmd_pick(). If it is not, the failure is in the CLI");
        T_NOTE(L"layer, not in this function.");
    }

    /*
     * Assertions. Only things that hold on any desktop, in any window state.
     */
    ok(L"the worker reached its last stage", job.stage == STAGE_DONE);
    ok(L"the counters add up",
       job.with_identity + job.without_identity == job.windows);
    ok(L"pickup_layout() saw exactly what the scan saw",
       !job.picked_ok || job.sum.scanned == job.windows);
    ok(L"picked + skipped never exceeds what was scanned",
       job.sum.picked + job.sum.skipped <= job.sum.scanned);
    ok(L"the monitor enumeration is not silently failing here",
       job.monitors > 0);

    T_SEC(L"Result");
    T_KV(L"passed", L"%d", g_pass);
    T_KV(L"failed", L"%d", g_fail);
    if (g_fail == 0) T_KV(L"VERDICT", L"PASS - all %d assertions hold", g_pass);
    else             T_KV(L"VERDICT", L"FAIL - %d assertion(s) broken", g_fail);

    if (!no_pause) t_pause();

    t_log_close();
    return (g_fail == 0) ? 0 : 1;
}
