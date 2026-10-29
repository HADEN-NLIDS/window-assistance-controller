/*
 * monitor.c - monitor identification.
 *
 * See monitor.h for the cascade and for why match() takes the monitor set as
 * a parameter instead of enumerating internally.
 *
 * The EDID reading here is the same approach validated in stage-1
 * experiment 7 (tests/t06_monitor.c): the friendly name is not available
 * through any Win32 API, only as a blob in the registry, one level deeper
 * than the device instance key. Getting that path wrong yields
 * ERROR_FILE_NOT_FOUND from RegQueryValueEx and looks exactly like "this
 * machine has no EDID" - see TRAPS.md #3.
 */

#include "monitor.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* ------------------------------------------------------------------ */
/* Match level names                                                   */
/* ------------------------------------------------------------------ */

const wchar_t *monitor_match_level_name(MonitorMatchLevel level)
{
    switch (level) {
    case MONITOR_MATCH_EXACT:    return L"exact (device name)";
    case MONITOR_MATCH_NAME:     return L"name (EDID friendly name)";
    case MONITOR_MATCH_GEOMETRY: return L"geometry (resolution and position)";
    case MONITOR_MATCH_NONE:     return L"none (not found)";
    }
    return L"unknown";
}

/* ------------------------------------------------------------------ */
/* Matching - pure                                                     */
/* ------------------------------------------------------------------ */

/*
 * Two fingerprints have the same geometry.
 *
 * virtual_x/virtual_y are included on purpose: two identical monitors at the
 * same resolution are told apart by where they sit in the virtual desktop, so
 * dropping the position would make any two same-model screens interchangeable
 * and send windows to the wrong one.
 */
static BOOL same_geometry(const MonitorFingerprint *a, const MonitorFingerprint *b)
{
    return a->width     == b->width  &&
           a->height    == b->height &&
           a->virtual_x == b->virtual_x &&
           a->virtual_y == b->virtual_y;
}

/*
 * Friendly names match, and the comparison is meaningful.
 *
 * The emptiness check matters: an unreadable EDID leaves the name empty on
 * both sides, and without this every monitor with a missing EDID would match
 * every other one.
 */
static BOOL same_friendly_name(const MonitorFingerprint *a,
                               const MonitorFingerprint *b)
{
    if (a->friendly_name[0] == L'\0' || b->friendly_name[0] == L'\0') return FALSE;
    return _wcsicmp(a->friendly_name, b->friendly_name) == 0;
}

MonitorMatchLevel monitor_match(const MonitorFingerprint *target,
                                const MonitorFingerprint *current, int count,
                                int *out_index)
{
    if (out_index != NULL) *out_index = -1;
    if (target == NULL || current == NULL || count <= 0) return MONITOR_MATCH_NONE;

    /* Level 1: device name. If this matches, nothing else needs checking -
     * it is the same device path Windows would hand back. */
    for (int i = 0; i < count; ++i) {
        if (target->device_name[0] != L'\0' &&
            _wcsicmp(target->device_name, current[i].device_name) == 0) {
            if (out_index != NULL) *out_index = i;
            return MONITOR_MATCH_EXACT;
        }
    }

    /* Level 2: friendly name. Reached when the device name changed - a
     * driver reinstall or a different port usually leaves this intact. */
    for (int i = 0; i < count; ++i) {
        if (same_friendly_name(target, &current[i])) {
            if (out_index != NULL) *out_index = i;
            return MONITOR_MATCH_NAME;
        }
    }

    /* Level 3: geometry. A guess, and reported as one, because two identical
     * monitors at different positions can be swapped without this noticing. */
    for (int i = 0; i < count; ++i) {
        if (same_geometry(target, &current[i])) {
            if (out_index != NULL) *out_index = i;
            return MONITOR_MATCH_GEOMETRY;
        }
    }

    return MONITOR_MATCH_NONE;
}

