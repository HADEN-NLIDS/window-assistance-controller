/*
 * t50_overlay.c - stage 6, task 6.1/6.2: the edit overlay window.
 *
 * WHAT THIS VERIFIES
 *
 *   Two things, and the second one is the reason this program exists rather
 *   than a manual look at the screen:
 *
 *   1. The overlay covers the whole rcMonitor of the chosen monitor, is
 *      translucent enough to see real windows through, and repaints correctly.
 *      An operator can judge that by eye.
 *
 *   2. Entering and leaving edit mode does not leak. Task 6.9 makes this a
 *      hard gate - "50 entries, GDI and USER handle growth <= 10, or stage 7
 *      does not start" - so it is measured here against the real window, not
 *      argued about in review. A leak of one brush per entry is invisible at
 *      entry 1 and fatal at entry 500, and this is the only place that would
 *      notice.
 *
 * HOW TO RUN
 *     t50_overlay.exe                      show it, wait for Esc/Ctrl+Q
 *     t50_overlay.exe --seconds 10         show it for 10 seconds
 *     t50_overlay.exe --monitor 1          the second monitor
 *     t50_overlay.exe --loop 50            create/destroy 50 times, report leaks
 *     t50_overlay.exe --loop 50 --no-show  the same, without taking focus
 *
 * WHAT TO RECORD
 *     GDI and USER deltas must both be <= 10 after the loop. Anything larger
 *     names a resource that release_resources() missed.
 *
 * BUILD
 *     tests\build_tests.cmd t50_overlay
 */

#include "test_common.h"

/*
 * Paths relative to /Isrc, the way every other test reaches into src.
 * build_tests.cmd puts src, src\core and src\model on the include path but not
 * src\ui - nothing under tests/ has needed a ui header before this file.
 */
#include "ui/overlay.h"
#include "ui/toolbar.h"     /* the name box geometry (stage 6 renaming) */
#include "ui/snapgrid.h"    /* SNAP_COUNT - what the layout panel holds */
#include "core/monitor.h"   /* MonitorFingerprint / monitor_enumerate / WAC_MON_MAX */

#define IDT_FUSE 1

static int g_seconds = 0;       /* 0 = wait for a key */
static int g_loops   = 0;       /* 0 = single interactive run */
static int g_monitor = 0;
static int g_no_show = 0;

static volatile int g_exit_seen = 0;
static OverlayExit  g_exit_why  = OVERLAY_EXIT_NONE;

static void on_overlay_exit(OverlayExit why, void *ctx)
{
    (void)ctx;
    g_exit_why  = why;
    g_exit_seen = 1;
}

/*
 * The name box geometry (stage 6: renaming lives inside edit mode).
 *
 * Pure arithmetic, so it is checked here rather than by looking at a screen -
 * and the properties that matter are the ones a screenshot cannot show:
 *
 *   1. the box is to the left of every button, and never overlaps them;
 *   2. the two buttons sit between the box and the buttons, in order;
 *   3. the two buttons do NOT move when the strip narrows - the BOX gives way
 *      instead. The design requires this: a text box that jumps under the
 *      cursor loses the click that was meant to open it.
 */
