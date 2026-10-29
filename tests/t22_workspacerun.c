/*
 * t22_workspacerun.c - the whole chain, on a real desktop.
 *
 * WHAT THIS PROVES THAT THE UNIT TESTS CANNOT
 *
 *   t18 checks that a slot and a preset combine into the right rule, and t19
 *   checks that a program starts and can be ended. Neither proves the thing
 *   the user actually asked for: "run this workspace" opens the program AND
 *   leaves its window where the preset says.
 *
 *   So this test builds a one-slot workspace in memory, pointing at Notepad,
 *   and runs it for real:
 *
 *     slot -> preset(notepad, left half) -> launch -> wait for the window
 *          -> place -> read the rectangle back
 *
 *   Notepad is the right subject: it is present on every Windows install, it
 *   is a packaged application whose window belongs to a different process than
 *   the one that was started (the reason core/launch.c exists at all), and it
 *   is harmless.
 *
 * IT CLEANS UP AFTER ITSELF
 *
 *   The window it opens is closed again through the LaunchGroup, and that the
 *   group really can end it is asserted rather than assumed. A test that
 *   leaves a stray Notepad on the user's desktop is a test people stop
 *   running.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t22_workspacerun
 *   tests\out\t22_workspacerun.exe --no-pause
 */

#include "test_common.h"

#include "../src/model/workspace.h"
#include "../src/model/workspace_run.h"
#include "../src/model/store.h"     /* the pin's round trip through the file */

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

/* WorkspaceSet is ~1.4 MB: static, never a stack frame (trap 11). */
static WorkspaceSet g_set;
static RuleSet      g_rules;

static void trace(void *ctx, const wchar_t *line)
{
    (void)ctx;
    t_out_line(L"    %ls", line);
}

