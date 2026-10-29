/*
 * sidebar.h - the slide-out application list on the right edge of the edit
 * overlay: what a point on it means, how far out it is, and which rows the
 * window can show.
 *
 * This is stage 8 task 2b. The DATA it lists
 * already exists (core/appindex.h).
 *
 * WHY THIS IS NOT A WINDOW, AND NOT PART OF overlay.c
 *
 *   The overlay is a WS_EX_LAYERED window, so a child window inside it is not
 *   drawn at all - the constraint that made the toolbar geometry instead of a
 *   sub-window. The sidebar is drawn by overlay.c's paint path like everything
 *   else on the overlay.
 *
 *   What it does not have to be is overlay.c's business. A thousand-row list
 *   that slides in and out is arithmetic: where is the edge strip, how far has
 *   the body come out, which row is under the pointer, and which rows are on
 *   screen at all. Written inside overlay.c those answers cannot be tested;
 *   written here they are pure functions, and the awkward cases - a 1000-item
 *   list in a 20-row window, a pointer one pixel outside the strip, a drag
 *   that leaves the panel while the button is still down - are assertions
 *   instead of things found on someone else's monitor.
 *
 *   So this module owns the STATE and the ARITHMETIC. It does not draw, it
 *   does not know what an AppIndex is, and it calls no Win32 function beyond
 *   the RECT/POINT types and MulDiv - which is what lets a test drive it with
 *   rectangles and points and no window at all.
 *
 * THE HIT-TEST ORDER IS NOT HERE
 *
 *   popup.h writes the overlay's order down in one place, and the sidebar is
 *   line 4 of it:
 *
 *     1. an open pop-up panel
 *     2. toolbar buttons
 *     3. the name box
 *     4. the sidebar's edge strip and body        <- this module answers this
 *     5. the canvas
 *
 *   sidebar_hit() answers line 4 only, and only when the caller asks it at
 *   that point in the sequence. It deliberately says nothing about the other
 *   four.
 *
 * WHAT "AREA" MEANS
 *
 *   Every geometry function takes `area`: the rectangle the sidebar may use -
 *   the overlay's client area MINUS the toolbar strip, so the strip's buttons
 *   are never covered (trap 33: before painting a panel, ask whose rectangle
 *   it is). The caller computes it, which is what keeps this module from
 *   having to know that a toolbar exists.
 */

#ifndef WAC_SIDEBAR_H
#define WAC_SIDEBAR_H

#include <windows.h>

#include "../core/relcoord.h"   /* RelRect - the type only; nothing is linked */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Metrics - the 96-dpi bases and their scaling                        */
/* ------------------------------------------------------------------ */

/*
 * The edge strip: how close to the right edge the pointer has to come for the
 * panel to slide out. 8 px at 96 dpi, as the acceptance list in section 8.6
 * specifies.
 */
#define SIDEBAR_EDGE_W      8

/*
 * The handle: a tab sticking out of the right edge, vertically centred.
 *
 * WHY IT EXISTS (user report, 2026-10-10): "the sidebar is not obvious enough -
 * on first use you cannot find it". The 8-px strip is a hot zone and nothing
 * more; three small dots on it are easy to miss against a full-screen wash, and
 * a feature nobody finds is a feature that is not there.
 *
 * 24 px wide, i.e. three times the strip and comfortably past the 10 px the
 * user asked for - a target that has to be aimed at is not a signpost. It is
 * drawn only while the panel is away, and the panel slides over it.
 */
#define SIDEBAR_HANDLE_W   24
#define SIDEBAR_HANDLE_H   96

/* The panel itself. Wide enough for an icon, a program name and a size column,
 * narrow enough to leave most of the screen editable - the sidebar is a tool
 * beside the work, not a replacement for it. */
#define SIDEBAR_BODY_W    320

#define SIDEBAR_PAD         8
#define SIDEBAR_ROW_H      32
#define SIDEBAR_HEADER_H   34    /* the "applications" line and the index state */
#define SIDEBAR_FILTER_H   32    /* the search box under it (2026-10-10) */
#define SIDEBAR_ICON       20
#define SIDEBAR_SCROLLBAR_W 6

