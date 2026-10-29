/*
 * t23_runstate.c - the record that lets a workspace be closed later.
 *
 * THE ONE ASSERTION THAT MATTERS MOST
 *
 *   Pids are recycled. Between "wac run" and "wac close" the number this
 *   program wrote down can be handed to an unrelated process, and terminating
 *   it blindly would turn the close command into a random process killer.
 *
 *   So the record carries the executable name, and termination re-reads it
 *   from the live process first. Test 4 points that check at THIS process -
 *   with a name that does not match - and asserts that it is still running
 *   afterwards. If that assertion ever fails, the test run dies, loudly, which
 *   is exactly the right amount of noise for this bug.
 *
 * WHAT IT WRITES
 *
 *   %APPDATA%\WindowAssistanceController\running.txt, under a workspace id
 *   that only this test uses, and it removes its own lines on the way out.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t23_runstate
 *   tests\out\t23_runstate.exe --no-pause
 */

#include "test_common.h"

#include "../src/model/runstate.h"

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

static void trace(void *ctx, const wchar_t *line)
{
    (void)ctx;
    t_out_line(L"    %ls", line);
}

#define TEST_WS "t23-runstate"

/* ------------------------------------------------------------------ */

static void test_basics(void)
{
    T_SEC(L"init and add");

    RunState st;
    runstate_init(&st, TEST_WS);
    ok_int(L"the id is kept", strcmp(st.workspace_id, TEST_WS), 0);
    ok_int(L"starts empty", st.count, 0);

    ok(L"pid 0 is refused", !runstate_add(&st, 0, L"x.exe"));
    ok_int(L"still empty", st.count, 0);

    /* A pid that does not exist has no executable name to read, so recording
     * it would produce an entry that could only ever be "stale". */
    ok(L"a dead pid with no name given is refused",
       !runstate_add(&st, 0xFFFFFFF0u, NULL));

    /* Named explicitly: accepted without asking the process. */
    ok(L"an explicitly named entry is accepted",
       runstate_add(&st, 0xFFFFFFF0u, L"ghost.exe"));
    ok_int(L"one entry", st.count, 1);

    ok(L"NULL state refused", !runstate_add(NULL, 1234, L"x.exe"));

    /* Fill it up. */
    while (st.count < RUNSTATE_MAX) {
        if (!runstate_add(&st, 0xFFFF0000u + (DWORD)st.count, L"filler.exe")) break;
    }
    ok_int(L"filled to the maximum", st.count, RUNSTATE_MAX);
    ok(L"one more is refused", !runstate_add(&st, 9999, L"x.exe"));
}

static void test_round_trip(void)
{
    T_SEC(L"save, load, forget");

    RunState st;
    runstate_init(&st, TEST_WS);
    runstate_add(&st, 11111, L"alpha.exe");
    runstate_add(&st, 22222, L"beta-program.exe");

    ok(L"saved", runstate_save(&st));

    RunState back;
    ok(L"loaded", runstate_load(TEST_WS, &back));
    ok_int(L"both entries came back", back.count, 2);
    ok_int(L"first pid", (int)back.entries[0].pid, 11111);
    ok_int(L"second pid", (int)back.entries[1].pid, 22222);
    ok(L"first name survived",
       wcscmp(back.entries[0].exe, L"alpha.exe") == 0);
    ok(L"second name survived",
       wcscmp(back.entries[1].exe, L"beta-program.exe") == 0);

    /* Saving again must REPLACE this workspace's lines, not append a second
     * copy - otherwise every run doubles the record. */
    RunState again;
    runstate_init(&again, TEST_WS);
    runstate_add(&again, 33333, L"gamma.exe");
    ok(L"saved again", runstate_save(&again));

    RunState third;
    ok(L"loaded again", runstate_load(TEST_WS, &third));
    ok_int(L"still two lines' worth: one entry", third.count, 1);
    ok_int(L"and it is the new one", (int)third.entries[0].pid, 33333);

    /* Another workspace's lines must not be touched. */
    RunState other;
    runstate_init(&other, "t23-other");
    runstate_add(&other, 44444, L"other.exe");
    runstate_save(&other);

    RunState check;
    ok(L"the test workspace still loads", runstate_load(TEST_WS, &check));
    ok_int(L"its single entry is intact", check.count, 1);

    ok(L"forget reports something was removed", runstate_forget(TEST_WS));
    ok(L"a second forget finds nothing", !runstate_forget(TEST_WS));
    ok(L"it no longer loads", !runstate_load(TEST_WS, &check));

    runstate_forget("t23-other");
}