/* Where Notepad lives on this machine, or NULL. */
static const wchar_t *find_notepad(wchar_t *buf, size_t cch)
{
    const wchar_t *candidates[] = {
        L"C:\\Windows\\System32\\notepad.exe",
        L"C:\\Windows\\notepad.exe",
        L"C:\\Windows\\SysWOW64\\notepad.exe"
    };

    for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); ++i) {
        const DWORD a = GetFileAttributesW(candidates[i]);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            wcsncpy_s(buf, cch, candidates[i], _TRUNCATE);
            return buf;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */

static void test_workspace_without_slots(void)
{
    T_SEC(L"a workspace with nothing in it");

    ws_clear(&g_set);
    Workspace *w = ws_add(&g_set, "empty", "Empty");
    ok(L"workspace created", w != NULL);

    const WorkspaceRunReport rep = workspace_run(w, &g_rules, NULL, NULL, NULL);
    ok(L"reports zero slots", rep.count == 0);
    ok(L"and counts nothing", rep.ok == 0 && rep.failed == 0);

    /* NULL arguments must not crash: the executor is called from a menu item
     * and from the command line, and both can be reached with no data. */
    const WorkspaceRunReport nul = workspace_run(NULL, &g_rules, NULL, NULL, NULL);
    ok(L"a NULL workspace reports nothing", nul.count == 0);
}

static void test_missing_preset_is_reported(void)
{
    T_SEC(L"a slot whose preset has gone away");

    ws_clear(&g_set);
    rules_clear(&g_rules);

    Workspace *w = ws_add(&g_set, "broken", "Broken");
    WorkspaceSlot *s = ws_slot_add(w);
    strncpy_s(s->preset_id, sizeof(s->preset_id), "nothere", _TRUNCATE);

    LaunchGroup group;
    launch_group_init(&group);

    const WorkspaceRunReport rep = workspace_run(w, &g_rules, &group, trace, NULL);

    ok(L"one slot reported", rep.count == 1);
    ok(L"it was skipped, not failed", rep.slots[0].result == RUN_SKIPPED);
    ok(L"the count agrees", rep.skipped == 1);
    ok(L"the note names the missing preset",
       wcsstr(rep.slots[0].note, L"nothere") != NULL);
    T_KV(L"note", L"%ls", rep.slots[0].note);

    launch_group_destroy(&group);
}

static void test_a_slot_with_no_executable(void)
{
    T_SEC(L"a slot that names nothing");

    ws_clear(&g_set);
    rules_clear(&g_rules);

    Workspace *w = ws_add(&g_set, "blank", "Blank");
    ws_slot_add(w);          /* defaults: no preset, no path */

    LaunchGroup group;
    launch_group_init(&group);

    const WorkspaceRunReport rep = workspace_run(w, &g_rules, &group, NULL, NULL);

    ok(L"one slot reported", rep.count == 1);
    ok(L"it was skipped", rep.slots[0].result == RUN_SKIPPED);
    T_KV(L"note", L"%ls", rep.slots[0].note);

    launch_group_destroy(&group);
}

/*
 * The real thing: start Notepad and put it on the left half.
 */
static void test_real_launch_and_place(void)
{
    T_SEC(L"notepad, started and placed on the left half");

    wchar_t exe[MAX_PATH];
    if (find_notepad(exe, MAX_PATH) == NULL) {
        ok(L"notepad.exe exists on this machine", 0);
        return;
    }
    T_KV(L"executable", L"%ls", exe);

    /* The main monitor's work area, for a target we can predict. */
    RECT work;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        ok(L"work area is readable", 0);
        return;
    }
    T_KV(L"work area", L"%ld,%ld %ldx%ld",
         (long)work.left, (long)work.top,
         (long)(work.right - work.left), (long)(work.bottom - work.top));

    ws_clear(&g_set);
    rules_clear(&g_rules);

    /* A preset saying "notepad, left half". */
    AppId app;
    ZeroMemory(&app, sizeof(app));
    wcsncpy_s(app.exe_name, WAC_EXE_MAX, L"notepad.exe", _TRUNCATE);
    wcsncpy_s(app.exe_path, WAC_EXE_MAX, exe, _TRUNCATE);

    RelRect zone;
    ZeroMemory(&zone, sizeof(zone));
    zone.x = 0.0f; zone.y = 0.0f;
    zone.w = 0.5f; zone.h = 1.0f;
    zone.min_w = 1; zone.min_h = 1;

    const int rule_index = rules_upsert(&g_rules, &app, &zone, L"notepad");
    ok(L"preset created", rule_index >= 0);
    T_KV(L"preset id", L"%hs", g_rules.items[rule_index].id);

    /* A workspace with one slot referring to it. */
    Workspace *w = ws_add(&g_set, "demo", "Demo");
    WorkspaceSlot *s = ws_slot_add(w);
    strncpy_s(s->preset_id, sizeof(s->preset_id),
              g_rules.items[rule_index].id, _TRUNCATE);
    s->wait_timeout_ms = 20000;

    LaunchGroup group;
    ok(L"launch group ready", launch_group_init(&group));

    const WorkspaceRunReport rep = workspace_run(w, &g_rules, &group, trace, NULL);

    ok_int(L"one slot", rep.count, 1);
    ok(L"it was placed", rep.slots[0].result == RUN_OK);
    T_KV(L"result", L"%ls - %ls",
         slot_run_result_name(rep.slots[0].result), rep.slots[0].note);

    if (rep.slots[0].hwnd != NULL) {
        RECT got;
        if (GetWindowRect(rep.slots[0].hwnd, &got)) {
            const int width  = (int)(got.right - got.left);
            const int height = (int)(got.bottom - got.top);
            T_KV(L"window rect", L"%ld,%ld %dx%d",
                 (long)got.left, (long)got.top, width, height);

            /* "Left half" of the work area. The height is checked loosely
             * because a packaged application may adjust it by a few pixels
             * for its own chrome. */
            const int want_w = (work.right - work.left) / 2;
            ok(L"it is at the left edge", got.left <= work.left + 4);
            ok(L"its width is about half the work area",
               width >= want_w - 16 && width <= want_w + 16);
        } else {
            ok(L"the window is still there to measure", 0);
        }
    }

    /* Clean up: the whole point of the LaunchGroup. */
    if (group.count > 0) {
        int failed = 0;
        const int ended = launch_group_terminate(&group, &failed);
        T_KV(L"cleanup", L"ended=%d failed=%d", ended, failed);
        ok(L"the notepad we opened was closed", ended >= 1 && failed == 0);
    } else {
        /* A shell launch may not hand over a process; then there is nothing to
         * close through the group, and the window is left for the user. Say so
         * rather than pretending the cleanup happened. */
        t_out_line(L"    note: no process handle was captured, nothing to close");
    }

    launch_group_destroy(&group);
}

