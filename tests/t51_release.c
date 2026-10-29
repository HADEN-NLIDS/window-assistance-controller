/*
 * t51_release.c - stage 6, task 6.9: the resource gate for edit mode.
 *
 * WHAT THIS VERIFIES
 *
 *   docs/分段式编程计划.md makes this a hard gate - "fifty entries, GDI and
 *   USER handle growth within ten, or stage 7 does not start" - and this
 *   program is the gate. It is not a demonstration and its verdict is not
 *   advisory.
 *
 *   Two things are measured that a single entry cannot show:
 *
 *   1. Growth. One leaked brush per session is invisible; fifty is not. The
 *      delta is what matters, not the absolute count.
 *
 *   2. Pairing. edit_entries() and edit_exits() must climb together. A session
 *      that starts and never releases is the failure mode that leaks, and it
 *      shows up here as a mismatch long before it shows up as missing memory.
 *
 *   It also exercises the fuse, which is the one exit path that cannot be
 *   tested by pressing a key.
 *
 * HOW TO RUN
 *     t51_release.exe                     enter/leave once, report
 *     t51_release.exe --loop 50           the gate: fifty cycles
 *     t51_release.exe --monitor 1         the second monitor
 *     t51_release.exe --fuse 35           wait for the fuse to fire (min 30 s)
 *
 * HOW TO READ A FAILURE
 *
 *   MEASURED 2026-10-11: the same binary, run nine times with --loop 50,
 *   produced gdi_delta 18, 12, and then 0 on seven consecutive runs. So this
 *   gate is neither "always fails" nor "always passes", and the number on its
 *   own does not say whether edit mode leaked anything.
 *
 *   The per-cycle lines, printed just above the result, do say. They appear
 *   only where GDI MOVES:
 *
 *     ONE large jump in a single cycle - something outside the session. This
 *     process also hosts the shell's caches and the window watcher, and those
 *     live on other threads, so GetGuiResources counts their objects too
 *     (docs/手册.md section 8.32). All three non-zero runs above happened while
 *     other test programs were being driven on the same machine.
 *
 *     MANY small steps of +1 - a real leak inside this process. That is what
 *     the gate was built to catch.
 *
 *   gdi_idle_drift is the tie-breaker: GDI is read twice, a second apart, with
 *   nothing of ours running. Non-zero there means the baseline moves on its own
 *   and the delta cannot be attributed to a session at all.
 *
 * WHY THE LOOP NEEDS NO MESSAGE PUMP
 *
 *   edit_request_exit() records the reason on the overlay, and edit_poll()
 *   releases the session as soon as it sees one. Nothing in that path waits
 *   for a message, so fifty cycles run in well under a second - which is what
 *   makes this usable as a gate rather than a chore. UpdateWindow() forces the
 *   paint, so the drawing path is covered too.
 *
 * BUILD
 *     tests\build_tests.cmd t51_release
 */

#include "test_common.h"

#include "ui/editstate.h"
#include "core/monitor.h"   /* MonitorFingerprint / monitor_enumerate / WAC_MON_MAX */

static int g_loops   = 0;
static int g_monitor = 0;
static int g_fuse    = 0;

static DWORD gdi_now(void)
{
    return GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
}

static DWORD user_now(void)
{
    return GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
}

