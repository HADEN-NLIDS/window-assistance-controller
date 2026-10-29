/*
 * t70_sidebar.c - unit tests for src/ui/sidebar.c
 *
 * WHY THIS FILE CAN EXIST AT ALL
 *
 *   The sidebar is drawn by overlay.c and driven by the mouse, and neither can
 *   be tested without a window. What CAN be tested is the arithmetic that the
 *   drawing and the mouse both depend on - and those are exactly the parts
 *   that go wrong on someone else's machine rather than on the developer's:
 *
 *     - a 1000-item index in a window that shows 42 rows
 *     - a pointer one pixel to the left of the edge strip
 *     - a click on the bottom row, which the screen cuts in half
 *     - a drag that has left the panel while the button is still down
 *     - a slide reversed halfway out
 *
 *   sidebar.c calls no Win32 function (see sidebar.h), so this test links no
 *   window, pumps no messages and passes on any machine. It is written the way
 *   t52_popup.c is: constructed rectangles, constructed points, no screen.
 *
 * WHAT IT DOES NOT COVER
 *
 *   Whether a click on the edge strip really opens the panel, and whether
 *   dragging an item out really creates a preset, are properties of the
 *   overlay's message handler and of the wiring that does not exist yet - so
 *   they are not here. They are covered by screenshots and a live run when 2b
 *   is wired up (docs/手册.md section 8.23), the same way the pop-up panels
 *   were.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t70_sidebar
 *   tests\out\t70_sidebar.exe --no-pause
 */

#include "test_common.h"

#include "../src/ui/sidebar.h"

static int g_pass, g_fail;

static void ok(const wchar_t *what, int cond)
{
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls", what);
    }
}

static void ok_int(const wchar_t *what, int got, int want)
{
    if (got == want) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls  (got %d, want %d)", what, got, want);
    }
}

/* Fractions come back as ratios of integers, so they are compared with a
 * tolerance rather than for equality - the same thing t10_relcoord does. */
static void ok_near(const wchar_t *what, float got, float want)
{
    const float d = got - want;
    if (d < 0.0005f && d > -0.0005f) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls  (got %.4f, want %.4f)", what, got, want);
    }
}

static RECT r4(int l, int t, int r, int b)
{
    RECT x;
    x.left = l; x.top = t; x.right = r; x.bottom = b;
    return x;
}

static POINT p2(int x, int y)
{
    POINT p;
    p.x = x; p.y = y;
    return p;
}

/*
 * The screen the tests work on: 2560x1440 (the development machine's desktop
 * panel), with the toolbar strip's 64 px already taken off the top - which is
 * what overlay.c hands the sidebar as `area`.
 */
static RECT area_std(void)  { return r4(0, 64, 2560, 1440); }

/* Bring a state to fully open, the way the overlay does: request, then let the
 * clock run past the slide. */
static void open_fully(SidebarState *st, unsigned long long t0)
{
    sidebar_set_open(st, TRUE, t0);
    (void)sidebar_advance(st, t0 + SIDEBAR_SLIDE_MS);
}

/* ------------------------------------------------------------------ */

static void test_metrics(void)
{
    T_SEC(L"1. metrics: one dpi in, every length out");

    SidebarMetrics m;
    sidebar_metrics(96, &m);

    ok_int(L"edge strip 8 px at 96 dpi", m.edge_w, 8);
    ok_int(L"body 320 px", m.body_w, 320);
    ok_int(L"padding 8", m.pad, 8);
    ok_int(L"row 32", m.row_h, 32);
    ok_int(L"header 34", m.header_h, 34);
    ok_int(L"filter box 32", m.filter_h, 32);
    ok_int(L"icon 20", m.icon, 20);
    ok_int(L"scrollbar 6", m.scrollbar_w, 6);
    ok_int(L"ghost 160", m.ghost_w, 160);
    ok_int(L"handle 24 x 96", m.handle_w * 1000 + m.handle_h, 24096);

    /* At 150% every length grows by the same factor - that is the whole point
     * of trap 36: geometry and font must be scaled from ONE number. */
    SidebarMetrics m150;
    sidebar_metrics(144, &m150);
    ok_int(L"150%: edge 12", m150.edge_w, 12);
    ok_int(L"150%: body 480", m150.body_w, 480);
    ok_int(L"150%: padding 12", m150.pad, 12);
    ok_int(L"150%: row 48", m150.row_h, 48);
    ok_int(L"150%: header 51", m150.header_h, 51);
    ok_int(L"150%: filter box 48", m150.filter_h, 48);
    ok_int(L"150%: icon 30", m150.icon, 30);
    ok_int(L"150%: scrollbar 9", m150.scrollbar_w, 9);
    ok_int(L"150%: ghost 240", m150.ghost_w, 240);
    ok_int(L"150%: handle 36 x 144", m150.handle_w * 1000 + m150.handle_h, 36144);

    /* A dpi Windows never reports must not be able to make a panel wider than
     * the monitor - same guard toolbar_set_dpi() applies. */
    SidebarMetrics bad;
    sidebar_metrics(0, &bad);
    ok_int(L"dpi 0 falls back to 96", bad.body_w, 320);
    sidebar_metrics(100000, &bad);
    ok_int(L"an absurd dpi falls back to 96", bad.body_w, 320);

    /* The smallest dpi the guard lets through still produces usable numbers:
     * the icon fits its row, and nothing is zero-wide. */
    SidebarMetrics m48;
    sidebar_metrics(48, &m48);
    ok_int(L"48 dpi: row 16", m48.row_h, 16);
    ok_int(L"48 dpi: icon 10", m48.icon, 10);
    ok(L"48 dpi: the icon still fits the row", m48.row_h >= m48.icon + 4);
    ok(L"48 dpi: the edge strip is still hittable", m48.edge_w >= 1);
    ok(L"48 dpi: the scrollbar is still grabbable", m48.scrollbar_w >= 2);

    sidebar_metrics(96, NULL);   /* must not crash */
    ok(L"NULL metrics output is ignored", 1);
}

