/*
 * t71_iconcache.c - unit tests for src/ui/iconcache.c
 *
 * WHAT IT PROVES, AND HOW
 *
 *   Acceptance item 13 says "more than 128 entries must evict correctly and
 *   leak nothing". Leaking is the half that a green test usually fails to
 *   catch, so this test does not infer it from a handle count alone (the shell
 *   has caches of its own, and a count can move for reasons that are not ours).
 *   It counts the RELEASES:
 *
 *     - with a fake loader handing out numbered handles, every eviction and
 *       every destroy must hand exactly the right handle back;
 *     - with the real shell loader wrapped in counters, the number of handles
 *       created must equal the number released once the cache is destroyed.
 *
 *   The second one is exact: "loads - frees == 0" cannot be true while a
 *   handle is outstanding. The USER-object count is reported alongside it as
 *   supporting evidence, with a tolerance, because the shell legitimately
 *   keeps some of its own.
 *
 *   Everything except the last two sections uses the fake loader, so those
 *   assertions hold on any machine and do not depend on what is installed.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t71_iconcache
 *   tests\out\t71_iconcache.exe --no-pause
 */

#include "test_common.h"

#include "../src/ui/iconcache.h"

#include <stdlib.h>     /* _wtoi */

static int g_pass, g_fail;

static void ok(const wchar_t *what, int cond)
{
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls", what);
    }
}

static void ok_int(const wchar_t *what, int got, int want)
{
    if (got == want) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls  (got %d, want %d)", what, got, want);
    }
}

/* ------------------------------------------------------------------ */
/* The fake loader                                                     */
/* ------------------------------------------------------------------ */

/*
 * Handles are small integers wearing an HICON: nothing in the cache touches
 * what a handle IS, so a test can use whatever is convenient - and a number is
 * convenient, because a released handle identifies which entry was dropped.
 */
static int    g_loads;
static int    g_frees;
static HICON  g_last_freed;
static wchar_t g_last_key[ICONCACHE_KEY_MAX];

/* Keys of the form "k<N>" load as handle N+1; "kfail" loads as nothing. */
static int key_number(const wchar_t *key)
{
    if (key == NULL || key[0] != L'k') return -1;
    if (wcscmp(key, L"kfail") == 0) return -1;
    return _wtoi(key + 1);
}

static HICON fake_load(void *ctx, const wchar_t *key, int px)
{
    (void)ctx;
    (void)px;

    g_loads++;
    wcsncpy_s(g_last_key, ICONCACHE_KEY_MAX, key, _TRUNCATE);

    const int n = key_number(key);
    if (n < 0) return NULL;
    return (HICON)(INT_PTR)(n + 1);
}

static void fake_free(void *ctx, HICON icon)
{
    (void)ctx;
    g_frees++;
    g_last_freed = icon;
}

static void fake_reset(void)
{
    g_loads      = 0;
    g_frees      = 0;
    g_last_freed = NULL;
    g_last_key[0] = L'\0';
}

/* A cache over the fake loader, with the counters zeroed. */
static IconCache *fake_cache(int capacity)
{
    fake_reset();
    return iconcache_create(capacity, 20, fake_load, fake_free, NULL);
}

/* ------------------------------------------------------------------ */

static void test_basics(void)
{
    T_SEC(L"1. creating, and refusing what cannot work");

    ok(L"a loader is required", iconcache_create(8, 20, NULL, NULL, NULL) == NULL);

    IconCache *ic = fake_cache(8);
    ok(L"a cache can be created", ic != NULL);
    ok_int(L"with the capacity asked for", iconcache_capacity(ic), 8);
    ok_int(L"and nothing in it yet", iconcache_count(ic), 0);

    /* Dangerous input is clamped, not obeyed: a ceiling of zero would make
     * every get an eviction of nothing. */
    IconCache *tiny = iconcache_create(0, 20, fake_load, fake_free, NULL);
    ok(L"a zero capacity is still a cache", tiny != NULL);
    ok(L"clamped up to at least one", iconcache_capacity(tiny) >= 1);
    iconcache_destroy(tiny);

    IconCache *huge = iconcache_create(100000, 20, fake_load, fake_free, NULL);
    ok(L"a huge capacity is still a cache", huge != NULL);
    ok_int(L"and is clamped to the ceiling",
           iconcache_capacity(huge), ICONCACHE_MAX);
    iconcache_destroy(huge);

    ok(L"a NULL cache answers nothing", iconcache_get(NULL, L"k1") == NULL);
    ok(L"and an empty key is not a key", iconcache_get(ic, L"") == NULL);
    ok(L"nor is a NULL one", iconcache_get(ic, NULL) == NULL);
    ok_int(L"none of which counted as a miss", iconcache_misses(ic), 0);

    iconcache_destroy(ic);
    iconcache_destroy(NULL);       /* safe, and does nothing */
    ok(L"destroy is safe on NULL", 1);
}

