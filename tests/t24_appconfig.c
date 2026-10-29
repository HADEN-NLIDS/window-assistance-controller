/*
 * t24_appconfig.c - the shared-config-file regression test.
 *
 * THE BUG THIS EXISTS FOR (measured 2026-09-23)
 *
 * The configuration file holds sections owned by two modules: [meta],
 * [settings] and [workspace:*] by the main window, [rule:*] by the rule
 * module. Saving the main window's configuration used to build a FRESH
 * document and overwrite the file, which deleted every [rule:*] section.
 *
 * Because that save runs unconditionally on exit, a user who anchored a
 * window preset and then closed the program lost the preset: the next start
 * reported zero presets, and nothing warned them at any point.
 *
 * appconfig_save() is that operation, extracted from main.c so it can be
 * driven against a temporary file. Test 1 checks the property that matters -
 * a section this module does not own must still be there afterwards - and
 * test 3 pins the old shape so that nobody "simplifies" it back.
 *
 * All files are created in %TEMP%\wac-appconfig-test and removed afterwards.
 * Nothing outside that directory is touched.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t24_appconfig
 *   tests\out\t24_appconfig.exe
 */

#include "test_common.h"

#include <string.h>
#include <shlobj.h>

#include "../src/appconfig.h"
#include "../src/model/store.h"
#include "../src/model/workspace.h"
#include "../src/ui/page_settings.h"

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

static void ok_str(const wchar_t *what, const char *got, const char *want)
{
    if (got != NULL && want != NULL && strcmp(got, want) == 0) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls  (got \"%hs\", want \"%hs\")", what,
                   got ? got : "(null)", want ? want : "(null)");
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
/* Test files live here                                                */
/* ------------------------------------------------------------------ */

static wchar_t g_dir[MAX_PATH];

static void make_path(wchar_t *out, size_t cch, const wchar_t *leaf)
{
    _snwprintf_s(out, cch, _TRUNCATE, L"%ls\\%ls", g_dir, leaf);
}

static BOOL dir_init(void)
{
    wchar_t tmp[MAX_PATH];

    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, tmp) != S_OK) {
        return FALSE;
    }

    _snwprintf_s(g_dir, MAX_PATH, _TRUNCATE, L"%ls\\wac-appconfig-test", tmp);

    if (!CreateDirectoryW(g_dir, NULL)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
    }
    return TRUE;
}

/*
 * A file shaped like the real one: both writers' sections are present.
 * [rule:*] is written here through the raw store API rather than through
 * rules_save(), because this test is about the file, not about the rule
 * module - the rule module has its own coverage.
 */
static void write_seed_file(const wchar_t *path)
{
    StoreDocument *d = store_create();
    if (d == NULL) return;

    store_set(d, "meta", "version", "1");
    store_set(d, "settings", "language", "auto");
    store_set(d, "settings", "close_action", "0");
    store_set(d, "settings", "watch_windows", "1");

    store_set(d, "workspace:old", "name", "Old");
    store_set(d, "workspace:old", "slot_count", "0");

    /* The section that must survive: owned by the rule module. */
    store_set(d, "rule:notepad", "exe", "notepad.exe");
    store_set(d, "rule:notepad", "x", "0.0000");
    store_set(d, "rule:notepad", "w", "0.5000");

    store_save(path, d);
    store_destroy(d);
}

/* ------------------------------------------------------------------ */
/* 1. The regression proper                                            */
/* ------------------------------------------------------------------ */

static void test_foreign_sections_survive(void)
{
    wchar_t path[MAX_PATH];
    make_path(path, MAX_PATH, L"survive.ini");
    store_delete_all(path);
    write_seed_file(path);

    T_SEC(L"1. sections this module does not own survive the save");

    Settings s;
    settings_defaults(&s);
    s.language     = UI_LANG_EN;          /* different, so the write is visible */
    s.close_action = CLOSE_ACTION_QUIT;
    s.watch_windows = FALSE;

    /*
     * STATIC, not a local.
     *
     * WorkspaceSet is 64 workspaces x 16 slots - about 1.4 MB, well past the
     * 1 MB default stack. Declaring it here crashes the program with
     * 0xC00000FD before a single assertion runs (measured while writing this
     * test; it is the same trap as pitfall 11 in the test README). The
     * product keeps it in g_app for the same reason.
     */
    static WorkspaceSet ws;
    ws_clear(&ws);
    ok(L"the new workspace was added to the set",
       ws_add(&ws, "demo", "Demo") != NULL);

    ok(L"appconfig_save reports success", appconfig_save(path, &s, &ws));

    StoreDocument *d = NULL;
    ok_int(L"the file reloads", store_load(path, &d), STORE_OK);

    /* ---- THE assertion this whole program exists for ---- */
    ok(L"[rule:notepad] is STILL THERE", store_has_section(d, "rule:notepad"));
    ok_str(L"  and keeps its first value",
           store_get(d, "rule:notepad", "exe", ""), "notepad.exe");
    ok_str(L"  and its second value",
           store_get(d, "rule:notepad", "w", ""), "0.5000");

    /* ---- our own sections were written ---- */
    ok_str(L"[settings] language was written",
           store_get(d, "settings", "language", ""), i18n_code(s.language));
    ok_str(L"[settings] close_action was written",
           store_get(d, "settings", "close_action", ""), "1");
    ok_str(L"[settings] watch_windows was written",
           store_get(d, "settings", "watch_windows", ""), "0");
    ok(L"[workspace:demo] was written", store_has_section(d, "workspace:demo"));
    ok_str(L"  with its display name",
           store_get(d, "workspace:demo", "name", ""), "Demo");

    /*
     * A workspace that left the set must be GONE.
     *
     * This is the other half of the discipline, and the reason a save may not
     * simply merge: a save that only adds would leave [workspace:old] behind,
     * and it would come back on the next load - a deletion that silently
     * undoes itself.
     */
    ok(L"[workspace:old] was removed", !store_has_section(d, "workspace:old"));

    store_destroy(d);
    store_delete_all(path);
}