static void test_slide(void)
{
    T_SEC(L"2. the slide: requests, reversal, and settling");

    SidebarState st;
    sidebar_init(&st);
    ok_int(L"a fresh state is closed", (int)sidebar_phase(&st), (int)SIDEBAR_CLOSED);
    ok_int(L"at progress 0", sidebar_progress(&st), 0);
    ok(L"and needs no timer", !sidebar_is_animating(&st));
    ok_int(L"and no row is hovered", sidebar_hover_row(&st), -1);
    ok_int(L"and no row is pressed", sidebar_pressed_row(&st), -1);

    sidebar_set_open(&st, TRUE, 1000);
    ok_int(L"asking to open starts a slide", (int)sidebar_phase(&st), (int)SIDEBAR_OPENING);
    ok_int(L"which begins where it was", sidebar_progress(&st), 0);
    ok(L"and wants a timer", sidebar_is_animating(&st));
    ok(L"but is not open yet", !sidebar_is_open(&st));

    /* Repeating the request must not restart the slide: the overlay calls this
     * on every mouse move, and restarting would mean a panel that never
     * arrives. */
    sidebar_set_open(&st, TRUE, 1010);
    ok(L"a repeated request leaves the phase alone", sidebar_is_animating(&st));

    ok(L"halfway through: still moving",
       sidebar_advance(&st, 1000 + SIDEBAR_SLIDE_MS / 2));
    ok_int(L"halfway through: progress is half",
           sidebar_progress(&st), SIDEBAR_PROGRESS_MAX / 2);

    ok(L"at the end: settled", !sidebar_advance(&st, 1000 + SIDEBAR_SLIDE_MS));
    ok_int(L"fully out", sidebar_progress(&st), SIDEBAR_PROGRESS_MAX);
    ok_int(L"phase is OPEN", (int)sidebar_phase(&st), (int)SIDEBAR_OPEN);
    ok(L"and no timer is needed", !sidebar_is_animating(&st));
    ok(L"and it is open", sidebar_is_open(&st));

    /* Closing, then changing your mind halfway: progress must resume from
     * where it is, not from an end. Restarting from 0 is what makes a reversed
     * slide visibly jump. */
    sidebar_set_open(&st, FALSE, 2000);
    ok_int(L"asking to close starts the slide back",
           (int)sidebar_phase(&st), (int)SIDEBAR_CLOSING);
    (void)sidebar_advance(&st, 2000 + SIDEBAR_SLIDE_MS / 2);
    ok_int(L"halfway back", sidebar_progress(&st), SIDEBAR_PROGRESS_MAX / 2);

    sidebar_set_open(&st, TRUE, 2100);
    ok_int(L"reversing goes back to OPENING",
           (int)sidebar_phase(&st), (int)SIDEBAR_OPENING);
    (void)sidebar_advance(&st, 2100 + SIDEBAR_SLIDE_MS / 2);
    ok_int(L"and resumes from where it was, not from 0",
           sidebar_progress(&st), SIDEBAR_PROGRESS_MAX * 3 / 4);

    (void)sidebar_advance(&st, 2100 + SIDEBAR_SLIDE_MS);
    ok_int(L"settles fully out again", sidebar_progress(&st), SIDEBAR_PROGRESS_MAX);

    /* All the way back to closed is a settled state, not an animating one. */
    sidebar_set_open(&st, FALSE, 3000);
    ok(L"the way back finishes", !sidebar_advance(&st, 3000 + SIDEBAR_SLIDE_MS));
    ok_int(L"progress 0", sidebar_progress(&st), 0);
    ok_int(L"phase CLOSED", (int)sidebar_phase(&st), (int)SIDEBAR_CLOSED);

    /* A timestamp that went backwards is "no time has passed", not a jump. */
    sidebar_set_open(&st, TRUE, 5000);
    (void)sidebar_advance(&st, 5000 + SIDEBAR_SLIDE_MS / 2);
    const int before = sidebar_progress(&st);
    (void)sidebar_advance(&st, 4000);
    ok_int(L"a backwards clock does not rewind the slide", sidebar_progress(&st), before);

    /* Safe on NULL throughout - overlay.c calls these from paths where the
     * overlay may already be gone. */
    sidebar_init(NULL);
    sidebar_set_open(NULL, TRUE, 0);
    ok(L"NULL state is not animating", !sidebar_is_animating(NULL));
    ok(L"NULL state does not advance", !sidebar_advance(NULL, 0));
    ok_int(L"NULL state is closed", (int)sidebar_phase(NULL), (int)SIDEBAR_CLOSED);
    ok_int(L"NULL state has progress 0", sidebar_progress(NULL), 0);
}