int monitor_at_point(const MonitorFingerprint *set, int count, int x, int y)
{
    if (set == NULL || count <= 0) return -1;

    for (int i = 0; i < count; ++i) {
        const MonitorFingerprint *m = &set[i];
        if (x >= m->virtual_x && x < m->virtual_x + m->width &&
            y >= m->virtual_y && y < m->virtual_y + m->height) {
            return i;
        }
    }
    return -1;
}

int monitor_primary_index(const MonitorFingerprint *set, int count)
{
    if (set == NULL || count <= 0) return -1;

    for (int i = 0; i < count; ++i) {
        if (set[i].is_primary) return i;
    }
    /* No monitor claims to be primary: the first is the best available
     * answer, and callers need one rather than -1. */
    return 0;
}

int monitor_selection_index(int selection, int item_data, int monitor_count)
{
    /* CB_GETCURSEL/CB_GETITEMDATA report -1 (CB_ERR) for "nothing selected"
     * and "no data on that item"; both mean there is no monitor to act on. */
    if (selection < 0 || item_data < 0) return -1;

    /* The list is a snapshot: it was filled from one enumeration, and the
     * index is only meaningful against an enumeration that still has it. */
    if (monitor_count <= 0 || item_data >= monitor_count) return -1;

    return item_data;
}

/* ------------------------------------------------------------------ */
/* EDID                                                                */
/* ------------------------------------------------------------------ */

/*
 * Read a binary value from a display instance's "Device Parameters" subkey.
 *
 * `instance_path` is the full path to the ENUM instance, for example
 *   SYSTEM\CurrentControlSet\Enum\DISPLAY\AUO75AC\5&3598922c&0&UID512
 * The EDID value is NOT there - it lives one level deeper, in the
 * "Device Parameters" subkey. Opening the instance key succeeds either way,
 * so omitting that component produces ERROR_FILE_NOT_FOUND from the value
 * query, which is easy to misread as "this machine has no EDID".
 */
static DWORD read_edid_blob(const wchar_t *instance_path, BYTE *out, DWORD out_size)
{
    wchar_t subkey[384];
    const int n = _snwprintf_s(subkey, 384, _TRUNCATE,
                               L"%ls\\Device Parameters", instance_path);
    if (n < 0) return 0;

    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return 0;
    }

    DWORD type = 0, size = 0;
    LONG rc = RegQueryValueExW(key, L"EDID", NULL, &type, NULL, &size);
    if (rc != ERROR_SUCCESS || type != REG_BINARY || size == 0 || size > out_size) {
        RegCloseKey(key);
        return 0;
    }

    /* size must be re-supplied: RegQueryValueEx can modify it. */
    DWORD got = size;
    rc = RegQueryValueExW(key, L"EDID", NULL, &type, out, &got);
    RegCloseKey(key);

    return (rc == ERROR_SUCCESS) ? got : 0;
}

/*
 * Extract the monitor name from an EDID blob.
 *
 * The name lives in one of the four 18-byte descriptor blocks, tagged 0xFC.
 * Not every monitor has one; when it is absent the caller falls back to
 * geometry matching.
 */
static void parse_edid_name(const BYTE *edid, DWORD len, wchar_t *out, size_t out_cch)
{
    if (out == NULL || out_cch == 0) return;
    out[0] = L'\0';
    if (edid == NULL || len < 128) return;

    for (int block = 0; block < 4; ++block) {
        const BYTE *d = edid + 54 + block * 18;
        if (d[0] != 0x00 || d[1] != 0x00 || d[2] != 0x00 || d[3] != 0xFC) continue;

        size_t n = 0;
        for (int k = 5; k < 18 && n + 1 < out_cch; ++k) {
            if (d[k] == 0x0A) break;      /* newline terminates */
            if (d[k] >= 0x20) out[n++] = (wchar_t)d[k];
        }
        while (n > 0 && out[n - 1] == L' ') n--;   /* trim trailing spaces */
        out[n] = L'\0';
        return;
    }
}

/*
 * Read a REG_SZ value from a display instance key.
 *
 * Returns FALSE, with an empty string, for a missing value, a different type
 * or an unreadable key: the caller only ever wants "the string, or nothing".
 */
