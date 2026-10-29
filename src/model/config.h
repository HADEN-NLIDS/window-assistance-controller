/*
 * config.h - where this program keeps its state.
 *
 * Extracted from main.c when the placement rules needed the same path. Two
 * copies of "which file do we own" is exactly the kind of duplication that
 * ends with the GUI and the command line writing to different files.
 *
 * THE LOCATION IS NOT NEGOTIABLE
 *
 *   %APPDATA%\WindowAssistanceController\
 *
 *   Never the directory holding the exe. When the program runs from a USB
 *   stick, a network share, or Program Files, writes next to the executable
 *   are redirected to the VirtualStore by UAC - the user then reports that
 *   their settings vanished, and the file is nowhere they would think to
 *   look. See docs/安全设计.md section 3.5.
 */

#ifndef WAC_CONFIG_H
#define WAC_CONFIG_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The configuration directory, created if it does not exist.
 *
 * Returns FALSE when %APPDATA% cannot be resolved or the directory cannot be
 * created. Callers must treat that as "run without persistence", not as a
 * fatal error - the program is still useful without a config file.
 *
 * `out` receives a NUL-terminated path with no trailing separator.
 */
BOOL config_dir(wchar_t *out, size_t cch);

/*
 * The main INI file: <config_dir>\workspaces.ini
 *
 * Despite the name it holds everything this program persists, including the
 * [rule:*] sections that record "this program always opens here". The name is
 * kept as it is rather than renamed: it is what stage 2 shipped, a rename
 * would silently orphan a user's existing file, and the cost of a slightly
 * stale name is lower than the cost of one lost configuration.
 *
 * Does NOT create the file.
 */
BOOL config_file(wchar_t *out, size_t cch);

#ifdef __cplusplus
}
#endif

#endif /* WAC_CONFIG_H */
