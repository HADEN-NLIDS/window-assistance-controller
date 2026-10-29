/*
 * rename_dlg.h - ask the user for a name.
 *
 * WHY A DIALOG AND NOT AN EMBEDDED PANEL
 *
 *   The window-preset page's editor is a panel because it has a preview and
 *   four fields that update as you type. This asks for ONE line of text, and
 *   a panel for that would take the whole page away from the list the user is
 *   looking at - which is exactly what they need to see while choosing a name.
 *
 *   The templates live in res/wac.rc with EMPTY captions; every string is
 *   filled from src/ui/i18n.c at run time, so one template serves both
 *   languages (see the note at the top of wac.rc).
 */

#ifndef WAC_RENAME_DLG_H
#define WAC_RENAME_DLG_H

#include <windows.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Ask for a new name, starting from `initial`.
 *
 * Returns TRUE and writes the new name into `out` when the user accepted,
 * FALSE on Cancel or close - in which case `out` is untouched, so a caller
 * cannot mistake a dismissal for an empty name.
 *
 * An empty or whitespace-only result is refused by the dialog itself (the OK
 * button stays disabled): a workspace with no name would show up as a blank
 * row, which reads as a rendering fault rather than as a setting.
 */
BOOL rename_dlg_prompt(HWND owner, const wchar_t *initial, wchar_t *out, size_t cch);

#ifdef __cplusplus
}
#endif

#endif /* WAC_RENAME_DLG_H */
