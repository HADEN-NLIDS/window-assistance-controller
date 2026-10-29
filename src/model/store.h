/*
 * store.h - INI persistence with atomic writes, backups and a checksum.
 *
 * WHY THIS MODULE IS CAREFUL
 *
 * The config file is the only persistent state this program owns, and the
 * worst possible failure is losing it: a user who has laid out half a dozen
 * workspaces would have to redo all of it, with no explanation. Two things
 * make that failure likely if the code is naive:
 *
 *   1. Monitor changes - and therefore config saves - happen during
 *      sleep/wake, display reconfiguration and driver resets, which is
 *      exactly when a machine is most likely to lose power or crash. A
 *      non-atomic write can leave a half-written file.
 *
 *   2. The file is plain text that users are invited to hand-edit. It can
 *      arrive truncated, mis-encoded, or with a stray character.
 *
 * So every write goes through a temp file + MoveFileEx(REPLACE_EXISTING),
 * a rolling set of backups is kept, and the content carries a checksum that
 * lets a load detect damage and fall back to a backup instead of silently
 * starting from defaults.
 *
 * FORMAT
 *
 *   Plain INI. Deliberately not JSON: it needs no library, it is ~150 lines
 *   to parse, a human can fix it in Notepad, and it diffs cleanly.
 *
 *     [meta]
 *     version=1
 *     checksum=0x8F3A21C7
 *
 *     [workspace:coding]
 *     name=Code
 *
 *     [slot:coding:0]
 *     app_id=code.exe
 *     x=0.0000
 *
 * The checksum covers everything except its own line, so it can be verified
 * without knowing the file's final size.
 */

#ifndef WAC_STORE_H
#define WAC_STORE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- limits ------------------------------------------------------ */

#define STORE_LINE_MAX    1024   /* longest single line, including newline */
#define STORE_KEY_MAX     64
#define STORE_VALUE_MAX   768
#define STORE_MAX_SECTIONS 256
#define STORE_MAX_KEYS     128   /* per section */

/* How many rotating backups to keep alongside the file. */
#define STORE_BACKUP_COUNT 3

/*
 * SIZING WARNING - READ BEFORE ADDING A STORE HANDLE TO A STACK FRAME
 *
 * A fully populated document is about 26 MB:
 *
 *     256 sections x (64-byte name + 128 keys x (64 + 768) bytes) = 26.0 MB
 *
 * The default thread stack is 1 MB. A StoreDocument local variable
 * therefore overflows the stack the instant it is declared - measured:
 * the test program died with 0xC00000FD (STACK_OVERFLOW) before printing
 * a single line.
 *
 * The type is OPAQUE for exactly this reason: callers cannot declare one on
 * the stack even by accident, and must go through store_create(), which
 * allocates from the heap. Do not expose the internals.
 *
 * For the product's real workload (a handful of workspaces) a document uses
 * a few kilobytes. The arrays are sized for the pathological case so that a
 * malformed file can never cause an allocation failure mid-parse.
 */
typedef struct StoreDocument StoreDocument;

/* Allocate an empty document. Returns NULL on allocation failure.
 * The result must be released with store_destroy(). */
StoreDocument *store_create(void);
void           store_destroy(StoreDocument *doc);

/*
 * Open the file for writing: load it if it exists, otherwise return a fresh
 * empty document. Returns NULL only when a document cannot be allocated.
 *
 * EVERY WRITER OF THE SHARED FILE MUST GO THROUGH THIS.
 *
 * The configuration file holds sections owned by different modules:
 * [settings] and [workspace:*] belong to the main window, [rule:*] belongs to
 * the rule module. A writer that builds a NEW document and saves it overwrites
 * the whole file, so every section it does not know about is deleted.
 *
 * That is not hypothetical. save_all() did exactly that, and because it runs
 * unconditionally on exit, closing the program normally deleted every window
 * preset. Measured 2026-09-23: a preset anchored in the UI was gone after a
 * restart, and nothing said so - the user just found the preset missing.
 *
 * The correct shape is always:
 *
 *     StoreDocument *doc = store_open_for_write(path);
 *     if (doc == NULL) return;              // out of memory
 *     ... store_set() only the sections you own ...
 *     store_save(path, doc);
 *     store_destroy(doc);
 *
 * Sections you own must be written through a function that also REMOVES the
 * ones you own but no longer want (see ws_to_store() and rules_to_store()):
 * keeping a stale section and re-adding it are both failures.
 *
 * A file that is missing, unreadable, or damaged beyond backup recovery also
 * yields an empty document. The caller has been told about the damage by then,
 * and refusing to save would leave the program unable to persist anything.
 */
StoreDocument *store_open_for_write(const wchar_t *path);

/* ---- result of a load -------------------------------------------- */

