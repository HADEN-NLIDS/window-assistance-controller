/*
 * t07_chromium.c - stage 1, experiment 8
 *
 * PURPOSE
 *   Decide whether the product should keep a special "Chromium launch path"
 *   that passes --window-position / --window-size to the application
 *   instead of calling SetWindowPos afterwards.
 *
 *   If it works, it is strictly better: the window is born in the right
 *   place, so the whole splash-window filter, read-back verification and
 *   DPI-mismatch machinery is bypassed for those applications. If it does
 *   not work - or covers too few of your applications - the extra code path
 *   is not worth its complexity and should be dropped.
 *
 * HOW TO RUN
 *     t07_chromium.exe                       auto-detect a browser and test
 *     t07_chromium.exe --browser "C:\...\msedge.exe"
 *     t07_chromium.exe --seconds 20          longer wait per case
 *     t07_chromium.exe --no-isolated         do NOT use a private profile
 *     t07_chromium.exe --keep-open           leave the browser running
 *
 *   --isolated (the default) launches each case with its own
 *   --user-data-dir under %TEMP%. This matters: without it the browser may
 *   restore the previous window's geometry and the measurement is garbage.
 *
 * WHAT TO RECORD (into docs/P0验证记录.md, experiment 8)
 *   - did the position/size flags take effect at all
 *   - the observed offset between requested and actual window rect
 *     (expected: the content-area vs window-frame difference)
 *   - what fraction of YOUR daily applications are Chromium-based
 *     -> >= 30% justifies the extra code path, below that drop it
 *
 * BUILD
 *   tests\build_tests.cmd t07_chromium
 */

#include "test_common.h"

#include <tlhelp32.h>

#define MAX_CASES 4

typedef struct {
    int x, y, w, h;
    const wchar_t *label;
} TestCase;

static const TestCase k_cases[MAX_CASES] = {
    {   0,   0,  960, 1000, L"A-left-half"  },
    { 960,   0,  960, 1000, L"B-right-half" },
    { 300, 200,  800,  600, L"C-floating"   },
    { 100, 100, 1280,  720, L"D-widescreen" },
};

/* ---------------- browser detection ---------------- */

typedef struct {
    wchar_t path[MAX_PATH];
    wchar_t name[64];
} Browser;

static int file_exists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int read_app_path(const wchar_t *exe, wchar_t *out, DWORD out_chars)
{
    wchar_t sub[512];
    _snwprintf_s(sub, 512, _TRUNCATE,
                 L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%ls", exe);

    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, KEY_READ, &key) != ERROR_SUCCESS &&
        RegOpenKeyExW(HKEY_CURRENT_USER,  sub, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return 0;

    DWORD type = 0, size = out_chars * sizeof(wchar_t);
    LONG rc = RegQueryValueExW(key, NULL, NULL, &type, (BYTE *)out, &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ) return 0;

    /* strip surrounding quotes if present */
    size_t n = wcslen(out);
    if (n >= 2 && out[0] == L'"' && out[n - 1] == L'"') {
        memmove(out, out + 1, (n - 2) * sizeof(wchar_t));
        out[n - 2] = L'\0';
    }
    return file_exists(out);
}

static int detect_browsers(Browser *out, int cap)
{
    struct { const wchar_t *exe; const wchar_t *rel; const wchar_t *name; } candidates[] = {
        { L"chrome.exe",  L"Google\\Chrome\\Application\\chrome.exe", L"Chrome" },
        { L"msedge.exe",  L"Microsoft\\Edge\\Application\\msedge.exe", L"Edge"   },
        { L"brave.exe",   L"BraveSoftware\\Brave-Browser\\Application\\brave.exe", L"Brave" },
        { L"vivaldi.exe", L"Vivaldi\\Application\\vivaldi.exe", L"Vivaldi" },
    };

    const wchar_t *roots[3];
    wchar_t pf[MAX_PATH], pf86[MAX_PATH], local[MAX_PATH];
    pf[0] = pf86[0] = local[0] = L'\0';
    DWORD n;

    n = MAX_PATH;
    if (GetEnvironmentVariableW(L"ProgramW6432", pf, n) == 0) pf[0] = L'\0';
    n = MAX_PATH;
    GetEnvironmentVariableW(L"ProgramFiles(x86)", pf86, n);
    n = MAX_PATH;
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, n);

    roots[0] = pf; roots[1] = pf86; roots[2] = local;

    int count = 0;
    for (int c = 0; c < (int)(sizeof(candidates) / sizeof(candidates[0])) && count < cap; ++c) {
        int found = 0;

        /* 1. App Paths is the authoritative registration. */
        if (read_app_path(candidates[c].exe, out[count].path, MAX_PATH)) {
            found = 1;
        } else {
            /* 2. Probe the usual install roots. */
            for (int r = 0; r < 3 && !found; ++r) {
                if (!roots[r][0]) continue;
                wchar_t probe[MAX_PATH];
                _snwprintf_s(probe, MAX_PATH, _TRUNCATE, L"%ls\\%ls", roots[r], candidates[c].rel);
                if (file_exists(probe)) {
                    wcsncpy_s(out[count].path, MAX_PATH, probe, _TRUNCATE);
                    found = 1;
                }
            }
        }
        if (found) {
            wcsncpy_s(out[count].name, 64, candidates[c].name, _TRUNCATE);
            count++;
        }
    }
    return count;
}

