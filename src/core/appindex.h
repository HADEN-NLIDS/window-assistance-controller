/*
 * appindex.h - the list of programs installed on this machine.
 *
 * WHAT IT IS FOR
 *
 *   Stage 8's side bar: a list of applications that are NOT running, so a
 *   preset can be created for something without starting it first. Until this
 *   existed, [Create preset] could only offer windows that were already open
 *   (STR_MSG_CREATE_MORE_TODO said so).
 *
 * WHAT IT READS - AND WHAT IT DELIBERATELY DOES NOT
 *
 *   Three sources, all of them already named in the privacy statement
 *   ("the installed-application list, read once when you open the side bar"):
 *
 *     HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths    (exe paths)
 *     HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall    (names)
 *     HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall    (names)
 *     the Start Menu FOLDER LISTING                              (names)
 *
 *   A shortcut's NAME is read, and - since 2026-10-10, with the user's
 *   approval - its TARGET PATH, so the entry can actually be launched.
 *
 *   That last one is a change to what this program reads, and it is written
 *   into the privacy statement (§1.3) rather than buried here: listing the
 *   Start Menu by name alone produced 413 entries of which 328 could be shown
 *   but never started, which makes a side bar pointless. The shortcut file is
 *   opened and only its target path is taken from it; the file's contents are
 *   not interpreted, and nothing is written anywhere. A shortcut pointing at
 *   something that is not an .exe keeps an empty exe_path and is still listed.
 *
 *   Everything is opened KEY_READ. Nothing here writes a registry value, and
 *   nothing here touches the network.
 *
 * WHEN IT RUNS
 *
 *   NEVER at start-up (§8.6 acceptance: "no index scan on start-up"). The user
 *   opens the side bar, the scan runs on a worker thread, the result is cached
 *   on disk, and the next time the cache is used if it is still valid.
 *
 * WHY THE STRUCTURE IS OPAQUE
 *
 *   A thousand entries at ~800 bytes each is most of a megabyte, which must not
 *   be copied by assignment or put in a stack frame (TRAPS.md #11).
 *   Same reasoning as StoreDocument: allocate it, pass the pointer.
 */

#ifndef WAC_APPINDEX_H
#define WAC_APPINDEX_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APPINDEX_NAME_MAX  128
#define APPINDEX_PATH_MAX  260      /* MAX_PATH, without pulling in the macro */

/* Where an entry came from. Kept in the data because "this one has no path"
 * means something different for each source. */
typedef enum {
    APPINDEX_FROM_APPPATHS = 0,   /* an exe path, the best kind of entry */
    APPINDEX_FROM_UNINSTALL,      /* a product name, path often missing */
    APPINDEX_FROM_STARTMENU       /* a shortcut name, path always missing */
} AppIndexSource;

typedef struct {
    wchar_t        name[APPINDEX_NAME_MAX];
    wchar_t        exe_path[APPINDEX_PATH_MAX];   /* empty = listed, not launchable */
    AppIndexSource source;
} AppIndexItem;

typedef struct AppIndex AppIndex;   /* opaque: see the note above */

AppIndex *appindex_create(void);
void      appindex_destroy(AppIndex *idx);

int  appindex_count(const AppIndex *idx);
const AppIndexItem *appindex_at(const AppIndex *idx, int index);

/*
 * Add one entry. Used by the scanners and by the tests; the cache loader fills
 * the array directly.
 *
 * Duplicates are NOT filtered here - appindex_dedup() does that once, after
 * every source has been read, so the rule lives in exactly one place.
 *
 * Returns FALSE when the index is full or the name is empty.
 */
BOOL appindex_add(AppIndex *idx, const wchar_t *name, const wchar_t *exe_path,
                  AppIndexSource source);

/*
 * Remove duplicate executables and sort by name. Pure: no I/O, no registry,
 * no window - which is what lets the interesting half (which of two entries
 * for the same exe survives, and how names are compared) be unit-tested.
 *
 * An entry WITH a path beats one without when both describe the same program:
 * the one with a path can be launched, and the Start Menu copy of a program is
 * usually the one that is missing it.
 *
 * Returns the number of entries removed.
 */
int appindex_dedup(AppIndex *idx);

/* ------------------------------------------------------------------ */
/* Scanning - the OS-touching part                                     */
/* ------------------------------------------------------------------ */

/*
 * Read the three sources. Blocking: it walks two registry trees and lists
 * Start Menu folders, so it belongs on a worker thread (see AppIndexJob below)
 * and never on the UI thread.
 *
 * Returns how many entries were added.
 */
int appindex_scan(AppIndex *idx);

/*
 * The value that says whether a cached index is still current: the newest
 * last-write time of the Start Menu folders, in FILETIME ticks.
 *
 * The registry offers no such thing, so a program installed WITHOUT adding a
 * Start Menu entry is picked up on the next scan rather than immediately. That
 * is a deliberate trade: the alternative is re-scanning the registry on every
 * open, which is the behaviour §7.3.2 calls out as suspicious.
 *
 * Returns 0 when the folders cannot be read, which callers treat as "always
 * rescan".
 */
unsigned long long appindex_stamp(void);

/* ------------------------------------------------------------------ */
/* Cache                                                               */
/* ------------------------------------------------------------------ */

/*
 * The cache lives in <config_dir>\appindex.ini - the program's own directory,
 * never next to the exe and never in the registry (§7.3.2).
 *
 * Load returns FALSE when there is no cache, when it is unreadable, or when
 * its stamp does not match `stamp`. Every FALSE means "scan", never "empty
 * list": a missing cache and an empty machine are different answers.
 */
BOOL appindex_cache_load(AppIndex *idx, unsigned long long stamp);
BOOL appindex_cache_save(const AppIndex *idx, unsigned long long stamp);

/* ------------------------------------------------------------------ */
/* Doing it without freezing the window                               */
/* ------------------------------------------------------------------ */

typedef struct AppIndexJob AppIndexJob;

/*
 * Start a worker thread that loads the cache or scans, whichever the stamp
 * says. When it finishes it posts `notify_msg` to `notify_wnd` - the same
 * shape winwatch uses, so the window never blocks and never polls.
 *
 * Returns NULL when the thread could not be created; the caller then reports
 * "indexing is unavailable" rather than pretending an empty list is the truth.
 *
 * The worker exits when it is done, so the process thread count falls back on
 * its own (§8.6 acceptance: "at most 3 threads once indexing is done").
 */
AppIndexJob *appindex_job_start(HWND notify_wnd, UINT notify_msg);

/*
 * Take the finished index, or NULL while it is still running. Ownership of the
 * AppIndex moves to the caller, who destroys it with appindex_destroy().
 *
 * Safe to call any number of times; once taken, it keeps returning NULL.
 */
AppIndex *appindex_job_take(AppIndexJob *job);

/* Milliseconds the last run took: 0 while it is still running. This is what
 * the "indexing..." line reports, and what §8.6 asks to be recorded. */
unsigned long appindex_job_elapsed_ms(const AppIndexJob *job);

/* TRUE once the worker has finished (successfully or not). */
BOOL appindex_job_done(const AppIndexJob *job);

/* Stop waiting and release everything. Safe while the worker is still running:
 * the thread is told to finish and is joined before this returns. */
void appindex_job_destroy(AppIndexJob *job);

#ifdef __cplusplus
}
#endif

#endif /* WAC_APPINDEX_H */
