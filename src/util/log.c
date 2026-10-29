/*
 * log.c - append-only diagnostic log.
 *
 * See log.h for the privacy rules and the reasoning behind opening the file
 * per line.
 */

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static wchar_t g_path[MAX_PATH];   /* empty until log_init succeeds */
static int     g_ready;

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

void log_init(const wchar_t *directory)
{
    g_ready = 0;
    g_path[0] = L'\0';

    if (directory == NULL || directory[0] == L'\0') return;

    const int n = _snwprintf_s(g_path, MAX_PATH, _TRUNCATE,
                               L"%ls\\wac.log", directory);
    if (n < 0) {
        g_path[0] = L'\0';   /* path did not fit: refuse rather than truncate
                              * into a different file name */
        return;
    }
    g_ready = 1;
}

const wchar_t *log_path(void)
{
    return g_ready ? g_path : L"";
}

void log_rotate_if_large(unsigned long long max_bytes)
{
    if (!g_ready) return;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(g_path, GetFileExInfoStandard, &fad)) return;

    const unsigned long long size =
        ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if (size < max_bytes) return;

    wchar_t old[MAX_PATH];
    if (_snwprintf_s(old, MAX_PATH, _TRUNCATE, L"%ls.old", g_path) < 0) return;

    /* Replace, so the generation count stays at one. */
    MoveFileExW(g_path, old, MOVEFILE_REPLACE_EXISTING);
}

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

static const wchar_t *level_name(LogLevel level)
{
    switch (level) {
    case LOG_DEBUG: return L"debug";
    case LOG_INFO:  return L"info ";
    case LOG_WARN:  return L"WARN ";
    case LOG_ERROR: return L"ERROR";
    }
    return L"?????";
}

void log_write(LogLevel level, const wchar_t *fmt, ...)
{
    if (!g_ready) return;   /* not initialised: stay silent, never crash */

    FILE *f = NULL;
    if (_wfopen_s(&f, g_path, L"at, ccs=UTF-8") != 0 || f == NULL) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"%04u-%02u-%02u %02u:%02u:%02u %ls  ",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond,
             level_name(level));

    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);

    fwprintf(f, L"\n");
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Window references                                                   */
/* ------------------------------------------------------------------ */

const wchar_t *log_window_ref(HWND hwnd)
{
    /*
     * Rotating buffers so two references in one log_write call do not
     * clobber each other - a single static buffer would make
     * log_info(L"%ls -> %ls", ref(a), ref(b)) print the same value twice.
     */
    static wchar_t bufs[4][160];
    static int slot;

    wchar_t *b = bufs[slot];
    slot = (slot + 1) & 3;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    /*
     * Executable NAME only, never the full path: a path contains the user
     * name, and this file is meant to be safe to attach to a bug report.
     */
    wchar_t exe[64] = L"?";
    if (pid != 0) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h != NULL) {
            wchar_t full[MAX_PATH];
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, full, &n)) {
                const wchar_t *base = wcsrchr(full, L'\\');
                base = base ? base + 1 : full;
                wcsncpy_s(exe, 64, base, _TRUNCATE);
            }
            CloseHandle(h);
        }
    }

    /* The handle is included because it is what makes two lines correlatable;
     * it reveals nothing about the user's work. */
    _snwprintf_s(b, 160, _TRUNCATE, L"%ls(hwnd=0x%p)", exe, (void *)hwnd);
    return b;
}
