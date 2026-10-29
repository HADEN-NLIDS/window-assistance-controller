/*
 * iconcache.h - the sidebar's application icons, with a hard ceiling.
 *
 * WHY IT EXISTS
 *
 *   The sidebar shows every installed program, and a list of names with no
 *   icons is a list you have to read. Icons are also the one thing on that
 *   screen that cannot be produced from data the program already has: each one
 *   is a file read plus a shell lookup (see the privacy note below).
 *
 *   Both halves of that matter:
 *
 *     - an icon costs a FILE READ of the program it belongs to, so it must be
 *       fetched once and kept, never once per frame;
 *     - a machine has hundreds of programs while the panel draws forty rows,
 *       so what is kept must have a CEILING, or the list you scrolled through
 *       an hour ago is still holding a hundred handles.
 *
 * WHY THE LOADER IS INJECTED RATHER THAN CALLED HERE
 *
 *   Everything below except iconcache_shell_load() is bookkeeping: find, keep,
 *   evict, count. That part is what the acceptance item is about ("more than
 *   128 entries must evict correctly and leak nothing"), and it is the part
 *   that can be tested exactly - with a loader that hands out numbered fake
 *   handles and counts every release. The product passes the shell's
 *   SHGetFileInfo.
 *
 * WHAT READING AN ICON TOUCHES - the privacy statement names this
 *   Reading a program's icon opens that program's file, and the shell resolves
 *   file associations and its own icon cache to do it.
 *   A new read has to be written down before it can be called expected.
 */

#ifndef WAC_ICONCACHE_H
#define WAC_ICONCACHE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The ceiling (acceptance item 13). Past this the least recently used entry is
 * dropped, so the memory a session spends on icons is bounded by the ceiling
 * and not by how far the user scrolled.
 */
#define ICONCACHE_MAX 128

/* Longest key kept: an executable path. Longer keys are not cached at all
 * rather than truncated - two programs whose paths agree for 260 characters
 * would then share one icon. */
#define ICONCACHE_KEY_MAX 260

/*
 * How an icon is obtained and released.
 *
 * `load` returns NULL when there is none; that answer is CACHED like any other,
 * because the alternative is asking the shell again on every repaint for every
 * row that has no icon (a shortcut to a document, a program that was
 * uninstalled) - which is exactly the per-frame file access this cache exists
 * to prevent.
 *
 * `free_fn` is called for every handle the cache holds, both on eviction and
 * on destroy/clear. A cache that forgets one leaks a USER object per session
 * and tests/t51_release.c is what notices.
 */
typedef HICON (*IconCacheLoadFn)(void *ctx, const wchar_t *key, int px);
typedef void  (*IconCacheFreeFn)(void *ctx, HICON icon);

typedef struct IconCache IconCache;

/*
 * `capacity` is clamped to [1, ICONCACHE_MAX]; `px` is the size the icons are
 * wanted at, passed on to the loader. `load` may not be NULL; `free_fn` may be
 * NULL when the loader returns handles nobody owns.
 *
 * Returns NULL when the allocation fails (the sidebar then draws no icons -
 * see the note about degradation in iconcache_get).
 */
IconCache *iconcache_create(int capacity, int px,
                            IconCacheLoadFn load, IconCacheFreeFn free_fn,
                            void *ctx);

/* Releases every handle it still holds. Safe on NULL, safe twice. */
void iconcache_destroy(IconCache *ic);

/* Drop everything but keep the cache usable - for a dpi change, where every
 * icon was rasterized at the wrong size. */
void iconcache_clear(IconCache *ic);

/*
 * The icon for `key`, loading it if this is the first time it has been asked
 * for. NULL when there is none (or when the cache is NULL), and the caller
 * draws its row without one: a missing icon is a smaller problem than a list
 * that will not draw.
 *
 * The handle belongs to the CACHE. The caller must not destroy it, and must
 * not keep it past the next call for a different key - it may be the one that
 * gets evicted.
 */
HICON iconcache_get(IconCache *ic, const wchar_t *key);

/* Bookkeeping, for the log and for tests/t71_iconcache.c. */
int iconcache_count(const IconCache *ic);
int iconcache_capacity(const IconCache *ic);
int iconcache_hits(const IconCache *ic);
int iconcache_misses(const IconCache *ic);
int iconcache_evictions(const IconCache *ic);
int iconcache_failures(const IconCache *ic);

/* ------------------------------------------------------------------ */
/* The loader the product uses                                         */
/* ------------------------------------------------------------------ */

/*
 * The shell's icon for `key`, or NULL.
 *
 * SHGFI_LARGEICON (32x32) rather than the small one: the sidebar draws at
 * 20 px on a 100% screen and 30 px on a 150% one, and DrawIconEx scales - so
 * asking for the 16x16 version would mean upscaling, which is where a scaled
 * icon starts to look wrong.
 *
 * The returned handle is owned by the caller and is destroyed by
 * iconcache_shell_free().
 */
HICON iconcache_shell_load(void *ctx, const wchar_t *key, int px);
void  iconcache_shell_free(void *ctx, HICON icon);

#ifdef __cplusplus
}
#endif

#endif /* WAC_ICONCACHE_H */
