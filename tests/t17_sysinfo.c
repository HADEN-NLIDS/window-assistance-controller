/*
 * t17_sysinfo.c - tests for src/util/sysinfo.c
 *
 * WHAT CAN AND CANNOT BE ASSERTED HERE
 *
 *   These functions are thin wrappers over Win32, so there is no arithmetic to
 *   pin down. What is worth asserting is the part that CAN silently go wrong:
 *
 *     - the startup folder has to resolve to a directory that really exists,
 *       otherwise the guide's [Open startup folder] button does nothing and the
 *       user is told to follow steps they cannot start (the failure would look
 *       like "the button is broken", not "the path is wrong");
 *     - every function must survive bad arguments without writing anything -
 *       an empty buffer that gets filled with garbage is worse than a FALSE.
 *
 *   The elevation answer is RECORDED rather than asserted: a console test
 *   cannot make itself elevated, so a hard assertion would only be able to
 *   check one of the two branches. Run this once normally and once from an
 *   elevated prompt and compare the logged line - that is the honest check.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t17_sysinfo
 *   tests\out\t17_sysinfo.exe --no-pause
 */

#include "test_common.h"

#include "../src/util/sysinfo.h"

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

static void test_startup_folder(void)
{
    T_SEC(L"startup folder");

    wchar_t path[MAX_PATH];
    path[0] = L'\0';

    const BOOL got = sysinfo_startup_folder(path, MAX_PATH);
    ok(L"resolves", got);
    T_KV(L"path", L"%ls", path);

    if (!got) return;

    ok(L"is not empty", path[0] != L'\0');

    /*
     * The folder must EXIST. A path that merely looks right - for instance
     * one built by hand from %APPDATA% on a machine with a redirected profile
     * - would raise a shell error dialog at the moment the user clicks the
     * button, which is the worst place to find out.
     */
    const DWORD attrs = GetFileAttributesW(path);
    ok(L"exists on disk", attrs != INVALID_FILE_ATTRIBUTES);
    ok(L"is a directory",
       attrs != INVALID_FILE_ATTRIBUTES &&
       (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);

    /* A buffer too small must fail cleanly and leave nothing behind.
     *
     * This assertion caught a real overrun: the first version passed the
     * caller's buffer straight to SHGetFolderPathW, which takes no size and
     * documents a MAX_PATH destination. With a 4-character buffer it wrote
     * past the end. The implementation now fills a local MAX_PATH buffer and
     * copies out afterwards, so the short case is reportable. */
    wchar_t small[4];
    small[0] = L'x';
    ok(L"a 4-character buffer is refused", !sysinfo_startup_folder(small, 4));
    ok(L"the short buffer was cleared", small[0] == L'\0');

    ok(L"NULL buffer refused", !sysinfo_startup_folder(NULL, MAX_PATH));
    ok(L"zero capacity refused", !sysinfo_startup_folder(path, 0));
}

static void test_open_folder_arguments(void)
{
    T_SEC(L"open folder - argument handling");

    /*
     * Only the rejection paths are checked: actually opening Explorer from a
     * test would leave a window on the user's desktop, and the success path is
     * the shell's behaviour, not ours.
     */
    ok(L"NULL path refused", !sysinfo_open_folder(NULL, NULL));
    ok(L"empty path refused", !sysinfo_open_folder(NULL, L""));
}

static void test_elevation(void)
{
    T_SEC(L"elevation");

    const BOOL elevated = sysinfo_is_elevated();
    ok(L"answers with TRUE or FALSE", elevated == TRUE || elevated == FALSE);

    /*
     * Recorded, not asserted - see the file header. The expected readings are:
     *   normal prompt     -> no      (build\wac.exe started by double-click)
     *   elevated prompt   -> yes     (right-click, "Run as administrator")
     */
    T_KV(L"elevated", L"%ls", elevated ? L"yes" : L"no");
    T_KV(L"process", L"%ls",
         elevated ? L"administrator token"
                  : L"standard token (UIPI applies to elevated windows)");
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t17_sysinfo");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t17_sysinfo - machine and process facts");
    T_KV(L"run_at", L"%ls", t_now_string());

    test_startup_folder();
    test_open_folder_arguments();
    test_elevation();

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
