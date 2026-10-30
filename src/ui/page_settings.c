/*
 * page_settings.c - see page_settings.h.
 *
 * The controls are created once, hidden, and shown by mainwindow's
 * apply_page(). Every caption comes from src/ui/i18n.c at creation time, so
 * there is no dialog template with text baked into it and no second place for
 * a translation to go stale.
 */

#include "page_settings.h"

#include "model/store.h"

#include <stdio.h>
#include <stdlib.h>   /* calloc / free */
#include <string.h>

struct PageSettings {
    HWND      parent;
    HINSTANCE hinst;
    HFONT     font;
    int       dpi;
    BOOL      elevated;      /* fixed for the life of the process */

    HWND lbl_lang;
    HWND cb_lang;
    HWND note_lang;

    HWND grp_close;
    HWND rb_ask;
    HWND rb_quit;
    HWND rb_tray;

    HWND lbl_hotkey;
    HWND txt_hotkey;
    HWND note_hotkey;

    HWND lbl_perm;
    HWND txt_perm;
    HWND note_perm;

    HWND chk_hook;
    HWND note_hook;
    HWND chk_taskbar;

    HWND btn_guide;
    HWND btn_apply;
    HWND btn_cancel;

    /* What the controls were last filled from; page_settings_revert() puts
     * them back to this. */
    Settings loaded;
    BOOL     have_loaded;
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int px(const PageSettings *p, int logical)
{
    return MulDiv(logical, (p->dpi > 0) ? p->dpi : 96, 96);
}

/*
 * Where controls go once the page has run out of room.
 *
 * Far outside any window, so the control keeps a normal size and simply is
 * never seen. Collapsing it to zero height was the alternative and it leaves
 * whoever reads this file wondering whether the control is broken.
 */
#define OFF_PAGE (-4000)

static void place(HWND h, int x, int y, int w, int height)
{
    if (h != NULL) MoveWindow(h, x, y, w, height, TRUE);
}

/*
 * Advance the layout cursor past one block of rows.
 *
 * The buttons are pinned to the bottom of the content area and the rows are
 * placed from the top, so in a short window the two eventually meet. Measured
 * at 520x320 logical pixels: the [Apply] row used to be drawn straight across
 * the hotkey field, which reads as a rendering fault rather than as "this
 * window is too small for this page". A block that does not fit above the
 * buttons therefore takes the rest of the page with it, and the page shows
 * only what fits.
 *
 * OFF_PAGE is sticky: once the cursor is there it stays there.
 */
static int advance(int y, int block_h, int limit)
{
    if (y <= OFF_PAGE || y + block_h > limit) return OFF_PAGE;
    return y + block_h;
}

static HWND add_ctrl(PageSettings *p, const wchar_t *cls, StrId text,
                     DWORD style, DWORD ex_style, int id)
{
    /* STR_COUNT is this file's "no caption" sentinel: i18n_text() documents
     * an empty result for an out-of-range id, which is what the combo box and
     * the read-only hotkey field want. */
    HWND h = CreateWindowExW(ex_style, cls,
                             (text == STR_COUNT) ? L"" : i18n_text(text),
                             WS_CHILD | style, 0, 0, 0, 0,
                             p->parent, (HMENU)(INT_PTR)id, p->hinst, NULL);
    if (h != NULL && p->font != NULL) {
        SendMessageW(h, WM_SETFONT, (WPARAM)p->font, TRUE);
    }
    return h;
}

static void put(HWND h, StrId id)
{
    if (h != NULL) SetWindowTextW(h, i18n_text(id));
}

/* ------------------------------------------------------------------ */
/* Language combo                                                      */
/* ------------------------------------------------------------------ */

/*
 * The three entries are Follow-Windows, English, Chinese. Their labels come
 * from i18n_language_label() rather than the i18n table on purpose: a user who
 * cannot read the current interface language still has to be able to find
 * their own language in the list, so each entry is written in that language.
 */
static void fill_languages(PageSettings *p)
{
    static const WacLanguage k_langs[3] = { UI_LANG_AUTO,
                                            UI_LANG_EN,
                                            UI_LANG_ZH_CN };

    if (p->cb_lang == NULL) return;

    SendMessageW(p->cb_lang, CB_RESETCONTENT, 0, 0);

    for (int i = 0; i < 3; ++i) {
        const LRESULT idx = SendMessageW(p->cb_lang, CB_ADDSTRING, 0,
                                         (LPARAM)i18n_language_label(k_langs[i]));
        if (idx < 0) continue;
        /* The item DATA carries the value: an index would map to the wrong
         * language the moment an entry is inserted in the middle of the list. */
        SendMessageW(p->cb_lang, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)k_langs[i]);
    }
}

