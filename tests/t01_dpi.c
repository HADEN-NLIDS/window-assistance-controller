/*
 * t01_dpi.c - stage 1, experiment 2
 *
 * PURPOSE
 *   Confirm that under PerMonitorV2 every coordinate the program sees is a
 *   PHYSICAL pixel, 1:1 with the real screen. If this is wrong, every
 *   normalized-coordinate calculation in the product is wrong, so this is
 *   the single most load-bearing assumption in the whole plan.
 *
 * WHAT TO RECORD (into docs/P0验证记录.md, experiment 2)
 *   - whether SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2) succeeded
 *   - for each monitor: rcMonitor, rcWork, DPI, scale%, and whether
 *     rcMonitor's width matches the real panel width in physical pixels
 *   - for each monitor: whether GetDpiForMonitor agrees with
 *     GetDpiForWindow on a window living on that monitor
 *
 * HOW TO RUN
 *   Run it at 100%, 150% and 200% scaling (Settings > Display > Scale),
 *   and with a second monitor attached if available.
 *
 *     t01_dpi.exe
 *
 * BUILD
 *   tests\build_tests.cmd t01_dpi
 */

#include "test_common.h"

#include <shellscalingapi.h>   /* GetDpiForMonitor, MONITOR_DPI_TYPE */

#pragma comment(lib, "shcore.lib")

typedef BOOL (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT *, UINT *);

/* Per-monitor facts collected so the report can end with a compact summary
   of exactly the values stage 1 needs recorded. */
typedef struct {
    wchar_t device[32];
    int     primary;
    long    x, y, w, h;
    long    work_w, work_h;
    unsigned dpi, scale;
} MonSummary;

#define MAX_SUMMARY 8
static MonSummary g_sum[MAX_SUMMARY];
static int        g_sum_count;

/* Result of the "is a real window inside its monitor's numbers" check. */
static int g_cross_checked;
static int g_cross_ok;
static int g_probe_was_console;   /* 1 = a real visible console window was used */
static long g_probe_w, g_probe_h; /* read-back size of the probe window */
static long g_probe_dw, g_probe_dh;

/*
 * Awareness reporting lives in test_common.h as t_dpi_awareness_name().
 * It is NOT implemented here on purpose: the obvious implementation -
 * GetModuleHandleW + GetAwarenessFromDpiAwarenessContext - is wrong twice
 * over (module not loaded, and the enum cannot distinguish V1 from V2).
 * See the comments on that function before changing anything here.
 */

static int g_monitor_index = 0;

/*
 * GetDpiForMonitor resolved once and kept for the process lifetime.
 *
 * The first version of this file did LoadLibraryW/FreeLibrary on every
 * monitor callback. That is needless churn, and in a VM (virtual display
 * driver, possibly a different shcore build) it is the least trustworthy
 * part of the program. Resolve once; if it is unavailable, the monitor
 * loop simply skips the DPI field instead of doing anything clever.
 */
static PFN_GetDpiForMonitor g_get_dpi_for_monitor = NULL;
static int g_shcore_tried = 0;

static void resolve_dpi_for_monitor(void)
{
    if (g_shcore_tried) return;
    g_shcore_tried = 1;

    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    t_log(L"shcore.dll = %p", (void *)shcore);
    if (shcore) {
        g_get_dpi_for_monitor =
            (PFN_GetDpiForMonitor)(void *)GetProcAddress(shcore, "GetDpiForMonitor");
        t_log(L"GetDpiForMonitor = %p", (void *)g_get_dpi_for_monitor);
    }
    /* Deliberately NOT freed: the pointer must stay valid for the whole run,
       and a DLL that is still resident costs nothing here. */
}

