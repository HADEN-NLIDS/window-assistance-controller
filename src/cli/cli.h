/*
 * cli.h - the command-line face of wac.exe.
 *
 * THE SUBSYSTEM PROBLEM
 *
 *   wac.exe is built as /SUBSYSTEM:WINDOWS, because it is a window
 *   application and must not flash a console window when double-clicked.
 *   A GUI-subsystem process starts with NO console, so anything it prints
 *   goes nowhere at all.
 *
 *   The documented way out is AttachConsole(ATTACH_PARENT_PROCESS): if the
 *   program was started from a command prompt, it joins that console and can
 *   print into it. cli_attach_console() does that and then points the C
 *   runtime's stdout/stderr at it, which is the part that is easy to forget -
 *   AttachConsole alone leaves the inherited (invalid) standard handles in
 *   place, so printf goes on writing into the void.
 *
 *   The known cost: cmd.exe does not wait for a GUI-subsystem process, so the
 *   prompt reappears before the output arrives and the text lands after it.
 *   That is inherent to the subsystem choice, not a bug here. Use
 *
 *       start /wait wac place ...
 *
 *   from a batch file when the ordering matters.
 *
 * WHY THE COMMAND CHECK COMES BEFORE THE SINGLE-INSTANCE MUTEX
 *
 *   `wac place --app notepad.exe` must work while the window is already open.
 *   The mutex exists to stop a SECOND WINDOW appearing; a command-line run
 *   opens no window at all, so it must not be turned away by it. main.c
 *   therefore asks cli_is_command() first, before it creates the mutex.
 */

#ifndef WAC_CLI_H
#define WAC_CLI_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Does this command line ask for a command rather than the window?
 *
 * argc/argv come from CommandLineToArgvW(GetCommandLineW()). No arguments at
 * all returns FALSE, which is what makes a double-click open the window.
 */
BOOL cli_is_command(int argc, wchar_t **argv);

/*
 * Attach to the parent console and route stdout/stderr into it.
 *
 * Falls back to AllocConsole() when there is no parent console, so a command
 * started from a shortcut still shows its output somewhere instead of losing
 * it. Returns FALSE only when neither worked - in which case the command
 * still runs, it just has nowhere to print.
 */
BOOL cli_attach_console(void);

/*
 * Run a command and return the process exit code.
 *
 * 0 = success, 1 = the command ran but did not achieve its goal (no matching
 * window, the target refused), 2 = the command line itself was wrong. A batch
 * file can rely on those three without parsing any output.
 *
 * Assumes cli_attach_console() has already been called.
 */
int cli_run(int argc, wchar_t **argv);

/* Print the command list. */
void cli_usage(void);

#ifdef __cplusplus
}
#endif

#endif /* WAC_CLI_H */