int wmain(int argc, wchar_t **argv)
{
    t_diag_init(L"t51_release");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t51_release - edit mode resource discipline");
    T_KV(L"run_at", L"%ls", t_now_string());

    for (int i = 1; i < argc; ++i) {
        if      (!wcscmp(argv[i], L"--loop")    && i + 1 < argc) g_loops   = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--monitor") && i + 1 < argc) g_monitor = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--fuse")    && i + 1 < argc) g_fuse    = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--help")) {
            t_out_line(L"usage: t51_release.exe [--loop N] [--monitor N] [--fuse SECONDS]\n");
            return 0;
        }
    }

    t_set_dpi_awareness();

    HINSTANCE hinst = GetModuleHandleW(NULL);

    MonitorFingerprint mons[WAC_MON_MAX];
    const int mon_count = monitor_enumerate(mons, WAC_MON_MAX);
    T_KV(L"monitor_count", L"%d", mon_count);
    if (mon_count <= 0 || g_monitor < 0 || g_monitor >= mon_count) {
        T_WARN(L"monitor %d is not available (0..%d)", g_monitor, mon_count - 1);
        t_pause_if_console();
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* The fuse: the only exit that works when nothing else does            */
    /* ------------------------------------------------------------------ */
    if (g_fuse > 0) {
        T_SEC(L"Fuse");
        T_KV(L"requested_seconds", L"%d", g_fuse);

        if (!edit_enter(hinst, g_monitor, g_fuse)) {
            T_WARN(L"edit_enter refused - see the log");
            t_pause_if_console();
            return 1;
        }
        T_KV(L"armed_seconds", L"%d  (clamped up to the %d s floor)",
             edit_fuse_seconds(), EDIT_FUSE_MIN_SECONDS);
        T_NOTE(L"Waiting. Do NOT touch anything - this measures whether the");
        T_NOTE(L"session ends by itself, with no key pressed and no click.");

        MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            /* Handle the fuse message before dispatching: a WM_TIMER with
             * hwnd == NULL belongs to no window, and dispatching it would do
             * nothing at all. */
            (void)edit_handle_message(&msg);
            if (edit_poll()) {
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        const OverlayExit why = edit_exit_reason();
        T_KV(L"exit_reason", L"%ls", overlay_exit_name(why));
        T_KV(L"live_overlays_after", L"%d  (must be 0)", edit_live_overlays());

        const BOOL ok = (why == OVERLAY_EXIT_FUSE && edit_live_overlays() == 0);
        T_KV(L"verdict", L"%ls", ok ? L"PASS" : L"FAIL");
        if (!ok) {
            T_WARN(L"the fuse did not end the session on its own, or the");
            T_WARN(L"session did not release its overlay");
        }

        t_pause_if_console();
        t_log_close();
        return ok ? 0 : 1;
    }

    /* ------------------------------------------------------------------ */
    /* The gate                                                            */
    /* ------------------------------------------------------------------ */
    const int loops = (g_loops > 0) ? g_loops : 1;

    T_SEC(L"Enter and leave edit mode");
    T_KV(L"cycles", L"%d", loops);
    T_KV(L"monitor_index", L"%d", g_monitor);

    /*
     * Warm-up session, deliberately NOT counted.
     *
     * MEASURED 2026-09-22: the first time a 2560x1600 layered window is shown,
     * the process gains about 81 GDI and 42 USER objects - and those numbers do
     * not grow again however many sessions follow (one iteration and five both
     * report 81). That is the system standing up composition resources for a
     * layered surface, not this program leaking: overlay_gdi_objects() reads 0
     * either way, and the module reports every object it creates and destroys.
     *
     * Measuring from a cold start therefore charged the module for work the
     * window manager does once per process. This gate is about growth PER
     * SESSION, so it takes its baseline after one session has already run. If a
     * real per-session leak exists, the counters below still catch it - only
     * the one-off cost is moved out of the measurement.
     */
    if (edit_enter(hinst, g_monitor, EDIT_FUSE_DEFAULT_SECONDS)) {
        UpdateWindow(edit_overlay_hwnd());
        edit_request_exit(OVERLAY_EXIT_TOOLBAR);
        (void)edit_poll();
        T_KV(L"warmup", L"one session run, excluded from the baseline");
    } else {
        T_WARN(L"the warm-up session could not start - the deltas below will");
        T_WARN(L"include the one-off cost of the first layered window");
    }

    const DWORD gdi_before  = gdi_now();
    const DWORD user_before = user_now();
    T_KV(L"gdi_before", L"%lu", (unsigned long)gdi_before);
    T_KV(L"user_before", L"%lu", (unsigned long)user_before);

    int entered = 0;
    int polled  = 0;
    int stuck   = 0;
    int wrong_reason = 0;

    /*
     * Per-stage USER object accounting.
     *
     * The gate said one USER object per session while every module counter read
     * zero, so the leak is inside the window's lifetime rather than in session
     * bookkeeping. Splitting each cycle into its three stages says which one:
     * create+show, paint, or destroy. Without this the only method available is
     * reading overlay.c and guessing - which is how a one-per-session leak
     * survives review in the first place.
     */
    long user_enter = 0, user_paint = 0, user_leave = 0;

    /*
     * Per-cycle GDI readings, printed only where the number MOVES.
     *
     * The gate compares two readings fifty cycles apart, and that cannot tell a
     * leak that happens every cycle from one that happens in three cycles out
     * of fifty. MEASURED 2026-10-11: the same binary, run four times, produced
     * gdi_delta 12, 18, 12 and 0 - so "it failed" was never a complete
     * description of what this gate sees. Printing only the changes says which
     * cycles moved without fifty lines of noise to read past.
     */
    DWORD gdi_prev = gdi_before;

    for (int i = 0; i < loops; ++i) {
        const DWORD u0 = user_now();

        /*
         * A long fuse on purpose: it must be armed and disarmed fifty times
         * without ever firing, so this measures the arm/disarm path rather
         * than the expiry path. --fuse covers the other one.
         */
        if (!edit_enter(hinst, g_monitor, EDIT_FUSE_DEFAULT_SECONDS)) {
            T_WARN(L"edit_enter refused on cycle %d", i + 1);
            break;
        }
        entered++;
        const DWORD u1 = user_now();

        /* Force a full paint so the drawing path is exercised as well. A leak
         * in paint would otherwise be invisible to a loop that only enters and
         * leaves. */
        UpdateWindow(edit_overlay_hwnd());

        /*
         * Pump whatever ShowWindow and the paint queued.
         *
         * Real edit mode always has a message loop running - that is what
         * edit_handle_message() and edit_poll() are called from. A test loop
         * that skips the pump measures its own omission instead of the
         * product.
         *
         * MEASURED 2026-09-22: adding this did NOT change the delta (still 51
         * for 50 cycles), which is how the message pump was ruled out.
         */
        MSG pump;
        while (PeekMessageW(&pump, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&pump);
            DispatchMessageW(&pump);
        }
        const DWORD u2 = user_now();

        edit_request_exit(OVERLAY_EXIT_TOOLBAR);
        if (edit_poll()) {
            polled++;
            if (edit_exit_reason() != OVERLAY_EXIT_TOOLBAR) {
                wrong_reason++;
            }
        } else {
            /*
             * The session recorded a reason and edit_poll still did not end
             * it. That is a logic failure, not a leak, and it would otherwise
             * show up as an inexplicable mismatch below.
             */
            stuck++;
            break;
        }
        const DWORD u3 = user_now();

        user_enter += (long)u1 - (long)u0;
        user_paint += (long)u2 - (long)u1;
        user_leave += (long)u3 - (long)u2;

        const DWORD g4 = gdi_now();
        if (g4 != gdi_prev) {
            t_out_line(L"  cycle %2d: gdi %lu -> %lu (%+ld)\n",
                       i + 1, (unsigned long)gdi_prev, (unsigned long)g4,
                       (long)g4 - (long)gdi_prev);
            gdi_prev = g4;
        }
    }

    T_SEC(L"Result");
    T_KV(L"user_delta_on_enter", L"%+ld  (create + show)", user_enter);
    T_KV(L"user_delta_on_paint", L"%+ld  (UpdateWindow + pump)", user_paint);
    T_KV(L"user_delta_on_leave", L"%+ld  (request_exit + poll + destroy)", user_leave);
    T_KV(L"entries", L"%d", edit_entries());
    T_KV(L"exits", L"%d", edit_exits());
    T_KV(L"cycles_entered", L"%d", entered);
    T_KV(L"cycles_polled", L"%d", polled);
    T_KV(L"live_overlays_after", L"%d  (must be 0)", edit_live_overlays());
    T_KV(L"fuse_seconds_after", L"%d  (must be 0)", edit_fuse_seconds());

    const long gdi_delta  = (long)gdi_now()  - (long)gdi_before;
    const long user_delta = (long)user_now() - (long)user_before;
    T_KV(L"gdi_after", L"%lu", (unsigned long)gdi_now());
    T_KV(L"gdi_delta", L"%ld  (gate: <= 10)", gdi_delta);
    T_KV(L"user_after", L"%lu", (unsigned long)user_now());
    T_KV(L"user_delta", L"%ld  (gate: <= 10)", user_delta);

    /*
     * Then sit still for a second and read again.
     *
     * This process also hosts the shell's own caches, from the moment anything
     * asks it for a file association or an icon (docs/手册.md section 8.32), and
     * those live on OTHER threads. So if GDI moves while nothing of ours is
     * running, that movement is not ours - and this is the reading that tells a
     * real leak apart from a baseline that drifts on its own.
     */
    const DWORD gdi_idle_0 = gdi_now();
    Sleep(1000);
    const DWORD gdi_idle_1 = gdi_now();
    T_KV(L"gdi_idle_drift", L"%+ld  (one second, nothing of ours running)",
         (long)gdi_idle_1 - (long)gdi_idle_0);

    const BOOL ok = (entered == loops && polled == loops && stuck == 0 &&
                     wrong_reason == 0 &&
                     edit_entries() == edit_exits() &&
                     edit_live_overlays() == 0 && edit_fuse_seconds() == 0 &&
                     gdi_delta <= 10 && user_delta <= 10);

    T_KV(L"verdict", L"%ls", ok ? L"PASS" : L"FAIL");

    if (stuck > 0) {
        T_WARN(L"a session recorded an exit reason that edit_poll() did not act on");
    }
    if (wrong_reason > 0) {
        T_WARN(L"%d session(s) reported the wrong exit reason", wrong_reason);
    }
    if (edit_entries() != edit_exits()) {
        T_WARN(L"%d session(s) entered but never released - this is the leak",
               edit_entries() - edit_exits());
    }
    if (edit_live_overlays() != 0) {
        T_WARN(L"an overlay outlived edit mode: release_edit_resources()");
        T_WARN(L"missed one, or nobody called it");
    }
    if (gdi_delta > 10 || user_delta > 10) {
        T_WARN(L"handle growth is over the gate. Read the per-cycle lines above");
        T_WARN(L"before blaming edit mode:");
        T_WARN(L"  ONE large jump in one cycle -> activity from outside this");
        T_WARN(L"  process: the shell's caches and the window watcher live on");
        T_WARN(L"  other threads and are counted here too.");
        T_WARN(L"  MANY small +1 steps -> a real leak in this process. That is");
        T_WARN(L"  what this gate exists to catch - see the last line of");
        T_WARN(L"  edit_release_resources().");
    }

    T_SEC(L"What to record");
    T_NOTE(L"entries == exits, live_overlays == 0, and both deltas <= 10.");
    T_NOTE(L"--loop 50 is the gate. One cycle always reports 0 and proves nothing.");
    T_NOTE(L"When a delta is over the gate, the per-cycle lines say whether it was");
    T_NOTE(L"one external jump or a leak that grows with every session.");

    t_pause_if_console();
    t_log_close();
    return ok ? 0 : 1;
}
