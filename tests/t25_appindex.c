/*
 * t25_appindex.c - the installed-application index.
 *
 * WHY IT EXISTS
 *
 *   Stage 8's side bar offers programs that are NOT running, which means
 *   reading the machine's own list of installed applications. Three things
 *   about that are worth pinning down with assertions:
 *
 *     1. the de-duplication rule - which of two entries for the same program
 *        survives. Getting this wrong shows the user the same application
 *        twice, or shows a Start-Menu name that cannot be launched when a
 *        launchable copy was right there;
 *     2. that the scan really finds something on a real machine, and says how
 *        many of those entries carry a path (a list of names that cannot be
 *        started is not what the side bar is for);
 *     3. that the cache is used only when it is still TRUE - the stamp check
 *        is what stops a stale list from outliving an installation.
 *
 * WHAT IT CANNOT CHECK
 *
 *   The numbers depend on this machine (how many programs are installed), so
 *   the assertions are about *shape* - "more than zero", "at least one has a
 *   path" - and the counts are printed for the record instead of being
 *   asserted. A test that demanded "exactly 312 programs" would fail on the
 *   next machine and teach nobody anything.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t25_appindex
 *   tests\out\t25_appindex.exe --no-pause
 */

#include "test_common.h"

#include <shlobj.h>   /* SHGetFolderPathW: to clean up the cache this test writes */

#include "../src/core/appindex.h"

static int g_pass, g_fail;

static void ok(const wchar_t *what, int cond)
{
    if (cond) { g_pass++; }
    else { g_fail++; t_out_line(L"  FAIL: %ls", what); }
}

static void ok_int(const wchar_t *what, int got, int want)
{
    if (got == want) { g_pass++; }
    else { g_fail++; t_out_line(L"  FAIL: %ls  (got %d, want %d)", what, got, want); }
}

/* ------------------------------------------------------------------ */

static void test_basics(void)
{
    T_SEC(L"an index holds entries and says so");

    AppIndex *idx = appindex_create();
    ok(L"an index can be created", idx != NULL);
    if (idx == NULL) return;

    ok_int(L"it starts empty", appindex_count(idx), 0);
    ok(L"nothing at index 0", appindex_at(idx, 0) == NULL);

    ok(L"an entry can be added",
       appindex_add(idx, L"Notepad", L"C:\\Windows\\notepad.exe", APPINDEX_FROM_APPPATHS));
    ok_int(L"and it is counted", appindex_count(idx), 1);

    const AppIndexItem *it = appindex_at(idx, 0);
    ok(L"it can be read back", it != NULL);
    if (it != NULL) {
        ok(L"with its name", wcscmp(it->name, L"Notepad") == 0);
        ok(L"with its path", wcscmp(it->exe_path, L"C:\\Windows\\notepad.exe") == 0);
        ok_int(L"and its source", (int)it->source, (int)APPINDEX_FROM_APPPATHS);
    }

    /* A nameless entry is refused: it would be a row the user cannot read. */
    ok(L"an empty name is refused", !appindex_add(idx, L"", NULL, APPINDEX_FROM_STARTMENU));
    ok(L"a NULL name is refused", !appindex_add(idx, NULL, NULL, APPINDEX_FROM_STARTMENU));
    ok_int(L"and neither changed the count", appindex_count(idx), 1);

    appindex_destroy(idx);
}

