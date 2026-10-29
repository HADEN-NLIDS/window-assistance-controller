/*
 * nav.c - the navigation stack. Pure data, no Win32 calls.
 *
 * Kept free of window handles on purpose: the rule that makes this project
 * testable is that anything with logic in it is separated from anything that
 * touches the OS. See docs/架构与数据结构.md section 1.1.
 */

#include "nav.h"

BOOL nav_is_valid(PageId page)
{
    return (page >= 0 && page < PAGE_COUNT) ? TRUE : FALSE;
}

void nav_init(NavStack *n)
{
    if (n == NULL) return;

    ZeroMemory(n, sizeof(*n));
    n->items[0] = PAGE_OVERVIEW;
    n->depth    = 1;
}

PageId nav_current(const NavStack *n)
{
    /* An uninitialised or emptied stack reads as the root rather than
     * returning a bogus id - callers never have to guard against it. */
    if (n == NULL || n->depth <= 0) return PAGE_OVERVIEW;
    return n->items[n->depth - 1];
}

BOOL nav_at_root(const NavStack *n)
{
    return (n == NULL || n->depth <= 1) ? TRUE : FALSE;
}

BOOL nav_push(NavStack *n, PageId page)
{
    if (n == NULL || !nav_is_valid(page)) return FALSE;

    if (n->depth <= 0) nav_init(n);

    if (nav_current(n) == page) return FALSE;
    if (n->depth >= NAV_STACK_MAX) return FALSE;

    n->items[n->depth] = page;
    n->depth++;
    return TRUE;
}

/*
 * Top-level navigation: replace everything above the root.
 *
 * See the header for why. The root is preserved so Back always means "up one
 * level" and never "out of the application".
 */
BOOL nav_set(NavStack *n, PageId page)
{
    if (n == NULL || !nav_is_valid(page)) return FALSE;

    if (n->depth <= 0) nav_init(n);

    const PageId root = n->items[0];
    const BOOL changed = (nav_current(n) != page);

    n->items[0] = root;
    if (page == root) {
        n->depth = 1;
    } else {
        n->items[1] = page;
        n->depth = 2;
    }

    return changed;
}

BOOL nav_pop(NavStack *n)
{
    if (n == NULL) return FALSE;

    /* The root is never popped: "back" from the overview would leave the
     * window showing nothing. */
    if (n->depth <= 1) return FALSE;

    n->depth--;
    return TRUE;
}

BOOL nav_reset(NavStack *n)
{
    if (n == NULL) return FALSE;
    if (n->depth == 1 && n->items[0] == PAGE_OVERVIEW) return FALSE;

    nav_init(n);
    return TRUE;
}
