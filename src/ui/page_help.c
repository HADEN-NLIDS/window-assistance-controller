/*
 * page_help.c - see page_help.h.
 *
 * Everything visible here is text from src/ui/i18n.c plus one button. The page
 * holds no state of its own: the permission line is fixed when it is created,
 * because the elevation of a process cannot change while it runs.
 */

#include "page_help.h"

#include "i18n.h"

#include <stdlib.h>   /* calloc / free */

struct PageHelp {
    HWND      parent;
    HINSTANCE hinst;
    int       dpi;

    HWND head_autostart;
    HWND steps;
    HWND undo;
    HWND manual;
    HWND btn_startup;

    HWND head_perm;
    HWND perm_body;
    HWND perm_how;
    HWND perm_warn;
    HWND perm_status;

    HWND head_cli;
    HWND cli;
    HWND config_note;
    HWND btn_about;

    /* The body face, and the monospaced face the command samples use. The
     * latter is owned by this page when it could be created at all. */
    HFONT font;
    HFONT mono;
};

static int px(const PageHelp *p, int logical)
{
    return MulDiv(logical, (p->dpi > 0) ? p->dpi : 96, 96);
}

static HWND add_static(PageHelp *p, StrId text, HFONT font, int id)
{
    HWND h = CreateWindowExW(0, L"STATIC", i18n_text(text),
                             WS_CHILD | SS_LEFT, 0, 0, 0, 0,
                             p->parent, (HMENU)(INT_PTR)id, p->hinst, NULL);
    if (h != NULL && font != NULL) {
        SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    }
    return h;
}

/*
 * A fixed-pitch face for the command samples.
 *
 * The lines are aligned with spaces so that a reader can scan the verbs; a
 * proportional face turns that into ragged text. Consolas ships with Windows;
 * if it cannot be created the body font is used and the page still reads
 * correctly, just less tidily.
 */
static HFONT make_mono_font(int dpi)
{
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));

    lf.lfHeight  = -MulDiv(9, dpi, 96);
    lf.lfWeight  = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    wcsncpy_s(lf.lfFaceName, LF_FACESIZE, L"Consolas", _TRUNCATE);

    return CreateFontIndirectW(&lf);
}

PageHelp *page_help_create(HWND parent, HINSTANCE hinst, HFONT font,
                           HFONT section_font, int dpi, BOOL elevated)
{
    PageHelp *p = (PageHelp *)calloc(1, sizeof(PageHelp));
    if (p == NULL) return NULL;

    p->parent = parent;
    p->hinst  = hinst;
    p->dpi    = (dpi > 0) ? dpi : 96;
    p->font   = font;

    if (section_font == NULL) section_font = font;
    p->mono = make_mono_font(p->dpi);
    if (p->mono == NULL) p->mono = font;

    /* ---- starting with Windows ----------------------------------- */
    p->head_autostart = add_static(p, STR_HELP_AUTOSTART_HEAD, section_font,
                                   IDC_HELP_AUTOSTART_HEAD);
    p->steps = add_static(p, STR_HELP_AUTOSTART_STEPS, font, IDC_HELP_AUTOSTART_STEPS);
    p->undo  = add_static(p, STR_HELP_AUTOSTART_UNDO,  font, IDC_HELP_AUTOSTART_UNDO);
    p->manual = add_static(p, STR_HELP_AUTOSTART_MANUAL, font,
                           IDC_HELP_AUTOSTART_MANUAL);

    p->btn_startup = CreateWindowExW(0, L"BUTTON", i18n_text(STR_HELP_OPEN_STARTUP),
                                     WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                     0, 0, 0, 0, parent,
                                     (HMENU)(INT_PTR)IDC_HELP_OPEN_STARTUP,
                                     hinst, NULL);
    if (p->btn_startup != NULL && font != NULL) {
        SendMessageW(p->btn_startup, WM_SETFONT, (WPARAM)font, TRUE);
    }

    /* ---- permissions --------------------------------------------- */
    p->head_perm  = add_static(p, STR_HELP_PERM_HEAD, section_font,
                               IDC_HELP_PERM_HEAD);
    p->perm_body  = add_static(p, STR_HELP_PERM_BODY, font, IDC_HELP_PERM_BODY);
    p->perm_how   = add_static(p, STR_HELP_PERM_HOW,  font, IDC_HELP_PERM_HOW);
    p->perm_warn  = add_static(p, STR_HELP_PERM_WARN, font, IDC_HELP_PERM_WARN);
    p->perm_status = add_static(p,
                                elevated ? STR_HELP_PERM_STATUS_ADMIN
                                         : STR_HELP_PERM_STATUS_NORMAL,
                                font, IDC_HELP_PERM_STATUS);

    /* ---- command line -------------------------------------------- */
    p->head_cli   = add_static(p, STR_HELP_CLI_HEAD, section_font, IDC_HELP_CLI_HEAD);
    p->cli        = add_static(p, STR_HELP_CLI_BODY, p->mono, IDC_HELP_CLI_BODY);
    p->config_note = add_static(p, STR_HELP_CONFIG_NOTE, font,
                                IDC_HELP_CONFIG_NOTE);

    /* About ends up here rather than on the settings page: it answers "what is
     * this program", which is a help question, and keeping it beside the guide
     * leaves the settings page with only things that are settings. */
    p->btn_about = CreateWindowExW(0, L"BUTTON", i18n_text(STR_MSG_ABOUT_TITLE),
                                   WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                   0, 0, 0, 0, parent,
                                   (HMENU)(INT_PTR)IDC_HELP_ABOUT,
                                   hinst, NULL);
    if (p->btn_about != NULL && font != NULL) {
        SendMessageW(p->btn_about, WM_SETFONT, (WPARAM)font, TRUE);
    }

    return p;
}

