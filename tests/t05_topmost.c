/*
 * t05_topmost.c - stage 1, experiment 6
 *
 * PURPOSE
 *   Find the ceiling of what a full-screen topmost layered window can cover.
 *   The product's edit overlay (src/ui/overlay.c) is exactly this kind of
 *   window, so this run tells us what the overlay can and cannot dominate -
 *   and therefore what must be documented as a limitation.
 *
 * WHAT IT DOES
 *   Creates a WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW popup
 *   covering an entire monitor, paints a border plus diagonals, and shows
 *   a countdown. A second refresh timer re-asserts HWND_TOPMOST at a low
 *   rate, mirroring the product's Z-order maintenance strategy. The
 *   program also reports how much CPU that maintenance costs, which is
 *   what decides the refresh interval in the product.
 *
 * HOW TO RUN
 *     t05_topmost.exe                        30s, 60% opacity, monitor 0
 *     t05_topmost.exe --seconds 120
 *     t05_topmost.exe --alpha 100
 *     t05_topmost.exe --monitor 1            cover the second monitor
 *     t05_topmost.exe --click-through        add WS_EX_TRANSPARENT
 *     t05_topmost.exe --launch <exe>         start a target FOR you (repeatable)
 *
 *   While it runs, try to bring each of these to the front and note whether
 *   the overlay stays on top:
 *     1. an ordinary window
 *     2. the taskbar / Start menu
 *     3. Task Manager          (Task Manager is itself topmost)
 *     4. a fullscreen game or fullscreen video
 *     5. another topmost window (e.g. this program run twice)
 *
 *   Press Ctrl+Alt+Shift+F12 to quit. F12 alone, Ctrl+Alt+F12 and
 *   Ctrl+Shift+F12 are tried as fallbacks, and the overlay always displays
 *   the one that actually registered - see g_exit_keys.
 *
 *   Anything passed to --launch is put in a job object and terminated when
 *   this program exits, so a run cannot leave its targets behind.
 *
 * WHAT TO RECORD (into docs/P0验证记录.md, experiment 6)
 *   - one line per target above: covered / not covered
 *   - cpu cost of the topmost refresh at 2s and at 1s intervals
 *   - whether the hotkey still worked while the overlay was up
 *
 * BUILD
 *   tests\build_tests.cmd t05_topmost
 */

#include "test_common.h"
#include "proc_launch.h"   /* LaunchGroup: start the targets, then clean them up */

#define OVERLAY_CLASS   L"WacTestOverlay"
#define IDT_COUNTDOWN   1
#define IDT_TOPMOST     2
/* (There used to be an IDT_EXIT_KEY here. It is gone: the countdown timer now
 * ends the run itself, so there is no second timer left that can fire late.) */

#define EXIT_HOTKEY_ID  1

/*
 * Exit keys, tried in this order.
 *
 * The first entry is the PRODUCT's own hotkey (src/main.c). That is not
 * decoration: this overlay is a preview of the product's edit overlay, so the
 * key that gets the operator out has to be the key the product will use. A
 * bare F12 is claimed by browser developer tools, IDEs and debuggers - exactly
 * what is likely to be in front while someone tests a full-screen overlay.
 *
 * MEASURED 2026-09-22 (reported by the user): with a browser full-screen, a
 * bare F12 never reached this program and opened the browser's developer tools
 * instead, while the overlay still said "press F12 to quit". The operator was
 * told to press a key that could not work, on the one control whose entire job
 * is ending a full-screen takeover.
 *
 * So: several candidates, and the overlay displays the one that ACTUALLY
 * registered. When none can be, it says that instead of inventing one.
 */
typedef struct {
    UINT           mods;
    UINT           vk;
    const wchar_t *label;
} ExitKey;

static const ExitKey g_exit_keys[] = {
    { MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F12, L"Ctrl+Alt+Shift+F12" },
    { MOD_CONTROL | MOD_ALT,             VK_F12, L"Ctrl+Alt+F12"       },
    { MOD_CONTROL | MOD_SHIFT,           VK_F12, L"Ctrl+Shift+F12"     },
    { 0,                                 VK_F12, L"F12"                },
};

#define EXIT_KEY_COUNT ((int)(sizeof(g_exit_keys) / sizeof(g_exit_keys[0])))

