/*
 * t11_store.c - unit tests for src/model/store.c
 *
 * The store is the only thing standing between the user and total loss of
 * their workspaces, so these tests exercise the failure paths, not just the
 * happy one: a damaged file, a missing file, a zero-byte file, a truncated
 * file, a hand-edited file with no checksum, and the backup rotation that
 * has to keep working across repeated saves.
 *
 * All files are created in %TEMP%\wac-store-test and removed afterwards.
 * Nothing outside that directory is touched.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t11_store
 *   tests\out\t11_store.exe
 */

#include "test_common.h"

#include <math.h>
#include <string.h>
#include <shlobj.h>

#include "../src/model/store.h"

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
    if (GetTempPathW(MAX_PATH, tmp) == 0) return FALSE;

    _snwprintf_s(g_dir, MAX_PATH, _TRUNCATE, L"%lswac-store-test", tmp);

    if (!CreateDirectoryW(g_dir, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) return FALSE;
    }
    return TRUE;
}

/* Overwrite a file with arbitrary bytes. Used to simulate damage. */
static BOOL write_bytes(const wchar_t *path, const void *data, DWORD len)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    DWORD got = 0;
    BOOL ok = (len == 0) || WriteFile(h, data, len, &got, NULL);
    CloseHandle(h);
    return ok;
}

/* Read a file back as bytes; caller frees. */
static char *read_bytes(const wchar_t *path, DWORD *out_len)
{
    *out_len = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 1024 * 1024) {
        CloseHandle(h);
        return NULL;
    }

    char *buf = (char *)malloc((size_t)sz.QuadPart + 1);
    if (buf == NULL) { CloseHandle(h); return NULL; }

    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL)) {
        free(buf);
        CloseHandle(h);
        return NULL;
    }
    CloseHandle(h);

    buf[got] = '\0';
    *out_len = got;
    return buf;
}

