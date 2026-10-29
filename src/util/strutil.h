/*
 * strutil.h - string helpers used across the program.
 *
 * Everything here exists because the same few conversions and comparisons
 * were being open-coded in several files. Centralising them means the project
 * has ONE answer to "how do we compare process names" and ONE answer to "how
 * do we turn UTF-8 config text into a wide string".
 *
 * SAFETY
 *
 *   All output buffers are explicitly sized and every function truncates
 *   rather than overruns. The project disables strcpy/strcat/sprintf;
 *   these wrappers use the strsafe family
 *   internally so callers never need to reach for the unsafe forms.
 *
 * ENCODING
 *
 *   Two encodings are in play and mixing them is the usual source of bugs:
 *
 *     - The config file is UTF-8 bytes stored in `char`. Workspace display
 *       names go through here, so a Chinese name survives a round trip.
 *     - Win32 W APIs are UTF-16. Anything reaching the OS is wide.
 *
 *   str_to_wide / wide_to_str are the only sanctioned crossing points.
 */

#ifndef WAC_STRUTIL_H
#define WAC_STRUTIL_H

#include <windows.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- conversion --------------------------------------------------- */

/*
 * UTF-8 to UTF-16.
 *
 * Returns FALSE (and writes an empty string) when the input is not valid
 * UTF-8 or does not fit. Callers get a usable empty string either way, so a
 * hand-edited config file with a stray byte degrades to an unnamed entry
 * instead of aborting.
 */
BOOL str_to_wide(const char *src, wchar_t *dst, size_t dst_cch);

/*
 * UTF-16 to UTF-8.
 *
 * Same contract: FALSE and an empty result on failure or overflow.
 */
BOOL wide_to_str(const wchar_t *src, char *dst, size_t dst_cch);

/* ---- comparison --------------------------------------------------- */

/* Case-insensitive equality for narrow strings. NULL-safe: both NULL is
 * equal, one NULL is not. */
BOOL str_eq_i(const char *a, const char *b);

/* Case-insensitive "does hay contain needle". An empty needle matches, so a
 * caller can pass an optional filter through without branching. NULL hay
 * never matches a non-empty needle. */
BOOL str_contains_i(const char *hay, const char *needle);

/* ---- paths -------------------------------------------------------- */

/*
 * File name portion of a path, or the whole string when there is no
 * separator. Never returns NULL for a non-NULL input.
 */
const wchar_t *path_file_name_w(const wchar_t *path);
const char    *path_file_name_a(const char *path);

/*
 * Append a leaf to a directory, inserting a separator when needed.
 * Truncates rather than overruns.
 */
BOOL path_join(wchar_t *dst, size_t dst_cch,
               const wchar_t *dir, const wchar_t *leaf);

/* ---- formatting --------------------------------------------------- */

/*
 * Wide sprintf that cannot overrun and reports truncation.
 *
 * Returns FALSE when the result did not fit, so a caller that cares can tell
 * "the name was cut short" from "the name was empty". strsafe's own return
 * value distinguishes these, but the STRSAFE_E_* codes are awkward to test
 * at every call site.
 */
BOOL str_format_w(wchar_t *dst, size_t dst_cch, const wchar_t *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* WAC_STRUTIL_H */