/*
 * THE ONE THAT MATTERS: a recorded pid whose program no longer matches must
 * be left alone.
 */
static void test_recycled_pid_is_not_killed(void)
{
    T_SEC(L"a pid that now belongs to another program is left alone");

    RunState st;
    runstate_init(&st, TEST_WS);

    /* This process, but recorded under a name it does not have. */
    ok(L"recorded with a WRONG executable name",
       runstate_add(&st, GetCurrentProcessId(), L"definitely-not-this.exe"));

    int failed = 0, stale = 0;
    const int ended = runstate_terminate(&st, trace, NULL, &failed, &stale);

    ok_int(L"nothing was ended", ended, 0);
    ok_int(L"the mismatch was reported as stale", stale, 1);
    ok_int(L"and not as a failure", failed, 0);

    t_out_line(L"    (this process is still here, which is the point)");

    runstate_forget(TEST_WS);
}

/* A matching entry IS ended. Done on a child, never on ourselves. */
static void test_matching_entry_is_ended(void)
{
    T_SEC(L"a matching entry is ended");

    /* Start something harmless that stays alive for a while. */
    wchar_t self[MAX_PATH];
    if (GetModuleFileNameW(NULL, self, MAX_PATH) == 0) {
        ok(L"own path known", 0);
        return;
    }

    wchar_t cmd[512];
    _snwprintf_s(cmd, 512, _TRUNCATE,
                 L"\"%ls\" --child-of-t23", self);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        ok(L"child started", 0);
        return;
    }
    ok(L"child started", 1);
    CloseHandle(pi.hThread);

    wchar_t exe[MAX_PATH];
    wcsncpy_s(exe, MAX_PATH, self, _TRUNCATE);
    const wchar_t *leaf = wcsrchr(exe, L'\\');
    if (leaf != NULL) wmemmove(exe, leaf + 1, wcslen(leaf));

    RunState st;
    runstate_init(&st, TEST_WS);
    ok(L"recorded with the right name", runstate_add(&st, pi.dwProcessId, exe));

    int failed = 0, stale = 0;
    const int ended = runstate_terminate(&st, trace, NULL, &failed, &stale);

    ok_int(L"one process ended", ended, 1);
    ok_int(L"nothing stale", stale, 0);
    ok_int(L"nothing failed", failed, 0);

    WaitForSingleObject(pi.hProcess, 3000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    ok(L"the child really exited", code != STILL_ACTIVE);
    CloseHandle(pi.hProcess);

    runstate_forget(TEST_WS);
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    /* Child mode first: it must not initialise the harness. */
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--child-of-t23") == 0) {
            FILE *sink = NULL;
            (void)freopen_s(&sink, "NUL", "w", stdout);
            (void)freopen_s(&sink, "NUL", "w", stderr);
            Sleep(8000);
            return 0;
        }
    }

    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t23_runstate");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t23_runstate - the record of what a workspace started");
    T_KV(L"run_at", L"%ls", t_now_string());

    test_basics();
    test_round_trip();
    test_recycled_pid_is_not_killed();
    test_matching_entry_is_ended();

    T_SEC(L"Result");
    T_KV(L"passed", L"%d", g_pass);
    T_KV(L"failed", L"%d", g_fail);

    if (g_fail == 0) {
        T_KV(L"VERDICT", L"PASS - all %d assertions hold", g_pass);
    } else {
        T_KV(L"VERDICT", L"FAIL - %d assertion(s) broken", g_fail);
    }

    if (!no_pause) t_pause();

    t_log_close();
    return (g_fail == 0) ? 0 : 1;
}
