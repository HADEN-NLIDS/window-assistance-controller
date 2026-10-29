/*
 * t06_monitor.c - stage 1, experiment 7
 *
 * PURPOSE
 *   This is the ONLY factual basis for two later stages, so it matters more
 *   than the other six programs:
 *     - stage 3 (multi-monitor matching: device name -> friendly name ->
 *       geometry fingerprint)
 *     - stage 7 (monitor change handling and the 60s grace period)
 *
 *   The plan assumes "\\.\DISPLAYn" is stable enough to be the primary key.
 *   That assumption is hardware- and driver-dependent, so it has to be
 *   measured on the real machine rather than trusted.
 *
 * HOW TO RUN
 *     t06_monitor.exe --snapshot before     capture the "before" state
 *     <unplug / replug the monitor, change primary, change resolution, ...>
 *     t06_monitor.exe --snapshot after      capture the "after" state
 *     t06_monitor.exe --compare before after
 *
 *   Other useful invocations:
 *     t06_monitor.exe                       just print the current state
 *     t06_monitor.exe --edid                also dump raw EDID keys found
 *
 *   Snapshots are written next to the executable, so nothing outside the
 *   tests directory is touched.
 *
 * WHAT TO RECORD (into docs/P0验证记录.md, experiment 7)
 *   - which fields changed between before/after for each operation:
 *       replug same port, change primary, change resolution,
 *       disable+enable adapter, sleep/wake, reinstall GPU driver
 *   - whether the 3-level match would have succeeded
 *
 * BUILD
 *   tests\build_tests.cmd t06_monitor
 */

#include "test_common.h"

#include <stdarg.h>

#define MAX_MONITORS 16
#define SNAP_MAX_LINES 256
#define SNAP_LINE_LEN  512

/* Set by --diag; prints the registry walk step by step.
   Declared early because read_reg_binary() below also reports through it. */
static int g_diag = 0;

/* ------------------------------------------------------------------ */
/* EDID parsing                                                        */
/*                                                                     */
/* The product will need this too, because Windows exposes no monitor  */
/* serial number through GetMonitorInfo. Friendly names come from the  */
/* EDID blob stored per display device under:                          */
/*   HKLM\SYSTEM\CurrentControlSet\Enum\DISPLAY\<id>\<instance>\        */
/*        Device Parameters\EDID                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char    manufacturer[4];   /* 3-letter PNP id, e.g. "DEL" */
    WORD    product_code;
    DWORD   serial;
    wchar_t product_name[32];
    int     valid;
} EdidInfo;

/*
 * Read a binary value from a display instance's "Device Parameters" subkey.
 *
 * `instance_path` is the full path to the ENUM instance, e.g.
 *   SYSTEM\CurrentControlSet\Enum\DISPLAY\AUO75AC\5&3598922c&0&UID512
 * The EDID value does NOT live there - it lives one level deeper, in the
 * "Device Parameters" subkey. Opening the instance key succeeds either way,
 * so forgetting the extra component produces ERROR_FILE_NOT_FOUND (2) from
 * the value query, which is easy to misread as "this machine has no EDID".
 */
static DWORD read_reg_binary(HKEY root, const wchar_t *instance_path,
                             const wchar_t *value, BYTE *out, DWORD out_size)
{
    wchar_t subkey[384];
    _snwprintf_s(subkey, 384, _TRUNCATE, L"%ls\\Device Parameters", instance_path);

    if (g_diag) t_out_line(L"[DIAG]     open %ls", subkey);

    HKEY key;
    LONG rc = RegOpenKeyExW(root, subkey, 0, KEY_READ, &key);
    if (rc != ERROR_SUCCESS) {
        if (g_diag) t_out_line(L"[DIAG]       RegOpenKeyExW rc=%ld", rc);
        return 0;
    }

    DWORD type = 0, size = 0;
    rc = RegQueryValueExW(key, value, NULL, &type, NULL, &size);
    if (rc != ERROR_SUCCESS) {
        if (g_diag) t_out_line(L"[DIAG]       query size rc=%ld", rc);
        RegCloseKey(key);
        return 0;
    }
    if (g_diag) t_out_line(L"[DIAG]       value type=%lu size=%lu (buffer %lu)",
                        (unsigned long)type, (unsigned long)size, (unsigned long)out_size);

    if (type != REG_BINARY || size == 0 || size > out_size) {
        if (g_diag) t_out_line(L"[DIAG]       rejected: type or size out of range");
        RegCloseKey(key);
        return 0;
    }

    /* size must be re-supplied: RegQueryValueEx can modify it. */
    DWORD got = size;
    rc = RegQueryValueExW(key, value, NULL, &type, out, &got);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        if (g_diag) t_out_line(L"[DIAG]       read rc=%ld", rc);
        return 0;
    }
    return got;
}

