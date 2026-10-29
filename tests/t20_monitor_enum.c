/*
 * t20_monitor_enum.c - real-machine monitor enumeration.
 *
 * This is the one monitor test that needs actual hardware, and the only one
 * that is useful TODAY: it runs on a single monitor, and its output is meant
 * to be SAVED.
 *
 * WHY SAVE THE OUTPUT
 *
 *   docs/P0验证记录.md experiment 7 ("how monitor identity survives an
 *   unplug") is unverified because the development machine has one monitor.
 *   When a second monitor is available, run this program again and compare
 *   the two files. That answers the question directly: which fields changed
 *   across a replug - device_name, friendly_name, the EDID device id, the
 *   geometry - and which did not. The entire three-level match cascade in
 *   monitor.c rests on that assumption, and it is currently an inference.
 *
 *   So the output is deliberately stable: one `[key] value` line per field,
 *   no tables, no columns, no colour. Redirect it to a file.
 *
 * WHAT IT CHECKS
 *
 *   monitor_enumerate() is the only function in monitor.c that touches the
 *   OS, so t21_monitor_match cannot cover it - t21 tests the pure cascade
 *   with constructed sets. Everything enumerate() returns is therefore
 *   cross-checked here against a SECOND, independent path
 *   (EnumDisplayMonitors + GetMonitorInfoW + EnumDisplayDevicesW), matched
 *   by device name rather than by index, because enumeration order is not
 *   guaranteed to agree between the two APIs.
 *
 *   The rest of monitor.h is pure and covered by t21. It is still exercised
 *   here against real data, because a pure function fed real input is where
 *   wiring mistakes show up - and because a single-monitor machine can still
 *   prove that the primary index, the point lookup and the set comparison
 *   are consistent with each other.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t20_monitor_enum
 *   tests\out\t20_monitor_enum.exe --no-pause --log-all > baseline.txt
 *
 * ARGUMENTS
 *   --no-pause   exit immediately instead of waiting for a key
 *   --log-all    mirror the report into tests\out\t20_monitor_enum.log
 */

#include "test_common.h"

#include <string.h>

#include "../src/core/monitor.h"

static int g_pass, g_fail;

static void ok(const wchar_t *what, int cond)
{
    if (cond) { g_pass++; }
    else { g_fail++; t_out_line(L"  [FAIL] %ls", what); }
}

static void ok_int(const wchar_t *what, int got, int want)
{
    if (got == want) { g_pass++; }
    else { g_fail++; t_out_line(L"  [FAIL] %ls  (got %d, want %d)", what, got, want); }
}

static void ok_rect(const wchar_t *what, const RECT *got, const RECT *want)
{
    if (got->left == want->left && got->top == want->top &&
        got->right == want->right && got->bottom == want->bottom) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  [FAIL] %ls", what);
        t_out_line(L"         got  (%d,%d)-(%d,%d)",
                   (int)got->left, (int)got->top, (int)got->right, (int)got->bottom);
        t_out_line(L"         want (%d,%d)-(%d,%d)",
                   (int)want->left, (int)want->top, (int)want->right, (int)want->bottom);
    }
}

/* ------------------------------------------------------------------ */
/* An independent second opinion                                       */
/*                                                                     */
/* Deliberately NOT reusing anything from monitor.c: the point is to    */
/* have two unrelated code paths that must agree.                       */
/* ------------------------------------------------------------------ */

typedef struct {
    wchar_t device[WAC_MON_NAME_MAX];
    wchar_t device_id[128];     /* MONITOR\GSM5B08\{...}\0001 - from EDID */
    RECT    monitor;
    RECT    work;
    int     primary;
    UINT    dpi;
} RawMon;

#define RAW_MAX 16

typedef struct {
    RawMon items[RAW_MAX];
    int    count;
} RawSet;

typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT *, UINT *);

static PFN_GetDpiForMonitor raw_dpi_fn(void)
{
    static PFN_GetDpiForMonitor fn;
    static int tried;
    if (!tried) {
        tried = 1;
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore) {
            fn = (PFN_GetDpiForMonitor)(void *)GetProcAddress(shcore, "GetDpiForMonitor");
        }
    }
    return fn;
}

