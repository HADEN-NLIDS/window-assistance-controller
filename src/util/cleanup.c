/*
 * cleanup.c - a registry of resources to release at shutdown.
 *
 * See cleanup.h for why this exists and what it is deliberately not.
 *
 * Uses no allocation of its own: a fixed table, so registering a resource can
 * never be the thing that fails because memory ran out.
 */

#include "cleanup.h"

#include <stdlib.h>   /* free */
#include <string.h>   /* memset */

typedef struct {
    CleanupKind kind;
    void       *value;
    void       *context;   /* the owner window, for CLEANUP_RELEASE_DC */
    CleanupCustomFn fn;    /* for CLEANUP_CUSTOM */
    BOOL        used;
} CleanupItem;

static CleanupItem g_items[CLEANUP_MAX_ITEMS];
static int         g_count;   /* number of used slots, not the high-water mark */

/* ------------------------------------------------------------------ */

static CleanupItem *find_item(CleanupKind kind, void *value)
{
    for (int i = 0; i < CLEANUP_MAX_ITEMS; ++i) {
        if (!g_items[i].used) continue;
        if (g_items[i].kind == kind && g_items[i].value == value) return &g_items[i];
    }
    return NULL;
}

BOOL cleanup_register(CleanupKind kind, void *value, void *context)
{
    if (value == NULL) return FALSE;

    /* Registering the same object twice would release it twice at shutdown,
     * which for a handle or a GDI object means touching freed memory. */
    if (find_item(kind, value) != NULL) return TRUE;

    for (int i = 0; i < CLEANUP_MAX_ITEMS; ++i) {
        if (g_items[i].used) continue;

        g_items[i].used    = TRUE;
        g_items[i].kind    = kind;
        g_items[i].value   = value;
        g_items[i].context = context;
        g_items[i].fn      = NULL;
        g_count++;
        return TRUE;
    }

    /* Table full: report rather than grow. See cleanup.h. */
    return FALSE;
}

BOOL cleanup_register_fn(CleanupCustomFn fn, void *ptr)
{
    if (fn == NULL) return FALSE;

    for (int i = 0; i < CLEANUP_MAX_ITEMS; ++i) {
        if (g_items[i].used) continue;

        g_items[i].used    = TRUE;
        g_items[i].kind    = CLEANUP_CUSTOM;
        g_items[i].value   = ptr;
        g_items[i].context = NULL;
        g_items[i].fn      = fn;
        g_count++;
        return TRUE;
    }
    return FALSE;
}

BOOL cleanup_forget(CleanupKind kind, void *value)
{
    CleanupItem *item = find_item(kind, value);
    if (item == NULL) return FALSE;

    memset(item, 0, sizeof(*item));
    g_count--;
    return TRUE;
}

/* ------------------------------------------------------------------ */

static void release_one(CleanupItem *item)
{
    switch (item->kind) {
    case CLEANUP_CLOSE_HANDLE:
        CloseHandle((HANDLE)item->value);
        break;

    case CLEANUP_DELETE_OBJECT:
        DeleteObject((HGDIOBJ)item->value);
        break;

    case CLEANUP_DELETE_DC:
        DeleteDC((HDC)item->value);
        break;

    case CLEANUP_RELEASE_DC:
        ReleaseDC((HWND)item->context, (HDC)item->value);
        break;

    case CLEANUP_DESTROY_WINDOW:
        /* The window may already be gone on a normal path; IsWindow keeps
         * that from turning into a use-after-free. */
        if (IsWindow((HWND)item->value)) DestroyWindow((HWND)item->value);
        break;

    case CLEANUP_DESTROY_MENU:
        DestroyMenu((HMENU)item->value);
        break;

    case CLEANUP_FREE:
        free(item->value);
        break;

    case CLEANUP_CUSTOM:
        if (item->fn != NULL) item->fn(item->value);
        break;
    }
}

void cleanup_run_all(void)
{
    /*
     * Reverse order: something registered later may depend on something
     * registered earlier. The main window is registered last so it is
     * destroyed first, before the fonts and brushes its controls use.
     */
    for (int i = CLEANUP_MAX_ITEMS - 1; i >= 0; --i) {
        if (!g_items[i].used) continue;

        release_one(&g_items[i]);
        memset(&g_items[i], 0, sizeof(g_items[i]));
    }
    g_count = 0;
}

int cleanup_count(void)
{
    return g_count;
}