static void test_load_once(void)
{
    T_SEC(L"2. an icon is fetched once, not once per frame");

    IconCache *ic = fake_cache(8);

    HICON a = iconcache_get(ic, L"k0");
    ok(L"the first ask loads it", a == (HICON)(INT_PTR)1);
    ok_int(L"one load", g_loads, 1);
    ok_int(L"one miss", iconcache_misses(ic), 1);
    ok_int(L"and nothing held before", iconcache_hits(ic), 0);

    /* The sidebar repaints constantly; a second ask must not touch the disk. */
    HICON b = iconcache_get(ic, L"k0");
    ok(L"the second ask returns the same handle", b == a);
    ok_int(L"without loading anything", g_loads, 1);
    ok_int(L"and counts as a hit", iconcache_hits(ic), 1);

    /* Case does not matter: Windows paths are case-insensitive, and the index
     * takes its paths from wherever the shell reported them. */
    HICON c = iconcache_get(ic, L"K0");
    ok(L"a differently-cased key is the same entry", c == a);
    ok_int(L"with no second load", g_loads, 1);
    ok_int(L"and one more hit", iconcache_hits(ic), 2);

    ok_int(L"one entry held", iconcache_count(ic), 1);
    iconcache_destroy(ic);
    ok_int(L"and it was released", g_frees, 1);
}

static void test_failure_cached(void)
{
    T_SEC(L"3. an entry with no icon is an answer worth keeping");

    IconCache *ic = fake_cache(8);

    ok(L"a key with no icon gives nothing", iconcache_get(ic, L"kfail") == NULL);
    ok_int(L"one attempt", g_loads, 1);

    /* The important half: the row that has no icon must not ask the shell
     * again on every repaint - a document shortcut or an uninstalled program
     * would otherwise mean a file access per frame. */
    ok(L"the second ask still gives nothing", iconcache_get(ic, L"kfail") == NULL);
    ok_int(L"but does not ask again", g_loads, 1);
    ok_int(L"it is a hit", iconcache_hits(ic), 1);
    ok_int(L"and it is remembered", iconcache_count(ic), 1);
    ok_int(L"as a failure", iconcache_failures(ic), 1);

    iconcache_destroy(ic);
    ok_int(L"nothing to release for it", g_frees, 0);
}

static void test_eviction(void)
{
    T_SEC(L"4. past the ceiling, the least recently used one goes");

    IconCache *ic = fake_cache(4);

    for (int i = 0; i < 4; ++i) {
        wchar_t key[16];
        _snwprintf_s(key, 16, _TRUNCATE, L"k%d", i);
        (void)iconcache_get(ic, key);
    }
    ok_int(L"four entries fill it", iconcache_count(ic), 4);
    ok_int(L"with four loads", g_loads, 4);
    ok_int(L"and no evictions", iconcache_evictions(ic), 0);

    /* Touch k0 and k2, so k1 is now the oldest. */
    (void)iconcache_get(ic, L"k0");
    (void)iconcache_get(ic, L"k2");

    (void)iconcache_get(ic, L"k4");
    ok_int(L"a fifth entry evicts one", iconcache_evictions(ic), 1);
    ok_int(L"the ceiling holds", iconcache_count(ic), 4);
    ok_int(L"five loads happened", g_loads, 5);
    ok_int(L"and one handle came back", g_frees, 1);
    ok(L"the handle released was k1's", g_last_freed == (HICON)(INT_PTR)2);

    /* k0 was touched, so it is still there - and asking again is a hit, which
     * is what "least recently used" is supposed to mean. */
    const int hits_before = iconcache_hits(ic);
    (void)iconcache_get(ic, L"k0");
    ok_int(L"the touched one survived", iconcache_hits(ic), hits_before + 1);

    /* k1 was evicted, so it comes back through the loader. */
    const int loads_before = g_loads;
    (void)iconcache_get(ic, L"k1");
    ok_int(L"the evicted one is loaded again", g_loads, loads_before + 1);
    ok_int(L"evicting another one", iconcache_evictions(ic), 2);
    ok_int(L"and releasing another handle", g_frees, 2);

    /* Destroy returns everything that is still held, and only that. */
    const int held = iconcache_count(ic);
    iconcache_destroy(ic);
    ok_int(L"destroy returns exactly what was held", g_frees, 2 + held);
    ok_int(L"loads minus releases is zero", g_loads - g_frees, 0);
}

