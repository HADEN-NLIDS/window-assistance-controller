/*
 * workspace_run.h - start a workspace's applications and put them where they
 * belong.
 *
 * WHY THIS IS IN model/ AND NOT core/
 *
 *   The plan listed it as core/workspace_run.c next to proc_launch.c, but it
 *   depends on the WORKSPACE model (which slots, which presets) and on rule.h.
 *   core is the layer below the model, so
 *   putting it there would point a dependency arrow the wrong way. It sits
 *   beside workspace.c instead, where "the thing that executes a workspace"
 *   can see what a workspace is.
 *
 * WHAT IT IS RESPONSIBLE FOR
 *
 *   Reading a slot through its preset reference, starting the program,
 *   waiting for ITS window, and placing it. Nothing about the UI, and nothing
 *   about the command line - progress goes out through a callback so the same
 *   code serves both.
 *
 * SEQUENTIAL AND PARALLEL
 *
 *   A slot with order > 0 makes the whole workspace SEQUENTIAL: slots run in
 *   ascending order, and each one is placed before the next is started. That
 *   is what a user means by "start the IDE first, then its terminals".
 *
 *   With every order at 0 the workspace is PARALLEL, and parallel means what
 *   it can mean here: all the programs are started first and polled for their
 *   windows afterwards. Launching is where the seconds are; waiting is cheap
 *   and already overlapped by the fact that the other programs are starting
 *   while this one's window is looked for.
 *
 * ONE KNOWN LIMIT
 *
 *   Two slots naming the SAME executable are ambiguous in parallel mode: both
 *   look for a new window of that name and find the same one. Sequential mode
 *   has no such problem because the first window is placed before the second
 *   process starts. Reported rather than papered over - the executor marks the
 *   second slot as seeing an already-claimed window.
 */

#ifndef WAC_WORKSPACE_RUN_H
#define WAC_WORKSPACE_RUN_H

#include <windows.h>

#include "proc_launch.h"
#include "rule.h"
#include "workspace.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How one slot ended up. Names are for the log; the report says what the
 * caller must tell the user. */
typedef enum {
    RUN_OK = 0,        /* started, window found, placed where it was asked */
    RUN_SKIPPED,       /* nothing identifiable to start - bad configuration */
    RUN_NOT_FOUND,     /* the program could not be started at all */
    RUN_TIMEOUT,       /* it started, but no window appeared in time */
    RUN_DENIED,        /* Windows refused the move: an elevated target (UIPI) */
    RUN_PLACE_FAILED,  /* window found, placement reported a real failure */
    RUN_MONITOR_GONE   /* the monitor this slot was recorded on is not connected
                        * (stage 7). Deliberately NOT folded into RUN_SKIPPED:
                        * nothing is wrong with the configuration, and what the
                        * user has to do about it is different - plug the screen
                        * back in, or rebind the slot */
} SlotRunResult;

typedef struct {
    SlotRunResult result;
    DWORD   pid;                 /* 0 when the process was never identified */
    HWND    hwnd;                /* 0 when no window was found */
    wchar_t note[160];           /* one line, for the log and the status bar */
} SlotRunReport;

typedef struct {
    SlotRunReport slots[WSS_SLOT_MAX];
    int count;
    int ok;           /* RUN_OK */
    int denied;       /* RUN_DENIED */
    int failed;       /* RUN_NOT_FOUND, RUN_TIMEOUT, RUN_PLACE_FAILED */
    int skipped;      /* RUN_SKIPPED */
    int monitor_gone; /* RUN_MONITOR_GONE - counted apart from skipped so the
                       * caller can say "a screen is missing" instead of
                       * "your configuration is broken" */
} WorkspaceRunReport;

/* Progress: a complete, unterminated line. NULL silences the module. */
typedef void (*WorkspaceRunTrace)(void *ctx, const wchar_t *line);

/*
 * Run `w`'s slots, resolving references against `rules`.
 *
 * `group` may be NULL. When it is not, every process that could be identified
 * is added to it, so the caller can end the whole workspace afterwards.
 *
 * The report is always filled in, one entry per slot, in SLOT ORDER (not
 * completion order) so the caller can pair it with the configuration.
 */
WorkspaceRunReport workspace_run(const Workspace *w, const RuleSet *rules,
                                 LaunchGroup *group,
                                 WorkspaceRunTrace trace, void *ctx);

/* Stable English name, for logs. Never NULL. */
const wchar_t *slot_run_result_name(SlotRunResult r);

#ifdef __cplusplus
}
#endif

#endif /* WAC_WORKSPACE_RUN_H */
