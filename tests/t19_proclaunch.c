/*
 * t19_proclaunch.c - tests for src/core/proc_launch.c
 *
 * WHAT THIS ACTUALLY VERIFIES
 *
 *   proc_launch is thin over Win32, so there is no arithmetic to pin down.
 *   What is worth proving is the part that only fails at runtime:
 *
 *     1. THE JOB OBJECT REALLY KILLS. "Close this workspace" is the only
 *        destructive operation in the program; if TerminateJobObject quietly
 *        does nothing, the user presses the button, is told it worked, and
 *        their programs are still running.
 *
 *     2. DESTROY DOES *NOT* KILL. The other half of the same promise:
 *        finishing a workspace normally must leave the applications alone.
 *        Closing a job with KILL_ON_JOB_CLOSE set would end them, which is
 *        why the limit is cleared first - and that is a one-line property
 *        nobody would notice going wrong.
 *
 *   Both are checked by starting a copy of THIS program with --child, which
 *   sleeps for a while. A console program started from a console inherits the
 *   console, so nothing flashes on the user's desktop.
 *
 * THE --child MODE MUST COME FIRST
 *
 *   It returns before the test harness is initialised, so a child writes no
 *   log and touches nothing.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t19_proclaunch
 *   tests\out\t19_proclaunch.exe --no-pause
 */

#include "test_common.h"

#include "../src/core/proc_launch.h"

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

static wchar_t g_self[MAX_PATH];

/* A child that outlives the few milliseconds the test needs. */
#define CHILD_SLEEP_MS 8000

static BOOL is_alive(HANDLE h)
{
    DWORD code = 0;
    if (h == NULL) return FALSE;
    if (!GetExitCodeProcess(h, &code)) return FALSE;
    return code == STILL_ACTIVE;
}

/*
 * Child mode: sleep, then exit.
 *
 * IT DETACHES FROM STDOUT FIRST, and that is not tidiness. The child inherits
 * our stdout, so while it lives it holds the write end of whatever pipe the
 * caller is reading us through - and a caller that pipes this program into
 * something else waits for the CHILD to exit, not for us. Measured: running
 * this test with its output piped timed out after two minutes with no output
 * at all, while the same run redirected to a file finished in under a second.
 */
static int run_child(void)
{
    FILE *sink = NULL;
    (void)freopen_s(&sink, "NUL", "w", stdout);
    (void)freopen_s(&sink, "NUL", "w", stderr);

    Sleep(CHILD_SLEEP_MS);
    return 0;
}

/* ------------------------------------------------------------------ */

static void test_bad_specs(void)
{
    T_SEC(L"incomplete specs start nothing");

    HANDLE h = (HANDLE)1;   /* deliberately not NULL: it must be cleared */

    ok(L"NULL spec refused", !launch_app(NULL, &h));
    ok(L"out handle cleared", h == NULL);

    LaunchSpec spec;
    ZeroMemory(&spec, sizeof(spec));

    h = (HANDLE)1;
    ok(L"missing path refused", !launch_app(&spec, &h));
    ok(L"out handle cleared again", h == NULL);

    spec.exe_path = L"";
    spec.out_pid = NULL;
    ok(L"empty path refused", !launch_app(&spec, NULL));
}