static void test_dedup(void)
{
    T_SEC(L"the same program, listed twice");

    AppIndex *idx = appindex_create();
    if (idx == NULL) { ok(L"an index can be created", 0); return; }

    /* A Start-Menu name (no path) and a launchable App Paths entry. */
    appindex_add(idx, L"Notepad", NULL, APPINDEX_FROM_STARTMENU);
    appindex_add(idx, L"Notepad", L"C:\\Windows\\notepad.exe", APPINDEX_FROM_APPPATHS);
    ok_int(L"two entries to start with", appindex_count(idx), 2);

    ok_int(L"one is removed", appindex_dedup(idx), 1);
    ok_int(L"one entry left", appindex_count(idx), 1);

    const AppIndexItem *it = appindex_at(idx, 0);
    ok(L"the surviving entry is the launchable one",
       it != NULL && wcscmp(it->exe_path, L"C:\\Windows\\notepad.exe") == 0);

    /* Two names for one executable: the friendlier (longer) name wins. */
    appindex_destroy(idx);
    idx = appindex_create();
    if (idx == NULL) { ok(L"an index can be created", 0); return; }

    appindex_add(idx, L"np.exe", L"C:\\Windows\\notepad.exe", APPINDEX_FROM_APPPATHS);
    appindex_add(idx, L"Notepad", L"C:\\Windows\\notepad.exe", APPINDEX_FROM_UNINSTALL);
    ok_int(L"one is removed for the shared executable", appindex_dedup(idx), 1);
    ok_int(L"one entry left", appindex_count(idx), 1);

    it = appindex_at(idx, 0);
    ok(L"the longer name is the one kept",
       it != NULL && wcscmp(it->name, L"Notepad") == 0);

    /* Nothing to do: dedup must not invent work. */
    ok_int(L"a second pass removes nothing", appindex_dedup(idx), 0);

    appindex_destroy(idx);
}

static void test_sorting(void)
{
    T_SEC(L"entries come out sorted by name");

    AppIndex *idx = appindex_create();
    if (idx == NULL) { ok(L"an index can be created", 0); return; }

    appindex_add(idx, L"zeta", NULL, APPINDEX_FROM_STARTMENU);
    appindex_add(idx, L"Alpha", NULL, APPINDEX_FROM_STARTMENU);
    appindex_add(idx, L"middle", NULL, APPINDEX_FROM_STARTMENU);

    appindex_dedup(idx);

    const AppIndexItem *a = appindex_at(idx, 0);
    const AppIndexItem *b = appindex_at(idx, 1);
    const AppIndexItem *c = appindex_at(idx, 2);

    ok(L"first is Alpha", a != NULL && wcscmp(a->name, L"Alpha") == 0);
    ok(L"then middle", b != NULL && wcscmp(b->name, L"middle") == 0);
    ok(L"then zeta", c != NULL && wcscmp(c->name, L"zeta") == 0);

    appindex_destroy(idx);
}

static void test_real_scan(void)
{
    T_SEC(L"a real scan of this machine");

    AppIndex *idx = appindex_create();
    if (idx == NULL) { ok(L"an index can be created", 0); return; }

    const int added = appindex_scan(idx);
    ok(L"the scan found something", added > 0);

    appindex_dedup(idx);

    const int n = appindex_count(idx);
    T_KV(L"entries after de-duplication", L"%d", n);
    ok(L"more than a handful", n > 10);

    /* How many can actually be LAUNCHED: this is the number the side bar can
     * do something with, and the one worth watching as sources change. */
    int with_path = 0;
    int from_app_paths = 0;
    int from_start_menu = 0;
    int startmenu_with_path = 0;

    for (int i = 0; i < n; ++i) {
        const AppIndexItem *it = appindex_at(idx, i);
        if (it == NULL) continue;
        if (it->exe_path[0] != L'\0') with_path++;
        if (it->source == APPINDEX_FROM_APPPATHS) from_app_paths++;
        if (it->source == APPINDEX_FROM_STARTMENU) {
            from_start_menu++;
            if (it->exe_path[0] != L'\0') startmenu_with_path++;
        }
    }

    T_KV(L"entries with a launchable path", L"%d", with_path);
    T_KV(L"from App Paths / Applications", L"%d", from_app_paths);
    T_KV(L"from the Start Menu", L"%d", from_start_menu);
    T_KV(L"  of those, with a resolved target", L"%d", startmenu_with_path);

    ok(L"at least one entry can be launched", with_path > 0);
    ok(L"the Start Menu contributed names", from_start_menu > 0);
    /* The whole point of resolving shortcuts: rows that DO something. */
    ok(L"resolving shortcuts produced launchable entries", startmenu_with_path > 0);

    /* Print the first few: when a source stops working, this is what shows
     * which one, without having to read the registry by hand. */
    for (int i = 0; i < n && i < 5; ++i) {
        const AppIndexItem *it = appindex_at(idx, i);
        if (it == NULL) continue;
        T_KV(L"sample", L"%ls | %ls", it->name,
             (it->exe_path[0] != L'\0') ? it->exe_path : L"(no path)");
    }

    appindex_destroy(idx);
}