static void parse_edid(const BYTE *e, DWORD len, EdidInfo *info)
{
    ZeroMemory(info, sizeof(*info));
    if (len < 128) return;
    if (e[0] != 0x00 || e[1] != 0xFF || e[2] != 0xFF || e[3] != 0xFF) return;

    /* Manufacturer id: three 5-bit letters packed big-endian at 8..9. */
    WORD mfg = (WORD)((e[8] << 8) | e[9]);
    info->manufacturer[0] = (char)('A' + ((mfg >> 10) & 0x1F) - 1);
    info->manufacturer[1] = (char)('A' + ((mfg >>  5) & 0x1F) - 1);
    info->manufacturer[2] = (char)('A' + ((mfg      ) & 0x1F) - 1);
    info->manufacturer[3] = '\0';

    info->product_code = (WORD)(e[10] | (e[11] << 8));
    info->serial       = (DWORD)(e[12] | (e[13] << 8) | ((DWORD)e[14] << 16) | ((DWORD)e[15] << 24));

    /* Scan the four 18-byte descriptor blocks for a monitor-name tag. */
    for (int i = 0; i < 4; ++i) {
        const BYTE *d = e + 54 + i * 18;
        if (d[0] == 0x00 && d[1] == 0x00 && d[2] == 0x00 && d[3] == 0xFC) {
            int n = 0;
            for (int k = 5; k < 18 && n < 31; ++k) {
                if (d[k] == 0x0A) break;         /* terminator */
                if (d[k] >= 0x20) info->product_name[n++] = (wchar_t)d[k];
            }
            while (n > 0 && info->product_name[n - 1] == L' ') --n;
            info->product_name[n] = L'\0';
        }
    }
    info->valid = 1;
}

typedef struct {
    wchar_t  device_key[128];     /* the ENUM subkey name */
    wchar_t  instance_key[160];
    EdidInfo edid;
} EdidEntry;