/* ---------------- window lookup ---------------- */

typedef struct {
    DWORD pid;
    HWND  hwnd;
} FindCtx;

static BOOL CALLBACK find_window_proc(HWND hwnd, LPARAM lp)
{
    FindCtx *ctx = (FindCtx *)lp;
    if (ctx->hwnd) return FALSE;
    if (!t_is_app_window(hwnd)) return TRUE;
    if (t_window_pid(hwnd) == ctx->pid) { ctx->hwnd = hwnd; return FALSE; }
    return TRUE;
}

/* Poll for a window owned by pid. TEST-ONLY: the product uses
   SetWinEventHook instead, because polling every 200ms forever would
   violate the idle-CPU budget. Here it runs for seconds at a time. */
static HWND wait_for_window(DWORD pid, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 200) {
        FindCtx ctx = { pid, NULL };
        EnumWindows(find_window_proc, (LPARAM)&ctx);
        if (ctx.hwnd) return ctx.hwnd;
        Sleep(200);
    }
    return NULL;
}

static void kill_tree(DWORD pid)
{
    if (!pid) return;
    wchar_t cmd[64];
    _snwprintf_s(cmd, 64, _TRUNCATE, L"/PID %lu /T /F", (unsigned long)pid);

    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));

    wchar_t exe[MAX_PATH];
    GetEnvironmentVariableW(L"SystemRoot", exe, MAX_PATH);
    wcscat_s(exe, MAX_PATH, L"\\System32\\taskkill.exe");

    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                       NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

/* ---------------- one test case ---------------- */

typedef struct {
    int launched;
    int window_found;
    int x, y, w, h;          /* requested (content area) */
    int got_x, got_y, got_w, got_h;
    int all_zero_pos;        /* did the flag visibly do nothing? */
} CaseResult;

static CaseResult run_case(const Browser *br, const TestCase *tc,
                           int timeout_ms, int isolated, int keep_open)
{
    CaseResult r;
    ZeroMemory(&r, sizeof(r));
    r.x = tc->x; r.y = tc->y; r.w = tc->w; r.h = tc->h;

    wchar_t cmdline[2048];
    wchar_t profile[MAX_PATH] = L"";
    if (isolated) {
        wchar_t tmp[MAX_PATH];
        GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH);
        _snwprintf_s(profile, MAX_PATH, _TRUNCATE,
                     L"%ls\\wac-test-%ls-%lu", tmp, br->name,
                     (unsigned long)GetTickCount());
    }

    _snwprintf_s(cmdline, 2048, _TRUNCATE,
                 L"\"%ls\" --new-window --window-position=%d,%d --window-size=%d,%d%ls%ls",
                 br->path, tc->x, tc->y, tc->w, tc->h,
                 isolated ? L" --user-data-dir=\"" : L"",
                 isolated ? profile : L"");
    if (isolated) wcscat_s(cmdline, 2048, L"\"");

    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));

    /* lpApplicationName is passed explicitly. Even in a test program we do
       not build a single command string: this is the same discipline
       docs/安全设计.md requires of the product. */
    if (!CreateProcessW(br->path, cmdline, NULL, NULL, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, NULL, NULL, &si, &pi)) {
        T_LASTERR(L"CreateProcessW");
        return r;
    }
    r.launched = 1;

    HWND hwnd = wait_for_window(pi.dwProcessId, timeout_ms);
    if (!hwnd) {
        T_WARN(L"no window appeared for pid %lu within %d ms",
               (unsigned long)pi.dwProcessId, timeout_ms);
    } else {
        /* give the browser a moment to apply its own geometry */
        Sleep(600);
        RECT rc;
        if (GetWindowRect(hwnd, &rc)) {
            r.window_found = 1;
            r.got_x = rc.left; r.got_y = rc.top;
            r.got_w = rc.right - rc.left;
            r.got_h = rc.bottom - rc.top;
            r.all_zero_pos = (rc.left == 0 && rc.top == 0 && tc->x != 0);
        }
    }

    if (!keep_open) {
        kill_tree(pi.dwProcessId);
        Sleep(800);
        if (profile[0]) {
            wchar_t del[MAX_PATH + 16];
            _snwprintf_s(del, MAX_PATH + 16, _TRUNCATE, L"rmdir /S /Q \"%ls\"", profile);
            wchar_t cmd[64];
            GetEnvironmentVariableW(L"SystemRoot", cmd, MAX_PATH);
            wcscat_s(cmd, MAX_PATH, L"\\System32\\cmd.exe");
            wchar_t cargs[MAX_PATH + 32];
            _snwprintf_s(cargs, MAX_PATH + 32, _TRUNCATE, L"/C %ls", del);

            STARTUPINFOW dsi; ZeroMemory(&dsi, sizeof(dsi)); dsi.cb = sizeof(dsi);
            PROCESS_INFORMATION dpi; ZeroMemory(&dpi, sizeof(dpi));
            if (CreateProcessW(cmd, cargs, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                               NULL, NULL, &dsi, &dpi)) {
                WaitForSingleObject(dpi.hProcess, 3000);
                CloseHandle(dpi.hProcess); CloseHandle(dpi.hThread);
            }
        }
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return r;
}