static BOOL CALLBACK monitor_proc(HMONITOR hmon, HDC hdc, LPRECT rect, LPARAM lp)
{
    (void)hdc; (void)rect; (void)lp;

    int idx = g_monitor_index;
    t_log(L"monitor[%d]: callback entered, handle=%p", idx, (void *)hmon);

    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, (MONITORINFO *)&mi)) {
        DWORD err = GetLastError();
        t_log(L"monitor[%d]: GetMonitorInfoW FAILED err=%lu - skipped", idx, (unsigned long)err);
        t_out_line(L"  !! monitor[%d]: GetMonitorInfoW failed (error %lu) - skipped",
                idx, (unsigned long)err);
        return TRUE;
    }

    g_monitor_index++;
    t_log(L"monitor[%d]: %ls %ldx%ld", idx, mi.szDevice,
          mi.rcMonitor.right - mi.rcMonitor.left,
          mi.rcMonitor.bottom - mi.rcMonitor.top);

    UINT dpi_x = 0, dpi_y = 0;
    if (g_get_dpi_for_monitor) {
        g_get_dpi_for_monitor(hmon, 0 /* MDT_EFFECTIVE_DPI */, &dpi_x, &dpi_y);
    }
    t_log(L"monitor[%d]: dpi=%ux%u", idx, dpi_x, dpi_y);

    LONG mon_w = mi.rcMonitor.right  - mi.rcMonitor.left;
    LONG mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    LONG wrk_w = mi.rcWork.right     - mi.rcWork.left;
    LONG wrk_h = mi.rcWork.bottom    - mi.rcWork.top;

    /* Collect for the closing summary. */
    if (g_sum_count < MAX_SUMMARY) {
        MonSummary *s = &g_sum[g_sum_count++];
        ZeroMemory(s, sizeof(*s));
        wcsncpy_s(s->device, 32, mi.szDevice, _TRUNCATE);
        s->primary  = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0;
        s->x = mi.rcMonitor.left; s->y = mi.rcMonitor.top;
        s->w = mon_w;             s->h = mon_h;
        s->work_w = wrk_w;        s->work_h = wrk_h;
        s->dpi    = dpi_x;
        s->scale  = dpi_x ? (dpi_x * 100 / 96) : 0;
    }

    t_out_line(L"");
    t_out_line(L"[MONITOR.%d.device] %ls", idx, mi.szDevice);
    t_out_line(L"[MONITOR.%d.primary] %ls",  idx,
               (mi.dwFlags & MONITORINFOF_PRIMARY) ? L"yes" : L"no");
    t_out_line(L"[MONITOR.%d.rcMonitor] %ld,%ld %ldx%ld",
               idx, mi.rcMonitor.left, mi.rcMonitor.top, mon_w, mon_h);
    t_out_line(L"[MONITOR.%d.rcWork] %ld,%ld %ldx%ld",
               idx, mi.rcWork.left, mi.rcWork.top, wrk_w, wrk_h);
    t_out_line(L"[MONITOR.%d.taskbar_inset] left=%ld top=%ld right=%ld bottom=%ld",
               idx,
               mi.rcWork.left - mi.rcMonitor.left,
               mi.rcWork.top - mi.rcMonitor.top,
               mi.rcMonitor.right - mi.rcWork.right,
               mi.rcMonitor.bottom - mi.rcWork.bottom);
    t_out_line(L"[MONITOR.%d.dpi] %ux%u  scale=%u%%",
               idx, dpi_x, dpi_y, dpi_x ? (dpi_x * 100 / 96) : 0);
    t_out_line(L"[MONITOR.%d.scale_check] %ld physical px / %u%% scale = %.1f px at 100%%",
               idx, mon_w, dpi_x ? (dpi_x * 100 / 96) : 100,
               dpi_x ? ((double)mon_w * 100.0 / (double)(dpi_x * 100 / 96)) : (double)mon_w);

    return TRUE;
}

/*
 * Closing block: the values stage 1 actually needs, in a form that can be
 * copied into docs/P0验证记录.md without hunting through the report above.
 *
 * Kept ASCII-only on purpose: console output is written as UTF-8, but a
 * console font with no CJK glyphs renders such text as "????" even though
 * the bytes are correct. Rather than make the text depend on the reader's
 * font, structure carries the meaning here and the wording stays English.
 * The Chinese explanations live in TRAPS.md.
 */
