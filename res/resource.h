/*
 * resource.h - resource and control identifiers.
 *
 * Shared between the .rc file and the C sources. Kept ASCII-only and free of
 * includes: rc.exe's preprocessor is not the same as cl.exe's, and a header
 * that both can use has to stay simple.
 */

#ifndef WAC_RESOURCE_H
#define WAC_RESOURCE_H

/*
 * ID ALLOCATION - read this before adding a control.
 *
 *   4000-4009   dialog templates (IDD_*)
 *   4050-4069   close-confirm dialog controls (IDC_CLOSE_*)
 *
 * The main window allocates its own ids inside src/ui/mainwindow.c
 * (1001-1074 for controls, 2001-2020 for menu commands). They deliberately do
 * not appear here: nothing outside that file needs them.
 *
 * The pages that own their controls allocate a segment each, next to the code
 * that creates them: src/ui/page_settings.h (1080-1099) and
 * src/ui/page_help.h (1100-1119). Those controls are children of the MAIN
 * window rather than of a dialog template, and rc.exe never sees their ids.
 *
 * 4010-4049 held the settings DIALOG's controls until stage 4.5 turned
 * settings into a page. The segment is free again and stays free: reusing a
 * retired number for something unrelated is how a stale reference somewhere
 * becomes a collision nobody can see.
 *
 * WHY THE SEGMENTS: a dialog template id and a control id live in the SAME
 * integer space, and neither rc.exe nor /W4 reports a collision.
 * IDD_CLOSE_CONFIRM and IDC_SET_HOTKEY_NOTE were both 4010 and nothing
 * complained - it only worked because GetDlgItem searches the children of one
 * dialog. See TRAPS.md #25.
 */

/* ---- dialogs ------------------------------------------------------ */
#define IDD_CLOSE_CONFIRM       4001
#define IDD_RENAME_WS           4002

/* ---- close-confirm dialog controls -------------------------------- */
#define IDC_CLOSE_ICON          4050
#define IDC_CLOSE_TEXT          4051
#define IDC_CLOSE_OPT_KEEP      4052   /* radio: keep running */
#define IDC_CLOSE_OPT_QUIT      4053   /* radio: quit completely */
#define IDC_CLOSE_REMEMBER      4054   /* checkbox: do not ask again */

/* ---- rename dialog controls --------------------------------------- */
#define IDC_RENAME_LABEL        4070
#define IDC_RENAME_EDIT         4071

#endif /* WAC_RESOURCE_H */
