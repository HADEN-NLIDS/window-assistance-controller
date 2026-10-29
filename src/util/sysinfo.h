/*
 * sysinfo.h - facts about this machine and this process.
 *
 * WHY IT IS A MODULE OF ITS OWN
 *
 *   Three places need the same answers and none of them owns the question:
 *   the status bar shows whether the program runs elevated, the settings page
 *   repeats it with an explanation, and the user-guide page opens the startup
 *   folder. Writing the token check twice is how the status bar and the
 *   settings page would eventually disagree about the same process.
 *
 * WHAT IT IS NOT
 *
 *   Not a general "system information" dump. Every function here answers a
 *   question some screen actually asks. Nothing is cached: the elevation of a
 *   process cannot change, but caching it would be a second source of truth
 *   for something Windows already stores.
 */

#ifndef WAC_SYSINFO_H
#define WAC_SYSINFO_H

#include <windows.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Is this process running elevated?
 *
 * Read from the process token, not from IsUserAnAdmin(): the latter is
 * deprecated, and the token answers the question that actually matters here -
 * whether Windows will let us move the window of an elevated process at all
 * (UIPI; see docs/架构与数据结构.md section 7.5).
 *
 * Never fails. An unreadable token reports "not elevated", which is the safe
 * answer: nothing privileged is attempted on the strength of it, and the
 * wording shown to the user stays on the cautious side.
 */
BOOL sysinfo_is_elevated(void);

/*
 * The current user's Startup folder, so the guide can open it in Explorer.
 *
 * Returns FALSE when the shell cannot resolve it, in which case `out` is an
 * empty string - the caller shows the manual route (Win+R, "shell:startup")
 * rather than an empty path.
 */
BOOL sysinfo_startup_folder(wchar_t *out, size_t cch);

/*
 * Open a folder in the shell. FALSE when the path is unusable or the shell
 * refused; the return value of ShellExecuteW is the documented one (> 32 is
 * success), not a guess.
 */
BOOL sysinfo_open_folder(HWND owner, const wchar_t *path);

#ifdef __cplusplus
}
#endif

#endif /* WAC_SYSINFO_H */
