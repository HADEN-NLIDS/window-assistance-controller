/*
 * t21_monitor_match.c - unit tests for the monitor matching cascade.
 *
 * These tests need NO monitor. Every case constructs the monitor set it wants
 * and hands it to monitor_match(), which is a pure function by design - see
 * monitor.h. That design decision is what makes this file possible on a
 * single-monitor development machine that cannot be reconfigured.
 *
 * Cases that would otherwise require unplugging hardware:
 *
 *   - the device name changed but it is the same monitor   -> level 2
 *   - the device name changed AND the EDID is unreadable    -> level 3
 *   - a monitor was removed                                 -> level 4
 *   - two identical monitors must not be confused           -> geometry position
 *   - a monitor left of the primary (negative origin)
 *   - the monitor order came back different
 *
 * The cases that genuinely need hardware - real unplug/replug field changes,
 * and mixed-DPI coordinate behaviour - are marked in docs/P0验证记录.md as
 * unverified rather than being faked here.
 *
 * Since 2026-10-10 it also covers monitor_selection_index(): the rule that
 * turns the overview's monitor-box selection into a monitor index, which the
 * [Move to the selected monitor] button acts on. Same reason it lives here -
 * it is pure, so it needs no window.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t21_monitor_match
 *   tests\out\t21_monitor_match.exe
 */

#include "test_common.h"

#include <math.h>
#include <string.h>

#include "../src/core/monitor.h"

static int g_pass, g_fail;

static void ok(const wchar_t *what, int cond)
{
    if (cond) { g_pass++; }
    else { g_fail++; t_out_line(L"  FAIL: %ls", what); }
}

static void ok_int(const wchar_t *what, int got, int want)
{
    if (got == want) { g_pass++; }
    else { g_fail++; t_out_line(L"  FAIL: %ls  (got %d, want %d)", what, got, want); }
}

static void ok_level(const wchar_t *what, MonitorMatchLevel got, MonitorMatchLevel want)
{
    if (got == want) { g_pass++; }
    else {
        g_fail++;
        t_out_line(L"  FAIL: %ls", what);
        t_out_line(L"        got  %ls", monitor_match_level_name(got));
        t_out_line(L"        want %ls", monitor_match_level_name(want));
    }
}

/* ------------------------------------------------------------------ */
/* Builders                                                            */
/* ------------------------------------------------------------------ */

/* A monitor described the way a stored workspace would record one. */
static MonitorFingerprint mon(const wchar_t *device, const wchar_t *friendly,
                              int w, int h, int vx, int vy, int primary)
{
    MonitorFingerprint m;
    ZeroMemory(&m, sizeof(m));

    wcsncpy_s(m.device_name, WAC_MON_NAME_MAX, device, _TRUNCATE);
    if (friendly) wcsncpy_s(m.friendly_name, WAC_MON_FRIENDLY_MAX, friendly, _TRUNCATE);

    m.width = w; m.height = h;
    m.virtual_x = vx; m.virtual_y = vy;
    m.is_primary = primary;
    m.dpi = 96;
    return m;
}

/* The common two-monitor setup: a 2560x1600 primary with a 1920x1080 to its
   right, exactly what the development machine would have if it had a second
   screen. */
