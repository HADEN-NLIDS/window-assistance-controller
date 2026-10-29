/*
 * t14_i18nfmt.c - every i18n TEMPLATE must use a specifier the code can supply.
 *
 * WHY THIS TEST EXISTS
 *
 *   src/ui/i18n.h defines two kinds of user-visible string:
 *
 *     I18N_TEMPLATES(X)  contain placeholders. They are ONLY valid as the
 *                        FORMAT argument of a printf-family call.
 *     I18N_STRINGS(X)    finished text, displayed as-is.
 *
 *   The status bar showed a literal "%hs" until stage 4.5 because a template
 *   was handed to _snwprintf_s as a %ls ARGUMENT instead of being the format
 *   (TRAPS.md #23). The compiler cannot catch that: the format
 *   string is a variable, so /W4 has nothing to check it against.
 *
 *   This program closes the gap from the other side. It walks every entry of
 *   I18N_TEMPLATES in both languages and asserts:
 *
 *     1. every placeholder is one the call sites can supply -
 *        %ls (wide string), %d (int) or %% (a literal percent sign);
 *     2. a template contains at least one placeholder, otherwise it belongs in
 *        I18N_STRINGS;
 *     3. **both languages have the same placeholder sequence**. This is the
 *        one that matters most: if a translator drops a %ls, the argument list
 *        and the format no longer line up, and _snwprintf_s reads a value that
 *        was never passed. Nothing else in the project checks that.
 *
 *   Finished text is checked in the opposite direction: a printf specifier
 *   hiding in I18N_STRINGS would never be substituted. That check is
 *   deliberately narrow - real text may contain a percent sign for its own
 *   reasons (STR_MSG_ABOUT_CONFIG prints "%APPDATA%" literally), so only the
 *   known specifiers are flagged.
 *
 * WHAT IT CANNOT CHECK
 *
 *   The number of arguments at each call site. That mapping lives in the call
 *   sites, not in the table - keeping the two languages in step is the part
 *   that can be mechanised, and it is where a translation mistake is silent.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t14_i18nfmt
 *   tests\out\t14_i18nfmt.exe --no-pause
 */

#include "test_common.h"

#include <string.h>

#include "../src/ui/i18n.h"

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

/* ------------------------------------------------------------------ */
/* Placeholder scanning                                                */
/* ------------------------------------------------------------------ */

#define MAX_PH 16

typedef struct {
    wchar_t kind[MAX_PH][8];   /* "ls", "d" - one entry per placeholder */
    int     count;
    int     bad;               /* number of illegal sequences seen */
    wchar_t first_bad[24];     /* the first illegal one, for the report */
} PhSeq;

/*
 * Walk a string and record its placeholder sequence.
 *
 * Allowed: %% , %ls , %d. Everything else is recorded as illegal and skipped,
 * so one bad sequence does not hide the rest of the string.
 */
static void scan_placeholders(const wchar_t *s, PhSeq *out)
{
    int i = 0;
    out->count = 0;
    out->bad = 0;
    out->first_bad[0] = L'\0';

    if (s == NULL) return;

    while (s[i] != L'\0') {
        if (s[i] != L'%') { ++i; continue; }

        const wchar_t c = s[i + 1];

        if (c == L'%') { i += 2; continue; }

        if (c == L'l' && s[i + 2] == L's') {
            if (out->count < MAX_PH) {
                wcscpy_s(out->kind[out->count], 8, L"ls");
                out->count++;
            }
            i += 3;
            continue;
        }

        if (c == L'd') {
            if (out->count < MAX_PH) {
                wcscpy_s(out->kind[out->count], 8, L"d");
                out->count++;
            }
            i += 2;
            continue;
        }

        /* Illegal - remember the first one, count them all. */
        out->bad++;
        if (out->first_bad[0] == L'\0') {
            wchar_t seq[4];
            int n = 0;
            seq[n++] = L'%';
            if (c != L'\0' && n < 3) seq[n++] = c;
            if (c == L'l' && s[i + 2] != L'\0' && n < 3) seq[n++] = s[i + 2];
            seq[n] = L'\0';
            wcscpy_s(out->first_bad, 24, seq);
        }
        i += 2;
    }
}