/*
 * The box that follows the pointer while an application is being carried.
 *
 * 160, not "the icon plus its padding": MEASURED 2026-10-10 by cropping the
 * screenshot of a real drag at native size - at 36 px wide the name inside the
 * ghost was ellipsised down to a single letter ("A..."), which tells the user
 * nothing about what they are carrying. The width has to fit a program name,
 * and a program name is not an icon.
 */
#define SIDEBAR_GHOST_W   160

/*
 * Every length, already scaled. Computed once for a dpi and then passed in.
 *
 * WHY A STRUCTURE AND NOT A MODULE-LEVEL dpi LIKE toolbar.c HAS
 *
 *   toolbar.c keeps one global dpi because exactly one overlay exists at a
 *   time. The sidebar could do the same, and then there would be two places
 *   holding a scale factor - which is how the 150% layout panel came to be
 *   laid out at one scale and drawn at another (trap 36). Passing the numbers
 *   in means the caller scales once, from the one dpi it already knows, and
 *   this module cannot disagree with it.
 *
 * `dpi` outside [48,480] is treated as 96, the same guard toolbar_set_dpi()
 * applies: a bad dpi must not be able to produce a panel wider than the screen.
 */
typedef struct {
    int edge_w;
    int body_w;
    int pad;
    int row_h;
    int header_h;
    int filter_h;
    int icon;
    int scrollbar_w;
    int ghost_w;
    int handle_w;
    int handle_h;
} SidebarMetrics;

void sidebar_metrics(int dpi, SidebarMetrics *out);

/* ------------------------------------------------------------------ */
/* State: how far out the panel is, where it is scrolled to            */
/* ------------------------------------------------------------------ */

/*
 * Progress is 0..SIDEBAR_PROGRESS_MAX rather than a pixel width so that the
 * animation is independent of the panel's size and of the dpi: the caller
 * multiplies, and a test asserts ratios instead of pixels that change with the
 * monitor.
 */
#define SIDEBAR_PROGRESS_MAX 1000

/* How long the slide takes. Short enough to feel like a response to the
 * pointer rather than an animation to wait for. */
#define SIDEBAR_SLIDE_MS     140

typedef enum {
    SIDEBAR_CLOSED = 0,   /* progress 0: only the edge strip is on screen */
    SIDEBAR_OPENING,
    SIDEBAR_OPEN,
    SIDEBAR_CLOSING
} SidebarPhase;

typedef struct {
    SidebarPhase phase;

    int          progress;       /* 0 .. SIDEBAR_PROGRESS_MAX               */
    int          from_progress;  /* progress when the current phase began -
                                  * without it a reversal mid-slide snaps    */
    unsigned long long phase_ms; /* when the current phase began            */

    int          scroll;         /* index of the first row the body shows    */

    int          hover_row;      /* -1 when none                            */
    int          press_row;      /* the row the button went down on, or -1   */

    BOOL         dragging;       /* the button is down and the pointer has
                                  * left the row it started on               */
    int          drag_row;       /* the row being dragged, or -1             */
    POINT        drag_pt;        /* where the ghost box follows the pointer  */
} SidebarState;

/*
 * A zeroed state is a closed one - SIDEBAR_CLOSED is 0 and progress 0 - so the
 * overlay needs no special case for "never touched". Safe on NULL throughout,
 * like popup.c.
 */
void sidebar_init(SidebarState *st);

int  sidebar_progress(const SidebarState *st);
SidebarPhase sidebar_phase(const SidebarState *st);

/* TRUE while the slide still has somewhere to go, i.e. while a timer is worth
 * having. FALSE for a settled state, which is what stops the overlay from
 * repainting a panel that is not moving. */
BOOL sidebar_is_animating(const SidebarState *st);

/* Fully out and settled - the state in which the list is being read. */
BOOL sidebar_is_open(const SidebarState *st);