static int build_two(MonitorFingerprint *set)
{
    set[0] = mon(L"\\\\.\\DISPLAY1", L"DELL U2720Q", 2560, 1600, 0, 0, 1);
    set[1] = mon(L"\\\\.\\DISPLAY2", L"LG HDR 4K",   1920, 1080, 2560, 0, 0);
    return 2;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_exact(void)
{
    T_SEC(L"1. Level 1: device name matches");

    MonitorFingerprint cur[WAC_MON_MAX];
    const int n = build_two(cur);

    int idx = -1;
    const MonitorFingerprint target = mon(L"\\\\.\\DISPLAY2", L"LG HDR 4K",
                                          1920, 1080, 2560, 0, 0);
    ok_level(L"second monitor found by device name",
             monitor_match(&target, cur, n, &idx), MONITOR_MATCH_EXACT);
    ok_int(L"index is 1", idx, 1);

    const MonitorFingerprint first = mon(L"\\\\.\\DISPLAY1", L"DELL U2720Q",
                                         2560, 1600, 0, 0, 1);
    ok_level(L"first monitor found", monitor_match(&first, cur, n, &idx), MONITOR_MATCH_EXACT);
    ok_int(L"index is 0", idx, 0);
}

static void test_name_after_rename(void)
{
    T_SEC(L"2. Level 2: device name changed, friendly name survived");
    T_NOTE(L"This is the 'driver reinstalled' or 'plugged into a different'");
    T_NOTE(L"port case: DISPLAY1/DISPLAY2 swap, the panels do not.");

    /* Two panels, and the stored target refers to the LG by a device name
       that no longer appears in the current set. Level 1 finds nothing, so
       the friendly name has to carry the match. */
    MonitorFingerprint cur[WAC_MON_MAX];
    const int n = build_two(cur);

    const MonitorFingerprint target = mon(L"\\\\.\\DISPLAY9", L"LG HDR 4K",
                                          1920, 1080, 2560, 0, 0);
    int idx = -1;
    ok_level(L"matched by friendly name, not device name",
             monitor_match(&target, cur, n, &idx), MONITOR_MATCH_NAME);
    ok_int(L"found the LG at index 1", idx, 1);

    /* And the same for the other panel, so the test cannot pass by always
       returning the first name match. */
    const MonitorFingerprint dell = mon(L"\\\\.\\DISPLAY8", L"DELL U2720Q",
                                        2560, 1600, 0, 0, 1);
    ok_level(L"the DELL is found by name too",
             monitor_match(&dell, cur, n, &idx), MONITOR_MATCH_NAME);
    ok_int(L"at index 0", idx, 0);

    /* A device name that IS present must still win over a friendly name that
       would have matched a different monitor - level 1 has priority. */
    const MonitorFingerprint by_dev = mon(L"\\\\.\\DISPLAY1", L"LG HDR 4K",
                                          1920, 1080, 2560, 0, 0);
    ok_level(L"device name takes priority over friendly name",
             monitor_match(&by_dev, cur, n, &idx), MONITOR_MATCH_EXACT);
    ok_int(L"and it wins, giving index 0", idx, 0);
}

static void test_geometry_fallback(void)
{
    T_SEC(L"3. Level 3: both names changed, geometry still matches");
    T_NOTE(L"Reached when the EDID cannot be read at all, so there is no");
    T_NOTE(L"friendly name to compare.");

    MonitorFingerprint cur[WAC_MON_MAX];
    cur[0] = mon(L"\\\\.\\DISPLAY1", L"", 2560, 1600, 0, 0, 1);
    cur[1] = mon(L"\\\\.\\DISPLAY2", L"", 1920, 1080, 2560, 0, 0);

    const MonitorFingerprint target = mon(L"\\\\.\\DISPLAY9", L"",
                                          1920, 1080, 2560, 0, 0);
    int idx = -1;
    ok_level(L"matched by geometry", monitor_match(&target, cur, 2, &idx),
             MONITOR_MATCH_GEOMETRY);
    ok_int(L"found the matching geometry at index 1", idx, 1);
}

static void test_empty_names_do_not_match(void)
{
    T_SEC(L"4. Two monitors with no EDID name must NOT match each other");
    T_NOTE(L"Without the emptiness check, every monitor whose EDID could not");
    T_NOTE(L"be read would match every other one, and windows would land on");
    T_NOTE(L"whichever monitor happened to enumerate first.");

    MonitorFingerprint cur[WAC_MON_MAX];
    cur[0] = mon(L"\\\\.\\DISPLAY1", L"", 2560, 1600, 0, 0, 1);
    cur[1] = mon(L"\\\\.\\DISPLAY2", L"", 1920, 1080, 2560, 0, 0);

    /* Same empty name, but a different size and position. It must fall past
       level 2 and land on geometry, not on the name. */
    const MonitorFingerprint target = mon(L"\\\\.\\DISPLAY9", L"",
                                          1920, 1080, 2560, 0, 0);
    int idx = -1;
    const MonitorMatchLevel level = monitor_match(&target, cur, 2, &idx);
    ok_level(L"empty names fall through to geometry", level, MONITOR_MATCH_GEOMETRY);
    ok_int(L"and land on the right monitor", idx, 1);
}

static void test_identical_monitors(void)
{
    T_SEC(L"5. Two identical monitors: geometry position disambiguates");
    T_NOTE(L"Same model, same resolution, different desk positions. Matching");
    T_NOTE(L"must use virtual_x/virtual_y, not resolution alone.");

    MonitorFingerprint cur[WAC_MON_MAX];
    cur[0] = mon(L"\\\\.\\DISPLAY1", L"DELL U2720Q", 2560, 1440, 0, 0, 1);
    cur[1] = mon(L"\\\\.\\DISPLAY2", L"DELL U2720Q", 2560, 1440, 2560, 0, 0);

    /* Stored against the right-hand one, found by position. */
    const MonitorFingerprint target = mon(L"\\\\.\\DISPLAYX", L"",
                                          2560, 1440, 2560, 0, 0);
    int idx = -1;
    ok_level(L"matched by geometry", monitor_match(&target, cur, 2, &idx),
             MONITOR_MATCH_GEOMETRY);
    ok_int(L"got the right-hand monitor, not the left", idx, 1);
}

static void test_orphaned(void)
{
    T_SEC(L"6. Level 4: the monitor is gone");

    /* Only the internal panel remains; the external one was unplugged. */
    MonitorFingerprint cur[WAC_MON_MAX];
    cur[0] = mon(L"\\\\.\\DISPLAY1", L"DELL U2720Q", 2560, 1600, 0, 0, 1);

    const MonitorFingerprint target = mon(L"\\\\.\\DISPLAY2", L"LG HDR 4K",
                                          1920, 1080, 2560, 0, 0);
    int idx = -1;
    ok_level(L"no match found", monitor_match(&target, cur, 1, &idx), MONITOR_MATCH_NONE);
    ok_int(L"the index is left as -1", idx, -1);
}

static void test_negative_origin(void)
{
    T_SEC(L"7. Monitor to the LEFT of the primary (negative origin)");
    T_NOTE(L"The primary sits at x=0; the second monitor is at x=-1920.");

    MonitorFingerprint cur[WAC_MON_MAX];
    cur[0] = mon(L"\\\\.\\DISPLAY1", L"Internal Panel", 2560, 1600, 0, 0, 1);
    cur[1] = mon(L"\\\\.\\DISPLAY2", L"Old VGA",        1920, 1080, -1920, 0, 0);

    const MonitorFingerprint target = mon(L"\\\\.\\DISPLAY2", L"Old VGA",
                                          1920, 1080, -1920, 0, 0);
    int idx = -1;
    ok_level(L"negative-origin monitor found", monitor_match(&target, cur, 2, &idx),
             MONITOR_MATCH_EXACT);
    ok_int(L"index is 1", idx, 1);

    /* Geometry matching must treat -1920 as a real position, not as "unset". */
    const MonitorFingerprint by_geom = mon(L"\\\\.\\DISPLAYZ", L"",
                                           1920, 1080, -1920, 0, 0);
    ok_level(L"negative origin matches by geometry",
             monitor_match(&by_geom, cur, 2, &idx), MONITOR_MATCH_GEOMETRY);
    ok_int(L"and finds the left-hand monitor", idx, 1);
}

static void test_above_primary(void)
{
    T_SEC(L"8. Monitor ABOVE the primary (negative y)");

    MonitorFingerprint cur[WAC_MON_MAX];
    cur[0] = mon(L"\\\\.\\DISPLAY1", L"Main",  2560, 1600, 0, 0, 1);
    cur[1] = mon(L"\\\\.\\DISPLAY2", L"Upper", 1920, 1080, 0, -1080, 0);

    const MonitorFingerprint target = mon(L"\\\\.\\DISPLAY2", L"Upper",
                                          1920, 1080, 0, -1080, 0);
    int idx = -1;
    ok_level(L"monitor above the primary found",
             monitor_match(&target, cur, 2, &idx), MONITOR_MATCH_EXACT);
    ok_int(L"index is 1", idx, 1);
}

static void test_order_changed(void)
{
    T_SEC(L"9. Enumeration order changed, panels unchanged");
    T_NOTE(L"The same two panels come back in the opposite order. Each panel");
    T_NOTE(L"keeps its own friendly name but gets a different device name, so");
    T_NOTE(L"level 1 fails for both and the names have to carry the match.");

    MonitorFingerprint before[WAC_MON_MAX];
    before[0] = mon(L"\\\\.\\DISPLAY1", L"DELL U2720Q", 2560, 1600, 0, 0, 1);
    before[1] = mon(L"\\\\.\\DISPLAY2", L"LG HDR 4K",   1920, 1080, 2560, 0, 0);

    /* Same two panels, re-enumerated under different device names: the LG
       is now DISPLAY3, the DELL is now DISPLAY4. Device names never repeat
       between the two sets, so level 1 cannot match and the friendly names
       are what has to carry it. */
    MonitorFingerprint after[WAC_MON_MAX];
    after[0] = mon(L"\\\\.\\DISPLAY3", L"LG HDR 4K",   1920, 1080, 0, 0, 1);
    after[1] = mon(L"\\\\.\\DISPLAY4", L"DELL U2720Q", 2560, 1600, 1920, 0, 0);

    int idx = -1;
    ok_level(L"the LG is found by name",
             monitor_match(&before[1], after, 2, &idx), MONITOR_MATCH_NAME);
    ok_int(L"at its new index 0", idx, 0);

    ok_level(L"the DELL is found by name",
             monitor_match(&before[0], after, 2, &idx), MONITOR_MATCH_NAME);
    ok_int(L"at its new index 1", idx, 1);

    /* The set comparison must also see this as "unchanged topology", because
       the panels are the same ones - re-laying-out the user's windows here
       would be pointless churn. */
    ok(L"the set is considered unchanged",
       monitor_set_equal(before, 2, after, 2));
}

static void test_set_equal(void)
{
    T_SEC(L"10. monitor_set_equal: has the topology actually changed?");

    MonitorFingerprint a[WAC_MON_MAX];
    const int na = build_two(a);

    /* Re-enumerated identically: equal. */
    MonitorFingerprint b[WAC_MON_MAX];
    const int nb = build_two(b);
    ok(L"identical sets are equal", monitor_set_equal(a, na, b, nb));

    /* A monitor disappeared: not equal. */
    ok(L"a missing monitor makes them unequal",
       !monitor_set_equal(a, na, b, 1));

    /* Same monitors, different device names: still equal, because the
       friendly names carry it - this is the case where re-laying-out windows
       would be pointless churn. */
    MonitorFingerprint c[WAC_MON_MAX];
    c[0] = mon(L"\\\\.\\DISPLAY3", L"DELL U2720Q", 2560, 1600, 0, 0, 1);
    c[1] = mon(L"\\\\.\\DISPLAY4", L"LG HDR 4K",   1920, 1080, 2560, 0, 0);
    ok(L"a device-name change alone keeps them equal", monitor_set_equal(a, na, c, 2));

    /* Only geometry in common: NOT equal. Its caller uses this answer to
       decide whether to re-lay-out the user's windows, and a guess is not
       good enough for that. */
    MonitorFingerprint d[WAC_MON_MAX];
    d[0] = mon(L"\\\\.\\DISPLAY7", L"", 2560, 1600, 0, 0, 1);
    d[1] = mon(L"\\\\.\\DISPLAY8", L"", 1920, 1080, 2560, 0, 0);
    ok(L"geometry-only agreement does NOT count as equal",
       !monitor_set_equal(a, na, d, 2));

    /* Empty sets. */
    ok(L"two empty sets are equal", monitor_set_equal(a, 0, b, 0));
    ok(L"NULL is not equal to anything", !monitor_set_equal(NULL, 0, b, 0));
}

static void test_at_point(void)
{
    T_SEC(L"11. monitor_at_point: which monitor is this coordinate on?");

    MonitorFingerprint set[WAC_MON_MAX];
    set[0] = mon(L"\\\\.\\DISPLAY1", L"Main", 2560, 1600, 0, 0, 1);
    set[1] = mon(L"\\\\.\\DISPLAY2", L"Left", 1920, 1080, -1920, 0, 0);
    const int n = 2;

    ok_int(L"a point on the primary",      monitor_at_point(set, n, 100, 100), 0);
    ok_int(L"the primary's far corner",    monitor_at_point(set, n, 2559, 1599), 0);
    ok_int(L"a point on the left monitor", monitor_at_point(set, n, -1000, 500), 1);
    ok_int(L"the left monitor's origin",   monitor_at_point(set, n, -1920, 0), 1);

    /* One past the right edge is NOT on the primary: the rectangle is
       half-open, which is what MonitorFromPoint does and what keeps a point
       on a shared border from belonging to two monitors. */
    ok_int(L"one past the right edge is not the primary",
           monitor_at_point(set, n, 2560, 100), -1);
    ok_int(L"a point in empty space", monitor_at_point(set, n, 9999, 9999), -1);
    ok_int(L"an empty set", monitor_at_point(set, 0, 0, 0), -1);
    ok_int(L"a NULL set", monitor_at_point(NULL, 2, 0, 0), -1);
}

static void test_primary(void)
{
    T_SEC(L"12. monitor_primary_index");

    MonitorFingerprint set[WAC_MON_MAX];
    set[0] = mon(L"\\\\.\\DISPLAY1", L"A", 1920, 1080, -1920, 0, 0);
    set[1] = mon(L"\\\\.\\DISPLAY2", L"B", 2560, 1600, 0, 0, 1);
    ok_int(L"finds the flagged primary", monitor_primary_index(set, 2), 1);

    /* No monitor flagged: the first is the best available answer, because
       callers need somewhere to put a window rather than -1. */
    MonitorFingerprint none[WAC_MON_MAX];
    none[0] = mon(L"\\\\.\\DISPLAY1", L"A", 1920, 1080, 0, 0, 0);
    ok_int(L"falls back to index 0", monitor_primary_index(none, 1), 0);

    ok_int(L"empty set returns -1", monitor_primary_index(none, 0), -1);
    ok_int(L"NULL returns -1", monitor_primary_index(NULL, 3), -1);
}

static void test_degenerate_inputs(void)
{
    T_SEC(L"13. Degenerate inputs must not crash");

    MonitorFingerprint cur[WAC_MON_MAX];
    const int n = build_two(cur);

    int idx = 12345;
    ok_level(L"NULL target", monitor_match(NULL, cur, n, &idx), MONITOR_MATCH_NONE);
    ok_int(L"index cleared", idx, -1);

    const MonitorFingerprint t = mon(L"\\\\.\\DISPLAY1", L"DELL U2720Q",
                                     2560, 1600, 0, 0, 1);
    ok_level(L"NULL set", monitor_match(&t, NULL, n, &idx), MONITOR_MATCH_NONE);
    ok_level(L"zero count", monitor_match(&t, cur, 0, &idx), MONITOR_MATCH_NONE);
    ok_level(L"negative count", monitor_match(&t, cur, -5, &idx), MONITOR_MATCH_NONE);

    /* NULL out_index must be accepted: callers that only want the level
       should not have to pass a dummy. */
    ok_level(L"NULL out_index is allowed",
             monitor_match(&t, cur, n, NULL), MONITOR_MATCH_EXACT);

    /* A target with an empty device name must not match an empty one, and
       must fall through to the other levels rather than matching index 0. */
    const MonitorFingerprint empty_dev = mon(L"", L"LG HDR 4K", 1920, 1080, 2560, 0, 0);
    ok_level(L"empty device name falls to level 2",
             monitor_match(&empty_dev, cur, n, &idx), MONITOR_MATCH_NAME);
    ok_int(L"and finds the LG", idx, 1);
}

static void test_same_name_preempts_geometry(void)
{
    T_SEC(L"14. A name shared by two panels PRE-EMPTS the geometry check");
    T_NOTE(L"Case 5 works because its target has NO name. Give the target a");
    T_NOTE(L"name that both panels report and the cascade stops at level 2:");
    T_NOTE(L"geometry is never consulted and the first same-named entry wins.");

    MonitorFingerprint cur[WAC_MON_MAX];
    cur[0] = mon(L"\\\\.\\DISPLAY1", L"DELL U2720Q", 2560, 1440, 0, 0, 1);
    cur[1] = mon(L"\\\\.\\DISPLAY2", L"DELL U2720Q", 2560, 1440, 2560, 0, 0);

    /* Really the right-hand panel, and its geometry says so - but it carries
       the name both panels report. */
    const MonitorFingerprint target = mon(L"\\\\.\\DISPLAYX", L"DELL U2720Q",
                                          2560, 1440, 2560, 0, 0);
    int idx = -1;
    ok_level(L"an ambiguous name still matches at level 2",
             monitor_match(&target, cur, 2, &idx), MONITOR_MATCH_NAME);
    ok_int(L"and lands on the FIRST same-named panel, not its own", idx, 0);

    /* The cost of a name that is wrong rather than missing: drop the name and
       the very same target is placed correctly, exactly as case 5 shows. */
    MonitorFingerprint unnamed = target;
    unnamed.friendly_name[0] = L'\0';
    ok_level(L"the same target with no name falls through to geometry",
             monitor_match(&unnamed, cur, 2, &idx), MONITOR_MATCH_GEOMETRY);
    ok_int(L"and is then placed correctly", idx, 1);

    T_NOTE(L"2026-10-08: this stopped being hypothetical. A lookup bug handed");
    T_NOTE(L"both real monitors the same name although they are different");
    T_NOTE(L"models, so every stored fingerprint matched whichever panel");
    T_NOTE(L"enumerated first. A wrong name is worse than a missing one.");
}

/*
 * 15. The monitor box's selection -> a monitor index.
 *
 * The overview's monitor box stores monitor_enumerate()'s index in each item's
 * data, and [Move to the selected monitor] acts on whatever that says. Every
 * way the box can fail to name a monitor has to answer -1 rather than a
 * default: the button promises "the selected monitor", and quietly placing the
 * window somewhere else is exactly the defect this rule removes (2026-10-10).
 *
 * This is pure for the same reason monitor_match() is: the caller supplies
 * what the box reported, so no window has to exist to test the decision.
 */
static void test_selection_index(void)
{
    T_SEC(L"15. The monitor box's selection -> monitor index");

    /* The normal case: row 1 of a two-monitor list carries index 1. */
    ok_int(L"a selected row carrying index 1 means monitor 1",
           monitor_selection_index(1, 1, 2), 1);
    ok_int(L"index 0 is a monitor too, not 'nothing selected'",
           monitor_selection_index(0, 0, 2), 0);

    /* The four ways there is nothing usable to act on. */
    ok_int(L"no selection (CB_ERR) -> -1",
           monitor_selection_index(-1, -1, 2), -1);
    ok_int(L"a selected row carrying no index -> -1",
           monitor_selection_index(0, -1, 2), -1);
    ok_int(L"an empty enumeration -> -1",
           monitor_selection_index(0, 0, 0), -1);
    ok_int(L"an index the enumeration no longer covers -> -1",
           monitor_selection_index(1, 2, 2), -1);

    /* The stale row is the interesting one: it is what "the monitor was
     * unplugged while the box still showed it" looks like, and it must not
     * fold onto monitor 0 - or onto the primary, which is where the button
     * used to send every window regardless of the selection. */
    ok_int(L"a stale row is not folded onto the first monitor",
           monitor_selection_index(1, 5, 2), -1);
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t21_monitor_match");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t21_monitor_match - monitor matching cascade");
    T_KV(L"run_at", L"%ls", t_now_string());
    T_NOTE(L"No monitor is required: every case constructs the monitor set it");
    T_NOTE(L"needs and calls monitor_match(), which is a pure function.");
    T_NOTE(L"This is what makes multi-monitor logic testable on a machine");
    T_NOTE(L"with one screen that cannot be reconfigured.");

    test_exact();
    test_name_after_rename();
    test_geometry_fallback();
    test_empty_names_do_not_match();
    test_identical_monitors();
    test_orphaned();
    test_negative_origin();
    test_above_primary();
    test_order_changed();
    test_set_equal();
    test_at_point();
    test_primary();
    test_degenerate_inputs();
    test_same_name_preempts_geometry();
    test_selection_index();

    T_SEC(L"Result");
    T_KV(L"passed", L"%d", g_pass);
    T_KV(L"failed", L"%d", g_fail);

    if (g_fail == 0) T_KV(L"VERDICT", L"PASS - all %d assertions hold", g_pass);
    else             T_KV(L"VERDICT", L"FAIL - %d assertion(s) broken", g_fail);

    if (!no_pause) t_pause();
    t_log_close();
    return (g_fail == 0) ? 0 : 1;
}