void page_help_destroy(PageHelp *p)
{
    if (p == NULL) return;

    /* Only delete the monospaced face when this page created it: a fallback to
     * the caller's font must not destroy the caller's object. */
    if (p->mono != NULL && p->mono != p->font) {
        DeleteObject(p->mono);
    }
    free(p);
}

void page_help_show(PageHelp *p, BOOL show)
{
    if (p == NULL) return;

    const int cmd = show ? SW_SHOW : SW_HIDE;

    HWND all[] = {
        p->head_autostart, p->steps, p->undo, p->manual, p->btn_startup,
        p->head_perm, p->perm_body, p->perm_how, p->perm_warn, p->perm_status,
        p->head_cli, p->cli, p->config_note, p->btn_about
    };

    for (int i = 0; i < (int)(sizeof(all) / sizeof(all[0])); ++i) {
        if (all[i] != NULL) ShowWindow(all[i], cmd);
    }
}

void page_help_layout(PageHelp *p, int x, int top, int width, int bottom)
{
    if (p == NULL || width <= 0) return;

    const int head_h  = px(p, 24);
    const int line_h  = px(p, 18);
    const int gap     = px(p, 8);
    const int btn_h   = px(p, 28);

    /*
     * Row counts follow the text exactly, including the blank lines the steps
     * use as paragraph separators: a STATIC does not grow, so a line that is
     * not budgeted for here is a line that gets clipped.
     */
    const int steps_lines = 9;   /* 5 steps + 4 blank lines between them */
    const int undo_lines  = 3;   /* "to undo it:" + blank + the line itself */

    int y = top;

    /* ---- starting with Windows ----------------------------------- */
    MoveWindow(p->head_autostart, x, y, width, head_h, TRUE);
    y += head_h;
    MoveWindow(p->steps, x, y, width, line_h * steps_lines, TRUE);
    y += line_h * steps_lines + px(p, 4);

    MoveWindow(p->undo,   x, y, width, line_h * undo_lines, TRUE);
    y += line_h * undo_lines;
    MoveWindow(p->manual, x, y, width, line_h, TRUE);
    y += line_h + gap * 2;

    MoveWindow(p->btn_startup, x, y, px(p, 180), btn_h, TRUE);
    y += btn_h + gap * 2;

    /* ---- permissions --------------------------------------------- */
    MoveWindow(p->head_perm, x, y, width, head_h, TRUE);
    y += head_h;
    MoveWindow(p->perm_body, x, y, width, line_h * 2, TRUE);
    y += line_h * 2 + px(p, 4);
    MoveWindow(p->perm_how,  x, y, width, line_h * 2, TRUE);
    y += line_h * 2 + px(p, 4);
    MoveWindow(p->perm_warn, x, y, width, line_h * 2, TRUE);
    y += line_h * 2 + px(p, 4);
    MoveWindow(p->perm_status, x, y, width, line_h, TRUE);
    y += line_h + gap * 2;

    /* ---- command line -------------------------------------------- */
    MoveWindow(p->head_cli, x, y, width, head_h, TRUE);
    y += head_h;
    MoveWindow(p->cli, x, y, width, line_h * 5, TRUE);
    y += line_h * 5 + px(p, 4);
    MoveWindow(p->config_note, x, y, width, line_h * 2, TRUE);
    y += line_h * 2 + px(p, 6);

    MoveWindow(p->btn_about, x, y, px(p, 120), btn_h, TRUE);

    /*
     * `bottom` is unused: the page is a fixed block of text that ends where it
     * ends, and stretching the last line to the bottom of the window would
     * undo the point of the shared line height. Kept in the signature so both
     * pages are laid out the same way by the caller.
     */
    (void)bottom;
}
