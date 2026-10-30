/*
 * launch_score.h - how a candidate main window is scored.
 *
 * The splash-screen filter lives in the score: a window that is not visible,
 * cloaked or a tool window cannot be the main window at all; one that is big
 * enough wins by area; and a minimized or maximized window is quartered
 * because an application that has restored its own state is still starting up.
 *
 * Split out of launch.c so a test can pin the rules down without a live
 * window (tests/t31_splash.c).
 */

#ifndef WAC_LAUNCH_SCORE_H
#define WAC_LAUNCH_SCORE_H

#include <windows.h>

#include "win_enum.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 0 means "cannot be the main window". Larger is a better candidate. */
LONGLONG launch_score_candidate(const WindowInfo *w);

#ifdef __cplusplus
}
#endif

#endif /* WAC_LAUNCH_SCORE_H */
