/*
 * nav.h - pages and the navigation stack.
 *
 * The main window is ONE window with several pages that replace each other's
 * content. A page never opens a window of its own (the single exception is the
 * close-confirmation dialog, which is a destructive-action prompt and must be
 * able to interrupt whatever page is showing).
 *
 * The stack is plain data with no Win32 dependency and no side effects - the
 * same split that makes relcoord and monitor matching testable. It is covered
 * by tests/t13_navstack.c.
 */

#ifndef WAC_NAV_H
#define WAC_NAV_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAGE_OVERVIEW = 0,   /* default: running windows + window presets */
    PAGE_WINDOWS,        /* window presets (CRUD) */
    PAGE_CREATE_PRESET,  /* pick a window to remember (a snapshot page) */
    PAGE_WORKSPACES,     /* workspace presets */
    PAGE_SETTINGS,
    PAGE_HELP,           /* the user guide, reached from Settings */
    PAGE_COUNT
} PageId;

#define NAV_STACK_MAX 8

typedef struct {
    PageId items[NAV_STACK_MAX];
    int    depth;        /* entries in use; the top is items[depth-1] */
} NavStack;

/* Reset to the root page (overview). */
void nav_init(NavStack *n);

/* The page currently on top. Never invalid: an empty stack reads as overview. */
PageId nav_current(const NavStack *n);

/* TRUE when there is nothing to go back to. */
BOOL nav_at_root(const NavStack *n);

/*
 * Push a page. Returns TRUE when the stack changed.
 *
 * Pushing the page that is already on top is a no-op: clicking the navigation
 * button of the page you are looking at must not grow the stack, otherwise
 * "back" would need several clicks to escape.
 *
 * This is for SUB-pages (the create-preset picker, the help page). Top-level
 * navigation uses nav_set().
 */
BOOL nav_push(NavStack *n, PageId page);

/*
 * Show a TOP-LEVEL page: everything above the root is REPLACED, not stacked.
 *
 * The navigation strip means "show me this page", not "go deeper". Stacking it
 * meant clicking window-presets -> workspace-presets -> window-presets left
 * three entries behind, so one Back did not return to the overview and the
 * stack grew without bound. After nav_set() the depth is at most two: the root
 * and the page you are looking at.
 *
 * Returns TRUE when the visible page changed.
 */
BOOL nav_set(NavStack *n, PageId page);

/* Pop one level. Returns TRUE when the stack changed. Never pops the root. */
BOOL nav_pop(NavStack *n);

/* Go straight back to the root. Returns TRUE when the stack changed. */
BOOL nav_reset(NavStack *n);

/* Bounds check, so a corrupt id can never index a page table. */
BOOL nav_is_valid(PageId page);

#ifdef __cplusplus
}
#endif

#endif /* WAC_NAV_H */