static BOOL file_exists(const wchar_t *path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

/*
 * Build a small document with the fields the product will actually use.
 *
 * Returns a HEAP document. It must not be a local: a StoreDocument is about
 * 26 MB and the default stack is 1 MB, so `StoreDoc doc;` here would blow the
 * stack before the first assertion ran - which is exactly what happened on
 * the first run of this test (exit code 0xC00000FD).
 */
static StoreDocument *build_sample(const char *ws_name, int slot_count)
{
    StoreDocument *doc = store_create();
    if (doc == NULL) return NULL;

    store_set(doc, "meta", "version", "1");

    char sec[STORE_KEY_MAX];
    _snprintf_s(sec, sizeof(sec), _TRUNCATE, "workspace:%s", ws_name);
    store_set(doc, sec, "name", "Sample workspace");
    store_set(doc, sec, "gap_px", "8");

    for (int i = 0; i < slot_count; ++i) {
        _snprintf_s(sec, sizeof(sec), _TRUNCATE, "slot:%s:%d", ws_name, i);
        store_set(doc, sec, "app_id", "code.exe");

        char num[32];
        store_format_float(num, sizeof(num), 0.5f);
        store_set(doc, sec, "x", num);
        store_set(doc, sec, "y", "0.0000");
    }

    return doc;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_missing_file(void)
{
    T_SEC(L"1. A file that does not exist is not an error");

    wchar_t path[MAX_PATH];
    make_path(path, MAX_PATH, L"missing.ini");
    store_delete_all(path);

    /* Poison the pointer so a successful load is unambiguous, then check
     * that a missing file leaves it NULL rather than half-initialised. */
    StoreDocument *doc = (StoreDocument *)(ULONG_PTR)0xA5A5A5A5;

    StoreLoadResult r = store_load(path, &doc);
    ok_int(L"returns STORE_MISSING", r, STORE_MISSING);
    ok(L"out pointer cleared to NULL on failure", doc == NULL);
}

static void test_round_trip(void)
{
    T_SEC(L"2. Save then load: values survive exactly");

    wchar_t path[MAX_PATH];
    make_path(path, MAX_PATH, L"roundtrip.ini");
    store_delete_all(path);

    StoreDocument *out = NULL;
    out = build_sample("coding", 3);

    ok(L"save succeeds", store_save(path, out));
    ok(L"file now exists", file_exists(path));

    StoreDocument *in = NULL;
    StoreLoadResult r = store_load(path, &in);
    ok_int(L"load returns STORE_OK", r, STORE_OK);

    ok_str(L"meta version", store_get(in, "meta", "version", NULL), "1");
    ok_str(L"workspace name", store_get(in, "workspace:coding", "name", NULL),
           "Sample workspace");
    ok_int(L"gap_px parsed as int", store_get_int(in, "workspace:coding", "gap_px", 0), 8);
    ok_str(L"slot 0 app_id", store_get(in, "slot:coding:0", "app_id", NULL), "code.exe");
    ok_str(L"slot 2 exists", store_get(in, "slot:coding:2", "app_id", NULL), "code.exe");
    ok(L"slot 3 does not exist",
       store_get(in, "slot:coding:3", "app_id", NULL) == NULL);

    float fx = store_get_float(in, "slot:coding:1", "x", -1.0f);
    ok(L"float x survived", fabsf(fx - 0.5f) < 0.0001f);

    store_delete_all(path);
}

static void test_missing_keys(void)
{
    T_SEC(L"3. Missing keys, empty sections and defaults");

    wchar_t path[MAX_PATH];
    make_path(path, MAX_PATH, L"defaults.ini");
    store_delete_all(path);

    StoreDocument *out = NULL;
    out = build_sample("a", 1);
    store_save(path, out);

    StoreDocument *in = NULL;
    store_load(path, &in);

    ok_int(L"missing int falls back", store_get_int(in, "nope", "nope", 42), 42);
    ok_int(L"missing section falls back", store_get_int(in, "x:y", "k", 7), 7);
    ok(L"missing string falls back to NULL",
       store_get(in, "nope", "nope", NULL) == NULL);
    ok_str(L"missing string falls back to a default",
           store_get(in, "nope", "nope", "fallback"), "fallback");

    store_delete_all(path);
}

static void test_checksum_sensitivity(void)
{
    T_SEC(L"4. The checksum notices a one-byte change");

    unsigned a = store_checksum_bytes("hello", 5);
    unsigned b = store_checksum_bytes("hellp", 5);
    ok(L"one changed byte changes the checksum", a != b);

    unsigned c = store_checksum_bytes("hello", 5);
    ok(L"the same bytes give the same checksum", a == c);

    unsigned d = store_checksum_bytes("hello ", 6);
    ok(L"an extra byte changes the checksum", a != d);
}

static void test_external_edit_is_used(void)
{
    T_SEC(L"5. A file edited outside the program is USED, not discarded");

    /*
     * This is the case that made hand editing useless until 2026-09-22: the
     * user adds a section with Notepad, the checksum no longer matches, and
     * the old code loaded a backup instead - silently throwing the edit away.
     *
     * The checksum's job is to catch a half-written file. A document that
     * parses has demonstrably not been truncated, so the edit is kept and the
     * caller is told.
     */
    wchar_t path[MAX_PATH];
    make_path(path, MAX_PATH, L"damage.ini");
    store_delete_all(path);

    StoreDocument *out = NULL;
    out = build_sample("d", 2);
    ok(L"initial save", store_save(path, out));

    /* Change one value and ADD a section, the way a user would. The checksum
     * line stays as it was. */
    DWORD len = 0;
    char *data = read_bytes(path, &len);
    ok(L"file readable", data != NULL);
    if (data) {
        char *hit = strstr(data, "code.exe");
        ok(L"payload present", hit != NULL);
        if (hit) hit[0] = 'X';          /* code.exe -> Xode.exe */
        write_bytes(path, data, len);
        free(data);
    }
    {
        const char *extra = "[workspace:handmade]\r\nname=Handmade\r\nmarker=KEEPME\r\n";
        FILE *f = NULL;
        if (_wfopen_s(&f, path, L"ab") == 0 && f != NULL) {
            fwrite(extra, 1, strlen(extra), f);
            fclose(f);
        }
    }

    StoreDocument *in = NULL;
    StoreLoadResult r = store_load(path, &in);
    ok_int(L"reports MODIFIED_EXTERNALLY", r, STORE_MODIFIED_EXTERNALLY);
    ok(L"the document is usable", in != NULL);

    if (in != NULL) {
        ok_str(L"the hand-added section is there",
               store_get(in, "workspace:handmade", "marker", NULL), "KEEPME");
        ok_str(L"and the edited value too",
               store_get(in, "slot:d:0", "app_id", NULL), "Xode.exe");
        store_destroy(in);
    }

    store_delete_all(path);
}

static void test_backup_recovery(void)
{
    T_SEC(L"6. A file that cannot be parsed recovers from the rotating backup");

    wchar_t path[MAX_PATH];
    make_path(path, MAX_PATH, L"recover.ini");
    store_delete_all(path);

    /* First save: content A. */
    StoreDocument *a = NULL;
    a = build_sample("first", 1);
    store_set(a, "workspace:first", "marker", "AAA");
    ok(L"save A", store_save(path, a));

    /* Second save: content B. This creates a backup of A. */
    StoreDocument *b = NULL;
    b = build_sample("second", 1);
    store_set(b, "workspace:second", "marker", "BBB");
    ok(L"save B", store_save(path, b));

    wchar_t bak[MAX_PATH];
    ok(L"backup path resolves", store_backup_path(path, 0, bak, MAX_PATH));
    ok(L"a backup file exists", file_exists(bak));

    /*
     * Destroy the main file in the one way that really is unparseable: more
     * sections than the document can hold.
     *
     * This is narrower than it used to be, and deliberately so. "Garbage that
     * happens to look like INI" is no longer damage - it is a file somebody
     * else wrote, and its contents are used (see the test above). What is left
     * here is content no parser can accept, which is exactly when the
     * backups earn their keep.
     */
    {
        char junk[STORE_MAX_SECTIONS * 32];
        int n = 0;
        for (int i = 0; i < STORE_MAX_SECTIONS + 8; ++i) {
            n += _snprintf_s(junk + n, sizeof(junk) - n, _TRUNCATE, "[s%d]\r\n", i);
            if (n >= (int)sizeof(junk) - 16) break;
        }
        ok(L"main file overwritten with too many sections",
           write_bytes(path, junk, (DWORD)n));
    }

    StoreDocument *in = NULL;
    StoreLoadResult r = store_load(path, &in);
    ok_int(L"recovered from backup", r, STORE_RECOVERED_FROM_BACKUP);

    /* The backup should hold the PREVIOUS content (A). */
    ok_str(L"recovered content is the previous save",
           store_get(in, "workspace:first", "marker", NULL), "AAA");

    store_delete_all(path);
}

static void test_no_checksum_accepted(void)
{
    T_SEC(L"7. A hand-written file without a checksum still loads");

    wchar_t path[MAX_PATH];
    make_path(path, MAX_PATH, L"handmade.ini");
    store_delete_all(path);

    /* No [meta] section at all - as if the user typed it themselves. */
    const char *text =
        "; written by hand\r\n"
        "[workspace:manual]\r\n"
        "name=Hand made\r\n"
        "gap_px=12\r\n"
        "\r\n"
        "[slot:manual:0]\r\n"
        "app_id=notepad.exe\r\n"
        "x=0.2500\r\n";
    ok(L"hand-made file written", write_bytes(path, text, (DWORD)strlen(text)));

    StoreDocument *in = NULL;
    StoreLoadResult r = store_load(path, &in);
    ok_int(L"accepted with OK_NO_CHECKSUM", r, STORE_OK_NO_CHECKSUM);
    ok_str(L"value parsed", store_get(in, "workspace:manual", "name", NULL), "Hand made");
    ok_int(L"int parsed", store_get_int(in, "workspace:manual", "gap_px", 0), 12);

    float x = store_get_float(in, "slot:manual:0", "x", -1.0f);
    ok(L"float parsed", fabsf(x - 0.25f) < 0.0001f);

    store_delete_all(path);
}

static void test_malformed_values(void)
{
    T_SEC(L"8. Malformed values fall back instead of corrupting geometry");

    wchar_t path[MAX_PATH];
    make_path(path, MAX_PATH, L"malformed.ini");
    store_delete_all(path);

    const char *text =
        "[t]\r\n"
        "trailing_garbage=12abc\r\n"
        "not_a_number=hello\r\n"
        "empty=\r\n"
        "float_garbage=0.5xyz\r\n"
        "float_nan=nan\r\n"
        "float_huge=1e30\r\n"
        "negative=-5\r\n"
        "huge_int=99999999999999999999\r\n";
    write_bytes(path, text, (DWORD)strlen(text));

    StoreDocument *in = NULL;
    store_load(path, &in);

    ok_int(L"trailing garbage rejected", store_get_int(in, "t", "trailing_garbage", 99), 99);
    ok_int(L"non-numeric rejected", store_get_int(in, "t", "not_a_number", 99), 99);
    ok_int(L"empty value falls back", store_get_int(in, "t", "empty", 99), 99);
    ok_int(L"negative ints work", store_get_int(in, "t", "negative", 0), -5);
    ok_int(L"overflowing int is clamped, not wrapped",
           store_get_int(in, "t", "huge_int", 0), 2147483647);

    float fg = store_get_float(in, "t", "float_garbage", -1.0f);
    ok(L"float garbage rejected", fg < 0.0f);

    float fn = store_get_float(in, "t", "float_nan", -1.0f);
    ok(L"NaN rejected", fn < 0.0f);

    float fh = store_get_float(in, "t", "float_huge", -1.0f);
    ok(L"absurd float rejected", fh < 0.0f);

    store_delete_all(path);
}

static void test_zero_byte_file(void)
{
    T_SEC(L"9. A zero-byte file is treated as damaged, not as empty config");

    wchar_t path[MAX_PATH];
    make_path(path, MAX_PATH, L"zero.ini");
    store_delete_all(path);

    ok(L"zero-byte file created", write_bytes(path, "", 0));
    ok(L"it exists on disk", file_exists(path));

    StoreDocument *in = NULL;
    StoreLoadResult r = store_load(path, &in);
    /* Exists but unreadable-as-config: the caller must be told, so it can
     * offer the backup instead of quietly starting from defaults. */
    ok_int(L"reports CORRUPT_NO_RECOVERY", r, STORE_CORRUPT_NO_RECOVERY);

    store_delete_all(path);
}

static void test_truncated_file(void)
{
    T_SEC(L"10. A truncated file does not crash the parser");

    wchar_t path[MAX_PATH];
    make_path(path, MAX_PATH, L"trunc.ini");
    store_delete_all(path);

    StoreDocument *out = NULL;
    out = build_sample("t", 3);
    store_save(path, out);

    DWORD len = 0;
    char *data = read_bytes(path, &len);
    ok(L"file readable", data != NULL);
    if (data) {
        /* Cut it in the middle, leaving an unterminated section header and
         * a half-written key. */
        DWORD cut = len / 2;
        write_bytes(path, data, cut);
        free(data);
    }

    StoreDocument *in = NULL;
    StoreLoadResult r = store_load(path, &in);
    /* Either it parses what survived, or it reports damage - but it must
     * never crash, and must never claim a checksum passed. A surviving prefix
     * of a file is a file somebody else wrote, so it is used (see test 5). */
    ok(L"load returns a defined result",
       r == STORE_OK_NO_CHECKSUM || r == STORE_MODIFIED_EXTERNALLY ||
       r == STORE_CORRUPT_NO_RECOVERY || r == STORE_RECOVERED_FROM_BACKUP);
    ok(L"did not claim the checksum was verified", r != STORE_OK);

    store_delete_all(path);
}

static void test_backup_rotation(void)
{
    T_SEC(L"11. Backups rotate and stay bounded");

    wchar_t path[MAX_PATH];
    make_path(path, MAX_PATH, L"rotate.ini");
    store_delete_all(path);

    /* Six saves: more than the number of backups kept. */
    for (int i = 0; i < 6; ++i) {
        StoreDocument *d = NULL;
        d = build_sample("r", 1);
        char v[32];
        _snprintf_s(v, sizeof(v), _TRUNCATE, "gen%d", i);
        store_set(d, "workspace:r", "gen", v);
        ok(L"save in the loop", store_save(path, d));
    }

    int existing = 0;
    for (int i = 0; i < STORE_BACKUP_COUNT; ++i) {
        wchar_t bak[MAX_PATH];
        if (store_backup_path(path, i, bak, MAX_PATH) && file_exists(bak)) existing++;
    }
    ok(L"at least one backup exists", existing >= 1);
    ok(L"backup count is bounded", existing <= STORE_BACKUP_COUNT);

    /* No stray temp file may be left behind. */
    wchar_t tmp[MAX_PATH];
    _snwprintf_s(tmp, MAX_PATH, _TRUNCATE, L"%ls.tmp", path);
    ok(L"no temp file left behind", !file_exists(tmp));

    /* The current file still holds the newest content. */
    StoreDocument *in = NULL;
    ok_int(L"current file loads", store_load(path, &in), STORE_OK);
    ok_str(L"newest generation present",
           store_get(in, "workspace:r", "gen", NULL), "gen5");

    store_delete_all(path);
}

static void test_float_formatting(void)
{
    T_SEC(L"12. Float formatting is locale-safe and round-trips");

    char buf[32];

    store_format_float(buf, sizeof(buf), 0.5f);
    ok_str(L"0.5 formats", buf, "0.5000");

    store_format_float(buf, sizeof(buf), 0.0f);
    ok_str(L"0 formats", buf, "0.0000");

    store_format_float(buf, sizeof(buf), 1.0f);
    ok_str(L"1.0 formats", buf, "1.0000");

    store_format_float(buf, sizeof(buf), 0.1234f);
    ok_str(L"0.1234 formats", buf, "0.1234");

    /* A value whose rounded form would carry into the whole part. */
    store_format_float(buf, sizeof(buf), 0.99999f);
    ok_str(L"0.99999 carries correctly", buf, "1.0000");

    store_format_float(buf, sizeof(buf), -0.25f);
    ok_str(L"negative formats", buf, "-0.2500");

    /* A negative value whose integer part is zero must keep its sign: on a
     * monitor left of the primary that is an ordinary coordinate. */
    store_format_float(buf, sizeof(buf), -0.0001f);
    ok_str(L"small negative keeps its sign", buf, "-0.0001");

    store_format_float(buf, sizeof(buf), -1.5f);
    ok_str(L"negative with a whole part", buf, "-1.5000");

    /* Whatever it produces must parse back to the same number.
     * The document comes from store_create(): a zeroed pointer would make
     * store_set() a silent no-op, which is how this assertion first failed. */
    StoreDocument *doc = store_create();
    ok(L"document created", doc != NULL);
    if (doc) {
        store_format_float(buf, sizeof(buf), 0.3333f);
        store_set(doc, "t", "f", buf);
        float back = store_get_float(doc, "t", "f", -1.0f);
        ok(L"formatted value parses back", fabsf(back - 0.3333f) < 0.00005f);

        store_format_float(buf, sizeof(buf), -0.25f);
        store_set(doc, "t", "neg", buf);
        float negback = store_get_float(doc, "t", "neg", 0.0f);
        ok(L"negative value parses back", fabsf(negback + 0.25f) < 0.00005f);

        store_destroy(doc);
    }
}

static void test_atomic_no_temp_left(void)
{
    T_SEC(L"13. A successful save leaves no .tmp file");

    wchar_t path[MAX_PATH];
    make_path(path, MAX_PATH, L"atomic.ini");
    store_delete_all(path);

    StoreDocument *d = NULL;
    d = build_sample("a", 2);
    ok(L"save", store_save(path, d));

    wchar_t tmp[MAX_PATH];
    _snwprintf_s(tmp, MAX_PATH, _TRUNCATE, L"%ls.tmp", path);
    ok(L"no .tmp after save", !file_exists(tmp));

    /* Saving repeatedly must not accumulate temp files either. */
    for (int i = 0; i < 5; ++i) store_save(path, d);
    ok(L"no .tmp after repeated saves", !file_exists(tmp));

    /* And the content is still intact. */
    StoreDocument *in = NULL;
    ok_int(L"still loads cleanly", store_load(path, &in), STORE_OK);

    store_delete_all(path);
}

/*
 * A file that PARSES but yields nothing recognizable.
 *
 * MEASURED 2026-10-09, by running stage 7's acceptance list against the
 * PRODUCT: a config file containing "@@@ truncated @@@" was accepted as an
 * empty configuration, its defaults were seeded, and the next save overwrote
 * the file - backups untouched, user never told. The parser skips lines that
 * are neither a section header nor key=value (so an unrelated section cannot
 * break loading), which makes almost any junk "parse".
 *
 * Every file this program writes has at least [meta], so "parsed and empty" is
 * never a file it wrote. It is junk, or a file cut off before its first
 * section header.
 */
static void test_junk_that_parses(void)
{
    T_SEC(L"14. Junk that 'parses' but yields no sections is damage, not emptiness");

    wchar_t path[MAX_PATH];
    make_path(path, MAX_PATH, L"junk.ini");
    store_delete_all(path);

    /* A readable save first, so there is something in the backup slot. */
    StoreDocument *a = NULL;
    a = build_sample("keeper", 1);
    store_set(a, "workspace:keeper", "marker", "KEEP");
    ok(L"a real save exists", store_save(path, a));
    store_save(path, a);                 /* second save rotates a backup in */

    wchar_t bak[MAX_PATH];
    ok(L"backup path resolves", store_backup_path(path, 0, bak, MAX_PATH));
    ok(L"a backup exists to fall back to", file_exists(bak));

    ok(L"main file replaced with junk that parses to nothing",
       write_bytes(path, "@@@ truncated @@@", 17));

    StoreDocument *in = NULL;
    const StoreLoadResult r = store_load(path, &in);
    ok_int(L"recovered from the backup instead of starting over",
           r, STORE_RECOVERED_FROM_BACKUP);
    if (r == STORE_RECOVERED_FROM_BACKUP) {
        ok_str(L"and the recovered content is real",
               store_get(in, "workspace:keeper", "marker", NULL), "KEEP");
    }

    /*
     * The same bytes with NO backups: the caller must be told, because the
     * alternative is the silent wipe this test exists for.
     */
    store_delete_all(path);
    ok(L"junk written with no backups present",
       write_bytes(path, "@@@ truncated @@@", 17));
    in = NULL;
    ok_int(L"reports CORRUPT_NO_RECOVERY", store_load(path, &in),
           STORE_CORRUPT_NO_RECOVERY);

    /* Whitespace only is the same situation. */
    store_delete_all(path);
    ok(L"whitespace-only file written", write_bytes(path, "   \r\n\r\n\t\r\n", 9));
    in = NULL;
    ok_int(L"whitespace-only reports CORRUPT_NO_RECOVERY", store_load(path, &in),
           STORE_CORRUPT_NO_RECOVERY);

    /*
     * The other side of the line: a file this program wrote with NO workspaces
     * in it is legitimately empty, and must still load as STORE_OK - otherwise
     * "delete every workspace" would look like corruption.
     */
    store_delete_all(path);
    StoreDocument *empty = store_create();
    ok(L"an empty document saves", store_save(path, empty));
    in = NULL;
    ok_int(L"and loads as a clean, empty config", store_load(path, &in), STORE_OK);
    ok_int(L"with its [meta] section present",
           (in != NULL) ? store_section_count(in) : -1, 1);

    store_delete_all(path);
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t11_store");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t11_store - unit tests for INI persistence");
    T_KV(L"run_at", L"%ls", t_now_string());

    if (!dir_init()) {
        T_KV(L"VERDICT", L"FAIL - cannot create the test directory");
        t_log_close();
        return 1;
    }
    T_KV(L"test_dir", L"%ls", g_dir);

    t_out_line(L"[progress] beginning tests");

    test_missing_file();
    test_round_trip();
    test_missing_keys();
    test_checksum_sensitivity();
    test_external_edit_is_used();
    test_backup_recovery();
    test_no_checksum_accepted();
    test_malformed_values();
    test_zero_byte_file();
    test_truncated_file();
    test_backup_rotation();
    test_float_formatting();
    test_atomic_no_temp_left();
    test_junk_that_parses();

    T_SEC(L"Result");
    T_KV(L"passed", L"%d", g_pass);
    T_KV(L"failed", L"%d", g_fail);

    if (g_fail == 0) {
        T_KV(L"VERDICT", L"PASS - all %d assertions hold", g_pass);
    } else {
        T_KV(L"VERDICT", L"FAIL - %d assertion(s) broken", g_fail);
    }

    /* Leave nothing behind. */
    wchar_t leafs[][32] = {
        L"missing.ini", L"roundtrip.ini", L"defaults.ini", L"damage.ini",
        L"recover.ini", L"handmade.ini", L"malformed.ini", L"zero.ini",
        L"trunc.ini", L"rotate.ini", L"atomic.ini"
    };
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
