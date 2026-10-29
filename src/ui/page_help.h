/*
 * page_help.h - the user-guide page.
 *
 * WHAT IT IS FOR
 *
 *   Two things the program deliberately does NOT do for the user still have to
 *   be explained where the user is: starting with Windows
 *   (no shortcut is written, no registry key is touched) and
 *   dealing with elevated windows (UIPI - the program reports PLACE_DENIED and
 *   does not try to get around it).
 *
 *   The content mirrors the usage guide, which is the source: if the
 *   page and the guide ever disagree, the user has no way to tell which one is
 *   lying, so they are edited together.
 *
 * WHAT IT IS NOT
 *
 *   Not a developer document. No stage numbers, no module names, no file
 *   paths inside the repository - the reader is someone who wants their
 *   notepad to open on the left half of the screen.
 */

#ifndef WAC_PAGE_HELP_H
#define WAC_PAGE_HELP_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ID ALLOCATION - children of the main window (see res/resource.h for the
 * rule). 1080-1099 belongs to the settings page; this page owns 1100-1119.
 *
 * 1101 and 1113 are RETIRED: they were the "why this program does not do it
 * for you" paragraph and the command-line footnote, both dropped when the page
 * text was rewritten on 2026-09-22. Retired numbers stay retired - see the
 * note in res/resource.h about why reusing one is how a stale reference turns
 * into a collision nobody can see.
 */
#define IDC_HELP_AUTOSTART_HEAD    1100
#define IDC_HELP_AUTOSTART_STEPS   1102
#define IDC_HELP_AUTOSTART_UNDO    1103
#define IDC_HELP_AUTOSTART_MANUAL  1104
#define IDC_HELP_OPEN_STARTUP      1105
#define IDC_HELP_PERM_HEAD         1106
#define IDC_HELP_PERM_BODY         1107
#define IDC_HELP_PERM_HOW          1108
#define IDC_HELP_PERM_WARN         1109
#define IDC_HELP_PERM_STATUS       1110
#define IDC_HELP_CLI_HEAD          1111
#define IDC_HELP_CLI_BODY          1112
#define IDC_HELP_CONFIG_NOTE       1114
#define IDC_HELP_ABOUT             1115

typedef struct PageHelp PageHelp;

/*
 * Create the page's controls, hidden.
 *
 * `section_font` may be NULL, in which case the headings keep the body face.
 * `elevated` decides the permission line - it is passed in so that this page
 * and the settings page cannot describe the same process differently.
 */
PageHelp *page_help_create(HWND parent, HINSTANCE hinst, HFONT font,
                           HFONT section_font, int dpi, BOOL elevated);

void page_help_destroy(PageHelp *p);

/* Visibility: called from mainwindow's apply_page() and nowhere else. */
void page_help_show(PageHelp *p, BOOL show);

/* Place the controls; `top` is the first free pixel under the page title row. */
void page_help_layout(PageHelp *p, int x, int top, int width, int bottom);

#ifdef __cplusplus
}
#endif

#endif /* WAC_PAGE_HELP_H */