/*
 * Ask for the panel to be out (`want_open` TRUE) or away (FALSE).
 *
 * Called from the overlay's message handler, which is the only place that
 * knows the pointer's position and the hit-test order. Repeating the request
 * that is already in effect does nothing, so this is safe on every mouse move.
 *
 * A REQUEST TO CLOSE IS IGNORED WHILE AN ITEM IS BEING DRAGGED. The whole
 * point of dragging an application out of the list is to carry it onto the
 * canvas, and the pointer is off the panel for every pixel of that trip; a
 * panel that closed on the way would take the item with it. This is the same
 * rule popup_keep_hover() implements for the panels, stated as state instead
 * of as a rectangle test.
 */
void sidebar_set_open(SidebarState *st, BOOL want_open, unsigned long long now_ms);

/*
 * Move the slide along to time `now_ms`.
 *
 * Returns TRUE when the caller should keep the animation going - repaint and
 * come back with a later timestamp - and FALSE when it has settled, at which
 * point the caller stops its timer. The time is a parameter rather than a call
 * to GetTickCount() for the same reason relcoord.c takes sizes rather than
 * querying the monitor: a test can then assert the exact progress at 0 ms,
 * half the slide, and after it.
 */
BOOL sidebar_advance(SidebarState *st, unsigned long long now_ms);

/* ------------------------------------------------------------------ */
/* Rows: hover, press, drag                                            */
/* ------------------------------------------------------------------ */

/* The row under the pointer, or -1. Callers pass what sidebar_hit() reported. */
void sidebar_note_hover(SidebarState *st, int row);
int  sidebar_hover_row(const SidebarState *st);

/*
 * The button went down on `row` / came back up.
 *
 * press_row exists only to tell a CLICK from a DRAG: a press followed by a
 * release on the same row is a click (open the editor for that application), a
 * press followed by movement is a drag (carry it onto the canvas). Keeping the
 * two apart in the state is what stops a small tremor of the hand from being
 * read as a drag.
 */
void sidebar_press(SidebarState *st, int row);
int  sidebar_pressed_row(const SidebarState *st);
void sidebar_release_press(SidebarState *st);

/* Start dragging the row that was pressed. `pt` is the current pointer. */
void sidebar_drag_begin(SidebarState *st, POINT pt);

/* Follow the pointer. TRUE when the ghost moved, so the caller repaints only
 * when there is something new to draw. */
BOOL sidebar_drag_move(SidebarState *st, POINT pt);

/* The drag is over - dropped on the canvas or cancelled. */
void sidebar_drag_end(SidebarState *st);

BOOL sidebar_is_dragging(const SidebarState *st);
int  sidebar_drag_row(const SidebarState *st);

/* The box that follows the pointer while dragging, centred on it: the thing
 * that tells the user they are carrying an application rather than having
 * selected one. */
BOOL sidebar_drag_ghost(const SidebarState *st, const SidebarMetrics *m, RECT *out);

/* ------------------------------------------------------------------ */
/* Scrolling                                                           */
/* ------------------------------------------------------------------ */

/*
 * How many rows the rows area can show. 0 for an area too short to hold even
 * one - the caller then draws the header and nothing else.
 *
 * Rounded UP, so a row cut by the bottom edge counts: it is still drawn and
 * still clickable, and drawing, hit testing and the scroll range all read this
 * one number rather than each rounding for itself.
 */
int sidebar_visible_rows(const SidebarMetrics *m, const RECT *area);

/* The largest valid value of `scroll`. 0 when everything fits. */
int sidebar_max_scroll(int total_rows, int visible_rows);

int  sidebar_scroll(const SidebarState *st);

/* All three clamp: a scroll position past the end shows blank rows under the
 * last item, which reads as "the list ended early". Illegal input (negative
 * rows, a total smaller than the window) is clamped rather than rejected,
 * because these are called from a wheel handler where refusing would mean the
 * wheel does nothing. */
void sidebar_scroll_to(SidebarState *st, int first, int total_rows, int visible_rows);
void sidebar_scroll_by(SidebarState *st, int rows, int total_rows, int visible_rows);

/* Bring `row` into view with the least movement - what the keyboard and
 * ensure-visible-on-hover need. */
void sidebar_ensure_visible(SidebarState *st, int row, int total_rows, int visible_rows);

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