static void check_name_box(void)
{
    RECT wide = { 0, 0, 2560, TOOLBAR_HEIGHT };
    RECT narrow;
    ToolbarItem items[TB_COUNT];
    ToolbarItem items2[TB_COUNT];
    ToolbarNameBox box, box2;

    T_SEC(L"the name box (pure geometry)");

    const int n = toolbar_layout(&wide, 0xFFFFFFFFu, items, TB_COUNT);
    if (n != TB_COUNT) {
        T_KV(L"name_box", L"SKIP - toolbar_layout returned %d on a 2560px strip", n);
        return;
    }

    if (!toolbar_name_layout(&wide, items, n, &box)) {
        T_KV(L"name_box", L"FAIL - no name box on a 2560px strip");
        return;
    }

    T_KV(L"box", L"%ld..%ld  (width %ld)",
         box.name.left, box.name.right, box.name.right - box.name.left);
    T_KV(L"box < buttons", L"%ls",
         (box.name.right < items[0].rect.left) ? L"PASS" : L"FAIL");
    T_KV(L"box < save < cancel < buttons", L"%ls",
         (box.name.right <= box.save.left &&
          box.save.right  <= box.cancel.left &&
          box.cancel.right <= items[0].rect.left) ? L"PASS" : L"FAIL");
    T_KV(L"box and buttons share the strip's vertical band", L"%ls",
         (box.name.top == items[0].rect.top &&
          box.name.bottom == items[0].rect.bottom) ? L"PASS" : L"FAIL");

    /*
     * The row must reach the right edge - that is what right-alignment means.
     *
     * The margin is private to toolbar.c, so this is a bound rather than an
     * exact number, but it is tight enough to catch the failure that actually
     * happened: folding the name box's width into the number used for
     * right-alignment pushed the whole row 130 px off the right edge and left a
     * bare strip there. MEASURED 2026-09-23, found by the user from a
     * screenshot - the assertions above only ever checked that the parts do not
     * OVERLAP, never that the row reaches the edge.
     */
    {
        const int right_gap = (int)wide.right - items[n - 1].rect.right;
        T_KV(L"right edge gap", L"%d px -> %ls", right_gap,
             (right_gap >= 0 && right_gap <= 32) ? L"PASS" : L"FAIL");
    }

    /*
     * Narrower strip: the box must shrink, and the two buttons must not move
     * relative to the action buttons. That offset is what the user's click
     * lands on, so it is the thing that has to stay put.
     */
    narrow.left = 0; narrow.top = 0; narrow.right = 1900; narrow.bottom = TOOLBAR_HEIGHT;

    if (toolbar_layout(&narrow, 0xFFFFFFFFu, items2, TB_COUNT) != TB_COUNT) {
        T_KV(L"narrow strip", L"SKIP - toolbar did not fit at 1900px");
        return;
    }
    if (!toolbar_name_layout(&narrow, items2, TB_COUNT, &box2)) {
        T_KV(L"narrow strip", L"box did not fit at 1900px (still acceptable)");
        return;
    }

    T_KV(L"narrow box width", L"%ld  (was %ld)",
         box2.name.right - box2.name.left, box.name.right - box.name.left);
    T_KV(L"box shrank, buttons did not move", L"%ls",
         ((box2.name.right - box2.name.left) < (box.name.right - box.name.left) &&
          (items2[0].rect.left - box2.cancel.right) ==
          (items[0].rect.left  - box.cancel.right)) ? L"PASS" : L"FAIL");
}

/*
 * The geometry scales with the monitor's dpi.
 *
 * WHY THIS IS A TEST AND NOT A SCREENSHOT
 *
 *   MEASURED 2026-10-09 on the development pair: the same 22-px caption is
 *   5.1 mm tall on the 109-ppi desktop panel and 2.96 mm on the 189-ppi laptop
 *   panel - 1.7x smaller - while every other control on that screen grows 1.5x
 *   with its 150% scaling. The overlay therefore looked cramped on exactly the
 *   monitor whose UI is largest.
 *
 *   The scale is one number, and every length in ui/toolbar.c passes through
 *   toolbar_px(). These assertions check the conversion at both ends (96 and
 *   144 dpi) plus the fallback, so a future constant that forgets to scale is
 *   caught here rather than on someone's laptop.
 */