static int enumerate_edid(EdidEntry *out, int cap)
{
    int count = 0;
    HKEY display;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                            L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY",
                            0, KEY_READ | KEY_ENUMERATE_SUB_KEYS, &display);
    if (rc != ERROR_SUCCESS) {
        if (g_diag) t_out_line(L"[DIAG] RegOpenKeyExW DISPLAY failed: %ld", rc);
        return 0;
    }
    if (g_diag) t_out_line(L"[DIAG] opened Enum\\DISPLAY");

    for (DWORD i = 0; count < cap; ++i) {
        wchar_t dev[128];
        DWORD dev_len = 128;
        rc = RegEnumKeyExW(display, i, dev, &dev_len, NULL, NULL, NULL, NULL);
        if (rc != ERROR_SUCCESS) {
            if (g_diag) t_out_line(L"[DIAG] dev enumeration ended at i=%lu (rc=%ld)",
                                (unsigned long)i, rc);
            break;
        }
        if (g_diag) t_out_line(L"[DIAG] dev[%lu] = %ls", (unsigned long)i, dev);

        wchar_t dev_path[192];
        _snwprintf_s(dev_path, 192, _TRUNCATE,
                     L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\%ls", dev);

        HKEY dev_key;
        rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, dev_path, 0,
                           KEY_READ | KEY_ENUMERATE_SUB_KEYS, &dev_key);
        if (rc != ERROR_SUCCESS) {
            if (g_diag) t_out_line(L"[DIAG]   open %ls failed rc=%ld", dev_path, rc);
            continue;
        }

        for (DWORD j = 0; count < cap; ++j) {
            wchar_t inst[64];
            DWORD inst_len = 64;
            rc = RegEnumKeyExW(dev_key, j, inst, &inst_len, NULL, NULL, NULL, NULL);
            if (rc != ERROR_SUCCESS) {
                if (g_diag) t_out_line(L"[DIAG]   inst enumeration ended at j=%lu (rc=%ld)",
                                    (unsigned long)j, rc);
                break;
            }

            wchar_t inst_path[288];
            _snwprintf_s(inst_path, 288, _TRUNCATE, L"%ls\\%ls", dev_path, inst);

            BYTE edid[512];
            DWORD got = read_reg_binary(HKEY_LOCAL_MACHINE, inst_path, L"EDID",
                                        edid, sizeof(edid));
            if (g_diag) t_out_line(L"[DIAG]   %ls -> EDID %lu bytes", inst, (unsigned long)got);
            if (got == 0) continue;

            EdidEntry *e = &out[count];
            wcsncpy_s(e->device_key,   128, dev,      _TRUNCATE);
            wcsncpy_s(e->instance_key, 160, inst_path, _TRUNCATE);
            parse_edid(edid, got, &e->edid);
            count++;
        }
        RegCloseKey(dev_key);
    }
    RegCloseKey(display);
    if (g_diag) t_out_line(L"[DIAG] total EDID entries = %d", count);
    return count;
}

/* ------------------------------------------------------------------ */
/* Monitor enumeration                                                 */
/* ------------------------------------------------------------------ */

static int g_index = 0;
static FILE *g_snapshot = NULL;

static void emit(const wchar_t *fmt, ...)
{
    wchar_t buf[SNAP_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, SNAP_LINE_LEN, _TRUNCATE, fmt, ap);
    va_end(ap);

    /* Console + diagnostic log + optional snapshot file. The log copy matters
       most: this program's whole output is measurements, and a console that
       closes with the process would lose every one of them. */
    t_out_line(L"%ls", buf);

    if (g_snapshot) {
        /* fwprintf, not t_out_line: this writes to the snapshot FILE only.
           A batch edit once turned this into "ft_out_line" by matching the
           "wprintf(" inside "fwprintf(" - keep the f prefix intact. */
        fwprintf(g_snapshot, L"%ls\n", buf);
        fflush(g_snapshot);
    }
}

