/*
 * iconcache.c - see iconcache.h for what this module owns and why the loader
 * is injected.
 *
 * The bookkeeping below calls NO Win32 function at all - not even DestroyIcon:
 * releasing a handle is the loader's counterpart and the caller passes it in.
 * That is what lets tests/t71_iconcache.c drive eviction and release with
 * numbered fake handles and count every one of them.
 *
 * iconcache_shell_load()/iconcache_shell_free() at the bottom are the only
 * functions here that talk to the system, and they are the product's pair.
 */

#include "iconcache.h"

#include <shellapi.h>   /* SHGetFileInfoW */
#include <stdlib.h>     /* calloc / free - see trap 30: without this MSVC
                         * assumes calloc returns int and truncates the
                         * pointer on x64 */
#include <string.h>     /* wcsncpy_s, _wcsicmp */

struct IconCache {
    int capacity;
    int px;

    IconCacheLoadFn load;
    IconCacheFreeFn free_fn;
    void           *ctx;

    /*
     * The slots, in no particular order.
     *
     * `icon` NULL means "asked, and there is none" - a real answer that is
     * worth keeping (see iconcache.h): without it, every row whose program has
     * no icon would ask the shell again on every single repaint.
     */
    struct {
        BOOL               used;
        wchar_t            key[ICONCACHE_KEY_MAX];
        HICON              icon;
        unsigned long long used_at;
    } slots[ICONCACHE_MAX];

    int                count;
    unsigned long long clock;      /* bumped on every touch */

    int hits;
    int misses;
    int evictions;
    int failures;
};

IconCache *iconcache_create(int capacity, int px,
                            IconCacheLoadFn load, IconCacheFreeFn free_fn,
                            void *ctx)
{
    if (load == NULL) return NULL;

    if (capacity < 1) capacity = 1;
    if (capacity > ICONCACHE_MAX) capacity = ICONCACHE_MAX;
    if (px < 1) px = 1;

    /*
     * Allocated, never a local: the slots are ICONCACHE_MAX * ~540 bytes,
     * which is 68 KB - most of a default stack frame (TRAPS.md #11,
     * where a 26 MB StoreDocument on the stack was 0xC00000FD).
     */
    IconCache *ic = (IconCache *)calloc(1, sizeof(*ic));
    if (ic == NULL) return NULL;

    ic->capacity = capacity;
    ic->px       = px;
    ic->load     = load;
    ic->free_fn  = free_fn;
    ic->ctx      = ctx;
    return ic;
}

/* Release one slot's handle and mark it free. */
static void slot_release(IconCache *ic, int index)
{
    if (!ic->slots[index].used) return;

    if (ic->slots[index].icon != NULL && ic->free_fn != NULL) {
        ic->free_fn(ic->ctx, ic->slots[index].icon);
    }
    ic->slots[index].icon    = NULL;
    ic->slots[index].used    = FALSE;
    ic->slots[index].key[0]  = L'\0';
    ic->slots[index].used_at = 0;
    if (ic->count > 0) ic->count--;
}

void iconcache_clear(IconCache *ic)
{
    if (ic == NULL) return;

    for (int i = 0; i < ICONCACHE_MAX; ++i) {
        slot_release(ic, i);
    }
    ic->count = 0;
}

void iconcache_destroy(IconCache *ic)
{
    if (ic == NULL) return;

    /* Every handle the cache still holds goes back through the caller's
     * release function - the one place a leak could hide, and the reason
     * tests/t71_iconcache.c counts the releases. */
    iconcache_clear(ic);
    free(ic);
}

/* The least recently used slot in use, or -1 when none is. */
static int victim_index(const IconCache *ic)
{
    int best = -1;

    /* Only the first `capacity` slots are ever used - see the note in
     * iconcache_get() about what ignoring that cost. */
    for (int i = 0; i < ic->capacity; ++i) {
        if (!ic->slots[i].used) continue;
        if (best < 0 || ic->slots[i].used_at < ic->slots[best].used_at) {
            best = i;
        }
    }
    return best;
}