/*
 * Finished text must not contain a printf specifier - it would never be
 * substituted. %% is fine, and so is a lone percent that starts no known
 * specifier (that is how "%APPDATA%" survives).
 */
static int has_specifier(const wchar_t *s)
{
    if (s == NULL) return 0;

    for (int i = 0; s[i] != L'\0'; ++i) {
        if (s[i] != L'%') continue;
        if (s[i + 1] == L'%') { ++i; continue; }

        const wchar_t c = s[i + 1];
        if (c == L'd' || c == L's' || c == L'x' || c == L'u' || c == L'f') return 1;
        if (c == L'h') return 1;                       /* %hs, %hd */
        if (c == L'l' && (s[i + 2] == L's' || s[i + 2] == L'd')) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* The tables under test                                               */
/* ------------------------------------------------------------------ */

static const struct { StrId id; const char *name; } k_templates[] = {
#define X(id, en) { STR_##id, #id },
    I18N_TEMPLATES(X)
#undef X
};

static const struct { StrId id; const char *name; } k_finished[] = {
#define X(id, en) { STR_##id, #id },
    I18N_STRINGS(X)
#undef X
};

/* ------------------------------------------------------------------ */

static void test_templates(WacLanguage lang, const wchar_t *lang_name)
{
    (void)lang;
    T_SEC(L"templates (%ls)", lang_name);

    const int n = (int)(sizeof(k_templates) / sizeof(k_templates[0]));
    T_KV(L"count", L"%d", n);

    for (int i = 0; i < n; ++i) {
        const wchar_t *text = i18n_text(k_templates[i].id);
        wchar_t what[128];

        _snwprintf_s(what, 128, _TRUNCATE, L"%hs: non-empty", k_templates[i].name);
        ok(what, text != NULL && text[0] != L'\0');

        if (text == NULL) continue;

        PhSeq ph;
        scan_placeholders(text, &ph);

        _snwprintf_s(what, 128, _TRUNCATE,
                     L"%hs: placeholders are %%ls/%%d only (found %ls)",
                     k_templates[i].name, ph.first_bad);
        ok(what, ph.bad == 0);

        _snwprintf_s(what, 128, _TRUNCATE,
                     L"%hs: has at least one placeholder", k_templates[i].name);
        ok(what, ph.count > 0);

        T_KV(L"template", L"%hs -> %d placeholder(s)", k_templates[i].name, ph.count);
    }
}

/* Capture the placeholder sequence of one template in one language. */
static void seq_of(StrId id, WacLanguage lang, PhSeq *out)
{
    i18n_init(lang);
    scan_placeholders(i18n_text(id), out);
}

static void test_language_parity(void)
{
    T_SEC(L"placeholder parity between languages");

    const int n = (int)(sizeof(k_templates) / sizeof(k_templates[0]));

    for (int i = 0; i < n; ++i) {
        PhSeq en, zh;
        seq_of(k_templates[i].id, UI_LANG_EN, &en);
        seq_of(k_templates[i].id, UI_LANG_ZH_CN, &zh);

        wchar_t what[160];
        _snwprintf_s(what, 160, _TRUNCATE,
                     L"%hs: same placeholder count in en and zh-CN (%d vs %d)",
                     k_templates[i].name, en.count, zh.count);
        ok(what, en.count == zh.count);

        if (en.count != zh.count) continue;

        int same = 1;
        for (int k = 0; k < en.count; ++k) {
            if (wcscmp(en.kind[k], zh.kind[k]) != 0) same = 0;
        }
        _snwprintf_s(what, 160, _TRUNCATE,
                     L"%hs: same placeholder order and types in en and zh-CN",
                     k_templates[i].name);
        ok(what, same);
    }
}

static void test_finished_text(void)
{
    T_SEC(L"finished text");

    const int n = (int)(sizeof(k_finished) / sizeof(k_finished[0]));
    T_KV(L"count", L"%d", n);

    const WacLanguage langs[2] = { UI_LANG_EN, UI_LANG_ZH_CN };
    const wchar_t *names[2] = { L"en", L"zh-CN" };

    for (int L = 0; L < 2; ++L) {
        i18n_init(langs[L]);

        for (int i = 0; i < n; ++i) {
            const wchar_t *text = i18n_text(k_finished[i].id);
            if (text == NULL) continue;

            if (has_specifier(text)) {
                g_fail++;
                t_out_line(L"  FAIL: %hs [%ls] contains a printf specifier "
                           L"but is finished text: %ls",
                           k_finished[i].name, names[L], text);
            } else {
                g_pass++;
            }
        }
    }
}

/*
 * A sample of well-known strings, asserted by hand.
 *
 * The mechanical checks above prove internal consistency; these pin the
 * concrete values that two real bugs were about, so a future edit that
 * "simplifies" the templates has to argue with a test.
 */
static void test_known_strings(void)
{
    T_SEC(L"known strings");

    i18n_init(UI_LANG_EN);
    ok(L"en: status hotkey is a %ls template",
       wcsstr(i18n_text(STR_STATUS_HOTKEY), L"%ls") != NULL);
    ok(L"en: no %hs survives anywhere in the template block",
       wcsstr(i18n_text(STR_STATUS_HOTKEY), L"%hs") == NULL &&
       wcsstr(i18n_text(STR_STATUS_ACTIVE), L"%hs") == NULL &&
       wcsstr(i18n_text(STR_MSG_CONFIRM_DELETE), L"%hs") == NULL &&
       wcsstr(i18n_text(STR_MSG_ABOUT_HOTKEY), L"%hs") == NULL);

    i18n_init(UI_LANG_ZH_CN);
    ok(L"zh: status hotkey is a %ls template",
       wcsstr(i18n_text(STR_STATUS_HOTKEY), L"%ls") != NULL);
    ok(L"zh: no %hs survives anywhere in the template block",
       wcsstr(i18n_text(STR_STATUS_HOTKEY), L"%hs") == NULL &&
       wcsstr(i18n_text(STR_STATUS_ACTIVE), L"%hs") == NULL &&
       wcsstr(i18n_text(STR_MSG_CONFIRM_DELETE), L"%hs") == NULL &&
       wcsstr(i18n_text(STR_MSG_ABOUT_HOTKEY), L"%hs") == NULL);

    /* The literal percent in the config path must keep working: it is shown,
     * not formatted. */
    ok(L"config path keeps its literal %APPDATA%",
       wcsstr(i18n_text(STR_MSG_ABOUT_CONFIG), L"%APPDATA%") != NULL);
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t14_i18nfmt");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t14_i18nfmt - i18n placeholder rules");
    T_KV(L"run_at", L"%ls", t_now_string());

    i18n_init(UI_LANG_EN);
    test_templates(UI_LANG_EN, L"en");
    i18n_init(UI_LANG_ZH_CN);
    test_templates(UI_LANG_ZH_CN, L"zh-CN");

    test_language_parity();
    test_finished_text();
    test_known_strings();

    T_SEC(L"Result");
    T_KV(L"passed", L"%d", g_pass);
    T_KV(L"failed", L"%d", g_fail);

    if (g_fail == 0) {
        T_KV(L"VERDICT", L"PASS - all %d assertions hold", g_pass);
    } else {
        T_KV(L"VERDICT", L"FAIL - %d assertion(s) broken", g_fail);
    }

    if (!no_pause) t_pause();

    t_log_close();
    return (g_fail == 0) ? 0 : 1;
}
