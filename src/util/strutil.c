/*
 * strutil.c - string helpers used across the program.
 *
 * See strutil.h for the encoding contract and the safety rules.
 */

#include "strutil.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Conversion                                                          */
/* ------------------------------------------------------------------ */

BOOL str_to_wide(const char *src, wchar_t *dst, size_t dst_cch)
{
    if (dst == NULL || dst_cch == 0) return FALSE;
    dst[0] = L'\0';
    if (src == NULL) return FALSE;

    /*
     * MB_ERR_INVALID_CHARS: a config file that is not valid UTF-8 must be
     * reported, not silently converted with replacement characters. A
     * half-decoded workspace name would then be written back on the next
     * save, turning a one-off corruption into a permanent one.
     */
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      src, -1, dst, (int)dst_cch);
    if (n > 0) return TRUE;

    dst[0] = L'\0';
    return FALSE;
}

BOOL wide_to_str(const wchar_t *src, char *dst, size_t dst_cch)
{
    if (dst == NULL || dst_cch == 0) return FALSE;
    dst[0] = '\0';
    if (src == NULL) return FALSE;

    /* WC_ERR_INVALID_CHARS is the wide-side counterpart, for the same
     * reason: lone surrogates should fail loudly rather than be replaced. */
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                      src, -1, dst, (int)dst_cch, NULL, NULL);
    if (n > 0) return TRUE;

    dst[0] = '\0';
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Comparison                                                          */
/* ------------------------------------------------------------------ */

BOOL str_eq_i(const char *a, const char *b)
{
    if (a == NULL || b == NULL) return (a == b);
    return _stricmp(a, b) == 0;
}

BOOL str_contains_i(const char *hay, const char *needle)
{
    if (needle == NULL || needle[0] == '\0') return TRUE;   /* empty matches */
    if (hay == NULL) return FALSE;

    const size_t nl = strlen(needle);
    for (const char *p = hay; *p; ++p) {
        if (_strnicmp(p, needle, nl) == 0) return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Paths                                                               */
/* ------------------------------------------------------------------ */

const wchar_t *path_file_name_w(const wchar_t *path)
{
    if (path == NULL) return L"";
    const wchar_t *slash = wcsrchr(path, L'\\');
    const wchar_t *fwd   = wcsrchr(path, L'/');

    const wchar_t *best = slash;
    if (fwd != NULL && (best == NULL || fwd > best)) best = fwd;

    return (best != NULL) ? best + 1 : path;
}

const char *path_file_name_a(const char *path)
{
    if (path == NULL) return "";
    const char *slash = strrchr(path, '\\');
    const char *fwd   = strrchr(path, '/');

    const char *best = slash;
    if (fwd != NULL && (best == NULL || fwd > best)) best = fwd;

    return (best != NULL) ? best + 1 : path;
}

BOOL path_join(wchar_t *dst, size_t dst_cch,
               const wchar_t *dir, const wchar_t *leaf)
{
    if (dst == NULL || dst_cch == 0) return FALSE;
    dst[0] = L'\0';
    if (dir == NULL || leaf == NULL) return FALSE;

    /* Avoid a doubled separator when the caller's directory already ends in
     * one - "C:\a\\b" works but looks wrong in a log or an error message. */
    const size_t dl = wcslen(dir);
    const int needs_sep = (dl > 0 && dir[dl - 1] != L'\\' && dir[dl - 1] != L'/');

    const int n = _snwprintf_s(dst, dst_cch, _TRUNCATE, L"%ls%ls%ls",
                               dir, needs_sep ? L"\\" : L"", leaf);
    return n >= 0;
}

/* ------------------------------------------------------------------ */
/* Formatting                                                          */
/* ------------------------------------------------------------------ */

BOOL str_format_w(wchar_t *dst, size_t dst_cch, const wchar_t *fmt, ...)
{
    if (dst == NULL || dst_cch == 0 || fmt == NULL) return FALSE;

    va_list ap;
    va_start(ap, fmt);
    const int n = _vsnwprintf_s(dst, dst_cch, _TRUNCATE, fmt, ap);
    va_end(ap);

    /* _vsnwprintf_s returns -1 on truncation. Callers that do not care get a
     * correctly terminated string either way. */
    return n >= 0;
}
