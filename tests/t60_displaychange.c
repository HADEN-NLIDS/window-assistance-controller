/*
 * t60_displaychange.c - WM_DISPLAYCHANGE: the debounce, the diff, and the live
 *                       message.
 *
 * WHY IT IS SHAPED LIKE THIS
 *
 *   Two halves that answer two different questions.
 *
 *   1. THE PURE HALF. "Did the topology stop moving" and "what is different"
 *      are arithmetic, and the interesting cases are ones a desk cannot
 *      produce on demand: five messages inside 400 ms, a burst that keeps
 *      being extended, GetTickCount wrapping at 49.7 days, a monitor that is
 *      still the same panel under a new \\.\DISPLAYn, and a resolution change
 *      that is NOT a different monitor. Those are constructed here.
 *
 *   2. THE LIVE HALF (--probe <seconds>). Whether the system actually delivers
 *      WM_DISPLAYCHANGE to wac's window is not arithmetic and cannot be
 *      asserted from a table - docs/手册.md appendix B.1 has carried it as an
 *      untested item since stage 1 ("the moment WM_DISPLAYCHANGE really
 *      fires"). The probe creates TWO windows in this process:
 *
 *        a message-only window   HWND_MESSAGE
 *        a hidden top-level one  WS_POPUP + WS_EX_TOOLWINDOW, never shown
 *
 *      and reports which of them receives the message. That difference is the
 *      reason wac's own message window is top-level (main.c): a message-only
 *      window receives no broadcasts at all.
 *
 *      Send it one by hand - no display change needed:
 *
 *        # powershell, HWND_BROADCAST = 0xFFFF
 *        $s = Add-Type -PassThru -Name B -Namespace W -MemberDefinition @'
 *        [DllImport("user32.dll")] public static extern IntPtr SendMessageW(
 *            IntPtr h, uint m, IntPtr w, IntPtr l);
 *        '@
 *        [void]$s::SendMessageW([IntPtr]0xFFFF, 0x007E, [IntPtr]32,
 *                               [IntPtr](1920 -bor (1080 -shl 16)))
 *
 *      Or change a resolution / unplug a monitor and watch the same lines.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t60_displaychange
 *   tests\out\t60_displaychange.exe --no-pause
 *   tests\out\t60_displaychange.exe --probe 30 --no-pause
 */

#include "test_common.h"

#include "../src/core/displaywatch.h"

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

/* ------------------------------------------------------------------ */
/* A monitor set to build cases from                                  */
/* ------------------------------------------------------------------ */

/*
 * The two monitors of the development machine, as measured: an external HDMI
 * panel (primary, 100%) and the laptop panel (150%). Using the real names is
 * not decoration - the rename case below is the measured DISPLAY6 -> DISPLAY10
 * that disabling a display adapter produces, and match-on-EDID is what has to
 * survive it.
 */
static MonitorFingerprint mon(const wchar_t *dev, const wchar_t *edid,
                              int w, int h, int x, int y)
{
    MonitorFingerprint m;
    ZeroMemory(&m, sizeof(m));

    wcsncpy_s(m.device_name,   WAC_MON_NAME_MAX,     dev,  _TRUNCATE);
    wcsncpy_s(m.friendly_name, WAC_MON_FRIENDLY_MAX, edid, _TRUNCATE);
    m.width     = w;
    m.height    = h;
    m.virtual_x = x;
    m.virtual_y = y;
    return m;
}

/* ------------------------------------------------------------------ */
/* 1. the debounce                                                     */
/* ------------------------------------------------------------------ */

