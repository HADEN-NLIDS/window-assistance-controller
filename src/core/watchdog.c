/*
 * watchdog.c - see watchdog.h.
 */

#include "watchdog.h"

#include "../util/log.h"

static HANDLE            g_thread;
static HANDLE            g_event;
static volatile LONG     g_stop;
static volatile LONG     g_fired;
static volatile LONG64   g_target;
static volatile LONG64   g_last_tick;

BOOL watchdog_should_fire(ULONGLONG now, ULONGLONG last_tick, DWORD timeout_ms)
{
    if (now <= last_tick) return FALSE;   /* the clock did not move forward */
    return (now - last_tick) > (ULONGLONG)timeout_ms;
}

static DWORD WINAPI watchdog_thread(LPVOID param)
{
    (void)param;

    for (;;) {
        const DWORD w = WaitForSingleObject(g_event, WATCHDOG_POLL_MS);

        if (InterlockedCompareExchange(&g_stop, 1, 1) != 0) break;
        if (w != WAIT_TIMEOUT) continue;   /* heartbeat arrived: nothing to do */

        const ULONGLONG now  = GetTickCount64();
        const ULONGLONG last = (ULONGLONG)InterlockedCompareExchange64(&g_last_tick, 0, 0);

        if (!watchdog_should_fire(now, last, WATCHDOG_TIMEOUT_MS)) continue;

        /* One action per session, whichever check notices first. */
        if (InterlockedExchange(&g_fired, 1) != 0) break;

        const HWND target = (HWND)(LONG_PTR)InterlockedCompareExchange64(&g_target, 0, 0);
        if (target == NULL) break;

        log_info(L"watchdog: no heartbeat for %llu ms - hiding the overlay",
                 (unsigned long long)(now - last));

        /*
         * Hide and close, both asynchronously. SWP_ASYNCWINDOWPOS matters here:
         * the UI thread may be blocked, and a synchronous ShowWindow from this
         * thread blocks the guard as well - measured 6 s against a suspended
         * thread, which is exactly the case this exists for. The window itself
         * cannot disappear while its own thread is frozen (Windows makes the
         * owning thread do it); it goes away the moment that thread pumps
         * again, together with the WM_CLOSE below.
         */
        SetWindowPos(target, NULL, 0, 0, 0, 0,
                     SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE |
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        PostMessageW(target, WM_CLOSE, 0, 0);
        break;
    }

    return 0;
}

BOOL watchdog_start(HWND overlay)
{
    if (g_thread != NULL) return TRUE;   /* already running */

    InterlockedExchange(&g_stop, 0);
    InterlockedExchange(&g_fired, 0);
    InterlockedExchange64(&g_target, (LONG64)(LONG_PTR)overlay);
    InterlockedExchange64(&g_last_tick, (LONG64)GetTickCount64());

    g_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (g_event == NULL) return FALSE;

    g_thread = CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
    if (g_thread == NULL) {
        CloseHandle(g_event);
        g_event = NULL;
        return FALSE;
    }

    return TRUE;
}

void watchdog_stop(void)
{
    if (g_thread != NULL) {
        InterlockedExchange(&g_stop, 1);
        if (g_event != NULL) SetEvent(g_event);
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }

    if (g_event != NULL) {
        CloseHandle(g_event);
        g_event = NULL;
    }

    InterlockedExchange64(&g_target, 0);
}

void watchdog_tick(void)
{
    if (g_thread == NULL || g_event == NULL) return;
    InterlockedExchange64(&g_last_tick, (LONG64)GetTickCount64());
    SetEvent(g_event);
}

BOOL watchdog_fired(void)
{
    return (InterlockedCompareExchange(&g_fired, 1, 1) != 0) ? TRUE : FALSE;
}
