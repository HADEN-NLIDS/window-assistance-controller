/*
 * pagelist.c - see the header for why this is its own module.
 *
 * The functions here were lifted out of mainwindow.c unchanged in behaviour.
 * The one that gained something is pagelist_column_widths(): the arithmetic was
 * always there, but it was reached only through a live HWND, so it could not be
 * tested. Now it can (tests/t12_uiformat.c).
 */

#include "pagelist.h"

#include <commctrl.h>

#include <stdlib.h>

BOOL pagelist_column_widths(const int *weights, int ncols, int avail, int *out)
{
    if (weights == NULL || out == NULL) return FALSE;
    if (ncols <= 0 || avail <= 0) return FALSE;

    int total = 0;
    for (int i = 0; i < ncols; ++i) {
        if (weights[i] <= 0) return FALSE;   /* a column with no width */
        total += weights[i];
    }
    if (total <= 0) return FALSE;

    int used = 0;
    for (int i = 0; i < ncols; ++i) {
        /*
         * The last column takes whatever is left rather than its rounded share.
         * Rounding every column independently can leave the total one or two
         * pixels short of `avail`, and a list whose columns do not fill it
         * shows a horizontal scrollbar with nothing beyond it.
         */
        const int w = (i == ncols - 1) ? (avail - used)
                                       : MulDiv(avail, weights[i], total);
        out[i] = w;
        used += w;
    }

    return TRUE;
}

HWND pagelist_create(HWND parent, HINSTANCE hinst, int id,
                     const PageColumn *cols, int ncols)
{
    HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | LVS_REPORT |
                                    LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, hinst, NULL);
    if (list == NULL) return NULL;

    /* Full-row select and grid lines: this is a data table, and the eye needs
     * the row to read across five columns. Double buffering is not optional -
     * the lists are rebuilt on every refresh. */
    ListView_SetExtendedListViewStyle(list,
                                      LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
                                          LVS_EX_DOUBLEBUFFER);

    for (int i = 0; i < ncols; ++i) {
        LVCOLUMNW c;
        ZeroMemory(&c, sizeof(c));
        c.mask     = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText  = (LPWSTR)cols[i].text;
        /* A provisional width; pagelist_fit_columns() sets the real one before
         * the window is shown. */
        c.cx       = cols[i].weight;
        c.iSubItem = i;
        SendMessageW(list, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&c);
    }

    return list;
}

void pagelist_fit_columns(HWND list, const PageColumn *cols, int ncols, int client_w)
{
    if (list == NULL || cols == NULL || ncols <= 0) return;

    int weights[16];
    if (ncols > (int)(sizeof(weights) / sizeof(weights[0]))) return;

    for (int i = 0; i < ncols; ++i) weights[i] = cols[i].weight;

    pagelist_fit_weights(list, weights, ncols, client_w);
}

void pagelist_fit_weights(HWND list, const int *weights, int ncols, int client_w)
{
    if (list == NULL || weights == NULL || ncols <= 0 || client_w <= 0) return;

    /* The vertical scrollbar takes its space from the client area as soon as
     * the list is long enough to need one. */
    const int usable = client_w - GetSystemMetrics(SM_CXVSCROLL) - 4;
    if (usable < 80) return;   /* too narrow to be worth dividing */

    int widths[16];
    if (ncols > (int)(sizeof(widths) / sizeof(widths[0]))) return;

    if (!pagelist_column_widths(weights, ncols, usable, widths)) return;

    for (int i = 0; i < ncols; ++i) {
        if (widths[i] > 20) {
            ListView_SetColumnWidth(list, i, widths[i]);
        }
    }
}

void pagelist_select_first(HWND list)
{
    if (list == NULL) return;
    if (ListView_GetItemCount(list) <= 0) return;

    LVITEMW it;
    ZeroMemory(&it, sizeof(it));
    it.mask      = LVIF_STATE;
    it.iItem     = 0;
    it.state     = LVIS_SELECTED | LVIS_FOCUSED;
    it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(list, LVM_SETITEMSTATE, 0, (LPARAM)&it);
    SendMessageW(list, LVM_ENSUREVISIBLE, 0, FALSE);
}

int pagelist_selected(HWND list)
{
    if (list == NULL) return -1;
    return ListView_GetNextItem(list, -1, LVNI_SELECTED);
}

void pagelist_select_row(HWND list, int row)
{
    if (list == NULL || row < 0) return;
    if (row >= ListView_GetItemCount(list)) return;

    LVITEMW it;
    ZeroMemory(&it, sizeof(it));
    it.mask      = LVIF_STATE;
    it.iItem     = row;
    it.state     = LVIS_SELECTED | LVIS_FOCUSED;
    it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(list, LVM_SETITEMSTATE, (WPARAM)row, (LPARAM)&it);
    SendMessageW(list, LVM_ENSUREVISIBLE, (WPARAM)row, FALSE);
}

void pagelist_begin(HWND list)
{
    if (list == NULL) return;
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
}

void pagelist_end(HWND list)
{
    if (list == NULL) return;
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, NULL, TRUE);
}

void pagelist_show_empty(HWND list, const wchar_t *text)
{
    if (list == NULL || text == NULL) return;

    LVITEMW item;
    ZeroMemory(&item, sizeof(item));
    item.mask    = LVIF_TEXT;
    item.iItem   = 0;
    item.pszText = (LPWSTR)text;
    SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item);
}

int pagelist_add_row(HWND list, const wchar_t *const *cells, int ncols)
{
    if (list == NULL || cells == NULL || ncols <= 0) return -1;

    LVITEMW item;
    ZeroMemory(&item, sizeof(item));
    item.mask    = LVIF_TEXT;
    item.iItem   = ListView_GetItemCount(list);
    item.pszText = (cells[0] != NULL) ? (LPWSTR)cells[0] : L"";

    const int row = (int)SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item);
    if (row < 0) return -1;

    for (int i = 1; i < ncols; ++i) {
        ListView_SetItemText(list, row, i,
                             (cells[i] != NULL) ? (LPWSTR)cells[i] : L"");
    }

    return row;
}
