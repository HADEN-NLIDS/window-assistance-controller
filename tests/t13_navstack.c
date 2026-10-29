/*
 * t13_navstack.c - unit tests for src/ui/nav.c
 *
 * The navigation stack decides which page the main window shows. It is pure
 * data with no Win32 calls, which is exactly why it can be tested here instead
 * of by clicking buttons: the interesting behaviour is the edge cases - the
 * root that must never be popped, the repeated push that must not grow the
 * stack, and the full stack that must refuse rather than corrupt itself.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t13_navstack
 *   tests\out\t13_navstack.exe --no-pause
 */

#include "test_common.h"

#include "../src/ui/nav.h"

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

static void test_init(void)
{
    T_SEC(L"initial state");

    NavStack n;
    nav_init(&n);

    ok_int(L"depth is 1",            n.depth, 1);
    ok_int(L"current is overview",   (int)nav_current(&n), (int)PAGE_OVERVIEW);
    ok(L"at_root is TRUE",           nav_at_root(&n));
    ok_int(L"root slot is overview", (int)n.items[0], (int)PAGE_OVERVIEW);
}

static void test_push_pop(void)
{
    T_SEC(L"push and pop");

    NavStack n;
    nav_init(&n);

    ok(L"push windows changes the stack", nav_push(&n, PAGE_WINDOWS));
    ok_int(L"depth is 2",             n.depth, 2);
    ok_int(L"current is windows",     (int)nav_current(&n), (int)PAGE_WINDOWS);
    ok(L"at_root is FALSE",           !nav_at_root(&n));

    ok(L"push settings changes it again", nav_push(&n, PAGE_SETTINGS));
    ok_int(L"depth is 3",             n.depth, 3);
    ok_int(L"current is settings",    (int)nav_current(&n), (int)PAGE_SETTINGS);

    ok(L"pop returns to windows",     nav_pop(&n));
    ok_int(L"current is windows",     (int)nav_current(&n), (int)PAGE_WINDOWS);

    ok(L"pop returns to overview",    nav_pop(&n));
    ok_int(L"current is overview",    (int)nav_current(&n), (int)PAGE_OVERVIEW);
    ok(L"at_root is TRUE again",      nav_at_root(&n));

    /* The root must never be popped: "back" from the overview would leave the
     * window with no page at all. */
    ok(L"pop at root is refused",     !nav_pop(&n));
    ok_int(L"depth is still 1",       n.depth, 1);
}

static void test_repeat_push(void)
{
    T_SEC(L"pushing the current page");

    NavStack n;
    nav_init(&n);

    ok(L"first push accepted",        nav_push(&n, PAGE_WINDOWS));
    ok_int(L"depth is 2",             n.depth, 2);

    /* Clicking the navigation button of the page you are already on must do
     * nothing, otherwise "back" needs several clicks to escape. */
    ok(L"same page refused",          !nav_push(&n, PAGE_WINDOWS));
    ok_int(L"depth still 2",          n.depth, 2);

    ok(L"a different page is accepted", nav_push(&n, PAGE_SETTINGS));
    ok_int(L"depth is 3",             n.depth, 3);
}

static void test_full_stack(void)
{
    T_SEC(L"full stack");

    NavStack n;
    nav_init(&n);

    /* Fill with alternating pages: consecutive duplicates are refused by
     * design, so the filler has to vary. */
    int pushed = 0;
    for (int i = 0; i < NAV_STACK_MAX + 4; ++i) {
        const PageId p = (i % 2) ? PAGE_WINDOWS : PAGE_WORKSPACES;
        if (nav_push(&n, p)) pushed++;
    }

    ok_int(L"depth is capped at NAV_STACK_MAX", n.depth, NAV_STACK_MAX);
    ok_int(L"accepted pushes",                  pushed, NAV_STACK_MAX - 1);
    ok_int(L"current is the last accepted",     (int)nav_current(&n),
           (int)(((NAV_STACK_MAX - 1 + 1) % 2) ? PAGE_WINDOWS : PAGE_WORKSPACES));

    /* And a refused push must leave the stack usable. */
    ok(L"pop still works",            nav_pop(&n));
    ok_int(L"depth decreased",        n.depth, NAV_STACK_MAX - 1);
}

static void test_reset(void)
{
    T_SEC(L"reset to root");

    NavStack n;
    nav_init(&n);
    nav_push(&n, PAGE_WINDOWS);
    nav_push(&n, PAGE_SETTINGS);
    ok_int(L"depth is 3",             n.depth, 3);

    ok(L"reset reports a change",     nav_reset(&n));
    ok_int(L"depth is 1",             n.depth, 1);
    ok_int(L"current is overview",    (int)nav_current(&n), (int)PAGE_OVERVIEW);

    ok(L"reset at root reports no change", !nav_reset(&n));
}

