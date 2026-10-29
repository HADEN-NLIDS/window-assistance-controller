/*
 * test_common.h - shared helpers for the stage-1 smoke test programs.
 *
 * Header-only on purpose: every test program must remain a SINGLE .c file
 * that can be compiled on its own, so nothing here may require a .c file.
 * All functions are static inline or macros.
 *
 * These programs are throwaway measurement tools. They are allowed to use
 * constructs the product code must not use (polling timers, direct console
 * output, no cleanup registry). Every such shortcut is marked with
 * "TEST-ONLY" so it never gets copied into src/.
 *
 * Output format is deliberately `[TAG] value` lines rather than markdown
 * tables: console columns never align with non-ASCII text, and this format
 * is greppable and pastes into docs/P0验证记录.md with minimal editing.
 */

#ifndef WAC_TEST_COMMON_H
#define WAC_TEST_COMMON_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

/* ------------------------------------------------------------------ */
/* Console output                                                      */
/* ------------------------------------------------------------------ */

/*
 * Variadic on purpose.
 *
 * A fixed-arity two-argument macro breaks whenever the VALUE argument
 * contains a top-level comma, which a `cond ? a : b` expression does not
 * protect against in MSVC's traditional preprocessor. Using
 * (fmt, ...) keeps the format string in its own parameter and lets the
 * value be anything, including a ternary.
 *
 * MSVC needs /Zc:preprocessor for fully conforming variadic macros; the
 * build script passes it. Without it, keep ternaries out of these.
 */
/*
 * Output macros.
 *
 * They route through t_out() rather than calling wprintf directly, so the
 * same text can also land in the log file. That matters when the console
 * window closes with the process: the log becomes the only record of the
 * measurements, not just of the steps.
 *
 * Mirroring is enabled by t_log_mirror(1) - see --log-all in the programs.
 */
static FILE *g_tlog;
static int   g_tlog_mirror;

static inline void t_out(const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, 2048, _TRUNCATE, fmt, ap);
    va_end(ap);

    wprintf(L"%ls", buf);
    if (g_tlog_mirror && g_tlog) {
        fwprintf(g_tlog, L"%ls", buf);
        fflush(g_tlog);
    }
}

static inline void t_out_line(const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, 2048, _TRUNCATE, fmt, ap);
    va_end(ap);

    wprintf(L"%ls\n", buf);
    if (g_tlog_mirror && g_tlog) {
        fwprintf(g_tlog, L"%ls\n", buf);
        fflush(g_tlog);
    }
}

#define T_TITLE(...)  do { t_out_line(L"\n=== " __VA_ARGS__); } while (0)
#define T_SEC(...)    do { t_out_line(L"\n--- " __VA_ARGS__); } while (0)
#define T_KV(k, ...)  do { t_out(L"[" k L"] " __VA_ARGS__); t_out(L"\n"); } while (0)
#define T_NOTE(...)   do { t_out(L"  " __VA_ARGS__); t_out(L"\n"); } while (0)
#define T_WARN(...)   do { t_out(L"  !! " __VA_ARGS__); t_out(L"\n"); } while (0)

/* Print a Win32 failure with the decoded error code. */
static inline void t_winerr(const wchar_t *what, DWORD err)
{
    wchar_t buf[512];
    buf[0] = L'\0';
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   buf, (DWORD)(sizeof(buf) / sizeof(buf[0])), NULL);
    /* strip trailing CR/LF that FormatMessage appends */
    for (wchar_t *p = buf; *p; ++p) { if (*p == L'\r' || *p == L'\n') { *p = L'\0'; break; } }
    t_out_line(L"  !! %ls failed: GetLastError=%lu (%ls)", what, err, buf);
}

#define T_LASTERR(what) t_winerr((what), GetLastError())

