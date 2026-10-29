/*
 * proc_launch.c - see proc_launch.h.
 */

#include "proc_launch.h"

/* WIN32_LEAN_AND_MEAN excludes shellapi.h, and ShellExecuteExW lives there. */
#include <shellapi.h>

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Starting a program                                                  */
/* ------------------------------------------------------------------ */

/* The exe path, its quotes, and the arguments. See launch_via_create. */
#define WAC_LAUNCH_CMDLINE_MAX (WAC_LAUNCH_ARGS_MAX + 300)

/*
 * The ordinary path.
 *
 * TWO THINGS HAVE TO BE RIGHT HERE, AND THE FIRST ONE COST A DEBUGGING
 * SESSION.
 *
 * 1. THE COMMAND LINE MUST START WITH THE PROGRAM NAME.
 *
 *    MEASURED (tests/t19_proclaunch.c): with lpApplicationName set and
 *    lpCommandLine holding ONLY the arguments, the child's argv[0] becomes the
 *    first argument. GetCommandLineW() returns exactly what was passed, the
 *    C runtime treats its first token as the program name, and every argument
 *    shifts left by one. The test's own child was started with "--child" and
 *    reported argc=1, argv[1]=(none) - it did not recognise itself as a child
 *    and started another one, two hundred processes deep before anyone
 *    noticed.
 *
 *    So the line is built as "\"<exe>\" <args>". THIS IS NOT THE FORBIDDEN
 *    PATTERN. What the architecture document rules out is
 *    CreateProcessW(NULL, line), where Windows parses that string to decide
 *    WHICH program to run - a path with a quote in it then selects a different
 *    executable. Here lpApplicationName still names the executable, so the
 *    string can only ever be arguments plus a name for argv[0].
 *
 * 2. lpApplicationName STAYS SET, for exactly that reason.
 *
 * The buffer is a local array because CreateProcessW is allowed to modify it.
 */
static BOOL launch_via_create(const LaunchSpec *spec, HANDLE *out_process)
{
    wchar_t cmdline[WAC_LAUNCH_CMDLINE_MAX];

    if (spec->args != NULL && spec->args[0] != L'\0') {
        _snwprintf_s(cmdline, WAC_LAUNCH_CMDLINE_MAX, _TRUNCATE,
                     L"\"%ls\" %ls", spec->exe_path, spec->args);
    } else {
        _snwprintf_s(cmdline, WAC_LAUNCH_CMDLINE_MAX, _TRUNCATE,
                     L"\"%ls\"", spec->exe_path);
    }

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    const BOOL ok = CreateProcessW(
        spec->exe_path,        /* the executable, named - never parsed */
        cmdline,               /* argv[0] plus the arguments - see above */
        NULL, NULL, FALSE,
        CREATE_UNICODE_ENVIRONMENT,
        NULL,
        (spec->work_dir != NULL && spec->work_dir[0] != L'\0') ? spec->work_dir : NULL,
        &si, &pi);

    if (!ok) return FALSE;

    /*
     * Both handles come back open. The thread handle is of no use here: the
     * program is not waited on, and holding it would keep the thread object
     * alive for nothing.
     */
    CloseHandle(pi.hThread);

    if (spec->out_pid != NULL) *spec->out_pid = pi.dwProcessId;

    if (out_process != NULL) {
        *out_process = pi.hProcess;
    } else {
        CloseHandle(pi.hProcess);
    }

    return TRUE;
}

/*
 * The shell path: a document, a shell association, or a UAC prompt.
 *
 * lpFile and lpParameters are separate for the same reason lpApplicationName
 * is: the shell must not have to guess where the program name ends.
 */
static BOOL launch_via_shell(const LaunchSpec *spec, HANDLE *out_process)
{
    wchar_t params[WAC_LAUNCH_ARGS_MAX];
    params[0] = L'\0';

    if (spec->args != NULL && spec->args[0] != L'\0') {
        wcsncpy_s(params, WAC_LAUNCH_ARGS_MAX, spec->args, _TRUNCATE);
    }

    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpFile       = spec->exe_path;
    sei.lpParameters = (params[0] != L'\0') ? params : NULL;
    sei.lpDirectory  = (spec->work_dir != NULL && spec->work_dir[0] != L'\0')
                           ? spec->work_dir : NULL;
    sei.nShow        = SW_SHOWNORMAL;
    if (spec->elevated) sei.lpVerb = L"runas";

    if (!ShellExecuteExW(&sei)) return FALSE;

    /*
     * The shell is not obliged to hand over a process: for a document, or for
     * an application that hands off to an existing instance, hProcess comes
     * back NULL and the launch still succeeded. Callers must live with that -
     * the executor reports "started, but not tracked" rather than failing.
     */
    if (sei.hProcess != NULL) {
        if (spec->out_pid != NULL) *spec->out_pid = GetProcessId(sei.hProcess);

        if (out_process != NULL) {
            *out_process = sei.hProcess;
        } else {
            CloseHandle(sei.hProcess);
        }
    }

    return TRUE;
}