static void test_invalid(void)
{
    T_SEC(L"invalid input");

    NavStack n;
    nav_init(&n);

    ok(L"PAGE_COUNT is not a page",   !nav_push(&n, PAGE_COUNT));
    ok(L"negative is not a page",     !nav_push(&n, (PageId)-1));
    ok_int(L"depth unchanged",        n.depth, 1);

    ok(L"nav_is_valid rejects PAGE_COUNT", !nav_is_valid(PAGE_COUNT));
    ok(L"nav_is_valid rejects -1",         !nav_is_valid((PageId)-1));
    ok(L"nav_is_valid accepts overview",   nav_is_valid(PAGE_OVERVIEW));

    /* NULL must be survivable: a caller that forgot nav_init() should get the
     * root page, not a crash. */
    ok_int(L"nav_current(NULL) is overview", (int)nav_current(NULL), (int)PAGE_OVERVIEW);
    ok(L"nav_at_root(NULL) is TRUE",         nav_at_root(NULL));
    ok(L"nav_push(NULL) is refused",         !nav_push(NULL, PAGE_WINDOWS));
    ok(L"nav_pop(NULL) is refused",          !nav_pop(NULL));
    ok(L"nav_reset(NULL) is refused",        !nav_reset(NULL));

    /* An empty stack (depth 0) is what a zeroed struct looks like; pushing
     * onto it must repair it rather than index items[-1]. */
    NavStack zero;
    ZeroMemory(&zero, sizeof(zero));
    ok_int(L"zeroed stack reads as overview", (int)nav_current(&zero), (int)PAGE_OVERVIEW);
    ok(L"push onto a zeroed stack works",     nav_push(&zero, PAGE_WINDOWS));
    ok_int(L"depth is 2 after repair",        zero.depth, 2);
    ok_int(L"current is windows",             (int)nav_current(&zero), (int)PAGE_WINDOWS);
}

/* ------------------------------------------------------------------ */

static void test_nav_set(void)
{
    T_SEC(L"nav_set - top-level navigation replaces instead of stacking");

    NavStack n;
    nav_init(&n);

    /* The reported bug: window-presets -> workspace-presets -> window-presets
     * left three entries behind, so one Back did not reach the overview. */
    ok(L"set to windows reports a change", nav_set(&n, PAGE_WINDOWS));
    ok_int(L"depth is 2", n.depth, 2);

    ok(L"set to workspaces reports a change", nav_set(&n, PAGE_WORKSPACES));
    ok_int(L"depth is still 2", n.depth, 2);

    ok(L"back to windows reports a change", nav_set(&n, PAGE_WINDOWS));
    ok_int(L"depth is still 2 after three clicks", n.depth, 2);

    ok(L"one pop succeeds", nav_pop(&n));
    ok(L"and it reaches the root", nav_at_root(&n));
    ok_int(L"the root is the overview", (int)nav_current(&n), (int)PAGE_OVERVIEW);

    /* Selecting the root itself collapses the stack rather than pushing it. */
    nav_set(&n, PAGE_SETTINGS);
    ok(L"set to the root reports a change", nav_set(&n, PAGE_OVERVIEW));
    ok_int(L"depth is 1", n.depth, 1);
    ok(L"and we are at the root", nav_at_root(&n));

    /* Re-selecting the page already showing is not a change. */
    ok(L"first set to help changes", nav_set(&n, PAGE_HELP));
    ok(L"second set to help does not", !nav_set(&n, PAGE_HELP));
    ok_int(L"depth unchanged", n.depth, 2);

    /* Invalid input never corrupts the stack. */
    const int before = n.depth;
    ok(L"invalid page refused", !nav_set(&n, (PageId)999));
    ok_int(L"depth untouched", n.depth, before);
    ok(L"NULL stack refused", !nav_set(NULL, PAGE_WINDOWS));

    /* Every page can be reached and left with a single pop. */
    for (int p = 0; p < PAGE_COUNT; ++p) {
        NavStack one;
        nav_init(&one);
        nav_set(&one, (PageId)p);

        if ((PageId)p == PAGE_OVERVIEW) {
            /* The overview IS the root: it has nothing to pop to, and nav_pop
             * is documented never to pop the root. */
            ok(L"the overview leaves the stack at the root", nav_at_root(&one));
            continue;
        }

        const BOOL back = nav_pop(&one);
        wchar_t what[64];
        _snwprintf_s(what, 64, _TRUNCATE, L"page %d: one pop returns to the root", p);
        ok(what, back && nav_at_root(&one));
    }
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t13_navstack");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t13_navstack - navigation stack");
    T_KV(L"run_at", L"%ls", t_now_string());

    test_init();
    test_push_pop();
    test_repeat_push();
    test_full_stack();
    test_reset();
    test_invalid();
    test_nav_set();

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
