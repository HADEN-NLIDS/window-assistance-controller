/*
 * log.h - append-only diagnostic log.
 *
 * Deliberately minimal. It exists so that a startup failure or an unexpected
 * state is diagnosable after the fact, not as a general-purpose tracing
 * framework - an idle program must not pay for logging it never uses.
 *
 * PRIVACY RULES (see docs/安全设计.md section 4.6)
 *
 *   Never write a full user path, and never write a window title. A window
 *   title routinely contains a document name, which turns a diagnostic file
 *   into a record of what the user was working on. Callers pass file names,
 *   not paths, and use log_window_ref() when they need to identify a window.
 *
 * COST
 *
 *   The file is opened per line and closed immediately. That sounds wasteful
 *   and is not: log lines are rare (tens per run), and an always-open handle
 *   would hold the file against deletion and add a shutdown dependency. The
 *   expensive part of a log line is the disk write, not the open.
 */

#ifndef WAC_LOG_H
#define WAC_LOG_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

/*
 * Point the log at a directory. The file inside it is "wac.log".
 *
 * Called once at startup. Until it is called, every log call is a no-op, so
 * a failure before the config directory is known cannot crash the logger.
 */
void log_init(const wchar_t *directory);

/* Current log file path, or an empty string when not initialised. */
const wchar_t *log_path(void);

/*
 * Write one line. Adds a timestamp and the level, then a newline.
 *
 * Formats with printf-style rules against a wide format string; %hs takes a
 * narrow string, %ls a wide one, %d an int.
 */
void log_write(LogLevel level, const wchar_t *fmt, ...);

/* Shorthand. */
#define log_debug(...) log_write(LOG_DEBUG, __VA_ARGS__)
#define log_info(...)  log_write(LOG_INFO,  __VA_ARGS__)
#define log_warn(...)  log_write(LOG_WARN,  __VA_ARGS__)
#define log_error(...) log_write(LOG_ERROR, __VA_ARGS__)

/*
 * A short, non-identifying reference for a window, for use in log lines.
 *
 * Returns the process executable NAME (not its full path) and the window
 * handle. Enough to correlate two lines, and useless as a record of what the
 * user was doing - which is the point.
 *
 * The result is a pointer into a small rotating buffer, valid until the next
 * few calls.
 */
const wchar_t *log_window_ref(HWND hwnd);

/*
 * Truncate the log if it has grown past `max_bytes`.
 *
 * Called once at startup so a long-running installation cannot accumulate an
 * unbounded file. One rotation only: wac.log becomes wac.log.old, replacing
 * whatever was there. A single generation is enough for a diagnostic log and
 * keeps the file count predictable.
 */
void log_rotate_if_large(unsigned long long max_bytes);

#ifdef __cplusplus
}
#endif

#endif /* WAC_LOG_H */