static void test_geometry(void)
{
    T_SEC(L"3. where the edge strip and the body are");

    const RECT area = area_std();
    SidebarMetrics m;
    sidebar_metrics(96, &m);

    /* The edge strip hugs the right edge, and its height is the area's - the
     * sidebar never covers the toolbar (trap 33). */
    RECT edge;
    ok(L"the edge strip has a rectangle", sidebar_edge_rect(&m, &area, &edge));
    ok_int(L"at the right edge", edge.right, 2560);
    ok_int(L"8 px wide", edge.left, 2552);
    ok_int(L"starting below the toolbar", edge.top, 64);
    ok_int(L"down to the bottom", edge.bottom, 1440);

    /* Nothing is drawn at progress 0: the body rectangle does not exist. */
    SidebarState st;
    sidebar_init(&st);
    RECT body, vis;
    ok(L"closed: no body rectangle", !sidebar_body_rect(&st, &m, &area, &body));

    open_fully(&st, 0);
    ok(L"open: the body has a rectangle", sidebar_body_rect(&st, &m, &area, &body));
    ok_int(L"its right edge is the screen's", body.right, 2560);
    ok_int(L"and it is 320 wide", body.left, 2240);

    ok(L"the visible part exists", sidebar_body_visible_rect(&st, &m, &area, &vis));
    ok_int(L"fully out: the whole body is visible", vis.left, 2240);

    /* Halfway: the panel is half revealed. Its LEFT edge is what moves - the
     * content is laid out in the full rectangle and revealed by the screen
     * edge, so its right edge is PAST the screen during the slide. */
    SidebarState half;
    sidebar_init(&half);
    sidebar_set_open(&half, TRUE, 0);
    (void)sidebar_advance(&half, SIDEBAR_SLIDE_MS / 2);

    ok(L"halfway: the body has a rectangle",
       sidebar_body_rect(&half, &m, &area, &body));
    ok_int(L"halfway: 160 px are on screen", 2560 - body.left, 160);
    ok_int(L"halfway: the panel keeps its width", body.right - body.left, 320);
    ok_int(L"halfway: its right edge is off screen", body.right, 2720);

    ok(L"halfway: the visible part exists",
       sidebar_body_visible_rect(&half, &m, &area, &vis));
    ok_int(L"halfway: visible from 2400", vis.left, 2400);
    ok_int(L"halfway: visible to the screen edge", vis.right, 2560);

    /* A panel wider than the screen is clamped to it, or its own left edge
     * would be off screen and nothing could be read from it. */
    const RECT narrow = r4(0, 64, 200, 1000);
    SidebarState open_st;
    sidebar_init(&open_st);
    open_fully(&open_st, 0);
    ok(L"a narrow screen still has a body",
       sidebar_body_rect(&open_st, &m, &narrow, &body));
    ok_int(L"clamped to the screen width", body.right - body.left, 200);

    /* Degenerate input is refused rather than guessed at. */
    ok(L"NULL area refused", !sidebar_body_rect(&open_st, &m, NULL, &body));
    ok(L"NULL output refused", !sidebar_body_rect(&open_st, &m, &area, NULL));
    ok(L"NULL metrics refused", !sidebar_body_rect(&open_st, NULL, &area, &body));
    const RECT empty = r4(10, 10, 10, 10);
    ok(L"an empty area refused", !sidebar_body_rect(&open_st, &m, &empty, &body));
    ok(L"an empty area has no edge strip", !sidebar_edge_rect(&m, &empty, &edge));
}

static void test_scrolling(void)
{
    T_SEC(L"4. virtual scrolling: 1000 items, 42 rows on screen");

    const RECT area = area_std();
    SidebarMetrics m;
    sidebar_metrics(96, &m);

    RECT rows;
    ok(L"the rows area exists", sidebar_rows_rect(&m, &area, &rows));
    ok_int(L"below the padding, the header and the filter box", rows.top, 138);
    ok_int(L"above the bottom padding", rows.bottom, 1432);

    /* Rounded UP: 1326 px of rows area holds 41 whole rows and a 14-px sliver,
     * and that sliver is still drawn and still clickable. */
    ok_int(L"42 rows, counting the cut one", sidebar_visible_rows(&m, &area), 41);

    ok_int(L"1000 items cannot fit", sidebar_max_scroll(1000, 41), 959);
    ok_int(L"10 items fit: no scrolling", sidebar_max_scroll(10, 41), 0);
    ok_int(L"nothing to show", sidebar_max_scroll(0, 41), 0);
    ok_int(L"a window too short for one row", sidebar_max_scroll(10, 0), 0);

    SidebarState st;
    sidebar_init(&st);
    open_fully(&st, 0);

    sidebar_scroll_to(&st, 100, 1000, 41);
    ok_int(L"scrolled to 100", sidebar_scroll(&st), 100);
    sidebar_scroll_to(&st, 100000, 1000, 41);
    ok_int(L"clamped at the end", sidebar_scroll(&st), 959);
    sidebar_scroll_to(&st, -5, 1000, 41);
    ok_int(L"clamped at the start", sidebar_scroll(&st), 0);

    sidebar_scroll_by(&st, 10, 1000, 41);
    ok_int(L"wheel down by 10", sidebar_scroll(&st), 10);
    sidebar_scroll_by(&st, -100, 1000, 41);
    ok_int(L"wheel up past the top stops at 0", sidebar_scroll(&st), 0);

    /* The window may have shrunk since the scroll was set; a stored position
     * past the end shows blank rows under the last item. */
    sidebar_scroll_to(&st, 959, 1000, 41);
    sidebar_scroll_to(&st, sidebar_scroll(&st), 20, 41);
    ok_int(L"a shorter list re-clamps", sidebar_scroll(&st), 0);

    /* ensure_visible: the least movement that shows the row. */
    sidebar_scroll_to(&st, 100, 1000, 41);
    sidebar_ensure_visible(&st, 120, 1000, 41);
    ok_int(L"already visible: nothing moves", sidebar_scroll(&st), 100);
    sidebar_ensure_visible(&st, 100, 1000, 41);
    ok_int(L"the first visible row is visible", sidebar_scroll(&st), 100);
    sidebar_ensure_visible(&st, 50, 1000, 41);
    ok_int(L"above the window: scrolls up to it", sidebar_scroll(&st), 50);
    sidebar_ensure_visible(&st, 500, 1000, 41);
    ok_int(L"below the window: scrolls down just enough", sidebar_scroll(&st), 460);
    sidebar_ensure_visible(&st, -1, 1000, 41);
    ok_int(L"no row asked for: nothing moves", sidebar_scroll(&st), 460);

    /* Row rectangles: only the visible window answers, and the cut bottom row
     * is handed back clipped so the caller can fill it as it is. */
    sidebar_scroll_to(&st, 0, 1000, 41);
    RECT row;
    ok(L"row 0 exists", sidebar_row_rect(&st, &m, &area, 0, &row));
    ok_int(L"row 0 starts at the top of the rows area", row.top, 138);
    ok_int(L"row 0 is one row tall", row.bottom - row.top, 32);
    ok_int(L"row 0 starts inside the padding", row.left, 2248);
    ok_int(L"row 0 ends before the right padding", row.right, 2552);

    ok(L"row 40 - the cut one - exists", sidebar_row_rect(&st, &m, &area, 40, &row));
    ok_int(L"and is clipped to the rows area", row.bottom, 1432);
    ok_int(L"so it is shorter than a row", row.bottom - row.top, 14);

    ok(L"row 41 is below the window", !sidebar_row_rect(&st, &m, &area, 41, &row));
    ok(L"a negative row does not exist", !sidebar_row_rect(&st, &m, &area, -1, &row));

    /* THIS is the virtual scrolling: a 1000-item list produces exactly as many
     * rectangles as fit, not a thousand. */
    int drawn = 0;
    for (int i = 0; i < 1000; ++i) {
        if (sidebar_row_rect(&st, &m, &area, i, &row)) drawn++;
    }
    ok_int(L"1000 items draw 42 rows", drawn, 41);

    /* Scrolled down, the window moves with it and nothing above it answers. */
    sidebar_scroll_to(&st, 100, 1000, 41);
    ok(L"row 99 is now above the window",
       !sidebar_row_rect(&st, &m, &area, 99, &row));
    ok(L"row 100 is the first one", sidebar_row_rect(&st, &m, &area, 100, &row));
    ok_int(L"and it is at the top of the rows area", row.top, 138);
    ok(L"row 140 is the last one", sidebar_row_rect(&st, &m, &area, 140, &row));
    ok(L"row 141 is below the window",
       !sidebar_row_rect(&st, &m, &area, 141, &row));

    drawn = 0;
    for (int i = 0; i < 1000; ++i) {
        if (sidebar_row_rect(&st, &m, &area, i, &row)) drawn++;
    }
    ok_int(L"still 42 rows drawn when scrolled", drawn, 41);

    /* Closed: nothing is drawn at all. */
    SidebarState closed;
    sidebar_init(&closed);
    ok(L"closed: no rows are drawn",
       !sidebar_row_rect(&closed, &m, &area, 0, &row));
}