/* ------------------------------------------------------------------ */
/* File logging + crash capture                                        */
/*                                                                     */
/* Added because t01_dpi.exe was reported to flash and close with no    */
/* output inside a Windows 11 VM while working on the host. A console   */
/* that vanishes takes its output with it, so every program now also    */
/* tees to a log file next to the exe and installs an exception filter. */
/*                                                                     */
/* If the program still dies silently in the VM, the log file names the */
/* last step reached, and a crash names the faulting address and module.*/
/* ------------------------------------------------------------------ */

static FILE *g_tlog = NULL;

/* Defined near the bottom of this header; declared here because the logging
   helpers below need it and C requires a declaration before use. */
static inline const wchar_t *t_now_string(void);

/* Append a UTF-8 line to the log file (if open) and flush immediately, so
   a crash cannot lose the tail. */
static inline void t_log(const wchar_t *fmt, ...)
{
    if (!g_tlog) return;
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, 1024, _TRUNCATE, fmt, ap);
    va_end(ap);
    fwprintf(g_tlog, L"%ls\n", buf);
    fflush(g_tlog);
}

/*
 * Open <exe-dir>\<exe-name>.log for writing.
 *
 * Deliberately writes next to the executable rather than to %TEMP%: in a
 * VM the interesting case is "the program never got far enough to do
 * anything", and a file that appears beside the exe is impossible to miss.
 * TEST-ONLY - the product writes to %APPDATA% and uses atomic replace.
 */
static inline void t_log_open(const wchar_t *exe_name)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;

    /* Copy the real exe path for reporting before rewriting it into a log
       path - otherwise the log claims its own path is the executable. */
    wchar_t real_exe[MAX_PATH];
    wcsncpy_s(real_exe, MAX_PATH, path, _TRUNCATE);

    /* strip the file name, keep the directory */
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) return;
    slash[1] = L'\0';

    wcsncat_s(path, MAX_PATH, exe_name, _TRUNCATE);
    wcsncat_s(path, MAX_PATH, L".log", _TRUNCATE);

    g_tlog = _wfopen(path, L"wt, ccs=UTF-8");
    if (g_tlog) {
        t_log(L"=== log opened %ls ===", t_now_string());
        t_log(L"exe     : %ls", real_exe);
        t_log(L"logfile : %ls", path);
    }
}

static inline void t_log_close(void)
{
    if (g_tlog) {
        t_log(L"=== log closed %ls ===", t_now_string());
        fclose(g_tlog);
        g_tlog = NULL;
    }
}

/*
 * Open the log file in Notepad.
 *
 * Added because a beginner reasonably sent back the 0-byte .crash.txt
 * instead of the .log: the two files sit side by side and neither name
 * says which one holds the results. Rather than explain it again, the
 * programs can open the right one themselves.
 *
 * Saved before launching so the file on disk is complete.
 */
static inline void t_open_log_in_notepad(void)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;

    wchar_t *dot = wcsrchr(path, L'.');
    if (dot) *dot = L'\0';
    wcsncat_s(path, MAX_PATH, L".log", _TRUNCATE);

    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;

    wchar_t cmd[MAX_PATH + 32];
    _snwprintf_s(cmd, MAX_PATH + 32, _TRUNCATE, L"notepad.exe \"%ls\"", path);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_UNICODE_ENVIRONMENT, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

/*
 * Also write the report text (T_KV / T_NOTE / ... output) into the log.
 *
 * Without this the log records only the steps, so when the console window
 * closes with the process the measurements are lost. Enable with --log-all;
 * it is off by default so an interactive run does not duplicate everything.
 */
static inline void t_log_mirror(int on)
{
    g_tlog_mirror = on ? 1 : 0;
}

/*
 * Unhandled-exception filter.
 *
 * Installed first thing in main(). Without it a crash in a console app
 * just closes the window, which is exactly the symptom being investigated.
 * Installed via SetUnhandledExceptionFilter rather than AddVectoredException
 * Handler so it runs only when the exception is genuinely unhandled.
 *
 * Writes with WriteFile on a pre-opened handle instead of wprintf: if the
 * crash happened inside the CRT's stdio, calling back into stdio here could
 * deadlock or crash again.
 */