static void test_clear(void)
{
    T_SEC(L"5. clear: everything goes back, the cache stays usable");

    IconCache *ic = fake_cache(8);
    (void)iconcache_get(ic, L"k0");
    (void)iconcache_get(ic, L"k1");
    ok_int(L"two held", iconcache_count(ic), 2);

    iconcache_clear(ic);
    ok_int(L"clear releases both", g_frees, 2);
    ok_int(L"and empties it", iconcache_count(ic), 0);
    ok_int(L"loads minus releases is zero", g_loads - g_frees, 0);

    /* Usable afterwards - this is what a dpi change does to it. */
    HICON again = iconcache_get(ic, L"k0");
    ok(L"it still works", again == (HICON)(INT_PTR)1);
    ok_int(L"by loading again", g_loads, 3);

    iconcache_clear(NULL);
    ok(L"clear is safe on NULL", 1);
    iconcache_destroy(ic);
}

static void test_long_key(void)
{
    T_SEC(L"6. a key too long to store is not cached at all");

    IconCache *ic = fake_cache(8);

    wchar_t key[ICONCACHE_KEY_MAX + 8];
    for (int i = 0; i < ICONCACHE_KEY_MAX + 4; ++i) key[i] = L'x';
    key[ICONCACHE_KEY_MAX + 4] = L'\0';

    /* Refused rather than truncated: two paths that agree for 259 characters
     * would otherwise share one icon, and the second would simply be wrong. */
    ok(L"an over-long key gives nothing", iconcache_get(ic, key) == NULL);
    ok_int(L"without loading", g_loads, 0);
    ok_int(L"and without being stored", iconcache_count(ic), 0);
    ok_int(L"it is counted as a failure", iconcache_failures(ic), 1);

    iconcache_destroy(ic);
}

/* ------------------------------------------------------------------ */
/* The real thing                                                      */
/* ------------------------------------------------------------------ */

static int    g_real_loads;
static int    g_real_frees;

static HICON counting_shell_load(void *ctx, const wchar_t *key, int px)
{
    HICON icon = iconcache_shell_load(ctx, key, px);
    if (icon != NULL) g_real_loads++;
    return icon;
}

static void counting_shell_free(void *ctx, HICON icon)
{
    g_real_frees++;
    iconcache_shell_free(ctx, icon);
}

static int user_objects(void)
{
    return (int)GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
}

/* Paths of up to `cap` executables in the system directory. */
static int collect_system_exes(wchar_t paths[][ICONCACHE_KEY_MAX], int cap)
{
    wchar_t dir[MAX_PATH];
    if (GetSystemDirectoryW(dir, MAX_PATH) == 0) return 0;

    wchar_t pattern[MAX_PATH + 8];
    _snwprintf_s(pattern, MAX_PATH + 8, _TRUNCATE, L"%ls\\*.exe", dir);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int n = 0;
    do {
        if (n >= cap) break;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        _snwprintf_s(paths[n], ICONCACHE_KEY_MAX, _TRUNCATE,
                     L"%ls\\%ls", dir, fd.cFileName);
        n++;
    } while (FindNextFileW(h, &fd));

    (void)FindClose(h);
    return n;
}

static void test_shell_icons(void)
{
    T_SEC(L"7. the real loader: one file, one handle, one release");

    wchar_t dir[MAX_PATH];
    if (GetSystemDirectoryW(dir, MAX_PATH) == 0) {
        T_WARN(L"the system directory could not be read - skipped");
        return;
    }

    wchar_t notepad[MAX_PATH];
    _snwprintf_s(notepad, MAX_PATH, _TRUNCATE, L"%ls\\notepad.exe", dir);

    g_real_loads = 0;
    g_real_frees = 0;

    IconCache *ic = iconcache_create(8, 20, counting_shell_load,
                                     counting_shell_free, NULL);
    ok(L"a cache over the shell loader", ic != NULL);

    HICON icon = iconcache_get(ic, notepad);
    T_KV(L"notepad icon", L"%ls", (icon != NULL) ? L"loaded" : L"none");
    ok(L"the shell gave an icon for notepad.exe", icon != NULL);
    ok_int(L"one handle created", g_real_loads, 1);

    (void)iconcache_get(ic, notepad);
    ok_int(L"asking twice creates no second handle", g_real_loads, 1);

    iconcache_destroy(ic);
    ok_int(L"destroy released it", g_real_frees, 1);
    ok_int(L"loads minus releases is zero", g_real_loads - g_real_frees, 0);
}