/* Index into g_exit_keys of the one that registered, or -1 when none did. */
static int g_exit_key_used = -1;

/* How the run ended. Reported in the summary so the countdown can be judged. */
static const wchar_t *g_exit_reason = L"auto-exit";

static int   g_total_seconds = 30;
static int   g_alpha         = 150;      /* 0..255 */
static int   g_monitor_index = 0;
static int   g_click_through = 0;
static int   g_topmost_interval_ms = 2000;
static int   g_topmost_reasserts = 0;
static DWORD g_start_tick = 0;
static HWND  g_overlay = NULL;

/*
 * Targets passed to --launch, and the group that owns them.
 *
 * MEASURED 2026-09-22 (reported by the user): an earlier run had to be ended
 * by hand and left its browser windows open. Terminating one pid is not enough
 * for a browser - the window on screen belongs to a process the started pid
 * only brokered - so the targets go into a job object and the whole tree goes
 * when the job is closed.
 */
#define TARGET_MAX 4

static wchar_t     g_target[TARGET_MAX][MAX_PATH];
static int         g_target_count = 0;
static LaunchGroup g_group;
static BOOL        g_group_ready = FALSE;

static void paint_overlay(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    /* Semi-transparent wash. With LWA_ALPHA the whole window is already
       blended, so this just tints it; it is deliberately cheap. */
    HBRUSH bg = CreateSolidBrush(RGB(15, 30, 60));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    /* Border + diagonals, so partial coverage is obvious at a glance. */
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(255, 200, 0));
    HPEN old_pen = (HPEN)SelectObject(hdc, pen);
    HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

    Rectangle(hdc, rc.left + 2, rc.top + 2, rc.right - 2, rc.bottom - 2);
    MoveToEx(hdc, rc.left, rc.top, NULL);
    LineTo(hdc, rc.right, rc.bottom);
    MoveToEx(hdc, rc.right, rc.top, NULL);
    LineTo(hdc, rc.left, rc.bottom);

    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(pen);

    /* Countdown */
    DWORD elapsed = (GetTickCount() - g_start_tick) / 1000;
    int remaining = g_total_seconds - (int)elapsed;
    if (remaining < 0) remaining = 0;

    wchar_t line1[160], line2[160], line3[160];
    _snwprintf_s(line1, 160, _TRUNCATE,
                 L"t05_topmost  -  overlay covers rcMonitor of monitor %d", g_monitor_index);
    _snwprintf_s(line2, 160, _TRUNCATE,
                 L"alpha=%d  click-through=%ls  refresh=%dms  reasserts=%d",
                 g_alpha, g_click_through ? L"on" : L"off",
                 g_topmost_interval_ms, g_topmost_reasserts);
    if (g_exit_key_used >= 0) {
        _snwprintf_s(line3, 160, _TRUNCATE,
                     L"auto-exit in %d s   -   press %ls to quit",
                     remaining, g_exit_keys[g_exit_key_used].label);
    } else {
        _snwprintf_s(line3, 160, _TRUNCATE,
                     L"auto-exit in %d s   -   NO quit key available; only the countdown ends this",
                     remaining);
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));

    HFONT big = CreateFontW(-44, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT old_font = (HFONT)SelectObject(hdc, big);

    int cx = rc.left + 80;
    int cy = rc.top + 100;
    TextOutW(hdc, cx, cy, line1, (int)wcslen(line1));
    TextOutW(hdc, cx, cy + 70, line2, (int)wcslen(line2));
    TextOutW(hdc, cx, cy + 140, line3, (int)wcslen(line3));

    SelectObject(hdc, old_font);
    DeleteObject(big);

    /* Static scenario checklist, so the operator does not need the console. */
    HFONT small = CreateFontW(-24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    old_font = (HFONT)SelectObject(hdc, small);
    SetTextColor(hdc, RGB(220, 230, 255));

    const wchar_t *checklist[] = {
        L"Bring each of these to the front and check whether this overlay stays on top:",
        L"  1. an ordinary window          2. taskbar / Start menu",
        L"  3. Task Manager                4. a fullscreen game or video",
        L"  5. another topmost window",
        L"Re-run with --click-through to compare mouse behaviour.",
    };
    int y = cy + 260;
    for (int i = 0; i < (int)(sizeof(checklist) / sizeof(checklist[0])); ++i) {
        TextOutW(hdc, cx, y, checklist[i], (int)wcslen(checklist[i]));
        y += 40;
    }

    SelectObject(hdc, old_font);
    DeleteObject(small);

    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        paint_overlay(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;   /* we paint everything ourselves */

    case WM_TIMER:
        if (wp == IDT_COUNTDOWN) {
            /*
             * The countdown doubles as the exit.
             *
             * MEASURED 2026-09-22 (reported by the user): the previous version
             * trusted a separate one-shot thread timer and exited late - the
             * overlay outlived its own countdown while a full-screen browser
             * was in front.
             *
             * A timer that is CHECKED cannot do that. This runs every second,
             * compares against the same clock the countdown is drawn from, and
             * ends the run the moment the two agree, whatever else the message
             * queue happens to be doing.
             */
            const DWORD elapsed = (GetTickCount() - g_start_tick) / 1000;
            if ((int)elapsed >= g_total_seconds) {
                g_exit_reason = L"auto-exit";
                DestroyWindow(hwnd);
                return 0;
            }
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wp == IDT_TOPMOST) {
            /* Mirror the product's low-rate Z-order maintenance. Doing this
               in a tight loop would burn CPU for no benefit. */
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ++g_topmost_reasserts;
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Enumerate monitors and return the Nth. Returns FALSE if out of range. */
typedef struct { int want; int seen; HMONITOR found; } MonPick;

static BOOL CALLBACK pick_monitor(HMONITOR hm, HDC hdc, LPRECT r, LPARAM lp)
{
    (void)hdc; (void)r;
    MonPick *p = (MonPick *)lp;
    if (p->seen == p->want) { p->found = hm; return FALSE; }
    p->seen++;
    return TRUE;
}

/* Plain counter, used only to validate --monitor bounds. */
static BOOL CALLBACK count_monitor(HMONITOR hm, HDC hdc, LPRECT r, LPARAM lp)
{
    (void)hm; (void)hdc; (void)r;
    (*(int *)lp)++;
    return TRUE;
}

/*
 * Start everything given to --launch.
 *
 * Called after the overlay is on screen, so the operator can watch each target
 * appear underneath it - which is the scenario this experiment is about.
 */
static void start_targets(void)
{
    if (g_target_count <= 0) return;

    /*
     * A group whose job object could not be created still works: it falls back
     * to terminating the processes it holds. It just cannot reach
     * grandchildren, which for a browser means leaving helpers behind - so
     * that case is said out loud rather than discovered later.
     */
    if (!launch_group_init(&g_group)) {
        T_WARN(L"could not create the cleanup job object; targets will be ended");
        T_WARN(L"one process at a time, which may leave browser helpers running");
    }
    g_group_ready = TRUE;

    T_SEC(L"Targets started by this run");
    for (int i = 0; i < g_target_count; ++i) {
        LaunchSpec spec;
        ZeroMemory(&spec, sizeof(spec));
        spec.exe_path = g_target[i];
        spec.args     = NULL;
        spec.kind     = LAUNCH_CREATE;

        DWORD pid = 0;
        spec.out_pid = &pid;

        HANDLE proc = NULL;
        if (!launch_app(&spec, &proc)) {
            T_WARN(L"could not start %ls (error %lu)", g_target[i],
                   (unsigned long)GetLastError());
            continue;
        }

        T_KV(L"started", L"%ls  pid=%lu", g_target[i], (unsigned long)pid);

        if (proc == NULL) {
            /* LAUNCH_CREATE normally hands over the process, but a shell
             * launch need not. Without the handle cleanup cannot reach it, and
             * saying so beats a silent leak. */
            T_WARN(L"no process handle for pid %lu - it will NOT be cleaned up",
                   (unsigned long)pid);
        } else if (!launch_group_add(&g_group, proc, pid)) {
            /* launch_group_add closes the handle when it refuses, so nothing
             * leaks - but this target is now untracked. */
            T_WARN(L"could not track pid %lu for cleanup", (unsigned long)pid);
        }
    }
}

/* End everything --launch started. Safe to call when nothing was started. */
static void stop_targets(void)
{
    if (!g_group_ready) return;
    g_group_ready = FALSE;

    if (g_group.count <= 0) {
        launch_group_destroy(&g_group);
        return;
    }

    int failed = 0;
    const int ended = launch_group_terminate(&g_group, &failed);

    T_SEC(L"Cleanup");
    T_KV(L"targets_ended", L"%d", ended);
    if (failed > 0) {
        T_WARN(L"%d target process(es) refused to end - close them by hand",
               failed);
    }
    if (g_group.job == NULL) {
        T_WARN(L"there was no job object, so grandchildren (browser helpers,");
        T_WARN(L"for example) may still be running");
    }

    launch_group_destroy(&g_group);
}

int wmain(int argc, wchar_t **argv)
{
    /* Crash filter + file log first: a console that flashes and closes
       takes its output with it. See TRAPS.md. */
    t_diag_init(L"t05_topmost");
    t_log_mirror(1);   /* full report goes to the log by default */

    t_console_init();

    T_TITLE(L"t05_topmost - full-screen topmost window coverage");
    T_KV(L"run_at", L"%ls", t_now_string());

    for (int i = 1; i < argc; ++i) {
        if      (!wcscmp(argv[i], L"--seconds")  && i + 1 < argc) g_total_seconds = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--alpha")    && i + 1 < argc) g_alpha = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--monitor")  && i + 1 < argc) g_monitor_index = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--refresh")  && i + 1 < argc) g_topmost_interval_ms = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--click-through")) g_click_through = 1;
        else if (!wcscmp(argv[i], L"--launch") && i + 1 < argc) {
            if (g_target_count < TARGET_MAX) {
                wcsncpy_s(g_target[g_target_count], MAX_PATH, argv[++i], _TRUNCATE);
                ++g_target_count;
            } else {
                T_WARN(L"--launch: at most %d targets, ignoring %ls",
                       TARGET_MAX, argv[++i]);
            }
        }
        else if (!wcscmp(argv[i], L"--help")) {
            t_out_line(L"usage: t05_topmost.exe [--seconds N] [--alpha 0..255] [--monitor N]\n"
                    L"                       [--refresh MS] [--click-through]\n"
                    L"                       [--launch <exe>]   repeatable; the targets\n"
                    L"                                         are ended when this exits\n");
            return 0;
        }
    }

    t_set_dpi_awareness();

    if (g_alpha < 0)   g_alpha = 0;
    if (g_alpha > 255) g_alpha = 255;

    /* ---------------- monitor selection ---------------- */
    MonPick pick = { g_monitor_index, 0, NULL };
    EnumDisplayMonitors(NULL, NULL, pick_monitor, (LPARAM)&pick);

    int mon_count = 0;
    EnumDisplayMonitors(NULL, NULL, count_monitor, (LPARAM)&mon_count);

    T_KV(L"monitor_count", L"%d", mon_count);

    if (!pick.found) {
        T_WARN(L"monitor index %d does not exist (valid: 0..%d)",
               g_monitor_index, mon_count - 1);
        t_pause_if_console();
        return 1;
    }

    /* MONITORINFOEXW, not MONITORINFO: only the EX variant carries
       szDevice, and the struct must be sized accordingly or the call
       fails. Same pattern as tests/t01_dpi.c. */
    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(pick.found, (MONITORINFO *)&mi)) {
        T_LASTERR(L"GetMonitorInfoW");
        t_pause_if_console();
        return 1;
    }

    LONG w = mi.rcMonitor.right - mi.rcMonitor.left;
    LONG h = mi.rcMonitor.bottom - mi.rcMonitor.top;

    T_SEC(L"Configuration");
    T_KV(L"monitor_index", L"%d", g_monitor_index);
    T_KV(L"monitor_device", L"%ls", mi.szDevice);
    T_KV(L"cover_rect", L"%ld,%ld %ldx%ld  (rcMonitor, includes the taskbar area)",
         mi.rcMonitor.left, mi.rcMonitor.top, w, h);
    T_KV(L"alpha", L"%d / 255", g_alpha);
    T_KV(L"click_through", L"%ls", g_click_through ? L"on" : L"off");
    T_KV(L"topmost_refresh_ms", L"%d", g_topmost_interval_ms);
    T_KV(L"auto_exit_seconds", L"%d", g_total_seconds);

    T_SEC(L"Try to defeat the overlay");
    T_NOTE(L"Bring each target to the front and watch whether the border and");
    T_NOTE(L"diagonals remain visible above it:");
    T_NOTE(L"  1. an ordinary window");
    T_NOTE(L"  2. the taskbar, then the Start menu");
    T_NOTE(L"  3. Task Manager (it is itself topmost)");
    T_NOTE(L"  4. a fullscreen game or fullscreen video");
    T_NOTE(L"  5. a second instance of this program");
    T_NOTE(L"");
    T_NOTE(L"Note whether the taskbar clock is still readable through the wash -");
    T_NOTE(L"the product's overlay must stay faint enough to see real windows.");

    /* ---------------- create the overlay ---------------- */
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = overlay_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = OVERLAY_CLASS;
    if (!RegisterClassExW(&wc)) {
        T_LASTERR(L"RegisterClassExW");
        t_pause_if_console();
        return 1;
    }

    DWORD ex_style = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (g_click_through) ex_style |= WS_EX_TRANSPARENT;

    g_overlay = CreateWindowExW(ex_style, OVERLAY_CLASS, L"WAC t05 overlay",
                                WS_POPUP,
                                mi.rcMonitor.left, mi.rcMonitor.top, w, h,
                                NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_overlay) {
        T_LASTERR(L"CreateWindowExW");
        t_pause_if_console();
        return 1;
    }

    SetLayeredWindowAttributes(g_overlay, 0, (BYTE)g_alpha, LWA_ALPHA);

    /* ---------------- focus / hotkey safety ----------------
     *
     * Candidates are tried most-specific first and the overlay is told which
     * one won. An overlay that says "press F12" after failing to claim F12 is
     * worse than one offering no key at all: the operator presses it,
     * something else reacts, and the overlay is still there.
     * ------------------------------------------------------ */
    for (int i = 0; i < EXIT_KEY_COUNT; ++i) {
        if (RegisterHotKey(NULL, EXIT_HOTKEY_ID,
                           g_exit_keys[i].mods | MOD_NOREPEAT,
                           g_exit_keys[i].vk)) {
            g_exit_key_used = i;
            break;
        }
        T_WARN(L"could not register %ls as a quit key (already taken?)",
               g_exit_keys[i].label);
    }

    if (g_exit_key_used >= 0) {
        T_KV(L"quit_hotkey", L"%ls registered",
             g_exit_keys[g_exit_key_used].label);
    } else {
        T_WARN(L"NO quit key could be registered. The overlay says so on");
        T_WARN(L"screen; only the %d second countdown will end this run.",
               g_total_seconds);
    }

    t_report_foreground(L"foreground_before_show");
    ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
    SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    UpdateWindow(g_overlay);
    t_report_foreground(L"foreground_after_show");

    T_KV(L"focus_check", L"the two foreground lines above must be identical");

    /* Held in a variable rather than passed inline: a ternary inside a macro
       argument trips MSVC's traditional preprocessor. See test_common.h. */
    const wchar_t *fg_note = (GetForegroundWindow() == g_overlay)
        ? L"YES - bad, it stole focus"
        : L"no - correct";
    T_KV(L"overlay_is_foreground", L"%ls", fg_note);

    /* ---------------- run ---------------- */
    T_SEC(L"Running");
    T_NOTE(L"Auto-exit in %d seconds, or press %ls.", g_total_seconds,
           g_exit_key_used >= 0 ? g_exit_keys[g_exit_key_used].label
                                : L"(no quit key could be registered)");
    T_NOTE(L"CPU cost of the topmost refresh is reported below - this is what");
    T_NOTE(L"decides the refresh interval in the product's overlay.");

    /*
     * Bring the targets up UNDER the overlay - that is the thing being
     * watched. Started before the clock is set, so a browser that takes three
     * seconds to appear does not eat the operator's countdown.
     */
    start_targets();

    g_start_tick = GetTickCount();
    SetTimer(g_overlay, IDT_COUNTDOWN, 1000, NULL);
    SetTimer(g_overlay, IDT_TOPMOST, (UINT)g_topmost_interval_ms, NULL);
    /* No third timer. The countdown above is the only thing that ends the run,
     * so the run cannot outlive the number the overlay is showing. */

    /*
     * WM_HOTKEY arrives with hwnd == NULL, so DispatchMessageW would throw it
     * away - it has to be handled here, in the loop.
     */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY) {
            /*
             * Log EVERY hotkey that arrives, not only the one meant to end the
             * run.
             *
             * MEASURED 2026-09-22: the operator reported the overlay vanishing
             * "before F12 was pressed". That report could not be answered
             * afterwards - the only evidence was what had been pressed and
             * remembered, and the log from that run had already been
             * overwritten by a later one. lParam carries the modifiers in its
             * low word and the virtual key in its high word, so this line says
             * exactly which combination arrived and when. The next occurrence
             * diagnoses itself.
             */
            t_out_line(L"  [hotkey] id=%lu mods=0x%04X vk=0x%02X at %lu ms",
                       (unsigned long)msg.wParam,
                       (unsigned)(msg.lParam & 0xFFFFu),
                       (unsigned)(((unsigned long)msg.lParam >> 16) & 0xFFFFu),
                       (unsigned long)(GetTickCount() - g_start_tick));

            if (msg.wParam == EXIT_HOTKEY_ID) {
                g_exit_reason = L"quit key";
                t_out_line(L"  [quit] %ls ended the run",
                           g_exit_keys[g_exit_key_used].label);
                break;
            }
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    KillTimer(g_overlay, IDT_COUNTDOWN);
    KillTimer(g_overlay, IDT_TOPMOST);
    if (g_exit_key_used >= 0) {
        UnregisterHotKey(NULL, EXIT_HOTKEY_ID);
    }

    if (IsWindow(g_overlay)) DestroyWindow(g_overlay);

    /* Everything --launch started goes away with this process. Done after the
     * overlay is gone, so the operator watches it happen instead of guessing
     * whether it did. */
    stop_targets();

    /* ---------------- summary ---------------- */
    T_SEC(L"Summary");
    const unsigned long ran = (unsigned long)((GetTickCount() - g_start_tick) / 1000);
    T_KV(L"ran_seconds", L"%lu", ran);
    T_KV(L"exit_reason", L"%ls", g_exit_reason);
    T_KV(L"quit_key_used", L"%ls",
         g_exit_key_used >= 0 ? g_exit_keys[g_exit_key_used].label : L"none");

    /*
     * The countdown has to mean something. A test that announces "auto-exit in
     * 30 s" and then stays up for 45 is precisely the kind of unverified claim
     * this project keeps getting caught by - and it is the complaint that
     * produced this check (2026-09-22).
     */
    if (wcscmp(g_exit_reason, L"auto-exit") == 0) {
        const long drift = (long)ran - (long)g_total_seconds;
        T_KV(L"countdown_drift_s", L"%ld  (0 or 1 means on time)", drift);
        if (drift < 0 || drift > 1) {
            T_WARN(L"the overlay outlived its own countdown by %ld s", drift);
        }
    }

    T_KV(L"topmost_reasserts", L"%d  (at %d ms interval)", g_topmost_reasserts, g_topmost_interval_ms);
    T_KV(L"alpha_used", L"%d", g_alpha);
    T_KV(L"click_through", L"%ls", g_click_through ? L"on" : L"off");

    T_SEC(L"Verdict and what to record");
    T_NOTE(L"1. For each of the five targets, note COVERED or NOT COVERED.");
    T_NOTE(L"   Anything NOT COVERED belongs in the product's documented limits.");
    T_NOTE(L"2. If the taskbar or Start menu is covered, confirm the overlay is");
    T_NOTE(L"   still faint enough to read real windows behind it.");
    T_NOTE(L"3. Compare CPU usage at --refresh 2000 versus --refresh 1000 and");
    T_NOTE(L"   pick the product's interval from that measurement.");
    T_NOTE(L"4. overlay_is_foreground must be 'no' - the overlay must never");
    T_NOTE(L"   take focus from the user.");
    T_NOTE(L"5. Kill this process from Task Manager while the overlay is up and");
    T_NOTE(L"   confirm the screen returns to normal with no leftover window.");

    t_pause_if_console();
    t_log_close();
    return 0;
}