static void print_summary(int v2, int monitor_count)
{
    t_out_line(L"");
    t_out_line(L"================================================================");
    t_out_line(L"  RECORD THESE VALUES");
    t_out_line(L"================================================================");
    t_out_line(L"  Open this file and fill in the table under 'experiment 2':");
    t_out_line(L"");
    t_out_line(L"    docs\\P0-verification.md      <-- English name");
    t_out_line(L"    docs\\P0<CJK-3-chars>.md      <-- the same file, Chinese name");
    t_out_line(L"");
    t_out_line(L"  The file name contains Chinese characters that some console");
    t_out_line(L"  fonts cannot draw, so it is spelled out rather than printed.");
    t_out_line(L"  Both names point at the same file.");
    t_out_line(L"================================================================");
    t_out_line(L"");
    t_out_line(L"  R1. DPI awareness method");
    t_out_line(L"      V2 runtime call ............ %ls",
               v2 ? L"SUCCESS" : L"FAILED (fell back)");
    t_out_line(L"      effective awareness ........ %ls", t_dpi_awareness_name());
    t_out_line(L"      expected ................... PER_MONITOR_AWARE_V2");
    t_out_line(L"");
    t_out_line(L"  R2. Monitors found ............. %d", monitor_count);

    for (int i = 0; i < g_sum_count; ++i) {
        MonSummary *s = &g_sum[i];
        t_out_line(L"");
        t_out_line(L"      monitor %d: %ls%ls", i, s->device,
                   s->primary ? L"  (primary)" : L"");
        t_out_line(L"        position .................. %ld,%ld", s->x, s->y);
        t_out_line(L"        visible area (rcMonitor) .. %ld x %ld", s->w, s->h);
        t_out_line(L"        work area (rcWork) ........ %ld x %ld", s->work_w, s->work_h);
        t_out_line(L"        DPI / scale ............... %u  (%u%%)", s->dpi, s->scale);
        t_out_line(L"        negative origin? .......... %ls",
                   (s->x < 0 || s->y < 0) ? L"YES - stage 3 must handle it" : L"no");
    }

    t_out_line(L"");
    t_out_line(L"  R3. Cross-check (are the coordinates real pixels?)");
    if (!g_cross_checked) {
        t_out_line(L"      NOT RUN - see the 'attempt:' lines earlier in this log for why.");
        t_out_line(L"");
        t_out_line(L"      THIS IS THE CHECK THAT MATTERS. R2 alone cannot tell you");
        t_out_line(L"      whether the reported size is real pixels or a scaled number.");
        t_out_line(L"");
        t_out_line(L"      Try again like this:");
        t_out_line(L"        1. press Win, type cmd, press Enter");
        t_out_line(L"        2. cd /d \"<the folder holding this exe>\"");
        t_out_line(L"        3. t01_dpi.exe        (no --no-pause, no > redirect)");
        t_out_line(L"        4. read the report on screen, then press any key");
    } else {
        t_out_line(L"      result ..................... %ls",
                   g_cross_ok ? L"PASS" : L"FAIL");
        t_out_line(L"      window used ................ %ls",
                   g_probe_was_console ? L"the real console window"
                                       : L"a 400x300 probe window created by this program");
        t_out_line(L"      size round-trip ............ asked 400x300, read back %ldx%ld (delta %ld,%ld)",
                   g_probe_w, g_probe_h, g_probe_dw, g_probe_dh);
        t_out_line(L"");
        t_out_line(L"      How to read this: the program asked Windows for a 400x300");
        t_out_line(L"      window and measured it back. A small delta is the window");
        t_out_line(L"      frame. A large one means Windows is scaling the numbers --");
        t_out_line(L"      which would break every calculation in stage 3.");
        if (!g_cross_ok) {
            t_out_line(L"");
            t_out_line(L"      FAIL means coordinates are being virtualized.");
            t_out_line(L"      Do NOT start stage 3 until this is resolved.");
        }
    }

    t_out_line(L"");
    t_out_line(L"  R4. Compare R2 (rcMonitor) with the real resolution");
    t_out_line(L"      Settings > System > Display > Display resolution");
    t_out_line(L"      Same number -> physical pixels confirmed.");
    t_out_line(L"      Different   -> virtualized; report it before continuing.");
    t_out_line(L"");
    t_out_line(L"================================================================");
}