/* The edge strip, along the right edge of `area` and full height. FALSE for a
 * NULL argument or a degenerate area. */
BOOL sidebar_edge_rect(const SidebarMetrics *m, const RECT *area, RECT *out);

/*
 * The handle: the tab in the MIDDLE of the right edge.
 *
 * This is the visible sign that the panel exists - the strip alone is a hot
 * zone with three dots on it, which is not enough for someone who has never
 * seen this screen before (user report, 2026-10-10). It is centred vertically
 * and clamped to `area`, so a screen shorter than the handle still gets one
 * that fits rather than one that runs off the top.
 *
 * FALSE for a NULL argument or a degenerate area.
 */
BOOL sidebar_handle_rect(const SidebarMetrics *m, const RECT *area, RECT *out);

/*
 * The body, as it is being drawn: the full panel width, slid to the right by
 * however much of it is still off screen. Its `right` may therefore be PAST
 * area.right during the slide, and its `left` may equal area.right when it is
 * fully away.
 *
 * The content is laid out in THIS rectangle, not in the visible part of it, so
 * the rows do not shift as the panel slides - they are revealed, not pushed.
 * FALSE when progress is 0 (nothing to draw).
 */
BOOL sidebar_body_rect(const SidebarState *st, const SidebarMetrics *m,
                       const RECT *area, RECT *out);

/* The part of the body actually on screen: the intersection with `area`. FALSE
 * when that is empty - which is what the caller tests before drawing, and what
 * sidebar_hit() uses to decide the pointer is on the panel at all. */
BOOL sidebar_body_visible_rect(const SidebarState *st, const SidebarMetrics *m,
                               const RECT *area, RECT *out);

/* The rectangle rows are drawn in: the body minus the padding, the header and
 * the search box. */
BOOL sidebar_rows_rect(const SidebarMetrics *m, const RECT *area, RECT *out);

/*
 * The header line: the title and the count, ABOVE the search box.
 *
 * It is EXACTLY header_h tall - deliberately not "everything above the rows".
 * Those were the same rectangle until the search box added a row between them,
 * and then "up to the rows" silently made the header two rows tall with its
 * caption centred across both: the box covered the lower half of the labels.
 * MEASURED 2026-10-10, reported by the user looking at the screen.
 *
 * The three rectangles - header, search box, rows - are now defined in terms
 * of each other (each one starts where the previous ends), so that gap cannot
 * come back.
 */
BOOL sidebar_header_rect(const SidebarState *st, const SidebarMetrics *m,
                         const RECT *area, RECT *out);

/*
 * The search box, under the header (2026-10-10).
 *
 * It exists because 343 installed programs cannot be found by scrolling: the
 * user knows the name of the thing they are looking for long before they know
 * where it sorts. The box is DRAWN by overlay.c and TYPED INTO by a real EDIT
 * window placed exactly over this rectangle - a layered window cannot host a
 * child control, and text the overlay paints itself never reaches the IME, so
 * a Chinese program name could not be typed into it at all (the same constraint
 * the name box has, MEASURED 2026-09-22).
 *
 * FALSE for a NULL argument or an area too short to hold it.
 */
BOOL sidebar_filter_rect(const SidebarState *st, const SidebarMetrics *m,
                         const RECT *area, RECT *out);

/*
 * Does `name` match `filter`?
 *
 * Case-insensitive SUBSTRING, and an empty filter matches everything - which
 * is what an empty box means. Not an anchored match and not a fuzzy one: the
 * user is looking for a program they can name, and "vsc" should find
 * "Code.exe"? No - it should not. Guessing here would make the list jump
 * around under a user who is typing the name they already know.
 *
 * NULL name matches nothing; NULL filter matches everything.
 */
BOOL sidebar_matches(const wchar_t *name, const wchar_t *filter);

/*
 * Where row `row` is drawn, clipped to the rows area - the caller fills what
 * it is given without asking again, so it never reaches into the padding or
 * the header.
 *
 * FALSE when the row is outside the visible window, which is the caller's cue
 * not to draw it, and is the whole of the virtual scrolling: a 1000-item list
 * draws `visible_rows` rectangles, not a thousand.
 */
