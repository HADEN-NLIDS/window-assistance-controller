/*
 * page_settings.h - the settings page, and the settings themselves.
 *
 * WHY THE PAGE AND THE DATA LIVE TOGETHER
 *
 *   These used to be split: settings_dlg.c owned the values and showed them in
 *   a modal dialog. Stage 4.5 page-ifies settings, so the dialog frame goes
 *   away - and what is left is
 *   "the values the user can change" plus "the controls that change them".
 *   Keeping them in one file means adding a setting touches one header.
 *
 *   Persistence stays in main.c: this file turns a Settings into controls and
 *   back, and knows nothing about when a file is written. That is what keeps
 *   Cancel meaningful - nothing here can save by accident.
 *
 * WHAT THE PAGE DELIBERATELY DOES NOT DO
 *
 *   Apply the language live. The note under the box says a restart is needed,
 *   and that stays true: rebuilding every caption of a running UI is a much
 *   larger change than it looks, and it is not what the user asked for.
 */

#ifndef WAC_PAGE_SETTINGS_H
#define WAC_PAGE_SETTINGS_H

#include <windows.h>

#include "i18n.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* The settings                                                        */
/* ------------------------------------------------------------------ */

/* What the user chose for the X button. Stored in the config file, so the
 * numbers must not be renumbered. */
typedef enum {
    CLOSE_ACTION_ASK  = 0,
    CLOSE_ACTION_QUIT = 1,
    CLOSE_ACTION_TRAY = 2
} CloseAction;

typedef struct {
    WacLanguage language;          /* UI_LANG_AUTO / ENGLISH / CHINESE_SIMP */
    CloseAction close_action;
    char        hotkey_label[64];  /* display only; not editable yet */

    /*
     * Put a window back as it appears, even when this program did not start
     * it (task 5.11: the resident SetWinEventHook listener).
     *
     * On by default, because it is the feature the program exists for: the
     * user double-clicks their own icon and expects the window where the
     * preset says. Stored as 0/1 in the config file; an older file that has no
     * such key reads as 1.
     */
    BOOL watch_windows;

    /*
     * Draw the taskbar guide line in edit mode and let the bottom edge of a
     * box snap to it.
     *
     * On by default: most machines show the taskbar, and a layout that ignores
     * it puts the window's bottom edge under it. Stored as 0/1; an older file
     * with no such key reads as 1.
     */
    BOOL taskbar_guide;
} Settings;

/* Fill *out with the built-in defaults. */
void settings_defaults(Settings *out);

/*
 * Record the hotkey combination that is actually registered.
 *
 * A helper rather than a plain strncpy at the call site because the label
 * otherwise lives in two places - and updating only one of them is exactly how
 * the old dialog came to show "?" while the status bar showed the real key.
 */
void settings_set_hotkey_label(Settings *s, const char *label);

/*
 * The keys are read and written by main.c through the store, but the
 * translation between Settings and the file lives here so that adding a
 * setting touches one file. `doc` is a StoreDocument*.
 */
void settings_from_store(Settings *out, const void *doc);
void settings_to_store(const Settings *s, void *doc);

/* ------------------------------------------------------------------ */
/* Control ids                                                         */
/* ------------------------------------------------------------------ */

/*
 * ID ALLOCATION - these are children of the MAIN window, not of a dialog
 * template, which is why they live here and not in res/resource.h (rc.exe
 * never sees them). The rule from res/resource.h still applies: one segment
 * per page, never two meanings for one number.
 *
 *   1001-1074   main window: navigation, overview, presets, editor
 *   1080-1099   settings page
 *   1100-1119   user-guide page
 */
#define IDC_SETTINGS_LANG_LABEL    1080
#define IDC_SETTINGS_LANG          1081
#define IDC_SETTINGS_LANG_NOTE     1082
#define IDC_SETTINGS_CLOSE_GROUP   1083
#define IDC_SETTINGS_CLOSE_ASK     1084
#define IDC_SETTINGS_CLOSE_QUIT    1085
#define IDC_SETTINGS_CLOSE_TRAY    1086
#define IDC_SETTINGS_HOTKEY_LABEL  1087
#define IDC_SETTINGS_HOTKEY        1088
#define IDC_SETTINGS_HOTKEY_NOTE   1089
#define IDC_SETTINGS_PERM_LABEL    1090
#define IDC_SETTINGS_PERM          1091
#define IDC_SETTINGS_PERM_NOTE     1092
#define IDC_SETTINGS_HOOK          1093
#define IDC_SETTINGS_HOOK_NOTE     1094
#define IDC_SETTINGS_GUIDE         1095
#define IDC_SETTINGS_TASKBAR       1098
#define IDC_SETTINGS_APPLY         1096
#define IDC_SETTINGS_CANCEL        1097

/* ------------------------------------------------------------------ */
/* The page                                                            */
/* ------------------------------------------------------------------ */

typedef struct PageSettings PageSettings;

/*
 * Create the page's controls, hidden.
 *
 * `elevated` is the process's own elevation, passed in rather than looked up
 * here so that the status bar and this page cannot end up disagreeing about
 * the same process. It cannot change while the program runs.
 */
PageSettings *page_settings_create(HWND parent, HINSTANCE hinst, HFONT font,
                                   int dpi, BOOL elevated);

/* Frees the page's own memory. The controls belong to the parent window and
 * are destroyed with it. */
void page_settings_destroy(PageSettings *p);

/*
 * Show or hide the page.
 *
 * Called from mainwindow's apply_page() and from nowhere else: that function
 * is the single place that decides which controls are visible, and a second
 * decision point is what once drew the preset editor on top of the overview.
 */
void page_settings_show(PageSettings *p, BOOL show);

/*
 * Place the controls.
 *
 * `top` is the first free pixel below the navigation strip and the page title
 * row; `bottom` is the bottom of the content area, which is where the
 * [Apply] / [Cancel] row is pinned.
 */
void page_settings_layout(PageSettings *p, int x, int top, int width, int bottom);

/*
 * Fill the controls from a Settings value.
 *
 * Also remembers what it was given, so that leaving the page without pressing
 * [Apply] can put it back - see page_settings_revert().
 */
void page_settings_load(PageSettings *p, const Settings *s);

/*
 * Put the controls back to the last loaded values, discarding anything typed
 * since. This is what leaving the page does, so that "walked away" and
 * "pressed Cancel" mean the same thing.
 */
void page_settings_revert(PageSettings *p);

/*
 * Read back the language and close behaviour the user picked. Nothing else is
 * touched; the hotkey field is read-only, and the listener switch is disabled
 * until stage 5 provides the listener.
 */
void page_settings_read(const PageSettings *p, Settings *out);

#ifdef __cplusplus
}
#endif

#endif /* WAC_PAGE_SETTINGS_H */