static BOOL read_instance_string(const wchar_t *instance_path,
                                 const wchar_t *value_name,
                                 wchar_t *out, size_t out_cch)
{
    if (out == NULL || out_cch == 0) return FALSE;
    ZeroMemory(out, out_cch * sizeof(wchar_t));

    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, instance_path, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return FALSE;
    }

    DWORD type = 0;
    DWORD size = (DWORD)(out_cch * sizeof(wchar_t));
    const LONG rc = RegQueryValueExW(key, value_name, NULL, &type, (BYTE *)out, &size);
    RegCloseKey(key);

    if (rc != ERROR_SUCCESS || type != REG_SZ) {
        out[0] = L'\0';
        return FALSE;
    }
    out[out_cch - 1] = L'\0';   /* a value that fills the buffer still terminates */
    return (out[0] != L'\0') ? TRUE : FALSE;
}

/*
 * Split a monitor DeviceID into the two halves that are actually useful.
 *
 * EnumDisplayDevicesW reports DeviceID as "MONITOR\SKG2728\{4d36e96e-...}\0002":
 *
 *   hardware_id  "SKG2728"              - the EDID manufacturer plus product
 *                                         code. Survives a device-name change,
 *                                         and is the fallback name when the
 *                                         EDID text descriptor cannot be read.
 *   driver_key   "{4d36e96e-...}\0002"  - class GUID plus instance number. This
 *                                         is what the ENUM instance key stores
 *                                         as its "Driver" value, so it is the
 *                                         only reliable link from a
 *                                         "\\.\DISPLAYn" to a registry
 *                                         instance.
 *
 * WHAT THE LINK IS NOT: the instance's "Service" value. That is the driver
 * service name - measured on this machine it is literally "monitor" for every
 * display - so comparing it against "\\.\DISPLAYn" never matches anything. The
 * first version of this code did exactly that and then fell back to "the first
 * EDID with any name", which handed BOTH monitors the same name. Found by t20
 * on the first real dual-monitor run, 2026-10-08.
 */
static void split_device_id(const wchar_t *device_id,
                            wchar_t *hardware_id, size_t hw_cch,
                            wchar_t *driver_key, size_t dk_cch)
{
    if (hardware_id != NULL && hw_cch > 0) hardware_id[0] = L'\0';
    if (driver_key != NULL && dk_cch > 0) driver_key[0] = L'\0';
    if (device_id == NULL) return;

    /* Expect "MONITOR\<hardware>\<class guid>\<instance>". Anything else is
     * left unset rather than guessed at. */
    const wchar_t *p = wcschr(device_id, L'\\');
    if (p == NULL) return;
    ++p;                                        /* past "MONITOR\" */

    const wchar_t *hw_end = wcschr(p, L'\\');
    if (hw_end == NULL || hw_end == p) return;

    if (hardware_id != NULL) {
        const size_t n = (size_t)(hw_end - p);
        if (n < hw_cch) wcsncpy_s(hardware_id, hw_cch, p, n);
    }

    /* Everything after the hardware id is the driver key. */
    const wchar_t *dk = hw_end + 1;
    if (driver_key != NULL && dk[0] != L'\0') {
        wcsncpy_s(driver_key, dk_cch, dk, _TRUNCATE);
    }
}

/*
 * Look up the EDID name for one display.
 *
 * `device_id` is the DeviceID EnumDisplayDevicesW reports for the monitor
 * attached to the adapter, e.g. "MONITOR\SKG2728\{4d36e96e-...}\0002".
 *
 * The instance is found by matching its "Driver" value against the trailing
 * "{class guid}\<n>" of the DeviceID. Measured 2026-10-08, first dual-monitor
 * run: 4 of 4 instances matched exactly, across two monitors of different
 * makes, and the two instances that were NOT attached at the time were
 * correctly skipped.
 *
 * A name is never taken from another monitor. When the instance or its EDID
 * cannot be read, the name falls back to the hardware id ("SKG2728"), which
 * still tells different monitors apart, and then to empty, which makes
 * matching fall through to geometry by design. A wrong name is worse than no
 * name: every later match treats it as fact.
 *
 * UNVERIFIED: whether the "Driver" instance number survives a physical
 * unplug/replug. It is exact while the topology is static; the replug matrix
 * is stage 3, and this is the field to re-measure there.
 *
 * Read-only throughout: no registry value is ever written.
 */
