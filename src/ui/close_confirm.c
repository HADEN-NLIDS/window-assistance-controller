/*
 * close_confirm.c - the "what should the X button do?" prompt.
 *
 * See close_confirm.h for why this is a dialog rather than a message box, and
 * for why the radio buttons replace Yes/No.
 *
 * All text comes from src/ui/i18n.c at run time, so the template in
 * res/wac.rc carries no captions.
 */

#include "close_confirm.h"

#include "i18n.h"
#include "../res/resource.h"

#include <string.h>

/*
 * Dialog parameters.
 *
 * Passed through DialogBoxParam's lParam rather than a static, so two things
 * are guaranteed: no hidden lifetime assumptions, and the "previous answer"
 * is what the caller says it is rather than whatever ran last.
 */
typedef struct {
    CloseAction current;
    BOOL        was_asked_before;
    BOOL        ok;              /* set when the user pressed OK */
    CloseAction chosen;
    BOOL        remember;
} ConfirmCtx;

/* ------------------------------------------------------------------ */

static void centre_on_owner(HWND dlg)
{
    RECT r, ro;
    if (!GetWindowRect(dlg, &r)) return;

    HWND owner = GetParent(dlg);
    if (owner == NULL || !GetWindowRect(owner, &ro)) return;

    SetWindowPos(dlg, NULL,
                 ro.left + ((ro.right - ro.left) - (r.right - r.left)) / 2,
                 ro.top + ((ro.bottom - ro.top) - (r.bottom - r.top)) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static INT_PTR CALLBACK confirm_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    static ConfirmCtx *ctx;

    switch (msg) {

    case WM_INITDIALOG: {
        ctx = (ConfirmCtx *)lp;
        if (ctx == NULL) { EndDialog(dlg, IDCANCEL); return TRUE; }

        SetWindowTextW(dlg, i18n_text(STR_ERR_TITLE));

        SetDlgItemTextW(dlg, IDC_CLOSE_TEXT,     i18n_text(STR_MSG_CLOSE_ASK));
        SetDlgItemTextW(dlg, IDC_CLOSE_OPT_KEEP, i18n_text(STR_MSG_CLOSE_ASK2));
        SetDlgItemTextW(dlg, IDC_CLOSE_OPT_QUIT, i18n_text(STR_MSG_CLOSE_ASK3));
        SetDlgItemTextW(dlg, IDC_CLOSE_REMEMBER, i18n_text(STR_MSG_CLOSE_ASK4));
        SetDlgItemTextW(dlg, IDOK,               i18n_text(STR_MSG_OK));
        SetDlgItemTextW(dlg, IDCANCEL,           i18n_text(STR_MSG_CANCEL));

        /* Preselect what the user chose last time. When they have never been
         * asked, "keep running" is preselected because it is the reversible
         * choice - the window can still be closed from the File menu. */
        CheckRadioButton(dlg,
                         IDC_CLOSE_OPT_KEEP, IDC_CLOSE_OPT_QUIT,
                         (ctx->current == CLOSE_ACTION_QUIT) ? IDC_CLOSE_OPT_QUIT
                                                             : IDC_CLOSE_OPT_KEEP);

        CheckDlgButton(dlg, IDC_CLOSE_REMEMBER,
                       ctx->was_asked_before ? BST_CHECKED : BST_UNCHECKED);

        /* The same glyph a message box would show, loaded at run time so no
         * icon has to be compiled into the resources. */
        {
            HICON icon = LoadIconW(NULL, IDI_QUESTION);
            if (icon != NULL) {
                SendDlgItemMessageW(dlg, IDC_CLOSE_ICON, STM_SETICON,
                                    (WPARAM)icon, 0);
            }
        }

        centre_on_owner(dlg);

        /* Focus the first radio so Tab and the arrow keys work immediately,
         * and so Enter activates the preselected (safe) option rather than
         * some default nobody chose. */
        SetFocus(GetDlgItem(dlg, IDC_CLOSE_OPT_KEEP));
        return FALSE;   /* focus set explicitly */
    }

    case WM_COMMAND:
        if (ctx == NULL) break;

        switch (LOWORD(wp)) {
        case IDOK:
            ctx->chosen = (IsDlgButtonChecked(dlg, IDC_CLOSE_OPT_QUIT) == BST_CHECKED)
                              ? CLOSE_ACTION_QUIT
                              : CLOSE_ACTION_TRAY;
            ctx->remember = (IsDlgButtonChecked(dlg, IDC_CLOSE_REMEMBER) == BST_CHECKED);
            ctx->ok = TRUE;
            EndDialog(dlg, IDOK);
            return TRUE;

        case IDCANCEL:
            /* Escape or Cancel: leave everything as it was. */
            ctx->ok = FALSE;
            EndDialog(dlg, IDCANCEL);
            return TRUE;

        default:
            break;
        }
        return FALSE;

    case WM_CLOSE:
        if (ctx) ctx->ok = FALSE;
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }

    return FALSE;
}

/* ------------------------------------------------------------------ */

BOOL close_confirm_show(HWND owner, CloseAction current,
                        BOOL was_asked_before, CloseConfirmResult *out)
{
    if (out == NULL) return FALSE;

    memset(out, 0, sizeof(*out));

    ConfirmCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.current          = current;
    ctx.was_asked_before = was_asked_before;

    const INT_PTR r = DialogBoxParamW(
        GetModuleHandleW(NULL),
        MAKEINTRESOURCEW(IDD_CLOSE_CONFIRM),
        owner,
        confirm_proc,
        (LPARAM)&ctx);

    /* Anything other than IDOK - Cancel, Escape, a failed dialog - changes
     * nothing. Read the outcome from ctx rather than from the return value,
     * so the two can never disagree. */
    if (r != IDOK || !ctx.ok) return FALSE;

    out->action   = ctx.chosen;
    out->remember = ctx.remember;
    return TRUE;
}
