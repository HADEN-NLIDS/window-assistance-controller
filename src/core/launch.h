/*
 * launch.h - start a program and find ITS window.
 *
 * WHY THIS IS NOT JUST CreateProcessW PLUS EnumWindows
 *
 *   "Take the first window that appears with the right process id" fails on
 *   most real applications, and it fails in a way that looks like the tool
 *   doing nothing:
 *
 *   1. PACKAGED APPLICATIONS. Windows Notepad, Calculator, Photos and every
 *      Store app start a launcher process that hands off and exits; the
 *      window belongs to a different process entirely. MEASURED (2026-09-20):
 *      `notepad.exe` returned pid 19672 and the window that appeared was not
 *      its. Waiting on that pid timed out after fifteen seconds with the
 *      window sitting on screen the whole time.
 *
 *   2. SPLASH SCREENS. VS Code, Office and most Java applications show one
 *      before the real window. Placing the first window that appears moves
 *      the splash, which then vanishes - and the real window opens wherever
 *      it wanted to, which again looks exactly like nothing happened.
 *
 *   3. AN ALREADY-RUNNING PROGRAM. Launching a second document of a program
 *      that is already open does not create a new process at all on some
 *      toolkits (Windows 11 Notepad is one process holding several windows).
 *
 *   So the search key is: THE EXECUTABLE NAME, plus NEWNESS against a
 *   snapshot taken before the launch. Both are required. The name alone would
 *   pick the window the user already had open; newness alone would pick
 *   whatever unrelated window happened to appear first.
 *
 * This module never writes to the console. Progress goes through a callback,
 * so the same code serves the GUI and the command line.
 */

#ifndef WAC_LAUNCH_H
#define WAC_LAUNCH_H

#include <windows.h>

#include "win_enum.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WAC_LAUNCH_CMD_MAX 1024

/*
 * How long a candidate window must keep the SAME rectangle before it is
 * accepted.
 *
 * This is the splash-screen filter. A splash that is about to be replaced
 * either disappears (so it stops being a candidate) or is resized by the
 * application as it finishes loading (so its rectangle changes). Either way
 * it fails to settle. A heuristic, not a guarantee - the timeout path says so
 * rather than pretending otherwise.
 */
#define WAC_LAUNCH_SETTLE_MS 300

typedef struct {
    wchar_t command[WAC_LAUNCH_CMD_MAX];  /* the command line as given */
    wchar_t exe_name[WAC_EXE_MAX];        /* derived, used to match windows */
    DWORD   pid;                          /* the process CreateProcessW returned,
                                           * for reporting only - see above */
    HWND    hwnd;                         /* what launch_wait() settled on */
} Launch;

/*
 * Progress reporting. `line` is a complete, unterminated message. Passing
 * NULL for the callback silences the module entirely.
 */
typedef void (*LaunchTrace)(void *ctx, const wchar_t *line);

/*
 * Pull the executable name out of a command line.
 *
 * Handles a quoted path and a bare token, strips the directory, and returns
 * the file name. An empty result means the caller should not try to match by
 * name. `out` is always NUL-terminated.
 */
void launch_derive_exe(const wchar_t *command, wchar_t *out, size_t cch);

/*
 * Photograph the desktop once, for a whole batch of launches.
 *
 * launch_start() takes its own snapshot, which is right for one program. A
 * WORKSPACE starts several before waiting for any of them, and every window it
 * opens has to count as new: a snapshot per program would put the earlier
 * programs' windows into the later snapshots, and those windows could then
 * never be found.
 *
 * Call once, then launch_prepare() + the launcher + launch_wait() per slot.
 */
void launch_snapshot_take(void);

/*
 * Fill `l` with what to look for, using the current snapshot. Starts nothing.
 *
 * This is the way to combine core/proc_launch.c with this module: proc_launch
 * keeps the executable and its arguments in separate fields and gets argv[0]
 * right (TRAPS.md #28), while the window search here needs the
 * snapshot and the file name. Neither is written twice.
 */
void launch_prepare(Launch *l, const wchar_t *exe_path);

/*
 * Photograph the desktop, then start `command`.
 *
 * The snapshot MUST happen before the process starts - it is what makes
 * "new" meaningful - so this function does both in that order. Do not split
 * them.
 *
 * Returns FALSE when the program could not be started, in which case `l` is
 * still filled with the derived name and pid 0 so the caller can report what
 * it tried.
 *
 * The child process handle is closed immediately: this module never waits on
 * the process, because for a packaged application the process it started is
 * not the one worth waiting for.
 */
BOOL launch_start(Launch *l, const wchar_t *command);

/*
 * Wait for the program's real main window and return it, or NULL on timeout.
 *
 * Search order, per poll:
 *   - a NEW window (absent from the pre-launch snapshot)
 *   - whose executable matches the derived name
 *   - that is visible, not cloaked, not a tool window, and at least 200x150
 *     (below that it is almost certainly a helper or a splash frame)
 *
 * The biggest such window wins, and it must hold its rectangle for
 * WAC_LAUNCH_SETTLE_MS before being accepted. On timeout the largest
 * candidate found anyway is returned, with a message saying the budget ran
 * out - a window that is present but slow to settle is a far better outcome
 * than "no window found".
 *
 * On success l->hwnd is filled. Safe to call with a NULL `l` (returns NULL).
 */
HWND launch_wait(Launch *l, int budget_ms, LaunchTrace trace, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* WAC_LAUNCH_H */