static void lookup_edid_name(const wchar_t *device_id, wchar_t *out, size_t out_cch)
{
    if (out == NULL || out_cch == 0) return;
    out[0] = L'\0';

    wchar_t hardware_id[64];
    wchar_t driver_key[128];
    split_device_id(device_id, hardware_id, 64, driver_key, 128);

    if (hardware_id[0] == L'\0') return;

    /* Fallback first: whatever happens below, this is already a valid name. */
    wcsncpy_s(out, out_cch, hardware_id, _TRUNCATE);

    if (driver_key[0] == L'\0') return;

    wchar_t dev_path[192];
    if (_snwprintf_s(dev_path, 192, _TRUNCATE,
                     L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\%ls",
                     hardware_id) < 0) {
        return;
    }

    HKEY dev_key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, dev_path, 0,
                      KEY_READ | KEY_ENUMERATE_SUB_KEYS, &dev_key) != ERROR_SUCCESS) {
        return;
    }

    for (DWORD i = 0; ; ++i) {
        wchar_t inst[64];
        DWORD inst_len = 64;
        if (RegEnumKeyExW(dev_key, i, inst, &inst_len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
            break;
        }

        wchar_t inst_path[288];
        if (_snwprintf_s(inst_path, 288, _TRUNCATE, L"%ls\\%ls",
                         dev_path, inst) < 0) {
            continue;
        }

        /* The Driver value is the class GUID plus instance number, which is
         * exactly what the DeviceID ends with. */
        wchar_t this_driver[128];
        if (!read_instance_string(inst_path, L"Driver", this_driver, 128)) continue;
        if (_wcsicmp(this_driver, driver_key) != 0) continue;

        /* This is our instance, so its EDID is this monitor's own. */
        BYTE edid[512];
        const DWORD got = read_edid_blob(inst_path, edid, sizeof(edid));
        if (got > 0) {
            wchar_t name[WAC_MON_FRIENDLY_MAX];
            parse_edid_name(edid, got, name, WAC_MON_FRIENDLY_MAX);
            if (name[0] != L'\0') wcsncpy_s(out, out_cch, name, _TRUNCATE);
        }
        break;
    }

    RegCloseKey(dev_key);
}

/* ------------------------------------------------------------------ */
/* Enumeration                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    MonitorFingerprint *out;
    int                 cap;
    int                 count;
} EnumCtx;

static BOOL CALLBACK enum_proc(HMONITOR hmon, HDC hdc, LPRECT rect, LPARAM lp)
{
    (void)hdc; (void)rect;

    EnumCtx *ctx = (EnumCtx *)lp;
    if (ctx->count >= ctx->cap) return FALSE;

    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, (MONITORINFO *)&mi)) return TRUE;

    MonitorFingerprint *fp = &ctx->out[ctx->count];
    ZeroMemory(fp, sizeof(*fp));

    wcsncpy_s(fp->device_name, WAC_MON_NAME_MAX, mi.szDevice, _TRUNCATE);
    fp->width     = mi.rcMonitor.right - mi.rcMonitor.left;
    fp->height    = mi.rcMonitor.bottom - mi.rcMonitor.top;
    fp->virtual_x = mi.rcMonitor.left;
    fp->virtual_y = mi.rcMonitor.top;
    fp->is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0;

    /* Per-monitor DPI. Loaded on first use rather than at startup: shcore is
     * not present on the oldest supported builds, and a missing DLL should
     * degrade to "no DPI information", not to a failed enumeration. */
    {
        typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT *, UINT *);
        static PFN_GetDpiForMonitor fn = (PFN_GetDpiForMonitor)-1;

        if (fn == (PFN_GetDpiForMonitor)-1) {
            HMODULE shcore = LoadLibraryW(L"shcore.dll");
            fn = shcore ? (PFN_GetDpiForMonitor)(void *)
                          GetProcAddress(shcore, "GetDpiForMonitor")
                        : NULL;
            /* Deliberately not freed: the pointer must stay valid. */
        }
        if (fn != NULL) {
            UINT dx = 0, dy = 0;
            if (SUCCEEDED(fn(hmon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy))) {
                fp->dpi = dx;
            }
        }
    }

    /*
     * The EDID name is looked up through the monitor's DeviceID, not through
     * "\\.\DISPLAYn": a registry instance carries a "Driver" value that the
     * DeviceID's tail matches, and nothing that the display name matches. See
     * split_device_id(). A failure here leaves the name empty on purpose -
     * matching then falls through to geometry instead of inventing an answer.
     */
    {
        DISPLAY_DEVICEW dd;
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);

        /* index 0 of the adapter's monitor list is the one physically attached */
        if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0)) {
            lookup_edid_name(dd.DeviceID, fp->friendly_name, WAC_MON_FRIENDLY_MAX);
        }
    }

    ctx->count++;
    return TRUE;
}