static void test_debounce(void)
{
    T_SEC(L"1. the debounce: 500 ms of QUIET, not 500 ms after the first");

    DisplayWatch dw;

    displaywatch_init(&dw);
    ok(L"a fresh watch is not pending", !dw.pending);
    ok(L"a fresh watch is not due", !displaywatch_due(&dw, 1000));
    ok_int(L"and waits for nothing", (int)displaywatch_ms_until_due(&dw, 1000), 0);
    ok(L"taking without a change reports nothing", !displaywatch_take(&dw));

    /* One message: due exactly at the boundary, not one tick before. */
    displaywatch_note_change(&dw, 1000);
    ok(L"a change is pending", dw.pending);
    ok_int(L"one message so far", dw.messages, 1);
    ok(L"299 ms later: not due", !displaywatch_due(&dw, 1299));
    ok_int(L"and it reports the time left", (int)displaywatch_ms_until_due(&dw, 1299), 201);
    ok(L"499 ms later: not due", !displaywatch_due(&dw, 1499));
    ok(L"500 ms later: due", displaywatch_due(&dw, 1500));
    ok_int(L"with nothing left to wait for", (int)displaywatch_ms_until_due(&dw, 1500), 0);
    ok(L"still due later, until it is taken", displaywatch_due(&dw, 9000));

    ok(L"take consumes it", displaywatch_take(&dw));
    ok(L"a second take reports nothing", !displaywatch_take(&dw));
    ok_int(L"the burst counter is cleared", dw.messages, 0);
    ok_int(L"and one settled pass is on record", dw.settled, 1);

    /* A burst: each message EXTENDS the quiet period. This is the case a driver
     * reset produces, and the reason the timer is re-armed with
     * ms_until_due() instead of being left alone. */
    displaywatch_init(&dw);
    displaywatch_note_change(&dw, 1000);
    displaywatch_note_change(&dw, 1100);
    displaywatch_note_change(&dw, 1200);
    ok_int(L"three messages, one pending change", dw.messages, 3);
    ok(L"1699 ms: still not due (the last one was at 1200)",
       !displaywatch_due(&dw, 1699));
    ok_int(L"and the remaining wait is measured from the LAST message",
           (int)displaywatch_ms_until_due(&dw, 1300), 400);
    ok(L"1700 ms: due", displaywatch_due(&dw, 1700));

    /* A message that arrives after the wait expired but before the pass ran
     * restarts it - the pass must describe a topology that has stopped moving. */
    displaywatch_note_change(&dw, 5000);
    ok(L"a late message re-arms the wait", !displaywatch_due(&dw, 5400));
    ok(L"and it is due 500 ms after THAT one", displaywatch_due(&dw, 5500));
    ok_int(L"with four messages in the burst now", dw.messages, 4);

    /*
     * GetTickCount wraps after 49.7 days of uptime. The comparison is an
     * unsigned difference precisely so that a wrap is not read as "50 days
     * since the last change" - which would fire instantly and forever.
     */
    displaywatch_init(&dw);
    displaywatch_note_change(&dw, 0xFFFFFF9Bu);   /* 100 ms before the wrap */
    ok(L"499 ms across the wrap: not due", !displaywatch_due(&dw, 0x0000018Eu));
    ok(L"500 ms across the wrap: due", displaywatch_due(&dw, 0x0000018Fu));

    /* Nothing here may crash on a watch that was never initialised. */
    displaywatch_init(NULL);
    displaywatch_note_change(NULL, 1);
    ok(L"NULL watch is never due", !displaywatch_due(NULL, 1));
    ok_int(L"NULL watch waits forever", (int)displaywatch_ms_until_due(NULL, 1), 0);
    ok(L"NULL watch has nothing to take", !displaywatch_take(NULL));
}

/* ------------------------------------------------------------------ */
/* 2. the diff                                                         */
/* ------------------------------------------------------------------ */