static void select_language(PageSettings *p, WacLanguage lang)
{
    if (p->cb_lang == NULL) return;

    const int count = (int)SendMessageW(p->cb_lang, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < count; ++i) {
        if ((WacLanguage)SendMessageW(p->cb_lang, CB_GETITEMDATA, (WPARAM)i, 0) == lang) {
            SendMessageW(p->cb_lang, CB_SETCURSEL, (WPARAM)i, 0);
            return;
        }
    }
    SendMessageW(p->cb_lang, CB_SETCURSEL, 0, 0);
}

static WacLanguage read_language(const PageSettings *p)
{
    if (p->cb_lang == NULL) return UI_LANG_AUTO;

    const LRESULT sel = SendMessageW(p->cb_lang, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) return UI_LANG_AUTO;

    const LRESULT data = SendMessageW(p->cb_lang, CB_GETITEMDATA, (WPARAM)sel, 0);
    if (data == CB_ERR) return UI_LANG_AUTO;

    return (WacLanguage)data;
}

/* ------------------------------------------------------------------ */
/* Close behaviour                                                     */
/* ------------------------------------------------------------------ */

static void select_close_action(const PageSettings *p, CloseAction action)
{
    /*
     * CheckRadioButton also clears the others in the group, which is what a
     * radio group requires; setting BM_SETCHECK on one alone leaves two
     * buttons checked.
     *
     * It - and IsDlgButtonChecked below - only look controls up by id among a
     * window's children. Neither needs a modal dialog, which is why they
     * carried over unchanged from the dialog this page replaces.
     */
    CheckRadioButton(p->parent, IDC_SETTINGS_CLOSE_ASK, IDC_SETTINGS_CLOSE_TRAY,
                     action == CLOSE_ACTION_QUIT ? IDC_SETTINGS_CLOSE_QUIT :
                     action == CLOSE_ACTION_TRAY ? IDC_SETTINGS_CLOSE_TRAY :
                                                   IDC_SETTINGS_CLOSE_ASK);
}

static CloseAction read_close_action(const PageSettings *p)
{
    if (IsDlgButtonChecked(p->parent, IDC_SETTINGS_CLOSE_QUIT) == BST_CHECKED) {
        return CLOSE_ACTION_QUIT;
    }
    if (IsDlgButtonChecked(p->parent, IDC_SETTINGS_CLOSE_TRAY) == BST_CHECKED) {
        return CLOSE_ACTION_TRAY;
    }
    return CLOSE_ACTION_ASK;
}

/* ------------------------------------------------------------------ */
/* Hotkey and permissions                                              */
/* ------------------------------------------------------------------ */

/*
 * The hotkey field shows what is actually registered, read-only. An editable
 * field that silently discards what the user types would be worse than no
 * field at all; a capture UI is a separate job.
 */
static void set_hotkey_text(PageSettings *p, const char *label)
{
    if (p->txt_hotkey == NULL) return;

    wchar_t buf[128];
    _snwprintf_s(buf, 128, _TRUNCATE, L"%hs",
                 (label != NULL && label[0] != '\0') ? label : "?");
    SetWindowTextW(p->txt_hotkey, buf);
}

/*
 * The one place that turns "is this process elevated" into words. Both this
 * page and the user-guide page show the state, and having each of them decide
 * which caption fits is how two screens end up describing the same process
 * differently.
 */
static void apply_permission_text(PageSettings *p)
{
    put(p->txt_perm,  p->elevated ? STR_SETTINGS_PERM_ADMIN
                                  : STR_SETTINGS_PERM_NORMAL);
    put(p->note_perm, p->elevated ? STR_SETTINGS_PERM_NOTE_ADMIN
                                  : STR_SETTINGS_PERM_NOTE_NORMAL);
}

/* ------------------------------------------------------------------ */
/* Life cycle                                                          */
/* ------------------------------------------------------------------ */

