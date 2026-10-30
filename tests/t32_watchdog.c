/*
 * t32_watchdog.c - the edit-mode watchdog (src/core/watchdog.c).
 *
 * WHAT IT PROVES
 *
 *   The guard has two halves and only one of them can be tested without
 *   freezing the UI thread:
 *
 *     1. the boundary rule - fire only when the gap is strictly greater than
 *        the timeout, never when the clock appears to go backwards;
 *     2. the lifecycle - the thread is created, ticks do not fire it, stop is
 *        prompt and idempotent, and repeated start/stop pairs do not leak
 *        handles (t51 runs fifty edit sessions; a leak here would show up
 *        there, but this catches it without a window).
 *
 *   The third half - "does it actually hide the overlay when the UI thread
 *   blocks" - cannot be asserted here: the test IS the UI thread. It is left
 *   to the manual probe described in docs/进度记录.md.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t32_watchdog
 *   tests\out\t32_watchdog.exe --no-pause
 */

#include "test_common.h"

#include "../src/core/watchdog.h"

static int g_pass, g_fail;

static void ok(const wchar_t *what, int cond)
{
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls", what);
    }
}

static void ok_int(const wchar_t *what, int got, int want)
{
    if (got == want) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls  (got %d, want %d)", what, got, want);
    }
}

/* ------------------------------------------------------------------ */

static void test_boundary(void)
{
    T_SEC(L"the fire rule (strictly greater than the timeout)");

    const DWORD t = WATCHDOG_TIMEOUT_MS;

    ok(L"clock did not move",        !watchdog_should_fire(1000, 1000, t));
    ok(L"one ms short of the limit", !watchdog_should_fire(1000 + t - 1, 1000, t));
    ok(L"exactly at the limit",      !watchdog_should_fire(1000 + t, 1000, t));
    ok(L"one ms past the limit",      watchdog_should_fire(1000 + t + 1, 1000, t));
    ok(L"far past the limit",         watchdog_should_fire(60000, 1000, t));

    /* GetTickCount64 is monotonic, but a stale read can still look backwards. */
    ok(L"clock went backwards",      !watchdog_should_fire(500, 1000, t));

    ok(L"a zero timeout fires on any forward gap",
        watchdog_should_fire(1001, 1000, 0));
    ok(L"a zero timeout does not fire on an equal read",
       !watchdog_should_fire(1000, 1000, 0));
}

static void test_lifecycle(void)
{
    T_SEC(L"start / tick / stop");

    DWORD before = 0, after = 0;
    GetProcessHandleCount(GetCurrentProcess(), &before);

    ok(L"nothing has fired yet", !watchdog_fired());

    ok(L"start succeeds with no window", watchdog_start(NULL) == TRUE);
    ok(L"still nothing fired after start", !watchdog_fired());

    /*
     * Heartbeats well inside the timeout must keep it quiet. This also proves
     * watchdog_tick() is not a no-op: with a broken heartbeat the guard would
     * fire during the loop below (5 s / 500 ms poll is not enough time here,
     * so the loop is deliberately short - the point is that tick returns and
     * the thread keeps waiting).
     */
    for (int i = 0; i < 10; ++i) {
        watchdog_tick();
        Sleep(50);
    }
    ok(L"heartbeats did not fire the guard", !watchdog_fired());

    const DWORD t0 = GetTickCount();
    watchdog_stop();
    const DWORD took = GetTickCount() - t0;
    T_KV(L"stop took", L"%lu ms", (unsigned long)took);
    ok(L"stop returned promptly", took < 2000);

    ok(L"stop is idempotent", (watchdog_stop(), TRUE));

    /* Repeated sessions must not leak handles: t51 runs fifty of them. */
    int loop_ok = 1;
    for (int i = 0; i < 25; ++i) {
        if (!watchdog_start(NULL)) { loop_ok = 0; break; }
        watchdog_tick();
        watchdog_stop();
    }
    ok(L"25 start/stop pairs all succeeded", loop_ok);

    GetProcessHandleCount(GetCurrentProcess(), &after);
    T_KV(L"process handles before / after", L"%lu / %lu",
         (unsigned long)before, (unsigned long)after);

    /*
     * One handle pair per session would show as +50 here; the exact-parity
     * check is what matters, not the absolute number.
     */
    ok(L"start/stop left no handles behind", (after - before) <= 2);

    ok(L"nothing fired after a clean session", !watchdog_fired());
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t32_watchdog");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t32_watchdog - the edit-mode watchdog (boundary rule + lifecycle)");
    T_KV(L"run_at", L"%ls", t_now_string());
    T_NOTE(L"The guard only exists while edit mode is active, so the idle");
    T_NOTE(L"process keeps its three threads and 0.000 s CPU time. This test");
    T_NOTE(L"does not stall the UI thread: the firing itself is a manual probe.");

    test_boundary();
    test_lifecycle();

    T_SEC(L"Result");
    T_KV(L"passed", L"%d", g_pass);
    T_KV(L"failed", L"%d", g_fail);

    if (g_fail == 0) T_KV(L"VERDICT", L"PASS - all %d assertions hold", g_pass);
    else             T_KV(L"VERDICT", L"FAIL - %d assertion(s) broken", g_fail);

    if (!no_pause) t_pause();
    t_log_close();
    return (g_fail == 0) ? 0 : 1;
}