static HANDLE g_tcrash = INVALID_HANDLE_VALUE;

static void t_write_raw(const char *s)
{
    if (g_tcrash == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(g_tcrash, s, (DWORD)strlen(s), &written, NULL);
}

static LONG WINAPI t_crash_filter(EXCEPTION_POINTERS *ep)
{
    char buf[512];
    const char *name = "unknown";

    switch (ep->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:         name = "ACCESS_VIOLATION"; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:      name = "ILLEGAL_INSTRUCTION"; break;
    case EXCEPTION_STACK_OVERFLOW:           name = "STACK_OVERFLOW"; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       name = "INT_DIVIDE_BY_ZERO"; break;
    case EXCEPTION_PRIV_INSTRUCTION:         name = "PRIV_INSTRUCTION"; break;
    case EXCEPTION_IN_PAGE_ERROR:            name = "IN_PAGE_ERROR"; break;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    name = "ARRAY_BOUNDS_EXCEEDED"; break;
    default: break;
    }

    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "\r\n*** CRASH ***\r\n"
                "  code    : 0x%08lX (%s)\r\n"
                "  address : 0x%p\r\n"
                "  thread  : %lu\r\n",
                (unsigned long)ep->ExceptionRecord->ExceptionCode, name,
                ep->ExceptionRecord->ExceptionAddress,
                (unsigned long)GetCurrentThreadId());
    t_write_raw(buf);

    /* For access violations, parameter 0 says read/write and 1 the address. */
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "  op      : %s at 0x%p\r\n",
                    ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ",
                    (void *)ep->ExceptionRecord->ExceptionInformation[1]);
        t_write_raw(buf);
    }

    /* Which module owns the faulting address? Names the culprit directly. */
    HMODULE mod = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &mod)) {
        wchar_t wpath[MAX_PATH];
        if (GetModuleFileNameW(mod, wpath, MAX_PATH)) {
            char a[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, wpath, -1, a, sizeof(a), NULL, NULL);
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "  module  : %s\r\n", a);
            t_write_raw(buf);
        }
    }

    t_write_raw("*** END CRASH ***\r\n");
    return EXCEPTION_EXECUTE_HANDLER;
}

/* Call at the very top of main(), before anything else. */
static inline void t_diag_init(const wchar_t *exe_name)
{
    /* Raw handle for the crash path. Independent of the stdio log so a
       stdio failure cannot suppress the crash report. */
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash) {
            slash[1] = L'\0';
            wcsncat_s(path, MAX_PATH, exe_name, _TRUNCATE);
            wcsncat_s(path, MAX_PATH, L".crash.txt", _TRUNCATE);
            g_tcrash = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        }
    }
    SetUnhandledExceptionFilter(t_crash_filter);
    t_log_open(exe_name);
}

/* ------------------------------------------------------------------ */
/* Console setup                                                       */
/* ------------------------------------------------------------------ */

/*
 * Make UTF-8 output work and give the console a readable codepage.
 * Call once at the top of main(), after t_diag_init().
 *
 * Every step is failure-tolerant: a VM console can have a different buffer
 * size or no visible window at all, and none of that should stop the
 * program from producing its measurements.
 */