PageSettings *page_settings_create(HWND parent, HINSTANCE hinst, HFONT font,
                                   int dpi, BOOL elevated)
{
    PageSettings *p = (PageSettings *)calloc(1, sizeof(PageSettings));
    if (p == NULL) return NULL;

    p->parent   = parent;
    p->hinst    = hinst;
    p->font     = font;
    p->dpi      = (dpi > 0) ? dpi : 96;
    p->elevated = elevated;

    /* ---- language ------------------------------------------------ */
    p->lbl_lang  = add_ctrl(p, L"STATIC", STR_SETTINGS_LANGUAGE,
                            SS_LEFT | SS_CENTERIMAGE, 0, IDC_SETTINGS_LANG_LABEL);
    p->cb_lang   = add_ctrl(p, L"COMBOBOX", STR_COUNT,
                            WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                            0, IDC_SETTINGS_LANG);
    p->note_lang = add_ctrl(p, L"STATIC", STR_SETTINGS_LANG_NOTE,
                            SS_LEFT, 0, IDC_SETTINGS_LANG_NOTE);

    /* ---- close behaviour ----------------------------------------- */
    p->grp_close = add_ctrl(p, L"BUTTON", STR_SETTINGS_CLOSE_BEHAV,
                            BS_GROUPBOX, 0, IDC_SETTINGS_CLOSE_GROUP);
    p->rb_ask    = add_ctrl(p, L"BUTTON", STR_SETTINGS_CLOSE_ASK,
                            WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                            0, IDC_SETTINGS_CLOSE_ASK);
    p->rb_quit   = add_ctrl(p, L"BUTTON", STR_SETTINGS_CLOSE_QUIT,
                            BS_AUTORADIOBUTTON, 0, IDC_SETTINGS_CLOSE_QUIT);
    p->rb_tray   = add_ctrl(p, L"BUTTON", STR_SETTINGS_CLOSE_TRAY,
                            BS_AUTORADIOBUTTON, 0, IDC_SETTINGS_CLOSE_TRAY);

    /* ---- exit hotkey --------------------------------------------- */
    p->lbl_hotkey  = add_ctrl(p, L"STATIC", STR_SETTINGS_HOTKEY,
                              SS_LEFT | SS_CENTERIMAGE, 0, IDC_SETTINGS_HOTKEY_LABEL);
    p->txt_hotkey  = add_ctrl(p, L"EDIT", STR_COUNT,
                              ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP,
                              WS_EX_CLIENTEDGE, IDC_SETTINGS_HOTKEY);
    p->note_hotkey = add_ctrl(p, L"STATIC", STR_SETTINGS_HOTKEY_NOTE,
                              SS_LEFT | SS_CENTERIMAGE, 0, IDC_SETTINGS_HOTKEY_NOTE);

    /* ---- permissions --------------------------------------------- */
    p->lbl_perm  = add_ctrl(p, L"STATIC", STR_SETTINGS_PERM_LABEL,
                            SS_LEFT | SS_CENTERIMAGE, 0, IDC_SETTINGS_PERM_LABEL);
    p->txt_perm  = add_ctrl(p, L"STATIC", STR_COUNT,
                            SS_LEFT | SS_CENTERIMAGE, 0, IDC_SETTINGS_PERM);
    p->note_perm = add_ctrl(p, L"STATIC", STR_COUNT,
                            SS_LEFT, 0, IDC_SETTINGS_PERM_NOTE);
    apply_permission_text(p);

    /* ---- the resident listener switch ----------------------------
     *
     * Live as of task 5.11. The listener is a SetWinEventHook, not a timer: it
     * costs nothing while idle and is what makes "start the program yourself
     * and the window still lands in the right place" work. See core/winwatch.h.
     * ------------------------------------------------------------- */
    p->chk_hook  = add_ctrl(p, L"BUTTON", STR_SETTINGS_HOOK_LABEL,
                            WS_TABSTOP | BS_AUTOCHECKBOX, 0, IDC_SETTINGS_HOOK);
    p->note_hook = add_ctrl(p, L"STATIC", STR_SETTINGS_HOOK_NOTE,
                            SS_LEFT, 0, IDC_SETTINGS_HOOK_NOTE);

    p->chk_taskbar = add_ctrl(p, L"BUTTON", STR_SETTINGS_TASKBAR,
                              WS_TABSTOP | BS_AUTOCHECKBOX, 0, IDC_SETTINGS_TASKBAR);

    /* ---- actions -------------------------------------------------- */
    p->btn_guide  = add_ctrl(p, L"BUTTON", STR_SETTINGS_GUIDE,
                             WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_SETTINGS_GUIDE);
    p->btn_apply  = add_ctrl(p, L"BUTTON", STR_SETTINGS_APPLY,
                             WS_TABSTOP | BS_DEFPUSHBUTTON, 0, IDC_SETTINGS_APPLY);
    p->btn_cancel = add_ctrl(p, L"BUTTON", STR_SETTINGS_CANCEL,
                             WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_SETTINGS_CANCEL);

    fill_languages(p);

    /* Start from the built-in defaults so no control is ever empty; main.c
     * loads the stored values right after the window exists. */
    {
        Settings d;
        settings_defaults(&d);
        page_settings_load(p, &d);
    }

    return p;
}

