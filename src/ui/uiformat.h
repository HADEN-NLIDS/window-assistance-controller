/*
 * uiformat.h - text <-> number helpers for the preset editor.
 *
 * WHY THIS IS SEPARATE
 *
 *   The editor lets a user type a zone by hand, which means parsing what a
 *   HUMAN types rather than what a program writes: "23.5", "23,5" (comma
 *   decimal separator), "23.5%", "  23.5  ", or something that is none of
 *   those. Getting that wrong produces a preset that silently stores a
 *   different rectangle than the one on screen.
 *
 *   Pure functions, no Win32 and no globals, so every one of those spellings
 *   can be tested without a window (tests/t12_uiformat.c) - the same split
 *   that lets relcoord and monitor matching be tested.
 *
 * PERCENTAGES, NOT FRACTIONS
 *
 *   The editor shows percentages because that is how a person describes a
 *   screen region ("the left half" = 50%). The config stores fractions. Both
 *   sides of that conversion live here so they cannot drift.
 */

#ifndef WAC_UIFORMAT_H
#define WAC_UIFORMAT_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parse a user-typed percentage into a fraction.
 *
 * Accepts: optional surrounding whitespace, an optional trailing '%', a comma
 * or a dot as the decimal separator, and a leading '+'. Rejects: empty input,
 * anything non-numeric, NaN/Inf, and values outside 0..100.
 *
 * On failure it returns FALSE and leaves *out_frac untouched, so a caller can
 * keep the last good value rather than storing a zero it did not mean.
 */
BOOL uiformat_parse_percent(const wchar_t *text, double *out_frac);

/*
 * Format a fraction as a percentage WITHOUT the '%' sign - the editor puts the
 * sign in a label next to the field, where it cannot be edited away.
 *
 *  0.235 -> "23.5"      1.0 -> "100"      0.0 -> "0"
 *
 * Trailing zeros are dropped so the field reads "50" rather than "50.0000".
 */
void uiformat_format_percent(double frac, wchar_t *out, size_t cch);

/*
 * Parse a non-negative pixel count. Surrounding whitespace allowed; no sign,
 * no decimal point, no thousands separator. Rejects values above `max_px` so a
 * typo like 999999 cannot become a window size.
 */
BOOL uiformat_parse_pixels(const wchar_t *text, int max_px, int *out_px);

/*
 * Format a fraction as a plain decimal with `decimals` places:
 *  0.235 -> "0.2350" with 4 decimals.
 * Used for the read-only normalized readout, where the exact stored value
 * matters more than readability.
 */
void uiformat_format_frac(double frac, int decimals, wchar_t *out, size_t cch);

/*
 * `part` as a whole-number percentage of `whole`, rounded to nearest.
 *
 * The editor reports a window's REAL size as a share of the screen it is drawn
 * against ("1234x567 (48% x 35%)"). That is a measurement of a window rather
 * than a fraction the user typed, so it wants rounding to a readable whole
 * number - and it must not divide by zero when the monitor is unknown.
 *
 * Rounded, not truncated: 1268 of 2560 is 49.53%, and calling that 49% when the
 * window is a hair over half the screen reads as a bug in the tool.
 *
 * Returns 0 for a non-positive `whole`, or a non-positive `part`. Pure.
 */
int uiformat_share_percent(int part, int whole);

/*
 * TRUE when the four numbers describe a usable rectangle: each in 0..1, a
 * non-zero width and height, and the right/bottom edges inside the screen.
 *
 * A zone that fails this would place a window entirely off-screen, which looks
 * exactly like "the tool did nothing".
 */
BOOL uiformat_zone_valid(double x, double y, double w, double h);

#ifdef __cplusplus
}
#endif

#endif /* WAC_UIFORMAT_H */
