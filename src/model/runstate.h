/*
 * runstate.h - what a workspace started, so it can be closed later.
 *
 * THE PROBLEM
 *
 *   "Close this workspace" has to end the programs that workspace started -
 *   and the process that started them is usually gone by then. `wac run` exits
 *   as soon as the windows are placed; a job object would die with it.
 *
 *   So the pids are written down. That is the whole design, and it carries one
 *   real hazard: PIDS ARE RECYCLED. Between "started" and "closed" a pid can
 *   be handed to an unrelated program, and terminating it blindly would make
 *   this tool a random process killer.
 *
 *   Every entry therefore stores the EXECUTABLE NAME as well, and termination
 *   re-reads it from the live process before doing anything. A pid whose
 *   program no longer matches is reported as stale and left alone.
 *
 * WHY NOT A JOB OBJECT
 *
 *   The window process CAN use one - it lives long enough, and it does, for
 *   its own launches. The command line cannot, which is why this file exists
 *   for the cross-process case. The trade is visible rather than hidden: a job
 *   reaches grandchildren (a browser's helpers), pid-by-pid termination does
 *   not, and `wac close` says so when it uses this path.
 *
 * THE FILE
 *
 *   %APPDATA%\WindowAssistanceController\running.txt, one line per process:
 *
 *       <workspace id> <TAB> <pid> <TAB> <executable file name>
 *
 *   Plain text because it is not configuration: nothing here is precious, a
 *   lost record only means a workspace cannot be closed automatically, and a
 *   second INI dialect with checksums and rotating backups would be a lot of
 *   machinery for data that is disposable by design.
 */

#ifndef WAC_RUNSTATE_H
#define WAC_RUNSTATE_H

#include <windows.h>

#include "../core/app_id.h"   /* WAC_EXE_MAX */

#ifdef __cplusplus
extern "C" {
#endif

#define RUNSTATE_ID_MAX 64
#define RUNSTATE_MAX    16    /* must be able to hold one workspace's slots */

typedef struct {
    DWORD   pid;
    wchar_t exe[WAC_EXE_MAX];   /* the executable that pid had when recorded */
} RunEntry;

typedef struct {
    char     workspace_id[RUNSTATE_ID_MAX];
    int      count;
    RunEntry entries[RUNSTATE_MAX];
} RunState;

void runstate_init(RunState *st, const char *workspace_id);

/*
 * Remember a process. `exe` may be NULL, in which case it is read from the
 * live process - which is the better source anyway, since that is exactly what
 * termination will compare against.
 *
 * Returns FALSE when the state is full or the pid is 0.
 */
BOOL runstate_add(RunState *st, DWORD pid, const wchar_t *exe);

/*
 * Replace this workspace's lines in the file, leaving other workspaces alone.
 * Returns FALSE when the file could not be written - the caller should say so,
 * because the consequence is "this workspace can no longer be closed".
 */
BOOL runstate_save(const RunState *st);

/* Read one workspace's record back. FALSE when there is none. */
BOOL runstate_load(const char *workspace_id, RunState *out);

/* Drop one workspace's record. Returns TRUE when something was removed. */
BOOL runstate_forget(const char *workspace_id);

/*
 * End every process in the record.
 *
 * Each pid is re-checked against its recorded executable name first. A pid
 * that is gone, or that now belongs to a different program, is counted in
 * *out_stale and NOT touched.
 *
 * `trace` receives one line per process; NULL silences it.
 */
typedef void (*RunStateTrace)(void *ctx, const wchar_t *line);

int runstate_terminate(const RunState *st, RunStateTrace trace, void *ctx,
                       int *out_failed, int *out_stale);

#ifdef __cplusplus
}
#endif

#endif /* WAC_RUNSTATE_H */
