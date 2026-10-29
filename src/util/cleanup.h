/*
 * cleanup.h - a registry of resources to release at shutdown.
 *
 * WHY THIS EXISTS
 *
 *   A Win32 program creates GDI objects, handles and windows along many code
 *   paths, including error paths that return early. Releasing each one at
 *   every exit point is how leaks appear: one forgotten branch, and the
 *   process holds a font forever.
 *
 *   Here a resource is registered the moment it is created, and one call at
 *   shutdown releases everything still alive, in reverse order. An error path
 *   then only has to return - it does not have to remember what it allocated.
 *
 * WHAT IT IS NOT
 *
 *   Not a garbage collector and not a general allocator. It holds a small
 *   fixed table and is meant for the handful of long-lived objects a run
 *   creates (the main window's font, the cached brushes, an icon list), not
 *   for per-iteration temporaries - those belong in the function that uses
 *   them, released there.
 *
 *   The table is fixed-size on purpose: registration happens during startup,
 *   and a failed registration is reported rather than growing the table
 *   silently. If this ever fills up, the right fix is to release something
 *   earlier, not to raise the limit.
 *
 * ORDER
 *
 *   Released in reverse registration order, so a resource registered later
 *   (which may depend on an earlier one) is freed first. The main window is
 *   registered last and therefore destroyed first, which is what its child
 *   controls need.
 */

#ifndef WAC_CLEANUP_H
#define WAC_CLEANUP_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLEANUP_MAX_ITEMS 64

typedef enum {
    CLEANUP_CLOSE_HANDLE = 0,  /* CloseHandle */
    CLEANUP_DELETE_OBJECT,     /* DeleteObject  - GDI */
    CLEANUP_DELETE_DC,         /* DeleteDC      - GDI */
    CLEANUP_RELEASE_DC,        /* ReleaseDC     - needs the window too */
    CLEANUP_DESTROY_WINDOW,    /* DestroyWindow */
    CLEANUP_DESTROY_MENU,      /* DestroyMenu */
    CLEANUP_FREE,              /* free          - heap memory */
    CLEANUP_CUSTOM             /* user function */
} CleanupKind;

typedef void (*CleanupCustomFn)(void *ptr);

/*
 * Register a resource for shutdown release. Duplicate registrations of the
 * same value and kind are ignored, so it is safe to register a window that
 * may already have been registered.
 *
 * Returns FALSE when the table is full - which is a bug in the caller's
 * resource management rather than a runtime condition to handle, so callers
 * should assert on it rather than silently continuing.
 */
BOOL cleanup_register(CleanupKind kind, void *value, void *context);

/* Convenience: register a custom destructor with its argument. */
BOOL cleanup_register_fn(CleanupCustomFn fn, void *ptr);

/* Cancel a registration - for a resource that was already released on a
 * normal path, so shutdown does not release it twice. */
BOOL cleanup_forget(CleanupKind kind, void *value);

/*
 * Release everything registered, in reverse order, and empty the table.
 * Safe to call more than once and from the shutdown path only.
 */
void cleanup_run_all(void);

/* Number of items currently registered. Used by tests and by the shutdown
 * log line, which makes a leak visible as a non-zero count. */
int cleanup_count(void);

#ifdef __cplusplus
}
#endif

#endif /* WAC_CLEANUP_H */
