/*
 * proc_launch.h - starting a program, and keeping track of it afterwards.
 *
 * WHY THIS IS NOT ONE CreateProcessW CALL
 *
 *   Two things have to be right, and both of them are security-relevant.
 *
 *   1. THE EXECUTABLE AND ITS ARGUMENTS ARE PASSED SEPARATELY.
 *
 *      Building one command line with swprintf("\"%s\" %s", exe, args) and
 *      handing it to CreateProcessW(NULL, cmd) means Windows parses that
 *      string - and a path containing a quote or a space changes which program
 *      runs. Passing lpApplicationName makes the system skip the parsing
 *      entirely: the executable is whatever the caller named, and the string
 *      is only arguments.
 *
 *   2. THE PROCESSES ARE PUT IN A JOB OBJECT.
 *
 *      "Close this workspace" has to end the applications it started, and a
 *      browser has dozens of processes that were never started directly. A job
 *      object kills the tree. It is best-effort: AssignProcessToJobObject
 *      fails for a process that already belongs to another job or that runs
 *      elevated, so every process handle is kept as well and the fallback is
 *      TerminateProcess on the ones that could not join.
 *
 * WHAT THIS MODULE DELIBERATELY DOES NOT DO
 *
 *   It does not wait for windows (that is core/launch.c) and it does not
 *   decide when anything should be killed (that is the caller). It starts a
 *   program and remembers the handle.
 */

#ifndef WAC_PROC_LAUNCH_H
#define WAC_PROC_LAUNCH_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAC_LAUNCH_ARGS_MAX    1024   /* arguments only, never the exe path */

/*
 * How a program is started.
 *
 *   LAUNCH_CREATE    an ordinary executable, started directly
 *   LAUNCH_SHELL     through the shell - a document, a shell association, or
 *                    something that needs a UAC prompt
 *   LAUNCH_CHROMIUM  an executable that understands
 *                    --window-position/--window-size; started the same way as
 *                    CREATE, but the CALLER adds those arguments, because only
 *                    it knows the target rectangle
 */
typedef enum {
    LAUNCH_CREATE = 0,
    LAUNCH_SHELL  = 1,
    LAUNCH_CHROMIUM = 2
} LaunchKind;

typedef struct {
    const wchar_t *exe_path;   /* REQUIRED. Becomes lpApplicationName. */
    const wchar_t *args;       /* arguments only; may be NULL or empty */
    const wchar_t *work_dir;   /* may be NULL */
    LaunchKind     kind;
    BOOL           elevated;   /* ask the shell for a UAC prompt */
    DWORD         *out_pid;    /* filled when the process could be identified */
} LaunchSpec;

/*
 * Start `spec`. On success `*out_process` receives a handle the caller owns
 * (close it, or hand it to a LaunchGroup, which takes ownership).
 *
 * Returns FALSE without starting anything when the spec is incomplete. The
 * caller cannot tell "not found" from "refused" from here - GetLastError()
 * carries that, and the executor reports it per slot.
 *
 * `out_process` may be NULL. With a shell launch it can still come back NULL
 * on success: the shell is not always willing to hand over the process.
 */
BOOL launch_app(const LaunchSpec *spec, HANDLE *out_process);

/* ------------------------------------------------------------------ */
/* Job object - "close everything this workspace started"              */
/* ------------------------------------------------------------------ */

#define LAUNCH_GROUP_MAX 16

typedef struct {
    HANDLE job;                              /* NULL = no kernel job; the
                                              * per-process fallback is used */
    HANDLE process[LAUNCH_GROUP_MAX];        /* owned by the group */
    DWORD  pid[LAUNCH_GROUP_MAX];
    int    count;
    int    in_job;                           /* how many joined the job */
} LaunchGroup;

/*
 * Create the group and its job object.
 *
 * A failure to create the JOB is not a failure of this function: the group
 * still works, it just cannot kill grandchildren. That is recorded in
 * `job == NULL` and reported by the caller rather than silently ignored.
 */
BOOL launch_group_init(LaunchGroup *g);

/*
 * Take ownership of a process handle and try to put the process in the job.
 *
 * The handle is owned by the group from here on - the caller must not close
 * it. When the group is full the handle is closed and FALSE is returned, so a
 * caller that ignores the result still cannot leak it.
 */
BOOL launch_group_add(LaunchGroup *g, HANDLE process, DWORD pid);

/*
 * End everything the group knows about. Returns how many processes were
 * terminated, and sets *out_failed to the number that could not be.
 *
 * The job is used when it has members; the leftover handles are terminated
 * individually. This is the ONLY destructive operation in the program, and the
 * caller is expected to have asked the user first.
 */
int launch_group_terminate(LaunchGroup *g, int *out_failed);

/*
 * Close every handle and the job. Does NOT terminate anything: closing a job
 * would kill its members (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE), so this is for
 * the "the workspace finished normally" path only.
 */
void launch_group_destroy(LaunchGroup *g);

#ifdef __cplusplus
}
#endif

#endif /* WAC_PROC_LAUNCH_H */