static void test_diff(void)
{
    T_SEC(L"2. the diff: what changed, and what only looks like a change");

    const MonitorFingerprint ext = mon(L"\\\\.\\DISPLAY6", L"H27T22X",
                                       2560, 1440, 0, 0);
    const MonitorFingerprint lap = mon(L"\\\\.\\DISPLAY1", L"B160QAN02.7",
                                       2560, 1600, 2560, 0);

    DisplayDiff d;

    /* The same topology twice: this is what most WM_DISPLAYCHANGE messages
     * turn out to be, and reacting to them would re-evaluate every slot for
     * nothing. */
    {
        const MonitorFingerprint set[2] = { ext, lap };
        ok(L"a diff is computed", displaywatch_diff(set, 2, set, 2, &d));
        ok(L"an unchanged topology reports same", d.same);
        ok_int(L"nothing removed", d.removed, 0);
        ok_int(L"nothing added", d.added, 0);
        ok_int(L"nothing renamed", d.renamed, 0);
        ok_int(L"nothing moved", d.geometry_only, 0);
        ok_int(L"and both counts are reported", d.old_count * 10 + d.new_count, 22);
    }

    /* A monitor went away: the laptop was closed or unplugged. */
    {
        const MonitorFingerprint before[2] = { ext, lap };
        const MonitorFingerprint after[1]  = { ext };
        ok(L"a removal is computed", displaywatch_diff(before, 2, after, 1, &d));
        ok(L"it is a change", !d.same);
        ok_int(L"one removed", d.removed, 1);
        ok_int(L"none added", d.added, 0);
        ok_int(L"one monitor left", d.new_count, 1);
    }

    /* A monitor appeared. */
    {
        const MonitorFingerprint before[1] = { ext };
        const MonitorFingerprint after[2]  = { ext, lap };
        ok(L"an addition is computed", displaywatch_diff(before, 1, after, 2, &d));
        ok(L"it is a change", !d.same);
        ok_int(L"one added", d.added, 1);
        ok_int(L"none removed", d.removed, 0);
    }

    /* One out, one in: both counters, which one pass could not produce. */
    {
        const MonitorFingerprint before[2] = { ext, lap };
        const MonitorFingerprint after[2]  = { ext, mon(L"\\\\.\\DISPLAY9",
                                                       L"DELL2408", 1920, 1200, 2560, 0) };
        ok(L"a swap is computed", displaywatch_diff(before, 2, after, 2, &d));
        ok_int(L"one removed", d.removed, 1);
        ok_int(L"and one added", d.added, 1);
        ok_int(L"the count did not change", d.new_count, 2);
    }

    /*
     * THE MEASURED CASE (docs/手册.md section 8.10): disabling and re-enabling
     * a display adapter hands the same panel a new \\.\DISPLAYn - measured
     * DISPLAY6 -> DISPLAY10 - while the EDID name stays put.
     */
    {
        const MonitorFingerprint after[1] = { mon(L"\\\\.\\DISPLAY10", L"H27T22X",
                                                  2560, 1440, 0, 0) };
        ok(L"a rename is computed", displaywatch_diff(&ext, 1, after, 1, &d));
        ok_int(L"it is a rename, not a replacement", d.renamed, 1);
        ok_int(L"nothing removed", d.removed, 0);
        ok_int(L"nothing added", d.added, 0);
        ok_int(L"and the geometry is untouched", d.geometry_only, 0);
        ok(L"it still counts as a change", !d.same);
    }

    /* A resolution change: same panel, same port, different numbers. */
    {
        const MonitorFingerprint after[1] = { mon(L"\\\\.\\DISPLAY6", L"H27T22X",
                                                  1920, 1080, 0, 0) };
        ok(L"a resolution change is computed", displaywatch_diff(&ext, 1, after, 1, &d));
        ok_int(L"it is reported as geometry", d.geometry_only, 1);
        ok_int(L"not as a rename", d.renamed, 0);
        ok_int(L"and not as a removal", d.removed, 0);
        ok(L"it counts as a change", !d.same);
    }

    /* A pure re-lay-out: the same two panels, the primary swapped, so both
     * origins move. Both are "geometry" - and the slots on them must be
     * re-evaluated even though no monitor came or went. */
    {
        const MonitorFingerprint before[2] = { ext, lap };
        const MonitorFingerprint after[2]  = {
            mon(L"\\\\.\\DISPLAY6", L"H27T22X",      2560, 1440, 2560, 0),
            mon(L"\\\\.\\DISPLAY1", L"B160QAN02.7", 2560, 1600, 0, 0)
        };
        ok(L"a re-lay-out is computed", displaywatch_diff(before, 2, after, 2, &d));
        ok_int(L"both panels moved", d.geometry_only, 2);
        ok_int(L"none came or went", d.removed + d.added, 0);
        ok(L"it counts as a change", !d.same);
    }

    /* Two monitors whose EDID could not be read must NOT be paired up. */
    {
        const MonitorFingerprint a[1] = { mon(L"\\\\.\\DISPLAY7", L"", 2560, 1440, 0, 0) };
        const MonitorFingerprint b[1] = { mon(L"\\\\.\\DISPLAY8", L"", 2560, 1440, 0, 0) };
        ok(L"empty names are compared", displaywatch_diff(a, 1, b, 1, &d));
        ok_int(L"a different device name removes", d.removed, 1);
        ok_int(L"and adds", d.added, 1);
        ok_int(L"it is NOT reported as a rename", d.renamed, 0);
    }

    /* Zero monitors: both empty is "nothing changed", not "everything". */
    {
        ok(L"an empty pair is computed", displaywatch_diff(NULL, 0, NULL, 0, &d));
        ok(L"and reports no change", d.same);
        ok_int(L"with no monitors on either side", d.old_count + d.new_count, 0);
    }

    /* Degenerate input is refused rather than guessed at. */
    ok(L"NULL output refused", !displaywatch_diff(&ext, 1, &ext, 1, NULL));
    ok(L"a negative count refused", !displaywatch_diff(&ext, -1, &ext, 1, &d));
    ok(L"a missing set refused", !displaywatch_diff(NULL, 1, &ext, 1, &d));
    ok(L"a missing new set refused", !displaywatch_diff(&ext, 1, NULL, 1, &d));
}