static inline void t_console_init(void)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    /*
     * wprintf() converts wide characters to bytes through the C locale, and
     * the default "C" locale can represent NOTHING outside ASCII - so every
     * Chinese window title becomes '?' as soon as stdout is a pipe or a file
     * instead of a console. SetConsoleOutputCP above only covers the console
     * case, which is why this went unnoticed until the output was captured.
     *
     * ".UTF-8" is available from Windows 10 1803; if it ever fails the
     * previous behaviour is kept, so this cannot make an older machine worse.
     */
    setlocale(LC_ALL, ".UTF-8");

    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);

    /* Grow the console buffer so the full report is scrollable. The API
       fails if the requested height is smaller than the current one, so
       only ever grow, and ignore failure entirely. */
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(h, &info)) {
        COORD size;
        size.X = (SHORT)(info.dwSize.X < 120 ? 120 : info.dwSize.X);
        size.Y = (SHORT)(info.dwSize.Y < 3000 ? 3000 : info.dwSize.Y);
        if (size.X != info.dwSize.X || size.Y != info.dwSize.Y) {
            SetConsoleScreenBufferSize(h, size);   /* best effort */
        }
    }

    /* Report the environment - this is the first thing a VM diagnosis needs. */
    t_log(L"console : GetConsoleWindow=%p stdout=%p",
          (void *)GetConsoleWindow(), (void *)h);
    t_log(L"codepage: output=%u input=%u",
          (unsigned)GetConsoleOutputCP(), (unsigned)GetConsoleCP());

    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(NULL, exe, MAX_PATH)) t_log(L"exe     : %ls", exe);
}

/* ------------------------------------------------------------------ */
/* DPI awareness                                                       */
/* ------------------------------------------------------------------ */

typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);

/*
 * Opt into PerMonitorV2 at runtime.
 *
 * The product code declares this in the manifest AND at runtime; the test
 * programs do it only at runtime so they can be compiled without a manifest
 * and still speak physical pixels. Win10 1703+ is required for V2.
 *
 * Returns 1 on success, 0 if the OS is too old (test output stays valid but
 * may be in virtualized coordinates, which the caller should report).
 *
 * TWO TRAPS ARE HANDLED HERE - both were hit in practice:
 *
 * 1. GetModuleHandleW(L"user32.dll") returns NULL in a program that never
 *    references a user32 export. The linker only records libraries that are
 *    actually needed, so user32 is simply not loaded yet. LoadLibraryW
 *    forces it in. Using GetModuleHandleW made this function silently fall
 *    back to SetProcessDPIAware() and the caller reported SYSTEM_AWARE.
 *
 * 2. There is no guarantee GetModuleHandleW("user32.dll") finds it under
 *    that exact name either, so LoadLibraryW is the only safe lookup.
 */
static inline int t_set_dpi_awareness(void)
{
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    if (user32) {
        PFN_SetProcessDpiAwarenessContext fn =
            (PFN_SetProcessDpiAwarenessContext)(void *)
            GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (fn) {
            /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4 */
            if (fn((HANDLE)(INT_PTR)-4)) return 1;
        }
    }
    /* Fall back to plain per-monitor, then system aware, so the numbers are
       at least better than DPI-unaware. Reported by the caller either way. */
    SetProcessDPIAware();
    return 0;
}

/*
 * Report the process DPI awareness as text.
 *
 * TRAP: GetAwarenessFromDpiAwarenessContext() returns SYSTEM_AWARE (2) for a
 * process that is actually PerMonitorV2. V2 is internally "per-monitor plus
 * extra behaviour", and the enum predates it, so the numeric value does not
 * distinguish V1 from V2. The documented way to tell them apart is
 * AreDpiAwarenessContextsEqual() against the context constants.
 *
 * Reporting the enum alone made a working V2 setup look broken. Always
 * compare contexts instead.
 */