static void test_scrollbar(void)
{
    T_SEC(L"5. the scrollbar exists only when there is somewhere to scroll");

    const RECT area = area_std();
    SidebarMetrics m;
    sidebar_metrics(96, &m);

    SidebarState st;
    sidebar_init(&st);
    open_fully(&st, 0);

    RECT track, thumb;
    ok(L"1000 items: there is a scrollbar",
       sidebar_scrollbar_rects(&st, &m, &area, 1000, 41, &track, &thumb));
    ok_int(L"the track starts at the top of the rows area", track.top, 138);
    ok_int(L"and ends at its bottom", track.bottom, 1432);
    ok_int(L"the track is 6 px wide", track.right - track.left, 6);
    ok_int(L"inside the right padding", track.right, 2552);

    ok(L"the thumb has a height", thumb.bottom > thumb.top);
    ok_int(L"the thumb is at the top when the list is", thumb.top, 138);
    ok(L"the thumb is shorter than the track", thumb.bottom - thumb.top < 1294);

    /* 42 of 1000 rows: the thumb is about 4% of the track. */
    ok_int(L"thumb height is visible/total of the track", thumb.bottom - thumb.top, 53);
    ok_int(L"the thumb is as wide as the track", thumb.right - thumb.left, 6);

    /* At the end of the list the thumb sits on the bottom of its track. */
    sidebar_scroll_to(&st, 959, 1000, 41);
    ok(L"scrolled to the end: still a scrollbar",
       sidebar_scrollbar_rects(&st, &m, &area, 1000, 41, &track, &thumb));
    ok_int(L"the thumb has reached the bottom", thumb.bottom, 1432);

    /* A list that fits has no scrollbar: a control that does nothing is worse
     * than no control (trap 24). */
    ok(L"10 items: no scrollbar",
       !sidebar_scrollbar_rects(&st, &m, &area, 10, 41, &track, &thumb));
    ok(L"exactly as many items as rows: no scrollbar",
       !sidebar_scrollbar_rects(&st, &m, &area, 41, 42, &track, &thumb));
    ok(L"an empty index: no scrollbar",
       !sidebar_scrollbar_rects(&st, &m, &area, 0, 41, &track, &thumb));

    /* Dragging the thumb: the top of the track is row 0, the bottom is the
     * last row, and past either end it stops. */
    (void)sidebar_scrollbar_rects(&st, &m, &area, 1000, 41, &track, &thumb);
    const int thumb_h = thumb.bottom - thumb.top;
    ok_int(L"thumb at the very top asks for row 0",
           sidebar_scroll_from_thumb(&track, thumb_h, 1000, 41, track.top), 0);
    ok_int(L"thumb at the very bottom asks for the last row",
           sidebar_scroll_from_thumb(&track, thumb_h, 1000, 41, track.bottom), 959);
    ok_int(L"dragged above the track: still row 0",
           sidebar_scroll_from_thumb(&track, thumb_h, 1000, 41, track.top - 500), 0);
    ok_int(L"dragged below the track: still the last row",
           sidebar_scroll_from_thumb(&track, thumb_h, 1000, 41, track.bottom + 500), 959);

    const int mid = sidebar_scroll_from_thumb(&track, thumb_h, 1000, 41,
                                              track.top + 1326 / 2);
    ok(L"the middle of the track is somewhere in the middle",
       mid > 400 && mid < 560);

    /* A list that fits has nowhere to drag to. */
    ok_int(L"no scrolling: any y is row 0",
           sidebar_scroll_from_thumb(&track, thumb_h, 10, 41, 800), 0);

    RECT only_track;
    ok(L"the track can be asked for alone",
       sidebar_scrollbar_rects(&st, &m, &area, 1000, 41, &only_track, NULL));
    ok(L"and neither is refused",
       !sidebar_scrollbar_rects(&st, &m, &area, 1000, 41, NULL, NULL));
}