BOOL sidebar_row_rect(const SidebarState *st, const SidebarMetrics *m,
                      const RECT *area, int row, RECT *out);

/*
 * The scrollbar track and thumb, or FALSE when the whole list fits and there
 * is nothing to scroll - a scrollbar on a list that cannot scroll is a control
 * that does nothing, which is the shape trap 24 warns about.
 *
 * `thumb` may be NULL if the caller only wants the track.
 */
BOOL sidebar_scrollbar_rects(const SidebarState *st, const SidebarMetrics *m,
                             const RECT *area, int total_rows, int visible_rows,
                             RECT *track, RECT *thumb);

/* Dragging the thumb: which row a pointer at `y` asks for. The thumb is
 * centred on `y`, and the result is clamped, so dragging past either end stops
 * at the end rather than going blank. */
int sidebar_scroll_from_thumb(const RECT *track, int thumb_h,
                              int total_rows, int visible_rows, int y);

/* ------------------------------------------------------------------ */
/* Dropping an item on the canvas                                      */
/* ------------------------------------------------------------------ */

/*
 * The zone a drop at `pt` asks for.
 *
 * A drop says WHERE, not how big: the size is what the editor is for. So the
 * rectangle is `want_w` x `want_h` of the canvas, CENTRED on the pointer - the
 * user is pointing at the place they want the window, not at its top-left
 * pixel - and clamped inside `area`, because a zone hanging off the screen
 * edge is one the user cannot see to correct.
 *
 * `want_w`/`want_h` are passed in rather than fixed here: the default belongs
 * to whoever owns that policy, and this function stays arithmetic. Values
 * above 1 are clamped rather than refused - the clamp is the answer.
 *
 * The result is a FRACTION zone: min_w/min_h come back as 1 and abs_w/abs_h as
 * 0, which is the "follow the panel's shape" half of the rule in section 5.2.
 * There is no measured window yet, so there is nothing for the other half to
 * hold.
 *
 * FALSE for a NULL argument, an empty area, or a non-positive size.
 */
BOOL sidebar_drop_zone(const RECT *area, POINT pt, float want_w, float want_h,
                       RelRect *out);

/* ------------------------------------------------------------------ */
/* Hit testing                                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    SIDEBAR_HIT_NONE = 0,   /* not on the sidebar at all: the canvas gets it */
    SIDEBAR_HIT_EDGE,       /* the edge strip, when the body is not covering it */
    SIDEBAR_HIT_HANDLE,     /* the tab in the middle of the right edge         */
    SIDEBAR_HIT_HEADER,     /* the header line                                */
    SIDEBAR_HIT_FILTER,     /* the search box                                 */
    SIDEBAR_HIT_ROW,        /* one application row                            */
    SIDEBAR_HIT_SCROLLBAR,  /* the track or the thumb                         */
    SIDEBAR_HIT_BODY        /* the panel's own background                     */
} SidebarHitKind;

typedef struct {
    SidebarHitKind kind;
    int            row;     /* the row index, or -1 unless kind is ROW */
} SidebarHit;

/*
 * What is at `pt`.
 *
 * The body is tested BEFORE the edge strip: during the slide the panel covers
 * part of the strip, and the part it covers belongs to the panel (trap 33 -
 * ask whose rectangle it is first). Points outside the visible part of the
 * body never answer as the body, however far the panel has slid: a click on
 * invisible panel is a click on the canvas.
 *
 * Among the strip's own rectangles the HANDLE is asked before the strip: it is
 * wider than the strip and it is the part the user aims at, and the two overlap
 * by construction.
 *
 * `total_rows` is what the caller is actually showing, not what the index
 * holds - the two differ while indexing, and a row that is not there must not
 * be hittable.
 */
SidebarHit sidebar_hit(const SidebarState *st, const SidebarMetrics *m,
                       const RECT *area, int total_rows, POINT pt);

/* TRUE for every kind that means "the pointer is on the sidebar", which is
 * what keeps it open. */
BOOL sidebar_hit_keeps_open(SidebarHit hit);

#ifdef __cplusplus
}
#endif

#endif /* WAC_SIDEBAR_H */