static inline const wchar_t *t_dpi_awareness_name(void)
{
    typedef HANDLE (WINAPI *PFN_GetThreadDpiAwarenessContext)(void);
    typedef int    (WINAPI *PFN_GetAwarenessFromDpiAwarenessContext)(HANDLE);
    typedef BOOL   (WINAPI *PFN_AreDpiAwarenessContextsEqual)(HANDLE, HANDLE);

    HMODULE user32 = LoadLibraryW(L"user32.dll");
    if (!user32) return L"(user32 unavailable)";

    PFN_GetThreadDpiAwarenessContext p_get =
        (PFN_GetThreadDpiAwarenessContext)(void *)
        GetProcAddress(user32, "GetThreadDpiAwarenessContext");
    PFN_GetAwarenessFromDpiAwarenessContext p_aw =
        (PFN_GetAwarenessFromDpiAwarenessContext)(void *)
        GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext");
    PFN_AreDpiAwarenessContextsEqual p_eq =
        (PFN_AreDpiAwarenessContextsEqual)(void *)
        GetProcAddress(user32, "AreDpiAwarenessContextsEqual");

    if (!p_get) return L"(query API unavailable)";

    HANDLE ctx = p_get();
    (void)p_aw;   /* deliberately unused: it cannot tell V1 from V2 */

    if (p_eq) {
        if (p_eq(ctx, (HANDLE)(INT_PTR)-4)) return L"PER_MONITOR_AWARE_V2  <- what we want";
        if (p_eq(ctx, (HANDLE)(INT_PTR)-3)) return L"PER_MONITOR_AWARE (V1)";
        if (p_eq(ctx, (HANDLE)(INT_PTR)-2)) return L"SYSTEM_AWARE";
        if (p_eq(ctx, (HANDLE)(INT_PTR)-1)) return L"UNAWARE (coordinates are virtualized - BAD)";
    }
    return L"(unrecognised context)";
}

/* The raw enum value, reported alongside the name for completeness. */
static inline int t_dpi_awareness_enum(void)
{
    typedef HANDLE (WINAPI *PFN_GetThreadDpiAwarenessContext)(void);
    typedef int    (WINAPI *PFN_GetAwarenessFromDpiAwarenessContext)(HANDLE);

    HMODULE user32 = LoadLibraryW(L"user32.dll");
    if (!user32) return -1;
    PFN_GetThreadDpiAwarenessContext p_get =
        (PFN_GetThreadDpiAwarenessContext)(void *)
        GetProcAddress(user32, "GetThreadDpiAwarenessContext");
    PFN_GetAwarenessFromDpiAwarenessContext p_aw =
        (PFN_GetAwarenessFromDpiAwarenessContext)(void *)
        GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext");
    if (!p_get || !p_aw) return -1;
    return p_aw(p_get());
}

static inline DWORD t_get_dpi_for_window(HWND hwnd)
{
    typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow fn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) fn = (PFN_GetDpiForWindow)(void *)GetProcAddress(user32, "GetDpiForWindow");
    }
    if (fn && hwnd) return fn(hwnd);
    return 96;
}

/* ------------------------------------------------------------------ */
/* Window helpers                                                      */
/* ------------------------------------------------------------------ */

/* Fetch a window's title. Returns a pointer to a static rotating buffer. */
static inline const wchar_t *t_window_title(HWND hwnd)
{
    static wchar_t bufs[4][256];
    static int slot = 0;
    wchar_t *b = bufs[slot];
    slot = (slot + 1) & 3;
    b[0] = L'\0';
    if (!GetWindowTextW(hwnd, b, 256)) b[0] = L'\0';
    return b;
}

/* Fetch the owning process id for a window. Returns 0 on failure. */
static inline DWORD t_window_pid(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}

/* Fetch the executable file name for a pid. Returns "" on failure. */
static inline const wchar_t *t_process_name(DWORD pid)
{
    static wchar_t bufs[4][128];
    static int slot = 0;
    wchar_t *b = bufs[slot];
    slot = (slot + 1) & 3;
    b[0] = L'\0';
    if (!pid) return b;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return b;
    wchar_t full[MAX_PATH];
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, full, &n)) {
        const wchar_t *base = wcsrchr(full, L'\\');
        base = base ? base + 1 : full;
        wcsncpy_s(b, 128, base, _TRUNCATE);
    }
    CloseHandle(h);
    return b;
}