/*
 * Ask EnumDisplayDevicesW for the monitor attached to an adapter.
 *
 * DeviceID looks like "MONITOR\GSM5B08\{4d36e96e-...}\0001". The "GSM5B08"
 * part is the EDID manufacturer id plus product code, so it is a SECOND
 * identifier that survives a device-name change - the same property
 * friendly_name has, derived from the same blob by a different API. When
 * friendly_name comes back empty but DeviceID is populated, the EDID was
 * readable and only the text descriptor was missing, which is worth knowing
 * before blaming the code.
 */
static void raw_fill_device_id(const wchar_t *adapter_device, wchar_t *out, size_t cap)
{
    out[0] = L'\0';

    DISPLAY_DEVICEW mon;
    ZeroMemory(&mon, sizeof(mon));
    mon.cb = sizeof(mon);

    /* index 0 of the adapter's monitor list is the one physically attached */
    if (EnumDisplayDevicesW(adapter_device, 0, &mon, 0)) {
        wcsncpy_s(out, cap, mon.DeviceID, _TRUNCATE);
    }
}

static BOOL CALLBACK raw_enum_proc(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp)
{
    (void)dc;
    (void)rc;

    RawSet *set = (RawSet *)lp;
    if (set->count >= RAW_MAX) return FALSE;

    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, (MONITORINFO *)&mi)) return TRUE;

    RawMon *m = &set->items[set->count];
    ZeroMemory(m, sizeof(*m));

    wcsncpy_s(m->device, WAC_MON_NAME_MAX, mi.szDevice, _TRUNCATE);
    m->monitor = mi.rcMonitor;
    m->work    = mi.rcWork;
    m->primary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0;
    m->dpi     = 96;

    PFN_GetDpiForMonitor fn = raw_dpi_fn();
    if (fn) {
        UINT dx = 96, dy = 96;
        /* MDT_EFFECTIVE_DPI == 0 */
        if (SUCCEEDED(fn(hm, 0, &dx, &dy))) m->dpi = dx;
    }

    raw_fill_device_id(mi.szDevice, m->device_id, 128);

    set->count++;
    return TRUE;
}

static int raw_enum(RawSet *set)
{
    ZeroMemory(set, sizeof(*set));
    EnumDisplayMonitors(NULL, NULL, raw_enum_proc, (LPARAM)set);
    return set->count;
}

