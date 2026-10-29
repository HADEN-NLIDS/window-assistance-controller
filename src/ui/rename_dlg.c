/*
 * rename_dlg.c - see rename_dlg.h.
 */

#include "rename_dlg.h"

#include "i18n.h"

#include "../res/resource.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Dialog procedure                                                    */
/* ------------------------------------------------------------------ */

/*
 * One at a time, and the caller is blocked inside DialogBoxParam while this is
 * on screen, so a single slot is enough - the same reasoning settings_dlg.c
 * used before it became a page.
 */
static wchar_t *g_out;
static size_t   g_out_cch;

/* Enable OK only when the field holds something that is not just spaces. */
static void refresh_ok(HWND dlg)
{
    wchar_t text[256];
    text[0] = L'\0';
    GetDlgItemTextW(dlg, IDC_RENAME_EDIT, text, 256);

    BOOL has_content = FALSE;
    for (const wchar_t *p = text; *p; ++p) {
        if (*p != L' ' && *p != L'\t') { has_content = TRUE; break; }
    }

    EnableWindow(GetDlgItem(dlg, IDOK), has_content);
}

static INT_PTR CALLBACK rename_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_INITDIALOG: {
        SetWindowTextW(dlg, i18n_text(STR_RENAME_TITLE));
        SetDlgItemTextW(dlg, IDC_RENAME_LABEL, i18n_text(STR_RENAME_LABEL));
        SetDlgItemTextW(dlg, IDOK,             i18n_text(STR_SETTINGS_OK));
        SetDlgItemTextW(dlg, IDCANCEL,         i18n_text(STR_SETTINGS_CANCEL));

        SetDlgItemTextW(dlg, IDC_RENAME_EDIT, (lp != 0) ? (const wchar_t *)lp : L"");

        /* Select the whole name: the common case is replacing it. */
        SendDlgItemMessageW(dlg, IDC_RENAME_EDIT, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dlg, IDC_RENAME_EDIT));

        refresh_ok(dlg);
        return FALSE;   /* the focus was set above, do not let the system move it */
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {

        case IDC_RENAME_EDIT:
            if (HIWORD(wp) == EN_CHANGE) refresh_ok(dlg);
            return TRUE;

        case IDOK: {
            if (g_out == NULL || g_out_cch == 0) { EndDialog(dlg, IDCANCEL); return TRUE; }

            wchar_t text[256];
            text[0] = L'\0';
            GetDlgItemTextW(dlg, IDC_RENAME_EDIT, text, 256);

            /* Trim, so "  Coding  " is stored as "Coding" - the id is derived
             * from the name and leading spaces would end up as leading dashes. */
            wchar_t *s = text;
            while (*s == L' ' || *s == L'\t') ++s;
            wchar_t *e = s + wcslen(s);
            while (e > s && (e[-1] == L' ' || e[-1] == L'\t')) --e;
            *e = L'\0';

            if (*s == L'\0') { EndDialog(dlg, IDCANCEL); return TRUE; }

            wcsncpy_s(g_out, g_out_cch, s, _TRUNCATE);
            EndDialog(dlg, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            /* Nothing is written, so Cancel genuinely cancels. */
            EndDialog(dlg, IDCANCEL);
            return TRUE;

        default:
            break;
        }
        return FALSE;

    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }

    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

BOOL rename_dlg_prompt(HWND owner, const wchar_t *initial, wchar_t *out, size_t cch)
{
    if (out == NULL || cch == 0) return FALSE;

    g_out     = out;
    g_out_cch = cch;

    const INT_PTR r = DialogBoxParamW(
        GetModuleHandleW(NULL),
        MAKEINTRESOURCEW(IDD_RENAME_WS),
        owner,
        rename_proc,
        (LPARAM)((initial != NULL) ? initial : L""));

    g_out     = NULL;
    g_out_cch = 0;

    return r == IDOK;
}