/*
 * A window that refuses to be resized, and the preset that owns it.
 *
 * This is the decision the two buttons make when win_place_apply() reports
 * PLACE_MOVED_ONLY: "asking again will not help - remember it". Pure, so it
 * needs no window that actually refuses to resize; only a third-party program
 * provides one of those (2026-10-10, task B).
 */
static void test_lock_size_learning(void)
{
    T_SEC(L"learning that a window cannot be resized");

    static RuleSet set;   /* ~70 KB: never a stack frame (trap 11) */
    rules_clear(&set);

    AppId app;
    ZeroMemory(&app, sizeof(app));
    wcsncpy_s(app.exe_name, WAC_EXE_MAX, L"uu.exe", _TRUNCATE);

    RelRect zone;
    ZeroMemory(&zone, sizeof(zone));
    zone.w = 0.5f; zone.h = 0.5f;
    zone.min_w = 1; zone.min_h = 1;

    const int a = rules_upsert(&set, &app, &zone, L"uu");
    ok(L"a preset to learn on", a >= 0);

    wcsncpy_s(app.exe_name, WAC_EXE_MAX, L"other.exe", _TRUNCATE);
    const int b = rules_upsert(&set, &app, &zone, L"other");
    ok(L"a second preset, to prove the lesson stays local", b >= 0);

    if (a < 0 || b < 0) return;

    const char *id_a = set.items[a].id;
    T_KV(L"preset id", L"%hs", id_a);

    ok_int(L"it starts out resizable", (int)set.items[a].zone.lock_size, 0);
    ok_int(L"and so does the other one", (int)set.items[b].zone.lock_size, 0);

    /* The first PLACE_MOVED_ONLY is the call that learned it. */
    ok(L"the first sighting marks the preset", rule_lock_size(&set, id_a));
    ok_int(L"the flag is now set", (int)set.items[a].zone.lock_size, 1);

    /* Local: learning about one window says nothing about another. */
    ok_int(L"the other preset is untouched", (int)set.items[b].zone.lock_size, 0);

    /* Later sweeps must NOT report a change, or every [Apply all] would write
     * the config file again and repeat something the user has already read. */
    ok(L"a second sighting reports no change", !rule_lock_size(&set, id_a));
    ok(L"and a third one neither", !rule_lock_size(&set, id_a));

    /* Names that are not there, and arguments that are not names. */
    ok(L"an unknown preset id reports no change", !rule_lock_size(&set, "nosuch"));
    ok(L"an empty id reports no change", !rule_lock_size(&set, ""));
    ok(L"a NULL id reports no change", !rule_lock_size(&set, NULL));
}

/*
 * A slot that names a monitor sends its window THERE.
 *
 * This is what the manual has always promised ("the workspace decides which
 * screen"), and until 2026-10-10 it was not true: the recorded monitor was
 * only used to SKIP a slot whose panel was gone, while the placement asked
 * Windows where the window happened to be and applied the fractions there.
 * The user-visible result was a window that moved a little and stayed on the
 * wrong screen.
 *
 * Bound to each monitor in turn, so at least one of the two runs has to move
 * the window across screens - which is exactly the case that was broken. A
 * single-monitor machine cannot tell "go to THAT screen" from "stay where you
 * are", so it says so and skips rather than reporting a green that proves
 * nothing.
 */
