/*
 * t04_hotkey.c - stage 1, experiment 5
 *
 * PURPOSE
 *   Prove the emergency-exit mechanism works. This is the single most
 *   important safety feature in the product: the overlay is a full-screen
 *   topmost window that swallows the mouse, so if the hotkey cannot be
 *   delivered the user is stuck.
 *
 *   The product uses RegisterHotKey ONLY. It never installs a keyboard
 *   hook - see docs/安全设计.md - so this program tests exactly that
 *   mechanism and nothing else.
 *
 * HOW TO RUN
 *     t04_hotkey.exe                    120s window, stays normal
 *     t04_hotkey.exe --seconds 300      longer window
 *     t04_hotkey.exe --topmost          put a topmost window in front first
 *     t04_hotkey.exe --register "Ctrl+Alt+Q"
 *
 *   While it is running, switch to each scenario below, press the tested
 *   combination, and note whether the counter increments. The program
 *   prints a line on every delivery, so failures leave an obvious gap.
 *
 *   Press F12 at any time to quit (registered as its own hotkey).
 *
 * SCENARIOS TO TEST (record one line per scenario in the P0 log)
 *     1. plain window focused
 *     2. this program's own topmost window focused   (--topmost)
 *     3. Task Manager focused
 *     4. UAC prompt on screen
 *     5. fullscreen game / video focused
 *     6. after lock + unlock
 *
 *   Combinations deliberately avoided: the product must not pick a
 *   combination that other software commonly owns, and this program
 *   reports which candidates were already taken on your machine.
 *
 * BUILD
 *   tests\build_tests.cmd t04_hotkey
 */

#include "test_common.h"

#define IDT_TICK   1
#define IDT_POLL   2
#define IDT_LIMIT  3

#define MY_OVERLAY_CLASS L"WacTestTopmost"

/* Candidate combinations, in the order the product should try them. */
static const struct {
    const wchar_t *label;
    UINT mods;
    UINT vk;
} k_candidates[] = {
    { L"Ctrl+Alt+Shift+F12", MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F12 },
    { L"Ctrl+Alt+Shift+F11", MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F11 },
    { L"Ctrl+Alt+F12",       MOD_CONTROL | MOD_ALT,             VK_F12 },
    { L"Ctrl+Alt+Q",         MOD_CONTROL | MOD_ALT,             'Q'     },
    { L"Ctrl+Shift+Alt+F9",  MOD_CONTROL | MOD_SHIFT | MOD_ALT, VK_F9   },
};
#define CANDIDATE_COUNT (int)(sizeof(k_candidates) / sizeof(k_candidates[0]))

static int    g_deliveries = 0;
static DWORD  g_start_tick = 0;
static int    g_total_seconds = 120;
static UINT   g_test_hotkey_id = 1;
static UINT   g_exit_hotkey_id = 2;
static HWND   g_overlay = NULL;
static int    g_topmost = 0;
static wchar_t g_test_label[64] = L"(none registered)";