/* Case-insensitive substring search. Returns 1 when needle is empty. */
static inline int t_contains_i(const wchar_t *hay, const wchar_t *needle)
{
    if (!needle || !*needle) return 1;
    if (!hay) return 0;
    size_t nl = wcslen(needle);
    for (const wchar_t *p = hay; *p; ++p) {
        if (_wcsnicmp(p, needle, nl) == 0) return 1;
    }
    return 0;
}

/*
 * Why a top-level window was rejected as a candidate.
 *
 * The first version returned a bare yes/no and silently dropped everything
 * that failed. When a user could see a window on screen that the tool never
 * listed (a UU accelerator panel), there was no way to find out why. The
 * filter now reports its reason, and programs can print the rejected set.
 */
typedef enum {
    TFILTER_OK = 0,
    TFILTER_NOT_VISIBLE,
    TFILTER_OWNED,          /* has an owner window - dialog or popup */
    TFILTER_TOOLWINDOW,     /* WS_EX_TOOLWINDOW */
    TFILTER_NOACTIVATE,     /* WS_EX_NOACTIVATE */
    TFILTER_NO_RECT,
    TFILTER_TOO_SMALL,
    TFILTER_EMPTY_TITLE,
    TFILTER_CLOAKED         /* DWM cloaked: UWP suspended, other desktop */
} TFilterReason;

static inline const wchar_t *t_filter_reason_name(TFilterReason r)
{
    switch (r) {
    case TFILTER_OK:           return L"accepted";
    case TFILTER_NOT_VISIBLE:  return L"not visible (IsWindowVisible=false)";
    case TFILTER_OWNED:        return L"has an owner window (dialog/popup)";
    case TFILTER_TOOLWINDOW:   return L"WS_EX_TOOLWINDOW (tool palette)";
    case TFILTER_NOACTIVATE:   return L"WS_EX_NOACTIVATE (never takes focus)";
    case TFILTER_NO_RECT:      return L"GetWindowRect failed";
    case TFILTER_TOO_SMALL:    return L"smaller than 80x60";
    case TFILTER_EMPTY_TITLE:  return L"empty title";
    case TFILTER_CLOAKED:      return L"DWM-cloaked (suspended/other desktop)";
    }
    return L"(unknown)";
}

/*
 * Classify a top-level window. Returns TFILTER_OK when it is a plausible
 * "app window" (not a tooltip, not cloaked, ...).
 *
 * Note this is deliberately conservative: the product only ever moves
 * windows the user explicitly picked, but the *listing* should not hide
 * things the user can see. Rejected windows are reported, not dropped.
 */
static inline TFilterReason t_classify_window(HWND hwnd)
{
    if (!IsWindowVisible(hwnd)) return TFILTER_NOT_VISIBLE;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return TFILTER_OWNED;

    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TFILTER_TOOLWINDOW;
    if (ex & WS_EX_NOACTIVATE) return TFILTER_NOACTIVATE;

    RECT r;
    if (!GetWindowRect(hwnd, &r)) return TFILTER_NO_RECT;
    if ((r.right - r.left) < 80 || (r.bottom - r.top) < 60) return TFILTER_TOO_SMALL;

    if (GetWindowTextLengthW(hwnd) == 0) return TFILTER_EMPTY_TITLE;

    /* Skip cloaked windows (UWP suspended apps, virtual desktop ghosts). */
    typedef HRESULT (WINAPI *PFN_DwmGetWindowAttribute)(HWND, DWORD, PVOID, DWORD);
    static PFN_DwmGetWindowAttribute fn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm) fn = (PFN_DwmGetWindowAttribute)(void *)GetProcAddress(dwm, "DwmGetWindowAttribute");
    }
    if (fn) {
        BOOL cloaked = FALSE;
        /* DWMWA_CLOAKED == 14 */
        if (SUCCEEDED(fn(hwnd, 14, &cloaked, sizeof(cloaked))) && cloaked) return TFILTER_CLOAKED;
    }
    return TFILTER_OK;
}

