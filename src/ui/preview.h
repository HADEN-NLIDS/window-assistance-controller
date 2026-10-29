/*
 * preview.h - a small self-drawn picture of where a window will land.
 *
 * WHY IT EXISTS
 *
 *   Hand-typing a zone is four numbers with no feedback: 0.469 x 0.563 looks
 *   exactly like 0.469 x 0.356 until the window appears in the wrong shape.
 *   The preview turns the numbers back into the rectangle they describe, which
 *   is the whole reason the coordinate fields were kept at all.
 *
 * WHAT IT DRAWS
 *
 *   The monitor outline (at its true aspect ratio, so a portrait panel looks
 *   portrait), the taskbar as a strip along the working edge, and the target
 *   rectangle inside it.
 *
 * GEOMETRY IS SEPARATE AND PURE
 *
 *   preview_monitor_rect() and preview_zone_rect() do the arithmetic and touch
 *   no window; they are tested in tests/t12_uiformat.c. The window procedure
 *   only calls them and paints the result.
 */

#ifndef WAC_PREVIEW_H
#define WAC_PREVIEW_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create the control. It is an ordinary child window; the caller owns its
 * position and lifetime like any other control. */
HWND preview_create(HWND parent, HINSTANCE hinst, int id);

/*
 * Show a normalized zone. Values are clamped to 0..1 for DRAWING purposes
 * only - the preview never edits what the caller stores.
 */
void preview_set_zone(HWND preview, double x, double y, double w, double h);

/* Tell the control which monitor shape to draw. Defaults to 16:10. */
void preview_set_monitor(HWND preview, int mon_w, int mon_h);

/*
 * Draw the window's REAL size on top of the preset's zone.
 *
 * A window that refuses to be resized keeps its own width and height, so the
 * preset's fractions then describe only WHERE it goes - not how big it will
 * be. Drawing both rectangles is what makes "I typed 30% and the window covers
 * 48%" visible before anything is applied (2026-10-10, task B).
 *
 * w and h are fractions of the same monitor the zone is drawn against, so the
 * caller converts pixels once, in one place. Either one <= 0 turns the second
 * rectangle off - the normal state for a window that CAN be resized, and the
 * reason this is a separate entry point rather than another parameter of
 * preview_set_zone(): the stored zone keeps meaning exactly one thing.
 */
void preview_set_real_size(HWND preview, double w, double h);

/*
 * The monitor outline inside a control of the given size, letterboxed to keep
 * the monitor's aspect ratio and inset by `pad` pixels.
 *
 * Pure: no window, no globals. Returns FALSE when there is no room to draw.
 */
BOOL preview_monitor_rect(int ctrl_w, int ctrl_h, int mon_w, int mon_h,
                          int pad, RECT *out);

/*
 * Map a normalized zone onto a monitor rectangle. Pure.
 *
 * The result is clamped to the monitor rectangle, so a zone that hangs off the
 * edge still draws something recognisable instead of escaping the control.
 */
BOOL preview_zone_rect(const RECT *monitor, double x, double y, double w, double h,
                       RECT *out);

#ifdef __cplusplus
}
#endif

#endif /* WAC_PREVIEW_H */
