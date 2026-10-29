/*
 * close_confirm.h - "what should the X button do?" prompt.
 *
 * Replaces a MB_YESNOCANCEL message box. The message box asked "quit, or
 * keep it running?" and offered Yes/No - but on that question Yes means KEEP
 * RUNNING and No means QUIT, the reverse of the natural reading, so the
 * obvious-looking button produced the other outcome. Radio buttons that name
 * their own action remove the mapping step, and match the layout the settings
 * page already uses for the same choice.
 *
 * The dialog does not write anything. It reports the choice; main.c decides
 * whether to persist it. That keeps "ask the user" and "store a preference"
 * as separate decisions.
 */

#ifndef WAC_CLOSE_CONFIRM_H
#define WAC_CLOSE_CONFIRM_H

#include <windows.h>

#include "page_settings.h"   /* CloseAction */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    CloseAction action;    /* what closing should do */
    BOOL        remember;  /* "Do not ask again" was ticked */
} CloseConfirmResult;

/*
 * Show the prompt. Returns FALSE when the user cancelled, in which case
 * nothing should change.
 *
 * Cancelling includes closing with the X or Escape: a dismissal must never
 * end the program, so the safe outcome is the only one reachable by accident.
 *
 * `current` preselects the radio button matching the existing preference, and
 * `was_asked_before` decides whether the "do not ask again" box starts
 * ticked - a user who has answered this question before sees their previous
 * answer already in place.
 */
BOOL close_confirm_show(HWND owner, CloseAction current,
                        BOOL was_asked_before, CloseConfirmResult *out);

#ifdef __cplusplus
}
#endif

#endif /* WAC_CLOSE_CONFIRM_H */