/* ---------------- main ---------------- */

int wmain(int argc, wchar_t **argv)
{
    /* Crash filter + file log first: a console that flashes and closes
       takes its output with it. See TRAPS.md. */
    t_diag_init(L"t07_chromium");
    t_log_mirror(1);   /* full report goes to the log by default */

    t_console_init();

    wchar_t browser_path[MAX_PATH] = L"";
    int timeout_ms = 15000;
    int isolated = 1, keep_open = 0;

    for (int i = 1; i < argc; ++i) {
        if      (!wcscmp(argv[i], L"--browser") && i + 1 < argc)
            wcsncpy_s(browser_path, MAX_PATH, argv[++i], _TRUNCATE);
        else if (!wcscmp(argv[i], L"--seconds") && i + 1 < argc) timeout_ms = _wtoi(argv[++i]) * 1000;
        else if (!wcscmp(argv[i], L"--no-isolated")) isolated = 0;
        else if (!wcscmp(argv[i], L"--keep-open")) keep_open = 1;
        else if (!wcscmp(argv[i], L"--help")) {
            t_out_line(L"usage: t07_chromium.exe [--browser PATH] [--seconds N]\n"
                    L"                       [--no-isolated] [--keep-open]\n");
            return 0;
        }
    }

    T_TITLE(L"t07_chromium - Chromium command-line window geometry");
    T_KV(L"run_at", L"%ls", t_now_string());

    t_set_dpi_awareness();

    /* ---------------- pick a browser ---------------- */
    Browser browsers[8];
    int bn = 0;
    ZeroMemory(browsers, sizeof(browsers));

    if (browser_path[0]) {
        if (!file_exists(browser_path)) {
            T_WARN(L"--browser path does not exist: %ls", browser_path);
            t_pause_if_console();
            return 1;
        }
        wcsncpy_s(browsers[0].path, MAX_PATH, browser_path, _TRUNCATE);
        wcsncpy_s(browsers[0].name, 64, L"custom", _TRUNCATE);
        bn = 1;
    } else {
        bn = detect_browsers(browsers, 8);
    }

    T_SEC(L"Browser detection");
    if (bn == 0) {
        T_WARN(L"No Chromium-based browser found via App Paths or the usual");
        T_WARN(L"install roots. Pass --browser <path> explicitly.");
        T_NOTE(L"This result alone is informative: if your applications are not");
        T_NOTE(L"Chromium-based, the special launch path in the product is not");
        T_NOTE(L"worth implementing - record that decision in the P0 log.");
        t_pause_if_console();
        t_log_close();
        return 0;
    }

    for (int i = 0; i < bn; ++i) {
        T_KV(L"browser", L"%ls -> %ls", browsers[i].name, browsers[i].path);
    }

    const Browser *br = &browsers[0];
    T_KV(L"using", L"%ls", br->name);
    T_KV(L"isolated_profile", L"%ls", isolated ? L"yes (recommended)" : L"NO - results may reflect restored state");
    T_KV(L"wait_per_case_ms", L"%d", timeout_ms);

    /* ---------------- run cases ---------------- */
    T_SEC(L"Test cases");
    T_NOTE(L"Requested geometry is the CONTENT AREA, because that is what");
    T_NOTE(L"--window-position/--window-size mean. GetWindowRect returns the");
    T_NOTE(L"whole window including frame, so a small constant offset is");
    T_NOTE(L"expected and is exactly what we are measuring.");

    CaseResult results[MAX_CASES];
    for (int i = 0; i < MAX_CASES; ++i) {
        t_out_line(L"\n  case %ls: request pos=%d,%d size=%dx%d",
                k_cases[i].label, k_cases[i].x, k_cases[i].y,
                k_cases[i].w, k_cases[i].h);
        fflush(stdout);

        results[i] = run_case(br, &k_cases[i], timeout_ms, isolated, keep_open);

        if (!results[i].launched) {
            T_WARN(L"launch failed - stopping here");
            break;
        }
        if (!results[i].window_found) {
            t_out_line(L"    -> no window found");
            continue;
        }

        int dx = results[i].got_x - results[i].x;
        int dy = results[i].got_y - results[i].y;
        int dw = results[i].got_w - results[i].w;
        int dh = results[i].got_h - results[i].h;

        t_out_line(L"    -> got   pos=%d,%d size=%dx%d",
                results[i].got_x, results[i].got_y, results[i].got_w, results[i].got_h);
        t_out_line(L"    -> delta dx=%d dy=%d dw=%d dh=%d", dx, dy, dw, dh);
    }

    /* ---------------- summary ---------------- */
    T_SEC(L"Summary");
    t_out_line(L"\n  %-14ls %-10ls %-18ls %-18ls %ls",
            L"case", L"window", L"requested", L"actual", L"delta");

    int found = 0, moved = 0;
    for (int i = 0; i < MAX_CASES; ++i) {
        if (!results[i].launched) continue;
        wchar_t req[32], got[32];
        _snwprintf_s(req, 32, _TRUNCATE, L"%d,%d %dx%d",
                     results[i].x, results[i].y, results[i].w, results[i].h);
        if (results[i].window_found) {
            _snwprintf_s(got, 32, _TRUNCATE, L"%d,%d %dx%d",
                         results[i].got_x, results[i].got_y,
                         results[i].got_w, results[i].got_h);
            found++;
            if (results[i].got_x != 0 || results[i].got_y != 0 ||
                results[i].got_w != 0)
                moved++;
        } else {
            wcscpy_s(got, 32, L"(none)");
        }
        t_out_line(L"  %-14ls %-10ls %-18ls %-18ls",
                k_cases[i].label,
                results[i].window_found ? L"yes" : L"NO",
                req, got);
    }

    T_KV(L"cases_launched", L"%d", MAX_CASES);
    T_KV(L"windows_found", L"%d", found);

    /* ---------------- verdict ---------------- */
    T_SEC(L"Verdict");
    if (found == 0) {
        T_KV(L"diagnosis", L"INCONCLUSIVE - no window appeared");
        T_NOTE(L"Try --seconds 30, or pass an explicit --browser path.");
    } else {
        int first = -1;
        for (int i = 0; i < MAX_CASES; ++i) if (results[i].window_found) { first = i; break; }

        int applied = (results[first].got_x != 0) || (results[first].got_y != 0);
        int sized   = (results[first].got_w > 0 &&
                       results[first].got_w >= results[first].w);   /* frame adds, never removes */

        /* Held in variables rather than passed inline: a ternary inside a
           macro argument trips MSVC's traditional preprocessor. */
        const wchar_t *pos_note = applied
            ? L"YES"
            : L"NO (window ignored --window-position)";
        const wchar_t *size_note = sized ? L"plausibly yes" : L"NO";

        T_KV(L"position_flag_applied", L"%ls", pos_note);
        T_KV(L"size_flag_applied",     L"%ls", size_note);

        if (applied && sized) {
            T_KV(L"diagnosis", L"PASS - the Chromium launch path is viable");
            T_NOTE(L"Record the dx/dy/dw/dh offsets above: the product must");
            T_NOTE(L"compensate for the frame when it stores abs_w/abs_h.");
        } else {
            T_KV(L"diagnosis", L"FAIL - the flags did not take effect");
            T_NOTE(L"Drop the LAUNCH_CHROMIUM code path from src/core/proc_launch.c");
            T_NOTE(L"and fall back to SetWindowPos for these applications.");
        }

        if (results[first].all_zero_pos) {
            T_WARN(L"Window stayed at 0,0 despite a non-zero request - the flag");
            T_WARN(L"was accepted but ignored in this run.");
        }
    }

    T_SEC(L"Coverage question - this is the actual decision input");
    T_NOTE(L"List your daily applications and mark which are Chromium-based");
    T_NOTE(L"(Chrome, Edge, Brave, VS Code, Slack, Discord, Teams, Notion, ...):");
    T_NOTE(L"");
    T_NOTE(L"  app                              Chromium-based?");
    T_NOTE(L"  -------------------------------  ----------------");
    T_NOTE(L"  (fill in)");
    T_NOTE(L"");
    T_NOTE(L"  Chromium share = ___%%");
    T_NOTE(L"");
    T_NOTE(L"  >= 30%%  -> implement LAUNCH_CHROMIUM in src/core/proc_launch.c");
    T_NOTE(L"  <  30%%  -> drop it; one code path fewer to maintain and test");

    t_pause_if_console();
    t_log_close();
    return 0;
}
