/*
 * monitor.h - monitor identification.
 *
 * THE PROBLEM
 *
 *   A workspace stores "put this window on monitor X". Windows offers no
 *   stable, persistent monitor identifier, so there is no X to store. What
 *   exists instead:
 *
 *     device_name    "\\.\DISPLAY1" - assigned by enumeration order, stable
 *                    in daily use, but changes when drivers are reinstalled
 *                    or the plug order changes
 *     friendly_name  the EDID product name, e.g. "H27T22X" - survives a
 *                    device-name change, but two identical monitors share it.
 *                    Degrades to the PnP hardware id ("SKG2728") when the EDID
 *                    text descriptor cannot be read, and to empty when even
 *                    that is unavailable - never to another monitor's name
 *     geometry       resolution plus position in the virtual desktop - the
 *                    last resort, and ambiguous when two monitors match
 *
 *   So identification is a cascade, not a lookup: try the device name, then
 *   the friendly name, then geometry. The caller is told WHICH level matched,
 *   because a match by geometry is a guess and the UI should say so.
 *
 * TESTABILITY - THE POINT OF THIS MODULE'S SHAPE
 *
 *   match() takes the current monitor set as a PARAMETER. It performs no
 *   enumeration and touches no Win32 state. That is deliberate: when this was
 *   written the development machine had one monitor that could not be
 *   reconfigured, so a match() that called EnumDisplayMonitors internally
 *   could not have been tested for any multi-monitor case at all. The machine
 *   has had a mixed-DPI pair since 2026-09-23, and the injection stays - it is
 *   what lets t21 cover cases a real desk cannot produce on demand.
 *
 *   With the set injected, t21_monitor_match can construct "the same monitor
 *   after a device-name change", "the monitor is gone", "two identical
 *   monitors", and verify each.
 *
 *   Enumeration lives in monitor_enumerate(), which is the only function here
 *   that talks to the OS.
 */

#ifndef WAC_MONITOR_H
#define WAC_MONITOR_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAC_MON_NAME_MAX      32
#define WAC_MON_FRIENDLY_MAX  64
#define WAC_MON_MAX           16

/* ------------------------------------------------------------------ */
/* Fingerprint                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    wchar_t device_name[WAC_MON_NAME_MAX];       /* "\\.\DISPLAY1" - primary key */
    wchar_t friendly_name[WAC_MON_FRIENDLY_MAX]; /* EDID product name, may be empty */
    int     width, height;                       /* resolution at capture time */
    int     virtual_x, virtual_y;                /* origin in the virtual desktop */
    int     is_primary;
    unsigned dpi;
} MonitorFingerprint;

/* ------------------------------------------------------------------ */
/* Matching                                                            */
/* ------------------------------------------------------------------ */

typedef enum {
    MONITOR_MATCH_EXACT = 0,  /* device name equal - trust it completely */
    MONITOR_MATCH_NAME,       /* friendly name equal - the monitor survived a rename */
    MONITOR_MATCH_GEOMETRY,   /* resolution and position equal - a guess */
    MONITOR_MATCH_NONE        /* not found: the slot becomes ORPHANED */
} MonitorMatchLevel;

/* Human-readable level, for logs and the CLI. Never NULL. */
const wchar_t *monitor_match_level_name(MonitorMatchLevel level);

/*
 * Find `target` within `current`.
 *
 * Returns the match level; *out_index receives the position in `current`
 * when the level is not MONITOR_MATCH_NONE.
 *
 * Rules, in order:
 *
 *   1. device_name equal          -> EXACT
 *   2. friendly_name equal        -> NAME      (both names must be non-empty;
 *                                               two empty names are not a match)
 *   3. width, height, virtual_x and virtual_y all equal
 *                                 -> GEOMETRY
 *   4. otherwise                  -> NONE
 *
 * Pure: no OS calls, no allocation, no global state. Deterministic for the
 * same inputs, which is what makes it testable.
 */
MonitorMatchLevel monitor_match(const MonitorFingerprint *target,
                                const MonitorFingerprint *current, int count,
                                int *out_index);

/*
 * Find the monitor containing a point. Used to decide which monitor a window
 * or a screen coordinate belongs to.
 *
 * Returns the index, or -1 when the point is outside every monitor.
 */
int monitor_at_point(const MonitorFingerprint *set, int count, int x, int y);

/*
 * Index of the primary monitor, or 0 when the set is empty, or -1 for a
 * NULL/empty set. Callers that must place a window somewhere use this as the
 * fallback when the stored monitor is gone.
 */
int monitor_primary_index(const MonitorFingerprint *set, int count);

/*
 * The monitor a monitor-box selection points at, or -1 when there is nothing
 * usable to act on.
 *
 * The overview's box stores monitor_enumerate()'s index in each item's data
 * (see fill_monitor_combo in mainwindow.c), so a selection means an index into
 * the CURRENT enumeration rather than a position in the list. The four ways
 * there is nothing to act on - no selection, an item carrying no index, an
 * empty enumeration, an index the enumeration no longer covers (a monitor
 * unplugged while the box still showed it) - all answer -1, because "which
 * monitor does that row mean" has to be a fact, not a default. A caller that
 * wants a fallback picks one itself and says so.
 *
 * Pure: the caller supplies what the box reported. That is what makes the
 * question testable without a window (t21_monitor_match) - the same reason
 * monitor_match() takes its set as a parameter.
 */
int monitor_selection_index(int selection, int item_data, int monitor_count);

/* ------------------------------------------------------------------ */
/* Enumeration - the only OS-touching part                             */
/* ------------------------------------------------------------------ */

/*
 * Capture the current monitor set.
 *
 * Fills at most `cap` entries and returns how many were captured (0 on
 * failure). DPI comes from GetDpiForMonitor; friendly_name is derived from the
 * monitor's DeviceID, which is matched against the "Driver" value of a
 * registry instance in order to read that monitor's own EDID. It degrades to
 * the PnP hardware id and then to empty, which simply makes matching fall
 * through to geometry. It never takes another monitor's name.
 */
int monitor_enumerate(MonitorFingerprint *out, int cap);

/*
 * Capture the rcMonitor of one entry. Kept separate from the fingerprint so
 * that geometry can be re-read without redoing the EDID lookup.
 *
 * Returns FALSE for an out-of-range index or a failed query.
 */
BOOL monitor_rect(const MonitorFingerprint *set, int count, int index, RECT *out);

/*
 * Fill a fingerprint's geometry from its device name. Used after a resume or
 * a display change to refresh stored coordinates without a full re-enumeration.
 */
BOOL monitor_refresh_geometry(MonitorFingerprint *fp);

/* ------------------------------------------------------------------ */
/* Set comparison                                                      */
/* ------------------------------------------------------------------ */

/*
 * Are two sets the same topology?
 *
 * True when they have the same count and every entry matches at EXACT or
 * NAME level. Used by displaywatch to decide whether a WM_DISPLAYCHANGE
 * actually changed anything - the message fires for changes that do not
 * affect this program, and reacting to those would re-lay-out windows for
 * no reason.
 */
BOOL monitor_set_equal(const MonitorFingerprint *a, int count_a,
                       const MonitorFingerprint *b, int count_b);

#ifdef __cplusplus
}
#endif

#endif /* WAC_MONITOR_H */