static void test_stamp_and_cache(void)
{
    T_SEC(L"the cache is used only while it is still true");

    const unsigned long long stamp = appindex_stamp();
    T_KV(L"start-menu stamp", L"%llu", stamp);
    ok(L"this machine has a stamp", stamp != 0);

    if (stamp == 0) return;

    AppIndex *idx = appindex_create();
    if (idx == NULL) { ok(L"an index can be created", 0); return; }

    appindex_add(idx, L"Cache Prober", L"C:\\probe\\cache-prober.exe",
                 APPINDEX_FROM_APPPATHS);

    ok(L"the cache can be written", appindex_cache_save(idx, stamp));
    appindex_destroy(idx);

    /* Same stamp: the cache is authoritative. */
    AppIndex *back = appindex_create();
    ok(L"an index can be created", back != NULL);
    if (back != NULL) {
        ok(L"it loads with the same stamp", appindex_cache_load(back, stamp));
        ok(L"and it has the entry", appindex_count(back) >= 1);
        appindex_destroy(back);
    }

    /* A different stamp means "something was installed or removed", and the
     * ONLY correct answer is a rescan - not "close enough". */
    AppIndex *stale = appindex_create();
    if (stale != NULL) {
        ok(L"a stale stamp is refused", !appindex_cache_load(stale, stamp + 1));
        ok_int(L"and leaves the index empty", appindex_count(stale), 0);
        appindex_destroy(stale);
    }

    /* A zero stamp means "cannot tell", which must also rescan. */
    AppIndex *unknown = appindex_create();
    if (unknown != NULL) {
        ok(L"an unknown stamp is refused", !appindex_cache_load(unknown, 0));
        appindex_destroy(unknown);
    }
}

/* ------------------------------------------------------------------ */

/*
 * A cache has to survive REAL sizes.
 *
 * The first version wrote the index through the shared store, which caps a
 * document at STORE_MAX_SECTIONS = 256 - and this machine has 343 entries. The
 * tail was dropped silently and the truncated file still carried a stamp
 * claiming to be current, so the program would have shown a list missing 87
 * programs and had no way to know. The one-entry round trip above could never
 * have caught that: this one uses 400.
 */
/* ------------------------------------------------------------------ */
/* The cache lives at a FIXED path                                     */
/*                                                                     */
/* appindex_cache_save() takes no path argument, so EVERY test that     */
/* writes a cache is writing over the user's own. Stash it before the   */
/* first such test and put it back at the very end.                     */
/*                                                                     */
/* MEASURED 2026-10-11: before this existed, running the suite left a   */
/* 77-byte cache behind - written by the round-trip tests, not by the   */
/* 400-entry one - and the product, finding a cache it trusted, showed  */
/* a single invented program instead of the real 343. That is exactly   */
/* the "test data escapes into the product" mistake this project has a  */
/* trap number for, and it was happening on every test run.             */
/* ------------------------------------------------------------------ */

static wchar_t g_cache_path[MAX_PATH];
static wchar_t g_cache_backup[MAX_PATH];
static BOOL    g_had_cache = FALSE;

static void cache_stash(void)
{
    wchar_t dir[MAX_PATH];

    g_cache_path[0]   = L'\0';
    g_cache_backup[0] = L'\0';

    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, dir) != S_OK) return;

    _snwprintf_s(g_cache_path, MAX_PATH, _TRUNCATE,
                 L"%ls\\WindowAssistanceController\\appindex.ini", dir);
    _snwprintf_s(g_cache_backup, MAX_PATH, _TRUNCATE,
                 L"%ls\\WindowAssistanceController\\appindex.ini.t25bak", dir);

    (void)DeleteFileW(g_cache_backup);      /* a leftover from a crashed run */
    g_had_cache = MoveFileW(g_cache_path, g_cache_backup);
}