void page_settings_destroy(PageSettings *p)
{
    /* The controls are children of the main window and go away with it. */
    free(p);
}

void page_settings_show(PageSettings *p, BOOL show)
{
    if (p == NULL) return;

    const int cmd = show ? SW_SHOW : SW_HIDE;

    HWND all[] = {
        p->lbl_lang, p->cb_lang, p->note_lang,
        p->grp_close, p->rb_ask, p->rb_quit, p->rb_tray,
        p->lbl_hotkey, p->txt_hotkey, p->note_hotkey,
        p->lbl_perm, p->txt_perm, p->note_perm,
        p->chk_hook, p->note_hook, p->chk_taskbar,
        p->btn_guide, p->btn_apply, p->btn_cancel
    };

    for (int i = 0; i < (int)(sizeof(all) / sizeof(all[0])); ++i) {
        if (all[i] != NULL) ShowWindow(all[i], cmd);
    }
}

void page_settings_layout(PageSettings *p, int x, int top, int width, int bottom)
{
    if (p == NULL || width <= 0) return;

    const int row_h   = px(p, 26);
    const int gap     = px(p, 8);
    const int lbl_w   = px(p, 112);
    const int note_h  = px(p, 18);
    const int group_w = (width < px(p, 560)) ? width : px(p, 560);
    const int group_h = px(p, 104);
    const int radio_h = px(p, 24);
    const int box_h   = px(p, 24);
    const int btn_w   = px(p, 96);
    const int btn_h   = px(p, 30);
    const int body_w  = (width > lbl_w) ? (width - lbl_w) : width;

    /* The button row owns the bottom of the page; rows may not reach into it. */
    const int limit = bottom - btn_h - gap;

    int y = top;

    /* ---- language ------------------------------------------------ */
    place(p->lbl_lang,  x, y, lbl_w, row_h);
    place(p->cb_lang,   x + lbl_w, y, px(p, 260), row_h * 8);
    place(p->note_lang, x + lbl_w, y + row_h + px(p, 2), body_w, note_h);
    y = advance(y, row_h + px(p, 2) + note_h + gap * 2, limit);

    /* ---- close behaviour ----------------------------------------- */
    place(p->grp_close, x, y, group_w, group_h);
    {
        int       ry = y + px(p, 26);
        const int rx = x + px(p, 16);
        const int rw = (group_w > px(p, 32)) ? (group_w - px(p, 32)) : group_w;

        place(p->rb_ask,  rx, ry, rw, radio_h); ry += radio_h;
        place(p->rb_quit, rx, ry, rw, radio_h); ry += radio_h;
        place(p->rb_tray, rx, ry, rw, radio_h);
    }
    y = advance(y, group_h + gap * 2, limit);

    /* ---- exit hotkey (read only) --------------------------------- */
    place(p->lbl_hotkey,  x, y, lbl_w, row_h);
    place(p->txt_hotkey,  x + lbl_w, y, px(p, 200), row_h);
    place(p->note_hotkey, x + lbl_w + px(p, 200) + gap, y,
          (body_w > px(p, 200) + gap) ? (body_w - px(p, 200) - gap) : 0,
          row_h);
    y = advance(y, row_h + gap * 2, limit);

    /* ---- permissions --------------------------------------------- */
    place(p->lbl_perm,  x, y, lbl_w, row_h);
    place(p->txt_perm,  x + lbl_w, y, body_w, row_h);
    place(p->note_perm, x + lbl_w, y + row_h, body_w, note_h * 2);
    y = advance(y, row_h + note_h * 2 + gap * 2, limit);

    /* ---- the resident listener switch ---------------------------- */
    place(p->chk_hook, x, y, width, box_h);
    place(p->note_hook, x + px(p, 20), y + box_h,
          (width > px(p, 20)) ? (width - px(p, 20)) : width, note_h * 2);
    y = advance(y, box_h + note_h * 2 + gap * 2, limit);

    /* ---- the taskbar guide switch -------------------------------- */
    place(p->chk_taskbar, x, y, width, box_h);
    y = advance(y, box_h + gap * 2, limit);

    /* ---- guide entry --------------------------------------------- */
    place(p->btn_guide, x, y, px(p, 180), px(p, 28));

    /* ---- apply / cancel, pinned to the bottom of the page -------- */
    {
        const int by = bottom - btn_h;
        place(p->btn_apply,  x, by, btn_w, btn_h);
        place(p->btn_cancel, x + btn_w + gap, by, btn_w, btn_h);
    }
}

