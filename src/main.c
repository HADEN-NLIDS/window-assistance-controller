/*
 * main.c - program entry point, message loop, mode state machine, hotkey.
 *
 * This file owns the things that must exist exactly once:
 *
 *   - the single-instance mutex
 *   - the hidden message window that receives WM_HOTKEY
 *   - the mode state machine (every transition goes through mode_transition)
 *   - the shutdown path
 *
 * Everything else is delegated: the window lives in ui/mainwindow.c, the
 * data in model/workspace.c, persistence in model/store.c.
 *
 * STAGE NOTE
 *
 *   Stages 2 lays the frame: the window lists workspaces and the hotkey is
 *   registered, but "Apply" and "Edit" are not wired to the launcher or the
 *   overlay yet (stages 5 and 6). They report honestly that the feature is
 *   not present rather than pretending to work.
 */

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "appconfig.h"          /* saving the sections this window owns */
#include "ui/mainwindow.h"
#include "ui/i18n.h"
#include "ui/page_settings.h"   /* Settings: the values main.c persists */
#include "ui/editstate.h"       /* the full-screen edit session (stage 6) */
#include "ui/close_confirm.h"
#include "ui/rename_dlg.h"
#include "util/log.h"
#include "util/cleanup.h"
#include "util/strutil.h"
#include "util/sysinfo.h"
#include "core/win_enum.h"
#include "core/relcoord.h"      /* rel_from_pixels: a dropped program keeps its size */
#include "core/monitor.h"
#include "core/displaywatch.h"  /* WM_DISPLAYCHANGE: debounce + topology diff */
#include "core/rule_apply.h"
#include "core/winwatch.h"
#include "core/appindex.h"      /* the installed-application list (stage 8) */
#include "core/watchdog.h"      /* edit-mode guard: an independent thread (6.10) */
#include "model/store.h"
#include "model/workspace.h"
#include "model/workspace_run.h"
#include "model/runstate.h"
#include "model/rule.h"
#include "model/pickup.h"   /* [pick up this layout]: the screen -> presets (6.7) */
#include "model/config.h"   /* config_dir / config_file, shared with the CLI */
#include "cli/cli.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define WAC_MUTEX_NAME   L"Local\\WacSingleInstanceMutex"
#define WAC_MSG_WINDOW   L"WacMessageWindow"

/* Posted by a second instance to ask the first one to show itself. */
#define WM_WAC_ACTIVATE  (WM_APP + 1)

/* Tray icon. */
#define WAC_TRAY_ID      1
#define WM_WAC_TRAY      (WM_APP + 2)
#define IDM_TRAY_SHOW    3001
#define IDM_TRAY_QUIT    3002

/*
 * The resident listener (task 5.11).
 *
 * core/winwatch.c posts this to the hidden message window when a window
 * appears anywhere on the desktop; wParam carries the HWND. Nothing is
 * decided in the hook callback, only here on the main thread - see winwatch.h.
 *
 * The settle timer that waits for a new window to stop moving itself lives on
 * the same window, and exists ONLY while there is a window waiting.
 */
#define WM_WAC_WATCH       (WM_APP + 3)
#define WAC_WATCH_TIMER_ID 1

/*
 * The application index finished (stage 8).
 *
 * Posted by the worker thread that appindex_job_start() creates - the same
 * shape the window listener uses, so the UI thread never waits for either and
 * the worker never touches a window.
 */
#define WM_WAC_APPINDEX    (WM_APP + 4)

/*
 * Monitor topology changes (stage 7, task 7.1).
 *
 * WM_DISPLAYCHANGE arrives on the same window, and the timer that waits for
 * the topology to stop moving is a SECOND id on that window - so one window
 * carries all three of this program's asynchronous inputs: the hotkey, the
 * resident window listener, and the display.
 */
#define WAC_DISPLAY_TIMER_ID 2

/*
 * How long a broken topology is given to come back (stage 7, task 7.2).
 *
 * A third id on the same window. It runs only while something is actually
 * broken, so the program has no timer at all in the normal case - the same
 * rule the window listener's settle timer follows.
 */
#define WAC_GRACE_TIMER_ID 3

/* Hotkey: three modifiers plus F12 is very unlikely to be taken, and the
 * candidate list is what stage-1 experiment 5 measured. */
#define WAC_HOTKEY_ID    1

/* ------------------------------------------------------------------ */
/* Mode state machine                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    MODE_IDLE = 0,        /* nothing in flight; window may be shown or hidden */
    MODE_LAUNCHING,       /* starting a workspace (stage 5) */
    MODE_EDIT_OVERLAY,    /* preset editor open (stage 6) */
    MODE_EDIT_SIDEBAR,    /* editor with the app sidebar (stage 8) */
    MODE_SHUTTING_DOWN    /* cleaning up; refuse new work */
} AppMode;

static const wchar_t *mode_name(AppMode m)
{
    switch (m) {
    case MODE_IDLE:          return L"IDLE";
    case MODE_LAUNCHING:     return L"LAUNCHING";
    case MODE_EDIT_OVERLAY:  return L"EDIT_OVERLAY";
    case MODE_EDIT_SIDEBAR:  return L"EDIT_SIDEBAR";
    case MODE_SHUTTING_DOWN: return L"SHUTTING_DOWN";
    }
    return L"?";
}

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    HINSTANCE   hinst;
    HWND        msgwnd;          /* hidden window: hotkey + tray + IPC */
    HANDLE      mutex;

    /*
     * The installed-application list (stage 8), built on demand and kept for
     * the life of the process. NULL until the user asks for it - "no index
     * scan at start-up" is an acceptance item, and it is also what keeps the
     * 60-second idle CPU budget at zero.
     */
    AppIndex    *appindex;
    AppIndexJob *appjob;         /* the worker, while it is building */

    MainWindow *window;
    AppMode     mode;

    StoreDocument *doc;
    WorkspaceSet   workspaces;

    /*
     * Window presets (the [rule:*] sections). It lives here rather than in the
     * window because this is the only object that must exist exactly once -
     * and it is why g_app is a static: a RuleSet is about 70 KB, far too much
     * for a stack frame (TRAPS.md #11).
     */
    RuleSet        rules;

    char        active_id[WS_ID_MAX];

    /*
     * The monitor topology as it was when it was last believed (stage 7).
     *
     * Kept as a snapshot rather than re-read on demand, because the question
     * WM_DISPLAYCHANGE asks is "what is different NOW" - and that needs the
     * previous answer to compare against. Refreshed at start-up and after every
     * settled change, and nowhere else.
     */
    DisplayWatch       dwatch;
    MonitorFingerprint disp_snapshot[WAC_MON_MAX];
    int                disp_snapshot_count;
    DisplayGrace       grace;
    int                disp_orphans;   /* slots unusable at the last evaluation */

    /*
     * Which workspace the running edit session is naming.
     *
     * The overlay is handed the NAME to draw and knows nothing else - so when
     * the user finishes typing, this is how the callback finds out where the
     * new name belongs. Empty when no session is running, and when the session
     * was entered with nothing selected.
     */
    char        edit_workspace_id[WS_ID_MAX];

    BOOL        hotkey_ok;
    UINT        hotkey_mods;
    UINT        hotkey_vk;
    char        hotkey_label[64];

    /* User-visible preferences. Owned by src/ui/page_settings.c so that
     * adding a setting touches one file. */
    Settings    settings;
    BOOL        tray_added;
} App;

static App g_app;

/*
 * Is the Windows session ending (shutdown, restart, logoff)?
 *
 * REPORTED BY THE USER 2026-10-10: a prompt appeared while the machine was
 * shutting down. The mechanism: Windows sends WM_CLOSE to the top-level windows
 * on its way out, that arrives as MW_CMD_QUIT, and with close_action == ASK the
 * program asked "keep running or quit?" - at the one moment when the answer
 * cannot matter and when a modal box can make Windows tell the user that this
 * application is preventing shutdown.
 *
 * The session-end message (WM_QUERYENDSESSION) always arrives BEFORE those
 * closes, so this flag is set by the time it matters. While it is set:
 *
 *   - every prompt is suppressed (this file and ui/mainwindow.c);
 *   - the program saves and goes, which is what the WM_ENDSESSION handler has
 *     always done for the session-end path itself.
 *
 * It is never cleared: a session that is ending does not stop ending.
 */
static BOOL g_session_ending = FALSE;

BOOL app_session_ending(void)
{
    return g_session_ending;
}

/*
 * A one-line note to show in the status bar once the window exists.
 *
 * Startup INFORMATION goes here instead of into a dialog. This program is
 * meant to sit in the tray and start with Windows, and a modal box that
 * appears on every boot is exactly what "no popups" rules out - the user asked
 * for something that behaves like it is not there.
 *
 * Only a failure the user has to ACT on still gets a dialog: an unrecoverable
 * configuration file (otherwise they would think their presets vanished) and a
 * window that could not be created at all.
 *
 * Deferring also fixes a smaller wart: load_settings() runs before the
 * language is known, so a dialog there was always in the built-in default.
 */
static wchar_t g_startup_note[320];

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */



/* ------------------------------------------------------------------ */
/* Mode transitions                                                    */
/* ------------------------------------------------------------------ */

/*
 * The only place g_app.mode is assigned.
 *
 * Leaving an edit mode must release the editor's resources. Nothing is
 * allocated yet (the editor is stage 6), but the hook is here so that when
 * it arrives there is exactly one place that has to remember.
 */
static BOOL mode_transition(AppMode next)
{
    const AppMode prev = g_app.mode;

    if (prev == MODE_SHUTTING_DOWN) return FALSE;

    if ((prev == MODE_EDIT_OVERLAY || prev == MODE_EDIT_SIDEBAR) &&
        next != MODE_EDIT_OVERLAY && next != MODE_EDIT_SIDEBAR) {
        /*
         * Edit-mode resources are NOT released here: edit_poll() is the single
         * release point (ui/editstate.c), and it runs the moment the session
         * ends rather than when the mode label changes. This branch records
         * the transition so the log shows both events.
         */
        log_info(L"mode: leaving edit mode (%ls -> %ls)",
                 mode_name(prev), mode_name(next));
    }

    g_app.mode = next;
    log_info(L"mode: %ls -> %ls", mode_name(prev), mode_name(next));
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Tray icon                                                           */
/* ------------------------------------------------------------------ */

static void tray_add(void)
{
    if (g_app.tray_added || g_app.msgwnd == NULL) return;

    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g_app.msgwnd;
    nid.uID              = WAC_TRAY_ID;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_WAC_TRAY;
    nid.hIcon            = LoadIconW(NULL, IDI_APPLICATION);
    wcsncpy_s(nid.szTip, _countof(nid.szTip), i18n_text(STR_ERR_TITLE), _TRUNCATE);

    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        g_app.tray_added = TRUE;
        log_info(L"tray: icon added");
    } else {
        log_info(L"tray: Shell_NotifyIcon(NIM_ADD) failed");
    }
}

static void tray_remove(void)
{
    if (!g_app.tray_added || g_app.msgwnd == NULL) return;

    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd   = g_app.msgwnd;
    nid.uID    = WAC_TRAY_ID;

    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_app.tray_added = FALSE;
}

/* ------------------------------------------------------------------ */
/* Status line                                                         */
/* ------------------------------------------------------------------ */

/*
 * The status line.
 *
 * The i18n table holds two kinds of entry (see src/ui/i18n.h): TEMPLATES
 * contain placeholders and are only meaningful as the FORMAT argument of a
 * printf-family call; finished text is shown as-is. Passing a template as an
 * argument prints its placeholder literally - which is exactly how a bare
 * "%hs" sat in this status bar (TRAPS.md #23). Here
 * STR_STATUS_ACTIVE, STR_STATUS_PERM and STR_STATUS_HOTKEY are formats, while
 * STR_STATUS_NO_WORKSPACE, STR_STATUS_PERM_* and STR_STATUS_HOTKEY_NONE are
 * finished text.
 *
 * The hotkey is always shown, including when registration failed: the user
 * needs to know whether the emergency exit will work at all.
 *
 * The permission state is here because it decides what the program can do:
 * without it an elevated window that refuses to move looks like a bug in this
 * program rather than a boundary of the system.
 */
static void refresh_status(void)
{
    wchar_t buf[512];
    wchar_t active[256];
    wchar_t perm[96];
    wchar_t rights[64];
    wchar_t hotkey[128];
    wchar_t key[80];

    if (g_app.active_id[0]) {
        const Workspace *w = ws_find_const(&g_app.workspaces, g_app.active_id);
        wchar_t a[WS_NAME_MAX + 8];
        /* %hs converts the narrow workspace name to wide here; the result is
         * then handed to the %ls template below. */
        _snwprintf_s(a, WS_NAME_MAX + 8, _TRUNCATE, L"%hs",
                     w ? w->name : g_app.active_id);
        _snwprintf_s(active, 256, _TRUNCATE, i18n_text(STR_STATUS_ACTIVE), a);
    } else {
        _snwprintf_s(active, 256, _TRUNCATE, L"%ls",
                     i18n_text(STR_STATUS_NO_WORKSPACE));
    }

    /* Same function the settings page and the guide use: one answer for one
     * process, so the three places cannot describe it differently. */
    _snwprintf_s(rights, 64, _TRUNCATE, L"%ls",
                 i18n_text(sysinfo_is_elevated() ? STR_STATUS_PERM_ADMIN
                                                 : STR_STATUS_PERM_NORMAL));
    _snwprintf_s(perm, 96, _TRUNCATE, i18n_text(STR_STATUS_PERM), rights);

    if (g_app.hotkey_ok && g_app.hotkey_label[0]) {
        _snwprintf_s(key, 80, _TRUNCATE, L"%hs", g_app.hotkey_label);
    } else {
        _snwprintf_s(key, 80, _TRUNCATE, L"%ls",
                     i18n_text(STR_STATUS_HOTKEY_NONE));
    }
    _snwprintf_s(hotkey, 128, _TRUNCATE, i18n_text(STR_STATUS_HOTKEY), key);

    _snwprintf_s(buf, 512, _TRUNCATE, L"%ls   |   %ls   |   %ls",
                 active, perm, hotkey);

    /*
     * Stage 7 / task 7.3: say how many slots cannot be used right now, and WHY.
     * It hangs off the one line the user already reads, so nothing new has to be
     * noticed - and it disappears by itself: refresh_status() rebuilds this line
     * from scratch, so a recovery inside the grace period leaves no leftover
     * prompt ("自动恢复，无提示残留" is the acceptance wording).
     */
    if (g_app.disp_orphans > 0) {
        wchar_t warn[192];
        _snwprintf_s(warn, 192, _TRUNCATE,
                     i18n_text(STR_MSG_SLOTS_UNAVAILABLE_FMT), g_app.disp_orphans);
        _snwprintf_s(buf, 512, _TRUNCATE, L"%ls   |   %ls",
                     active, warn);
    }

    mainwindow_set_status(g_app.window, buf);
}

static void refresh_list(void)
{
    mainwindow_set_workspaces(g_app.window, &g_app.workspaces);
    if (g_app.active_id[0]) {
        mainwindow_set_active(g_app.window, g_app.active_id);
    }
    refresh_status();
}

/* ------------------------------------------------------------------ */
/* Window presets and the overview page                                */
/* ------------------------------------------------------------------ */

static void load_rules(void)
{
    const RulesLoadResult r = rules_load(&g_app.rules);

    switch (r) {
    case RULES_LOADED:
        log_info(L"rules: loaded %d", rules_count(&g_app.rules));
        break;

    case RULES_MISSING:
        /* Nothing stored yet is the normal first run, not an error. */
        log_info(L"rules: none stored yet");
        break;

    case RULES_RECOVERED:
        log_info(L"rules: recovered from a backup");
        /* A dialog here would break the "no popups on startup" rule the tray
         * behaviour depends on: nothing is lost, the user just needs to know
         * which file was used. The status bar says it once the window is up. */
        wcsncpy_s(g_startup_note, _countof(g_startup_note),
                  i18n_text(STR_MSG_CONFIG_RECOVERED), _TRUNCATE);
        break;

    case RULES_MODIFIED:
        log_info(L"rules: file was written outside this program; contents accepted");
        wcsncpy_s(g_startup_note, _countof(g_startup_note),
                  i18n_text(STR_MSG_CONFIG_MODIFIED), _TRUNCATE);
        break;

    case RULES_UNAVAILABLE:
    default:
        /* rule.h is explicit that this one MUST be reported: an empty list
         * caused by an unreadable file looks exactly like "you have no presets",
         * and the user would go and re-create them. */
        log_info(L"rules: could not be read");
        MessageBoxW(mainwindow_hwnd(g_app.window),
                    i18n_text(STR_MSG_CONFIG_DAMAGED),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONWARNING);
        break;
    }
}

/*
 * What to call a preset in the list: the user's label if there is one, else the
 * executable, else the id. Never empty - an unnamed row reads as a rendering
 * bug rather than as "this preset has no name".
 */
static void rules_display_name(const Rule *r, wchar_t *out, size_t cch)
{
    if (r->label[0] != L'\0') {
        wcsncpy_s(out, cch, r->label, _TRUNCATE);
    } else if (r->app.exe_name[0] != L'\0') {
        wcsncpy_s(out, cch, r->app.exe_name, _TRUNCATE);
    } else if (r->app.exe_path[0] != L'\0') {
        wcsncpy_s(out, cch, win_file_name(r->app.exe_path), _TRUNCATE);
    } else {
        _snwprintf_s(out, cch, _TRUNCATE, L"%hs", r->id);
    }
}

/*
 * The zone as the user thinks of it.
 *
 * The two kinds of preset print differently because they MEAN different things:
 * a fractional one is a share of the screen,
 * a remembered one is a pixel size that merely happens to be positioned by
 * fraction. Showing a remembered 1280x1600 as "50%x100%" would hide the very
 * distinction that decides how it behaves on another monitor.
 */
static void format_zone(const Rule *r, wchar_t *out, size_t cch)
{
    if (r->zone.abs_w > 0 && r->zone.abs_h > 0) {
        _snwprintf_s(out, cch, _TRUNCATE, L"%.0f%%,%.0f%%  %dx%d",
                     (double)(r->zone.x * 100.0f), (double)(r->zone.y * 100.0f),
                     r->zone.abs_w, r->zone.abs_h);
    } else {
        _snwprintf_s(out, cch, _TRUNCATE, L"%.0f%%,%.0f%%  %.0f%%x%.0f%%",
                     (double)(r->zone.x * 100.0f), (double)(r->zone.y * 100.0f),
                     (double)(r->zone.w * 100.0f), (double)(r->zone.h * 100.0f));
    }
}