static void check_dpi_scaling(void)
{
    T_SEC(L"the geometry follows the monitor dpi");

    /* 96 dpi is the base: every earlier assertion in this file relies on it. */
    toolbar_set_dpi(96);
    int pw = 0, ph = 0;
    toolbar_panel_size(SNAP_COUNT, &pw, &ph);
    T_KV(L"96 dpi: strip / layout panel / monitor box",
         L"%d / %dx%d / %d",
         toolbar_strip_h(), pw, ph, toolbar_monitor_w());
    T_KV(L"96 dpi unchanged", L"%ls",
         (toolbar_strip_h() == 64 && pw == 280 && ph == 332 &&
          toolbar_monitor_w() == 220 && toolbar_monitor_h() == 28 &&
          toolbar_monitor_list_height(2) == 72) ? L"PASS" : L"FAIL");

    /* 150%: everything is 1.5x. */
    toolbar_set_dpi(144);
    toolbar_panel_size(SNAP_COUNT, &pw, &ph);
    T_KV(L"144 dpi: strip / layout panel / monitor box",
         L"%d / %dx%d / %dx%d",
         toolbar_strip_h(), pw, ph, toolbar_monitor_w(), toolbar_monitor_h());
    T_KV(L"144 dpi = 1.5x of the base", L"%ls",
         (toolbar_strip_h() == 96 && pw == 420 && ph == 498 &&
          toolbar_monitor_w() == 330 && toolbar_monitor_h() == 42 &&
          toolbar_monitor_list_height(2) == 108) ? L"PASS" : L"FAIL");

    /* The buttons as well: a 130-px button is 195 px at 150%. */
    T_KV(L"layout button width", L"%d  (130 at 96 dpi)", toolbar_button_width(TB_LAYOUT));
    T_KV(L"button width scaled", L"%ls",
         (toolbar_button_width(TB_LAYOUT) == 195 &&
          toolbar_button_width(TB_PICK) == 255) ? L"PASS" : L"FAIL");

    /* And the strip really lays out inside the taller bar. */
    {
        RECT strip144 = { 0, 0, 2560, toolbar_strip_h() };
        ToolbarItem items[TB_COUNT];
        const int n = toolbar_layout(&strip144, 0xFFFFFFFFu, items, TB_COUNT);
        const int right_gap = 2560 - (int)items[n - 1].rect.right;
        T_KV(L"144 dpi: buttons still reach the right edge", L"%d px -> %ls",
             right_gap, (right_gap >= 0 && right_gap <= 48) ? L"PASS" : L"FAIL");
    }

    /* A dpi nobody can report falls back to the base instead of scaling to
     * something absurd. */
    toolbar_set_dpi(0);
    T_KV(L"a bogus dpi falls back to 96", L"%d -> %ls",
         toolbar_dpi(), (toolbar_dpi() == 96 && toolbar_strip_h() == 64) ? L"PASS" : L"FAIL");

    /* Leave the module as the rest of the file expects to find it. */
    toolbar_set_dpi(96);
}

static DWORD gdi_now(void)
{
    return GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
}

static DWORD user_now(void)
{
    return GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
}

