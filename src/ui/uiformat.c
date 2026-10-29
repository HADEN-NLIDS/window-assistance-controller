/*
 * uiformat.c - see the header for why this exists.
 *
 * Everything here is deliberately conservative: a value that cannot be
 * understood is REFUSED rather than guessed at. Guessing is how a user's
 * "50" becomes 0.5% and their window lands in a corner.
 */

#include "uiformat.h"

#include <math.h>
#include <stdlib.h>
#include <wchar.h>

/* Walk leading whitespace. */
static const wchar_t *skip_ws(const wchar_t *s)
{
    while (s != NULL && (*s == L' ' || *s == L'\t')) ++s;
    return s;
}

/* TRUE when nothing but whitespace remains. */
static BOOL only_ws_left(const wchar_t *s)
{
    s = skip_ws(s);
    return (s == NULL || *s == L'\0') ? TRUE : FALSE;
}

BOOL uiformat_parse_percent(const wchar_t *text, double *out_frac)
{
    if (text == NULL || out_frac == NULL) return FALSE;

    const wchar_t *p = skip_ws(text);
    if (*p == L'\0') return FALSE;

    /* A leading '+' is harmless and people do type it. */
    if (*p == L'+') ++p;

    /*
     * Copy the digits out with the decimal separator normalized to '.'.
     *
     * A comma is accepted because a Chinese or European keyboard layout makes
     * it the natural key for a decimal point, and wcstod would stop at it and
     * silently return the integer part - turning 23,5 into 23.
     */
    wchar_t buf[64];
    int n = 0;
    BOOL seen_digit = FALSE;
    BOOL seen_dot   = FALSE;

    while (*p != L'\0' && n < (int)(sizeof(buf) / sizeof(buf[0])) - 1) {
        const wchar_t c = *p;

        if (c >= L'0' && c <= L'9') {
            seen_digit = TRUE;
            buf[n++] = c;
            ++p;
            continue;
        }
        if ((c == L'.' || c == L',') && !seen_dot) {
            seen_dot = TRUE;
            buf[n++] = L'.';
            ++p;
            continue;
        }
        break;
    }

    if (!seen_digit) return FALSE;   /* "", "%", ".", "+" and the like */

    /*
     * What follows must be whitespace and/or a single '%'.
     *
     * The whitespace is skipped BEFORE the sign is looked for, so "50 %" is
     * accepted: someone typing a space before the percent sign meant the same
     * thing as someone who did not.
     */
    p = skip_ws(p);
    if (*p == L'%') ++p;
    if (!only_ws_left(p)) return FALSE;

    buf[n] = L'\0';

    wchar_t *end = NULL;
    const double v = wcstod(buf, &end);
    if (end == buf || *end != L'\0') return FALSE;

    /* NaN and infinities arrive as text; refusing them keeps the caller from
     * storing a coordinate that no comparison can ever match. */
    if (v != v) return FALSE;
    if (v > 1.0e300 || v < -1.0e300) return FALSE;

    if (v < 0.0 || v > 100.0) return FALSE;

    *out_frac = v / 100.0;
    return TRUE;
}

void uiformat_format_percent(double frac, wchar_t *out, size_t cch)
{
    if (out == NULL || cch == 0) return;
    out[0] = L'\0';

    double pct = frac * 100.0;
    if (pct != pct) pct = 0.0;               /* NaN guard */
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;

    /*
     * One decimal, then strip a trailing ".0" so a round number reads as "50"
     * rather than "50.0" - and never as "50.0000", which is what %f would give
     * and what makes a field look like a debug readout.
     */
    wchar_t tmp[64];
    _snwprintf_s(tmp, 64, _TRUNCATE, L"%.1f", pct);

    size_t len = wcslen(tmp);
    if (len >= 2 && tmp[len - 2] == L'.' && tmp[len - 1] == L'0') {
        tmp[len - 2] = L'\0';
    }

    wcsncpy_s(out, cch, tmp, _TRUNCATE);
}

BOOL uiformat_parse_pixels(const wchar_t *text, int max_px, int *out_px)
{
    if (text == NULL || out_px == NULL) return FALSE;

    const wchar_t *p = skip_ws(text);
    if (*p == L'\0') return FALSE;

    if (*p < L'0' || *p > L'9') return FALSE;   /* no sign, no dot */

    long v = 0;
    int digits = 0;

    while (*p >= L'0' && *p <= L'9') {
        v = v * 10 + (*p - L'0');
        ++p;
        if (++digits > 9) return FALSE;         /* beyond any real display */
        if (v > 100000000L) return FALSE;
    }

    if (!only_ws_left(p)) return FALSE;

    if (max_px > 0 && v > (long)max_px) return FALSE;

    *out_px = (int)v;
    return TRUE;
}

void uiformat_format_frac(double frac, int decimals, wchar_t *out, size_t cch)
{
    if (out == NULL || cch == 0) return;
    out[0] = L'\0';

    if (decimals < 0) decimals = 0;
    if (decimals > 6) decimals = 6;

    if (frac != frac) frac = 0.0;               /* NaN guard */

    wchar_t fmt[16];
    _snwprintf_s(fmt, 16, _TRUNCATE, L"%%.%df", decimals);

    wchar_t tmp[64];
    _snwprintf_s(tmp, 64, _TRUNCATE, fmt, frac);

    wcsncpy_s(out, cch, tmp, _TRUNCATE);
}

int uiformat_share_percent(int part, int whole)
{
    if (whole <= 0 || part <= 0) return 0;

    /* long long for the multiplication: a 4K-wide window would still fit an
     * int, but the arithmetic is not the place to be clever about it. */
    return (int)(((long long)part * 100 + whole / 2) / whole);
}

BOOL uiformat_zone_valid(double x, double y, double w, double h)
{
    /* Reject NaN up front: every comparison below is false for NaN, so an
     * unchecked one would fall through and be reported as valid. */
    if (x != x || y != y || w != w || h != h) return FALSE;

    if (x < 0.0 || y < 0.0 || w <= 0.0 || h <= 0.0) return FALSE;
    if (x > 1.0 || y > 1.0 || w > 1.0 || h > 1.0) return FALSE;

    /* The far edge has to stay on the screen. A zone of width 0.5 starting at
     * x = 0.8 would put 30% of the window past the right edge - and after
     * clamping the user would see a rectangle that is not the one they typed. */
    if (x + w > 1.0 + 1.0e-6) return FALSE;
    if (y + h > 1.0 + 1.0e-6) return FALSE;

    return TRUE;
}