/* The screen width the remembered size was measured against. */
static void format_monitor(const Rule *r, wchar_t *out, size_t cch)
{
    if (r->zone.ref_w > 0) {
        _snwprintf_s(out, cch, _TRUNCATE, L"%d px", r->zone.ref_w);
    } else {
        wcsncpy_s(out, cch, L"\u2014", _TRUNCATE);   /* em dash */
    }
}

/*
 * Fill both overview lists.
 *
 * Called on demand only - at startup, on [Refresh], and after an action. Never
 * from a timer: enumerating top-level windows costs a few milliseconds on this
 * machine (256+ of them, mostly invisible helpers), so polling would show up
 * immediately in the idle-CPU budget the project holds itself to.
 */
static void refresh_overview(void)
{
    if (g_app.window == NULL) return;

    /* Large objects live in static storage: a MwPresetRow set is tens of KB and
     * a WindowInfo set is a few hundred KB - neither belongs on the stack
     * (TRAPS.md #11). This runs on the UI thread only. */
    static MwPresetRow presets[MW_ROW_MAX];
    static MwWindowRow rows[MW_ROW_MAX];
    static WindowInfo  wins[WAC_WIN_MAX];

    /* ---- window presets ------------------------------------------ */
    int preset_count = 0;

    for (int i = 0; i < rules_count(&g_app.rules) && preset_count < MW_ROW_MAX; ++i) {
        const Rule *r = rules_at(&g_app.rules, i);
        if (r == NULL) continue;

        MwPresetRow *row = &presets[preset_count++];
        ZeroMemory(row, sizeof(*row));

        const wchar_t *exe = (r->app.exe_name[0] != L'\0')
                                 ? r->app.exe_name
                                 : win_file_name(r->app.exe_path);
        wcsncpy_s(row->app, MW_APP_MAX, exe, _TRUNCATE);
        rules_display_name(r, row->label, MW_TEXT_MAX);
        format_zone(r, row->zone, 64);
        format_monitor(r, row->monitor, 64);
        wcsncpy_s(row->state, 32,
                  i18n_text(r->enabled ? STR_PRESET_ENABLED : STR_PRESET_DISABLED),
                  _TRUNCATE);
        strncpy_s(row->id, sizeof(row->id), r->id, _TRUNCATE);

        /* The editor works on real numbers, not on the display string above. */
        row->x = (double)r->zone.x;
        row->y = (double)r->zone.y;
        row->w = (double)r->zone.w;
        row->h = (double)r->zone.h;
        row->locked_size = r->zone.lock_size ? TRUE : FALSE;
        row->pinned      = rule_is_pinned(r) ? TRUE : FALSE;
    }
    mainwindow_set_preset_rows(g_app.window, presets, preset_count);

    /* ---- running windows ----------------------------------------- */
    const int n = win_enum_top(wins, WAC_WIN_MAX, WIN_ENUM_DEFAULT);
    int count = 0;

    for (int i = 0; i < n && count < MW_ROW_MAX; ++i) {
        const WindowInfo *w = &wins[i];
        MwWindowRow *row = &rows[count++];
        ZeroMemory(row, sizeof(*row));

        wcsncpy_s(row->app, MW_APP_MAX, win_file_name(w->exe_path), _TRUNCATE);
        wcsncpy_s(row->title, MW_TEXT_MAX, w->title, _TRUNCATE);
        _snwprintf_s(row->size, 48, _TRUNCATE, L"%dx%d",
                     (int)(w->rect.right - w->rect.left),
                     (int)(w->rect.bottom - w->rect.top));
        row->hwnd = (unsigned long long)(ULONG_PTR)w->hwnd;

        const int mi = rules_match(&g_app.rules, w);
        if (mi >= 0) {
            const Rule *r = rules_at(&g_app.rules, mi);
            wchar_t name[MW_TEXT_MAX];
            rules_display_name(r, name, MW_TEXT_MAX);
            _snwprintf_s(row->state, MW_TEXT_MAX, _TRUNCATE,
                         i18n_text(STR_MATCH_OK), name);
        } else {
            wcsncpy_s(row->state, MW_TEXT_MAX, i18n_text(STR_MATCH_NONE), _TRUNCATE);
        }
    }
    mainwindow_set_window_rows(g_app.window, rows, count);

    log_info(L"overview: %d window(s), %d preset(s)", count, preset_count);
}

/* Everything the main window shows, refreshed together. */
/* Defined below, next to the other refresh helpers. */
static void refresh_slots(void);

static void refresh_all(void)
{
    refresh_list();
    refresh_overview();
    refresh_slots();
}

/* ------------------------------------------------------------------ */
/* Preset commands                                                     */
/* ------------------------------------------------------------------ */

/*
 * Save a preset for the window selected in the overview list: "this program
 * belongs here".
 *
 * The zone comes from where the window IS right now, via rel_from_pixels, so
 * the preset means what `wac place --remember` means: keep the size the user
 * made, positioned as a share of the screen. min_w/min_h are cleared for the
 * same reason cli.c clears them - a remembered window must not inherit an
 * arbitrary pixel floor it never had.
 *
 * Doing it the other way (fractions for the size too) would turn a 1280x1600
 * half-screen window into "half of whatever screen it lands on", which is the
 * bug that reshaped a 16:9 window into 9:16.
 */
/*
 * The preset most recently created by anchor_window(), or empty.
 *
 * A static rather than a return value because anchor_window() has five failure
 * paths that all end in "nothing was saved", and threading a return through
 * every one of them buys nothing here: this runs on the UI thread, one call at
 * a time, and the value is consumed immediately by the caller.
 */
static char g_last_anchored_id[64];

/*
 * Remember where this window is now. On success g_last_anchored_id names the
 * preset, so the caller can open the editor on it.
 */