HICON iconcache_get(IconCache *ic, const wchar_t *key)
{
    if (ic == NULL || key == NULL || key[0] == L'\0') return NULL;

    if (wcslen(key) >= ICONCACHE_KEY_MAX) {
        /* Not cached at all, and not loaded either: a key this long cannot be
         * stored, and loading it would mean loading it again every frame. */
        ic->failures++;
        return NULL;
    }

    for (int i = 0; i < ICONCACHE_MAX; ++i) {
        if (!ic->slots[i].used) continue;
        if (_wcsicmp(ic->slots[i].key, key) != 0) continue;

        ic->slots[i].used_at = ++ic->clock;
        ic->hits++;
        return ic->slots[i].icon;
    }

    /* A miss. Make room first: if the eviction happened after the load, a full
     * cache would briefly hold capacity+1 handles. */
    ic->misses++;

    int slot = -1;

    /*
     * Only the first `capacity` slots are candidates.
     *
     * MEASURED 2026-10-10, caught by tests/t71_iconcache.c: this loop used to
     * run to ICONCACHE_MAX, so a cache created with capacity 4 kept filling
     * slots 4, 5, 6... and never evicted anything. The ceiling held only
     * because the test that crossed it used the full 128 - the small-capacity
     * case is what exposed it, which is the whole reason that case is there.
     */
    for (int i = 0; i < ic->capacity; ++i) {
        if (!ic->slots[i].used) { slot = i; break; }
    }

    if (slot < 0) {
        /*
         * Full: drop the least recently used one. A slot is only ever "used"
         * when count says so, so a full cache always has a victim - and when it
         * somehow does not, refusing to evict is better than evicting a slot
         * that was never counted.
         */
        slot = victim_index(ic);
        if (slot < 0) return NULL;

        slot_release(ic, slot);
        ic->evictions++;
    }

    HICON icon = ic->load(ic->ctx, key, ic->px);

    ic->slots[slot].used    = TRUE;
    ic->slots[slot].icon    = icon;
    ic->slots[slot].used_at = ++ic->clock;
    wcsncpy_s(ic->slots[slot].key, ICONCACHE_KEY_MAX, key, _TRUNCATE);
    ic->count++;

    if (icon == NULL) ic->failures++;

    return icon;
}

int iconcache_count(const IconCache *ic)      { return (ic != NULL) ? ic->count : 0; }
int iconcache_capacity(const IconCache *ic)   { return (ic != NULL) ? ic->capacity : 0; }
int iconcache_hits(const IconCache *ic)       { return (ic != NULL) ? ic->hits : 0; }
int iconcache_misses(const IconCache *ic)     { return (ic != NULL) ? ic->misses : 0; }
int iconcache_evictions(const IconCache *ic)  { return (ic != NULL) ? ic->evictions : 0; }
int iconcache_failures(const IconCache *ic)   { return (ic != NULL) ? ic->failures : 0; }

/* ------------------------------------------------------------------ */
/* The shell                                                        */
/* ------------------------------------------------------------------ */

HICON iconcache_shell_load(void *ctx, const wchar_t *key, int px)
{
    (void)ctx;
    (void)px;

    if (key == NULL || key[0] == L'\0') return NULL;

    SHFILEINFOW sfi;
    ZeroMemory(&sfi, sizeof(sfi));

    /*
     * The path is passed as-is and the shell decides what to do with it: an
     * .exe gives its own icon, anything else gives the icon of its type. Both
     * are wanted - a row for a program that is listed but not launchable still
     * deserves to look like what it is.
     *
     * SHGFI_LARGEICON: 32x32, scaled down by DrawIconEx to whatever the sidebar
     * draws at. The small one is 16x16, which would have to be scaled UP on a
     * 150% screen.
     */
    if (SHGetFileInfoW(key, 0, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_LARGEICON) == 0) {
        return NULL;
    }

    return sfi.hIcon;
}

void iconcache_shell_free(void *ctx, HICON icon)
{
    (void)ctx;
    if (icon != NULL) {
        (void)DestroyIcon(icon);
    }
}