int wmain(int argc, wchar_t **argv)
{
    t_diag_init(L"t50_overlay");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t50_overlay - the edit overlay");
    T_KV(L"run_at", L"%ls", t_now_string());

    for (int i = 1; i < argc; ++i) {
        if      (!wcscmp(argv[i], L"--seconds") && i + 1 < argc) g_seconds = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--loop")    && i + 1 < argc) g_loops   = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--monitor") && i + 1 < argc) g_monitor = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--no-show")) g_no_show = 1;
        else if (!wcscmp(argv[i], L"--help")) {
            t_out_line(L"usage: t50_overlay.exe [--seconds N] [--monitor N]\n"
                       L"                       [--loop N] [--no-show]\n");
            return 0;
        }
    }

    t_set_dpi_awareness();

    /* Pure geometry first: it needs no window, and if the box would overlap a
     * button there is no point putting anything on screen. */
    check_name_box();
    check_dpi_scaling();

    HINSTANCE hinst = GetModuleHandleW(NULL);

    /*
     * Count the monitors and print what the overlay is about to cover, before
     * it covers it. If the numbers below are wrong, the overlay is wrong, and
     * the screen will not be readable enough to tell.
     */
    MonitorFingerprint mons[WAC_MON_MAX];
    const int mon_count = monitor_enumerate(mons, WAC_MON_MAX);
    T_KV(L"monitor_count", L"%d", mon_count);

    if (mon_count <= 0) {
        T_WARN(L"no monitors reported - nothing to cover");
        t_pause_if_console();
        return 1;
    }
    if (g_monitor < 0 || g_monitor >= mon_count) {
        T_WARN(L"--monitor %d does not exist (valid 0..%d)",
               g_monitor, mon_count - 1);
        t_pause_if_console();
        return 1;
    }

    RECT mon;
    ZeroMemory(&mon, sizeof(mon));
    if (!monitor_rect(mons, mon_count, g_monitor, &mon)) {
        T_WARN(L"monitor %d has no rectangle", g_monitor);
        t_pause_if_console();
        return 1;
    }

    T_SEC(L"Target");
    T_KV(L"monitor_index", L"%d", g_monitor);
    T_KV(L"cover_rect", L"%ld,%ld - %ld,%ld  (%ldx%ld, rcMonitor)",
         (long)mon.left, (long)mon.top, (long)mon.right, (long)mon.bottom,
         (long)(mon.right - mon.left), (long)(mon.bottom - mon.top));
    T_NOTE(L"The taskbar area must be inside that rectangle - the overlay");
    T_NOTE(L"covers rcMonitor, not rcWork.");

    /* ------------------------------------------------------------------ */
    /* Loop mode: the leak gate                                            */
    /* ------------------------------------------------------------------ */
    if (g_loops > 0) {
        T_SEC(L"Resource discipline");
        T_KV(L"loops", L"%d", g_loops);
        T_KV(L"show_each_time", L"%ls", g_no_show ? L"no" : L"yes");

        const DWORD gdi_before  = gdi_now();
        const DWORD user_before = user_now();
        T_KV(L"gdi_before", L"%lu", (unsigned long)gdi_before);
        T_KV(L"user_before", L"%lu", (unsigned long)user_before);

        int created = 0;
        int failed  = 0;

        for (int i = 0; i < g_loops; ++i) {
            g_exit_seen = 0;
            g_exit_why  = OVERLAY_EXIT_NONE;

            Overlay *o = overlay_create(hinst, g_monitor, on_overlay_exit, NULL);
            if (o == NULL) {
                failed++;
                T_WARN(L"overlay_create failed on iteration %d", i + 1);
                break;
            }
            created++;

            if (!g_no_show) {
                (void)overlay_show(o);
                /* Force a full paint so the drawing path is exercised too -
                 * a leak in the paint path would otherwise go unnoticed by a
                 * loop that only creates and destroys. */
                UpdateWindow(overlay_hwnd(o));
            }

            overlay_destroy(o);
        }

        /*
         * GetGuiResources is a process-wide count, so it also moves for
         * reasons this test does not control. That is why the gate is a small
         * tolerance rather than zero: the point is to catch growth that scales
         * with the number of entries.
         */
        const long gdi_delta  = (long)gdi_now()  - (long)gdi_before;
        const long user_delta = (long)user_now() - (long)user_before;

        T_SEC(L"Result");
        T_KV(L"overlays_created", L"%d", created);
        T_KV(L"overlays_failed", L"%d", failed);
        T_KV(L"module_gdi_still_held", L"%d  (must be 0)", overlay_gdi_objects());
        T_KV(L"module_created_total", L"%d", overlay_created_count());
        T_KV(L"module_destroyed_total", L"%d", overlay_destroyed_count());
        T_KV(L"gdi_after", L"%lu", (unsigned long)gdi_now());
        T_KV(L"gdi_delta", L"%ld  (gate: <= 10)", gdi_delta);
        T_KV(L"user_after", L"%lu", (unsigned long)user_now());
        T_KV(L"user_delta", L"%ld  (gate: <= 10)", user_delta);

        const BOOL ok = (overlay_gdi_objects() == 0 &&
                         gdi_delta <= 10 && user_delta <= 10 &&
                         created == g_loops && failed == 0);

        T_KV(L"verdict", L"%ls", ok ? L"PASS" : L"FAIL");
        if (!ok) {
            T_WARN(L"the overlay leaks, or could not be created %d time(s)",
                   failed);
            T_WARN(L"every resource must be created in build_resources() and");
            T_WARN(L"released in release_resources() - nothing else may hold");
            T_WARN(L"a GDI object past overlay_destroy().");
        }

        t_pause_if_console();
        t_log_close();
        return ok ? 0 : 1;
    }

    /* ------------------------------------------------------------------ */
    /* Interactive mode                                                    */
    /* ------------------------------------------------------------------ */
    T_SEC(L"Creating the overlay");
    Overlay *o = overlay_create(hinst, g_monitor, on_overlay_exit, NULL);
    if (o == NULL) {
        T_WARN(L"overlay_create returned NULL - see the log above for which");
        T_WARN(L"step refused, and note that this is a reportable outcome");
        T_WARN(L"rather than a crash: edit mode must not start with nothing");
        T_WARN(L"on screen.");
        t_pause_if_console();
        return 1;
    }
    T_KV(L"created", L"yes");

    t_report_foreground(L"foreground_before_show");
    (void)overlay_show(o);
    t_report_foreground(L"foreground_after_show");

    /*
     * Unlike t05, the overlay IS expected to be foreground here - it needs the
     * keyboard for Esc. Stated as a check rather than a hope, because the
     * opposite behaviour would be a silent failure: the window would be up,
     * the key would do nothing, and the log would look clean.
     */
    const wchar_t *fg = (GetForegroundWindow() == overlay_hwnd(o))
        ? L"yes - correct for edit mode (Esc needs it)"
        : L"no - Esc will NOT reach the overlay";
    T_KV(L"overlay_is_foreground", L"%ls", fg);

    T_SEC(L"Running");
    T_NOTE(L"The overlay covers monitor %d. Check that:", g_monitor);
    T_NOTE(L"  - real windows are still visible through the wash");
    T_NOTE(L"  - the taskbar area is covered too");
    T_NOTE(L"  - the border and quarter lines are crisp, not flickering");
    T_NOTE(L"");
    T_NOTE(L"Esc or Ctrl+Q leaves edit mode.%ls",
           g_seconds > 0 ? L" A timer will also end it." : L"");

    if (g_seconds > 0) {
        SetTimer(NULL, IDT_FUSE, (UINT)g_seconds * 1000u, NULL);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.hwnd == NULL &&
            msg.wParam == IDT_FUSE) {
            overlay_request_exit(o, OVERLAY_EXIT_FUSE);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (g_exit_seen) {
            break;
        }
    }

    if (g_seconds > 0) {
        KillTimer(NULL, IDT_FUSE);
    }

    T_SEC(L"Result");
    /*
     * Read the reason BEFORE destroying: overlay_destroy() frees the object,
     * and asking it afterwards would be reading freed memory. Same rule as
     * everything else here - the module owns the resource, the caller owns the
     * question, and the question has to be asked while the answer exists.
     */
    const OverlayExit why = overlay_exit_reason(o);
    T_KV(L"exit_reason", L"%ls", overlay_exit_name(why));
    T_KV(L"callback_saw", L"%ls", g_exit_seen
                                     ? overlay_exit_name(g_exit_why)
                                     : L"(never called)");

    overlay_destroy(o);

    T_KV(L"module_gdi_still_held", L"%d  (must be 0)", overlay_gdi_objects());
    t_report_foreground(L"foreground_after_destroy");

    T_SEC(L"Verdict");
    T_NOTE(L"1. exit_reason must name what you actually did, not 'none'.");
    T_NOTE(L"2. module_gdi_still_held must be 0 - the overlay must hand back");
    T_NOTE(L"   every object it created, on every exit path.");
    T_NOTE(L"3. The foreground must be back where it was before the overlay.");
    T_NOTE(L"4. Run --loop 50 and confirm both deltas stay within the gate.");

    t_pause_if_console();
    t_log_close();
    return (why != OVERLAY_EXIT_NONE) ? 0 : 1;
}
