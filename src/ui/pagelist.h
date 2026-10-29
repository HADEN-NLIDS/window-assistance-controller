/*
 * pagelist.h - the behaviour every report-view list in this program shares.
 *
 * WHY THIS EXISTS
 *
 *   The overview has two lists, the preset page and the create-preset picker
 *   have one each, and the workspace page has another: five lists that all need
 *   the same things - columns sized to the control, a row filled cell by cell,
 *   "nothing here yet" text, and a selection remembered by IDENTITY rather than
 *   by row number (every list is rebuilt wholesale on each refresh, so a row
 *   index is stale the moment anything changes).
 *
 *   That was ~350 lines inside mainwindow.c, which is the one file in this
 *   project that had outgrown its job. Moving it here also makes the arithmetic
 *   testable: pagelist_column_widths() touches no window, so the rules about
 *   rounding and remainders can be checked without a screen.
 *
 * NO i18n DEPENDENCY
 *
 *   Column headings arrive as finished strings. A list widget has no business
 *   knowing about string ids or languages.
 */

#ifndef WAC_PAGELIST_H
#define WAC_PAGELIST_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const wchar_t *text;    /* already translated */
    int            weight;  /* relative width, not pixels */
} PageColumn;

/*
 * Create a report-view list with the given columns.
 *
 * `weight` is a PROPORTION: the layout pass redistributes the control's actual
 * width in those ratios. Fixed pixel widths only look right at the one window
 * size they were chosen for.
 */
HWND pagelist_create(HWND parent, HINSTANCE hinst, int id,
                     const PageColumn *cols, int ncols);

/*
 * Divide `avail` pixels among `ncols` columns in proportion to `weights`.
 *
 * Pure - no window, no globals - so the rounding rules are testable. The last
 * column takes the remainder, which is what makes the widths sum to exactly
 * `avail`: distributing all of them by rounding leaves a one-pixel gap that
 * grows into a horizontal scrollbar.
 *
 * Returns FALSE for degenerate input (no room, no columns, a non-positive
 * weight), leaving `out` untouched.
 */
BOOL pagelist_column_widths(const int *weights, int ncols, int avail, int *out);

/* Apply pagelist_column_widths() to a live list. */
void pagelist_fit_columns(HWND list, const PageColumn *cols, int ncols, int client_w);

/*
 * Same, for a caller that keeps its column definitions in its own shape.
 *
 * The widths themselves are the part worth sharing and testing; how a caller
 * stores its column metadata (a translated heading, a string id, whatever) is
 * its own business.
 */
void pagelist_fit_weights(HWND list, const int *weights, int ncols, int client_w);

/* Select row 0 if the list has one. */
void pagelist_select_first(HWND list);

/* The selected row, or -1. */
int pagelist_selected(HWND list);

/* Select one row and scroll it into view. */
void pagelist_select_row(HWND list, int row);

/*
 * Batch update. Call begin, insert rows, then end: the list stops redrawing in
 * between, which is the difference between a flicker and a rebuild.
 */
void pagelist_begin(HWND list);
void pagelist_end(HWND list);

/*
 * Replace the list's contents with a single line of placeholder text.
 *
 * A report-view list cannot show a message any other way: an empty list is just
 * an empty box, which reads as "broken" rather than "nothing yet".
 */
void pagelist_show_empty(HWND list, const wchar_t *text);

/*
 * Insert one row. `cells` holds ncols strings; a NULL or empty cell is left
 * blank. Returns the new row index, or -1.
 */
int pagelist_add_row(HWND list, const wchar_t *const *cells, int ncols);

#ifdef __cplusplus
}
#endif

#endif /* WAC_PAGELIST_H */