int wmain(int argc, wchar_t **argv)
{
    /* Crash filter + file log first, so even a failure in the console
       setup below leaves evidence. Added after this program was reported
       to flash and close with no output in a Windows 11 VM. */
    t_diag_init(L"t01_dpi");
    t_log_mirror(1);   /* full report goes to the log by default */

    /*
     * --log-all   mirror the whole report into the log file. Use this when
     *             the console may close with the process, or when the
     *             output has to be sent to someone else.
     * --no-pause  do not wait for a key at the end (automation).
     */
    int opt_no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if      (!wcscmp(argv[i], L"--log-all"))  t_log_mirror(1);
        else if (!wcscmp(argv[i], L"--no-pause")) opt_no_pause = 1;
        else if (!wcscmp(argv[i], L"--help")) {
            t_out_line(L"usage: t01_dpi.exe [--log-all] [--no-pause]\n"
                    L"\n"
                    L"  --log-all   also write the full report to t01_dpi.log\n"
                    L"  --no-pause  exit immediately instead of waiting for a key\n");
            t_log_close();
            return 0;
        }
    }

    t_log(L"step 1: entering console init");

    t_console_init();
    t_log(L"step 2: console init done");

    T_TITLE(L"t01_dpi - PerMonitorV2 coordinate semantics");
    T_KV(L"run_at", L"%ls", t_now_string());
    T_KV(L"os", L"%ls", L"see [OS_VERSION] below");

    /* ---------------- awareness ---------------- */
    T_SEC(L"DPI awareness");
    t_log(L"step 3: setting DPI awareness");

    int v2 = t_set_dpi_awareness();
    T_KV(L"set_v2_result", L"%ls", v2 ? L"SUCCESS" : L"FAILED (fell back to system-aware)");
    if (!v2) {
        T_WARN(L"This OS/build does not support PER_MONITOR_AWARE_V2.");
        T_WARN(L"Coordinates below may be virtualized. Record this in the P0 log.");
    }
    t_log(L"step 4: awareness set, v2=%d", v2);

    T_KV(L"awareness_now", L"%ls", t_dpi_awareness_name());
    T_KV(L"awareness_enum_raw", L"%d", t_dpi_awareness_enum());
    t_log(L"step 5: awareness queried");
    T_NOTE(L"Note: the raw enum reads 2 (SYSTEM_AWARE) even when V2 is active.");
    T_NOTE(L"The enum predates V2 and cannot distinguish it from V1, so trust");
    T_NOTE(L"awareness_now, which compares context handles. This is a documented");
    T_NOTE(L"Win32 quirk, not a failure - the probe in tests/ confirmed it.");

    /* ---------------- monitors ---------------- */
    T_SEC(L"Monitors");
    t_log(L"step 6: resolving shcore and enumerating monitors");
    resolve_dpi_for_monitor();
    g_monitor_index = 0;
    EnumDisplayMonitors(NULL, NULL, monitor_proc, 0);
    T_KV(L"monitor_count", L"%d", g_monitor_index);
    t_log(L"step 7: enumerated %d monitor(s)", g_monitor_index);

    /* ---------------- cross-check with GetWindowRect ---------------- */
    T_SEC(L"Cross-check: does a real window fit inside the monitor numbers?");
    T_NOTE(L"Purpose: prove the numbers above are physical pixels, not scaled ones.");
    T_NOTE(L"Method : take a real window's rectangle and check it falls inside its");
    T_NOTE(L"         monitor's reported area. If coordinates were being virtualized,");
    T_NOTE(L"         the two would not line up.");

    /*
     * Several ways to obtain a window of our own, tried in order. The first
     * revision used GetConsoleWindow() alone and, when that returned nothing
     * usable, skipped the check with a guess about piping - which told the
     * reader nothing. Now each attempt is reported so a failure names itself.
     */
    struct {
        const wchar_t *what;
        HWND  hwnd;
        DWORD err;
        int   ok;
        int   created;   /* 1 = we own it and must destroy it */
    } tries[4];

    ZeroMemory(tries, sizeof(tries));

    /* 1. The console window, if this process really has a visible one. */
    tries[0].what = L"console window (GetConsoleWindow)";
    tries[0].hwnd = GetConsoleWindow();
    if (tries[0].hwnd) {
        RECT r;
        SetLastError(0);
        tries[0].ok = GetWindowRect(tries[0].hwnd, &r) &&
                      r.right > r.left && r.bottom > r.top;
        tries[0].err = tries[0].ok ? 0 : GetLastError();
    } else {
        tries[0].err = GetLastError();
    }

    /*
     * 2. A window we create, shown on screen. This is the important
     *    fallback: we choose its size in pixels, so reading it back
     *    immediately reveals whether the numbers are scaled.
     *
     *    Requested 400x300. Under a working PerMonitorV2 setup the read-back
     *    is ~400x300 regardless of the monitor's scale. If coordinates were
     *    being virtualized, a 150% monitor would read back ~267x200 - a
     *    difference far too large to be window-frame noise.
     */
    #define PROBE_W 400
    #define PROBE_H 300

    tries[1].what = L"probe window, 400x300 (own, on screen)";
    tries[1].hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"wac size probe",
                                    WS_POPUP | WS_VISIBLE,
                                    50, 50, PROBE_W, PROBE_H,
                                    NULL, NULL, GetModuleHandleW(NULL), NULL);
    tries[1].created = 1;
    if (tries[1].hwnd) {
        RECT r;
        SetLastError(0);
        tries[1].ok = GetWindowRect(tries[1].hwnd, &r) &&
                      r.right > r.left && r.bottom > r.top;
        tries[1].err = tries[1].ok ? 0 : GetLastError();
    } else {
        tries[1].err = GetLastError();
    }

    HWND probe = NULL;
    for (int i = 0; i < 2; ++i) {
        t_out_line(L"");
        t_out_line(L"  attempt: %ls", tries[i].what);
        t_out_line(L"    handle ......... 0x%p", (void *)tries[i].hwnd);
        t_out_line(L"    usable ......... %ls", tries[i].ok ? L"yes" : L"no");
        if (!tries[i].ok) {
            t_out_line(L"    GetLastError .. %lu", (unsigned long)tries[i].err);
        }
        if (tries[i].ok) probe = tries[i].hwnd;
    }

    if (!probe) {
        T_WARN(L"SKIPPED - no window of this process could be measured.");
        T_WARN(L"See the attempts above; the error codes identify the cause.");
        T_WARN(L"");
        T_WARN(L"How to get this check to run:");
        T_WARN(L"  - open cmd first, then type the exe name inside it");
        T_WARN(L"  - or double-click the exe from File Explorer");
        T_WARN(L"  - do NOT run it through a pipe, '>' redirect, or a script that");
        T_WARN(L"    captures its output");
    } else {
        RECT wr;
        GetWindowRect(probe, &wr);
        long got_w = wr.right - wr.left;
        long got_h = wr.bottom - wr.top;

        HMONITOR hm = MonitorFromWindow(probe, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hm, &mi)) {
            T_KV(L"probe_window_rect", L"%ld,%ld %ldx%ld",
                 wr.left, wr.top, got_w, got_h);
            T_KV(L"probe_monitor_rcMonitor", L"%ld,%ld %ldx%ld",
                 mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top);
            T_KV(L"probe_dpi_for_window", L"%lu (scale %lu%%)",
                 (unsigned long)t_get_dpi_for_window(probe),
                 (unsigned long)(t_get_dpi_for_window(probe) * 100 / 96));

            int inside = (wr.left   >= mi.rcMonitor.left &&
                          wr.top    >= mi.rcMonitor.top  &&
                          wr.right  <= mi.rcMonitor.right &&
                          wr.bottom <= mi.rcMonitor.bottom);

            /*
             * The size round-trip is the actual proof. We asked for
             * PROBE_W x PROBE_H and read back got_w x got_h. A few pixels of
             * difference is the window frame; a large one means scaling.
             */
            long dw = got_w - PROBE_W;
            long dh = got_h - PROBE_H;
            int size_ok = (dw > -8 && dw < 40) && (dh > -8 && dh < 40);

            g_cross_checked = 1;
            g_cross_ok = inside && size_ok;
            g_probe_was_console = (probe == tries[0].hwnd);
            g_probe_w = got_w;
            g_probe_h = got_h;
            g_probe_dw = dw;
            g_probe_dh = dh;

            T_KV(L"probe_requested_size", L"%dx%d", PROBE_W, PROBE_H);
            T_KV(L"probe_readback_size", L"%ldx%ld  (delta w=%ld h=%ld)",
                 got_w, got_h, dw, dh);
            T_KV(L"sanity_window_inside_monitor", L"%ls", inside ? L"PASS" : L"FAIL");
            T_KV(L"sanity_size_roundtrip", L"%ls", size_ok ? L"PASS" : L"FAIL");

            if (!inside) {
                T_WARN(L"The window does not fit inside its monitor's reported area.");
                T_WARN(L"That means the coordinates are being virtualized somewhere.");
                T_WARN(L"STOP - do not start stage 3 until this is understood.");
            }
            if (!size_ok) {
                T_WARN(L"A %dx%d window read back as %ldx%ld.",
                       PROBE_W, PROBE_H, got_w, got_h);
                T_WARN(L"That gap is far larger than a window frame, so the");
                T_WARN(L"coordinates ARE being scaled. Do not start stage 3.");
            }
        } else {
            DWORD err = GetLastError();
            T_WARN(L"GetMonitorInfoW failed on the chosen window (error %lu).", (unsigned long)err);
        }
    }

    /* Release any window we created. */
    for (int i = 0; i < 2; ++i) {
        if (tries[i].created && tries[i].hwnd) DestroyWindow(tries[i].hwnd);
    }

    /* ---------------- closing summary ---------------- */
    print_summary(v2, g_monitor_index);
    t_log(L"step 9: report complete");

    if (!opt_no_pause) {
        t_pause();
    } else {
        t_out_line(L"[pause] skipped (--no-pause)");
    }
    t_log_close();
    return 0;
}