static void anchor_window(HWND target)
{
    const HWND owner = mainwindow_hwnd(g_app.window);

    g_last_anchored_id[0] = '\0';

    WindowInfo info;
    ZeroMemory(&info, sizeof(info));
    if (!win_fill_info(target, &info, WIN_ENUM_NEED_EXE | WIN_ENUM_NEED_TITLE)) {
        MessageBoxW(owner, i18n_text(STR_MSG_ANCHOR_FAILED),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONWARNING);
        return;
    }

    /* A minimised window sits at about (-32000,-32000); anchoring it would
     * store a position that means nothing at all. */
    if (info.is_iconic) {
        MessageBoxW(owner, i18n_text(STR_MSG_ANCHOR_MINIMIZED),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        return;
    }

    RelRect rel;
    ZeroMemory(&rel, sizeof(rel));

    if (info.is_zoomed) {
        /*
         * A maximised window has no meaningful "here" either: GetWindowRect
         * returns the work area PLUS the invisible resize border, so this
         * machine reports 2582x1622 for a 2560x1600 screen. Storing that would
         * make the preset wider than the display and force a 1.4% shrink on
         * every future launch.
         *
         * "Maximised" is expressible in the existing model, so no new field is
         * needed: the whole screen, with no remembered pixel size (which is
         * what makes the fractions the source of truth).
         */
        rel.x = 0.0f; rel.y = 0.0f; rel.w = 1.0f; rel.h = 1.0f;
        rel.min_w = 1; rel.min_h = 1;
        rel.abs_w = 0; rel.abs_h = 0;
        rel.ref_w = 0;
    } else {
        /* Which monitor is it on? Enumeration is static because a fingerprint
         * set is a few KB and this runs on the UI thread only. */
        static MonitorFingerprint mons[WAC_MON_MAX];
        const int mon_count = monitor_enumerate(mons, WAC_MON_MAX);
        const int mi = monitor_at_point(mons, mon_count,
                                        (info.rect.left + info.rect.right) / 2,
                                        (info.rect.top + info.rect.bottom) / 2);

        RECT rc_mon;
        if (mi < 0 || !monitor_rect(mons, mon_count, mi, &rc_mon)) {
            MessageBoxW(owner, i18n_text(STR_MSG_ANCHOR_FAILED),
                        i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONWARNING);
            return;
        }

        if (!rel_from_pixels(&info.rect, &rc_mon, &rel)) {
            MessageBoxW(owner, i18n_text(STR_MSG_ANCHOR_FAILED),
                        i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONWARNING);
            return;
        }
        rel.min_w = 0;
        rel.min_h = 0;
    }

    AppId app;
    ZeroMemory(&app, sizeof(app));
    if (!app_id_from_window(&info, &app)) {
        MessageBoxW(owner, i18n_text(STR_MSG_ANCHOR_NOEXE),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONWARNING);
        return;
    }

    const int idx = rules_upsert(&g_app.rules, &app, &rel, NULL);
    if (idx < 0) {
        MessageBoxW(owner, i18n_text(STR_MSG_ANCHOR_FAILED),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONWARNING);
        return;
    }

    log_info(L"anchor: preset %d saved for hwnd=%p", idx, (void *)target);

    if (!rules_save(&g_app.rules)) {
        /* The rule IS in memory and will work for this session; say so plainly
         * rather than pretending nothing happened. */
        MessageBoxW(owner, i18n_text(STR_MSG_RULES_WRITE_FAILED),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONWARNING);
    }

    refresh_all();

    /* After the refresh: refresh_status() would overwrite anything set before
     * it, and this is the only feedback the user gets that it worked. */
    wchar_t name[RULE_LABEL_MAX];
    wchar_t msg[256];
    rules_display_name(rules_at(&g_app.rules, idx), name, RULE_LABEL_MAX);
    _snwprintf_s(msg, 256, _TRUNCATE, i18n_text(STR_MSG_ANCHORED), name);
    mainwindow_set_status(g_app.window, msg);

    strncpy_s(g_last_anchored_id, sizeof(g_last_anchored_id),
              rules_at(&g_app.rules, idx)->id, _TRUNCATE);
}

/* ------------------------------------------------------------------ */
/* Learning that a window cannot be resized (task B, 2026-10-10)       */
/* ------------------------------------------------------------------ */

/*
 * What the two explicit buttons learn while they sweep.
 *
 * A window that came back the SAME size although a resize was asked for cannot
 * be resized: win_place_apply() reports exactly that as PLACE_MOVED_ONLY, and
 * it is the only evidence this program ever gets (see win_place.h). The preset
 * that owns the window is marked here; the FILE is written once, afterwards, by
 * lock_sizes_finish().
 *
 * Why a callback instead of a second sweep: rule_apply.h exists so that
 * "match a window to a preset and place it" lives in one place. Why not save
 * per window: the config file is shared with the workspaces and every save
 * rewrites it, so three such windows would mean three writes for one click.
 */
typedef struct {
    int     count;                  /* presets newly marked during this sweep */
    int     measured;               /* presets whose measured size changed */
    wchar_t first[RULE_LABEL_MAX];  /* the first one, for the message */
} LockSizeNotes;

static void learn_lock_size(const WindowInfo *w, const Rule *r, const RECT *target,
                            PlaceResult res, const PlaceReport *rep, void *ctx)
{
    LockSizeNotes *notes = (LockSizeNotes *)ctx;

    (void)w; (void)target;   /* the decision needs neither */

    if (notes == NULL || r == NULL) return;

    /*
     * The size the window ACTUALLY ended up at (acceptance item 9).
     *
     * Recorded whenever the placement worked, whatever kind of success it was -
     * including PLACE_MOVED_ONLY, which is exactly the case where the measured
     * size differs from the requested one and is therefore the interesting one.
     * A refused or vanished window never had a size to measure.
     */
    if (rep != NULL && win_place_result_ok(res)) {
        const int px = (int)(rep->after.right - rep->after.left);
        const int py = (int)(rep->after.bottom - rep->after.top);
        if (rule_note_size(&g_app.rules, r->id, px, py)) {
            notes->measured++;
        }
    }

    if (res != PLACE_MOVED_ONLY) return;

    /* rule_lock_size() answers "did this call set it" - only the first time is
     * worth a message, and only the first time is there anything to save. */
    if (rule_lock_size(&g_app.rules, r->id)) {
        if (notes->count == 0) rules_display_name(r, notes->first, RULE_LABEL_MAX);
        notes->count++;
    }
}

/*
 * Write down what the sweep learned and put it into `msg` (may be empty).
 *
 * Called AFTER the sweep, so one user action means at most one write. The rule
 * module owns the [rule:*] sections, so this goes through rules_save() rather
 * than save_all() - save_all() owns meta/settings/workspaces and would not
 * touch a single lock_size.
 */
static void lock_sizes_finish(const LockSizeNotes *notes, wchar_t *msg, size_t cch)
{
    if (msg != NULL && cch > 0) msg[0] = L'\0';

    /* Nothing learned about a size lock AND nothing measured: no write. The
     * measured sizes ride along with this same save, so one user action is
     * still at most one write to the shared file. */
    if (notes == NULL || (notes->count <= 0 && notes->measured <= 0)) return;

    if (notes->measured > 0) {
        log_info(L"size: measured %d preset(s)", notes->measured);
    }

    if (notes->count <= 0) {
        /* Sizes only: written quietly, with no message to the user - they did
         * not ask a question that needs answering. */
        (void)rules_save(&g_app.rules);
        return;
    }

    if (rules_save(&g_app.rules)) {
        log_info(L"lock-size: %d preset(s) learned that their window cannot be resized",
                 notes->count);
    } else {
        /* Marked in memory, so this session behaves correctly - but say plainly
         * that the file did not take it, rather than reporting a success the
         * user will not see again after a restart. */
        log_info(L"lock-size: %d preset(s) marked in memory, writing the config FAILED",
                 notes->count);
    }

    if (msg != NULL && cch > 0) {
        _snwprintf_s(msg, cch, _TRUNCATE, i18n_text(STR_MSG_SIZE_LOCKED_FMT), notes->first);
    }
}

/*
 * [Apply every preset]: put every open window that matches a preset back.
 *
 * This is the button that makes anchoring useful. Anchoring only REMEMBERS
 * where a window was; until something applies the preset, nothing moves at all
 * - which is what "anchoring does not work" turned out to mean in testing.
 *
 * UIPI: a normal-privilege process cannot move a window owned by an elevated
 * one. Those land in `denied` and the status line says so, rather than the
 * whole operation reporting a silent success.
 */
static void cmd_apply_all(void)
{
    if (rules_count(&g_app.rules) <= 0) {
        MessageBoxW(mainwindow_hwnd(g_app.window),
                    i18n_text(STR_MSG_NO_PRESETS_YET),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        return;
    }

    LockSizeNotes notes;
    ZeroMemory(&notes, sizeof(notes));

    RuleApplySummary sum;
    const BOOL did = rule_apply_all(&g_app.rules, FALSE, learn_lock_size, &notes, &sum);

    log_info(L"apply: scanned=%d matched=%d placed=%d denied=%d failed=%d pinned_gone=%d",
             sum.scanned, sum.matched, sum.placed, sum.denied, sum.failed,
             sum.pinned_gone);

    /* After the sweep, never during it: one user action, at most one write. */
    wchar_t learned[192];
    lock_sizes_finish(&notes, learned, 192);

    /* Refresh first: refresh_status() would overwrite anything set before it,
     * and this summary is the only report the user gets. */
    refresh_all();

    wchar_t msg[320];
    if (!did || sum.matched == 0) {
        _snwprintf_s(msg, 320, _TRUNCATE, L"%ls", i18n_text(STR_MSG_APPLIED_NONE));
    } else {
        _snwprintf_s(msg, 320, _TRUNCATE, i18n_text(STR_MSG_APPLIED_FMT),
                     sum.placed, sum.denied, sum.failed);
    }
    if (learned[0] != L'\0') {
        /* Appended, not replaced: the counts are what the button promised, and
         * "this window cannot be resized" is news on top of them. */
        wcsncat_s(msg, 320, L"  ", _TRUNCATE);
        wcsncat_s(msg, 320, learned, _TRUNCATE);
    }
    if (sum.pinned_gone > 0) {
        /* Also appended: "3 placed" and "1 left alone because its screen is
         * not here" are both true and the user needs both. */
        wchar_t gone[160];
        _snwprintf_s(gone, 160, _TRUNCATE,
                     i18n_text(STR_MSG_PIN_GONE_FMT), sum.pinned_gone);
        wcsncat_s(msg, 320, L"  ", _TRUNCATE);
        wcsncat_s(msg, 320, gone, _TRUNCATE);
    }
    mainwindow_set_status(g_app.window, msg);
}

/*
 * [Move to the selected monitor] (2026-10-09, retargeted 2026-10-10).
 *
 * Takes every window that matches the SELECTED preset and places it on the
 * monitor the overview's monitor box has selected, using that preset's own
 * zone - the same arithmetic as `wac apply`, with the monitor forced instead
 * of derived from the window.
 *
 * ONE SHOT: nothing is stored. The preset keeps saying what it always said,
 * and the windows simply end up somewhere else until something moves them
 * again. That is the whole difference from [Apply every preset], which honours
 * each window's own monitor.
 *
 * The preset's windows are the ones rules_match() attributes to it - a window
 * that another preset claims first is left where it is (see RuleApplyOptions).
 *
 * Until 2026-10-10 the target was hard-coded to monitor_primary_index(): the
 * box above only changed the editor's preview, so "pick a screen, then move
 * there" did not exist at all (the user reported it as "the screen selection
 * does not take effect").
 */
static void cmd_move_to_selected_monitor(void)
{
    const char *id = mainwindow_selected_preset_id(g_app.window);

    if (id == NULL || id[0] == '\0') {
        MessageBoxW(mainwindow_hwnd(g_app.window),
                    i18n_text(STR_MSG_NO_PRESET_SEL),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        return;
    }

    const int target = mainwindow_selected_monitor_index(g_app.window);

    if (target < 0) {
        /* No usable selection: no monitors at all, an empty box, or a row the
         * current enumeration no longer covers. Reported rather than replaced
         * by the primary monitor - the button names the SELECTED monitor, so
         * quietly choosing a different one is the same "it did not take
         * effect" this change exists to fix. */
        log_info(L"move-to-monitor: no monitor selected (aborted)");
        mainwindow_set_status(g_app.window, i18n_text(STR_MSG_NO_MONITOR_SEL));
        return;
    }

    RuleApplyOptions opt;
    ZeroMemory(&opt, sizeof(opt));
    opt.only_id   = id;
    opt.monitor   = target;
    opt.move_only = FALSE;

    LockSizeNotes notes;
    ZeroMemory(&notes, sizeof(notes));

    RuleApplySummary sum;
    const BOOL did = rule_apply_ex(&g_app.rules, &opt, learn_lock_size, &notes, &sum);

    log_info(L"move-to-monitor: preset \"%hs\" -> monitor %d; "
             L"scanned=%d matched=%d placed=%d denied=%d failed=%d",
             id, target, sum.scanned, sum.matched, sum.placed,
             sum.denied, sum.failed);

    wchar_t learned[192];
    lock_sizes_finish(&notes, learned, 192);

    /* Refresh first: refresh_status() would overwrite anything set before it,
     * and this summary is the only report the user gets. */
    refresh_all();

    wchar_t msg[320];
    if (!did || sum.matched == 0) {
        _snwprintf_s(msg, 320, _TRUNCATE, L"%ls", i18n_text(STR_MSG_MOVE_NONE));
    } else {
        _snwprintf_s(msg, 320, _TRUNCATE, i18n_text(STR_MSG_MOVED_FMT),
                     sum.placed, sum.denied, sum.failed);
    }
    if (learned[0] != L'\0') {
        wcsncat_s(msg, 320, L"  ", _TRUNCATE);
        wcsncat_s(msg, 320, learned, _TRUNCATE);
    }
    mainwindow_set_status(g_app.window, msg);
}

/*
 * The two ways to create a preset. Both end in anchor_window(): they differ
 * only in where the target comes from - the overview's selected row, or the
 * preset page's target box.
 */
static void cmd_anchor_selected(void)
{
    const HWND target = (HWND)(ULONG_PTR)mainwindow_selected_window(g_app.window);
    if (target == NULL) {
        MessageBoxW(mainwindow_hwnd(g_app.window), i18n_text(STR_MSG_NO_WINDOW_SEL),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        return;
    }
    anchor_window(target);
}

/*
 * [Create preset] on the preset page: take a snapshot of the windows that are
 * open RIGHT NOW and show them on a picker page.
 *
 * A snapshot rather than a live list, because picking means "remember where
 * this window is now" - a list that reshuffles while the user reads it would
 * make that promise meaningless.
 */
/* ------------------------------------------------------------------ */
/* The installed-application list (stage 8, first slice)               */
/* ------------------------------------------------------------------ */

/*
 * Fill the picker from the index.
 *
 * Rows are the same shape the window picker uses, so the page, the list and
 * the [Create] button need no second implementation. What differs is which
 * field is set: `path` instead of `hwnd`, and a row without a path is shown
 * with a reason rather than hidden - the user may be looking for exactly that
 * program, and "it is here but I cannot start it" is an answer, while silence
 * is not.
 */
static void fill_create_page_from_index(void)
{
    if (g_app.appindex == NULL) return;

    static MwWindowRow rows[MW_CREATE_MAX];
    const int n = appindex_count(g_app.appindex);
    int count = 0;
    int skipped = 0;

    /*
     * Two passes: everything that can be launched first, then the rest.
     *
     * The list has a ceiling (MW_CREATE_MAX) and a real machine has more
     * entries than that, so WHICH ones survive the cut matters. Putting the
     * launchable ones first means the cut only ever falls on rows that could
     * not have been used anyway.
     */
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < n; ++i) {
            const AppIndexItem *it = appindex_at(g_app.appindex, i);
            if (it == NULL) continue;

            const BOOL launchable = (it->exe_path[0] != L'\0');
            if ((pass == 0) != launchable) continue;

            if (count >= MW_CREATE_MAX) { skipped++; continue; }

            MwWindowRow *row = &rows[count++];
            ZeroMemory(row, sizeof(*row));

            wcsncpy_s(row->app, MW_APP_MAX, it->name, _TRUNCATE);
            wcsncpy_s(row->title, MW_TEXT_MAX,
                      launchable ? it->exe_path : i18n_text(STR_APP_NO_PATH),
                      _TRUNCATE);
            wcsncpy_s(row->state, MW_TEXT_MAX,
                      launchable ? i18n_text(STR_APP_NOT_RUNNING)
                                 : i18n_text(STR_APP_UNLAUNCHABLE), _TRUNCATE);
            wcsncpy_s(row->path, MW_TEXT_MAX * 2, it->exe_path, _TRUNCATE);
        }
    }

    if (skipped > 0) {
        /* Said, not hidden: the user has to know the list is not everything,
         * or a program that IS installed looks missing. */
        log_info(L"appindex: %d entries do not fit the %d-row picker",
                 skipped, MW_CREATE_MAX);
    }

    log_info(L"appindex: showing %d of %d entries", count, n);
    mainwindow_show_create_page(g_app.window, rows, count);
}

/*
 * [More...] on the create page: choose a program that is not running.
 *
 * The index is built ONCE per process, on a worker thread, and only when this
 * is asked for - never at start-up (§8.6 acceptance). Until the worker posts
 * back, the page says so; after that the list is instant.
 */
static void cmd_browse_apps(void)
{
    if (g_app.appindex != NULL) {
        fill_create_page_from_index();
        return;
    }

    if (g_app.appjob == NULL) {
        g_app.appjob = appindex_job_start(g_app.msgwnd, WM_WAC_APPINDEX);
        if (g_app.appjob == NULL) {
            MessageBoxW(mainwindow_hwnd(g_app.window),
                        i18n_text(STR_MSG_INDEX_FAILED),
                        i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONWARNING);
            return;
        }
        log_info(L"appindex: indexing started (the cache decides how long)");
    }

    mainwindow_show_create_page_indexing(g_app.window);
}

/*
 * A preset for a program that is not running.
 *
 * There is no window to anchor to, so the zone is a STARTING POINT rather than
 * a measurement - left half, full height, the same default `wac place --zone
 * 0,0,0.5,1` uses. The editor opens right after this (the caller does that),
 * because the position is the one thing the user still has to decide.
 */
static void create_preset_from_path(const wchar_t *exe_path)
{
    AppId app;
    ZeroMemory(&app, sizeof(app));
    wcsncpy_s(app.exe_name, WAC_EXE_MAX, win_file_name(exe_path), _TRUNCATE);
    wcsncpy_s(app.exe_path, WAC_EXE_MAX, exe_path, _TRUNCATE);

    RelRect zone;
    ZeroMemory(&zone, sizeof(zone));
    zone.x = 0.0f; zone.y = 0.0f;
    zone.w = 0.5f; zone.h = 1.0f;
    zone.min_w = 1; zone.min_h = 1;

    const int idx = rules_upsert(&g_app.rules, &app, &zone, NULL);
    if (idx < 0) {
        MessageBoxW(mainwindow_hwnd(g_app.window), i18n_text(STR_MSG_ANCHOR_FAILED),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONWARNING);
        return;
    }

    log_info(L"create: preset for \"%ls\" (not running)", exe_path);

    if (!rules_save(&g_app.rules)) {
        MessageBoxW(mainwindow_hwnd(g_app.window),
                    i18n_text(STR_MSG_RULES_WRITE_FAILED),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONWARNING);
    }

    /* The same hand-off anchor_window() uses, so the caller's "open the
     * editor for the new preset" step works for both kinds of row. */
    const Rule *r = rules_at(&g_app.rules, idx);
    if (r != NULL) {
        strncpy_s(g_last_anchored_id, sizeof(g_last_anchored_id), r->id, _TRUNCATE);
    }
}

static void cmd_begin_create(void)
{
    static MwWindowRow rows[MW_ROW_MAX];
    static WindowInfo  wins[WAC_WIN_MAX];

    const int n = win_enum_top(wins, WAC_WIN_MAX, WIN_ENUM_DEFAULT);
    int count = 0;

    for (int i = 0; i < n && count < MW_ROW_MAX; ++i) {
        const WindowInfo *w = &wins[i];
        MwWindowRow *row = &rows[count++];
        ZeroMemory(row, sizeof(*row));

        wcsncpy_s(row->app, MW_APP_MAX, win_file_name(w->exe_path), _TRUNCATE);
        wcsncpy_s(row->title, MW_TEXT_MAX, w->title, _TRUNCATE);
        _snwprintf_s(row->size, 48, _TRUNCATE, L"%dx%d",
                     (int)(w->rect.right - w->rect.left),
                     (int)(w->rect.bottom - w->rect.top));
        row->hwnd = (unsigned long long)(ULONG_PTR)w->hwnd;

        const int mi = rules_match(&g_app.rules, w);
        if (mi >= 0) {
            const Rule *r = rules_at(&g_app.rules, mi);
            wchar_t name[MW_TEXT_MAX];
            rules_display_name(r, name, MW_TEXT_MAX);
            _snwprintf_s(row->state, MW_TEXT_MAX, _TRUNCATE,
                         i18n_text(STR_MATCH_OK), name);
        } else {
            wcsncpy_s(row->state, MW_TEXT_MAX, i18n_text(STR_MATCH_NONE), _TRUNCATE);
        }
    }

    log_info(L"create: snapshot of %d window(s)", count);
    mainwindow_show_create_page(g_app.window, rows, count);
}

static void cmd_create_preset(void)
{
    const HWND target = (HWND)(ULONG_PTR)mainwindow_create_pick(g_app.window);
    const wchar_t *path = mainwindow_create_pick_path(g_app.window);

    if (target == NULL && path == NULL) {
        MessageBoxW(mainwindow_hwnd(g_app.window), i18n_text(STR_MSG_CREATE_NO_WINDOW),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        return;
    }

    /* Leave the picker first: anchor_window() refreshes everything, and doing
     * that while a pushed page is still on top would lay out the wrong page. */
    mainwindow_close_create_page(g_app.window);

    /*
     * Two kinds of row, and the difference is which of the two fields is set:
     * a live window is anchored where it is; a program from the installed list
     * has no window yet, so it gets a preset with a starting zone.
     */
    if (target != NULL) {
        anchor_window(target);
    } else {
        create_preset_from_path(path);
    }

    /*
     * Go straight into the editor.
     *
     * Picking a window says "this program belongs somewhere"; where it happens
     * to be at that moment is rarely the answer, so the user lands in the
     * fields that decide it rather than on a list they now have to find the new
     * row in.
     */
    if (g_last_anchored_id[0] != '\0') {
        if (!mainwindow_editor_open_for(g_app.window, g_last_anchored_id)) {
            log_info(L"create: preset \"%hs\" was saved but not found for editing",
                     g_last_anchored_id);
        }
    }
}

/*
 * [Save] in the hand-editing panel: write the four typed percentages back to
 * the preset.
 *
 * The zone becomes a FRACTION rule - abs_w/abs_h are cleared - because that is
 * what typing a percentage means: "the left half", not "the size it happened to
 * be". Leaving a stale measured size in place would make the typed width
 * ignored on the next apply, which is the two-sources-of-truth bug the whole
 * design avoids.
 */
static void cmd_save_preset(void)
{
    const HWND  owner = mainwindow_hwnd(g_app.window);
    const char *id = mainwindow_editor_rule_id(g_app.window);
    if (id == NULL) return;

    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
    if (!mainwindow_editor_values(g_app.window, &x, &y, &w, &h)) {
        MessageBoxW(owner, i18n_text(STR_MSG_EDIT_BAD_INPUT),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONWARNING);
        return;
    }

    int idx = -1;
    for (int i = 0; i < rules_count(&g_app.rules); ++i) {
        const Rule *r = rules_at(&g_app.rules, i);
        if (r != NULL && _stricmp(r->id, id) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        MessageBoxW(owner, i18n_text(STR_MSG_PRESET_GONE),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        mainwindow_editor_close(g_app.window);
        return;
    }

    Rule *r = rules_at_mut(&g_app.rules, idx);
    if (r == NULL) return;

    r->zone.x = (float)x;
    r->zone.y = (float)y;
    r->zone.w = (float)w;
    r->zone.h = (float)h;

    /* Fraction semantics: no remembered pixel size, no reference width. */
    r->zone.abs_w = 0;
    r->zone.abs_h = 0;
    r->zone.ref_w = 0;

    /*
     * The screen pin (2026-10-10).
     *
     * The checkbox says WHETHER, the monitor box says WHICH - so saving an
     * unticked box clears the pin, which is how the user undoes it. The
     * fingerprint is recorded rather than an index: indices are enumeration
     * order, and that is not stable across a replug (§8.10).
     */
    if (mainwindow_editor_pin_wanted(g_app.window)) {
        MonitorFingerprint mons[WAC_MON_MAX];
        const int n = monitor_enumerate(mons, WAC_MON_MAX);
        const int mi = mainwindow_selected_monitor_index(g_app.window);

        if (mi >= 0 && mi < n) {
            rule_bind_monitor(r, &mons[mi]);
            log_info(L"preset: \"%hs\" pinned to %ls %dx%d at %d,%d", id,
                     (mons[mi].friendly_name[0] != L'\0')
                         ? mons[mi].friendly_name : mons[mi].device_name,
                     mons[mi].width, mons[mi].height,
                     mons[mi].virtual_x, mons[mi].virtual_y);
        } else {
            /* Ticked, but there is nothing to pin to. Said out loud rather
             * than stored as a pin that can never match. */
            MessageBoxW(owner, i18n_text(STR_PIN_NO_MONITOR),
                        i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
            rule_bind_monitor(r, NULL);
        }
    } else {
        if (rule_is_pinned(r)) log_info(L"preset: \"%hs\" unpinned", id);
        rule_bind_monitor(r, NULL);
    }

    log_info(L"preset: zone of \"%hs\" set to %.4f,%.4f %.4fx%.4f",
             id, x, y, w, h);

    if (!rules_save(&g_app.rules)) {
        MessageBoxW(owner, i18n_text(STR_MSG_RULES_WRITE_FAILED),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONWARNING);
    }

    mainwindow_editor_close(g_app.window);
    refresh_all();
    mainwindow_set_status(g_app.window, i18n_text(STR_MSG_EDIT_SAVED));
}

static void cmd_delete_selected_preset(void)
{
    const char *id = mainwindow_selected_preset_id(g_app.window);
    const HWND  owner = mainwindow_hwnd(g_app.window);

    if (id == NULL) {
        MessageBoxW(owner, i18n_text(STR_MSG_NO_PRESET_SEL),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        return;
    }

    /* Match on the id, not on the row: the list may have been rebuilt since
     * the selection was made. */
    int idx = -1;
    for (int i = 0; i < rules_count(&g_app.rules); ++i) {
        const Rule *r = rules_at(&g_app.rules, i);
        if (r != NULL && _stricmp(r->id, id) == 0) { idx = i; break; }
    }
    if (idx < 0) return;

    wchar_t name[RULE_LABEL_MAX];
    wchar_t msg[320];
    rules_display_name(rules_at(&g_app.rules, idx), name, RULE_LABEL_MAX);
    _snwprintf_s(msg, 320, _TRUNCATE, i18n_text(STR_MSG_CONFIRM_DEL_PRESET), name);

    if (MessageBoxW(owner, msg, i18n_text(STR_ERR_TITLE),
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    if (!rules_remove(&g_app.rules, idx)) return;

    log_info(L"preset: removed \"%hs\"", id);

    if (!rules_save(&g_app.rules)) {
        MessageBoxW(owner, i18n_text(STR_MSG_RULES_WRITE_FAILED),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONWARNING);
    }

    /* The selection now names something that no longer exists; the reselect
     * pass simply finds nothing and the lists come back with no selection. */
    refresh_all();
}

/* ------------------------------------------------------------------ */
/* Settings persistence                                                */
/* ------------------------------------------------------------------ */

static void load_settings(void)
{
    wchar_t path[MAX_PATH];
    if (!config_file(path, MAX_PATH)) return;

    StoreDocument *doc = NULL;
    const StoreLoadResult r = store_load(path, &doc);

    if (r == STORE_MISSING) {
        log_info(L"config: not found, starting from defaults");
        return;
    }
    if (r == STORE_CORRUPT_NO_RECOVERY) {
        log_info(L"config: damaged and no usable backup");
        wchar_t msg[512];
        _snwprintf_s(msg, 512, _TRUNCATE, L"%ls\n\n%ls",
                     i18n_text(STR_MSG_CONFIG_DAMAGED),
                     i18n_text(STR_MSG_CONFIG_DAMAGED2));
        MessageBoxW(NULL, msg, i18n_text(STR_ERR_TITLE),
                    MB_OK | MB_ICONWARNING);
        return;
    }
    if (r == STORE_RECOVERED_FROM_BACKUP) {
        log_info(L"config: recovered from a backup");
        /* Reported in the status bar once the window exists: it is good news,
         * not something to interrupt a boot for. */
        wcsncpy_s(g_startup_note, _countof(g_startup_note),
                  i18n_text(STR_MSG_CONFIG_RECOVERED), _TRUNCATE);
    }
    if (r == STORE_MODIFIED_EXTERNALLY) {
        /* The file parses and IS being used; it was just written by something
         * else. Worth a line because a user who edits the file by hand has no
         * other way to know the program picked the change up - and because
         * the opposite used to happen silently (see store.h). */
        log_info(L"config: written outside this program; contents accepted");
        wcsncpy_s(g_startup_note, _countof(g_startup_note),
                  i18n_text(STR_MSG_CONFIG_MODIFIED), _TRUNCATE);
    }

    if (doc == NULL) return;

    /* Language first: everything shown after this point, including the
     * warning dialogs above for a damaged file, uses it. The dialogs before
     * this line necessarily use the built-in default. */
    settings_from_store(&g_app.settings, doc);
    i18n_init(g_app.settings.language);

    ws_from_store(&g_app.workspaces, doc);

    store_destroy(doc);
}

static void save_all(void)
{
    wchar_t path[MAX_PATH];
    if (!config_file(path, MAX_PATH)) return;

    /*
     * appconfig_save() owns [meta], [settings] and [workspace:*], and it
     * loads the file before writing - so the [rule:*] sections, which belong
     * to the rule module, survive. See appconfig.h and model/store.h.
     *
     * This function runs unconditionally on exit, which is exactly why the
     * earlier "build a fresh document" version of it destroyed every window
     * preset on a normal close. Measured 2026-09-23.
     */
    if (appconfig_save(path, &g_app.settings, &g_app.workspaces)) {
        log_info(L"save: wrote %d workspace(s)", g_app.workspaces.count);
    } else {
        log_info(L"save: store_save failed");
    }
}

/* ------------------------------------------------------------------ */
/* Workspace commands                                                  */
/* ------------------------------------------------------------------ */

static void cmd_new_workspace(void)
{
    if (g_app.workspaces.count >= WS_MAX) {
        MessageBoxW(mainwindow_hwnd(g_app.window),
                    i18n_text(STR_MSG_LIMIT_REACHED),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        return;
    }

    /* Stage 2: create with a generated name. The rename dialog arrives with
     * the settings UI in stage 6. */
    wchar_t name[WS_NAME_MAX];
    _snwprintf_s(name, WS_NAME_MAX, _TRUNCATE, L"Workspace %d",
                 g_app.workspaces.count + 1);

    char narrow[WS_NAME_MAX];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), NULL, NULL);

    if (ws_add(&g_app.workspaces, NULL, narrow) == NULL) return;

    save_all();
    refresh_list();
    log_info(L"workspace: created \"%ls\"", name);
}

/*
 * [Rename workspace], the File menu's Rename, and F2 - all three end up here.
 *
 * The name is all there is to rename: the applications in it are managed by
 * the list right below the buttons, and the coordinates belong to the presets
 * it refers to. The ID is deliberately NOT regenerated - it is what the
 * [slot:<id>:<n>] sections are keyed on - so renaming costs nothing and
 * orphans nothing.
 *
 * This used to be a stub saying "renaming arrives with the settings dialog",
 * while the [Edit selected workspace] BUTTON did the real work. That is what
 * made the menu entry and F2 look broken: they promised a rename and produced
 * a message box. Now the button opens the full-screen editor (stage 6) and the
 * rename lives here, so every entry point does what its label says.
 */
static void cmd_rename_workspace(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        MessageBoxW(mainwindow_hwnd(g_app.window), i18n_text(STR_MSG_NO_WORKSPACE_SEL),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        return;
    }

    Workspace *w = ws_find(&g_app.workspaces, id);
    if (w == NULL) return;

    wchar_t current[WS_NAME_MAX];
    if (!str_to_wide(w->name, current, WS_NAME_MAX)) current[0] = L'\0';

    wchar_t name[WS_NAME_MAX];
    if (!rename_dlg_prompt(mainwindow_hwnd(g_app.window), current, name, WS_NAME_MAX)) {
        return;   /* cancelled: nothing is written back */
    }

    char narrow[WS_NAME_MAX];
    if (!wide_to_str(name, narrow, sizeof(narrow)) || narrow[0] == '\0') return;
    if (strcmp(narrow, w->name) == 0) return;   /* unchanged */

    if (!ws_rename(&g_app.workspaces, id, narrow)) return;

    save_all();
    refresh_all();

    log_info(L"workspace: \"%hs\" renamed to \"%hs\"", w->id, narrow);
}

static void cmd_delete_workspace(const char *id)
{
    if (id == NULL) return;

    const Workspace *w = ws_find_const(&g_app.workspaces, id);
    if (w == NULL) return;

    wchar_t msg[256];
    wchar_t wname[WS_NAME_MAX + 8];
    _snwprintf_s(wname, WS_NAME_MAX + 8, _TRUNCATE, L"%hs", w->name);
    _snwprintf_s(msg, 512, _TRUNCATE, L"%ls\n\n%ls",
                 i18n_text(STR_MSG_CONFIRM_DELETE),   /* %hs takes wname below */
                 i18n_text(STR_MSG_CONFIRM_DELETE2));
    /* The first line needs the name substituted, so rebuild it. */
    {
        wchar_t first[256];
        _snwprintf_s(first, 256, _TRUNCATE, i18n_text(STR_MSG_CONFIRM_DELETE), wname);
        _snwprintf_s(msg, 512, _TRUNCATE, L"%ls\n\n%ls",
                     first, i18n_text(STR_MSG_CONFIRM_DELETE2));
    }

    if (MessageBoxW(mainwindow_hwnd(g_app.window), msg, i18n_text(STR_ERR_TITLE),
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    if (g_app.active_id[0] && _stricmp(g_app.active_id, id) == 0) {
        g_app.active_id[0] = '\0';
    }

    if (ws_remove(&g_app.workspaces, id)) {
        save_all();
        refresh_list();
        log_info(L"workspace: deleted \"%hs\"", id);
    }
}

/* ------------------------------------------------------------------ */
/* Workspace commands (stage 5)                                        */
/* ------------------------------------------------------------------ */

/* workspace_run reports one line per step; the window has no room for that,
 * so it goes to the log and the status bar carries the summary. */
static void gui_run_trace(void *ctx, const wchar_t *line)
{
    (void)ctx;
    log_info(L"run: %ls", line);
}

/*
 * [Run] on the workspace page.
 *
 * The pids are recorded exactly as the command line does it, so `wac close`
 * and the [Close] button see the same thing - one record, two front ends.
 */
static void cmd_run_workspace(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        MessageBoxW(mainwindow_hwnd(g_app.window), i18n_text(STR_MSG_NO_WORKSPACE_SEL),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        return;
    }

    const Workspace *w = ws_find_const(&g_app.workspaces, id);
    if (w == NULL) return;

    if (w->slot_count == 0) {
        MessageBoxW(mainwindow_hwnd(g_app.window), i18n_text(STR_MSG_WS_EMPTY_RUN),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        return;
    }

    /* Starting programs and waiting for windows takes seconds. The buttons
     * would otherwise invite a second run on top of the first. */
    mainwindow_set_busy(g_app.window, TRUE);
    const WorkspaceRunReport rep = workspace_run(w, &g_app.rules, NULL,
                                                 gui_run_trace, NULL);
    mainwindow_set_busy(g_app.window, FALSE);

    {
        RunState st;
        runstate_init(&st, w->id);

        for (int i = 0; i < rep.count; ++i) {
            if (rep.slots[i].pid != 0) runstate_add(&st, rep.slots[i].pid, NULL);

            /* The window belongs to its own process for a packaged
             * application - record that one too, or [Close] ends something
             * invisible and leaves the window on screen. */
            if (rep.slots[i].hwnd != NULL) {
                DWORD owner = 0;
                GetWindowThreadProcessId(rep.slots[i].hwnd, &owner);
                if (owner != 0 && owner != rep.slots[i].pid) {
                    runstate_add(&st, owner, NULL);
                }
            }
        }

        if (st.count > 0) runstate_save(&st);
    }

    wchar_t line[256];
    _snwprintf_s(line, 256, _TRUNCATE, i18n_text(STR_MSG_RUN_DONE),
                 rep.ok, rep.denied, rep.failed, rep.skipped);
    mainwindow_set_status(g_app.window, line);

    log_info(L"run: workspace \"%hs\" -> %d placed, %d denied, %d failed, %d skipped",
             w->id, rep.ok, rep.denied, rep.failed, rep.skipped);

    refresh_all();
    mainwindow_set_status(g_app.window, line);   /* refresh_status() overwrites */
}

/*
 * [Close] on the workspace page: the destructive action, so the list of
 * programs comes before the question rather than after it.
 */
static void cmd_close_workspace(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        MessageBoxW(mainwindow_hwnd(g_app.window), i18n_text(STR_MSG_NO_WORKSPACE_SEL),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        return;
    }

    const Workspace *w = ws_find_const(&g_app.workspaces, id);
    if (w == NULL) return;

    RunState st;
    if (!runstate_load(id, &st) || st.count == 0) {
        MessageBoxW(mainwindow_hwnd(g_app.window),
                    i18n_text(STR_MSG_WS_NOTHING_TO_CLOSE),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        return;
    }

    wchar_t name[WS_NAME_MAX + 8];
    _snwprintf_s(name, WS_NAME_MAX + 8, _TRUNCATE, L"%hs", w->name);

    wchar_t msg[1024];
    _snwprintf_s(msg, 1024, _TRUNCATE,
                 i18n_text(STR_MSG_CONFIRM_CLOSE_WS), name);

    /* Name every process: "close the workspace" is not enough information to
     * decide with. */
    for (int i = 0; i < st.count && wcslen(msg) < 800; ++i) {
        wchar_t one[192];
        _snwprintf_s(one, 192, _TRUNCATE, L"\n  %ls (pid %lu)",
                     st.entries[i].exe, (unsigned long)st.entries[i].pid);
        wcsncat_s(msg, 1024, one, _TRUNCATE);
    }

    if (MessageBoxW(mainwindow_hwnd(g_app.window), msg, i18n_text(STR_ERR_TITLE),
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    int failed = 0, stale = 0;
    const int ended = runstate_terminate(&st, NULL, NULL, &failed, &stale);

    /* Forgetting is the safe direction: a stale record would make the next
     * close report processes that are gone, or worse, match a recycled pid. */
    runstate_forget(id);

    wchar_t line[256];
    _snwprintf_s(line, 256, _TRUNCATE,
                 L"%ls: ended %d, already gone %d, failed %d",
                 name, ended, stale, failed);
    mainwindow_set_status(g_app.window, line);

    log_info(L"close: workspace \"%hs\" -> ended %d, stale %d, failed %d",
             w->id, ended, stale, failed);

    refresh_all();
    mainwindow_set_status(g_app.window, line);
}

/*
 * [Add selected preset]: the workspace page's way of building a workspace.
 *
 * The preset comes from the OVERVIEW page's selection, because that is where
 * presets are browsed. Two clicks in two places, but each one where the thing
 * being chosen is on screen.
 */
/*
 * Add the preset selected on the overview page to the selected workspace.
 *
 * NO BUTTON SENDS THIS ANY MORE (since 2026-10-09). It lived on the workspace
 * page, but the preset it added was chosen on the OVERVIEW page - a selection
 * this page neither showed nor could change, so the user could not tell which
 * preset was about to be added, and could not compare the slot list before and
 * after (reported by the user). The capability is being moved into the
 * full-screen workspace editor, where the workspace is on screen while it is
 * edited; until then the command is reachable from nothing, and is kept
 * deliberately rather than deleted.
 */
static void cmd_add_slot(const char *ws_id)
{
    if (ws_id == NULL || ws_id[0] == '\0') {
        MessageBoxW(mainwindow_hwnd(g_app.window), i18n_text(STR_MSG_NO_WORKSPACE_SEL),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        return;
    }

    Workspace *w = ws_find(&g_app.workspaces, ws_id);
    if (w == NULL) return;

    const char *preset_id = mainwindow_selected_preset_id(g_app.window);
    if (preset_id == NULL || preset_id[0] == '\0') {
        MessageBoxW(mainwindow_hwnd(g_app.window),
                    rules_count(&g_app.rules) == 0 ? i18n_text(STR_MSG_WS_NO_SLOTS_ADD)
                                                   : i18n_text(STR_MSG_NO_SLOT_PRESET),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        return;
    }

    for (int i = 0; i < w->slot_count; ++i) {
        if (_stricmp(w->slots[i].preset_id, preset_id) == 0) {
            MessageBoxW(mainwindow_hwnd(g_app.window), i18n_text(STR_MSG_SLOT_EXISTS),
                        i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
            return;
        }
    }

    WorkspaceSlot *s = ws_slot_add(w);
    if (s == NULL) {
        /* At WSS_SLOT_MAX. Reported rather than silently ignored. */
        MessageBoxW(mainwindow_hwnd(g_app.window), i18n_text(STR_MSG_LIMIT_REACHED),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        return;
    }

    strncpy_s(s->preset_id, sizeof(s->preset_id), preset_id, _TRUNCATE);

    /* A slot that is not the first one runs after the ones before it, so the
     * order the user built is the order they get. */
    s->order = w->slot_count;

    /*
     * Stage 7: remember which monitor the user was looking at.
     *
     * The main window is where they clicked, so the monitor it sits on is the
     * one this slot belongs to. The window's CENTRE decides, not a corner: a
     * window straddling two screens belongs to the one showing more of it.
     *
     * If the rectangle cannot be read, the primary monitor is the fallback -
     * the same answer callers get from monitor_primary_index() elsewhere. A
     * slot that ends up unbound is not a failure: it applies wherever its
     * window turns up, exactly as it did before stage 7.
     */
    {
        MonitorFingerprint mons[WAC_MON_MAX];
        const int n = monitor_enumerate(mons, WAC_MON_MAX);
        if (n > 0) {
            int idx = -1;
            RECT rc;
            if (GetWindowRect(mainwindow_hwnd(g_app.window), &rc)) {
                idx = monitor_at_point(mons, n, (rc.left + rc.right) / 2,
                                                   (rc.top + rc.bottom) / 2);
            }
            if (idx < 0) idx = monitor_primary_index(mons, n);
            if (idx >= 0) ws_slot_bind_monitor(s, &mons[idx]);
        }
    }

    save_all();
    refresh_all();

    log_info(L"slot: \"%hs\" added preset \"%hs\" (%d slot(s))",
             w->id, preset_id, w->slot_count);

    mainwindow_set_status(g_app.window, i18n_text(STR_MSG_SLOT_ADDED));
}

/*
 * [Remove selected preset] on the workspace page.
 *
 * Addresses the slot by its POSITION, which is what the list shows and what
 * the user clicked. ws_slot_remove shifts the rest down, and ws_to_store
 * rewrites every [slot:<id>:<n>] section afterwards, so the stored numbering
 * follows the list rather than leaving a hole behind.
 */
static void cmd_remove_slot(const char *ws_id)   /* no button since 2026-10-09;
                                                  * see cmd_add_slot() above */
{
    if (ws_id == NULL || ws_id[0] == '\0') {
        MessageBoxW(mainwindow_hwnd(g_app.window), i18n_text(STR_MSG_NO_WORKSPACE_SEL),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        return;
    }

    Workspace *w = ws_find(&g_app.workspaces, ws_id);
    if (w == NULL) return;

    const int idx = mainwindow_selected_slot(g_app.window);
    if (idx < 0 || idx >= w->slot_count) {
        MessageBoxW(mainwindow_hwnd(g_app.window), i18n_text(STR_MSG_NO_SLOT_SEL),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (!ws_slot_remove(w, idx)) return;

    save_all();
    refresh_all();

    log_info(L"slot: \"%hs\" now has %d slot(s)", w->id, w->slot_count);
    mainwindow_set_status(g_app.window, i18n_text(STR_MSG_SLOT_REMOVED));
}

/*
 * [Unbind monitor] on the workspace page (stage 7).
 *
 * Drops the recorded monitor from the selected slot. The slot then behaves
 * exactly as every slot did before stage 7: it is applied wherever its window
 * turns up. This is the escape hatch for "I have moved this workspace to the
 * other screen" - without it, the only way out would be to remove the slot
 * and add it again, which would also lose its position in the run order.
 */
static void cmd_unbind_slot(const char *ws_id)
{
    if (ws_id == NULL || ws_id[0] == '\0') {
        MessageBoxW(mainwindow_hwnd(g_app.window), i18n_text(STR_MSG_NO_WORKSPACE_SEL),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        return;
    }

    Workspace *w = ws_find(&g_app.workspaces, ws_id);
    if (w == NULL) return;

    const int idx = mainwindow_selected_slot(g_app.window);
    if (idx < 0 || idx >= w->slot_count) {
        MessageBoxW(mainwindow_hwnd(g_app.window), i18n_text(STR_MSG_NO_SLOT_SEL),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        return;
    }

    WorkspaceSlot *s = &w->slots[idx];

    /*
     * The button is disabled when the selected slot is unbound, so getting
     * here in that state means the selection moved between the click and this
     * handler. Say nothing was done rather than reporting a success.
     */
    if (!s->mon_set) {
        mainwindow_set_status(g_app.window, i18n_text(STR_MSG_SLOT_UNBOUND));
        return;
    }

    /* Clear every level, not just mon_set: a later bind must not be able to
     * pick up a stale fingerprint from the slot it replaced. */
    s->mon_set = FALSE;
    s->mon_device[0]   = L'\0';
    s->mon_friendly[0] = L'\0';
    s->mon_w = s->mon_h = 0;
    s->mon_x = s->mon_y = 0;

    save_all();
    refresh_all();

    log_info(L"slot: \"%hs\" slot %d unbound from its monitor", w->id, idx);
    mainwindow_set_status(g_app.window, i18n_text(STR_MSG_SLOT_UNBOUND));
}

/*
 * [Edit selected workspace] - enter the full-screen edit mode.
 *
 * The user's design (2026-09-22): editing a WORKSPACE is based on the
 * full-screen overlay, so this button goes straight in rather than opening
 * anything else first. Editing a single WINDOW preset is a different thing -
 * it has a form that does not need the overlay - which is why the preset page
 * keeps its own editor.
 *
 * The overlay covers one monitor: the one the main window is on. That is the
 * least surprising choice, since the user is looking at the window they
 * clicked.
 */
/*
 * The overlay reports a finished rename; this is where it becomes a workspace
 * rename.
 *
 * The overlay knows nothing about workspaces - which is what will let the same
 * name box serve a window preset later - so what is being named is tracked
 * here, in the one place that owns the workspace set.
 */
static void on_overlay_rename(const wchar_t *new_name, void *ctx)
{
    char narrow[WS_NAME_MAX];

    (void)ctx;

    if (g_app.edit_workspace_id[0] == '\0') return;
    if (!wide_to_str(new_name, narrow, sizeof(narrow)) || narrow[0] == '\0') {
        return;
    }

    if (!ws_rename(&g_app.workspaces, g_app.edit_workspace_id, narrow)) {
        log_info(L"edit: rename of \"%hs\" was refused", g_app.edit_workspace_id);
        return;
    }

    /*
     * Written at once. The user asked for this change, and the session may end
     * before any other save happens - the fuse, or a close while the overlay is
     * up - so leaving it in memory would be losing it.
     */
    save_all();
    refresh_all();

    log_info(L"edit: workspace \"%hs\" renamed to \"%hs\"",
             g_app.edit_workspace_id, narrow);
}

/* ------------------------------------------------------------------ */
/* The edit sidebar's list (stage 8, task 2b)                          */
/* ------------------------------------------------------------------ */

/* Defined with the placeholder helpers further down, but called from the drop
 * handler and from cmd_edit_overlay() above it. */
static void edit_push_slots(void);

/*
 * The sidebar BORROWS the index rather than copying it: these two are called
 * only for the rows about to be drawn - forty-odd of them per frame - so a
 * list of a thousand costs what a list of forty costs, and a list that
 * finishes building while the panel is open needs no rebuild.
 */
static const wchar_t *sidebar_name_at(void *ctx, int index)
{
    (void)ctx;
    if (g_app.appindex == NULL) return L"";

    const AppIndexItem *it = appindex_at(g_app.appindex, index);
    return (it != NULL) ? it->name : L"";
}

static const wchar_t *sidebar_path_at(void *ctx, int index)
{
    (void)ctx;
    if (g_app.appindex == NULL) return L"";

    const AppIndexItem *it = appindex_at(g_app.appindex, index);
    return (it != NULL) ? it->exe_path : L"";
}

/*
 * An application dropped from the sidebar onto the canvas.
 *
 * UPSERT, not add: a program that already has a preset gets MOVED, because two
 * presets for one executable is a state the file can express and the user can
 * never resolve. The zone arrives normalized - the overlay does the geometry,
 * this decides what it means.
 */
static void on_overlay_drop(const wchar_t *exe_path, const RelRect *zone, void *ctx)
{
    (void)ctx;

    if (exe_path == NULL || zone == NULL) return;

    /*
     * The drop decides WHERE. The SIZE comes from the window itself when that
     * program is running: dragging an application onto the canvas should not
     * force it to half the screen, which is what the fixed drop rectangle did.
     */
    RelRect drop = *zone;
    {
        static WindowInfo wins[WAC_WIN_MAX];
        const int n = win_enum_top(wins, WAC_WIN_MAX,
                                   WIN_ENUM_VISIBLE_ONLY | WIN_ENUM_SKIP_TOOLWIN |
                                   WIN_ENUM_NEED_TITLE);
        const wchar_t *want = win_file_name(exe_path);

        for (int i = 0; i < n; ++i) {
            wchar_t path[MAX_PATH];
            if (!win_exe_path(wins[i].pid, path, MAX_PATH)) continue;
            if (_wcsicmp(win_file_name(path), want) != 0) continue;

            static MonitorFingerprint mons[WAC_MON_MAX];
            const int mc = monitor_enumerate(mons, WAC_MON_MAX);

            const int cx = (wins[i].rect.left + wins[i].rect.right) / 2;
            const int cy = (wins[i].rect.top + wins[i].rect.bottom) / 2;
            const int mi = (mc > 0) ? monitor_at_point(mons, mc, cx, cy) : -1;
            if (mi < 0) break;

            RECT mon;
            mon.left   = mons[mi].virtual_x;
            mon.top    = mons[mi].virtual_y;
            mon.right  = mon.left + mons[mi].width;
            mon.bottom = mon.top + mons[mi].height;

            RelRect rel;
            if (rel_from_pixels(&wins[i].rect, &mon, &rel)) {
                drop.w = rel.w;
                drop.h = rel.h;
                if (drop.x + drop.w > 1.0f) drop.x = 1.0f - drop.w;
                if (drop.y + drop.h > 1.0f) drop.y = 1.0f - drop.h;
                if (drop.x < 0.0f) drop.x = 0.0f;
                if (drop.y < 0.0f) drop.y = 0.0f;

                log_info(L"sidebar: \"%ls\" is running - dropping at its own size %.0fx%.0f",
                         want, rel.w * 100.0f, rel.h * 100.0f);
            }
            break;
        }
    }

    AppId app;
    ZeroMemory(&app, sizeof(app));
    wcsncpy_s(app.exe_name, WAC_EXE_MAX, win_file_name(exe_path), _TRUNCATE);
    wcsncpy_s(app.exe_path, WAC_EXE_MAX, exe_path, _TRUNCATE);

    const int idx = rules_upsert(&g_app.rules, &app, &drop, NULL);
    if (idx < 0) {
        log_info(L"sidebar: the preset for \"%ls\" could not be saved", exe_path);
        return;
    }

    /*
     * No modal box anywhere in here, even when the write fails.
     *
     * A MessageBox raised now would appear UNDER the overlay, which is
     * topmost: a dialog the user can neither see nor dismiss, over a
     * full-screen window they are in the middle of using. The status bar
     * carries the answer instead, and it is still there when edit mode ends.
     */
    if (!rules_save(&g_app.rules)) {
        log_info(L"sidebar: the settings file could not be written");
        mainwindow_set_status(g_app.window, i18n_text(STR_MSG_RULES_WRITE_FAILED));
        return;
    }

    wchar_t name[RULE_LABEL_MAX];
    name[0] = L'\0';
    const Rule *r = rules_at(&g_app.rules, idx);
    if (r != NULL) rules_display_name(r, name, RULE_LABEL_MAX);

    wchar_t msg[256];
    _snwprintf_s(msg, 256, _TRUNCATE, i18n_text(STR_MSG_ANCHORED), name);
    mainwindow_set_status(g_app.window, msg);

    log_info(L"sidebar: preset \"%ls\" placed by dropping it on the canvas", name);

    /*
     * ...and into the workspace being edited. Dragging a program onto the
     * canvas IS the act of adding it to this workspace's layout, so making the
     * user go to the overview page and add it a second time would be busywork -
     * and until this line existed, the drop created a preset the canvas never
     * showed.
     */
    {
        const char *ws_id = mainwindow_selected_id(g_app.window);
        Workspace *w = (ws_id != NULL) ? ws_find(&g_app.workspaces, ws_id) : NULL;

        if (w != NULL) {
            WorkspaceSlot *slot = ws_slot_add(w);
            if (slot != NULL) {
                strncpy_s(slot->preset_id, sizeof(slot->preset_id), r->id, _TRUNCATE);
                save_all();
                log_info(L"sidebar: \"%hs\" added to workspace \"%hs\"",
                         r->id, ws_id);
            } else {
                /* The workspace is full. Said, not silently dropped. */
                log_info(L"sidebar: workspace \"%hs\" is full - preset created but not added",
                         ws_id);
            }
        }
    }

    /* Every preset list on the main window is out of date the moment this
     * returns, and the user will be looking at them again very shortly. */
    refresh_all();

    /* ...and so are the boxes on the canvas, if an edit session is up: the
     * preset that just moved - or arrived - is one of them. */
    edit_push_slots();
}

/*
 * Hand the running session the list as it stands.
 *
 * Called when the session starts - the list may still be indexing, which the
 * panel says rather than showing an empty one - and again when the worker
 * posts back. That second call is the whole reason edit_set_sidebar() is not
 * part of edit_enter().
 */
static void edit_push_sidebar(void)
{
    if (!edit_is_active()) return;

    OverlaySidebarSource src;
    ZeroMemory(&src, sizeof(src));
    src.ctx      = NULL;
    src.count    = (g_app.appindex != NULL) ? appindex_count(g_app.appindex) : 0;
    src.indexing = (g_app.appindex == NULL && g_app.appjob != NULL) ? TRUE : FALSE;
    src.name_at  = sidebar_name_at;
    src.path_at  = sidebar_path_at;

    edit_set_sidebar(&src, on_overlay_drop, NULL);
}

/* ------------------------------------------------------------------ */
/* The edit canvas's placeholders (stage 8, acceptance items 8 and 9)  */
/* ------------------------------------------------------------------ */

/*
 * One box per preset the SELECTED workspace is meant to place.
 *
 * A static buffer, not a stack one: MW_SLOT_MAX of these is tens of kilobytes
 * (trap 11), and the overlay borrows the array through a callback for as long
 * as it takes to draw - so it has to outlive this call.
 */
static OverlaySlot g_slot_view[MW_SLOT_MAX];
static int         g_slot_view_count;

/*
 * Resolve the selected workspace's slots into something the overlay can draw.
 *
 * The same resolution refresh_slots() does - slot id -> preset -> rule - and it
 * deliberately SKIPS what it cannot resolve rather than drawing a box with no
 * name: a slot whose preset has been deleted has nothing to place, and a box
 * saying so would be a box the user cannot act on.
 */
static void build_slot_view(void)
{
    g_slot_view_count = 0;

    const char *id = mainwindow_selected_id(g_app.window);
    const Workspace *w = (id != NULL) ? ws_find_const(&g_app.workspaces, id) : NULL;
    if (w == NULL) {
        /* Worth saying out loud: "the canvas has no boxes on it" is otherwise
         * indistinguishable from "the boxes are wrong". */
        log_info(L"edit: no workspace selected - the canvas has no boxes");
        return;
    }

    for (int i = 0; i < w->slot_count && g_slot_view_count < MW_SLOT_MAX; ++i) {
        const WorkspaceSlot *s = &w->slots[i];
        if (s->preset_id[0] == '\0') continue;

        wchar_t wide_id[WSS_PRESET_ID_MAX];
        if (!str_to_wide(s->preset_id, wide_id, WSS_PRESET_ID_MAX)) continue;

        const int idx = rules_find_by_name(&g_app.rules, wide_id);
        if (idx < 0) continue;
        const Rule *r = rules_at(&g_app.rules, idx);
        if (r == NULL) continue;

        OverlaySlot *v = &g_slot_view[g_slot_view_count++];
        ZeroMemory(v, sizeof(*v));
        strncpy_s(v->rule_id, sizeof(v->rule_id), r->id, _TRUNCATE);
        wcsncpy_s(v->name, 128,
                  (r->label[0] != L'\0') ? r->label : r->app.exe_name, _TRUNCATE);
        wcsncpy_s(v->path, 260, r->app.exe_path, _TRUNCATE);
        v->zone   = r->zone;
        v->last_w = r->last_w;
        v->last_h = r->last_h;
    }

    log_info(L"edit: %d box(es) for workspace \"%hs\" (%d slot(s))",
             g_slot_view_count, id, w->slot_count);
}

static const OverlaySlot *slot_view_at(void *ctx, int index)
{
    (void)ctx;
    if (index < 0 || index >= g_slot_view_count) return NULL;
    return &g_slot_view[index];
}

/*
 * The user dragged a box on the canvas.
 *
 * Recorded in the DRAFT only - g_slot_view, not the rule set. Nothing is
 * written to the config file until [Apply] is pressed, which is what makes
 * [Apply] mean something and what makes leaving edit mode without pressing it
 * a discard.
 */
static void on_slot_moved(void *ctx, int index, const RelRect *zone)
{
    (void)ctx;

    if (index < 0 || index >= g_slot_view_count || zone == NULL) return;

    g_slot_view[index].zone = *zone;
    log_info(L"edit: box %d for \"%hs\" now %.4f,%.4f %.4fx%.4f", index,
             g_slot_view[index].rule_id, zone->x, zone->y, zone->w, zone->h);
}

/*
 * The user pressed Delete on a selected box.
 *
 * Removing the box means taking the preset out of THIS workspace, not deleting
 * the preset: another workspace may use it, and "I changed my mind about this
 * layout" is not "forget this program's position forever". Deleting the preset
 * stays on the preset page, where the action can say what it does.
 *
 * The view index is not the slot index - build_slot_view() skips slots it
 * cannot resolve - so the match is by preset id.
 */
static void on_slot_deleted(void *ctx, int index)
{
    (void)ctx;

    if (index < 0 || index >= g_slot_view_count) return;

    const char *ws_id = mainwindow_selected_id(g_app.window);
    Workspace *w = (ws_id != NULL) ? ws_find(&g_app.workspaces, ws_id) : NULL;
    if (w == NULL) return;

    const char *rule_id = g_slot_view[index].rule_id;

    for (int i = 0; i < w->slot_count; ++i) {
        if (_stricmp(w->slots[i].preset_id, rule_id) != 0) continue;

        if (!ws_slot_remove(w, i)) {
            log_info(L"edit-delete: \"%hs\" could not be removed from \"%hs\"",
                     rule_id, ws_id);
            return;
        }

        save_all();
        log_info(L"edit-delete: \"%hs\" removed from workspace \"%hs\"",
                 rule_id, ws_id);

        edit_push_slots();   /* the canvas no longer draws it */

        wchar_t msg[256];
        _snwprintf_s(msg, 256, _TRUNCATE, i18n_text(STR_MSG_EDIT_REMOVED_FMT),
                     g_slot_view[index].name);
        mainwindow_set_status(g_app.window, msg);
        return;
    }
}

/*
 * [Apply]: write every box back to the preset it belongs to.
 *
 * Only x/y/w/h move. abs_w / abs_h and the monitor binding are left exactly as
 * they were, because those decide what the preset MEANS (section 5.2) - an
 * edit on the canvas changes where a window goes, not which kind of rule it is.
 */
static void on_slots_apply(void *ctx)
{
    (void)ctx;

    const char *changed_ids[MW_SLOT_MAX];
    int changed = 0;

    for (int i = 0; i < g_slot_view_count; ++i) {
        const OverlaySlot *v = &g_slot_view[i];

        for (int r = 0; r < g_app.rules.count; ++r) {
            Rule *rule = &g_app.rules.items[r];
            if (strcmp(rule->id, v->rule_id) != 0) continue;

            if (rule->zone.x != v->zone.x || rule->zone.y != v->zone.y ||
                rule->zone.w != v->zone.w || rule->zone.h != v->zone.h) {
                rule->zone.x = v->zone.x;
                rule->zone.y = v->zone.y;
                rule->zone.w = v->zone.w;
                rule->zone.h = v->zone.h;
                if (changed < MW_SLOT_MAX) changed_ids[changed] = rule->id;
                changed++;
            }
            break;
        }
    }

    if (changed == 0) {
        log_info(L"edit-apply: nothing changed on the canvas");
        mainwindow_set_status(g_app.window, i18n_text(STR_MSG_EDIT_APPLIED_NONE));
        return;
    }

    /*
     * No modal box: the overlay is topmost, so one would appear behind it - the
     * same rule the drop handler follows. The status bar carries the answer.
     */
    if (!rules_save(&g_app.rules)) {
        log_info(L"edit-apply: %d preset(s) changed, writing the config FAILED", changed);
        mainwindow_set_status(g_app.window, i18n_text(STR_MSG_RULES_WRITE_FAILED));
        return;
    }

    log_info(L"edit-apply: %d preset(s) updated from the canvas", changed);

    /*
     * Put the real windows where the edited boxes say.
     *
     * The overlay covers them, so the move is seen when edit mode ends - which
     * is also when the user looks. Only the presets that changed are swept, so
     * a box the user did not touch leaves its window alone.
     */
    int placed = 0;
    for (int i = 0; i < changed && i < MW_SLOT_MAX; ++i) {
        RuleApplyOptions opt;
        ZeroMemory(&opt, sizeof(opt));
        opt.only_id   = changed_ids[i];
        opt.monitor   = -1;
        opt.move_only = FALSE;

        RuleApplySummary sum;
        ZeroMemory(&sum, sizeof(sum));
        rule_apply_ex(&g_app.rules, &opt, NULL, NULL, &sum);
        placed += sum.placed;
    }
    log_info(L"edit-apply: %d window(s) placed", placed);

    wchar_t msg[128];
    _snwprintf_s(msg, 128, _TRUNCATE, i18n_text(STR_MSG_EDIT_APPLIED_FMT), changed);
    mainwindow_set_status(g_app.window, msg);

    refresh_all();
}

/*
 * [Reset]: forget every draft change and load the boxes again.
 *
 * The draft lives in g_slot_view and nothing else was ever written, so
 * reloading it IS the reset - there is no undo stack to unwind, and no way for
 * this to disagree with what the session started with.
 */
static void on_slots_reset(void *ctx)
{
    (void)ctx;

    log_info(L"edit-reset: reloading the boxes, discarding draft changes");
    edit_push_slots();
    mainwindow_set_status(g_app.window, i18n_text(STR_MSG_EDIT_RESET));
}

/*
 * [Pick up this layout]: read the screen into the preset store.
 *
 * The opposite direction from [Apply] - apply imposes the stored layout on the
 * screen, pick reads the screen into the store - which is why it earns a button
 * of its own rather than being a mode of the other one.
 *
 * It writes immediately, unlike everything else in this draft model. It has to:
 * the material it works from is the screen, and the screen will not wait for a
 * second button press.
 *
 * What it does NOT do is touch the current workspace's slots. It adds and
 * updates PRESETS; the canvas is then rebuilt from that same store, so anything
 * picked up that this workspace already references simply appears on it.
 */
static void on_pick_layout(void *ctx)
{
    (void)ctx;

    PickupSummary sum;
    ZeroMemory(&sum, sizeof(sum));

    if (!pickup_layout(&g_app.rules, &sum) || sum.picked <= 0) {
        log_info(L"edit-pick: nothing picked up (%d scanned, %d skipped)",
                 sum.scanned, sum.skipped);
        mainwindow_set_status(g_app.window, i18n_text(STR_MSG_EDIT_PICK_NONE));
        return;
    }

    if (!rules_save(&g_app.rules)) {
        /* Saving is how the rest of the program sees these at all - an
         * in-memory set alone would be overwritten by the next save_all(). */
        log_info(L"edit-pick: %d picked up, writing the config FAILED", sum.picked);
        mainwindow_set_status(g_app.window, i18n_text(STR_MSG_EDIT_PICK_FAILED));
        return;
    }

    log_info(L"edit-pick: %d preset(s) picked up from the screen (%d maximized)",
             sum.picked, sum.maximized);

    wchar_t msg[128];
    _snwprintf_s(msg, 128, _TRUNCATE, i18n_text(STR_MSG_EDIT_PICKED_FMT),
                 sum.picked);
    mainwindow_set_status(g_app.window, msg);

    edit_push_slots();      /* the canvas now shows what was picked up */
}

/* Hand the running session the boxes as they stand. */
static void edit_push_slots(void)
{
    if (!edit_is_active()) return;

    build_slot_view();

    OverlaySlotSource src;
    ZeroMemory(&src, sizeof(src));
    src.ctx        = NULL;
    src.count      = g_slot_view_count;
    src.slot_at    = slot_view_at;
    src.slot_moved = on_slot_moved;
    src.apply      = on_slots_apply;
    src.reset      = on_slots_reset;
    src.pick       = on_pick_layout;
    src.deleted    = on_slot_deleted;

    edit_set_slots(&src);
}

static void cmd_edit_overlay(const char *id)
{
    wchar_t wname[WS_NAME_MAX];
    const Workspace *w;

    if (edit_is_active()) return;   /* a session is already running */

    /*
     * Remember what is being named BEFORE the session starts: the callback
     * above runs while the session is up and has nothing else to go on.
     */
    g_app.edit_workspace_id[0] = '\0';
    if (id != NULL && id[0] != '\0') {
        strncpy_s(g_app.edit_workspace_id, sizeof(g_app.edit_workspace_id),
                  id, _TRUNCATE);
    }

    static MonitorFingerprint mons[WAC_MON_MAX];
    const int count = monitor_enumerate(mons, WAC_MON_MAX);

    int mi = 0;
    RECT rc;
    if (count > 0 && GetWindowRect(mainwindow_hwnd(g_app.window), &rc)) {
        const int picked = monitor_at_point(mons, count,
                                            (rc.left + rc.right) / 2,
                                            (rc.top + rc.bottom) / 2);
        if (picked >= 0) mi = picked;
    }

    if (!edit_enter(g_app.hinst, mi, 0)) {
        MessageBoxW(mainwindow_hwnd(g_app.window),
                    i18n_text(STR_MSG_EDIT_ENTER_FAILED),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONWARNING);
        return;
    }

    /*
     * What the box shows. The session is handed the NAME only: it is this
     * function, not the overlay, that knows the name belongs to a workspace.
     */
    wname[0] = L'\0';
    w = (id != NULL) ? ws_find_const(&g_app.workspaces, id) : NULL;
    if (w != NULL && !str_to_wide(w->name, wname, WS_NAME_MAX)) {
        wname[0] = L'\0';
    }
    edit_set_name(wname, on_overlay_rename, NULL);

    /*
     * The sidebar's list.
     *
     * Building the index is the one thing in this program that deliberately
     * waits to be asked for - "no scan at start-up" is an acceptance item
     * (§8.6) - and entering edit mode is the asking. If the list is already
     * built this is free, and if the worker is already running the panel says
     * "indexing..." until it posts back.
     */
    if (g_app.appindex == NULL && g_app.appjob == NULL) {
        g_app.appjob = appindex_job_start(g_app.msgwnd, WM_WAC_APPINDEX);
        if (g_app.appjob == NULL) {
            log_info(L"appindex: the worker could not be started for the sidebar");
        }
    }
    edit_push_sidebar();
    edit_push_slots();
    edit_set_taskbar_guide(g_app.settings.taskbar_guide);

    mode_transition(MODE_EDIT_OVERLAY);
    if (!watchdog_start(edit_overlay_hwnd())) {
        log_info(L"watchdog: could not be started - the session has no guard");
    }
    mainwindow_set_status(g_app.window, i18n_text(STR_MSG_EDIT_ENTERED));
    log_info(L"edit: entered on monitor %d, naming \"%ls\"", mi, wname);
}

/*
 * Tell the user how the edit session ended.
 *
 * The overlay disappears the instant the session ends, so without this the
 * user has no idea what they pressed - which is exactly the complaint that
 * produced this function (the t05 hotkey investigation:
 * "I do not know what I hit"). The exit paths have different names, so the
 * message can say which one it was.
 */
static void report_edit_exit(OverlayExit why)
{
    /* Shutting down is not something to report: the window is going away. */
    if (why == OVERLAY_EXIT_NONE || why == OVERLAY_EXIT_SHUTDOWN) return;

    const wchar_t *how;
    switch (why) {
    case OVERLAY_EXIT_HOTKEY:  how = i18n_text(STR_EDIT_EXIT_HOTKEY);  break;
    case OVERLAY_EXIT_ESCAPE:  how = i18n_text(STR_EDIT_EXIT_ESCAPE);  break;
    case OVERLAY_EXIT_TOOLBAR: how = i18n_text(STR_EDIT_EXIT_TOOLBAR); break;
    case OVERLAY_EXIT_FUSE:    how = i18n_text(STR_EDIT_EXIT_FUSE);    break;
    default:                   how = i18n_text(STR_EDIT_EXIT_OTHER);   break;
    }

    wchar_t msg[256];
    _snwprintf_s(msg, 256, _TRUNCATE, i18n_text(STR_EDIT_EXIT_FMT), how);
    mainwindow_set_status(g_app.window, msg);

    log_info(L"edit: reported the exit to the status bar (%ls)",
             overlay_exit_name(why));
}

/*
 * End the session in one place: stop the guard, report, and change mode.
 *
 * There are two call sites - a session that ends inside edit_handle_message
 * (the fuse) and one that ends after DispatchMessage - and both must stop the
 * guard. A missed stop would leave a thread behind per session, and t51 runs
 * fifty sessions in a row.
 */
static void finish_edit_session(void)
{
    watchdog_stop();

    const BOOL by_guard = watchdog_fired();

    report_edit_exit(edit_exit_reason());
    mode_transition(MODE_IDLE);

    /*
     * The overlay reports a WM_CLOSE as a toolbar exit, which would be a lie
     * here: say what actually happened. The log line is for the same reason.
     */
    if (by_guard) {
        log_info(L"watchdog: the edit session was closed by the guard");
        mainwindow_set_status(g_app.window, i18n_text(STR_MSG_EDIT_WATCHDOG));
    }
}

/*
 * The workspace page's second list: what the SELECTED workspace contains.
 *
 * Rebuilt from the model every time rather than patched, for the same reason
 * the other lists are: a rebuild cannot leave a stale row behind, and a slot
 * that has been removed has to disappear from the screen.
 */
static void refresh_slots(void)
{
    static MwSlotRow rows[MW_SLOT_MAX];

    const char *id = mainwindow_selected_id(g_app.window);
    const Workspace *w = (id != NULL) ? ws_find_const(&g_app.workspaces, id) : NULL;

    int n = 0;

    if (w != NULL) {
        for (int i = 0; i < w->slot_count && n < MW_SLOT_MAX; ++i) {
            const WorkspaceSlot *s = &w->slots[i];
            MwSlotRow *r = &rows[n];

            ZeroMemory(r, sizeof(*r));
            strncpy_s(r->preset_id, sizeof(r->preset_id), s->preset_id, _TRUNCATE);
            _snwprintf_s(r->order, 16, _TRUNCATE, L"%d", i + 1);

            /*
             * Stage 7: which monitor this slot belongs to. The EDID name is
             * what a user recognises ("H27T22X"); the device name is the
             * fallback for a monitor whose EDID could not be read. A slot that
             * is not bound shows nothing at all, which is what every slot
             * written before this feature is.
             */
            if (s->mon_set) {
                wcsncpy_s(r->monitor, MW_TEXT_MAX,
                          (s->mon_friendly[0] != L'\0') ? s->mon_friendly
                                                        : s->mon_device,
                          _TRUNCATE);
            }

            if (s->preset_id[0] != '\0') {
                wchar_t wide_id[WSS_PRESET_ID_MAX];
                if (str_to_wide(s->preset_id, wide_id, WSS_PRESET_ID_MAX)) {
                    const int idx = rules_find_by_name(&g_app.rules, wide_id);
                    if (idx >= 0) {
                        const Rule *rule = rules_at(&g_app.rules, idx);
                        wcsncpy_s(r->preset, MW_TEXT_MAX,
                                  (rule->label[0] != L'\0') ? rule->label
                                                            : rule->app.exe_name,
                                  _TRUNCATE);
                    } else {
                        /* The preset this slot refers to is gone. Shown by its
                         * id so the row still says WHICH one is missing. */
                        wcsncpy_s(r->preset, MW_TEXT_MAX, wide_id, _TRUNCATE);
                    }
                }
            }

            if (r->preset[0] == L'\0') {
                wcsncpy_s(r->preset, MW_TEXT_MAX, i18n_text(STR_MATCH_NONE), _TRUNCATE);
            }

            /*
             * Stage 7 / task 7.3: a slot whose monitor is gone is shown greyed,
             * and clicking it explains why. The sentence names the monitor it
             * was recorded on - "which screen do I plug back in" is the first
             * question a user has, and the stored geometry is the only thing
             * that answers it once the monitor is not there to be enumerated.
             *
             * It says what was NOT done as well, because "my preset is broken"
             * and "my preset is gone" are very different fears.
             */
            if (s->state == SLOT_ORPHANED) {
                const wchar_t *nm = (s->mon_friendly[0] != L'\0') ? s->mon_friendly
                                                                  : s->mon_device;
                _snwprintf_s(r->blocked, MW_SLOT_WHY_MAX, _TRUNCATE,
                             i18n_text(STR_MSG_SLOT_BLOCKED_FMT),
                             nm, s->mon_w, s->mon_h, s->mon_x, s->mon_y);
            }

            n++;
        }
    }

    mainwindow_set_slot_rows(g_app.window, rows, n);
}

/* ------------------------------------------------------------------ */
/* Command dispatch                                                    */
/* ------------------------------------------------------------------ */

static void handle_command(MainWindowCommand cmd, const char *workspace_id,
                           void *user)
{
    (void)user;

    switch (cmd) {

    case MW_CMD_NEW:
        cmd_new_workspace();
        break;

    case MW_CMD_RENAME:
        cmd_rename_workspace(workspace_id);
        break;

    case MW_CMD_DELETE:
        cmd_delete_workspace(workspace_id);
        break;

    case MW_CMD_APPLY:
        /* [Run] on the workspace page. Until stage 5 this reported "not
         * implemented yet"; the launcher now exists, so it launches. */
        cmd_run_workspace(workspace_id);
        break;

    case MW_CMD_ADD_SLOT:
        cmd_add_slot(workspace_id);
        break;

    case MW_CMD_REMOVE_SLOT:
        cmd_remove_slot(workspace_id);
        break;

    case MW_CMD_UNBIND_SLOT:
        cmd_unbind_slot(workspace_id);
        break;

    case MW_CMD_EDIT_OVERLAY:
        cmd_edit_overlay(workspace_id);
        break;

    case MW_CMD_EDIT:
        MessageBoxW(mainwindow_hwnd(g_app.window),
                    i18n_text(STR_MSG_EDIT_TODO),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONINFORMATION);
        break;

    case MW_CMD_CLOSE_WORKSPACE:
        cmd_close_workspace(workspace_id);
        break;

    case MW_CMD_SETTINGS_APPLY: {
        /*
         * The page edits a copy: the running settings are only replaced here,
         * when the user says so. That is what keeps Cancel - and simply
         * walking to another page - from changing anything.
         */
        Settings edited = g_app.settings;
        mainwindow_settings_read(g_app.window, &edited);

        const BOOL lang_changed  = (edited.language != g_app.settings.language);
        const BOOL watch_changed = (edited.watch_windows !=
                                    g_app.settings.watch_windows);

        g_app.settings = edited;
        save_all();

        /*
         * The language is applied on restart rather than live: the page says
         * so. Rebuilding every caption of a running UI is a much bigger change
         * than it appears, and a restart is what a settings page is normally
         * expected to ask for.
         *
         * This is one of the few dialogs left, because the user pressed a
         * button and has to be told the change did not take effect yet.
         */
        if (lang_changed) {
            MessageBoxW(mainwindow_hwnd(g_app.window),
                        i18n_text(STR_SETTINGS_LANG_NOTE),
                        i18n_text(STR_SETTINGS_TITLE),
                        MB_OK | MB_ICONINFORMATION);
        }

        /*
         * The listener switch takes effect immediately, unlike the language.
         * It is one hook and one boolean, and a user who just turned "put
         * windows back" off and then watched a window jump anyway would
         * rightly call the switch a lie.
         */
        if (watch_changed) {
            if (g_app.settings.watch_windows) {
                if (winwatch_start(g_app.msgwnd, WM_WAC_WATCH)) {
                    log_info(L"watch: listening for new windows");
                } else {
                    /* The hook could not be installed. Rather than leaving a
                     * checked box that does nothing, the setting goes back to
                     * off, is written, and the page is reloaded from it. */
                    g_app.settings.watch_windows = FALSE;
                    save_all();
                    mainwindow_settings_load(g_app.window, &g_app.settings);
                    log_info(L"watch: could not start listening");
                }
            } else {
                winwatch_stop();
                log_info(L"watch: stopped listening");
            }
        }

        log_info(L"settings: applied (language=%hs close_action=%d)",
                 i18n_code(g_app.settings.language),
                 (int)g_app.settings.close_action);
        refresh_status();
        break;
    }

    case MW_CMD_SETTINGS_CANCEL:
        /* Put the controls back to the stored values. Nothing was written, so
         * there is nothing else to undo. */
        mainwindow_settings_load(g_app.window, &g_app.settings);
        log_info(L"settings: cancelled");
        break;

    case MW_CMD_OPEN_STARTUP: {
        /*
         * The guide's one button. The program still does not ADD anything to
         * the startup folder - it only opens
         * the folder so the user can put their own shortcut there.
         */
        wchar_t dir[MAX_PATH];
        if (!sysinfo_startup_folder(dir, MAX_PATH)) {
            log_info(L"startup: could not resolve the startup folder");
            break;
        }
        if (!sysinfo_open_folder(mainwindow_hwnd(g_app.window), dir)) {
            log_info(L"startup: ShellExecute refused to open the folder");
            break;
        }
        log_info(L"startup: opened the startup folder");
        break;
    }

    case MW_CMD_ABOUT: {
        wchar_t line_ws[64], line_hk[128], key[80], msg[768];

        _snwprintf_s(line_ws, 64, _TRUNCATE,
                     i18n_text(STR_MSG_ABOUT_WSCOUNT), g_app.workspaces.count);

        /* The template takes %ls, so the narrow label is converted once here
         * instead of being passed as a %hs argument. */
        _snwprintf_s(key, 80, _TRUNCATE, L"%hs",
                     g_app.hotkey_label[0] ? g_app.hotkey_label : "?");
        _snwprintf_s(line_hk, 128, _TRUNCATE,
                     i18n_text(STR_MSG_ABOUT_HOTKEY), key);

        _snwprintf_s(msg, 768, _TRUNCATE,
                     L"%ls\n"
                     L"%ls\n\n"
                     L"%ls\n"
                     L"%ls\n\n"
                     L"%ls",
                     i18n_text(STR_MSG_ABOUT_BODY),
                     i18n_text(STR_MSG_ABOUT_STAGE),
                     line_ws,
                     line_hk,
                     i18n_text(STR_MSG_ABOUT_CONFIG));

        MessageBoxW(mainwindow_hwnd(g_app.window), msg,
                    i18n_text(STR_MSG_ABOUT_TITLE), MB_OK | MB_ICONINFORMATION);
        break;
    }

    case MW_CMD_QUIT:
        /*
         * The X button and File > Exit arrive here.
         *
         * "Ask" and "remember" are separate facts:
         *
         *   close_action == ASK   -> show the dialog every time
         *   close_action == TRAY  -> hide, keep the hotkey alive
         *   close_action == QUIT  -> end the process
         *
         * The dialog only writes close_action when "Do not ask again" is
         * ticked. Answering the question is not the same as deciding to stop
         * being asked, and conflating them is how a program ends up silently
         * quitting on a user who only meant "yes, this time".
         */
        if (g_app.settings.close_action == CLOSE_ACTION_ASK) {
            CloseConfirmResult r;

            /*
             * NEVER ASK DURING A SHUTDOWN. The box would arrive at the one
             * moment the answer cannot change anything, and a modal box during
             * session end is what makes Windows show "this app is preventing
             * shutdown". Measured 2026-10-10: with close_action = ASK the user
             * saw exactly that prompt while shutting the machine down.
             */
            if (g_session_ending) {
                log_info(L"quit: session is ending, not asking");
                save_all();
                DestroyWindow(g_app.msgwnd);
                return;
            }

            if (!close_confirm_show(mainwindow_hwnd(g_app.window),
                                    CLOSE_ACTION_ASK,
                                    FALSE,          /* never answered before */
                                    &r)) {
                return;   /* cancelled: change nothing */
            }

            if (r.remember) {
                g_app.settings.close_action = r.action;
                save_all();
            }

            /* Act on this answer regardless of whether it was remembered. */
            if (r.action == CLOSE_ACTION_QUIT) {
                DestroyWindow(g_app.msgwnd);
            } else {
                mainwindow_hide(g_app.window);
                tray_add();
                log_info(L"window hidden to tray");
            }
            return;
        }

        if (g_app.settings.close_action == CLOSE_ACTION_TRAY) {
            mainwindow_hide(g_app.window);
            tray_add();
            log_info(L"window hidden to tray");
        } else {
            DestroyWindow(g_app.msgwnd);   /* ends the message loop */
        }
        break;

    case MW_CMD_REFRESH:
        /* Re-read what is on screen: running windows, presets and workspaces.
         * No state changes, no confirmation: this is what the F5 accelerator
         * and the [Refresh] button both send.
         *
         * The log line is deliberate: it is the only observable effect of the
         * accelerator path (TranslateAccelerator -> WM_COMMAND -> here), so it
         * is what proves the keyboard actually reaches this branch. F5 does
         * not happen often enough to make the log noisy. */
        log_info(L"refresh: re-reading (workspaces=%d, presets=%d)",
                 g_app.workspaces.count, rules_count(&g_app.rules));
        refresh_all();
        break;

    case MW_CMD_ANCHOR:
        cmd_anchor_selected();
        break;

    case MW_CMD_MOVE_TO_MONITOR:
        cmd_move_to_selected_monitor();
        break;

    case MW_CMD_DELETE_PRESET:
        cmd_delete_selected_preset();
        break;

    case MW_CMD_APPLY_ALL:
        cmd_apply_all();
        break;

    case MW_CMD_SAVE_PRESET:
        cmd_save_preset();
        break;

    case MW_CMD_CREATE_PRESET:
        cmd_create_preset();
        break;

    case MW_CMD_BEGIN_CREATE:
        cmd_begin_create();
        break;

    case MW_CMD_BROWSE_APPS:
        cmd_browse_apps();
        break;

    default:
        /* Nothing else should arrive. Report rather than guess - and rather
         * than silently doing the wrong thing, which is how the old "-1 means
         * refresh" sentinel behaved when anything else fell through. */
        log_info(L"command: unhandled (%d)", (int)cmd);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* The resident listener (task 5.11)                                   */
/* ------------------------------------------------------------------ */

/*
 * A window appeared that a preset matches - and this program did not start it.
 *
 * That is the case the program was asked for in the first place: the user
 * double-clicks their own icon and wants the window where the preset says.
 * Before this task the only windows that could be placed were ones started
 * from here ([Run], or `wac run`), because that was the only moment the
 * program knew when to look. Every window the user opened themselves was
 * invisible to it.
 *
 * How the work is split: core/winwatch.c owns the SetWinEventHook, and its
 * callback does nothing but post WM_WAC_WATCH. Everything below runs on the
 * main thread, which is also the only thread allowed to move a window.
 *
 * WHY A TIMER APPEARS HERE, AND WHY IT IS NOT POLLING
 *
 *   A window that has just been shown is usually still being arranged by its
 *   own program: it restores a saved position, applies a minimum size a moment
 *   later, then centres itself. Placing it the instant the event arrives would
 *   measure a window that is still moving, and then fight the program for its
 *   own geometry.
 *
 *   So the window is queued and the timer checks whether it has stopped moving.
 *   The timer is created with the first queued window and killed with the last,
 *   so while the user is opening nothing this program has no timer at all. The
 *   hook is what notices things; the timer only ever measures something that
 *   has already been noticed.
 */

/* Windows that can be waiting to settle at once. One is the normal case; the
 * margin is for a program that puts up a splash screen and a main window
 * together. */
#define WATCH_MAX 8

/* How often the settle check runs, and how many consecutive unchanged samples
 * count as settled: 100 ms x 2 = 200 ms of stillness. */
#define WATCH_TICK_MS      100
#define WATCH_STABLE_TICKS 2

/* Give up after this long. A window that is still moving itself after three
 * seconds belongs to a program that is busy setting itself up; moving it then
 * interrupts the user rather than helping. */
#define WATCH_GIVE_UP_MS 3000

typedef struct {
    HWND        hwnd;
    const Rule *rule;
    RECT        last;
    int         stable;
    int         waited;
} WatchCandidate;

static WatchCandidate g_watch[WATCH_MAX];
static int            g_watch_count;
static UINT_PTR       g_watch_timer;

/* Drop the candidate at `i` by moving the last one into its place. */
static void watch_drop(int i)
{
    g_watch[i] = g_watch[g_watch_count - 1];
    g_watch_count--;

    if (g_watch_count == 0 && g_watch_timer != 0) {
        /* Nothing is waiting any more, so the timer goes away again: an idle
         * program has no timer, and an idle thread has nothing to wake up for. */
        (void)KillTimer(g_app.msgwnd, WAC_WATCH_TIMER_ID);

        /* The index worker is told to finish and is joined; a list that is
         * still being built is simply dropped - it is derived data, and the
         * next run rebuilds it from the cache or the registry. */
        if (g_app.appjob != NULL) {
            appindex_job_destroy(g_app.appjob);
            g_app.appjob = NULL;
        }
        if (g_app.appindex != NULL) {
            appindex_destroy(g_app.appindex);
            g_app.appindex = NULL;
        }
        g_watch_timer = 0;
    }
}

/* One tick of the settle check. Only ever called while something is queued. */
static void watch_tick(void)
{
    int i = 0;

    while (i < g_watch_count) {
        WatchCandidate *c = &g_watch[i];

        if (!IsWindow(c->hwnd)) {
            /* Closed again while we waited: a splash screen that finished, or a
             * program that exited. There is nothing left to place. */
            watch_drop(i);
            continue;
        }

        RECT rc;
        ZeroMemory(&rc, sizeof(rc));
        if (!GetWindowRect(c->hwnd, &rc)) {
            watch_drop(i);
            continue;
        }

        if (rc.left  != c->last.left  || rc.top    != c->last.top ||
            rc.right != c->last.right || rc.bottom != c->last.bottom) {
            c->last   = rc;         /* still arranging itself */
            c->stable = 0;
        } else {
            c->stable++;
        }

        if (c->stable >= WATCH_STABLE_TICKS) {
            RECT target;
            ZeroMemory(&target, sizeof(target));
            const PlaceResult res = rule_apply_one(c->hwnd, c->rule, &target);
            if (win_place_result_ok(res)) {
                log_info(L"watch: placed a window (%ls)",
                         win_place_result_name(res));
            } else {
                /* No dialog and no status line. This happens while the user is
                 * busy with something else, and a program that announced
                 * "could not move that window" every time an elevated tool
                 * started would be worse than one that says nothing. The log
                 * has it, and the log is opened by the About page. */
                log_info(L"watch: could not place a window (%ls)",
                         win_place_result_name(res));
            }
            watch_drop(i);
            continue;
        }

        c->waited += WATCH_TICK_MS;
        if (c->waited >= WATCH_GIVE_UP_MS) {
            log_info(L"watch: gave up waiting for a window to settle");
            watch_drop(i);
            continue;
        }

        i++;
    }
}

/*
 * Is this window one we are responsible for?
 *
 * The cheap tests run first, and in this order on purpose. Everything up to
 * the marked line is a read of window metadata. The call after it opens a
 * handle to another process, which is both the expensive part and the part a
 * security product is entitled to notice.
 *
 * Frequency is the whole argument. This runs once per window the user actually
 * opens - seconds apart at best, and never while nothing is happening - rather
 * than for every window on the desktop several times a second. Measured on the
 * author's machine: the sweeping version was flagged by Huorong, and this is
 * the shape the design was changed to (see TRAPS.md and winwatch.h).
 */
static void watch_consider(HWND target)
{
    if (target == NULL || !IsWindow(target)) return;

    /* Our own windows. The hook skips this process, but a second copy of the
     * program is not covered by that flag. */
    DWORD pid = 0;
    (void)GetWindowThreadProcessId(target, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return;

    if (GetAncestor(target, GA_ROOT) != target) return;   /* not top-level */
    if (!IsWindowVisible(target)) return;
    if (GetWindowTextLengthW(target) == 0) return;        /* no caption yet */

    RECT rc;
    ZeroMemory(&rc, sizeof(rc));
    if (!GetWindowRect(target, &rc)) return;
    if (rc.right - rc.left < 80 || rc.bottom - rc.top < 60) return;

    /* ---- the one expensive step ---------------------------------- */
    WindowInfo w;
    ZeroMemory(&w, sizeof(w));
    if (!win_fill_info(target, &w, WIN_ENUM_NEED_EXE)) return;

    const int ri = rules_match(&g_app.rules, &w);
    if (ri < 0) return;                 /* not a window we were asked about */

    const Rule *r = rules_at(&g_app.rules, ri);
    if (r == NULL) return;

    /* Queue it, and make sure the settle timer is running. */
    if (g_watch_count >= WATCH_MAX) {
        /* Table full. Replace the oldest entry rather than dropping this one: a
         * program that opens several windows at once must not lose the last of
         * them silently, and the oldest is the one nearest to giving up. */
        g_watch_count = WATCH_MAX - 1;
    }

    WatchCandidate *c = &g_watch[g_watch_count++];
    ZeroMemory(c, sizeof(*c));
    c->hwnd = target;
    c->rule = r;
    (void)GetWindowRect(target, &c->last);

    if (g_watch_timer == 0) {
        g_watch_timer = SetTimer(g_app.msgwnd, WAC_WATCH_TIMER_ID,
                                 WATCH_TICK_MS, NULL);
    }

    log_info(L"watch: a window matched a preset, waiting for it to settle");
}

/* ------------------------------------------------------------------ */
/* Hidden message window                                               */
/* ------------------------------------------------------------------ */

/*
 * Believe the current monitor topology.
 *
 * Called at start-up and after every settled display change. Nothing else may
 * write the snapshot: the diff is only meaningful against the topology that
 * was last acted on, so a second writer would silently turn every later change
 * into "unchanged".
 */
static void display_refresh_snapshot(void)
{
    g_app.disp_snapshot_count =
        monitor_enumerate(g_app.disp_snapshot, WAC_MON_MAX);
}

/*
 * Re-evaluate every slot against a monitor set, and say what the states are.
 *
 * Runs at start-up and on every settled display change - INCLUDING one whose
 * topology turned out to be unchanged. The reason is the start-up case: a
 * workspace recorded on a monitor that is already unplugged has to be reported
 * as ORPHANED the first time the program looks, and "the topology did not
 * change" is not the same statement as "every slot is fine".
 *
 * Returns how many slots are unusable right now, which is what the grace
 * period is armed on.
 *
 * IT WRITES NO CONFIGURATION - see display_change_settled().
 */
static int display_evaluate_slots(const MonitorFingerprint *set, int count)
{
    int counts[4] = { 0, 0, 0, 0 };   /* indexed by SlotState */

    for (int w = 0; w < g_app.workspaces.count; ++w) {
        Workspace *ws = &g_app.workspaces.items[w];

        for (int s = 0; s < ws->slot_count; ++s) {
            WorkspaceSlot *slot = &ws->slots[s];

            int idx = -1;
            const SlotState st = ws_slot_evaluate_monitor(slot, set, count, &idx);
            slot->state = st;
            if (st >= 0 && st <= SLOT_DEGRADED) counts[st]++;

            if (st != SLOT_READY) {
                log_info(L"display: workspace \"%hs\" slot %d is %ls "
                         L"(recorded on %ls %dx%d at %d,%d)",
                         ws->id, s, ws_slot_state_name(st),
                         (slot->mon_device[0] != L'\0') ? slot->mon_device
                                                        : slot->mon_friendly,
                         slot->mon_w, slot->mon_h, slot->mon_x, slot->mon_y);
            }
        }
    }

    log_info(L"display: slots ready %d, orphaned %d, rebound %d, degraded %d "
             L"(config NOT written)",
             counts[SLOT_READY], counts[SLOT_ORPHANED],
             counts[SLOT_REBOUND], counts[SLOT_DEGRADED]);

    return counts[SLOT_ORPHANED];
}

/*
 * What to do about a topology that has just changed (task 7.2).
 *
 * The rule the design asks for: a monitor going away is an ENVIRONMENT state,
 * not a user intent, and most outages are transient - so the program waits 60
 * seconds before it believes one. Inside that window, a recovery means the
 * whole change is treated as if it had never happened (nothing was written
 * either way, so "never happened" costs nothing to honour).
 *
 * Outside it, the state simply stays ORPHANED and the configuration is left
 * alone - degradation level L1, which is the default (task 7.4). The user is
 * told; the preset is not rewritten. Nothing here ever calls save_all().
 */
static void display_grace_after_pass(int orphans, int prev_orphans)
{
    const DWORD now = GetTickCount();

    if (orphans > 0) {
        if (!displaywatch_grace_armed(&g_app.grace)) {
            displaywatch_grace_arm(&g_app.grace, now, orphans);
            (void)SetTimer(g_app.msgwnd, WAC_GRACE_TIMER_ID,
                           DISPLAYWATCH_GRACE_MS, NULL);
            log_info(L"grace: %d slot(s) unusable; giving the topology %d s "
                     L"to come back", orphans, DISPLAYWATCH_GRACE_MS / 1000);
        } else {
            log_info(L"grace: still %d slot(s) unusable, %lu s left of the "
                     L"%d s wait", orphans,
                     (unsigned long)(displaywatch_grace_ms_left(&g_app.grace, now) / 1000),
                     DISPLAYWATCH_GRACE_MS / 1000);
        }
        return;
    }

    /*
     * Everything works again. Whether that is a recovery inside the window or
     * merely a topology that came back later changes only what is said - the
     * program's state is the same either way, because nothing was ever
     * written.
     */
    if (displaywatch_grace_armed(&g_app.grace)) {
        const DWORD waited = displaywatch_grace_elapsed_ms(&g_app.grace, now);
        (void)displaywatch_grace_clear(&g_app.grace);
        (void)KillTimer(g_app.msgwnd, WAC_GRACE_TIMER_ID);
        log_info(L"grace: topology came back after %lu s - the whole change is "
                 L"treated as if it never happened",
                 (unsigned long)(waited / 1000));
    } else if (prev_orphans > 0) {
        log_info(L"grace: a slot that was unusable is usable again "
                 L"(matched by fingerprint)");
    }
}

/*
 * The topology has stopped moving: say what changed and re-evaluate the slots.
 *
 * This runs ~500 ms after a WM_DISPLAYCHANGE, never inside the handler - the
 * handler runs while the display settings app is blocked in its own
 * SendMessage, and monitor_enumerate() reads every monitor's EDID out of the
 * registry. See core/displaywatch.h.
 *
 * IT WRITES NO CONFIGURATION. A monitor being unplugged is an environment
 * state, not a user intent: the states below are recorded in memory and
 * reported in the log, and nothing reaches the INI file until the user acts
 * (task 7.2's grace period and 7.3's prompt). That rule is the reason this
 * function is allowed to run at all without asking.
 */
static void display_change_settled(void)
{
    MonitorFingerprint now_set[WAC_MON_MAX];
    const int now_count = monitor_enumerate(now_set, WAC_MON_MAX);

    if (now_count <= 0) {
        /* No monitors at all: the console session is going away (fast user
         * switch, RDP disconnect). The snapshot is left alone so that the
         * topology is compared against the one that was there before. */
        log_info(L"display: no monitors reported; snapshot kept");
        return;
    }

    DisplayDiff diff;
    if (!displaywatch_diff(g_app.disp_snapshot, g_app.disp_snapshot_count,
                           now_set, now_count, &diff)) {
        log_info(L"display: diff refused (bad snapshot)");
        return;
    }

    if (diff.same) {
        log_info(L"display: change settled, topology unchanged (%d monitor(s))",
                 now_count);
    } else {
        log_info(L"display: topology changed %d -> %d monitor(s); "
                 L"removed %d, added %d, renamed %d, geometry-moved %d",
                 diff.old_count, diff.new_count,
                 diff.removed, diff.added, diff.renamed, diff.geometry_only);
    }

    /* From here on the new set IS the topology. Recorded before the slots are
     * evaluated, so the messages below describe the state the program will act
     * on - not the one it is leaving. */
    CopyMemory(g_app.disp_snapshot, now_set, sizeof(now_set));
    g_app.disp_snapshot_count = now_count;

    const int prev_orphans = g_app.disp_orphans;
    g_app.disp_orphans = display_evaluate_slots(now_set, now_count);

    display_grace_after_pass(g_app.disp_orphans, prev_orphans);

    /*
     * Rebuild the lists LAST, so the greyed rows and the status line describe
     * the topology this pass just recorded. refresh_all() ends in
     * refresh_status(), which is what puts the "N slots unusable" line up - or
     * takes it down again when the count is back to zero.
     */
    refresh_all();
}

static LRESULT CALLBACK msgwnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_WAC_WATCH:
        /*
         * A window appeared somewhere on the desktop; wParam is the HWND.
         *
         * One window raises several events as it is created and then shown, so
         * the claim table decides which single one is acted on. Without it the
         * window would be queued - and placed - once per event.
         */
        if (!winwatch_claim((HWND)wp)) {
            watch_consider((HWND)wp);
        }
        return 0;

    case WM_WAC_APPINDEX: {
        /*
         * The index worker finished. Take its result, release the job, and -
         * only if the user is still on the picker page - refill it.
         *
         * The elapsed time is logged because §8.6 asks for it: it is the number
         * that says whether "indexing" is a blink or a wait, and whether the
         * cache is doing its job on the second run.
         */
        const unsigned long ms = appindex_job_elapsed_ms(g_app.appjob);

        AppIndex *built = appindex_job_take(g_app.appjob);
        appindex_job_destroy(g_app.appjob);
        g_app.appjob = NULL;

        if (built == NULL) {
            log_info(L"appindex: the worker produced nothing");
            /*
             * No modal box while an edit session is up: the overlay is
             * topmost, so the dialog would be behind a full-screen window -
             * invisible and undismissable. The panel simply stops saying
             * "indexing" and reports that nothing was found.
             */
            if (!edit_is_active()) {
                MessageBoxW(g_app.msgwnd, i18n_text(STR_MSG_INDEX_FAILED),
                            i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONWARNING);
            }
            edit_push_sidebar();
            return 0;
        }

        g_app.appindex = built;
        log_info(L"appindex: ready - %d entries in %lu ms",
                 appindex_count(built), ms);

        /* The sidebar may be open right now, waiting for exactly this. */
        edit_push_sidebar();

        if (mainwindow_create_page_open(g_app.window)) {
            fill_create_page_from_index();
        }
        return 0;
    }

    case WM_TIMER:
        /* The settle check for windows the listener queued. There is no timer
         * at all unless something is waiting - see watch_drop(). */
        if (wp == WAC_WATCH_TIMER_ID) {
            watch_tick();
            return 0;
        }

        /*
         * The other timer on this window: the display-change debounce.
         *
         * It is armed by WM_DISPLAYCHANGE below, and it RE-ARMS itself when the
         * topology moved again inside the window - the wait is "500 ms of
         * quiet", not "500 ms after the first message".
         */
        if (wp == WAC_DISPLAY_TIMER_ID) {
            const DWORD now = GetTickCount();

            if (!displaywatch_due(&g_app.dwatch, now)) {
                (void)SetTimer(hwnd, WAC_DISPLAY_TIMER_ID,
                               displaywatch_ms_until_due(&g_app.dwatch, now),
                               NULL);
                return 0;
            }

            (void)KillTimer(hwnd, WAC_DISPLAY_TIMER_ID);

            /* Read the burst size BEFORE take() clears it: "settled after 5
             * message(s)" is the number that proves the debounce worked, and
             * it is the only place that number exists. */
            const int msgs = g_app.dwatch.messages;
            if (displaywatch_take(&g_app.dwatch)) {
                log_info(L"display: debounce closed after %d message(s)", msgs);
                display_change_settled();
            }
            return 0;
        }

        /*
         * The third timer: how long a broken topology was given to come back
         * (task 7.2). Expiry does NOT change any state beyond the wait itself -
         * the slots stay ORPHANED and the configuration stays untouched. That
         * is degradation level L1, the default: the program reports the
         * conflict instead of deciding for the user.
         */
        if (wp == WAC_GRACE_TIMER_ID) {
            const DWORD now = GetTickCount();

            if (!displaywatch_grace_due(&g_app.grace, now)) {
                /* Kept as a guard rather than a possibility: nothing shortens
                 * this timer today, and a re-arm here would be the silent
                 * "the deadline moved" bug the state machine exists to
                 * prevent. */
                (void)SetTimer(hwnd, WAC_GRACE_TIMER_ID,
                               displaywatch_grace_ms_left(&g_app.grace, now),
                               NULL);
                return 0;
            }

            (void)KillTimer(hwnd, WAC_GRACE_TIMER_ID);

            const int orphans = g_app.grace.orphans;
            if (displaywatch_grace_expire(&g_app.grace)) {
                log_info(L"grace: %d slot(s) still unusable after %d s; keeping "
                         L"the presets as they are (L1 - config NOT written)",
                         orphans, DISPLAYWATCH_GRACE_MS / 1000);
            }
            return 0;
        }
        break;

    /*
     * The display configuration moved: a resolution change, a monitor plugged
     * or unplugged, a driver reset, a KVM switch, waking from sleep.
     *
     * IT MUST STAY CHEAP. This message is a BROADCAST - the sender is blocked
     * inside SendMessage until every top-level window returns - so the work is
     * deferred to the timer above. wParam is the new colour depth and lParam
     * the new size OF THE PRIMARY MONITOR, logged as a hint and never used as
     * the topology: a monitor plugged into a different port changes the
     * topology without changing either number.
     */
    case WM_DISPLAYCHANGE:
        log_info(L"display: WM_DISPLAYCHANGE %ux%u, %u bpp",
                 (unsigned)LOWORD(lp), (unsigned)HIWORD(lp), (unsigned)wp);
        displaywatch_note_change(&g_app.dwatch, GetTickCount());
        (void)SetTimer(hwnd, WAC_DISPLAY_TIMER_ID,
                       DISPLAYWATCH_DEBOUNCE_MS, NULL);
        return 0;

    /*
     * Sleep and resume (stage 7, task 7.7).
     *
     * WHY THIS IS NEEDED EVEN THOUGH RESUME USUALLY CHANGES NOTHING
     *
     *   MEASURED 2026-10-09 on this machine (Modern Standby): Kernel-Power 506
     *   at 23:47:48, 507 at 23:48:40, with wac running across it - and the log
     *   shows NO WM_DISPLAYCHANGE on resume at all. The system only broadcasts
     *   that message when the display mode actually changes, and coming back to
     *   the same mode is not a change.
     *
     *   So relying on WM_DISPLAYCHANGE alone means: the day a monitor fails to
     *   come back after a resume - a docked laptop whose external panel stays
     *   dark is the classic case - the program would not notice, because the
     *   one message it listens for never arrives. That case has NOT been
     *   observed here; the handler is cheap insurance for it, and it costs one
     *   call into the settled pass that already exists.
     *
     *   A message-only window would never have seen this message either - see
     *   trap 34; the message window is top-level precisely for this class of
     *   broadcast.
     */
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) {
            log_info(L"power: entering sleep");
            return 0;
        }
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            log_info(L"power: resumed (wParam=%u); re-checking the topology",
                     (unsigned)wp);

            /*
             * NOT display_change_settled() directly. This handler is reached
             * through a broadcast too - the power service waits for it - and the
             * settled pass reads every monitor's EDID out of the registry. So
             * the resume is treated exactly like a display change: note it, arm
             * the same 500 ms debounce, and let the timer run the pass. A
             * resume that DOES send WM_DISPLAYCHANGE therefore merges into one
             * pass instead of running two.
             */
            displaywatch_note_change(&g_app.dwatch, GetTickCount());
            (void)SetTimer(hwnd, WAC_DISPLAY_TIMER_ID,
                           DISPLAYWATCH_DEBOUNCE_MS, NULL);
            return 0;
        }
        break;

    case WM_WAC_ACTIVATE:
        /* A second launch asked us to surface. */
        log_info(L"ipc: activate request received");
        mode_transition(MODE_IDLE);
        mainwindow_show(g_app.window);
        return 0;

    case WM_HOTKEY:
        if (wp == WAC_HOTKEY_ID) {
            log_info(L"hotkey: fired");
            /* Stage 2: the emergency exit. Whatever mode the program is in,
             * this must return the user to a usable state. The editor does
             * not exist yet, so for now it surfaces the window. */
            mainwindow_show(g_app.window);
            mode_transition(MODE_IDLE);
            mainwindow_set_status(g_app.window,
                                  i18n_text(STR_MSG_EDIT_TODO));
            return 0;
        }
        break;

    case WM_WAC_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            mainwindow_show(g_app.window);
            return 0;
        case WM_RBUTTONUP: {
            /* Minimal tray menu: it is a convenience, never the only path to
             * anything. Every action here also exists in the main window. */
            HMENU m = CreatePopupMenu();
            if (m == NULL) return 0;
            AppendMenuW(m, MF_STRING, IDM_TRAY_SHOW, i18n_text(STR_TRAY_SHOW));
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING, IDM_TRAY_QUIT, i18n_text(STR_TRAY_QUIT));

            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            const UINT picked = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                               pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(m);

            if (picked == IDM_TRAY_SHOW) mainwindow_show(g_app.window);
            if (picked == IDM_TRAY_QUIT) {
                /* Quitting from the tray menu is explicit: honour it without
                 * asking, and without changing the stored preference - the
                 * user asked for this once, not forever. */
                g_app.settings.close_action = CLOSE_ACTION_QUIT;
                DestroyWindow(hwnd);
            }
            return 0;
        }
        default:
            break;
        }
        return 0;

    /*
     * The session is going down. This arrives BEFORE the closes, and it is the
     * only warning the program gets - see g_session_ending.
     *
     * Answering TRUE says "close me, I am ready"; FALSE would block the
     * shutdown, which is a thing this program must never do.
     */
    case WM_QUERYENDSESSION:
        g_session_ending = TRUE;
        mainwindow_set_prompt_suppressed(g_app.window, TRUE);
        /* The editor's list is rebuilt with the greyed rows; nothing new to do
         * here - the point is that NO prompt can appear from now on. */
        log_info(L"system: session is ending (lParam=0x%lX)", (unsigned long)lp);
        save_all();
        return TRUE;

    case WM_ENDSESSION:
        /* Windows is shutting down: persist and go, do not linger. */
        g_session_ending = TRUE;
        mainwindow_set_prompt_suppressed(g_app.window, TRUE);
        log_info(L"system: session ending (wParam=%lu)", (unsigned long)wp);
        save_all();
        return 0;

    case WM_DESTROY:
        /* Stop listening before the window that receives the posts disappears,
         * or the callback would keep posting to a dead handle. */
        winwatch_stop();
        if (g_watch_timer != 0) {
            (void)KillTimer(hwnd, WAC_WATCH_TIMER_ID);
            g_watch_timer = 0;
        }
        /* A pending display debounce dies with the window. Leaving it armed
         * would have it fire against a snapshot nobody is going to read. */
        (void)KillTimer(hwnd, WAC_DISPLAY_TIMER_ID);
        (void)KillTimer(hwnd, WAC_GRACE_TIMER_ID);
        displaywatch_init(&g_app.dwatch);
        displaywatch_grace_init(&g_app.grace);
        g_app.disp_orphans = 0;
        g_watch_count = 0;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND create_msg_window(HINSTANCE hinst)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = msgwnd_proc;
    wc.hInstance     = hinst;
    wc.lpszClassName = WAC_MSG_WINDOW;

    if (RegisterClassExW(&wc) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return NULL;
    }

    /*
     * A real window, just never shown: WM_HOTKEY is delivered to a window, not
     * to a thread, so one has to exist.
     *
     * IT IS A TOP-LEVEL WINDOW, NOT A MESSAGE-ONLY ONE, and that is not a
     * detail of style. This window also has to receive WM_DISPLAYCHANGE, which
     * Windows delivers as a BROADCAST - and an HWND_MESSAGE window receives no
     * broadcasts at all (it cannot even be enumerated, so the FindWindow below
     * never found it either).
     *
     * MEASURED 2026-10-09, t60_displaychange: one synthetic
     * SendMessage(HWND_BROADCAST, WM_DISPLAYCHANGE) from another process
     * reached a hidden top-level window and did not reach a message-only
     * window in the same process. Before that measurement the display-change
     * listener had nothing to listen on.
     *
     * WS_POPUP + WS_EX_TOOLWINDOW + never shown: no taskbar button, no Alt+Tab
     * entry, no z-order of its own, and zero-sized so the window enumerators in
     * core/win_enum.c skip it on their size test.
     */
    return CreateWindowExW(WS_EX_TOOLWINDOW, WAC_MSG_WINDOW, L"",
                           WS_POPUP,
                           0, 0, 0, 0, NULL, NULL, hinst, NULL);
}

/* ------------------------------------------------------------------ */
/* Hotkey                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    UINT        mods;
    UINT        vk;
    const char *label;
} HotkeyCandidate;

/*
 * Ordered by how unlikely each is to be taken by something else. Measured in
 * stage-1 experiment 5: the first entry was free on the development machine.
 */
static const HotkeyCandidate k_hotkeys[] = {
    { MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F12, "Ctrl+Alt+Shift+F12" },
    { MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F11, "Ctrl+Alt+Shift+F11" },
    { MOD_CONTROL | MOD_ALT,             VK_F12, "Ctrl+Alt+F12"       },
    { MOD_CONTROL | MOD_ALT,             'Q',     "Ctrl+Alt+Q"         },
};
#define HOTKEY_COUNT (int)(sizeof(k_hotkeys) / sizeof(k_hotkeys[0]))

static void register_hotkey(void)
{
    for (int i = 0; i < HOTKEY_COUNT; ++i) {
        if (RegisterHotKey(g_app.msgwnd, WAC_HOTKEY_ID,
                           k_hotkeys[i].mods | MOD_NOREPEAT, k_hotkeys[i].vk)) {
            g_app.hotkey_ok    = TRUE;
            g_app.hotkey_mods  = k_hotkeys[i].mods;
            g_app.hotkey_vk    = k_hotkeys[i].vk;
            strncpy_s(g_app.hotkey_label, sizeof(g_app.hotkey_label),
                      k_hotkeys[i].label, _TRUNCATE);
            log_info(L"hotkey: registered %hs", k_hotkeys[i].label);

            /* Mirror into the settings struct as well.
             *
             * The settings dialog reads its display label from there, and
             * these two fields are the same fact stored twice - which is
             * exactly how the dialog ended up showing "?" while the status
             * bar showed the real combination: only one of them was updated.
             * Keeping the copy in one function makes that impossible. */
            settings_set_hotkey_label(&g_app.settings, k_hotkeys[i].label);
            return;
        }
    }

    /* Every candidate was taken. Tell the user rather than running with no
     * way out - the plan treats an unreachable exit key as a hard failure. */
    strncpy_s(g_app.hotkey_label, sizeof(g_app.hotkey_label), "(none)", _TRUNCATE);
    settings_set_hotkey_label(&g_app.settings, "(none)");
    log_info(L"hotkey: ALL candidates were already registered");

    /* The status bar carries a permanent "(not registered)" marker for this, so
     * one line there is enough - a dialog on every boot is not. */
    wcsncpy_s(g_startup_note, _countof(g_startup_note),
              i18n_text(STR_MSG_HOTKEY_FAILED), _TRUNCATE);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev; (void)cmdline; (void)show;

    /*
     * Command line FIRST, ahead of the single-instance mutex.
     *
     * "wac place --app notepad.exe" has to work while the window is already
     * open. The mutex exists to stop a second WINDOW appearing; a
     * command-line run opens no window at all, so being turned away by that
     * mutex would make the command silently do nothing whenever the program
     * happened to be running - which is most of the time.
     */
    {
        int argc = 0;
        wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);

        if (argv != NULL) {
            if (cli_is_command(argc, argv)) {
                int rc;

                /*
                 * The console is attached best-effort: when it fails there is
                 * simply nowhere to print, but the command still has effects
                 * the user asked for, so refusing to run would be worse than
                 * running silently.
                 */
                cli_attach_console();
                rc = cli_run(argc, argv);

                LocalFree(argv);
                return rc;
            }
            LocalFree(argv);
        }
    }

    ZeroMemory(&g_app, sizeof(g_app));
    g_app.hinst = hinst;
    g_app.mode  = MODE_IDLE;

    /*
     * Single instance.
     *
     * The mutex is created first: if it already exists, another copy is
     * running, and the right response is to surface THAT one, not to exit
     * silently. A user who double-clicks the icon twice must not be left
     * wondering why nothing opened.
     */
    g_app.mutex = CreateMutexW(NULL, FALSE, WAC_MUTEX_NAME);
    if (g_app.mutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(WAC_MSG_WINDOW, NULL);
        if (existing != NULL) {
            /*
             * AllowSetForegroundWindow: the target process is the one that
             * gets to decide whether a background process may steal focus,
             * and this is the documented way to grant that permission.
             */
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(existing, WM_WAC_ACTIVATE, 0, 0);
        }
        CloseHandle(g_app.mutex);
        return 0;
    }

    /*
     * Logging comes first, before anything that can fail.
     *
     * The config directory is created here rather than in load_settings so
     * that a failure during window creation is still recorded - a startup
     * that dies with no diagnostic is the hardest kind to fix.
     *
     * log_init is a no-op when the directory cannot be determined, and every
     * log call after it is a no-op too, so this cannot itself become the
     * failure it is meant to report.
     */
    {
        wchar_t dir[MAX_PATH];
        if (config_dir(dir, MAX_PATH)) {
            log_init(dir);
            /* One generation of rotation: a long-lived install must not grow
             * an unbounded file. */
            log_rotate_if_large(1024u * 1024u);
        }
    }

    log_info(L"---- startup ----");

    /* ---- windows -------------------------------------------------- */
    if (!mainwindow_register_class(hinst)) {
        MessageBoxW(NULL, i18n_text(STR_ERR_WINDOW_CLASS),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONERROR);
        return 1;
    }

    g_app.msgwnd = create_msg_window(hinst);
    if (g_app.msgwnd == NULL) {
        MessageBoxW(NULL, i18n_text(STR_ERR_MSG_WINDOW),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONERROR);
        return 1;
    }

    /* ---- config --------------------------------------------------- */
    ws_clear(&g_app.workspaces);
    load_settings();

    const BOOL first_run = (g_app.workspaces.count == 0);
    if (first_run) {
        ws_seed_defaults(&g_app.workspaces);
        log_info(L"config: first run, seeded %d default workspace(s)",
                 g_app.workspaces.count);
    }

    /* ---- window --------------------------------------------------- */
    g_app.window = mainwindow_create(hinst, handle_command, NULL);
    if (g_app.window == NULL) {
        MessageBoxW(NULL, i18n_text(STR_ERR_MAIN_WINDOW),
                    i18n_text(STR_ERR_TITLE), MB_OK | MB_ICONERROR);
        return 1;
    }

    register_hotkey();

    /*
     * The monitor topology as it is now, so that the first WM_DISPLAYCHANGE has
     * something to be different FROM. Taken here - after the message window
     * exists and before any window is placed - because the snapshot describes
     * the environment the stored slots will be evaluated against.
     */
    displaywatch_init(&g_app.dwatch);
    displaywatch_grace_init(&g_app.grace);
    display_refresh_snapshot();
    log_info(L"display: watching %d monitor(s) for topology changes",
             g_app.disp_snapshot_count);

    /* And what that topology means for the stored workspaces, before the user
     * can see the window: a slot whose monitor is already unplugged is
     * reported here rather than waiting for a display change that may never
     * come. Nothing is written - see display_evaluate_slots().
     *
     * NO GRACE PERIOD AT START-UP. The wait exists to absorb a change that has
     * just happened; a monitor that was already gone when the program started
     * may have been gone for days, and there is nothing to wait for. The slots
     * are simply reported in whatever state they are in. */
    g_app.disp_orphans = display_evaluate_slots(g_app.disp_snapshot,
                                                g_app.disp_snapshot_count);

    /* The settings page shows the stored values and the hotkey that was
     * actually registered - which is only known after register_hotkey(). */
    mainwindow_settings_load(g_app.window, &g_app.settings);

    /* Presets feed the overview page, so they are read before the first
     * refresh. load_rules() owns the "could not be read" report. */
    load_rules();

    refresh_all();

    /* After the refresh: refresh_status() would overwrite anything set before
     * it, and this is the only report the user gets. */
    if (g_startup_note[0] != L'\0') {
        mainwindow_set_status(g_app.window, g_startup_note);
    }

    /*
     * The resident listener (task 5.11). Started last, on purpose: it needs
     * the presets, which load_rules() has just read, and it posts to the
     * message window, which must already exist.
     */
    if (g_app.settings.watch_windows) {
        if (winwatch_start(g_app.msgwnd, WM_WAC_WATCH)) {
            log_info(L"watch: listening for new windows");
        } else {
            /* Not fatal: everything except "start the program yourself" still
             * works without it, so this does not stop the program or show a
             * dialog at start-up. */
            log_info(L"watch: could not start listening");
        }
    }

    /*
     * The layout fill listener is NOT started here: it belongs to the edit
     * session (cmd_edit_overlay), because outside edit mode the system's own
     * snap layouts do the job better and a second one would fight them.
     */

    mainwindow_show(g_app.window);

    if (first_run) {
        save_all();   /* write the seeded workspace so the file exists */
    }

    log_info(L"ready: %d workspace(s), hotkey %hs",
             g_app.workspaces.count, g_app.hotkey_label);

    /* ---- message loop --------------------------------------------- */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* The guard is alive only while a session is; see core/watchdog.h. */
        watchdog_tick();

        /*
         * Order matters here.
         *
         *   1. Accelerators: TranslateAcceleratorW turns a matching key into
         *      WM_COMMAND for the main window. It must come first, otherwise
         *      the focused control would eat the key (F5 in a list, say).
         *   2. IsDialogMessageW: lets the window's controls handle Tab and the
         *      arrow keys as one dialog-like group.
         *   3. Everything else - thread messages and the hotkey, which belong
         *      to no window - is dispatched raw.
         */
        /*
         * The edit session gets first refusal on its own messages: the fuse's
         * WM_TIMER arrives on the thread queue (hwnd == NULL) and belongs to
         * no window at all.
         */
        if (edit_handle_message(&msg)) {
            /*
             * The fuse fired. Ending the session right here matters: with the
             * overlay up and the user doing nothing there is no next message
             * to notice it, and the overlay would sit on screen until they
             * touched the keyboard.
             */
            if (edit_poll()) {
                finish_edit_session();
            }
            continue;
        }

        if (mainwindow_translate_accel(g_app.window, &msg)) continue;
        if (IsDialogMessageW(mainwindow_hwnd(g_app.window), &msg)) continue;

        TranslateMessage(&msg);
        DispatchMessageW(&msg);

        /*
         * Any exit request raised while that message was handled - Esc, the
         * toolbar's exit button, or the global hotkey - ends the session here,
         * again without waiting for another message. edit_poll() is what
         * releases the session's resources; this is the only place it is
         * called from, so there is one release point (ui/editstate.h).
         */
        if (edit_poll()) {
            finish_edit_session();
        }
    }

    /* ---- shutdown ------------------------------------------------- */
    watchdog_stop();
    mode_transition(MODE_SHUTTING_DOWN);

    if (g_app.hotkey_ok) {
        UnregisterHotKey(g_app.msgwnd, WAC_HOTKEY_ID);
    }
    tray_remove();
    save_all();

    mainwindow_destroy(g_app.window);
    if (g_app.mutex) CloseHandle(g_app.mutex);

    log_info(L"---- shutdown complete ----");
    return 0;
}