/* ------------------------------------------------------------------ */
/* A topmost window, used for scenario 2.                              */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HBRUSH bg = CreateSolidBrush(RGB(20, 40, 90));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        HFONT font = CreateFontW(-28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, L"Segoe UI");
        HFONT old = (HFONT)SelectObject(hdc, font);

        const wchar_t *lines[] = {
            L"t04_hotkey - topmost scenario",
            L"",
            L"This window is WS_EX_TOPMOST, covering the whole monitor.",
            L"Like the product's overlay, it swallows the mouse.",
            L"",
            L"Press the tested hotkey now and watch the console counter.",
            L"Press F12 to quit.",
        };
        int y = rc.top + 60;
        for (int i = 0; i < (int)(sizeof(lines) / sizeof(lines[0])); ++i) {
            TextOutW(hdc, rc.left + 60, y, lines[i], (int)wcslen(lines[i]));
            y += 40;
        }

        SelectObject(hdc, old);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void create_overlay(void)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = overlay_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = MY_OVERLAY_CLASS;
    RegisterClassExW(&wc);

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(GetConsoleWindow(), MONITOR_DEFAULTTONEAREST), &mi);

    g_overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        MY_OVERLAY_CLASS, L"WAC t04 topmost scenario",
        WS_POPUP,
        mi.rcMonitor.left, mi.rcMonitor.top,
        mi.rcMonitor.right - mi.rcMonitor.left,
        mi.rcMonitor.bottom - mi.rcMonitor.top,
        NULL, NULL, GetModuleHandleW(NULL), NULL);

    if (g_overlay) {
        ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
        SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

/* ------------------------------------------------------------------ */
/* Hotkey registration reporting                                       */
/* ------------------------------------------------------------------ */

static void register_and_report(void)
{
    T_SEC(L"Registration of candidate combinations");
    T_NOTE(L"The product tries these in order and tells the user which one won.");
    T_NOTE(L"Any candidate already owned by other software is reported TAKEN.");

    t_out_line(L"\n  %-22ls %-10ls %ls", L"combination", L"result", L"note");

    int first_free = -1;
    for (int i = 0; i < CANDIDATE_COUNT; ++i) {
        /* Use ids 100+ so they cannot collide with the test/exit hotkeys. */
        UINT id = 100 + (UINT)i;
        BOOL ok = RegisterHotKey(NULL, id, k_candidates[i].mods | MOD_NOREPEAT,
                                 k_candidates[i].vk);
        DWORD err = ok ? 0 : GetLastError();

        if (ok) {
            t_out_line(L"  %-22ls %-10ls %ls",
                    k_candidates[i].label, L"FREE", L"");
            if (first_free < 0) first_free = i;
            UnregisterHotKey(NULL, id);
        } else {
            t_out_line(L"  %-22ls %-10ls %ls (error %lu)",
                    k_candidates[i].label, L"TAKEN",
                    err == ERROR_HOTKEY_ALREADY_REGISTERED ? L"already registered by another process" : L"",
                    (unsigned long)err);
        }
    }

    T_KV(L"first_available", L"%ls",
         first_free >= 0 ? k_candidates[first_free].label : L"NONE - product must ask the user");
    if (first_free < 0) {
        T_WARN(L"Every candidate is taken. The product must surface this to the");
        T_WARN(L"user instead of silently running without an exit hotkey.");
    }
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    /* Crash filter + file log first: a console that flashes and closes
       takes its output with it. See TRAPS.md. */
    t_diag_init(L"t04_hotkey");
    t_log_mirror(1);   /* full report goes to the log by default */

    t_console_init();

    T_TITLE(L"t04_hotkey - emergency exit hotkey reachability");
    T_KV(L"run_at", L"%ls", t_now_string());

    for (int i = 1; i < argc; ++i) {
        if      (!wcscmp(argv[i], L"--seconds") && i + 1 < argc) g_total_seconds = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--topmost")) g_topmost = 1;
        else if (!wcscmp(argv[i], L"--register") && i + 1 < argc) {
            wcsncpy_s(g_test_label, 64, argv[++i], _TRUNCATE);
        }
        else if (!wcscmp(argv[i], L"--help")) {
            t_out_line(L"usage: t04_hotkey.exe [--seconds N] [--topmost] [--register \"Ctrl+Alt+Q\"]");
            return 0;
        }
    }

    register_and_report();

    /* ---------------- register the hotkey actually under test ------- */
    T_SEC(L"Hotkey under test");

    UINT mods = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    UINT vk   = VK_F12;

    if (wcscmp(g_test_label, L"(none registered)") != 0) {
        /* Parse a small subset: Ctrl/Alt/Shift + one key. Good enough here;
           the product will have a real capture control in the settings UI. */
        mods = 0;
        wchar_t buf[64];
        wcsncpy_s(buf, 64, g_test_label, _TRUNCATE);
        wchar_t *ctx = NULL;
        for (wchar_t *tok = wcstok_s(buf, L"+", &ctx); tok; tok = wcstok_s(NULL, L"+", &ctx)) {
            if      (!_wcsicmp(tok, L"Ctrl"))  mods |= MOD_CONTROL;
            else if (!_wcsicmp(tok, L"Alt"))   mods |= MOD_ALT;
            else if (!_wcsicmp(tok, L"Shift")) mods |= MOD_SHIFT;
            else if (tok[0] == L'F' || tok[0] == L'f') vk = VK_F1 + (_wtoi(tok + 1) - 1);
            else vk = (UINT)towupper(tok[0]);
        }
    } else {
        wcsncpy_s(g_test_label, 64, L"Ctrl+Alt+Shift+F12", _TRUNCATE);
    }

    if (!RegisterHotKey(NULL, g_test_hotkey_id, mods | MOD_NOREPEAT, vk)) {
        DWORD err = GetLastError();
        T_KV(L"register_test_hotkey", L"FAILED (error %lu)", (unsigned long)err);
        t_winerr(L"RegisterHotKey", err);
        T_WARN(L"Pick another combination with --register and try again.");
        t_pause_if_console();
        return 1;
    }
    T_KV(L"register_test_hotkey", L"OK");
    T_KV(L"combination", L"%ls", g_test_label);

    if (!RegisterHotKey(NULL, g_exit_hotkey_id, MOD_NOREPEAT, VK_F12)) {
        T_WARN(L"Could not register plain F12 as the quit key.");
        T_WARN(L"Use the console's close button, or Ctrl+C.");
    } else {
        T_KV(L"register_quit_key", L"OK (plain F12)");
    }

    if (g_topmost) {
        create_overlay();
        /* Kept in a variable rather than inline: a ternary inside a macro
           argument trips MSVC's traditional preprocessor. See test_common.h. */
        const wchar_t *ovl_state = g_overlay ? L"created and shown" : L"FAILED to create";
        T_KV(L"topmost_window", L"%ls", ovl_state);
    }

    /* ---------------- instructions ---------------- */
    T_SEC(L"Instructions");
    T_NOTE(L"The counter below increases every time the hotkey is delivered.");
    T_NOTE(L"Switch between the scenarios and press %ls in each one.", g_test_label);
    T_NOTE(L"");
    T_NOTE(L"  scenario 1  a plain window focused (e.g. this console)");
    T_NOTE(L"  scenario 2  the topmost overlay focused        %ls",
           g_topmost ? L"(active in this run)" : L"(re-run with --topmost)");
    T_NOTE(L"  scenario 3  Task Manager focused");
    T_NOTE(L"  scenario 4  a UAC prompt on screen");
    T_NOTE(L"  scenario 5  a fullscreen game or video focused");
    T_NOTE(L"  scenario 6  lock the screen, unlock, then press again");
    T_NOTE(L"");
    T_NOTE(L"Press F12 (or wait %d seconds) to finish and print the summary.",
           g_total_seconds);

    T_SEC(L"Delivery log");

    g_start_tick = GetTickCount();
    SetTimer(NULL, IDT_TICK,  1000, NULL);
    SetTimer(NULL, IDT_LIMIT, (UINT)g_total_seconds * 1000u, NULL);

    MSG msg;
    int last_shown = -1;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY) {
            if (msg.wParam == g_test_hotkey_id) {
                ++g_deliveries;
                DWORD elapsed = (GetTickCount() - g_start_tick) / 1000;
                t_out_line(L"  [%3lu s] delivery #%d  foreground=\"%ls\"",
                        (unsigned long)elapsed, g_deliveries,
                        t_window_title(GetForegroundWindow()));
                fflush(stdout);
            } else if (msg.wParam == g_exit_hotkey_id) {
                t_out_line(L"  [quit] F12 received");
                break;
            }
        } else if (msg.message == WM_TIMER && msg.hwnd == NULL) {
            if (msg.wParam == IDT_TICK) {
                int remaining = g_total_seconds - (int)((GetTickCount() - g_start_tick) / 1000);
                if (remaining / 10 != last_shown / 10) {   /* print every 10s */
                    t_out_line(L"  ... %d s remaining, %d deliveries so far",
                            remaining > 0 ? remaining : 0, g_deliveries);
                    fflush(stdout);
                    last_shown = remaining;
                }
            } else if (msg.wParam == IDT_LIMIT) {
                t_out_line(L"  [timeout] reached %d seconds", g_total_seconds);
                break;
            }
        }
    }

    KillTimer(NULL, IDT_TICK);
    KillTimer(NULL, IDT_LIMIT);
    UnregisterHotKey(NULL, g_test_hotkey_id);
    UnregisterHotKey(NULL, g_exit_hotkey_id);
    if (g_overlay) DestroyWindow(g_overlay);

    /* ---------------- summary ---------------- */
    T_SEC(L"Summary");
    T_KV(L"combination", L"%ls", g_test_label);
    T_KV(L"deliveries", L"%d", g_deliveries);
    T_KV(L"ran_seconds", L"%lu", (unsigned long)((GetTickCount() - g_start_tick) / 1000));

    const wchar_t *topmost_note = g_topmost
        ? L"yes"
        : L"no (re-run with --topmost to cover it)";
    T_KV(L"topmost_scenario", L"%ls", topmost_note);

    T_SEC(L"Verdict");
    if (g_deliveries == 0) {
        T_KV(L"diagnosis", L"FAIL - the hotkey was never delivered");
        T_WARN(L"The product's exit mechanism would NOT be usable with this combination.");
        T_WARN(L"Try another candidate with --register, and record this in the P0 log.");
    } else {
        T_KV(L"diagnosis", L"PASS - hotkey delivered %d time(s)", g_deliveries);
        T_NOTE(L"The product's RegisterHotKey-based exit mechanism is viable.");
        T_NOTE(L"Still record which scenarios delivered and which did not - the");
        T_NOTE(L"per-scenario evidence is what the P0 log needs.");
    }

    t_pause_if_console();
    t_log_close();
    return 0;
}
