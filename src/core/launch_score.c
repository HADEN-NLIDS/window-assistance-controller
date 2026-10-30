/*
 * launch_score.c - see launch_score.h.
 */

#include "launch_score.h"

LONGLONG launch_score_candidate(const WindowInfo *w)
{
    LONG cw, ch;
    LONGLONG area;

    if (w == NULL) return 0;
    if (!w->is_visible) return 0;
    if (w->is_cloaked)  return 0;
    if (w->ex_style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return 0;

    cw = w->rect.right - w->rect.left;
    ch = w->rect.bottom - w->rect.top;

    /* Below this it is a helper or a splash frame, not a main window. */
    if (cw < 200 || ch < 150) return 0;

    area = (LONGLONG)cw * (LONGLONG)ch;
    if (w->is_iconic || w->is_zoomed) area /= 4;
    return area;
}