static void test_slot_monitor_binding(void)
{
    T_SEC(L"a slot that names a monitor sends the window there");

    MonitorFingerprint mons[WAC_MON_MAX];
    const int mon_count = monitor_enumerate(mons, WAC_MON_MAX);
    T_KV(L"monitors", L"%d", mon_count);

    if (mon_count < 2) {
        T_NOTE(L"Only one monitor: 'go to THAT screen' cannot be told apart");
        T_NOTE(L"from 'stay where you are' here. Skipped, not faked.");
        return;
    }

    wchar_t exe[MAX_PATH];
    if (find_notepad(exe, MAX_PATH) == NULL) {
        ok(L"notepad.exe exists on this machine", 0);
        return;
    }

    for (int target = 0; target < 2; ++target) {
        RECT mon_rect;
        ZeroMemory(&mon_rect, sizeof(mon_rect));
        if (!monitor_rect(mons, mon_count, target, &mon_rect)) {
            ok(L"the named monitor has a rectangle", 0);
            continue;
        }

        T_KV(L"run bound to", L"[%d] %ls  %dx%d at %d,%d", target,
             (mons[target].friendly_name[0] != L'\0')
                 ? mons[target].friendly_name : mons[target].device_name,
             mons[target].width, mons[target].height,
             mons[target].virtual_x, mons[target].virtual_y);

        ws_clear(&g_set);
        rules_clear(&g_rules);

        AppId app;
        ZeroMemory(&app, sizeof(app));
        wcsncpy_s(app.exe_name, WAC_EXE_MAX, L"notepad.exe", _TRUNCATE);
        wcsncpy_s(app.exe_path, WAC_EXE_MAX, exe, _TRUNCATE);

        RelRect zone;
        ZeroMemory(&zone, sizeof(zone));
        zone.x = 0.0f; zone.y = 0.0f; zone.w = 0.5f; zone.h = 0.5f;
        zone.min_w = 1; zone.min_h = 1;

        const int ri = rules_upsert(&g_rules, &app, &zone, L"notepad");
        if (ri < 0) { ok(L"preset created", 0); continue; }

        Workspace *w = ws_add(&g_set, "bound", "Bound");
        if (w == NULL) { ok(L"workspace created", 0); continue; }

        WorkspaceSlot *s = ws_slot_add(w);
        if (s == NULL) { ok(L"slot created", 0); continue; }

        strncpy_s(s->preset_id, sizeof(s->preset_id),
                  g_rules.items[ri].id, _TRUNCATE);
        s->wait_timeout_ms = 20000;

        /* THE POINT: record a monitor. The window is sent there no matter
         * which screen Windows decides to open it on. */
        ws_slot_bind_monitor(s, &mons[target]);

        LaunchGroup group;
        if (!launch_group_init(&group)) { ok(L"launch group ready", 0); continue; }

        const WorkspaceRunReport rep = workspace_run(w, &g_rules, &group, trace, NULL);
        T_KV(L"result", L"%ls - %ls",
             slot_run_result_name(rep.slots[0].result), rep.slots[0].note);

        ok(L"the slot was placed", rep.slots[0].result == RUN_OK);

        if (rep.slots[0].hwnd != NULL) {
            RECT got;
            ZeroMemory(&got, sizeof(got));
            if (GetWindowRect(rep.slots[0].hwnd, &got)) {
                T_KV(L"window rect", L"%ld,%ld %dx%d",
                     (long)got.left, (long)got.top,
                     (int)(got.right - got.left), (int)(got.bottom - got.top));

                /* The centre decides, the same rule the placement itself uses:
                 * a window may straddle a border, but its centre is on exactly
                 * one monitor. */
                const int cx = (int)((got.left + got.right) / 2);
                const int cy = (int)((got.top + got.bottom) / 2);
                ok(L"the window is on the monitor the slot named",
                   monitor_at_point(mons, mon_count, cx, cy) == target);

                /* And placed at the fractions, relative to THAT monitor. */
                ok(L"its left edge is that monitor's left edge",
                   got.left >= mon_rect.left - 4 && got.left <= mon_rect.left + 4);
            } else {
                ok(L"the window is still there to measure", 0);
            }
        }

        if (group.count > 0) {
            int failed = 0;
            launch_group_terminate(&group, &failed);
            ok(L"the notepad we opened was closed", failed == 0);
        }
    }
}

/*
 * A preset that names a screen (2026-10-10).
 *
 * The decision itself is pure, so it is tested with constructed monitors: the
 * case that matters - "the screen I named is not connected" - needs no
 * hardware to produce, and the round trip through the file is what makes the
 * feature survive a restart rather than lasting one session.
 */
