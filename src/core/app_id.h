/*
 * app_id.h - naming an application, so a rule can outlive a window handle.
 *
 * THE PROBLEM
 *
 *   A window handle is valid until the process exits, and is recycled after
 *   that. "Put THIS window here" therefore cannot be stored as an HWND: the
 *   next launch has a different one, and a stale one may now belong to an
 *   unrelated program.
 *
 *   What the user actually means is "every time I open the editor, put it
 *   here". So the identity is the EXECUTABLE, and the window is only how it
 *   is found.
 *
 * WHY THERE ARE SIX FIELDS AND NOT ONE
 *
 *   No single field identifies an application correctly in every case:
 *
 *     exe name    "notepad.exe"  - survives a reinstall or a different path,
 *                                  but two installs share it
 *     exe path    full path      - distinguishes installs, but breaks when the
 *                                  application updates into a new directory
 *     class name  "Notepad"      - survives a rename of the executable, but is
 *                                  not unique across vendors
 *     title       substring      - the only way to tell two windows of the
 *                                  SAME process apart (two documents)
 *     pid         one process    - exact, but dies with the process; useful
 *                                  when this program launched it
 *     hwnd        one window     - exact, for immediate operations only
 *
 *   A rule sets the fields it cares about and leaves the rest EMPTY, which
 *   means "any". An empty field is a wildcard, so the struct doubles as a
 *   selector: app_id_match() is used both to find a window to place now and,
 *   later, to find it again after a restart.
 *
 * ORDER OF PRECEDENCE
 *
 *   hwnd, then pid, then the string fields. The first two are exact and
 *   short-lived; when either is set the rest is ignored, because a caller
 *   that has a live handle is not asking a question that needs a heuristic.
 *
 *   An EMPTY rule matches NOTHING. A rule that matches everything would be a
 *   disaster in a tool that moves windows, so the empty case is defined as
 *   no-match rather than as a wildcard - it turns "the config file failed to
 *   load" into "nothing happened" instead of "every window on the desktop was
 *   rearranged".
 */

#ifndef WAC_APP_ID_H
#define WAC_APP_ID_H

#include <windows.h>

#include "win_enum.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    wchar_t exe_name[WAC_EXE_MAX];     /* file name only, e.g. "notepad.exe" */
    wchar_t exe_path[WAC_EXE_MAX];     /* full path, one specific install */
    wchar_t class_name[WAC_CLASS_MAX]; /* window class, e.g. "Notepad" */
    wchar_t title_sub[WAC_TITLE_MAX];  /* case-insensitive caption substring */
    DWORD   pid;                       /* one specific process, 0 = any */
    HWND    hwnd;                      /* one specific window, NULL = any */
} AppId;

/* True when nothing is set, i.e. the rule selects nothing. */
BOOL app_id_empty(const AppId *id);

/*
 * Build a durable identity for a window: its executable's file name plus its
 * window class.
 *
 * The full path is deliberately NOT copied, even though it was read. Packaged
 * applications live under a directory containing their version number
 * (Microsoft.WindowsNotepad_11.2607.14.0_x64__8wekyb3d8bbwe), so a stored
 * path stops matching the moment the application updates. A caller that needs
 * to separate two installs of the same program must set exe_path itself, with
 * app_id_set_exe().
 *
 * Returns FALSE for a NULL argument or a window whose executable path could
 * not be read (a protected process); the caller should report that rather
 * than fall back to the pid, which will not survive a restart.
 */
BOOL app_id_from_window(const WindowInfo *w, AppId *out);

/*
 * Set the executable part of a rule from user input, which may be either a
 * bare file name or a full path.
 *
 * Contains a separator -> exe_path AND exe_name are both set, and matching
 * requires the path to be equal. No separator -> only exe_name is set, so any
 * install of that program matches.
 *
 * A path given here must be the path as Windows reports it (the target of any
 * symlink or 8.3 name is what QueryFullProcessImageNameW returns), otherwise
 * the exact comparison will not match. Case is ignored, which is correct for
 * the default case-insensitive Windows filesystems.
 */
void app_id_set_exe(AppId *id, const wchar_t *exe_or_name);

/*
 * Does this window satisfy this rule?
 *
 * Every field that is set must match; every field that is empty is skipped.
 * An empty rule returns FALSE. A NULL argument returns FALSE.
 *
 * The window's exe_path must already be filled in for the exe fields to be
 * compared - pass WIN_ENUM_NEED_EXE to win_enum_top(), or call win_exe_path()
 * first.
 */
BOOL app_id_match(const AppId *rule, const WindowInfo *w);

/*
 * Human-readable one-line description for logs and the CLI, e.g.
 * "notepad.exe" or "pid=8124" or "hwnd=0x00000000001234AB".
 *
 * Never fails: writes an empty string when it cannot describe the rule, and
 * always NUL-terminates. Returns the number of characters written, excluding
 * the terminator.
 */
int app_id_describe(const AppId *id, wchar_t *out, size_t cch);

#ifdef __cplusplus
}
#endif

#endif /* WAC_APP_ID_H */