int monitor_enumerate(MonitorFingerprint *out, int cap)
{
    if (out == NULL || cap <= 0) return 0;

    EnumCtx ctx = { out, cap, 0 };
    EnumDisplayMonitors(NULL, NULL, enum_proc, (LPARAM)&ctx);
    return ctx.count;
}

/* ------------------------------------------------------------------ */
/* Geometry lookup                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const wchar_t *device_name;
    RECT          *out;
    BOOL           found;
} FindRectCtx;

static BOOL CALLBACK find_rect_proc(HMONITOR hmon, HDC hdc, LPRECT rect, LPARAM lp)
{
    (void)hdc; (void)rect;

    FindRectCtx *ctx = (FindRectCtx *)lp;

    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, (MONITORINFO *)&mi)) return TRUE;

    if (_wcsicmp(mi.szDevice, ctx->device_name) != 0) return TRUE;

    *ctx->out = mi.rcMonitor;
    ctx->found = TRUE;
    return FALSE;
}

BOOL monitor_rect(const MonitorFingerprint *set, int count, int index, RECT *out)
{
    if (set == NULL || out == NULL || index < 0 || index >= count) return FALSE;

    /* Stored geometry is authoritative for the set this fingerprint came
     * from; re-querying would be a second source of truth for the same
     * numbers. */
    out->left   = set[index].virtual_x;
    out->top    = set[index].virtual_y;
    out->right  = set[index].virtual_x + set[index].width;
    out->bottom = set[index].virtual_y + set[index].height;
    return TRUE;
}

BOOL monitor_refresh_geometry(MonitorFingerprint *fp)
{
    if (fp == NULL || fp->device_name[0] == L'\0') return FALSE;

    RECT rc;
    FindRectCtx ctx = { fp->device_name, &rc, FALSE };
    EnumDisplayMonitors(NULL, NULL, find_rect_proc, (LPARAM)&ctx);
    if (!ctx.found) return FALSE;

    fp->virtual_x = rc.left;
    fp->virtual_y = rc.top;
    fp->width     = rc.right - rc.left;
    fp->height    = rc.bottom - rc.top;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Set comparison                                                      */
/* ------------------------------------------------------------------ */

BOOL monitor_set_equal(const MonitorFingerprint *a, int count_a,
                       const MonitorFingerprint *b, int count_b)
{
    if (a == NULL || b == NULL) return FALSE;
    if (count_a != count_b) return FALSE;

    /*
     * Every entry in `a` must find a partner in `b` at EXACT or NAME level.
     *
     * GEOMETRY does not count here. Two sets that only agree by geometry can
     * be the same layout or a different one with the same numbers, and this
     * function's caller uses the answer to decide whether to re-lay-out the
     * user's windows - a guess is not good enough for that.
     */
    for (int i = 0; i < count_a; ++i) {
        int idx = -1;
        const MonitorMatchLevel level = monitor_match(&a[i], b, count_b, &idx);
        if (level != MONITOR_MATCH_EXACT && level != MONITOR_MATCH_NAME) return FALSE;
    }
    return TRUE;
}