/* Convenience wrapper kept for callers that only need yes/no. */
static inline int t_is_app_window(HWND hwnd)
{
    return t_classify_window(hwnd) == TFILTER_OK;
}

typedef struct {
    HWND  hwnd;
    RECT  rect;
    DWORD pid;
} TWindowInfo;

typedef struct {
    TWindowInfo *items;
    int          count;
    int          cap;
} TWindowList;

static BOOL CALLBACK t_enum_proc(HWND hwnd, LPARAM lp)
{
    TWindowList *list = (TWindowList *)lp;
    if (!t_is_app_window(hwnd)) return TRUE;
    if (list->count >= list->cap) return FALSE;

    TWindowInfo *w = &list->items[list->count];
    w->hwnd = hwnd;
    w->pid  = t_window_pid(hwnd);
    if (!GetWindowRect(hwnd, &w->rect)) return TRUE;
    list->count++;
    return TRUE;
}

/* Enumerate plausible app windows. Caller frees list->items. */
static inline int t_enum_app_windows(TWindowList *list, int cap)
{
    list->items = (TWindowInfo *)calloc((size_t)cap, sizeof(TWindowInfo));
    list->count = 0;
    list->cap   = cap;
    if (!list->items) return 0;
    EnumWindows(t_enum_proc, (LPARAM)list);
    return list->count;
}

/* ------------------------------------------------------------------ */
/* Small utilities                                                     */
/* ------------------------------------------------------------------ */

/* Timestamp for reports, so snapshots can be told apart. */
static inline const wchar_t *t_now_string(void)
{
    static wchar_t buf[32];
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snwprintf_s(buf, 32, _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

/*
 * Hold the console open until a key is pressed.
 *
 * This is the difference between "the program flashed and closed" and a
 * readable report, so it is deliberately hard to skip:
 *
 *   - the prompt goes to the console handle directly as well as to stdout,
 *     because a double-clicked console app can end up with a stdout handle
 *     that is not the visible console;
 *   - a fallback timer stops it hanging forever in automation, but only
 *     after a full minute, and only after printing how to skip the wait.
 *
 * Pass --no-pause to skip entirely (see each program's argument parsing).
 */
static inline void t_pause(void)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);

    DWORD mode = 0;
    int have_console = GetConsoleMode(in, &mode) ? 1 : 0;

    t_out_line(L"");   /* mirrored, so the log shows the wait too */
    t_out_line(L"--- press any key to exit (or wait 60s) ---");

    if (!have_console) {
        t_out_line(L"[pause] stdin is not a console - waiting 3s then exiting");
        Sleep(3000);
        return;
    }

    FlushConsoleInputBuffer(in);

    DWORD start = GetTickCount();
    for (;;) {
        /* Peek without consuming: leaves the input buffer clean. */
        INPUT_RECORD rec;
        DWORD avail = 0;
        if (GetNumberOfConsoleInputEvents(in, &avail) && avail > 0) {
            DWORD read = 0;
            if (ReadConsoleInputW(in, &rec, 1, &read) && read == 1) {
                if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) break;
            }
            continue;
        }
        if (GetTickCount() - start > 60000) {
            t_out_line(L"[pause] 60s elapsed - exiting");
            break;
        }
        Sleep(50);
    }
}

/*
 * Backwards-compatible alias. Earlier revisions returned immediately when
 * stdin was not a console, which is exactly the case that produced "no
 * output" reports, so the behaviour is now the stricter t_pause().
 */
static inline void t_pause_if_console(void)
{
    t_pause();
}

/* Print the current foreground window, used to prove we did not steal focus. */
static inline void t_report_foreground(const wchar_t *label)
{
    t_out_line(L"  %ls: hwnd=0x%p pid=%lu title=\"%ls\"",
               label, (void *)GetForegroundWindow(),
               (unsigned long)t_window_pid(GetForegroundWindow()),
               t_window_title(GetForegroundWindow()));
}

#endif /* WAC_TEST_COMMON_H */