BOOL launch_app(const LaunchSpec *spec, HANDLE *out_process)
{
    if (out_process != NULL) *out_process = NULL;
    if (spec == NULL) return FALSE;

    if (spec->exe_path == NULL || spec->exe_path[0] == L'\0') return FALSE;
    if (spec->out_pid != NULL) *spec->out_pid = 0;

    /*
     * LAUNCH_CHROMIUM takes the ordinary path: the window-position arguments
     * are the caller's business, because only the caller knows the rectangle.
     */
    if (spec->elevated || spec->kind == LAUNCH_SHELL) {
        return launch_via_shell(spec, out_process);
    }
    return launch_via_create(spec, out_process);
}

/* ------------------------------------------------------------------ */
/* Job object                                                          */
/* ------------------------------------------------------------------ */

BOOL launch_group_init(LaunchGroup *g)
{
    if (g == NULL) return FALSE;

    ZeroMemory(g, sizeof(*g));

    /*
     * JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE is what makes "close the workspace"
     * complete: a browser's helper processes are in the job without ever being
     * started by us, so TerminateProcess on the ones we hold would leave them
     * behind.
     *
     * Failing to create the job is NOT a failure of the group. The per-process
     * fallback still works, and the caller can report the reduced guarantee.
     */
    g->job = CreateJobObjectW(NULL, NULL);
    if (g->job == NULL) return TRUE;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    ZeroMemory(&li, sizeof(li));
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (!SetInformationJobObject(g->job, JobObjectExtendedLimitInformation,
                                 &li, sizeof(li))) {
        /* A job without the limit is worse than no job: it would report
         * success at closing while leaving processes behind. Drop it and let
         * the fallback be honest about what it can do. */
        CloseHandle(g->job);
        g->job = NULL;
    }

    return TRUE;
}

BOOL launch_group_add(LaunchGroup *g, HANDLE process, DWORD pid)
{
    if (g == NULL || process == NULL) return FALSE;

    if (g->count >= LAUNCH_GROUP_MAX) {
        /* Refuse, and close it: a caller that ignores the result must not be
         * able to leak the handle. */
        CloseHandle(process);
        return FALSE;
    }

    /*
     * Best effort, and the result is remembered rather than acted on. It fails
     * for a process that already belongs to another job (the shell sometimes
     * does that) and for an elevated process - neither is a reason to refuse
     * to start the program.
     */
    if (g->job != NULL && AssignProcessToJobObject(g->job, process)) {
        g->in_job++;
    }

    g->process[g->count] = process;
    g->pid[g->count]     = pid;
    g->count++;
    return TRUE;
}

int launch_group_terminate(LaunchGroup *g, int *out_failed)
{
    if (g == NULL) return 0;

    if (out_failed != NULL) *out_failed = 0;

    int ended  = 0;
    int failed = 0;

    /*
     * The job first: it reaches processes we never held a handle for. Killing
     * it also ends the ones below, so those handles are only used for the
     * processes that could not join.
     */
    if (g->job != NULL && g->in_job > 0) {
        if (TerminateJobObject(g->job, 1)) {
            ended += g->in_job;
        } else {
            failed += g->in_job;
        }
    }

    for (int i = 0; i < g->count; ++i) {
        if (g->process[i] == NULL) continue;

        /* Already ended by the job? Ask rather than assume: a process that
         * exited on its own is a success, not a failure. */
        DWORD code = 0;
        if (!GetExitCodeProcess(g->process[i], &code)) { failed++; continue; }
        if (code != STILL_ACTIVE) continue;

        if (TerminateProcess(g->process[i], 0)) {
            ended++;
        } else {
            failed++;
        }
    }

    if (out_failed != NULL) *out_failed = failed;
    return ended;
}

void launch_group_destroy(LaunchGroup *g)
{
    if (g == NULL) return;

    for (int i = 0; i < g->count; ++i) {
        if (g->process[i] != NULL) CloseHandle(g->process[i]);
    }

    /* Closing the job would kill whatever is still in it. By the time this is
     * called the caller has either terminated the group or decided to leave
     * the programs running, so the kill-on-close limit has to come off first -
     * otherwise "the workspace is done, leave the windows alone" would quietly
     * end them. */
    if (g->job != NULL) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        ZeroMemory(&li, sizeof(li));
        li.BasicLimitInformation.LimitFlags = 0;
        SetInformationJobObject(g->job, JobObjectExtendedLimitInformation,
                                &li, sizeof(li));

        CloseHandle(g->job);
    }

    ZeroMemory(g, sizeof(*g));
}