/* ------------------------------------------------------------------ */
/* 2. First run                                                        */
/* ------------------------------------------------------------------ */

static void test_missing_file_is_normal(void)
{
    wchar_t path[MAX_PATH];
    make_path(path, MAX_PATH, L"fresh.ini");
    store_delete_all(path);

    T_SEC(L"2. a missing file is a first run, not a failure");

    Settings s;
    settings_defaults(&s);

    static WorkspaceSet ws;         /* ~1.4 MB: see the note in test 1 */
    ws_clear(&ws);
    ws_seed_defaults(&ws);          /* what main.c does when nothing was loaded */

    ok(L"the save succeeds with no file present", appconfig_save(path, &s, &ws));

    StoreDocument *d = NULL;
    ok_int(L"the file now loads", store_load(path, &d), STORE_OK);
    ok(L"[meta] is present",  store_has_section(d, "meta"));
    ok(L"[settings] is present", store_has_section(d, "settings"));
    ok(L"the seeded workspace is present",
       store_section_count(d) >= 3);
    store_destroy(d);

    store_delete_all(path);
}

/* ------------------------------------------------------------------ */
/* 3. The old shape, pinned                                            */
/* ------------------------------------------------------------------ */

static void test_fresh_document_drops_it(void)
{
    wchar_t path[MAX_PATH];
    make_path(path, MAX_PATH, L"freshdoc.ini");
    store_delete_all(path);
    write_seed_file(path);

    T_SEC(L"3. the old shape is what destroyed the presets (why the fix exists)");

    /*
     * Exactly what save_all() used to do: a new document, written over the
     * file. Kept as an assertion rather than a comment, so that anyone who
     * reaches for store_create() in a save path sees the consequence.
     */
    StoreDocument *d = store_create();
    ok(L"fresh document allocated", d != NULL);
    store_set(d, "meta", "version", "1");
    store_set(d, "settings", "language", "en");
    ok(L"save succeeds - silently", store_save(path, d));
    store_destroy(d);

    StoreDocument *in = NULL;
    store_load(path, &in);
    ok(L"a fresh document DOES delete [rule:notepad] - this is the bug",
       !store_has_section(in, "rule:notepad"));
    store_destroy(in);

    store_delete_all(path);
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t24_appconfig");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t24_appconfig - the shared config file keeps every writer's sections");
    T_KV(L"run_at", L"%ls", t_now_string());

    if (!dir_init()) {
        T_KV(L"VERDICT", L"FAIL - cannot create the test directory");
        t_log_close();
        return 1;
    }
    T_KV(L"test_dir", L"%ls", g_dir);
    /*
     * Recorded because it is the reason the two tests below use static
     * storage: a WorkspaceSet local overflows the 1 MB stack (measured here
     * as 0xC00000FD, before any assertion ran).
     */
    T_KV(L"sizeof_WorkspaceSet", L"%d bytes", (int)sizeof(WorkspaceSet));

    test_foreign_sections_survive();
    test_missing_file_is_normal();
    test_fresh_document_drops_it();

    T_SEC(L"Result");
    T_KV(L"passed", L"%d", g_pass);
    T_KV(L"failed", L"%d", g_fail);

    if (g_fail == 0) {
        T_KV(L"VERDICT", L"PASS - all %d assertions hold", g_pass);
    } else {
        T_KV(L"VERDICT", L"FAIL - %d assertion(s) broken", g_fail);
    }

    /* Leave nothing behind. */
    wchar_t leafs[][32] = { L"survive.ini", L"fresh.ini", L"freshdoc.ini" };
    for (int i = 0; i < (int)(sizeof(leafs) / sizeof(leafs[0])); ++i) {
        wchar_t p[MAX_PATH];
        make_path(p, MAX_PATH, leafs[i]);
        store_delete_all(p);
    }
    RemoveDirectoryW(g_dir);

    if (!no_pause) t_pause();

    t_log_close();
    return (g_fail == 0) ? 0 : 1;
}