static void test_hit(void)
{
    T_SEC(L"6. what a point means");

    const RECT area = area_std();
    SidebarMetrics m;
    sidebar_metrics(96, &m);

    /* Closed: only the edge strip answers, and only for its own 8 px. The
     * canvas gets everything else. */
    SidebarState st;
    sidebar_init(&st);
    ok_int(L"closed: on the edge strip is EDGE",
           (int)sidebar_hit(&st, &m, &area, 1000, p2(2556, 200)).kind,
           (int)SIDEBAR_HIT_EDGE);
    ok_int(L"closed: one pixel left of it is the canvas",
           (int)sidebar_hit(&st, &m, &area, 1000, p2(2551, 200)).kind,
           (int)SIDEBAR_HIT_NONE);
    ok_int(L"closed: the middle of the screen is the canvas",
           (int)sidebar_hit(&st, &m, &area, 1000, p2(1000, 700)).kind,
           (int)SIDEBAR_HIT_NONE);
    ok_int(L"closed: above the area is the canvas",
           (int)sidebar_hit(&st, &m, &area, 1000, p2(2556, 10)).kind,
           (int)SIDEBAR_HIT_NONE);

    open_fully(&st, 0);

    /* Open: the body answers, and it answers as a ROW with the right index. */
    SidebarHit h = sidebar_hit(&st, &m, &area, 1000, p2(2400, 200));
    ok_int(L"open: a point in the list is a row", (int)h.kind, (int)SIDEBAR_HIT_ROW);
    ok_int(L"and it is row 2", h.row, 1);

    h = sidebar_hit(&st, &m, &area, 1000, p2(2400, 138));
    ok_int(L"the first pixel of the rows area is row 0", h.row, 0);
    h = sidebar_hit(&st, &m, &area, 1000, p2(2400, 169));
    ok_int(L"the last pixel of row 0 is still row 0", h.row, 0);
    h = sidebar_hit(&st, &m, &area, 1000, p2(2400, 170));
    ok_int(L"one pixel lower is row 1", h.row, 1);

    /* The header is above the rows, and it is not a row. */
    ok_int(L"open: the header line is the header",
           (int)sidebar_hit(&st, &m, &area, 1000, p2(2400, 80)).kind,
           (int)SIDEBAR_HIT_HEADER);

    /* The body covers the edge strip while it is out, and the covered part
     * belongs to the body (trap 33). */
    h = sidebar_hit(&st, &m, &area, 1000, p2(2556, 200));
    ok_int(L"an open body owns the edge strip's pixels", (int)h.kind,
           (int)SIDEBAR_HIT_ROW);
    ok_int(L"and it is still row 2", h.row, 1);

    /* The scrollbar wins over the row underneath it, or it could never be
     * grabbed. */
    ok_int(L"the scrollbar beats the row under it",
           (int)sidebar_hit(&st, &m, &area, 1000, p2(2549, 200)).kind,
           (int)SIDEBAR_HIT_SCROLLBAR);

    /* Past the last item is the panel's background, not a row that is not
     * there. */
    ok_int(L"below the last item is background",
           (int)sidebar_hit(&st, &m, &area, 10, p2(2400, 700)).kind,
           (int)SIDEBAR_HIT_BODY);
    ok_int(L"an empty index is all background",
           (int)sidebar_hit(&st, &m, &area, 0, p2(2400, 200)).kind,
           (int)SIDEBAR_HIT_BODY);
    ok(L"a row that is not there has no index",
       sidebar_hit(&st, &m, &area, 10, p2(2400, 700)).row == -1);

    /* Scrolled: the same pixel is a different row, and the rows above the
     * window are not hittable because they are not on screen. */
    sidebar_scroll_to(&st, 100, 1000, 41);
    ok_int(L"scrolled by 100: the top row is row 100",
           sidebar_hit(&st, &m, &area, 1000, p2(2400, 200)).row, 101);

    /* Partly slid out: only the visible part of the body is the body. A click
     * on the part that is still off screen is a click on the canvas. */
    SidebarState half;
    sidebar_init(&half);
    sidebar_set_open(&half, TRUE, 0);
    (void)sidebar_advance(&half, SIDEBAR_SLIDE_MS / 2);

    ok_int(L"halfway out: the revealed part is the panel",
           (int)sidebar_hit(&half, &m, &area, 1000, p2(2450, 200)).kind,
           (int)SIDEBAR_HIT_ROW);
    ok_int(L"halfway out: the hidden part is the canvas",
           (int)sidebar_hit(&half, &m, &area, 1000, p2(2300, 200)).kind,
           (int)SIDEBAR_HIT_NONE);
    ok_int(L"halfway out: the visible part covers the edge strip",
           (int)sidebar_hit(&half, &m, &area, 1000, p2(2556, 200)).kind,
           (int)SIDEBAR_HIT_ROW);

    /* One pixel out: a one-pixel column of panel, and it is still the panel. */
    SidebarState crack;
    sidebar_init(&crack);
    sidebar_set_open(&crack, TRUE, 0);
    (void)sidebar_advance(&crack, 1);      /* 1000 * 1 / 140 = 7 -> ceil(2.3) = 3 px */
    ok_int(L"just opening: the leading pixels are the panel",
           (int)sidebar_hit(&crack, &m, &area, 1000, p2(2559, 200)).kind,
           (int)SIDEBAR_HIT_ROW);

    /* Degenerate input says "not mine" rather than guessing. */
    ok_int(L"NULL state: not mine",
           (int)sidebar_hit(NULL, &m, &area, 10, p2(2556, 200)).kind,
           (int)SIDEBAR_HIT_NONE);
    ok_int(L"NULL area: not mine",
           (int)sidebar_hit(&st, &m, NULL, 10, p2(2556, 200)).kind,
           (int)SIDEBAR_HIT_NONE);
    ok_int(L"NULL metrics: not mine",
           (int)sidebar_hit(&st, NULL, &area, 10, p2(2556, 200)).kind,
           (int)SIDEBAR_HIT_NONE);

    /* The hover rule, as the overlay reads it. */
    ok(L"the edge strip keeps the panel open",
       sidebar_hit_keeps_open(sidebar_hit(&st, &m, &area, 1000, p2(2556, 200))));
    ok(L"the canvas does not",
       !sidebar_hit_keeps_open(sidebar_hit(&st, &m, &area, 1000, p2(1000, 700))));
}