static void cache_restore(void)
{
    if (g_cache_path[0] == L'\0') return;

    /* Whatever the tests left behind is theirs, not the product's. */
    (void)DeleteFileW(g_cache_path);

    if (g_had_cache) {
        ok(L"the real index cache is put back after the tests",
           MoveFileW(g_cache_backup, g_cache_path) != 0);
    }
}

static void test_cache_large(void)
{
    T_SEC(L"a cache as large as a real machine");

    const unsigned long long stamp = appindex_stamp();
    if (stamp == 0) {
        T_NOTE(L"No stamp on this machine: skipped, not faked.");
        return;
    }

    enum { N = 400 };


    AppIndex *big = appindex_create();
    ok(L"an index can be created", big != NULL);
    if (big == NULL) return;

    for (int i = 0; i < N; ++i) {
        wchar_t name[64];
        wchar_t path[96];
        _snwprintf_s(name, 64, _TRUNCATE, L"Program %03d", i);
        _snwprintf_s(path, 96, _TRUNCATE, L"C:\\probe\\prog%03d.exe", i);
        appindex_add(big, name, path, APPINDEX_FROM_APPPATHS);
    }

    ok_int(L"400 entries to cache", appindex_count(big), N);
    ok(L"they can be written", appindex_cache_save(big, stamp));
    appindex_destroy(big);

    AppIndex *back = appindex_create();
    if (back == NULL) { ok(L"an index can be created", 0); return; }

    ok(L"and read back", appindex_cache_load(back, stamp));
    ok_int(L"all 400 of them", appindex_count(back), N);

    if (appindex_count(back) == N) {
        const AppIndexItem *first = appindex_at(back, 0);
        const AppIndexItem *last  = appindex_at(back, N - 1);

        ok(L"the first entry survived",
           first != NULL && wcscmp(first->name, L"Program 000") == 0);
        ok(L"the last one too - that is the tail the store silently dropped",
           last != NULL && wcscmp(last->name, L"Program 399") == 0);
    }

    appindex_destroy(back);

    /*
     * The cache this test wrote is removed by cache_restore() at the end of the
     * run, together with whatever the other tests left. The removal is asserted
     * here because "the cache can be deleted" is part of what this test covers.
     */
    if (g_cache_path[0] != L'\0') {
        ok(L"the test cache can be removed afterwards",
           DeleteFileW(g_cache_path) != 0);
    }
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    /*
     * PerMonitorV2, like the product and like t22: without it any monitor or
     * window number this program reads back is DPI-virtualised. Nothing here
     * asserts on those today, and the line stays so that a future assertion
     * does not silently measure the wrong screen.
     */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t25_appindex");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t25_appindex - the installed-application index");
    T_KV(L"run_at", L"%ls", t_now_string());
    T_NOTE(L"Sources: App Paths, HKCR\\Applications, Uninstall, and the Start");
    T_NOTE(L"Menu - including shortcut TARGET PATHS, read with IShellLink. That");
    T_NOTE(L"last one is an APPROVED change to the privacy statement (manual");
    T_NOTE(L"§1.3), not something that happened quietly.");

    cache_stash();

    test_basics();
    test_dedup();
    test_sorting();
    test_real_scan();
    test_stamp_and_cache();
    test_cache_large();

    cache_restore();

    T_SEC(L"Result");
    T_KV(L"passed", L"%d", g_pass);
    T_KV(L"failed", L"%d", g_fail);

    if (g_fail == 0) T_KV(L"VERDICT", L"PASS - all %d assertions hold", g_pass);
    else             T_KV(L"VERDICT", L"FAIL - %d assertion(s) broken", g_fail);

    if (!no_pause) t_pause();

    t_log_close();
    return (g_fail == 0) ? 0 : 1;
}
