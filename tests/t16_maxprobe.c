/*
 * t16_maxprobe.c - measure what Windows actually does with ptMaxSize.
 *
 * WHY THIS EXISTS
 *
 *   frame_compute_maximized_rect() insets the work area by the frame thickness
 *   to cancel the border Windows adds around a maximised window that still
 *   carries WS_THICKFRAME. Two measurements on this machine disagreed with
 *   that model:
 *
 *     compensation  0 -> window (-11,-11)-(2571,1611)  2582x1622  (+22 px)
 *     compensation 11 -> window  (11, 11)-(2549,1589)  2538x1578  (-22 px)
 *
 *   The second result is exactly the input, which no simple "add the border"
 *   model explains. So this program varies the compensation and reports the
 *   window rectangle for each value - one run per value, driven from outside -
 *   and the answer comes from measurement instead of argument.
 *
 *   The window here carries the same styles as the product window and answers
 *   WM_NCCALCSIZE the same way (client area == window), so what it measures is
 *   what the product gets.
 *
 * USAGE
 *   t16_maxprobe.exe <compensation-px> [--no-pause]
 *   Prints: [border] N  [window] L,T-R,B  [size] WxH  [work] WxH
 *
 * This is a measurement tool, not a test: it asserts nothing and is not part
 * of the regular test run.
 */

#include "test_common.h"

#include "../src/ui/frame.h"

static int g_client_fix;      /* 1 = clamp without the origin shift, 2 = the product's version */

static LRESULT CALLBACK probe_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCCALCSIZE: {
        if (wp != TRUE) break;

        NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;

        if (g_client_fix && IsZoomed(hwnd)) {
            /*
             * Candidate fix: Windows makes the window rectangle one frame
             * thickness larger than the work area on every side. Keep the
             * CLIENT area at the work area, so the visible content stops at
             * the taskbar even though the window rectangle extends past it.
             */
            const RECT window_rect = p->rgrc[0];
            const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi;
            ZeroMemory(&mi, sizeof(mi));
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) {
                const int bx = GetSystemMetricsForDpi(SM_CXSIZEFRAME, 144)
                             + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, 144);
                const int shift = (g_client_fix == 2) ? bx : 0;

                p->rgrc[0].left   = mi.rcWork.left - window_rect.left - shift;
                p->rgrc[0].top    = mi.rcWork.top  - window_rect.top  - shift;
                p->rgrc[0].right  = mi.rcWork.right  - window_rect.left - shift;
                p->rgrc[0].bottom = mi.rcWork.bottom - window_rect.top  - shift;

                T_KV(L"nccalc", L"border %d win %d,%d-%d,%d -> client %d,%d-%d,%d",
                     bx,
                     window_rect.left, window_rect.top,
                     window_rect.right, window_rect.bottom,
                     p->rgrc[0].left, p->rgrc[0].top,
                     p->rgrc[0].right, p->rgrc[0].bottom);
            }
        }
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;

        /* Let the system fill its defaults first, like the product does. */
        DefWindowProcW(hwnd, msg, wp, lp);

        const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        ZeroMemory(&mi, sizeof(mi));
        mi.cbSize = sizeof(mi);

        if (GetMonitorInfoW(mon, &mi)) {
            frame_compute_maximized_rect(&mi.rcWork, &mi.rcMonitor,
                                         &mmi->ptMaxPosition, &mmi->ptMaxSize);
            T_KV(L"asked", L"pos(%d,%d) size %dx%d",
                 mmi->ptMaxPosition.x, mmi->ptMaxPosition.y,
                 mmi->ptMaxSize.x, mmi->ptMaxSize.y);
        }
        return 0;
    }

    case WM_DESTROY:
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

int wmain(int argc, wchar_t **argv)
{
    int no_pause = 0;
    g_client_fix = 0;

    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) { no_pause = 1; continue; }
        if (!wcscmp(argv[i], L"--client-fix"))  { g_client_fix = 1; continue; }
        if (!wcscmp(argv[i], L"--client-fix2")) { g_client_fix = 2; continue; }
    }

    t_diag_init(L"t16_maxprobe");
    t_console_init();

    /*
     * The product's manifest declares PerMonitorV2; the test programs do not
     * carry that manifest, so it has to be requested at run time - before any
     * window exists. Without it every coordinate this program reads is
     * virtualised for the system DPI and the numbers are meaningless.
     */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    T_TITLE(L"t16_maxprobe - maximised window geometry probe");
    T_KV(L"compare", L"client-area clamp %ls",
         g_client_fix == 0 ? L"off (what the frame would do without it)"
       : g_client_fix == 1 ? L"on, no origin shift"
                           : L"on, with origin shift (the product's version)");

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = probe_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"WacMaxProbe";
    if (RegisterClassExW(&wc) == 0) {
        T_KV(L"VERDICT", L"FAIL - could not register the window class");
        t_log_close();
        return 1;
    }

    HWND hwnd = CreateWindowExW(0, L"WacMaxProbe", L"probe",
                                WS_OVERLAPPEDWINDOW,
                                100, 100, 800, 600,
                                NULL, NULL, wc.hInstance, NULL);
    if (hwnd == NULL) {
        T_KV(L"VERDICT", L"FAIL - could not create the window");
        t_log_close();
        return 1;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    Sleep(300);

    /* Maximise the way the shell does. */
    SendMessageW(hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
    Sleep(500);

    RECT wr;
    GetWindowRect(hwnd, &wr);

    /* The client area in SCREEN coordinates: what the user actually sees. */
    RECT cr;
    GetClientRect(hwnd, &cr);
    POINT origin = { 0, 0 };
    ClientToScreen(hwnd, &origin);
    const RECT client_screen = { origin.x, origin.y,
                                 origin.x + cr.right, origin.y + cr.bottom };

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    T_KV(L"window", L"%d,%d - %d,%d", wr.left, wr.top, wr.right, wr.bottom);
    T_KV(L"size",   L"%dx%d", wr.right - wr.left, wr.bottom - wr.top);
    T_KV(L"client", L"%d,%d - %d,%d  (%dx%d)",
         client_screen.left, client_screen.top,
         client_screen.right, client_screen.bottom,
         client_screen.right - client_screen.left,
         client_screen.bottom - client_screen.top);
    T_KV(L"work",   L"%d,%d - %d,%d  (%dx%d)",
         mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom,
         mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top);
    T_KV(L"client==work", L"%ls",
         (client_screen.left == mi.rcWork.left &&
          client_screen.top == mi.rcWork.top &&
          client_screen.right == mi.rcWork.right &&
          client_screen.bottom == mi.rcWork.bottom) ? L"YES" : L"no");
    T_KV(L"window==work", L"%ls",
         ((wr.right - wr.left) == (mi.rcWork.right - mi.rcWork.left) &&
          (wr.bottom - wr.top) == (mi.rcWork.bottom - mi.rcWork.top) &&
          wr.left == mi.rcWork.left && wr.top == mi.rcWork.top) ? L"YES" : L"no");

    DestroyWindow(hwnd);

    if (!no_pause) t_pause();

    t_log_close();
    return 0;
}