static void test_drag(void)
{
    T_SEC(L"7. picking an application up and carrying it out");

    SidebarState st;
    sidebar_init(&st);
    open_fully(&st, 0);

    /* A drag is the continuation of a press: without one there is nothing to
     * carry. */
    sidebar_drag_begin(&st, p2(2400, 200));
    ok(L"a drag cannot start without a press", !sidebar_is_dragging(&st));

    /* A click and a drag start the same way and are told apart by what
     * happens next. */
    sidebar_note_hover(&st, 4);
    sidebar_press(&st, 4);
    ok_int(L"the press remembers its row", sidebar_pressed_row(&st), 4);
    ok_int(L"and the pointer is on it", sidebar_hover_row(&st), 4);

    sidebar_drag_begin(&st, p2(2400, 234));
    ok(L"the drag starts", sidebar_is_dragging(&st));
    ok_int(L"carrying the pressed row", sidebar_drag_row(&st), 4);
    ok_int(L"and the hover is cleared", sidebar_hover_row(&st), -1);

    /* The panel must not close while an item is being carried onto the canvas:
     * the pointer is off the panel for every pixel of that trip. */
    sidebar_set_open(&st, FALSE, 100);
    ok_int(L"a close request during a drag is ignored",
           (int)sidebar_phase(&st), (int)SIDEBAR_OPEN);
    ok_int(L"the panel stays out", sidebar_progress(&st), SIDEBAR_PROGRESS_MAX);
    ok(L"and the item is still being carried", sidebar_is_dragging(&st));

    /* The ghost follows the pointer, and only a real move is news. */
    ok(L"an unmoved pointer is not a move", !sidebar_drag_move(&st, p2(2400, 234)));
    ok(L"a move is news", sidebar_drag_move(&st, p2(1200, 700)));

    SidebarMetrics m;
    sidebar_metrics(96, &m);
    RECT ghost;
    ok(L"there is a ghost box", sidebar_drag_ghost(&st, &m, &ghost));
    ok_int(L"centred on the pointer, x", ghost.left, 1200 - 160 / 2);
    ok_int(L"centred on the pointer, y", ghost.top, 700 - 16);
    ok_int(L"the ghost is one row tall", ghost.bottom - ghost.top, 32);
    ok_int(L"the ghost is wide enough for a name", ghost.right - ghost.left, 160);

    /* Dropping ends it - and once it is over, the ignored close request goes
     * through. */
    sidebar_drag_end(&st);
    ok(L"the drag is over", !sidebar_is_dragging(&st));
    ok_int(L"and no row is being carried", sidebar_drag_row(&st), -1);
    ok_int(L"and the press is cleared", sidebar_pressed_row(&st), -1);
    ok(L"and the ghost is gone", !sidebar_drag_ghost(&st, &m, &ghost));

    sidebar_set_open(&st, FALSE, 200);
    ok_int(L"after the drag the panel may close",
           (int)sidebar_phase(&st), (int)SIDEBAR_CLOSING);

    /* Releasing on the same row without moving is a click, not a drag: the
     * press is simply cleared. */
    sidebar_press(&st, 7);
    sidebar_release_press(&st);
    ok_int(L"a click leaves no press behind", sidebar_pressed_row(&st), -1);
    ok(L"and starts no drag", !sidebar_is_dragging(&st));

    /* Safe on NULL. */
    sidebar_note_hover(NULL, 3);
    sidebar_press(NULL, 3);
    sidebar_release_press(NULL);
    sidebar_drag_begin(NULL, p2(0, 0));
    ok(L"NULL state does not drag", !sidebar_is_dragging(NULL));
    ok(L"NULL state does not move", !sidebar_drag_move(NULL, p2(1, 1)));
    ok(L"NULL state has no ghost", !sidebar_drag_ghost(NULL, &m, &ghost));
    ok_int(L"NULL state carries no row", sidebar_drag_row(NULL), -1);
    sidebar_drag_end(NULL);
    ok(L"NULL state is not open", !sidebar_is_open(NULL));
}

/* ------------------------------------------------------------------ */