static const RawMon *raw_find(const RawSet *set, const wchar_t *device)
{
    for (int i = 0; i < set->count; ++i) {
        if (_wcsicmp(set->items[i].device, device) == 0) return &set->items[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */

static void print_environment(void)
{
    T_SEC(L"environment");

    T_KV("time",          L"%ls", t_now_string());
    T_KV("dpi-awareness", L"%ls", t_dpi_awareness_name());
    T_KV("dpi-enum-value", L"%d", t_dpi_awareness_enum());
    T_NOTE(L"(UNAWARE would mean every coordinate below is virtualized)");
}

static void print_monitor(const MonitorFingerprint *m, int index, int count,
                          const MonitorFingerprint *set, const RawMon *raw)
{
    RECT rc;
    ZeroMemory(&rc, sizeof(rc));
    int have_rect = monitor_rect(set, count, index, &rc);

    T_SEC(L"monitor %d of %d", index, count);

    T_KV("device-name",   L"%ls", m->device_name[0] ? m->device_name : L"(empty)");
    T_KV("friendly-name", L"%ls", m->friendly_name[0] ? m->friendly_name : L"(empty - EDID name unreadable)");
    T_KV("device-id",     L"%ls", (raw && raw->device_id[0]) ? raw->device_id : L"(unavailable)");
    T_KV("resolution",    L"%d x %d", m->width, m->height);
    T_KV("virtual-origin", L"%d, %d", m->virtual_x, m->virtual_y);
    T_KV("primary",       L"%ls", m->is_primary ? L"yes" : L"no");
    T_KV("dpi",           L"%u", m->dpi);

    if (have_rect) {
        T_KV("rcMonitor", L"(%d,%d)-(%d,%d)",
             (int)rc.left, (int)rc.top, (int)rc.right, (int)rc.bottom);
    } else {
        T_KV("rcMonitor", L"(monitor_rect FAILED)");
    }

    if (raw) {
        T_KV("rcWork", L"(%d,%d)-(%d,%d)",
             (int)raw->work.left, (int)raw->work.top,
             (int)raw->work.right, (int)raw->work.bottom);

        /* A taskbar or a docked appbar makes rcWork smaller than rcMonitor.
         * The coordinate model uses rcMonitor precisely so that this
         * difference cannot shift stored positions - report it so the
         * difference is visible rather than assumed away. */
        if (raw->work.left   != raw->monitor.left   ||
            raw->work.top    != raw->monitor.top    ||
            raw->work.right  != raw->monitor.right  ||
            raw->work.bottom != raw->monitor.bottom) {
            T_NOTE(L"rcWork differs from rcMonitor - an appbar (taskbar) is present");
        } else {
            T_NOTE(L"rcWork equals rcMonitor - no appbar on this monitor");
        }
    }
}

int wmain(int argc, wchar_t **argv)
{
    int no_pause = 0, log_all = 0;

    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--no-pause") == 0)     no_pause = 1;
        else if (_wcsicmp(argv[i], L"--log-all") == 0) log_all = 1;
        else if (_wcsicmp(argv[i], L"--help") == 0 || _wcsicmp(argv[i], L"-h") == 0) {
            t_out_line(L"t20_monitor_enum [--no-pause] [--log-all]");
            return 0;
        } else {
            t_out_line(L"[warn] unknown argument ignored: %ls", argv[i]);
        }
    }

    t_diag_init(L"t20_monitor_enum");
    t_console_init();
    if (log_all) t_log_mirror(1);

    T_TITLE(L"t20: monitor enumeration (real machine)");
    T_NOTE(L"SAVE THIS OUTPUT. Re-run it after a monitor change and compare the");
    T_NOTE(L"two files - that answers experiment 7 in the P0 record directly.");
    T_NOTE(L"(the record is docs/P0-verification.md, the ASCII-named entry point)");

    if (!t_set_dpi_awareness()) {
        T_WARN(L"PerMonitorV2 is unavailable - coordinates below may be virtualized");
    }

    print_environment();

    /* ---- capture ------------------------------------------------- */

    MonitorFingerprint set[WAC_MON_MAX];
    ZeroMemory(set, sizeof(set));

    int count = monitor_enumerate(set, WAC_MON_MAX);

    T_SEC(L"enumeration");
    T_KV("monitor_enumerate", L"%d monitor(s)", count);

    if (count <= 0) {
        T_WARN(L"monitor_enumerate returned 0 - nothing further can be checked");
        T_NOTE(L"a working desktop always has at least one monitor, so this is a");
        T_NOTE(L"failure inside monitor_enumerate, not a hardware condition.");
        T_NOTE(L"run tests\\out\\t06_monitor.exe for a lower-level look");
        T_KV("verdict", L"FAIL - monitor_enumerate returned nothing");
        if (!no_pause) t_pause();
        return 1;
    }

    ok(L"at least one monitor was enumerated", count >= 1);
    ok(L"the count fits the documented maximum", count <= WAC_MON_MAX);

    /* ---- independent second opinion ------------------------------ */

    RawSet raw;
    int raw_count = raw_enum(&raw);

    T_SEC(L"independent path (EnumDisplayMonitors + GetMonitorInfoW)");
    T_KV("raw count", L"%d monitor(s)", raw_count);

    /* ---- print, and cross-check field by field -------------------- */

    for (int i = 0; i < count; ++i) {
        const RawMon *r = raw_find(&raw, set[i].device_name);
        print_monitor(&set[i], i, count, set, r);
    }

    T_SEC(L"cross-check");

    for (int i = 0; i < count; ++i) {
        const MonitorFingerprint *m = &set[i];
        wchar_t label[128];

        _snwprintf_s(label, 128, _TRUNCATE, L"monitor %d (%ls) found by the independent path", i, m->device_name);
        const RawMon *r = raw_find(&raw, m->device_name);
        ok(label, r != NULL);
        if (!r) continue;

        RECT rc;
        ZeroMemory(&rc, sizeof(rc));
        _snwprintf_s(label, 128, _TRUNCATE, L"monitor %d: monitor_rect matches GetMonitorInfoW", i);
        if (monitor_rect(set, count, i, &rc)) {
            ok_rect(label, &rc, &r->monitor);
        } else {
            ok(label, 0);
        }

        _snwprintf_s(label, 128, _TRUNCATE, L"monitor %d: width matches", i);
        ok_int(label, m->width, (int)(r->monitor.right - r->monitor.left));

        _snwprintf_s(label, 128, _TRUNCATE, L"monitor %d: height matches", i);
        ok_int(label, m->height, (int)(r->monitor.bottom - r->monitor.top));

        _snwprintf_s(label, 128, _TRUNCATE, L"monitor %d: virtual origin matches", i);
        ok_int(label, m->virtual_x, (int)r->monitor.left);
        _snwprintf_s(label, 128, _TRUNCATE, L"monitor %d: virtual origin y matches", i);
        ok_int(label, m->virtual_y, (int)r->monitor.top);

        _snwprintf_s(label, 128, _TRUNCATE, L"monitor %d: primary flag matches", i);
        ok_int(label, m->is_primary ? 1 : 0, r->primary);

        _snwprintf_s(label, 128, _TRUNCATE, L"monitor %d: dpi matches", i);
        ok_int(label, (int)m->dpi, (int)r->dpi);

        _snwprintf_s(label, 128, _TRUNCATE, L"monitor %d: has a device name", i);
        ok(label, m->device_name[0] != L'\0');

        _snwprintf_s(label, 128, _TRUNCATE, L"monitor %d: has a positive size", i);
        ok(label, m->width > 0 && m->height > 0);
    }

    ok_int(L"the two paths agree on how many monitors exist", count, raw_count);

    /* Device names must be unique, or matching by name is meaningless. */
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            wchar_t label[128];
            _snwprintf_s(label, 128, _TRUNCATE, L"device names %d and %d are distinct", i, j);
            ok(label, _wcsicmp(set[i].device_name, set[j].device_name) != 0);
        }
    }

    /* Exactly one primary - with more than one, "fall back to the primary"
     * stops being a decision. */
    {
        int primaries = 0;
        for (int i = 0; i < count; ++i) if (set[i].is_primary) primaries++;
        ok_int(L"exactly one monitor is flagged primary", primaries, 1);
    }

    /* ---- topology ------------------------------------------------- */

    T_SEC(L"virtual desktop topology");

    {
        RECT bounds;
        bounds.left   = set[0].virtual_x;
        bounds.top    = set[0].virtual_y;
        bounds.right  = set[0].virtual_x + set[0].width;
        bounds.bottom = set[0].virtual_y + set[0].height;

        for (int i = 1; i < count; ++i) {
            RECT r;
            r.left   = set[i].virtual_x;
            r.top    = set[i].virtual_y;
            r.right  = set[i].virtual_x + set[i].width;
            r.bottom = set[i].virtual_y + set[i].height;
            if (r.left   < bounds.left)   bounds.left   = r.left;
            if (r.top    < bounds.top)    bounds.top    = r.top;
            if (r.right  > bounds.right)  bounds.right  = r.right;
            if (r.bottom > bounds.bottom) bounds.bottom = r.bottom;
        }

        T_KV("bounds", L"(%d,%d)-(%d,%d)  %d x %d",
             (int)bounds.left, (int)bounds.top, (int)bounds.right, (int)bounds.bottom,
             (int)(bounds.right - bounds.left), (int)(bounds.bottom - bounds.top));

        if (bounds.left < 0 || bounds.top < 0) {
            T_NOTE(L"negative origin present - a monitor sits left of or above the primary");
            T_NOTE(L"this is the case t10_relcoord covers with constructed data");
        }

        /* Monitors must not overlap. Overlapping rectangles would make
         * monitor_at_point ambiguous. */
        for (int i = 0; i < count; ++i) {
            for (int j = i + 1; j < count; ++j) {
                RECT a, b, x;
                a.left   = set[i].virtual_x;
                a.top    = set[i].virtual_y;
                a.right  = set[i].virtual_x + set[i].width;
                a.bottom = set[i].virtual_y + set[i].height;
                b.left   = set[j].virtual_x;
                b.top    = set[j].virtual_y;
                b.right  = set[j].virtual_x + set[j].width;
                b.bottom = set[j].virtual_y + set[j].height;

                x.left   = a.left   > b.left   ? a.left   : b.left;
                x.top    = a.top    > b.top    ? a.top    : b.top;
                x.right  = a.right  < b.right  ? a.right  : b.right;
                x.bottom = a.bottom < b.bottom ? a.bottom : b.bottom;

                wchar_t label[128];
                _snwprintf_s(label, 128, _TRUNCATE, L"monitors %d and %d do not overlap", i, j);
                ok(label, x.right <= x.left || x.bottom <= x.top);
            }
        }
    }

    /* ---- the pure helpers, against real data ---------------------- */

    T_SEC(L"monitor.h helpers against real data");

    {
        int pi = monitor_primary_index(set, count);
        ok(L"monitor_primary_index returns a valid index", pi >= 0 && pi < count);
        if (pi >= 0 && pi < count) {
            ok(L"the index it returns is the one flagged primary", set[pi].is_primary != 0);

            /* A window placed on the primary must resolve back to it. */
            int cx = set[pi].virtual_x + set[pi].width / 2;
            int cy = set[pi].virtual_y + set[pi].height / 2;
            ok_int(L"monitor_at_point finds the primary at its centre", monitor_at_point(set, count, cx, cy), pi);
        }
    }

    for (int i = 0; i < count; ++i) {
        int cx = set[i].virtual_x + set[i].width / 2;
        int cy = set[i].virtual_y + set[i].height / 2;
        wchar_t label[128];
        _snwprintf_s(label, 128, _TRUNCATE, L"monitor_at_point finds monitor %d at its centre", i);
        ok_int(label, monitor_at_point(set, count, cx, cy), i);

        /* Just inside each corner, so the boundary convention is pinned. */
        _snwprintf_s(label, 128, _TRUNCATE, L"monitor_at_point finds monitor %d at its top-left pixel", i);
        ok_int(label, monitor_at_point(set, count, set[i].virtual_x, set[i].virtual_y), i);
    }

    /* A point far outside every monitor must not be attributed to one. */
    ok_int(L"a point far outside every monitor returns -1",
           monitor_at_point(set, count, -100000, -100000), -1);

    /* refresh_geometry must be idempotent on a set that was just captured. */
    {
        MonitorFingerprint copy[WAC_MON_MAX];
        memcpy(copy, set, sizeof(set));

        for (int i = 0; i < count; ++i) {
            wchar_t label[128];
            _snwprintf_s(label, 128, _TRUNCATE, L"monitor_refresh_geometry(%d) succeeds", i);
            ok(label, monitor_refresh_geometry(&copy[i]) != FALSE);
        }

        T_SEC(L"after refresh_geometry");
        for (int i = 0; i < count; ++i) {
            wchar_t label[128];
            _snwprintf_s(label, 128, _TRUNCATE, L"monitor %d geometry is unchanged by refresh", i);
            ok(label, copy[i].width  == set[i].width &&
                      copy[i].height == set[i].height &&
                      copy[i].virtual_x == set[i].virtual_x &&
                      copy[i].virtual_y == set[i].virtual_y);
        }
    }

    /* set_equal must be reflexive, and must notice a real change. */
    ok(L"monitor_set_equal is reflexive", monitor_set_equal(set, count, set, count) != FALSE);

    {
        MonitorFingerprint copy[WAC_MON_MAX];
        memcpy(copy, set, sizeof(set));

        /* Break the primary key AND the friendly name AND the geometry, so
         * the comparison has to fall all the way through to NONE. Breaking
         * only one of them would still match at a lower level and the
         * assertion would pass for the wrong reason - the mistake t21's
         * case 2 originally made. */
        wcsncpy_s(copy[0].device_name, WAC_MON_NAME_MAX, L"\\\\.\\__GONE__", _TRUNCATE);
        wcsncpy_s(copy[0].friendly_name, WAC_MON_FRIENDLY_MAX, L"__GONE__", _TRUNCATE);
        copy[0].width     += 1;
        copy[0].virtual_x += 1;

        ok(L"monitor_set_equal notices a replaced monitor",
           monitor_set_equal(set, count, copy, count) == FALSE);
    }

    ok(L"monitor_set_equal notices a count difference",
       monitor_set_equal(set, count, set, count - 1) == FALSE);

    /* ---- summary --------------------------------------------------- */

    T_SEC(L"result");
    T_KV("passed", L"%d", g_pass);
    T_KV("failed", L"%d", g_fail);

    if (g_fail == 0) {
        T_KV("verdict", L"PASS - all %d assertions hold", g_pass);
    } else {
        T_KV("verdict", L"FAIL - %d of %d assertions failed", g_fail, g_pass + g_fail);
    }

    T_NOTE(L"");
    T_NOTE(L"next: save this output, and re-run it after any monitor change.");

    if (!no_pause) t_pause();
    return g_fail == 0 ? 0 : 1;
}