static void test_create_path_and_job_kill(void)
{
    T_SEC(L"CreateProcess path, then kill through the job");

    LaunchSpec spec;
    ZeroMemory(&spec, sizeof(spec));
    spec.exe_path = g_self;
    spec.args     = L"--child";
    spec.kind     = LAUNCH_CREATE;

    DWORD pid = 0;
    spec.out_pid = &pid;

    HANDLE proc = NULL;
    ok(L"started", launch_app(&spec, &proc));
    ok(L"process handle came back", proc != NULL);
    ok(L"pid was reported", pid != 0);
    if (proc == NULL) return;

    T_KV(L"child", L"pid=%lu", (unsigned long)pid);
    ok(L"child is running", is_alive(proc));

    LaunchGroup g;
    ok(L"group initialised", launch_group_init(&g));
    T_KV(L"job object", L"%ls", (g.job != NULL) ? L"created" : L"NOT created");

    ok(L"process taken into the group", launch_group_add(&g, proc, pid));
    ok_int(L"group remembers one process", g.count, 1);
    ok_int(L"it joined the job", g.in_job, 1);

    int failed = 0;
    const int ended = launch_group_terminate(&g, &failed);
    T_KV(L"terminate", L"ended=%d failed=%d", ended, failed);

    ok(L"something was terminated", ended >= 1);
    ok_int(L"nothing failed to terminate", failed, 0);

    /* Ask the kernel, not our own bookkeeping. */
    if (g.count > 0) {
        WaitForSingleObject(g.process[0], 3000);
        ok(L"the child is really gone", !is_alive(g.process[0]));
    }

    launch_group_destroy(&g);
    ok(L"group emptied", g.count == 0 && g.job == NULL);
}

static void test_destroy_does_not_kill(void)
{
    T_SEC(L"destroying a group leaves the programs running");

    /*
     * This is the "the workspace finished, the windows stay open" path. It is
     * the reason launch_group_destroy clears KILL_ON_JOB_CLOSE before closing
     * the job.
     */
    LaunchSpec spec;
    ZeroMemory(&spec, sizeof(spec));
    spec.exe_path = g_self;
    spec.args     = L"--child";
    spec.kind     = LAUNCH_CREATE;

    DWORD pid = 0;
    spec.out_pid = &pid;

    HANDLE proc = NULL;
    if (!launch_app(&spec, &proc)) { ok(L"started", 0); return; }
    ok(L"started", 1);

    LaunchGroup g;
    launch_group_init(&g);
    launch_group_add(&g, proc, pid);

    /* The group owns the handle now, so a second one is opened to watch it. */
    HANDLE watch = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                               FALSE, pid);
    ok(L"watch handle opened", watch != NULL);

    launch_group_destroy(&g);

    if (watch != NULL) {
        Sleep(300);
        ok(L"the child is STILL running after destroy", is_alive(watch));
        TerminateProcess(watch, 0);
        WaitForSingleObject(watch, 3000);
        CloseHandle(watch);
    }
}

static void test_shell_path(void)
{
    T_SEC(L"ShellExecuteEx path");

    LaunchSpec spec;
    ZeroMemory(&spec, sizeof(spec));
    spec.exe_path = g_self;
    spec.args     = L"--child";
    spec.kind     = LAUNCH_SHELL;
    spec.elevated = FALSE;

    DWORD pid = 0;
    spec.out_pid = &pid;

    HANDLE proc = NULL;
    const BOOL started = launch_app(&spec, &proc);

    ok(L"started", started);
    T_KV(L"shell launch", L"process handle %ls", (proc != NULL) ? L"returned" : L"NOT returned");

    /*
     * A NULL handle here is not a failure: the shell is not obliged to hand
     * over a process, and the executor reports "started but not tracked".
     */
    if (started && proc != NULL) {
        ok(L"child is running", is_alive(proc));
        TerminateProcess(proc, 0);
        WaitForSingleObject(proc, 3000);
        CloseHandle(proc);
    }
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    /* FIRST: the child mode must not initialise the harness. */
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--child") == 0) {
            return run_child();
        }
    }

    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t19_proclaunch");
    t_log_mirror(1);
    t_console_init();

    if (GetModuleFileNameW(NULL, g_self, MAX_PATH) == 0) {
        t_out_line(L"  cannot determine own path; nothing can be tested");
        t_log_close();
        return 1;
    }

    T_TITLE(L"t19_proclaunch - starting programs and ending them");
    T_KV(L"run_at", L"%ls", t_now_string());
    T_KV(L"self", L"%ls", g_self);

    test_bad_specs();
    test_create_path_and_job_kill();
    test_destroy_does_not_kill();
    test_shell_path();

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