static void test_drop(void)
{
    T_SEC(L"8. where a dropped application lands");

    const RECT area = area_std();      /* (0,64)-(2560,1440): 2560 x 1376 */

    RelRect z;

    /* Half the width, full height, CENTRED on the pointer: the user points at
     * the place they want the window, not at its top-left pixel. */
    ok(L"a drop in the middle has a zone",
       sidebar_drop_zone(&area, p2(1280, 752), 0.5f, 1.0f, &z));
    ok_near(L"half the canvas wide", z.w, 0.5f);
    ok_near(L"the full canvas tall", z.h, 1.0f);
    ok_near(L"centred horizontally", z.x, 0.25f);
    ok_near(L"and at the top, because it is full height", z.y, 0.0f);

    /* A shorter default centres vertically too. */
    ok(L"a half-height drop has a zone",
       sidebar_drop_zone(&area, p2(1280, 752), 0.5f, 0.5f, &z));
    ok_near(L"half the canvas tall", z.h, 0.5f);
    ok_near(L"centred vertically", z.y, 0.25f);

    /* A FRACTION zone, not a measured one: section 5.2 tells the two apart by
     * whether abs_w/abs_h are zero, and there is no window here to measure. */
    ok_int(L"the minimum width is 1", z.min_w, 1);
    ok_int(L"the minimum height is 1", z.min_h, 1);
    ok_int(L"no measured width", z.abs_w, 0);
    ok_int(L"no measured height", z.abs_h, 0);

    /* Clamped inside the canvas: a zone hanging off the edge is one the user
     * cannot see, and therefore cannot correct. */
    ok(L"a drop in the top-left corner has a zone",
       sidebar_drop_zone(&area, p2(0, 64), 0.5f, 1.0f, &z));
    ok_near(L"pushed back to the left edge", z.x, 0.0f);
    ok(L"a drop in the bottom-right corner has a zone",
       sidebar_drop_zone(&area, p2(2559, 1439), 0.5f, 1.0f, &z));
    ok_near(L"pushed back from the right edge", z.x, 0.5f);
    ok_near(L"and the zone still fits", z.x + z.w, 1.0f);

    /* Bigger than the canvas: clamped to it, not refused. */
    ok(L"an oversized drop has a zone",
       sidebar_drop_zone(&area, p2(1280, 752), 2.0f, 2.0f, &z));
    ok_near(L"clamped to the full width", z.w, 1.0f);
    ok_near(L"clamped to the full height", z.h, 1.0f);
    ok_near(L"and to the origin", z.x, 0.0f);

    /* Degenerate input is refused rather than guessed at. */
    ok(L"NULL area refused", !sidebar_drop_zone(NULL, p2(0, 0), 0.5f, 0.5f, &z));
    ok(L"NULL output refused", !sidebar_drop_zone(&area, p2(0, 0), 0.5f, 0.5f, NULL));
    ok(L"a zero width refused", !sidebar_drop_zone(&area, p2(0, 0), 0.0f, 0.5f, &z));
    ok(L"a negative height refused",
       !sidebar_drop_zone(&area, p2(0, 0), 0.5f, -0.2f, &z));
    const RECT empty = r4(10, 10, 10, 10);
    ok(L"an empty canvas refused",
       !sidebar_drop_zone(&empty, p2(10, 10), 0.5f, 0.5f, &z));

    /* Every point of the screen produces a zone that fits inside it - the
     * property the clamp exists for, checked at the corners and the middle
     * rather than at one convenient sample. */
    const POINT probes[5] = { {0, 64}, {2559, 64}, {0, 1439}, {2559, 1439}, {1280, 752} };
    int fitted = 0;
    for (int i = 0; i < 5; ++i) {
        if (sidebar_drop_zone(&area, probes[i], 0.5f, 0.5f, &z) &&
            z.x >= -0.0001f && z.y >= -0.0001f &&
            z.x + z.w <= 1.0001f && z.y + z.h <= 1.0001f) {
            fitted++;
        }
    }
    ok_int(L"every probe lands inside the canvas", fitted, 5);
}

/* ------------------------------------------------------------------ */

static void test_handle(void)
{
    T_SEC(L"9. the handle: the signpost in the middle of the right edge");

    const RECT area = area_std();      /* (0,64)-(2560,1440): 1376 px tall */
    SidebarMetrics m;
    sidebar_metrics(96, &m);

    ok_int(L"the handle is 24 px wide", m.handle_w, 24);
    ok_int(L"and 96 px tall", m.handle_h, 96);
    ok(L"wider than the strip it sticks out of - that is the whole point",
       m.handle_w > m.edge_w);

    RECT h;
    ok(L"there is a handle", sidebar_handle_rect(&m, &area, &h));
    ok_int(L"against the right edge", h.right, 2560);
    ok_int(L"24 px wide", h.right - h.left, 24);
    ok_int(L"96 px tall", h.bottom - h.top, 96);

    /* Vertically centred: the middle of the edge is where a hand goes looking
     * for a drawer. */
    ok_int(L"centred vertically", (h.top + h.bottom) / 2,
           (area.top + area.bottom) / 2);

    /* It is a HIT TARGET as well as a signpost. */
    SidebarState st;
    sidebar_init(&st);
    ok_int(L"closed: the middle of the handle is the handle",
           (int)sidebar_hit(&st, &m, &area, 1000, p2(2556, 752)).kind,
           (int)SIDEBAR_HIT_HANDLE);
    ok_int(L"closed: its outer half is the handle too",
           (int)sidebar_hit(&st, &m, &area, 1000, p2(2540, 752)).kind,
           (int)SIDEBAR_HIT_HANDLE);
    ok_int(L"closed: one pixel outside it is the canvas",
           (int)sidebar_hit(&st, &m, &area, 1000, p2(2535, 752)).kind,
           (int)SIDEBAR_HIT_NONE);
    ok_int(L"closed: away from it, the strip still answers",
           (int)sidebar_hit(&st, &m, &area, 1000, p2(2556, 200)).kind,
           (int)SIDEBAR_HIT_EDGE);
    ok(L"the handle keeps the panel open, like the strip does",
       sidebar_hit_keeps_open(sidebar_hit(&st, &m, &area, 1000, p2(2540, 752))));

    /* Once the panel is out it slides OVER the handle - the signpost has done
     * its job, and the panel is its own signpost. */
    open_fully(&st, 0);
    ok_int(L"open: the handle's pixels belong to the panel",
           (int)sidebar_hit(&st, &m, &area, 1000, p2(2540, 752)).kind,
           (int)SIDEBAR_HIT_ROW);

    /* A screen shorter than the handle gets one that fits, rather than one
     * that runs off the top or the bottom. */
    const RECT tiny = r4(0, 64, 400, 124);      /* 60 px tall */
    ok(L"a short screen still has a handle", sidebar_handle_rect(&m, &tiny, &h));
    ok_int(L"clamped to the screen height", h.bottom - h.top, 60);
    ok_int(L"starting at the top of it", h.top, 64);
    ok_int(L"ending at the bottom of it", h.bottom, 124);

    ok(L"NULL area refused", !sidebar_handle_rect(&m, NULL, &h));
    ok(L"NULL output refused", !sidebar_handle_rect(&m, &area, NULL));
    ok(L"NULL metrics refused", !sidebar_handle_rect(NULL, &area, &h));
    const RECT empty = r4(10, 10, 10, 10);
    ok(L"an empty area refused", !sidebar_handle_rect(&m, &empty, &h));
}