typedef enum {
    STORE_OK = 0,               /* file read and checksum verified */
    STORE_OK_NO_CHECKSUM,       /* file read; no checksum present (older or hand-made) */
    STORE_MODIFIED_EXTERNALLY,  /* file parses, but its checksum does not match -
                                 * something other than this program wrote it.
                                 * The CONTENTS ARE USABLE and are returned; the
                                 * caller should say so and carry on. */
    STORE_RECOVERED_FROM_BACKUP,/* main file damaged; a backup was loaded instead */
    STORE_CORRUPT_NO_RECOVERY,  /* damaged and no usable backup; caller decides */
    STORE_MISSING,              /* file does not exist yet - NOT an error */
    STORE_OUT_OF_MEMORY         /* could not allocate the document */
} StoreLoadResult;

/* ---- API --------------------------------------------------------- */

/*
 * Load and parse a file into a freshly allocated document.
 *
 * On success *out receives a document the caller must release with
 * store_destroy(). On failure *out is NULL.
 *
 * THE THREE "USABLE" RESULTS, AND WHY THERE ARE THREE
 *
 *   STORE_OK                     written by this program, checksum verified
 *   STORE_OK_NO_CHECKSUM         no checksum line at all (hand-made file)
 *   STORE_MODIFIED_EXTERNALLY    checksum present but stale: the file was
 *                                edited by something else
 *
 *   The last one used to be treated as damage, and that made hand editing
 *   useless in a way nobody could diagnose: a user who added a [workspace:*]
 *   section with Notepad saw it silently discarded and their old configuration
 *   loaded from a backup instead. Measured 2026-09-22.
 *
 *   What the checksum is FOR is catching a half-written file. Atomic writing
 *   (temp file + MoveFileEx) already rules that out for anything this program
 *   writes, and a file that parses cleanly has demonstrably not been truncated.
 *   So a stale checksum is reported and the contents are used.
 *
 *   Real damage - a file that does not parse - still takes the backup path.
 *
 * On STORE_RECOVERED_FROM_BACKUP the document holds the backup's contents
 * and the caller should tell the user. On STORE_MISSING *out is NULL and
 * it is the caller's job to start from defaults - this is the normal
 * first-run path, not a failure.
 */
StoreLoadResult store_load(const wchar_t *path, StoreDocument **out);

/*
 * Write the document atomically.
 *
 * Sequence: rotate a backup of the current file, write "<path>.tmp",
 * flush it to disk, then MoveFileEx(REPLACE_EXISTING|WRITE_THROUGH).
 * A crash at any point leaves either the old file or the new one - never
 * a half-written one.
 */
BOOL store_save(const wchar_t *path, StoreDocument *doc);

/*
 * Recompute and store the checksum for a document.
 * Called automatically by store_save; exposed so tests can inspect it.
 */
void store_stamp_checksum(StoreDocument *doc);

/* ---- document access --------------------------------------------- */

/*
 * Section access by index, for iteration.
 * store_section_count() returns the number of sections currently present.
 * store_section_at() returns NULL when the index is out of range, and
 * store_section_name() returns NULL for a NULL section.
 *
 * Iteration needs these because a section is opaque too: the product walks
 * sections to build its workspace list rather than reaching into arrays.
 */
int           store_section_count(const StoreDocument *doc);
void         *store_section_at(const StoreDocument *doc, int index);
const char   *store_section_name(const void *section);
int           store_section_key_count(const void *section);
const char   *store_section_key_at(const void *section, int index);
const char   *store_section_value_at(const void *section, int index);

BOOL  store_has_section(const StoreDocument *doc, const char *name);
void  store_remove_section(StoreDocument *doc, const char *name);

const char *store_get(const StoreDocument *doc, const char *section,
                      const char *key, const char *fallback);
void        store_set(StoreDocument *doc, const char *section,
                      const char *key, const char *value);

/* Typed helpers - parse with a fallback, so a malformed value can never
 * crash the program. */
int   store_get_int(const StoreDocument *doc, const char *section,
                    const char *key, int fallback);
float store_get_float(const StoreDocument *doc, const char *section,
                      const char *key, float fallback);

/* Format a float the way this file writes them: 4 decimals, locale-safe. */
void store_format_float(char *out, size_t out_size, float v);

/* ---- maintenance ------------------------------------------------- */

/*
 * Human-readable path of the Nth backup (0 = most recent).
 * Returns FALSE when n is out of range.
 */
BOOL store_backup_path(const wchar_t *path, int n,
                       wchar_t *out, size_t out_cch);

/* Remove every file this module creates for `path` (main, tmp, backups).
 * Used by tests; the product never calls it. */
void store_delete_all(const wchar_t *path);

/* FNV-1a over a byte range. Exposed so tests can verify the checksum is
 * actually sensitive to a one-byte change. */
unsigned store_checksum_bytes(const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* WAC_STORE_H */