/*
 * Push stored values into the controls. One function so that load and revert
 * cannot drift: forgetting a setting here is how "Cancel" starts leaving a
 * checkbox showing a state the config file does not have.
 */
static void sync_controls(PageSettings *p, const Settings *s)
{
    select_language(p, s->language);
    select_close_action(p, s->close_action);
    set_hotkey_text(p, s->hotkey_label);

    /* The listener switch is a plain checkbox, so it is set and read with the
     * button messages rather than a control-specific accessor. */
    (void)SendMessageW(p->chk_hook, BM_SETCHECK,
                       s->watch_windows ? (WPARAM)BST_CHECKED
                                        : (WPARAM)BST_UNCHECKED, 0);
    (void)SendMessageW(p->chk_taskbar, BM_SETCHECK,
                       s->taskbar_guide ? (WPARAM)BST_CHECKED
                                        : (WPARAM)BST_UNCHECKED, 0);
}

void page_settings_load(PageSettings *p, const Settings *s)
{
    if (p == NULL || s == NULL) return;

    p->loaded      = *s;
    p->have_loaded = TRUE;

    sync_controls(p, s);
}

void page_settings_revert(PageSettings *p)
{
    if (p == NULL || !p->have_loaded) return;

    sync_controls(p, &p->loaded);
}

void page_settings_read(const PageSettings *p, Settings *out)
{
    if (p == NULL || out == NULL) return;

    /* Everything the user can actually change. The hotkey field is read-only,
     * and `out` arrives holding a copy of the current settings, so overwriting
     * the label here would blank it. */
    out->language     = read_language(p);
    out->close_action = read_close_action(p);
    out->watch_windows =
        (SendMessageW(p->chk_hook, BM_GETCHECK, 0, 0) == BST_CHECKED) ? TRUE : FALSE;
    out->taskbar_guide =
        (SendMessageW(p->chk_taskbar, BM_GETCHECK, 0, 0) == BST_CHECKED) ? TRUE : FALSE;
}

/* ------------------------------------------------------------------ */
/* The settings value itself                                           */
/* ------------------------------------------------------------------ */

void settings_defaults(Settings *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->language      = UI_LANG_AUTO;
    out->close_action  = CLOSE_ACTION_ASK;
    /* On by default: this is the feature the program exists for. */
    out->watch_windows = TRUE;
    /* On by default: most machines show the taskbar. */
    out->taskbar_guide = TRUE;
    strncpy_s(out->hotkey_label, sizeof(out->hotkey_label), "?", _TRUNCATE);
}

void settings_set_hotkey_label(Settings *s, const char *label)
{
    if (s == NULL) return;
    strncpy_s(s->hotkey_label, sizeof(s->hotkey_label),
              (label != NULL && label[0] != '\0') ? label : "?", _TRUNCATE);
}

/*
 * Keys live under [settings]. Defaults are chosen so that a config file
 * written by an older build - or none at all - still loads sensibly: an
 * unknown language code resolves to "follow the system", an out-of-range close
 * action falls back to asking, and a file written before the listener existed
 * (no watch_windows key) reads as ON, because that is what the program is for
 * and a silent upgrade to "off" would look like a regression.
 */
void settings_from_store(Settings *out, const void *doc)
{
    if (out == NULL || doc == NULL) return;

    const StoreDocument *d = (const StoreDocument *)doc;

    out->language = i18n_from_code(store_get(d, "settings", "language", "auto"));

    const int ca = store_get_int(d, "settings", "close_action", CLOSE_ACTION_ASK);
    if (ca < CLOSE_ACTION_ASK || ca > CLOSE_ACTION_TRAY) {
        out->close_action = CLOSE_ACTION_ASK;
    } else {
        out->close_action = (CloseAction)ca;
    }

    out->watch_windows =
        (store_get_int(d, "settings", "watch_windows", 1) != 0) ? TRUE : FALSE;

    out->taskbar_guide =
        (store_get_int(d, "settings", "taskbar_guide", 1) != 0) ? TRUE : FALSE;
}

void settings_to_store(const Settings *s, void *doc)
{
    if (s == NULL || doc == NULL) return;

    StoreDocument *d = (StoreDocument *)doc;

    store_set(d, "settings", "language", i18n_code(s->language));

    char num[16];
    _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", (int)s->close_action);
    store_set(d, "settings", "close_action", num);

    store_set(d, "settings", "watch_windows", s->watch_windows ? "1" : "0");
    store_set(d, "settings", "taskbar_guide", s->taskbar_guide ? "1" : "0");
}