/* ------------------------------------------------------------------ */

static void test_filter(void)
{
    T_SEC(L"10. the search box: what matches, where it is, what it covers");

    /* ---- the matching rule ---- */
    ok(L"an empty filter matches everything", sidebar_matches(L"Notepad", L""));
    ok(L"a NULL filter matches everything", sidebar_matches(L"Notepad", NULL));
    ok(L"a NULL name matches nothing", !sidebar_matches(NULL, L"pad"));
    ok(L"a substring matches", sidebar_matches(L"Notepad", L"pad"));
    ok(L"a prefix matches", sidebar_matches(L"Notepad", L"Note"));
    ok(L"the whole name matches", sidebar_matches(L"Notepad", L"Notepad"));
    ok(L"case is ignored one way", sidebar_matches(L"Notepad", L"notepad"));
    ok(L"and the other", sidebar_matches(L"notepad", L"NOTEPAD"));
    ok(L"a name that does not contain it does not match",
       !sidebar_matches(L"Notepad", L"chrome"));

    /* The two things this deliberately is NOT: an anchored match would find
     * nothing for "Code" in "Visual Studio Code", and a fuzzy one would make
     * the list jump around under a user typing a name they already know. */
    ok(L"it is not anchored at the start",
       sidebar_matches(L"Visual Studio Code", L"Code"));
    ok(L"and it is not fuzzy", !sidebar_matches(L"Visual Studio Code", L"vsc"));

    /* Chinese names are the reason the box is a real EDIT window - but the
     * matching has to work for them too. */
    ok(L"a Chinese name matches a Chinese filter",
       sidebar_matches(L"B5\u5bf9\u6218\u5e73\u53f0", L"\u5bf9\u6218"));
    ok(L"and does not match an unrelated one",
       !sidebar_matches(L"Notepad", L"\u5bf9\u6218"));

    /* ---- where the box is ---- */
    const RECT area = area_std();
    SidebarMetrics m;
    sidebar_metrics(96, &m);

    SidebarState st;
    sidebar_init(&st);

    RECT fr;
    ok(L"closed: there is no box to hit",
       !sidebar_filter_rect(&st, &m, &area, &fr));

    open_fully(&st, 0);
    ok(L"open: there is one", sidebar_filter_rect(&st, &m, &area, &fr));
    ok_int(L"inside the left padding", fr.left, 2248);
    ok_int(L"to the right padding", fr.right, 2552);
    ok_int(L"just under the header", fr.top, 106);
    ok_int(L"32 px tall", fr.bottom - fr.top, 32);

    /* It sits BETWEEN the header and the rows - that is what the extra row of
     * height bought, and the three must tile without overlapping. The header
     * used to be "everything above the rows", which silently made it two rows
     * tall once the box arrived: its caption was centred across both and the
     * box covered the lower half (the user reported it, 2026-10-10). */
    RECT head_r, rows_r;
    ok(L"the header has its own rectangle",
       sidebar_header_rect(&st, &m, &area, &head_r));
    ok_int(L"exactly one header tall", head_r.bottom - head_r.top, 34);
    ok_int(L"starting at the body's padding", head_r.top, 72);
    ok(L"the rows exist too", sidebar_rows_rect(&m, &area, &rows_r));
    ok_int(L"the search box starts where the header ends", fr.top, head_r.bottom);
    ok_int(L"the rows start exactly where the box ends", rows_r.top, fr.bottom);
    ok(L"and the three tile without overlapping",
       head_r.bottom == fr.top && fr.bottom == rows_r.top &&
       head_r.top < head_r.bottom && fr.top < fr.bottom);

    ok_int(L"the box is hittable",
           (int)sidebar_hit(&st, &m, &area, 1000, p2(2400, 120)).kind,
           (int)SIDEBAR_HIT_FILTER);
    ok_int(L"and it keeps the panel open",
           sidebar_hit_keeps_open(sidebar_hit(&st, &m, &area, 1000, p2(2400, 120))), 1);
    ok_int(L"above it is still the header",
           (int)sidebar_hit(&st, &m, &area, 1000, p2(2400, 80)).kind,
           (int)SIDEBAR_HIT_HEADER);
    ok_int(L"below it is the first row",
           (int)sidebar_hit(&st, &m, &area, 1000, p2(2400, 138)).kind,
           (int)SIDEBAR_HIT_ROW);

    /* Closed, the box is not there to hit: its pixels belong to the canvas. */
    SidebarState closed;
    sidebar_init(&closed);
    ok_int(L"closed: the box's pixels are the canvas",
           (int)sidebar_hit(&closed, &m, &area, 1000, p2(2400, 120)).kind,
           (int)SIDEBAR_HIT_NONE);
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t70_sidebar");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t70_sidebar - the edit overlay's side bar (geometry, state, hit testing)");
    T_KV(L"run_at", L"%ls", t_now_string());
    T_NOTE(L"No window is created and no message is pumped: sidebar.c calls no");
    T_NOTE(L"Win32 function, so a 1000-item list in a 42-row window, a pointer");
    T_NOTE(L"one pixel off the edge strip and a slide reversed halfway out are");
    T_NOTE(L"assertions here instead of surprises on someone else's monitor.");

    test_metrics();
    test_slide();
    test_geometry();
    test_scrolling();
    test_scrollbar();
    test_hit();
    test_drag();
    test_drop();
    test_handle();
    test_filter();

    T_SEC(L"Result");
    T_KV(L"passed", L"%d", g_pass);
    T_KV(L"failed", L"%d", g_fail);

    if (g_fail == 0) T_KV(L"VERDICT", L"PASS - all %d assertions hold", g_pass);
    else             T_KV(L"VERDICT", L"FAIL - %d assertion(s) broken", g_fail);

    if (!no_pause) t_pause();
    t_log_close();
    return (g_fail == 0) ? 0 : 1;
}