static void test_preset_pin(void)
{
    T_SEC(L"a preset that names a screen");

    MonitorFingerprint mons[WAC_MON_MAX];
    const int n = monitor_enumerate(mons, WAC_MON_MAX);
    T_KV(L"monitors", L"%d", n);
    if (n < 1) { ok(L"at least one monitor to pin to", 0); return; }

    static RuleSet set;   /* ~70 KB: static, never a stack frame (trap 11) */
    rules_clear(&set);

    AppId app;
    ZeroMemory(&app, sizeof(app));
    wcsncpy_s(app.exe_name, WAC_EXE_MAX, L"uu.exe", _TRUNCATE);

    RelRect zone;
    ZeroMemory(&zone, sizeof(zone));
    zone.w = 0.5f; zone.h = 0.5f;
    zone.min_w = 1; zone.min_h = 1;

    const int ri = rules_upsert(&set, &app, &zone, L"uu");
    ok(L"a preset to pin", ri >= 0);
    if (ri < 0) return;

    Rule *r = &set.items[ri];
    int idx = 123;

    ok(L"a new preset is not pinned", !rule_is_pinned(r));
    ok_int(L"resolving it answers NONE",
           (int)rule_pinned_monitor(r, mons, n, &idx), RULE_PIN_NONE);
    ok_int(L"and hands back no index", idx, -1);

    rule_bind_monitor(r, &mons[0]);
    ok(L"pinning sets the flag", rule_is_pinned(r));
    ok_int(L"resolving it finds that screen",
           (int)rule_pinned_monitor(r, mons, n, &idx), RULE_PIN_FOUND);
    ok_int(L"at its index", idx, 0);

    /* A screen that is not here. Constructed on purpose: the interesting case
     * is a configuration carried to another machine, or a panel unplugged -
     * and it must be reported, never quietly replaced by another screen. */
    MonitorFingerprint ghost;
    ZeroMemory(&ghost, sizeof(ghost));
    wcsncpy_s(ghost.device_name, WAC_MON_NAME_MAX, L"\\\\.\\DISPLAY99", _TRUNCATE);
    ghost.width = 1920; ghost.height = 1080;

    rule_bind_monitor(r, &ghost);
    ok_int(L"a screen that is not connected answers GONE",
           (int)rule_pinned_monitor(r, mons, n, &idx), RULE_PIN_GONE);
    ok_int(L"and still hands back no index", idx, -1);

    /* Unpinning is a real operation: the user has to be able to undo it. */
    rule_bind_monitor(r, NULL);
    ok(L"unpinning clears the flag", !rule_is_pinned(r));
    ok_int(L"and resolving answers NONE again",
           (int)rule_pinned_monitor(r, mons, n, &idx), RULE_PIN_NONE);

    /* Round trip through the file, or the feature would last one session. */
    rule_bind_monitor(r, &mons[0]);
    StoreDocument *doc = store_create();
    ok(L"a document to save into", doc != NULL);

    if (doc != NULL) {
        int round_trip_ok = 0;
        rules_to_store(&set, doc);

        static RuleSet back;
        rules_from_store(&back, doc);
        store_destroy(doc);

        ok_int(L"the reloaded set has the preset", rules_count(&back), 1);

        if (rules_count(&back) == 1) {
            const Rule *b = rules_at(&back, 0);
            ok(L"the pin survived the round trip", rule_is_pinned(b));
            ok_int(L"and still resolves to that screen",
                   (int)rule_pinned_monitor(b, mons, n, &idx), RULE_PIN_FOUND);
            ok_int(L"at its index", idx, 0);
            round_trip_ok = 1;
        }
        (void)round_trip_ok;
    }
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    /*
     * PerMonitorV2, like the product. Test programs are built WITHOUT the
     * product manifest (see tests\build_tests.cmd), so without this line every
     * monitor number this test reads back is DPI-VIRTUALISED: a 2560x1600 panel
     * at 150% reports 1707x1067, and the report stops matching the screen the
     * reader is looking at. It has to be set before anything asks about a
     * monitor or a window.
     */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t22_workspacerun");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t22_workspacerun - running a workspace");
    T_KV(L"run_at", L"%ls", t_now_string());

    test_workspace_without_slots();
    test_missing_preset_is_reported();
    test_a_slot_with_no_executable();
    test_real_launch_and_place();
    test_lock_size_learning();
    test_slot_monitor_binding();
    test_preset_pin();

    T_SEC(L"Result");
    T_KV(L"passed", L"%d", g_pass);
    T_KV(L"failed", L"%d", g_fail);

    if (g_fail == 0) {
        T_KV(L"VERDICT", L"PASS - all %d assertions hold", g_pass);
    } else {
        T_KV(L"VERDICT", L"FAIL - %d assertion(s) broken", g_fail);
    }

    if (!no_pause) t_pause();

    t_log_close();
    return (g_fail == 0) ? 0 : 1;
}