static BOOL CALLBACK monitor_proc(HMONITOR hmon, HDC hdc, LPRECT rect, LPARAM lp)
{
    (void)hdc; (void)rect;
    (void)lp;
    int idx = g_index++;

    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, (MONITORINFO *)&mi)) return TRUE;

    /* Match the monitor device name ("\\.\DISPLAY1") to its adapter entry so
       we can report the adapter description and state flags. This is the
       only link Windows gives us between HMONITOR and a human-readable
       device string; the EDID blob is reported separately below because
       mapping it to an HMONITOR needs SetupAPI. */
    DISPLAY_DEVICEW dd;
    ZeroMemory(&dd, sizeof(dd));
    dd.cb = sizeof(dd);
    wchar_t dd_string[128] = L"";
    DWORD   dd_state = 0;

    for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); ++i) {
        if (_wcsicmp(dd.DeviceName, mi.szDevice) == 0) {
            dd_state = dd.StateFlags;
            wcsncpy_s(dd_string, 128, dd.DeviceString, _TRUNCATE);
            break;
        }
    }

    /* DPI for this specific monitor. GetDpiForMonitor is the per-monitor
       API; fall back to the process DPI if shcore is unavailable. */
    UINT dpi_x = 0, dpi_y = 0;
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        typedef HRESULT (WINAPI *PFN)(HMONITOR, int, UINT *, UINT *);
        PFN fn = (PFN)(void *)GetProcAddress(shcore, "GetDpiForMonitor");
        if (fn) fn(hmon, 0, &dpi_x, &dpi_y);
        FreeLibrary(shcore);
    }

    LONG w = mi.rcMonitor.right - mi.rcMonitor.left;
    LONG h = mi.rcMonitor.bottom - mi.rcMonitor.top;

    emit(L"[MON.%d.name] %ls", idx, mi.szDevice);
    emit(L"[MON.%d.rcMonitor] %ld,%ld %ldx%ld", idx, mi.rcMonitor.left, mi.rcMonitor.top, w, h);
    emit(L"[MON.%d.rcWork] %ld,%ld %ldx%ld", idx,
         mi.rcWork.left, mi.rcWork.top,
         mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top);
    emit(L"[MON.%d.primary] %d", idx, (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0);
    emit(L"[MON.%d.dpi] %u  scale=%u%%", idx, dpi_x, dpi_x ? (dpi_x * 100 / 96) : 0);
    emit(L"[MON.%d.adapter_string] %ls", idx, dd_string[0] ? dd_string : L"(none)");
    emit(L"[MON.%d.adapter_flags] 0x%08lX desktop=%d primary=%d",
         idx, (unsigned long)dd_state,
         (dd_state & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ? 1 : 0,
         (dd_state & DISPLAY_DEVICE_PRIMARY_DEVICE) ? 1 : 0);

    /* Geometry fingerprint fields, spelled out individually so the diff
       shows exactly which component of level 3 changed. */
    emit(L"[MON.%d.fp.width] %ld", idx, w);
    emit(L"[MON.%d.fp.height] %ld", idx, h);
    emit(L"[MON.%d.fp.vx] %ld", idx, mi.rcMonitor.left);
    emit(L"[MON.%d.fp.vy] %ld", idx, mi.rcMonitor.top);

    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Comparison                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    wchar_t key[64];
    wchar_t value[256];
} SnapKV;

typedef struct {
    SnapKV line[SNAP_MAX_LINES];
    int    count;
} Snapshot;

static void snapshot_load(const wchar_t *path, Snapshot *snap)
{
    ZeroMemory(snap, sizeof(*snap));
    FILE *f = _wfopen(path, L"rt, ccs=UTF-8");
    if (!f) return;

    wchar_t buf[SNAP_LINE_LEN];
    while (fgetws(buf, SNAP_LINE_LEN, f) && snap->count < SNAP_MAX_LINES) {
        /* strip newline */
        size_t n = wcslen(buf);
        while (n > 0 && (buf[n - 1] == L'\n' || buf[n - 1] == L'\r')) buf[--n] = L'\0';

        const wchar_t *lb = wcschr(buf, L'[');
        const wchar_t *rb = lb ? wcschr(lb, L']') : NULL;
        if (!lb || !rb || rb <= lb + 1) continue;

        size_t klen = (size_t)(rb - lb - 1);
        if (klen >= 64) continue;
        wcsncpy_s(snap->line[snap->count].key, 64, lb + 1, _TRUNCATE);
        wcsncpy_s(snap->line[snap->count].value, 256, rb + 1, _TRUNCATE);
        snap->count++;
    }
    fclose(f);
}

static const wchar_t *snapshot_find(const Snapshot *s, const wchar_t *key)
{
    for (int i = 0; i < s->count; ++i) {
        if (!wcscmp(s->line[i].key, key)) return s->line[i].value;
    }
    return NULL;
}

static void compare_snapshots(const wchar_t *path_a, const wchar_t *path_b)
{
    Snapshot a, b;
    snapshot_load(path_a, &a);
    snapshot_load(path_b, &b);

    if (a.count == 0) { T_WARN(L"snapshot A is empty or unreadable: %ls", path_a); return; }
    if (b.count == 0) { T_WARN(L"snapshot B is empty or unreadable: %ls", path_b); return; }

    T_SEC(L"Field-by-field comparison");
    t_out_line(L"\n  %-32ls %-22ls %-22ls %ls", L"key", L"before", L"after", L"changed");

    int changed = 0, same = 0, only_a = 0, only_b = 0;

    for (int i = 0; i < a.count; ++i) {
        const wchar_t *bv = a.line[i].value;
        const wchar_t *av = snapshot_find(&b, a.line[i].key);
        if (!av) {
            t_out_line(L"  %-32ls %-22ls %-22ls %ls", a.line[i].key, bv, L"(absent)", L"REMOVED");
            only_a++;
            continue;
        }
        int diff = wcscmp(bv, av) != 0;
        t_out_line(L"  %-32ls %-22ls %-22ls %ls",
                a.line[i].key, bv, av, diff ? L"*** CHANGED ***" : L"");
        if (diff) changed++; else same++;
    }

    for (int i = 0; i < b.count; ++i) {
        if (!snapshot_find(&a, b.line[i].key)) {
            t_out_line(L"  %-32ls %-22ls %-22ls %ls",
                    b.line[i].key, L"(absent)", b.line[i].value, L"ADDED");
            only_b++;
        }
    }

    T_SEC(L"Comparison summary");
    T_KV(L"unchanged_fields", L"%d", same);
    T_KV(L"changed_fields", L"%d", changed);
    T_KV(L"removed_fields", L"%d  (monitor disappeared)", only_a);
    T_KV(L"added_fields", L"%d  (monitor appeared)", only_b);

    /* ---------------- the actual question ---------------- */
    T_SEC(L"Does the plan's 3-level match survive this?");
    T_NOTE(L"Level 1 uses device name (%ls).", L"MON.n.name");
    T_NOTE(L"Level 2 uses the EDID friendly name.");
    T_NOTE(L"Level 3 uses (width, height, virtual_x, virtual_y).");
    T_NOTE(L"");
    T_NOTE(L"Read the table above and answer:");
    T_NOTE(L"  a) Did [MON.n.name] change?  If yes, level 1 fails after this");
    T_NOTE(L"     operation and level 2 must carry the match.");
    T_NOTE(L"  b) Is there any EDID-based field that stayed identical?");
    T_NOTE(L"     If yes, level 2 is viable. If no, only geometry remains.");
    T_NOTE(L"  c) Did the resolution or virtual position change too?");
    T_NOTE(L"     If everything changed at once, only level 4 (ORPHANED) is left,");
    T_NOTE(L"     and stage 7's conflict UI becomes the primary mechanism.");

    T_KV(L"verdict", L"fill in after reading the table; record in docs/P0验证记录.md");
}

/* ------------------------------------------------------------------ */

static int usage(void)
{
    t_out_line(L"usage: t06_monitor.exe [--snapshot NAME] [--compare A B] [--edid] [--no-pause]\n"
            L"\n"
            L"  --snapshot NAME   write <NAME>.monitor-snapshot next to this exe\n"
            L"  --compare A B     diff two previously captured snapshots\n"
            L"  --edid            also list EDID keys found in the registry\n");
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    /* Crash filter + file log first: a console that flashes and closes
       takes its output with it. See TRAPS.md. */
    t_diag_init(L"t06_monitor");
    t_log_mirror(1);   /* full report goes to the log by default */

    t_console_init();

    const wchar_t *snap_name = NULL;
    const wchar_t *cmp_a = NULL, *cmp_b = NULL;
    int show_edid = 0;

    for (int i = 1; i < argc; ++i) {
        if      (!wcscmp(argv[i], L"--snapshot") && i + 1 < argc) snap_name = argv[++i];
        else if (!wcscmp(argv[i], L"--compare")  && i + 2 < argc) { cmp_a = argv[++i]; cmp_b = argv[++i]; }
        else if (!wcscmp(argv[i], L"--edid")) show_edid = 1;
        else if (!wcscmp(argv[i], L"--diag")) g_diag = 1;
        else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) return usage();
    }

    t_set_dpi_awareness();

    /* ---------------- compare mode ---------------- */
    if (cmp_a && cmp_b) {
        wchar_t pa[MAX_PATH], pb[MAX_PATH];
        _snwprintf_s(pa, MAX_PATH, _TRUNCATE, L"%ls.monitor-snapshot", cmp_a);
        _snwprintf_s(pb, MAX_PATH, _TRUNCATE, L"%ls.monitor-snapshot", cmp_b);

        T_TITLE(L"t06_monitor - compare");
        T_KV(L"before_file", L"%ls", pa);
        T_KV(L"after_file", L"%ls", pb);
        compare_snapshots(pa, pb);

        t_pause_if_console();
        t_log_close();
        return 0;
    }

    /* ---------------- EDID enumeration ---------------- */
    static EdidEntry entries[MAX_MONITORS];
    int edid_count = enumerate_edid(entries, MAX_MONITORS);

    T_TITLE(L"t06_monitor - monitor identity snapshot");
    T_KV(L"run_at", L"%ls", t_now_string());
    T_KV(L"snapshot_name", L"%ls", snap_name ? snap_name : L"(console only)");

    if (snap_name) {
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls.monitor-snapshot", snap_name);
        g_snapshot = _wfopen(path, L"wt, ccs=UTF-8");
        if (g_snapshot) T_KV(L"writing", L"%ls", path);
        else            T_WARN(L"could not open %ls for writing", path);
    }

    /* ---------------- monitors ---------------- */
    T_SEC(L"EnumDisplayMonitors");
    g_index = 0;
    EnumDisplayMonitors(NULL, NULL, monitor_proc, (LPARAM)entries);
    emit(L"[MONITOR.count] %d", g_index);

    /* ---------------- EDID ---------------- */
    T_SEC(L"EDID entries in the registry");
    emit(L"[EDID.count] %d", edid_count);

    for (int i = 0; i < edid_count; ++i) {
        EdidEntry *e = &entries[i];
        if (!e->edid.valid) {
            emit(L"[EDID.%d] %ls -> unparsable", i, e->device_key);
            continue;
        }
        emit(L"[EDID.%d.pnp] %hs", i, e->edid.manufacturer);
        emit(L"[EDID.%d.product] 0x%04X", i, e->edid.product_code);
        emit(L"[EDID.%d.serial] 0x%08lX", i, (unsigned long)e->edid.serial);
        emit(L"[EDID.%d.name] %ls", i,
             e->edid.product_name[0] ? e->edid.product_name : L"(no name descriptor)");
        if (show_edid) emit(L"[EDID.%d.key] %ls", i, e->instance_key);
    }

    if (edid_count == 0) {
        T_WARN(L"No EDID blobs found. The friendly name is unavailable, so");
        T_WARN(L"level 2 of the monitor match cannot be implemented as planned.");
        T_WARN(L"Fall back to device name + geometry, and record this in the P0 log.");
    }

    /* ---------------- how to run the experiment ---------------- */
    T_SEC(L"How to run this experiment");
    T_NOTE(L"1. With the machine in its normal state:");
    T_NOTE(L"     t06_monitor.exe --snapshot before");
    T_NOTE(L"2. Perform ONE of these operations:");
    T_NOTE(L"     unplug and replug the monitor (same port)");
    T_NOTE(L"     change which monitor is primary");
    T_NOTE(L"     change the resolution");
    T_NOTE(L"     disable and re-enable the display adapter");
    T_NOTE(L"     sleep and wake the machine");
    T_NOTE(L"     reinstall or roll back the GPU driver");
    T_NOTE(L"3. Capture again:");
    T_NOTE(L"     t06_monitor.exe --snapshot after");
    T_NOTE(L"4. Diff them:");
    T_NOTE(L"     t06_monitor.exe --compare before after");
    T_NOTE(L"5. Reset and repeat the whole sequence for the next operation.");
    T_NOTE(L"   Each operation must be measured separately - that is the whole");
    T_NOTE(L"   point, because level 1 may survive one and not another.");

    if (g_snapshot) {
        fclose(g_snapshot);
        g_snapshot = NULL;
        T_KV(L"snapshot_complete", L"yes");
    }

    t_pause_if_console();
    t_log_close();
    return 0;
}