/* ------------------------------------------------------------------ */
/* 3. the grace period                                                 */
/* ------------------------------------------------------------------ */

static void test_grace(void)
{
    T_SEC(L"3. the grace period: 60 s for a broken topology to come back");

    DisplayGrace g;

    displaywatch_grace_init(&g);
    ok(L"a fresh grace is not armed", !displaywatch_grace_armed(&g));
    ok(L"a fresh grace is not due", !displaywatch_grace_due(&g, 1000));
    ok_int(L"and has nothing left to wait for",
           (int)displaywatch_grace_ms_left(&g, 1000), 0);
    ok(L"clearing without arming reports nothing", !displaywatch_grace_clear(&g));
    ok(L"expiring without arming reports nothing", !displaywatch_grace_expire(&g));

    /* Armed: the deadline is 60 s from the FIRST broken topology, and it does
     * not move while the topology stays broken. */
    displaywatch_grace_arm(&g, 1000, 2);
    ok(L"arming arms", displaywatch_grace_armed(&g));
    ok_int(L"and records how many slots are unusable", g.orphans, 2);
    ok(L"59.999 s later: still waiting", !displaywatch_grace_due(&g, 60999));
    ok_int(L"with 1 ms left", (int)displaywatch_grace_ms_left(&g, 60999), 1);
    ok(L"60 s: the wait is over", displaywatch_grace_due(&g, 61000));
    ok_int(L"and nothing is left", (int)displaywatch_grace_ms_left(&g, 61000), 0);

    /* A second arming must NOT move the deadline: a monitor that flaps is one
     * outage as far as the user is concerned. Only the count is refreshed. */
    displaywatch_grace_arm(&g, 30000, 3);
    ok_int(L"re-arming does not move the deadline", (int)g.armed_ms, 1000);
    ok_int(L"but the count is the current one", g.orphans, 3);
    ok(L"so 61 s after the FIRST break it is still due", displaywatch_grace_due(&g, 61000));

    /* It came back in time. */
    ok(L"clearing an armed wait reports the recovery", displaywatch_grace_clear(&g));
    ok(L"and disarms it", !displaywatch_grace_armed(&g));
    ok_int(L"one recovery on record", g.recovered, 1);
    ok(L"clearing twice reports nothing the second time", !displaywatch_grace_clear(&g));

    /* It did not come back: the deadline expired, and the two outcomes are
     * counted separately because the log has to tell them apart. */
    displaywatch_grace_arm(&g, 100000, 1);
    ok(L"expiring an armed wait reports the expiry", displaywatch_grace_expire(&g));
    ok(L"and disarms it", !displaywatch_grace_armed(&g));
    ok_int(L"one expiry on record", g.expired, 1);
    ok_int(L"and it was not counted as a recovery", g.recovered, 1);
    ok(L"expiring twice reports nothing the second time", !displaywatch_grace_expire(&g));

    /* A recovery AFTER the wait expired is a normal topology change: a new
     * outage gets a fresh deadline, which is what makes "the monitor came back
     * a minute later" end up usable again (the acceptance list asks for it). */
    displaywatch_grace_arm(&g, 200000, 1);
    ok_int(L"a new outage gets its own deadline", (int)g.armed_ms, 200000);
    ok_int(L"with the full window ahead",
           (int)displaywatch_grace_ms_left(&g, 200000), DISPLAYWATCH_GRACE_MS);
    ok_int(L"elapsed starts at zero", (int)displaywatch_grace_elapsed_ms(&g, 200000), 0);
    ok_int(L"and is reported while waiting",
           (int)displaywatch_grace_elapsed_ms(&g, 215000), 15000);

    /* GetTickCount wraps at 49.7 days here too. */
    displaywatch_grace_init(&g);
    displaywatch_grace_arm(&g, 0xFFFFFF9Bu, 1);   /* 100 ms before the wrap */
    ok(L"59999 ms across the wrap: still waiting",
       !displaywatch_grace_due(&g, 0xFFFFFF9Bu + DISPLAYWATCH_GRACE_MS - 1));
    ok(L"60000 ms across the wrap: due",
       displaywatch_grace_due(&g, 0xFFFFFF9Bu + DISPLAYWATCH_GRACE_MS));

    /* Nothing here may crash on a state that was never initialised. */
    displaywatch_grace_init(NULL);
    displaywatch_grace_arm(NULL, 1, 1);
    ok(L"NULL grace is not armed", !displaywatch_grace_armed(NULL));
    ok(L"NULL grace is not due", !displaywatch_grace_due(NULL, 1));
    ok_int(L"NULL grace has nothing left", (int)displaywatch_grace_ms_left(NULL, 1), 0);
    ok_int(L"NULL grace has no elapsed time",
           (int)displaywatch_grace_elapsed_ms(NULL, 1), 0);
    ok(L"NULL grace cannot be cleared", !displaywatch_grace_clear(NULL));
    ok(L"NULL grace cannot expire", !displaywatch_grace_expire(NULL));
}