static void test_ceiling_with_real_icons(void)
{
    T_SEC(L"8. past the ceiling, with real icons (acceptance item 13)");

    static wchar_t paths[200][ICONCACHE_KEY_MAX];
    const int found = collect_system_exes(paths, 200);
    T_KV(L"system executables found", L"%d", found);

    if (found < ICONCACHE_MAX + 8) {
        T_WARN(L"not enough executables to cross the %d-entry ceiling - "
               L"asserting only what is here", ICONCACHE_MAX);
    }

    g_real_loads = 0;
    g_real_frees = 0;
    const int users_before = user_objects();

    IconCache *ic = iconcache_create(ICONCACHE_MAX, 20, counting_shell_load,
                                     counting_shell_free, NULL);
    ok(L"the cache exists", ic != NULL);

    int icons_seen = 0;
    for (int i = 0; i < found; ++i) {
        if (iconcache_get(ic, paths[i]) != NULL) icons_seen++;
    }

    T_KV(L"entries asked for", L"%d", found);
    T_KV(L"icons returned", L"%d", icons_seen);
    T_KV(L"held at the end", L"%d", iconcache_count(ic));
    T_KV(L"evictions", L"%d", iconcache_evictions(ic));
    T_KV(L"handles created", L"%d", g_real_loads);
    T_KV(L"handles released so far", L"%d", g_real_frees);

    ok(L"nothing was returned for a handle nobody created",
       g_real_loads >= icons_seen);
    ok(L"the ceiling held", iconcache_count(ic) <= ICONCACHE_MAX);
    ok(L"entries beyond it were evicted",
       found <= ICONCACHE_MAX || iconcache_evictions(ic) > 0);

    /*
     * Some entries hold "there is no icon" and produced no handle, so the
     * outstanding count is bounded by what is held, not equal to it. The exact
     * equality to zero comes after destroy, below.
     */
    ok(L"handles outstanding never exceed entries held",
       g_real_loads - g_real_frees >= 0 &&
       g_real_loads - g_real_frees <= iconcache_count(ic));

    iconcache_destroy(ic);

    const int users_after = user_objects();
    T_KV(L"user objects before / after", L"%d / %d", users_before, users_after);

    /* EXACT: this is the leak check. Every handle the loader produced has been
     * given back - a single outstanding one makes this non-zero. */
    ok_int(L"every handle created was released", g_real_loads - g_real_frees, 0);

    /* Supporting evidence, with a tolerance: the shell keeps caches of its own
     * and this process has just made it open a hundred files, so a small
     * difference is not ours to control. The exact check above is the one that
     * decides whether THIS module leaks. */
    ok(L"the USER-object count came back to where it started (within 10)",
       users_after - users_before <= 10);
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t71_iconcache");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t71_iconcache - the sidebar's icon cache (ceiling, eviction, release)");
    T_KV(L"run_at", L"%ls", t_now_string());
    T_NOTE(L"Leaking is what a cache usually fails at, so most of this does not");
    T_NOTE(L"infer it from a handle count: it counts RELEASES. With the fake");
    T_NOTE(L"loader every evicted handle is identified; with the real one,");
    T_NOTE(L"created minus released must be exactly what is still held.");

    test_basics();
    test_load_once();
    test_failure_cached();
    test_eviction();
    test_clear();
    test_long_key();
    test_shell_icons();
    test_ceiling_with_real_icons();

    T_SEC(L"Result");
    T_KV(L"passed", L"%d", g_pass);
    T_KV(L"failed", L"%d", g_fail);

    if (g_fail == 0) T_KV(L"VERDICT", L"PASS - all %d assertions hold", g_pass);
    else             T_KV(L"VERDICT", L"FAIL - %d assertion(s) broken", g_fail);

    if (!no_pause) t_pause();
    t_log_close();
    return (g_fail == 0) ? 0 : 1;
}