/* ------------------------------------------------------------------ */
/* 4. the live message                                                 */
/* ------------------------------------------------------------------ */

#define PROBE_CLASS L"WacT60ProbeWindow"

/* Which of the two probe windows a message arrived at. Stored per window
 * (GWLP_USERDATA), not in a global: one proc serves both, and a global read at
 * dispatch time labels BOTH of them with whichever value was assigned last -
 * which is exactly how the first version of this probe reported "message-only"
 * for a message that had reached the top-level window. */
#define PROBE_TAG_TOPLEVEL 0
#define PROBE_TAG_MSGONLY  1

static int g_seen_top;   /* broadcasts that reached the top-level window */
static int g_seen_msg;   /* ...and the message-only window               */
static DWORD g_t0;

static LRESULT CALLBACK probe_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DISPLAYCHANGE) {
        const LONG_PTR tag = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        const DWORD    dt  = GetTickCount() - g_t0;

        if (tag == PROBE_TAG_TOPLEVEL) g_seen_top++; else g_seen_msg++;

        T_KV(L"  WM_DISPLAYCHANGE", L"%ls window (0x%p), +%lu ms, %ux%u, %u bpp",
             (tag == PROBE_TAG_TOPLEVEL) ? L"top-level" : L"message-only",
             hwnd, (unsigned long)dt,
             (unsigned)LOWORD(lp), (unsigned)HIWORD(lp), (unsigned)wp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/*
 * The probe: two windows, one broadcast, and the answer to "which one can hear
 * it". Windows never sends anything by itself here - the caller does, either by
 * hand (the PowerShell line in the file header) or by changing a resolution.
 *
 * SEND IT WITH SendMessageTimeout, NOT SendMessage. Measured 2026-10-09: a
 * plain SendMessage(HWND_BROADCAST, ...) from a console process never returned.
 * A broadcast waits for every top-level window in the session, and the sender's
 * own console window is one of them - the classic console/broadcast deadlock.
 * The timeout is what makes the probe finish.
 */
static void test_live_probe(int seconds)
{
    T_SEC(L"4. the live message: which window can hear a broadcast");

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = probe_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = PROBE_CLASS;

    if (RegisterClassExW(&wc) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        T_KV(L"probe", L"SKIP - RegisterClassExW failed (%lu)", GetLastError());
        return;
    }

    HWND top = CreateWindowExW(WS_EX_TOOLWINDOW, PROBE_CLASS, L"", WS_POPUP,
                               0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    HWND msgonly = CreateWindowExW(0, PROBE_CLASS, L"", 0, 0, 0, 0, 0,
                                   HWND_MESSAGE, NULL, wc.hInstance, NULL);

    if (top != NULL)     SetWindowLongPtrW(top,     GWLP_USERDATA, PROBE_TAG_TOPLEVEL);
    if (msgonly != NULL) SetWindowLongPtrW(msgonly, GWLP_USERDATA, PROBE_TAG_MSGONLY);

    T_KV(L"windows", L"top-level=0x%p  message-only=0x%p", top, msgonly);
    if (top == NULL || msgonly == NULL) {
        T_KV(L"probe", L"SKIP - window creation failed (%lu)", GetLastError());
        if (top != NULL)     DestroyWindow(top);
        if (msgonly != NULL) DestroyWindow(msgonly);
        return;
    }

    /*
     * The second documented difference, and the one that had a real
     * consequence: FindWindow cannot see a message-only window. main.c used to
     * create its message window that way and then look for it with
     * FindWindowW - so the "a second launch surfaces the first one" path could
     * never work.
     */
    {
        HWND found = FindWindowW(PROBE_CLASS, NULL);
        T_KV(L"FindWindow", L"0x%p (the top-level window is 0x%p)", found, top);
        ok(L"FindWindow finds the top-level probe window", found == top);
        ok(L"and cannot see the message-only one", found != msgonly);
    }

    MonitorFingerprint mons[WAC_MON_MAX];
    const int count = monitor_enumerate(mons, WAC_MON_MAX);
    T_KV(L"monitors now", L"%d", count);
    for (int i = 0; i < count; ++i) {
        T_KV(L"  monitor", L"[%d] %ls  %ls  %dx%d at %d,%d", i,
             mons[i].device_name,
             (mons[i].friendly_name[0] != L'\0') ? mons[i].friendly_name : L"(no EDID name)",
             mons[i].width, mons[i].height, mons[i].virtual_x, mons[i].virtual_y);
    }

    T_NOTE(L"Send one now, from another process - WITH A TIMEOUT:");
    T_NOTE(L"  SendMessageTimeout(HWND_BROADCAST=0xFFFF, WM_DISPLAYCHANGE=0x7E, 32,");
    T_NOTE(L"                     1920 | (1080 << 16), SMTO_ABORTIFHUNG, 2000, NULL)");
    T_NOTE(L"or change a resolution / unplug a monitor.");
    T_KV(L"waiting", L"%d second(s)", seconds);

    g_t0 = GetTickCount();
    const DWORD deadline = g_t0 + (DWORD)seconds * 1000u;

    MSG msg;
    while (GetTickCount() < deadline) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }

    T_KV(L"received", L"top-level %d, message-only %d", g_seen_top, g_seen_msg);

    if (g_seen_top > 0) {
        /*
         * The measurement this whole section exists for. Only asserted once a
         * broadcast has actually been seen, so a quiet run reports "not
         * measured" instead of a green tick it did not earn.
         */
        ok_int(L"the message-only window received none of them", g_seen_msg, 0);
        T_KV(L"measured", L"broadcasts reach the hidden TOP-LEVEL window only");
    } else {
        T_KV(L"measured", L"nothing arrived - no broadcast was sent during the wait");
    }

    DestroyWindow(msgonly);
    DestroyWindow(top);
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    int no_pause = 0;
    int probe_s  = 0;

    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
        if (!wcscmp(argv[i], L"--probe") && i + 1 < argc) {
            probe_s = _wtoi(argv[++i]);
        }
    }

    t_diag_init(L"t60_displaychange");
    t_log_mirror(1);
    t_console_init();

    /*
     * Per-monitor awareness BEFORE anything reads a monitor. Without it this
     * process is DPI-unaware, and Windows reports the 150% panel as 1707x1067
     * instead of 2560x1600 - measured 2026-10-09, and the reason the product's
     * own manifest declares PerMonitorV2. Every geometry printed below has to
     * be the product's geometry, not a scaled copy of it.
     */
    const int aware = t_set_dpi_awareness();

    T_TITLE(L"t60_displaychange - display-change detection (stage 7, task 7.1)");
    T_KV(L"run_at", L"%ls", t_now_string());
    T_KV(L"dpi awareness", L"%ls (SetProcessDpiAwarenessContext: %ls)",
         t_dpi_awareness_name(), (aware == 1) ? L"accepted" : L"fell back");

    test_debounce();
    test_diff();
    test_grace();

    if (probe_s > 0) {
        test_live_probe(probe_s);
    } else {
        T_SEC(L"4. the live message");
        T_KV(L"probe", L"SKIP - pass --probe <seconds> to watch for the real message");
    }

    T_SEC(L"Result");
    T_KV(L"passed", L"%d", g_pass);
    T_KV(L"failed", L"%d", g_fail);

    if (g_fail == 0) T_KV(L"VERDICT", L"PASS - all %d assertions hold", g_pass);
    else             T_KV(L"VERDICT", L"FAIL - %d assertion(s) broken", g_fail);

    if (!no_pause) t_pause();
    t_log_close();
    return (g_fail == 0) ? 0 : 1;
}
