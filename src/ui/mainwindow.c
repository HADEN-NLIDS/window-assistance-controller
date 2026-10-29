/*
 * mainwindow.c - the main window.
 *
 * STRUCTURE
 *
 *   One window, several pages that replace each other's content, and no menu
 *   bar. The layout from top to bottom is:
 *
 *     [ self-drawn caption ]   <- frame.c
 *     [ navigation strip   ]   <- window presets / workspaces / refresh / settings
 *     [ page title row     ]   <- only on sub-pages: back button + page name
 *     [ page content       ]   <- one page visible at a time
 *     [ status bar         ]   <- a real status-bar control
 *
 *   Pages are NOT separate windows: a page is a group of controls that are
 *   shown or hidden together. That keeps every control's parent the main
 *   window, which matters because ListView notifications (LVN_ITEMCHANGED,
 *   NM_DBLCLK) are delivered to the PARENT - putting the list inside a
 *   container window would silently divert them.
 *
 *   The navigation stack itself lives in nav.c, free of Win32, so it can be
 *   unit tested (tests/t13_navstack.c).
 *
 * WHY THE MENU BAR IS GONE
 *
 *   The menu was a second, parallel navigation system that could not express
 *   "window presets" versus "workspace presets" as two first-class places.
 *   Its shortcuts were also pure decoration: nothing created an accelerator
 *   table, so Ctrl+N and F5 did nothing until stage 4.5 added one
 *   (TRAPS.md #24).
 */

#include "mainwindow.h"
#include "i18n.h"
#include "frame.h"
#include "nav.h"
#include "page_help.h"
#include "page_settings.h"
#include "preview.h"
#include "uiformat.h"
#include "pagelist.h"
#include "util/log.h"
#include "util/strutil.h"
#include "util/sysinfo.h"

/* fabs() lives in math.h. Leaving it out is TRAPS.md #9: the
 * implicitly declared version returns int, so equal doubles compare unequal. */
#include <math.h>

#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>   /* calloc / free */
#include <string.h>

#pragma comment(lib, "comctl32.lib")

/* ---- control and command ids --------------------------------------- */

#define IDC_LISTVIEW     1001
#define IDC_BTN_APPLY    1002   /* "Run" on the workspace page */
#define IDC_BTN_CLOSE    1004
#define IDC_BTN_NEW      1005
#define IDC_BTN_DELETE   1006
#define IDC_BTN_SETTINGS 1007
#define IDC_STATUSBAR    1008

#define IDC_NAV_WINDOWS    1010
#define IDC_NAV_WORKSPACES 1011
#define IDC_NAV_REFRESH    1012
#define IDC_NAV_SETTINGS   1013
#define IDC_BACK           1014
#define IDC_PAGE_TITLE     1015

/* Overview page: the two stacked lists, their headings, and the action row. */
#define IDC_WIN_LIST       1030
#define IDC_PRESET_LIST    1031
#define IDC_SEC_WINDOWS    1032
#define IDC_SEC_PRESETS    1033
#define IDC_BTN_ANCHOR     1034
#define IDC_BTN_MOVE_MONITOR 1080   /* 2026-10-09; renamed 2026-10-10 */
#define IDC_BTN_DEL_PRESET 1035
#define IDC_BTN_APPLY_ALL  1036

/* Window-preset page: its own list, plus the hand-editing panel. */
#define IDC_PRESET_PAGE_LIST 1040
#define IDC_BTN_EDIT_PRESET  1041
#define IDC_EDIT_TARGET      1042
#define IDC_EDIT_LABEL_X     1043
#define IDC_EDIT_LABEL_Y     1044
#define IDC_EDIT_LABEL_W     1045
#define IDC_EDIT_LABEL_H     1046
#define IDC_EDIT_X           1047
#define IDC_EDIT_Y           1048
#define IDC_EDIT_W           1049
#define IDC_EDIT_H           1050
#define IDC_EDIT_PIXELS      1051
#define IDC_EDIT_NORMALIZED  1052
#define IDC_EDIT_PREVIEW     1053
#define IDC_EDIT_SAVE        1054
#define IDC_EDIT_CANCEL      1055
#define IDC_BTN_CREATE_PRESET 1056
#define IDC_BTN_DEL_PRESET2  1057
#define IDC_LABEL_TARGET     1058
#define IDC_COMBO_TARGET     1059
#define IDC_LABEL_MONITOR    1060
#define IDC_COMBO_MONITOR    1061
#define IDC_LABEL_POSITION   1062
#define IDC_LABEL_SIZE       1063
#define IDC_EDIT_LOCKED      1064   /* 2026-10-10: "this window cannot be resized" */
#define IDC_EDIT_REAL        1065   /* 2026-10-10: the window's real size on screen */
#define IDC_CHK_PIN          1066   /* 2026-10-10: pin this preset to one screen */
#define IDC_EDIT_PIN         1067   /* 2026-10-10: what it is pinned to */

/* Create-preset picker page. */
#define IDC_CREATE_LIST      1070
#define IDC_BTN_DO_CREATE    1071
#define IDC_BTN_MORE         1072
#define IDC_BTN_CREATE_CANCEL 1073
#define IDC_CREATE_HINT      1074

/* Workspace page: the second list, showing what the selected one contains. */
#define IDC_SEC_SLOTS        1075
#define IDC_SLOT_LIST        1076
#define IDC_BTN_EDIT_WORKSPACE 1078
#define IDC_BTN_UNBIND_SLOT  1079   /* stage 7: drop the monitor binding */

/* The four coordinate fields, in the order the editor presents them. */
#define EDIT_FIELD_COUNT   4
#define EDIT_FIELD_X       0
#define EDIT_FIELD_Y       1
#define EDIT_FIELD_W       2
#define EDIT_FIELD_H       3

/* Command ids. These are also the accelerator ids, so the menu-less window
 * still has a keyboard path for everything the menu used to offer. */
#define IDM_NEW        2001
#define IDM_RENAME     2002
#define IDM_DELETE     2003
#define IDM_SETTINGS   2004
#define IDM_QUIT       2005
#define IDM_ABOUT      2006
#define IDM_APPLY      2010
#define IDM_EDIT       2011
#define IDM_CLOSE_WS   2012
#define IDM_REFRESH    2020

/*
 * Posted from WM_NCCALCSIZE.
 *
 * A change in the CLIENT area does NOT produce WM_SIZE - that message only
 * fires when the WINDOW rectangle changes, and with a self-drawn frame the two
 * are not the same event. The client area is still 22 px narrower while the
 * window is being created, so a layout that only ran then leaves the controls
 * short of the right-hand edge; re-running it from the queue, once the geometry
 * has settled, is what fixes it.
 */
#define WM_APP_RELAYOUT (WM_APP + 10)

/* ---- overview list columns ----------------------------------------- */

/* A column of a report-view list. */
typedef struct {
    StrId text;
    int   width;
} ColSpec;

/*
 * The overview lists' columns.
 *
 * `width` doubles as a RELATIVE WEIGHT, not a pixel count: the layout pass
 * redistributes the list's actual width in these proportions. Fixed pixel
 * widths look right only on the window size they were chosen for - and once
 * they were scaled by the window DPI they summed to more than the window,
 * which is exactly the horizontal scrollbar that was reported.
 */
static const ColSpec kWindowCols[] = {
    { STR_COL_APP,   180 },
    { STR_COL_TITLE, 360 },
    { STR_COL_STATE, 220 },
    { STR_COL_SIZE,  130 },
};

static const ColSpec kPresetCols[] = {
    { STR_COL_APP,     180 },
    { STR_COL_PRESET,  240 },
    { STR_COL_ZONE,    220 },
    { STR_COL_MONITOR, 180 },
    { STR_COL_STATE,   100 },
};

/* The workspace page's second list. Two columns: which presets the workspace
 * is made of (in run order), and which MONITOR each slot belongs to - the
 * stage-7 binding. An unbound slot shows a blank there, and a blank is
 * exactly what every slot written before that feature means: "put it wherever
 * its window turns up". */
static const ColSpec kSlotCols[] = {
    { STR_COL_PRESET,  500 },
    { STR_COL_SCREEN,  200 },
};

/* Defined next to make_listview(); the layout pass calls it for both lists. */
static void fit_columns(HWND list, const ColSpec *cols, int ncols, int client_w);

/* ---- layout constants (96 dpi; scaled at use) ---------------------- */

#define NAV_BTN_W   108
#define NAV_BTN_H   30
#define NAV_GAP     6
#define BTN_H       30
#define BTN_W       96
#define TITLE_H     30
#define BACK_W      92
#define MARGIN      8
#define SECTION_H   22   /* the "Running windows (3)" heading strip */

/* The navigation strip has four entries. */
#define NAV_COUNT   4

/* [Refresh] is created with the strip but laid out on the overview's action
 * row: it refreshes what the overview shows, so it belongs to that page. */
#define NAV_IDX_REFRESH 2

/*
 * What the workspace list actually shows.
 *
 * NOT a Workspace. A Workspace carries sixteen slots and is about 22 KB, so
 * keeping 64 of them here for a three-column list cost 1.4 MB of heap - and
 * because the window is calloc'd, all of it landed in the working set.
 * Measured: 13.91 MB before slots existed, 15.01 MB after, and this is where
 * the difference came from.
 */
typedef struct {
    char id[WS_ID_MAX];
    char name[WS_NAME_MAX];
    int  slot_count;
} MwWorkspaceRow;

struct MainWindow {
    HWND        hwnd;
    HWND        status;
    Frame       frame;
    NavStack    nav;
    /* Navigation strip. Index order matches nav_click_page(). */
    HWND        nav_btn[NAV_COUNT];
    HWND        back_btn;
    HWND        page_title;
    HWND        page_heading;      /* the large page name on sub-pages */

    /* Overview page: running windows, then window presets. */
    HWND        win_list;
    HWND        preset_list;
    HWND        sec_windows;
    HWND        sec_presets;
    BOOL        no_prompt;     /* session is ending: never ask (2026-10-10) */
    HWND        btn_anchor;
    HWND        btn_move_monitor;   /* 2026-10-09: move the selected preset's
                                     * windows to the monitor the box above
                                     * has selected (retargeted 2026-10-10) */
    HWND        btn_delete_preset;
    HWND        btn_apply_all;

    MwWindowRow win_rows[MW_ROW_MAX];
    int         win_count;
    MwPresetRow preset_rows[MW_ROW_MAX];
    int         preset_count;

    /* Selections, kept across a reload so [Refresh] does not lose the user's
     * place. The window id survives a rebuild; a row index would not. */
    unsigned long long sel_window;
    char        sel_preset[64];

    /* Workspace page (moved off the overview in stage 4.5 batch A3). */
    HWND        list;
    HWND        buttons[8];

    /*
     * The second list on that page: what the SELECTED workspace contains.
     *
     * A slot can be added from here but until stage 5 nothing could take one
     * out again - the page could only grow a workspace. This is the other
     * half, and it needs its own list because a slot is chosen by looking at
     * it.
     */
    HWND        sec_slots;
    HWND        slot_list;
    MwSlotRow   slot_rows[MW_SLOT_MAX];
    int         slot_row_count;

    /*
     * Window-preset page.
     *
     * It has its OWN list rather than reusing the overview's: a control lives
     * in one place at a time, and the editor has to be able to show the preset
     * being edited while the overview keeps showing everything.
     */
    HWND        preset_page_list;
    HWND        btn_create_preset;
    HWND        btn_edit_preset;
    HWND        btn_delete_preset2;

    /* Creating a preset needs a target. The list of running windows already
     * exists for the overview, so the combo is filled from the same rows. */
    HWND        cb_target;
    HWND        lbl_target;
    HWND        cb_monitor;      /* reserved: multi-monitor selection */
    HWND        lbl_monitor;
    HWND        lbl_position;
    HWND        lbl_size;

    /* Create-preset picker page: its own snapshot of the window list. */
    HWND        create_list;
    HWND        create_hint;
    HWND        btn_do_create;
    HWND        btn_more;
    HWND        btn_create_cancel;
    MwWindowRow create_rows[MW_CREATE_MAX];
    int         create_count;
    unsigned long long create_pick;
    int         create_pick_row;     /* which row that pick came from, -1 = none:
                                      * the row is what carries the exe path for
                                      * a program that is not running */

    /* Pages pushed with nav_push(); true while the picker is on top. */
    BOOL        create_open;

    /* The hand-editing panel. Hidden until [Edit selected preset] is pressed;
     * while it is open the page shows it instead of the lower half of the
     * list. */
    BOOL        ed_open;
    BOOL        ed_dirty;            /* typed values differ from what was loaded */
    double      ed_orig[EDIT_FIELD_COUNT];   /* what the preset held when opened */
    char        ed_rule_id[64];      /* which preset is being edited */
    HWND        ed_target;           /* read-only: shows what is being edited */
    HWND        ed_label[EDIT_FIELD_COUNT];
    HWND        ed_field[EDIT_FIELD_COUNT];
    HWND        ed_pixels;           /* the same zone in monitor pixels */
    HWND        ed_normalized;       /* and as it will be stored */
    HWND        ed_locked;           /* why the size fields are disabled */
    HWND        ed_real;             /* the window's real size, measured now */
    HWND        ed_pin;              /* "pin to this screen" checkbox */
    HWND        ed_pin_text;         /* what the preset is pinned to */
    BOOL        ed_locked_size;      /* the preset being edited has lock_size */
    HWND        ed_preview;
    HWND        ed_save;
    HWND        ed_cancel;

    /* Placeholder text, for pages that have nothing of their own yet. One
     * control per page id; the pages that arrived in stage 4.5 are skipped
     * when they are created (see create_pages). */
    HWND        todo[PAGE_COUNT];

    /* The two pages that own their controls, so that this file does not grow
     * a set of fields per setting. Their visibility is still decided here, in
     * apply_page() - the page modules only show and hide what they own. */
    PageSettings *settings_page;
    PageHelp     *help_page;

    HACCEL      accel;
    HFONT       font;
    HFONT       section_font;      /* a little larger, for the list headings */
    HICON       icon;
    HINSTANCE   hinst;

    MainWindowCmdFn on_command;
    void           *user;

    /* Displayed rows. A copy of what the list needs, not of the Workspace
     * itself - see MwWorkspaceRow above. */
    MwWorkspaceRow rows[WS_MAX];
    int         row_count;

    char        active_id[WS_ID_MAX];
    char        selected_id[WS_ID_MAX];

    BOOL        busy;
};

/* ------------------------------------------------------------------ */
/* Accelerators                                                        */
/* ------------------------------------------------------------------ */

/*
 * The keyboard path.
 *
 * With no menu bar there is nothing else that could turn a key into a
 * command, so this table is the only one. The commands are the same ids the
 * menu used, so every path still lands in the same WM_COMMAND handler.
 *
 * Enter (Apply) is deliberately absent: the ListView already handles it, and
 * a global Enter would steal the key from the coordinate fields that arrive
 * with the window-preset page.
 * Alt+F4 (Exit) is a system key and needs no entry.
 */
static HACCEL build_accel(void)
{
    ACCEL a[4];
    ZeroMemory(a, sizeof(a));

    a[0].fVirt = FVIRTKEY;              a[0].key = VK_F5;     a[0].cmd = IDM_REFRESH;
    a[1].fVirt = FVIRTKEY | FCONTROL;   a[1].key = 'N';       a[1].cmd = IDM_NEW;
    a[2].fVirt = FVIRTKEY;              a[2].key = VK_F2;     a[2].cmd = IDM_RENAME;
    a[3].fVirt = FVIRTKEY;              a[3].key = VK_DELETE; a[3].cmd = IDM_DELETE;

    return CreateAcceleratorTableW(a, (int)(sizeof(a) / sizeof(a[0])));
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int mw_dpi(const MainWindow *mw)
{
    return (mw != NULL && mw->frame.dpi > 0) ? mw->frame.dpi : 96;
}

static int mw_scale(const MainWindow *mw, int value)
{
    return MulDiv(value, mw_dpi(mw), 96);
}

static POINT screen_point(LPARAM lp)
{
    POINT pt;
    pt.x = (int)(short)LOWORD(lp);
    pt.y = (int)(short)HIWORD(lp);
    return pt;
}

static void show_ctrl(HWND h, BOOL show)
{
    if (h != NULL) ShowWindow(h, show ? SW_SHOW : SW_HIDE);
}

/* Defined under "Layout"; a page switch changes which rows are visible and
 * therefore has to re-lay out immediately. */
static void layout_children(MainWindow *mw);

/* Defined under "Overview selections"; the window procedure records a new
 * selection the moment a row changes. Both preset lists share one selection,
 * so the list is passed in. */
static void remember_window_selection(MainWindow *mw);
static void remember_preset_selection(MainWindow *mw, HWND list);
static const MwPresetRow *find_preset_row(const MainWindow *mw, const char *id);

/*
 * Lay out a row of buttons across `avail` pixels.
 *
 * Buttons SHRINK when the window is too narrow for their design width. The
 * alternative is what happened before: a row that ran off the right edge, with
 * the last button half-drawn and unclickable. They never grow past `want`, so a
 * wide window still looks deliberate rather than stretched.
 *
 * NULL entries are skipped but still consume their slot, which keeps the
 * remaining buttons in their intended order.
 */
static void layout_button_row(const HWND *btns, int n, int x, int y, int avail,
                              int want, int gap, int height)
{
    if (btns == NULL || n <= 0) return;

    const int room = avail - gap * (n - 1);
    if (room <= 0) return;               /* no sensible layout at this size */

    int w = room / n;
    if (w > want) w = want;
    if (w < 24) w = 24;                  /* below this nothing is readable */

    int bx = x;
    for (int i = 0; i < n; ++i) {
        if (btns[i] != NULL) {
            MoveWindow(btns[i], bx, y, w, height, TRUE);
        }
        bx += w + gap;
    }
}

/* Defined under "The hand-editing panel". */
static void show_editor(MainWindow *mw, BOOL show);
static BOOL editor_may_close(MainWindow *mw);
static void editor_close(MainWindow *mw);

/* Defined near mainwindow_set_status; the window list feeds both. */
static void fill_target_combo(MainWindow *mw);
static void fill_monitor_combo(MainWindow *mw);

/* Defined beside fill_monitor_combo; the editor's refresh calls it long before
 * that point, because it decides which screen shape the preview is drawn on. */
static BOOL editor_preview_monitor(const MainWindow *mw, int *out_w, int *out_h);

/* Defined with the list helpers; the layout pass writes the heading under the
 * count it just computed. */
static void set_section_text(HWND heading, StrId format, int count);

/* Page name shown in the title row. */
static StrId page_title_id(PageId page)
{
    switch (page) {
    case PAGE_WINDOWS:       return STR_NAV_WINDOWS;
    case PAGE_CREATE_PRESET: return STR_PAGE_CREATE_TITLE;
    case PAGE_WORKSPACES:    return STR_NAV_WORKSPACES;
    case PAGE_SETTINGS:      return STR_NAV_SETTINGS;
    case PAGE_HELP:          return STR_NAV_HELP;
    case PAGE_OVERVIEW:
    default:                 return STR_APP_TITLE;
    }
}

/* ------------------------------------------------------------------ */
/* List helpers                                                        */
/* ------------------------------------------------------------------ */

/*
 * Fill the ListView from mw->rows.
 *
 * The list is rebuilt wholesale rather than diffed. With at most a few dozen
 * workspaces the cost is irrelevant, and rebuilding cannot leave a stale row
 * behind - which is the bug a diffing implementation would eventually have.
 */
static void list_reload(MainWindow *mw)
{
    if (mw->list == NULL) return;

    SendMessageW(mw->list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(mw->list);

    for (int i = 0; i < mw->row_count; ++i) {
        const MwWorkspaceRow *w = &mw->rows[i];

        /*
         * The name arrives as UTF-8 - that is what the configuration file
         * holds - and %hs would decode it with the system code page instead.
         * A workspace called "演示工作区" then shows up as mojibake. Measured;
         * str_to_wide is the one sanctioned crossing point (strutil.h).
         */
        wchar_t wname[WS_NAME_MAX];
        if (!str_to_wide(w->name, wname, WS_NAME_MAX)) wname[0] = L'\0';

        /* The first column carries a marker when this workspace is the
         * active one - clearer than a separate column that is usually
         * empty. */
        wchar_t name[WS_NAME_MAX + 8];
        if (mw->active_id[0] && _stricmp(w->id, mw->active_id) == 0) {
            _snwprintf_s(name, WS_NAME_MAX + 8, _TRUNCATE, L"\u25CF  %ls", wname);
        } else {
            _snwprintf_s(name, WS_NAME_MAX + 8, _TRUNCATE, L"     %ls", wname);
        }

        LVITEMW item;
        ZeroMemory(&item, sizeof(item));
        item.mask     = LVIF_TEXT;
        item.iItem    = i;
        item.pszText  = name;
        const int row = (int)SendMessageW(mw->list, LVM_INSERTITEMW, 0, (LPARAM)&item);
        if (row < 0) continue;

        wchar_t apps[32];
        _snwprintf_s(apps, 32, _TRUNCATE, L"%d", w->slot_count);
        ListView_SetItemText(mw->list, row, 1, apps);

        wchar_t idbuf[WS_ID_MAX];
        str_to_wide(w->id, idbuf, WS_ID_MAX);
        ListView_SetItemText(mw->list, row, 2, idbuf);
    }

    SendMessageW(mw->list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(mw->list, NULL, TRUE);
}

static int list_selected_row(const MainWindow *mw)
{
    if (mw->list == NULL) return -1;
    return ListView_GetNextItem(mw->list, -1, LVNI_SELECTED);
}

static void remember_selection(MainWindow *mw)
{
    const int row = list_selected_row(mw);
    if (row < 0 || row >= mw->row_count) {
        mw->selected_id[0] = '\0';
        return;
    }
    strncpy_s(mw->selected_id, sizeof(mw->selected_id),
              mw->rows[row].id, _TRUNCATE);
}

/* Select a row by id. Used to keep the selection across a reload. */
static void select_by_id(MainWindow *mw, const char *id)
{
    if (mw->list == NULL || id == NULL || id[0] == '\0') return;

    for (int i = 0; i < mw->row_count; ++i) {
        if (_stricmp(mw->rows[i].id, id) != 0) continue;

        LVITEMW item;
        ZeroMemory(&item, sizeof(item));
        item.mask    = LVIF_STATE;
        item.iItem   = i;
        item.state   = LVIS_SELECTED | LVIS_FOCUSED;
        item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(mw->list, LVM_SETITEMSTATE, i, (LPARAM)&item);
        SendMessageW(mw->list, LVM_ENSUREVISIBLE, i, FALSE);
        remember_selection(mw);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Pages                                                               */
/* ------------------------------------------------------------------ */

/*
 * Show the page on top of the stack and hide every other one.
 *
 * Visibility is handled per control rather than per container window: see the
 * note at the top of this file about ListView notifications.
 */
static void apply_page(MainWindow *mw)
{
    if (mw == NULL || mw->hwnd == NULL) return;

    const PageId cur  = nav_current(&mw->nav);
    const BOOL   root = nav_at_root(&mw->nav);

    /* Overview page: the two stacked lists, their headings and the action row. */
    const BOOL overview = (cur == PAGE_OVERVIEW);
    show_ctrl(mw->win_list, overview);
    show_ctrl(mw->preset_list, overview);
    show_ctrl(mw->sec_windows, overview);
    show_ctrl(mw->sec_presets, overview);
    show_ctrl(mw->btn_anchor, overview);
    show_ctrl(mw->btn_move_monitor, overview);
    show_ctrl(mw->btn_delete_preset, overview);
    show_ctrl(mw->btn_apply_all, overview);

    /* [Refresh] belongs to the overview's action row: it redraws what that page
     * shows, and offering it everywhere let a click from a sub-page act on
     * lists belonging to a page that was not on screen. */
    show_ctrl(mw->nav_btn[NAV_IDX_REFRESH], overview);

    /* Workspace page: the workspace list and its action row. It used to live
     * on the overview; batch A3 moved it here, where "workspace presets"
     * actually belongs. */
    const BOOL ws_page = (cur == PAGE_WORKSPACES);
    show_ctrl(mw->list, ws_page);
    show_ctrl(mw->sec_slots, ws_page);
    show_ctrl(mw->slot_list, ws_page);
    for (int i = 0; i < 8; ++i) show_ctrl(mw->buttons[i], ws_page);

    /* Window-preset page: its list, the action row, and the editor (which only
     * appears on this page and only while it is open). */
    const BOOL preset_page = (cur == PAGE_WINDOWS);
    show_ctrl(mw->preset_page_list, preset_page);
    show_ctrl(mw->btn_edit_preset, preset_page);
    show_ctrl(mw->btn_create_preset, preset_page);
    show_ctrl(mw->btn_delete_preset2, preset_page);
    /* Create-preset picker page: a snapshot taken when [Create preset] was
     * pressed. */
    const BOOL create_page = (cur == PAGE_CREATE_PRESET);
    show_ctrl(mw->create_list, create_page);
    show_ctrl(mw->create_hint, create_page);
    show_ctrl(mw->btn_do_create, create_page);
    show_ctrl(mw->btn_more, create_page);
    show_ctrl(mw->btn_create_cancel, create_page);

    show_editor(mw, preset_page && mw->ed_open);

    /* Settings and the user guide own their controls; they are only asked to
     * show or hide them. The decision stays here. */
    page_settings_show(mw->settings_page, cur == PAGE_SETTINGS);
    page_help_show(mw->help_page, cur == PAGE_HELP);

    /* Placeholder text belongs only to pages that have nothing of their own
     * yet. Every page that gained real content in stage 4.5 must be added to
     * this list, or its placeholder draws on top of it.
     *
     * Stage 5 emptied the list: the workspace page was the last one. */
    for (int p = 0; p < PAGE_COUNT; ++p) {
        if (p == PAGE_OVERVIEW || p == PAGE_WINDOWS || p == PAGE_CREATE_PRESET ||
            p == PAGE_SETTINGS || p == PAGE_HELP || p == PAGE_WORKSPACES) {
            continue;
        }
        show_ctrl(mw->todo[p], cur == (PageId)p);
    }

    /* Page title row. */
    show_ctrl(mw->back_btn, !root);
    show_ctrl(mw->page_title, !root);
    if (!root) {
        SetWindowTextW(mw->page_title, i18n_text(page_title_id(cur)));
    }

    layout_children(mw);
}

/*
 * Leaving the settings page abandons whatever the user changed on it.
 *
 * "Walked away" and "pressed Cancel" are the same thing here, so that a value
 * seen on the page is never a value that was silently half-applied. The
 * settings themselves live in main.c and are only written when [Apply] is
 * pressed - this only puts the CONTROLS back.
 *
 * Called before every navigation, from either direction. It is harmless on the
 * other pages: putting the controls back to the values they already hold is
 * what it does.
 */
static void settings_discard_pending(MainWindow *mw)
{
    if (mw->settings_page != NULL) page_settings_revert(mw->settings_page);
}

/* Navigate to a top-level page.
 *
 * nav_set(), not nav_push(): the strip says "show me this page". Pushing made
 * window-presets -> workspace-presets -> window-presets leave a trail of
 * entries, so one Back no longer reached the overview and the stack grew with
 * every click.
 *
 * Leaving the page abandons whatever is in the editor, so it asks first when
 * there is something to lose. */
static void nav_go(MainWindow *mw, PageId page)
{
    if (!editor_may_close(mw)) return;

    settings_discard_pending(mw);

    nav_set(&mw->nav, page);
    editor_close(mw);
    apply_page(mw);
}

static void nav_back(MainWindow *mw)
{
    if (!editor_may_close(mw)) return;

    settings_discard_pending(mw);

    if (nav_pop(&mw->nav)) {
        editor_close(mw);
        apply_page(mw);
    }
}

/* ------------------------------------------------------------------ */
/* The hand-editing panel                                              */
/* ------------------------------------------------------------------ */

/* Show or hide every control that belongs to the editor. */
static void show_editor(MainWindow *mw, BOOL show)
{
    show_ctrl(mw->lbl_target, show);
    show_ctrl(mw->cb_target, show);
    show_ctrl(mw->lbl_monitor, show);
    show_ctrl(mw->cb_monitor, show);
    show_ctrl(mw->lbl_position, show);
    show_ctrl(mw->lbl_size, show);
    show_ctrl(mw->ed_target, show);
    for (int i = 0; i < EDIT_FIELD_COUNT; ++i) {
        show_ctrl(mw->ed_label[i], show);
        show_ctrl(mw->ed_field[i], show);
    }
    show_ctrl(mw->ed_pixels, show);
    show_ctrl(mw->ed_normalized, show);
    /* The explanation only exists while the preset it explains is open: an
     * empty line under the size fields would be a mystery, not a hint. */
    show_ctrl(mw->ed_locked, show && mw->ed_locked_size);
    show_ctrl(mw->ed_real, show);
    show_ctrl(mw->ed_pin, show);
    show_ctrl(mw->ed_pin_text, show);
    show_ctrl(mw->ed_preview, show);
    show_ctrl(mw->ed_save, show);
    show_ctrl(mw->ed_cancel, show);
}

/* Put a value into one of the four fields. */
static void set_field(MainWindow *mw, int which, double frac)
{
    if (which < 0 || which >= EDIT_FIELD_COUNT || mw->ed_field[which] == NULL) return;

    wchar_t buf[32];
    uiformat_format_percent(frac, buf, 32);
    SetWindowTextW(mw->ed_field[which], buf);
}

/*
 * Read the four fields back.
 *
 * Returns FALSE when ANY field is unusable, leaving the caller to report it.
 * A partially-valid set is not "mostly right": three good numbers and one typo
 * would place the window somewhere the user never described.
 */
static BOOL read_fields(MainWindow *mw, double out[EDIT_FIELD_COUNT])
{
    for (int i = 0; i < EDIT_FIELD_COUNT; ++i) {
        if (mw->ed_field[i] == NULL) return FALSE;

        wchar_t buf[64];
        buf[0] = L'\0';
        GetWindowTextW(mw->ed_field[i], buf, (int)(sizeof(buf) / sizeof(buf[0])));

        if (!uiformat_parse_percent(buf, &out[i])) return FALSE;
    }
    return TRUE;
}

/*
 * Redraw the readouts and the preview from what is currently typed.
 *
 * Called on every keystroke (EN_CHANGE). When the text is not usable yet - the
 * user is halfway through typing "23.5" and it currently reads "23." - the
 * readouts keep their previous text rather than flashing an error at someone
 * who is still typing.
 */
/*
 * The window's real pixel size, or FALSE when there is nothing to measure -
 * no target picked in the box, or it closed since the editor was opened.
 *
 * Measured on every refresh rather than remembered: the user can switch the
 * target box while the editor is open, and a stale number would look exactly
 * like a fresh measurement.
 */
static BOOL editor_real_size(const MainWindow *mw, int *out_w, int *out_h)
{
    if (out_w != NULL) *out_w = 0;
    if (out_h != NULL) *out_h = 0;
    if (mw == NULL) return FALSE;

    const unsigned long long target = mainwindow_target_window(mw);
    if (target == 0) return FALSE;

    RECT wr;
    ZeroMemory(&wr, sizeof(wr));
    if (!GetWindowRect((HWND)(ULONG_PTR)target, &wr)) return FALSE;

    const int w = (int)(wr.right - wr.left);
    const int h = (int)(wr.bottom - wr.top);
    if (w <= 0 || h <= 0) return FALSE;

    if (out_w != NULL) *out_w = w;
    if (out_h != NULL) *out_h = h;
    return TRUE;
}

static void editor_refresh(MainWindow *mw)
{
    if (!mw->ed_open) return;

    double v[EDIT_FIELD_COUNT];
    if (!read_fields(mw, v)) {
        /* Half-typed text still counts as a change: leaving the editor now
         * would silently drop it. */
        mw->ed_dirty = TRUE;
        return;
    }

    /* Same tolerance the save path uses when it compares against the preset. */
    mw->ed_dirty = FALSE;
    for (int i = 0; i < EDIT_FIELD_COUNT; ++i) {
        if (fabs(v[i] - mw->ed_orig[i]) > 1.0e-6) {
            mw->ed_dirty = TRUE;
            break;
        }
    }

    /*
     * Which screen shape these fractions are read against.
     *
     * The window's own monitor by default - that is where the numbers came
     * from, and a preset is applied on whichever screen its window turns up on.
     * The monitor box can point the PREVIEW at another screen instead; that
     * changes what is drawn here and nothing else (see fill_monitor_combo).
     */
    int mon_w = 2560, mon_h = 1600;
    if (!editor_preview_monitor(mw, &mon_w, &mon_h)) {
        const HMONITOR mon = MonitorFromWindow(mw->hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        ZeroMemory(&mi, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
            mon_w = mi.rcMonitor.right - mi.rcMonitor.left;
            mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        }
    }

    if (mw->ed_pixels != NULL) {
        wchar_t buf[160];
        _snwprintf_s(buf, 160, _TRUNCATE, i18n_text(STR_EDIT_PIXELS),
                     (int)(v[EDIT_FIELD_X] * mon_w + 0.5),
                     (int)(v[EDIT_FIELD_Y] * mon_h + 0.5),
                     (int)(v[EDIT_FIELD_W] * mon_w + 0.5),
                     (int)(v[EDIT_FIELD_H] * mon_h + 0.5));
        SetWindowTextW(mw->ed_pixels, buf);
    }

    /*
     * What the window is RIGHT NOW, in pixels and as a share of the screen the
     * preview is drawn against.
     *
     * Not the same question as ed_pixels above: that one converts the typed
     * fractions, this one measures the actual window. For a window that
     * refuses to resize the two differ by the most, and that gap is exactly
     * what the user reported as "setting the size does nothing" (task B).
     */
    if (mw->ed_real != NULL) {
        int real_w = 0, real_h = 0;
        wchar_t buf[192];

        if (!editor_real_size(mw, &real_w, &real_h)) {
            /* Said, not remembered: leaving the previous numbers up would read
             * as a fresh measurement of a window that is no longer there. */
            _snwprintf_s(buf, 192, _TRUNCATE, L"%ls", i18n_text(STR_EDIT_REAL_NONE));
        } else {
            /* Both parts are formatted here: an i18n template may only carry
             * %ls and %d (src/ui/i18n.h), and a literal percent sign has to be
             * written as %% before it reaches one. */
            wchar_t size[48];
            wchar_t pct[64];
            _snwprintf_s(size, 48, _TRUNCATE, L"%dx%d", real_w, real_h);
            /* The share arithmetic lives in uiformat_share_percent(): pure, so
             * it is unit-tested rather than only visible on screen. */
            _snwprintf_s(pct, 64, _TRUNCATE, L"%d%% x %d%%",
                         uiformat_share_percent(real_w, mon_w),
                         uiformat_share_percent(real_h, mon_h));
            _snwprintf_s(buf, 192, _TRUNCATE, i18n_text(STR_EDIT_REAL_FMT), size, pct);
        }
        SetWindowTextW(mw->ed_real, buf);
    }

    if (mw->ed_normalized != NULL) {
        /* The numbers are formatted first: an i18n template may only contain
         * %ls and %d (see src/ui/i18n.h), so a %.4f belongs here, not in the
         * translation table. tests/t14_i18nfmt enforces that. */
        wchar_t vals[96];
        _snwprintf_s(vals, 96, _TRUNCATE, L"%.4f,%.4f %.4fx%.4f",
                     v[EDIT_FIELD_X], v[EDIT_FIELD_Y],
                     v[EDIT_FIELD_W], v[EDIT_FIELD_H]);

        wchar_t buf[192];
        _snwprintf_s(buf, 192, _TRUNCATE, i18n_text(STR_EDIT_NORMALIZED), vals);
        SetWindowTextW(mw->ed_normalized, buf);
    }

    if (mw->ed_preview != NULL) {
        preview_set_monitor(mw->ed_preview, mon_w, mon_h);
        preview_set_zone(mw->ed_preview,
                         v[EDIT_FIELD_X], v[EDIT_FIELD_Y],
                         v[EDIT_FIELD_W], v[EDIT_FIELD_H]);

        /*
         * A window that cannot be resized keeps its own size, so the preview
         * gets a second rectangle for it: the zone above says where it goes,
         * this outline says how big it will actually be. 0 turns the second
         * rectangle off, which is the normal case.
         */
        double rw = 0.0, rh = 0.0;
        int real_w = 0, real_h = 0;
        if (mw->ed_locked_size && editor_real_size(mw, &real_w, &real_h) &&
            mon_w > 0 && mon_h > 0) {
            rw = (double)real_w / (double)mon_w;
            rh = (double)real_h / (double)mon_h;
        }
        preview_set_real_size(mw->ed_preview, rw, rh);
    }
}

/*
 * What the pin row says underneath the checkbox: the screen it currently
 * means, or a plain "not pinned". Readable without knowing the checkbox
 * convention, and it follows the monitor box - changing that box changes what
 * a tick would mean.
 */
static void editor_pin_text_update(MainWindow *mw)
{
    if (mw == NULL || mw->ed_pin_text == NULL) return;

    wchar_t buf[224];

    if (mw->ed_pin == NULL ||
        SendMessageW(mw->ed_pin, BM_GETCHECK, 0, 0) != BST_CHECKED) {
        _snwprintf_s(buf, 224, _TRUNCATE, L"%ls", i18n_text(STR_PIN_NONE));
        SetWindowTextW(mw->ed_pin_text, buf);
        return;
    }

    /* The same box the preview is drawn against, and the same decision
     * mainwindow_selected_monitor_index() makes for the [Move to the selected
     * monitor] button - one meaning for "the screen chosen up there". */
    MonitorFingerprint mons[WAC_MON_MAX];
    const int n = monitor_enumerate(mons, WAC_MON_MAX);
    const int mi = mainwindow_selected_monitor_index(mw);

    if (mi < 0 || mi >= n) {
        _snwprintf_s(buf, 224, _TRUNCATE, L"%ls", i18n_text(STR_PIN_NO_MONITOR));
    } else {
        const wchar_t *name = (mons[mi].friendly_name[0] != L'\0')
                                  ? mons[mi].friendly_name
                                  : mons[mi].device_name;

        /* Formatted here: an i18n template may only carry %ls and %d. */
        wchar_t label[160];
        _snwprintf_s(label, 160, _TRUNCATE, L"%ls %dx%d",
                     name, mons[mi].width, mons[mi].height);
        _snwprintf_s(buf, 224, _TRUNCATE, i18n_text(STR_PIN_SET_FMT), label);
    }

    SetWindowTextW(mw->ed_pin_text, buf);
}

/* Load a preset into the editor and open it. */
static void editor_open(MainWindow *mw, const MwPresetRow *row)
{
    if (mw == NULL || row == NULL) return;

    strncpy_s(mw->ed_rule_id, sizeof(mw->ed_rule_id), row->id, _TRUNCATE);

    if (mw->ed_target != NULL) {
        wchar_t buf[MW_TEXT_MAX + 32];
        _snwprintf_s(buf, MW_TEXT_MAX + 32, _TRUNCATE,
                     i18n_text(STR_EDIT_TARGET_FMT), row->label);
        SetWindowTextW(mw->ed_target, buf);
    }

    set_field(mw, EDIT_FIELD_X, row->x);
    set_field(mw, EDIT_FIELD_Y, row->y);
    set_field(mw, EDIT_FIELD_W, row->w);
    set_field(mw, EDIT_FIELD_H, row->h);

    /*
     * A window that refuses to be resized: the two size fields go dead and the
     * reason is written under them.
     *
     * The VALUES are left alone on purpose. They are still what the preset
     * stores and what the placement reads; if the window ever becomes
     * resizable the numbers are still there, and erasing them would be
     * destroying data the user never asked to change (task B).
     */
    mw->ed_locked_size = (row->locked_size != FALSE) ? TRUE : FALSE;

    if (mw->ed_field[EDIT_FIELD_W] != NULL) {
        EnableWindow(mw->ed_field[EDIT_FIELD_W], !mw->ed_locked_size);
    }
    if (mw->ed_field[EDIT_FIELD_H] != NULL) {
        EnableWindow(mw->ed_field[EDIT_FIELD_H], !mw->ed_locked_size);
    }
    if (mw->ed_locked != NULL) {
        SetWindowTextW(mw->ed_locked,
                       mw->ed_locked_size ? i18n_text(STR_EDIT_LOCKED) : L"");
    }

    /*
     * The pin. Ticked means "this preset names a screen"; the screen itself
     * comes from the monitor box and is read at SAVE time, so changing that
     * box after ticking changes what the tick means - which is what the row's
     * own text keeps saying.
     */
    if (mw->ed_pin != NULL) {
        SendMessageW(mw->ed_pin, BM_SETCHECK,
                     row->pinned ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    editor_pin_text_update(mw);

    /* Remember what was loaded, so "has the user changed anything" has an
     * answer that does not depend on the text fields. */
    mw->ed_orig[EDIT_FIELD_X] = row->x;
    mw->ed_orig[EDIT_FIELD_Y] = row->y;
    mw->ed_orig[EDIT_FIELD_W] = row->w;
    mw->ed_orig[EDIT_FIELD_H] = row->h;
    mw->ed_dirty = FALSE;

    mw->ed_open = TRUE;

    /* Fill the readouts and the preview from the values just loaded. */
    editor_refresh(mw);

    /*
     * apply_page(), not show_editor() + layout_children().
     *
     * Every control's visibility is decided in ONE place. Showing the editor
     * directly here meant that opening it from a page that does not own it -
     * which is exactly what happened when Back landed on the overview - drew
     * the panel on top of that page's controls. apply_page() shows the editor
     * only when the current page is the preset page.
     */
    apply_page(mw);
}

/*
 * Ask before throwing away typed values.
 *
 * Returns TRUE when it is safe to close the editor: either nothing was changed,
 * or the user agreed to lose it. Only a real change prompts - opening a preset
 * and pressing Cancel must not nag.
 */
static BOOL editor_may_close(MainWindow *mw)
{
    /* A session that is ending does not wait for an answer - see
     * mainwindow_set_prompt_suppressed(). Answering "yes, discard" here is the
     * only useful thing to do, and main.c has already saved everything it
     * owns. */
    if (mw->no_prompt) return TRUE;

    if (mw == NULL || !mw->ed_open || !mw->ed_dirty) return TRUE;

    return MessageBoxW(mw->hwnd, i18n_text(STR_MSG_DISCARD_EDITS),
                       i18n_text(STR_ERR_TITLE),
                       MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES
               ? TRUE : FALSE;
}

static void editor_close(MainWindow *mw)
{
    if (mw == NULL || !mw->ed_open) return;

    mw->ed_open = FALSE;
    mw->ed_rule_id[0] = '\0';

    /* Same rule as opening: visibility is apply_page()'s decision, so the
     * panel cannot be left on screen by a path that forgot to hide it. */
    apply_page(mw);
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

static void layout_children(MainWindow *mw)
{
    if (mw == NULL || mw->hwnd == NULL) return;

    RECT rc;
    GetClientRect(mw->hwnd, &rc);

    const int margin  = mw_scale(mw, MARGIN);
    const int gap     = mw_scale(mw, NAV_GAP);
    const int nav_w   = mw_scale(mw, NAV_BTN_W);
    const int nav_h   = mw_scale(mw, NAV_BTN_H);
    const int title_h = mw_scale(mw, TITLE_H);
    const int back_w  = mw_scale(mw, BACK_W);
    const int btn_w   = mw_scale(mw, BTN_W);
    const int btn_h   = mw_scale(mw, BTN_H);

    int top = mw->frame.height + margin;

    /* ---- navigation strip ----------------------------------------
     *
     * Three entries: refresh moved to the overview's action row. Refreshing is
     * a PAGE action, and offering it from every page is what let a click from a
     * sub-page redraw lists that belonged to a page which was not showing.
     * ------------------------------------------------------------- */
    {
        HWND row[NAV_COUNT];
        int n = 0;
        for (int i = 0; i < NAV_COUNT; ++i) {
            if (i == NAV_IDX_REFRESH) continue;
            row[n++] = mw->nav_btn[i];
        }
        layout_button_row(row, n, margin, top, rc.right - 2 * margin,
                          nav_w, gap, nav_h);
    }
    top += nav_h;

    /* ---- page title row (sub-pages only) ------------------------- */
    if (!nav_at_root(&mw->nav)) {
        top += gap;
        const int line_h = title_h;
        MoveWindow(mw->back_btn, margin, top, back_w, line_h, TRUE);
        MoveWindow(mw->page_title, margin + back_w + gap, top,
                   (rc.right - 2 * margin - back_w - gap > 0)
                       ? (rc.right - 2 * margin - back_w - gap) : 0,
                   line_h, TRUE);
        top += line_h;
    }

    /* ---- status bar height --------------------------------------- */
    int status_h = 0;
    if (mw->status != NULL) {
        RECT sr;
        GetWindowRect(mw->status, &sr);
        status_h = sr.bottom - sr.top;
    }

    const int content_top = top + gap;
    const int content_h   = rc.bottom - status_h - content_top - margin;
    if (content_h <= 0) return;      /* window too small to lay out */

    const int content_w = rc.right - 2 * margin;

    /* ---- overview page: two stacked lists + action row ----------- */
    {
        const int sec_h = mw_scale(mw, SECTION_H);   /* heading strip */
        const int avail = content_h - 2 * sec_h - 3 * gap - btn_h - gap;
        const int top_h = (avail > 0) ? (avail / 2) : 0;
        const int bot_h = (avail > 0) ? (avail - top_h) : 0;

        int y = content_top;
        if (mw->sec_windows != NULL) MoveWindow(mw->sec_windows, margin, y, content_w, sec_h, TRUE);
        y += sec_h + gap;
        if (mw->win_list != NULL) {
            MoveWindow(mw->win_list, margin, y, content_w, top_h, TRUE);
            fit_columns(mw->win_list, kWindowCols,
                        (int)(sizeof(kWindowCols) / sizeof(kWindowCols[0])), content_w);
        }
        y += top_h + gap;
        if (mw->sec_presets != NULL) MoveWindow(mw->sec_presets, margin, y, content_w, sec_h, TRUE);
        y += sec_h + gap;
        if (mw->preset_list != NULL) {
            MoveWindow(mw->preset_list, margin, y, content_w, bot_h, TRUE);
            fit_columns(mw->preset_list, kPresetCols,
                        (int)(sizeof(kPresetCols) / sizeof(kPresetCols[0])), content_w);
        }

        /* Action row: anchor, delete, apply-all, and refresh - which moved here
         * from the navigation strip, because refreshing redraws the lists on
         * THIS page. */
        {
            const int row_y = content_top + content_h - btn_h;
            const HWND row[5] = {
                mw->btn_anchor,
                /* Right after the anchor button, as the user asked; same
                 * interaction - pick a preset in the list below, then press. */
                mw->btn_move_monitor,
                mw->btn_delete_preset, mw->btn_apply_all,
                mw->nav_btn[NAV_IDX_REFRESH]
            };
            layout_button_row(row, 5, margin, row_y, content_w, btn_w * 2, gap, btn_h);
        }
    }

    /* ---- window-preset page: list, action row, editor ------------ */
    {
        /* While the editor is open it takes most of the page: the list shrinks
         * to a third so the numbers and the picture have room. */
        const int list_h = mw->ed_open
                             ? (content_h / 3)
                             : (content_h - btn_h - gap);

        if (mw->preset_page_list != NULL) {
            MoveWindow(mw->preset_page_list, margin, content_top, content_w, list_h, TRUE);
            fit_columns(mw->preset_page_list, kPresetCols,
                        (int)(sizeof(kPresetCols) / sizeof(kPresetCols[0])), content_w);
        }

        /* Action row under the list: create / edit / delete. */
        {
            const int by = content_top + list_h + gap;
            const HWND row[3] = {
                mw->btn_create_preset, mw->btn_edit_preset, mw->btn_delete_preset2
            };
            layout_button_row(row, 3, margin, by, content_w, btn_w * 2, gap, btn_h);
        }

        if (mw->ed_open) {
            const int dpi     = mw_dpi(mw);
            const int row_h   = nav_h;
            const int lbl_w   = MulDiv(72, dpi, 96);
            const int field_w = MulDiv(72, dpi, 96);
            const int gap2    = gap * 2;

            int y = content_top + list_h + gap + btn_h + gap;

            /* Row 1: which window, and which monitor. */
            {
                const int combo_w = (content_w - 2 * lbl_w - 2 * gap2) / 2;
                int cx = margin;

                if (mw->lbl_target != NULL) MoveWindow(mw->lbl_target, cx, y, lbl_w, row_h, TRUE);
                cx += lbl_w;
                if (mw->cb_target != NULL)  MoveWindow(mw->cb_target, cx, y, combo_w, row_h * 8, TRUE);
                cx += combo_w + gap2;
                if (mw->lbl_monitor != NULL) MoveWindow(mw->lbl_monitor, cx, y, lbl_w, row_h, TRUE);
                cx += lbl_w;
                if (mw->cb_monitor != NULL)  MoveWindow(mw->cb_monitor, cx, y, combo_w, row_h * 8, TRUE);
                y += row_h + gap;
            }

            /*
             * Two columns from here on.
             *
             * Left: position on one row, size on the NEXT row (not beside it),
             * which leaves the right-hand column wide enough for a picture
             * worth looking at. Right: the preview.
             */
            const int left_w  = (content_w * 45) / 100;
            const int right_x = margin + left_w + gap2;
            const int right_w = content_w - left_w - gap2;

            int ly = y;
            int fx = margin + lbl_w;

            if (mw->lbl_position != NULL) MoveWindow(mw->lbl_position, margin, ly, lbl_w, row_h, TRUE);
            if (mw->ed_label[EDIT_FIELD_X] != NULL) MoveWindow(mw->ed_label[EDIT_FIELD_X], fx, ly, MulDiv(20, dpi, 96), row_h, TRUE);
            fx += MulDiv(20, dpi, 96);
            if (mw->ed_field[EDIT_FIELD_X] != NULL) MoveWindow(mw->ed_field[EDIT_FIELD_X], fx, ly, field_w, row_h, TRUE);
            fx += field_w + gap;
            if (mw->ed_label[EDIT_FIELD_Y] != NULL) MoveWindow(mw->ed_label[EDIT_FIELD_Y], fx, ly, MulDiv(20, dpi, 96), row_h, TRUE);
            fx += MulDiv(20, dpi, 96);
            if (mw->ed_field[EDIT_FIELD_Y] != NULL) MoveWindow(mw->ed_field[EDIT_FIELD_Y], fx, ly, field_w, row_h, TRUE);
            ly += row_h + gap;

            fx = margin + lbl_w;
            if (mw->lbl_size != NULL) MoveWindow(mw->lbl_size, margin, ly, lbl_w, row_h, TRUE);
            if (mw->ed_label[EDIT_FIELD_W] != NULL) MoveWindow(mw->ed_label[EDIT_FIELD_W], fx, ly, MulDiv(28, dpi, 96), row_h, TRUE);
            fx += MulDiv(28, dpi, 96);
            if (mw->ed_field[EDIT_FIELD_W] != NULL) MoveWindow(mw->ed_field[EDIT_FIELD_W], fx, ly, field_w, row_h, TRUE);
            fx += field_w + gap;
            if (mw->ed_label[EDIT_FIELD_H] != NULL) MoveWindow(mw->ed_label[EDIT_FIELD_H], fx, ly, MulDiv(28, dpi, 96), row_h, TRUE);
            fx += MulDiv(28, dpi, 96);
            if (mw->ed_field[EDIT_FIELD_H] != NULL) MoveWindow(mw->ed_field[EDIT_FIELD_H], fx, ly, field_w, row_h, TRUE);
            ly += row_h + gap;

            if (mw->ed_locked != NULL)     MoveWindow(mw->ed_locked, margin, ly, left_w, row_h, TRUE);
            ly += row_h;
            if (mw->ed_pixels != NULL)     MoveWindow(mw->ed_pixels, margin, ly, left_w, row_h, TRUE);
            ly += row_h;
            if (mw->ed_normalized != NULL) MoveWindow(mw->ed_normalized, margin, ly, left_w, row_h, TRUE);
            ly += row_h;
            if (mw->ed_real != NULL)       MoveWindow(mw->ed_real, margin, ly, left_w, row_h, TRUE);
            ly += row_h + gap;

            /*
             * The pin: a checkbox plus the screen it currently means. The
             * screen comes from the monitor box at the top of the page, so the
             * row reads as "pin to THAT one" rather than introducing a third
             * way to choose a monitor.
             */
            {
                const int box_w = MulDiv(150, dpi, 96);
                if (mw->ed_pin != NULL) {
                    MoveWindow(mw->ed_pin, margin, ly, box_w, row_h, TRUE);
                }
                if (mw->ed_pin_text != NULL && left_w > box_w + gap) {
                    MoveWindow(mw->ed_pin_text, margin + box_w + gap, ly,
                               left_w - box_w - gap, row_h, TRUE);
                }
            }

            /*
             * The preview container keeps a 16:10 shape whatever the window
             * size - the outer frame is the container, and the monitor is
             * drawn inside it at its OWN aspect ratio by the control. Taking
             * the largest 16:10 rectangle that fits is what makes the picture
             * scale with the window without ever changing proportions.
             */
            {
                const int avail_h = (content_top + content_h - btn_h - gap) - y;
                int pw = right_w;
                int ph = (pw * 10) / 16;

                if (ph > avail_h) {
                    ph = avail_h;
                    pw = (ph * 16) / 10;
                }
                if (pw < 80) pw = 80;
                if (ph < 50) ph = 50;

                if (mw->ed_preview != NULL) {
                    MoveWindow(mw->ed_preview, right_x, y, pw, ph, TRUE);
                }
            }

            const int by = content_top + content_h - btn_h;
            if (mw->ed_save != NULL) {
                MoveWindow(mw->ed_save, margin, by, btn_w, btn_h, TRUE);
            }
            if (mw->ed_cancel != NULL) {
                MoveWindow(mw->ed_cancel, margin + btn_w + gap, by, btn_w, btn_h, TRUE);
            }
        }
    }

    /* ---- create-preset picker page ------------------------------- */
    {
        const int hint_h = nav_h;
        const int list_h = content_h - hint_h - btn_h - gap * 2;

        if (mw->create_list != NULL && list_h > 0) {
            MoveWindow(mw->create_list, margin, content_top, content_w, list_h, TRUE);
            fit_columns(mw->create_list, kWindowCols,
                        (int)(sizeof(kWindowCols) / sizeof(kWindowCols[0])), content_w);
        }
        if (mw->create_hint != NULL) {
            MoveWindow(mw->create_hint, margin, content_top + list_h + gap,
                       content_w, hint_h, TRUE);
        }

        const int by = content_top + content_h - btn_h;
        {
            const HWND row[3] = {
                mw->btn_do_create, mw->btn_more, mw->btn_create_cancel
            };
            layout_button_row(row, 3, margin, by, content_w, btn_w * 2, gap, btn_h);
        }
    }

    /* ---- workspace page: workspace list, its presets, two button rows ----
     *
     * Two stacked lists mirroring the overview page: pick a workspace above,
     * see (and edit) what is in it below. The buttons are split in two rows
     * because there are six of them and they do two different kinds of thing -
     * the top row acts on the programs, the bottom row on the definition.
     * -------------------------------------------------------------------- */
    {
        const int sec_h = mw_scale(mw, SECTION_H);
        const int rows_h = btn_h * 2 + gap;
        const int avail = content_h - sec_h - rows_h - gap * 2;
        const int top_h = (avail > 0) ? (avail / 2) : 0;
        const int bot_h = (avail > 0) ? (avail - top_h) : 0;

        int y = content_top;
        if (mw->list != NULL && top_h > 0) {
            MoveWindow(mw->list, margin, y, content_w, top_h, TRUE);
        }
        y += top_h + gap;

        if (mw->sec_slots != NULL) {
            set_section_text(mw->sec_slots, STR_SEC_SLOTS_FMT, mw->slot_row_count);
            MoveWindow(mw->sec_slots, margin, y, content_w, sec_h, TRUE);
        }
        y += sec_h + gap;

        if (mw->slot_list != NULL && bot_h > 0) {
            MoveWindow(mw->slot_list, margin, y, content_w, bot_h, TRUE);
            fit_columns(mw->slot_list, kSlotCols,
                        (int)(sizeof(kSlotCols) / sizeof(kSlotCols[0])), content_w);
        }

        const int by = content_top + content_h - rows_h;

        {
            /*
             * Two rows of three.
             *
             * [Add selected preset] and [Remove selected preset] USED TO SIT
             * HERE and are gone since 2026-10-09: adding a preset to a
             * workspace acted on the OVERVIEW page's preset selection, which
             * this page neither shows nor can change - so the button could
             * only be trusted by someone who remembered what they had clicked
             * on another page, and nothing on screen showed which preset was
             * about to be added (reported by the user, who also could not
             * compare the slot list before and after).
             *
             * The COMMANDS are kept (MW_CMD_ADD_SLOT / MW_CMD_REMOVE_SLOT and
             * their handlers in main.c): the capability moves into the
             * full-screen workspace editor, where the whole workspace is on
             * screen and the change can be seen as it is made.
             */
            const HWND row[3] = {
                mw->buttons[0],  /* Run */
                mw->buttons[1],  /* Close its applications */
                mw->buttons[5]   /* Edit selected workspace */
            };
            layout_button_row(row, 3, margin, by, content_w, btn_w * 2, gap, btn_h);
        }
        {
            /*
             * [Unbind monitor] sits on the SECOND row deliberately. The first
             * row already fills the width, and layout_button_row() divides
             * whatever it is given by the number of buttons: a sixth button
             * there leaves a gap of a few pixels, which reads as one long
             * strip rather than as separate buttons (seen in a screenshot,
             * not in any assertion).
             * Three on this row stay comfortably apart.
             */
            const HWND row[3] = {
                mw->buttons[3],  /* New workspace */
                mw->buttons[4],  /* Delete workspace */
                mw->buttons[7]   /* Unbind monitor (stage 7) */
            };
            layout_button_row(row, 3, margin, by + btn_h + gap, content_w,
                              btn_w * 2, gap, btn_h);
        }
    }

    /* ---- pages that own their controls ---------------------------
     *
     * Laid out unconditionally: a hidden page still has to be placed, because
     * showing it (apply_page) does not run a layout pass of its own and the
     * window may not have been resized since the page was last visible.
     * ------------------------------------------------------------- */
    if (mw->settings_page != NULL) {
        page_settings_layout(mw->settings_page, margin, content_top, content_w,
                             content_top + content_h);
    }
    if (mw->help_page != NULL) {
        page_help_layout(mw->help_page, margin, content_top, content_w,
                         content_top + content_h);
    }

    /* ---- placeholder pages --------------------------------------- */
    for (int p = 0; p < PAGE_COUNT; ++p) {
        if (p == PAGE_OVERVIEW || mw->todo[p] == NULL) continue;
        MoveWindow(mw->todo[p], margin, content_top, content_w, nav_h, TRUE);
    }

    /* The status bar sizes itself from WM_SIZE, but that message goes to the
     * PARENT and the bar reads the parent's client area when it gets its own -
     * which is not necessarily current during the layout pass. Moving it
     * explicitly removes the race and guarantees it spans the full width.
     * (Measured: it used to come out 22 px narrower than the window, leaving a
     * blank strip along the bottom edge.) */
    if (mw->status != NULL) {
        MoveWindow(mw->status, 0, rc.bottom - status_h, rc.right, status_h, TRUE);
    }
}

/* ------------------------------------------------------------------ */
/* Notification helpers                                                */
/* ------------------------------------------------------------------ */

static void emit(MainWindow *mw, MainWindowCommand cmd)
{
    if (mw->on_command == NULL) return;
    mw->on_command(cmd, mw->selected_id[0] ? mw->selected_id : NULL, mw->user);
}

static void update_button_states(MainWindow *mw)
{
    const BOOL has_selection = (mw->selected_id[0] != '\0') && !mw->busy;

    /*
     * Index order matches mw->buttons[]: Run, Close its apps, (unused),
     * New workspace, Delete workspace, Edit workspace, (unused),
     * Unbind monitor.
     *
     * Entries 2 and 6 have no button since 2026-10-09 - [Add selected preset]
     * and [Remove selected preset] moved into the full-screen workspace editor
     * (see the layout's comment). The table keeps their places so the indices
     * above stay stable, and update_button_states() skips a NULL entry.
     *
     * The enabled state follows from whether an action makes sense right now -
     * the buttons are never left clickable but inert.
     *
     * [Edit selected workspace] opens the full-screen editor, and renaming
     * happens inside it, so there is deliberately no
     * separate rename button here: two buttons doing one job is how a user
     * learns to distrust the labels.
     */

    /*
     * [Unbind monitor] needs more than a selection: the SELECTED SLOT has to
     * be bound, or the button would offer to undo something that was never
     * done. mainwindow_selected_slot() reads the list's own selection and
     * slot_rows[] is the copy rendered into it.
     */
    const int sel_slot = mainwindow_selected_slot(mw);
    const BOOL slot_bound = (sel_slot >= 0 && sel_slot < mw->slot_row_count &&
                             mw->slot_rows[sel_slot].monitor[0] != L'\0');

    const BOOL enabled[8] = {
        has_selection,   /* Run             - needs a selected workspace */
        has_selection,   /* Close its apps  */
        FALSE,           /* (unused)        - no button since 2026-10-09 */
        !mw->busy,       /* New workspace   - available unless busy */
        has_selection,   /* Delete workspace */
        has_selection,   /* Edit workspace  - opens the full-screen editor */
        FALSE,           /* (unused)        - no button since 2026-10-09 */
        has_selection && slot_bound                /* Unbind monitor */
    };

    for (int i = 0; i < 8; ++i) {
        if (mw->buttons[i]) EnableWindow(mw->buttons[i], enabled[i]);
    }
}

/* Repaint just the caption strip. */
static void invalidate_caption(MainWindow *mw)
{
    if (mw == NULL || mw->hwnd == NULL) return;

    RECT cap;
    GetClientRect(mw->hwnd, &cap);
    cap.bottom = mw->frame.height;
    InvalidateRect(mw->hwnd, &cap, FALSE);
}

/* ------------------------------------------------------------------ */
/* Window procedure                                                    */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK mainwnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    MainWindow *mw = (MainWindow *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        mw = (MainWindow *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)mw);
        if (mw) mw->hwnd = hwnd;
        return 0;
    }

    /* ---- self-drawn frame ---------------------------------------- */

    case WM_NCCALCSIZE:
        if (wp == TRUE) {
            /*
             * No non-client area at all: the caption is painted by WM_PAINT,
             * so the client area is normally the whole window.
             *
             * EXCEPT WHEN MAXIMISED. Windows makes the maximised window
             * rectangle one frame thickness larger than the work area on every
             * side - measured at 150% scaling: (-11,-11)-(2571,1611) for a
             * 2560x1600 work area. Letting the client area follow the window
             * would push the visible content past the screen edge and over the
             * taskbar, so it is clamped back to the work area and the extra
             * border ends up off-screen, where it belongs.
             */
            if (mw != NULL && IsZoomed(hwnd)) {
                NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;
                const RECT window_rect = p->rgrc[0];

                const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi;
                ZeroMemory(&mi, sizeof(mi));
                mi.cbSize = sizeof(mi);

                if (GetMonitorInfoW(mon, &mi)) {
                    p->rgrc[0] = frame_maximized_client_rect(&window_rect, &mi.rcWork,
                                                             mw->frame.border);
                }
            }

            /* The client area is about to change; WM_SIZE will not follow, so
             * ask for a layout pass through the queue instead. */
            if (mw != NULL) PostMessageW(hwnd, WM_APP_RELAYOUT, 0, 0);

            return 0;
        }
        break;

    case WM_APP_RELAYOUT:
        if (mw != NULL) layout_children(mw);
        return 0;

    case WM_NCHITTEST: {
        if (mw == NULL) break;
        const POINT pt = screen_point(lp);
        return frame_hit_test(&mw->frame, pt);
    }

    case WM_GETMINMAXINFO:
        if (mw == NULL) break;
        /* Let the system fill in its own defaults first (that is what a
         * MINMAXINFO arriving at a window procedure contains), then limit a
         * maximised window to the work area. */
        DefWindowProcW(hwnd, msg, wp, lp);
        frame_adjust_min_max_info(&mw->frame, (MINMAXINFO *)lp);
        return 0;

    case WM_NCMOUSEMOVE: {
        if (mw == NULL) break;

        const POINT pt  = screen_point(lp);
        const int   btn = frame_button_at(&mw->frame, pt);

        if (frame_set_hot(&mw->frame, btn)) {
            invalidate_caption(mw);
        }

        /* Ask for WM_NCMOUSELEAVE, otherwise a hovered button stays lit after
         * the cursor leaves the window entirely. */
        TRACKMOUSEEVENT tme;
        ZeroMemory(&tme, sizeof(tme));
        tme.cbSize    = sizeof(tme);
        tme.dwFlags   = TME_LEAVE | TME_NONCLIENT;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_NCMOUSELEAVE:
        if (mw != NULL && frame_set_hot(&mw->frame, FRAME_BTN_NONE)) {
            invalidate_caption(mw);
        }
        return 0;

    case WM_NCLBUTTONDOWN: {
        if (mw == NULL) break;

        /*
         * The caption buttons are handled here because frame_hit_test() claims
         * the whole caption strip as HTCAPTION - see the note there for why
         * handing the buttons to the system produces stray duplicates.
         *
         * The action runs on button DOWN, which is what Windows itself does:
         * press minimise and it minimises at once rather than waiting for the
         * release. The message is posted instead of sent so the minimise or
         * maximise loop does not start nested inside non-client handling.
         */
        const int btn = frame_button_at(&mw->frame, screen_point(lp));
        if (btn == FRAME_BTN_NONE) break;     /* not a button: let the system drag */

        switch (btn) {
        case FRAME_BTN_MIN:
            PostMessageW(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
            break;
        case FRAME_BTN_MAX:
            PostMessageW(hwnd, WM_SYSCOMMAND,
                         IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
            break;
        case FRAME_BTN_CLOSE:
            PostMessageW(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
            break;
        default:
            break;
        }
        return 0;
    }

    case WM_NCRBUTTONUP:
        /* Right-clicking the caption is a standard way to reach the system
         * menu (Restore / Move / Size / Minimise / Maximise / Close). */
        if (mw != NULL && (int)wp == HTCAPTION) {
            frame_show_system_menu(&mw->frame, screen_point(lp));
            return 0;
        }
        break;

    case WM_NCACTIVATE:
        /*
         * The caption belongs to us, so the active/inactive change has to
         * repaint it. Returning TRUE tells the system not to draw the
         * non-client area itself.
         */
        if (mw != NULL) invalidate_caption(mw);
        return TRUE;

    case WM_NCPAINT:
        /*
         * Draw nothing in the non-client area - ever.
         *
         * WM_NCCALCSIZE says there is no non-client area, but the system still
         * paints its own caption buttons when the cursor hovers over the
         * HTMINBUTTON / HTMAXBUTTON / HTCLOSE regions we report. The result is
         * a second set of square buttons appearing next to the real ones
         * (reported in testing). Returning 0 without calling DefWindowProc is
         * what stops it; the caption is painted by WM_PAINT instead.
         */
        return 0;

    case WM_DPICHANGED: {
        if (mw == NULL) break;

        frame_refresh_metrics(&mw->frame);

        /* The suggested rectangle keeps the window the same apparent size
         * after the DPI change. */
        const RECT *suggested = (const RECT *)lp;
        if (suggested != NULL) {
            SetWindowPos(hwnd, NULL,
                         suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        layout_children(mw);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);

        if (mw != NULL) {
            RECT cap;
            GetClientRect(hwnd, &cap);
            cap.bottom = mw->frame.height;

            /* Only paint when the update region actually touches the
             * caption: child controls cover the rest of the window. */
            RECT clipped = cap;
            if (IntersectRect(&clipped, &clipped, &ps.rcPaint)) {
                wchar_t title[256];
                title[0] = L'\0';
                GetWindowTextW(hwnd, title, (int)(sizeof(title) / sizeof(title[0])));

                const BOOL active = (GetActiveWindow() == hwnd);
                frame_paint(&mw->frame, dc, title, active);
            }
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
        /* Placeholder text sits on the window face, not on a white box. */
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    /* ---- layout and commands ------------------------------------- */

    case WM_SIZE:
        if (mw) layout_children(mw);
        return 0;

    case WM_COMMAND: {
        if (mw == NULL) break;
        const int id = LOWORD(wp);

        /* The editor's fields refresh the pixel readout and the preview on
         * every keystroke, so the picture follows what is being typed. */
        if (HIWORD(wp) == EN_CHANGE && id >= IDC_EDIT_X && id <= IDC_EDIT_H) {
            editor_refresh(mw);
            return 0;
        }

        switch (id) {
        /* Navigation strip. */
        case IDC_NAV_WINDOWS:    nav_go(mw, PAGE_WINDOWS);    return 0;
        case IDC_NAV_WORKSPACES: nav_go(mw, PAGE_WORKSPACES); return 0;
        case IDC_NAV_SETTINGS:   nav_go(mw, PAGE_SETTINGS);   return 0;
        case IDC_NAV_REFRESH:    emit(mw, MW_CMD_REFRESH);    return 0;
        case IDC_BACK:           nav_back(mw);                return 0;

        /* Overview page actions. The handler gets the selection through
         * mainwindow_selected_window() / _preset_id(), so nothing has to be
         * smuggled through the command itself. */
        case IDC_BTN_ANCHOR:        emit(mw, MW_CMD_ANCHOR);        return 0;
        case IDC_BTN_MOVE_MONITOR:  emit(mw, MW_CMD_MOVE_TO_MONITOR); return 0;
        case IDC_BTN_DEL_PRESET:    emit(mw, MW_CMD_DELETE_PRESET); return 0;
        case IDC_BTN_APPLY_ALL:     emit(mw, MW_CMD_APPLY_ALL);     return 0;

        /* Window-preset page. */
        case IDC_BTN_CREATE_PRESET: emit(mw, MW_CMD_BEGIN_CREATE);  return 0;
        case IDC_BTN_DEL_PRESET2:   emit(mw, MW_CMD_DELETE_PRESET); return 0;

        /* Create-preset picker page. */
        case IDC_BTN_DO_CREATE:     emit(mw, MW_CMD_CREATE_PRESET); return 0;
        case IDC_BTN_CREATE_CANCEL: mainwindow_close_create_page(mw); return 0;
        case IDC_BTN_MORE:
            /* Stage 8: the installed-application list. It used to say "this
             * arrives with the fake-desktop stage" - it has arrived. */
            emit(mw, MW_CMD_BROWSE_APPS);
            return 0;
        case IDC_BTN_EDIT_PRESET: {
            const MwPresetRow *row = find_preset_row(mw, mainwindow_selected_preset_id(mw));
            if (row != NULL) editor_open(mw, row);
            return 0;
        }
        case IDC_EDIT_SAVE:   emit(mw, MW_CMD_SAVE_PRESET); return 0;
        case IDC_EDIT_CANCEL:
            if (editor_may_close(mw)) editor_close(mw);
            return 0;

        /* Settings page. [Apply] is the only thing that writes; [Cancel] and
         * leaving the page both put the controls back. */
        case IDC_SETTINGS_APPLY:  emit(mw, MW_CMD_SETTINGS_APPLY);  return 0;
        case IDC_SETTINGS_CANCEL: emit(mw, MW_CMD_SETTINGS_CANCEL); return 0;
        case IDC_SETTINGS_GUIDE:
            /* A SUB-page of settings: pushed, so Back returns to the page the
             * user came from (nav.h explains the difference from nav_set). */
            if (nav_push(&mw->nav, PAGE_HELP)) apply_page(mw);
            return 0;

        /* User-guide page. */
        case IDC_HELP_OPEN_STARTUP: emit(mw, MW_CMD_OPEN_STARTUP); return 0;
        case IDC_HELP_ABOUT:
        case IDM_ABOUT:             emit(mw, MW_CMD_ABOUT);        return 0;

        /* Workspace page action row. */
        case IDC_BTN_NEW:
        case IDM_NEW:      emit(mw, MW_CMD_NEW);      return 0;
        case IDC_BTN_APPLY:
        case IDM_APPLY:    emit(mw, MW_CMD_APPLY);    return 0;
        /*
         * No control sends MW_CMD_ADD_SLOT / MW_CMD_REMOVE_SLOT any more: the
         * two buttons left this page on 2026-10-09 (see the layout comment).
         * The commands and their handlers stay, because the capability moves
         * into the full-screen workspace editor rather than disappearing.
         */
        case IDC_BTN_UNBIND_SLOT:
                           emit(mw, MW_CMD_UNBIND_SLOT); return 0;
        case IDC_BTN_EDIT_WORKSPACE:
                           emit(mw, MW_CMD_EDIT_OVERLAY); return 0;

        /*
         * The editor's monitor box: it picks which SCREEN SHAPE the preview is
         * drawn against, and changes nothing that is stored (fill_monitor_combo
         * has the reasoning). It arrives as CBN_SELCHANGE, whose LOWORD is the
         * control id like a button's - the two cannot collide because the ids
         * are distinct.
         */
        case IDC_COMBO_MONITOR:
                           editor_refresh(mw); return 0;

        /*
         * The editor's target box. Nothing STORED depends on it, but the
         * "window is ..." readout is measured from whatever it selects - so
         * without this the line keeps showing the previous window's numbers
         * after the user picks another one, which reads as a measurement of
         * the wrong window (2026-10-10, task B).
         */
        case IDC_COMBO_TARGET:
                           editor_refresh(mw); return 0;

        /* The pin box only changes what the preset will MEAN when it is saved;
         * the line beside it has to follow it immediately, or the row would
         * describe the state before the click. */
        case IDC_CHK_PIN:
                           editor_pin_text_update(mw); return 0;
        case IDC_BTN_CLOSE:
        case IDM_CLOSE_WS: emit(mw, MW_CMD_CLOSE_WORKSPACE); return 0;
        case IDC_BTN_DELETE:
        case IDM_DELETE:   emit(mw, MW_CMD_DELETE);   return 0;
        /* Settings is a page now, so both entries navigate instead of asking
         * the owner to open a dialog. (The navigation strip's own button is
         * handled with the other strip buttons, further up.) */
        case IDC_BTN_SETTINGS:
        case IDM_SETTINGS: nav_go(mw, PAGE_SETTINGS); return 0;
        case IDM_RENAME:   emit(mw, MW_CMD_RENAME);   return 0;
        case IDM_REFRESH:  emit(mw, MW_CMD_REFRESH);  return 0;
        case IDM_QUIT:     emit(mw, MW_CMD_QUIT);     return 0;
        default: break;
        }
        return 0;
    }

    case WM_NOTIFY: {
        if (mw == NULL) break;
        NMHDR *nh = (NMHDR *)lp;

        /* Both preset lists share one selection: they show the same rows, and
         * only one of them is ever visible. */
        if (nh->code == LVN_ITEMCHANGED) {
            if (nh->idFrom == IDC_WIN_LIST) {
                remember_window_selection(mw);
                return 0;
            }
            if (nh->idFrom == IDC_PRESET_LIST) {
                remember_preset_selection(mw, mw->preset_list);
                return 0;
            }
            if (nh->idFrom == IDC_PRESET_PAGE_LIST) {
                remember_preset_selection(mw, mw->preset_page_list);
                return 0;
            }
            if (nh->idFrom == IDC_CREATE_LIST) {
                const int row = ListView_GetNextItem(mw->create_list, -1, LVNI_SELECTED);
                mw->create_pick = (row >= 0 && row < mw->create_count)
                                      ? mw->create_rows[row].hwnd
                                      : 0ULL;
                mw->create_pick_row = (row >= 0 && row < mw->create_count) ? row : -1;
                return 0;
            }
        }

        /*
         * The slot list (workspace page, stage 7 / task 7.3).
         *
         * It is the only list that can hold a row the program cannot act on: a
         * slot whose monitor is gone. Two things happen here, and both take
         * their answer from the row itself, so the greyed row and the
         * explanation cannot disagree:
         *
         *   - the row is drawn in the greyed system colour (custom draw);
         *   - clicking it explains what is wrong and what can be done.
         *
         * The click is NM_CLICK, NOT the selection change: a refresh rebuilds
         * this list and changes the selection too, and a modal box appearing
         * because the program repainted a list would be the worst kind of
         * surprise.
         */
        if (nh->idFrom == IDC_SLOT_LIST) {
            if (nh->code == NM_CUSTOMDRAW) {
                NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lp;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    const int row = (int)cd->nmcd.dwItemSpec;
                    if (row >= 0 && row < mw->slot_row_count &&
                        mw->slot_rows[row].blocked[0] != L'\0') {
                        cd->clrText = GetSysColor(COLOR_GRAYTEXT);
                        return CDRF_NEWFONT;
                    }
                }
                return CDRF_DODEFAULT;
            }
            if (nh->code == NM_CLICK) {
                const NMITEMACTIVATE *ia = (const NMITEMACTIVATE *)lp;
                const int row = (int)ia->iItem;
                if (row >= 0 && row < mw->slot_row_count &&
                    mw->slot_rows[row].blocked[0] != L'\0') {
                    MessageBoxW(mw->hwnd, mw->slot_rows[row].blocked,
                                i18n_text(STR_ERR_TITLE),
                                MB_OK | MB_ICONWARNING);
                }
                return 0;
            }
        }

        if (nh->idFrom == IDC_LISTVIEW) {
            if (nh->code == LVN_ITEMCHANGED) {
                remember_selection(mw);
                update_button_states(mw);
                return 0;
            }
            if (nh->code == NM_DBLCLK) {
                /* Double-click is the natural "do the obvious thing" on a
                 * workspace: apply it. */
                emit(mw, MW_CMD_APPLY);
                return 0;
            }
            if (nh->code == LVN_KEYDOWN) {
                NMLVKEYDOWN *kd = (NMLVKEYDOWN *)lp;
                if (kd->wVKey == VK_RETURN) { emit(mw, MW_CMD_APPLY); return 0; }
                if (kd->wVKey == VK_DELETE) { emit(mw, MW_CMD_DELETE); return 0; }
                if (kd->wVKey == VK_F2)     { emit(mw, MW_CMD_RENAME); return 0; }
            }
        }
        break;
    }

    case WM_SETFOCUS:
        /* Focus whatever the visible page offers first. Never focus a hidden
         * control: the caret would be somewhere the user cannot see. */
        if (mw != NULL) {
            if (nav_current(&mw->nav) == PAGE_OVERVIEW) {
                if (mw->list != NULL) SetFocus(mw->list);
            } else if (mw->back_btn != NULL) {
                SetFocus(mw->back_btn);
            }
        }
        return 0;

    case WM_CLOSE:
        /*
         * The window does not decide whether closing means "hide" or "quit".
         * It forwards to the owner, which applies the user's saved
         * preference. Closing the window is a normal thing to do and must
         * never destroy state the user cares about.
         */
        if (mw) emit(mw, MW_CMD_QUIT);
        return 0;

    case WM_DESTROY:
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Creation                                                            */
/* ------------------------------------------------------------------ */

static const wchar_t *k_class = L"WacMainWindow";

BOOL mainwindow_register_class(HINSTANCE hinst)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = mainwnd_proc;
    wc.hInstance     = hinst;
    wc.hIcon         = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm       = LoadIconW(NULL, IDI_APPLICATION);
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = k_class;

    if (RegisterClassExW(&wc) != 0) return TRUE;

    /* Already registered is fine: a second call is not a failure. */
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

static HWND make_control(MainWindow *mw, const wchar_t *cls, const wchar_t *text,
                         DWORD style, DWORD ex_style, int id)
{
    HWND h = CreateWindowExW(ex_style, cls, text, WS_CHILD | style,
                             0, 0, 0, 0, mw->hwnd,
                             (HMENU)(INT_PTR)id, mw->hinst, NULL);
    if (h != NULL && mw->font != NULL) {
        SendMessageW(h, WM_SETFONT, (WPARAM)mw->font, TRUE);
    }
    return h;
}

static HWND make_button(MainWindow *mw, const wchar_t *text, int id, int index)
{
    HWND h = make_control(mw, L"BUTTON", text,
                          WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, id);
    mw->buttons[index] = h;
    return h;
}

/*
 * A slightly larger, semi-bold face for the list headings.
 *
 * The headings used to be plain body text in a 30 px strip, which left an
 * obvious gap between the words and the list below them and made the two look
 * unrelated. A bigger heading in a tighter strip reads as a title for the list
 * rather than as a stray line of text.
 *
 * Derived from the UI font so it follows the user's font settings and the
 * window DPI; NULL when the stock font cannot be queried, in which case the
 * headings simply keep the body font.
 */
static HFONT make_section_font(HFONT base, int dpi)
{
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    if (base == NULL || GetObjectW(base, sizeof(lf), &lf) == 0) return NULL;

    lf.lfHeight = -MulDiv(11, dpi, 96);
    lf.lfWeight = FW_SEMIBOLD;
    return CreateFontIndirectW(&lf);
}

/*
 * One report-style ListView.
 *
 * The extended styles matter: without LVS_EX_DOUBLEBUFFER the list flickers on
 * every rebuild, and without FULLROWSELECT the selection is a single cell,
 * which reads as "nothing is selected" when the user clicks a row.
 */
static HWND make_listview(MainWindow *mw, int id, const ColSpec *cols, int ncols)
{
    HWND h = make_control(mw, WC_LISTVIEWW, L"",
                          WS_VISIBLE | WS_TABSTOP |
                          LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                          WS_EX_CLIENTEDGE, id);
    if (h == NULL) return NULL;

    ListView_SetExtendedListViewStyle(h,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    for (int i = 0; i < ncols; ++i) {
        LVCOLUMNW c;
        ZeroMemory(&c, sizeof(c));
        c.mask     = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText  = (LPWSTR)i18n_text(cols[i].text);
        /* A provisional width; fit_columns() redistributes to the real one
         * before the window is ever shown. */
        c.cx       = cols[i].width;
        c.iSubItem = i;
        SendMessageW(h, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
    }
    return h;
}

/*
 * Distribute the list's width across its columns, in proportion to the design
 * widths, so the total always equals the client width.
 *
 * The arithmetic now lives in ui/pagelist.c, where it can be tested without a
 * window (tests/t12_uiformat.c). This project keeps its column metadata in its
 * own shape - a translated heading per column - so only the widths are shared.
 */
static void fit_columns(HWND list, const ColSpec *cols, int ncols, int client_w)
{
    if (cols == NULL) return;

    int weights[16];
    if (ncols <= 0 || ncols > (int)(sizeof(weights) / sizeof(weights[0]))) return;

    for (int i = 0; i < ncols; ++i) weights[i] = cols[i].width;

    pagelist_fit_weights(list, weights, ncols, client_w);
}

static void create_navigation(MainWindow *mw)
{
    struct { StrId text; int id; } nav[NAV_COUNT] = {
        { STR_NAV_WINDOWS,    IDC_NAV_WINDOWS    },
        { STR_NAV_WORKSPACES, IDC_NAV_WORKSPACES },
        { STR_NAV_REFRESH,    IDC_NAV_REFRESH    },
        { STR_NAV_SETTINGS,   IDC_NAV_SETTINGS   },
    };

    for (int i = 0; i < NAV_COUNT; ++i) {
        mw->nav_btn[i] = make_control(mw, L"BUTTON", i18n_text(nav[i].text),
                                      WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      0, nav[i].id);
    }

    /* The title row is created hidden and shown by apply_page() when the
     * stack is not at the root. */
    mw->back_btn = make_control(mw, L"BUTTON", i18n_text(STR_BTN_BACK),
                                WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_BACK);

    mw->page_title = make_control(mw, L"STATIC", L"",
                                  SS_LEFT | SS_CENTERIMAGE, 0, IDC_PAGE_TITLE);
}

static void create_pages(MainWindow *mw)
{
    /* The headings get their own face; the lists keep the body font. */
    mw->section_font = make_section_font(mw->font, mw_dpi(mw));

    /* ---- overview: running windows ------------------------------- */
    {
        mw->sec_windows = make_control(mw, L"STATIC", L"", SS_LEFT,
                                       0, IDC_SEC_WINDOWS);
        mw->win_list = make_listview(mw, IDC_WIN_LIST, kWindowCols,
                                     (int)(sizeof(kWindowCols) / sizeof(kWindowCols[0])));
    }

    /* ---- overview: window presets -------------------------------- */
    {
        mw->sec_presets = make_control(mw, L"STATIC", L"", SS_LEFT,
                                       0, IDC_SEC_PRESETS);
        mw->preset_list = make_listview(mw, IDC_PRESET_LIST, kPresetCols,
                                        (int)(sizeof(kPresetCols) / sizeof(kPresetCols[0])));
    }

    /* ---- overview: what the user can do with those two lists ------ */
    mw->btn_anchor = make_control(mw, L"BUTTON", i18n_text(STR_BTN_ANCHOR),
                                  WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  0, IDC_BTN_ANCHOR);
    mw->btn_move_monitor = make_control(mw, L"BUTTON", i18n_text(STR_BTN_MOVE_MONITOR),
                                      WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      0, IDC_BTN_MOVE_MONITOR);
    mw->btn_delete_preset = make_control(mw, L"BUTTON",
                                         i18n_text(STR_BTN_DELETE_PRESET),
                                         WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                         0, IDC_BTN_DEL_PRESET);
    mw->btn_apply_all = make_control(mw, L"BUTTON", i18n_text(STR_BTN_APPLY_ALL),
                                     WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                     0, IDC_BTN_APPLY_ALL);

    /* ---- window-preset page: the list ---------------------------- */
    mw->preset_page_list = make_listview(mw, IDC_PRESET_PAGE_LIST, kPresetCols,
                                         (int)(sizeof(kPresetCols) / sizeof(kPresetCols[0])));
    mw->btn_edit_preset = make_control(mw, L"BUTTON", i18n_text(STR_BTN_EDIT_PRESET),
                                       WS_TABSTOP | BS_PUSHBUTTON, 0,
                                       IDC_BTN_EDIT_PRESET);
    mw->btn_create_preset = make_control(mw, L"BUTTON", i18n_text(STR_BTN_CREATE_PRESET),
                                         WS_TABSTOP | BS_PUSHBUTTON, 0,
                                         IDC_BTN_CREATE_PRESET);
    mw->btn_delete_preset2 = make_control(mw, L"BUTTON", i18n_text(STR_BTN_DELETE_PRESET2),
                                          WS_TABSTOP | BS_PUSHBUTTON, 0,
                                          IDC_BTN_DEL_PRESET2);

    /* Target window and monitor selectors. */
    mw->lbl_target = make_control(mw, L"STATIC", i18n_text(STR_LABEL_TARGET_WINDOW),
                                  SS_LEFT | SS_CENTERIMAGE, 0, IDC_LABEL_TARGET);
    mw->cb_target  = make_control(mw, L"COMBOBOX", L"",
                                  WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                  0, IDC_COMBO_TARGET);
    mw->lbl_monitor = make_control(mw, L"STATIC", i18n_text(STR_LABEL_MONITOR_SEL),
                                   SS_LEFT | SS_CENTERIMAGE, 0, IDC_LABEL_MONITOR);
    mw->cb_monitor  = make_control(mw, L"COMBOBOX", L"",
                                   WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                   0, IDC_COMBO_MONITOR);

    /*
     * The monitor box is filled AND enabled by fill_monitor_combo(), and only
     * when there is at least one monitor to list. The comment that used to sit
     * here said "the list is real" - it was one hard-coded line, and it stayed
     * that way until multi-monitor placement had been measured
     * (2026-10-09).
     */

    mw->lbl_position = make_control(mw, L"STATIC", i18n_text(STR_LABEL_POSITION),
                                    SS_LEFT | SS_CENTERIMAGE, 0, IDC_LABEL_POSITION);
    mw->lbl_size = make_control(mw, L"STATIC", i18n_text(STR_LABEL_SIZE),
                                SS_LEFT | SS_CENTERIMAGE, 0, IDC_LABEL_SIZE);

    /* ---- create-preset picker page -------------------------------- */
    mw->create_list = make_listview(mw, IDC_CREATE_LIST, kWindowCols,
                                    (int)(sizeof(kWindowCols) / sizeof(kWindowCols[0])));
    mw->create_hint = make_control(mw, L"STATIC", i18n_text(STR_MSG_CREATE_PICK),
                                   SS_LEFT, 0, IDC_CREATE_HINT);
    mw->btn_do_create = make_control(mw, L"BUTTON", i18n_text(STR_BTN_CREATE),
                                     WS_TABSTOP | BS_DEFPUSHBUTTON, 0, IDC_BTN_DO_CREATE);
    mw->btn_more = make_control(mw, L"BUTTON", i18n_text(STR_BTN_MORE),
                                WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_BTN_MORE);
    mw->btn_create_cancel = make_control(mw, L"BUTTON", i18n_text(STR_SETTINGS_CANCEL),
                                         WS_TABSTOP | BS_PUSHBUTTON, 0,
                                         IDC_BTN_CREATE_CANCEL);

    /* ---- window-preset page: the hand-editing panel ---------------
     *
     * Created up front and hidden. Four percentage fields, a readout of what
     * they mean in monitor pixels, and a picture - because four numbers with
     * no feedback is how 0.469 x 0.563 gets typed as 0.469 x 0.356.
     * ------------------------------------------------------------- */
    mw->ed_target = make_control(mw, L"STATIC", L"", SS_LEFT, 0, IDC_EDIT_TARGET);

    {
        static const StrId kLabels[EDIT_FIELD_COUNT] = {
            STR_LABEL_X, STR_LABEL_Y, STR_LABEL_W, STR_LABEL_H
        };
        static const int kFieldIds[EDIT_FIELD_COUNT] = {
            IDC_EDIT_X, IDC_EDIT_Y, IDC_EDIT_W, IDC_EDIT_H
        };
        static const int kLabelIds[EDIT_FIELD_COUNT] = {
            IDC_EDIT_LABEL_X, IDC_EDIT_LABEL_Y, IDC_EDIT_LABEL_W, IDC_EDIT_LABEL_H
        };

        for (int i = 0; i < EDIT_FIELD_COUNT; ++i) {
            mw->ed_label[i] = make_control(mw, L"STATIC", i18n_text(kLabels[i]),
                                           SS_LEFT | SS_CENTERIMAGE, 0, kLabelIds[i]);
            mw->ed_field[i] = make_control(mw, L"EDIT", L"",
                                           WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL,
                                           WS_EX_CLIENTEDGE, kFieldIds[i]);
        }
    }

    mw->ed_pixels     = make_control(mw, L"STATIC", L"", SS_LEFT, 0, IDC_EDIT_PIXELS);
    mw->ed_normalized = make_control(mw, L"STATIC", L"", SS_LEFT, 0, IDC_EDIT_NORMALIZED);
    mw->ed_locked     = make_control(mw, L"STATIC", L"", SS_LEFT, 0, IDC_EDIT_LOCKED);
    mw->ed_real       = make_control(mw, L"STATIC", L"", SS_LEFT, 0, IDC_EDIT_REAL);
    mw->ed_pin        = make_control(mw, L"BUTTON", i18n_text(STR_CHK_PIN_MONITOR),
                                     WS_TABSTOP | BS_AUTOCHECKBOX, 0, IDC_CHK_PIN);
    mw->ed_pin_text   = make_control(mw, L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE,
                                     0, IDC_EDIT_PIN);
    mw->ed_preview    = preview_create(mw->hwnd, mw->hinst, IDC_EDIT_PREVIEW);
    mw->ed_save       = make_control(mw, L"BUTTON", i18n_text(STR_BTN_SAVE),
                                     WS_TABSTOP | BS_DEFPUSHBUTTON, 0, IDC_EDIT_SAVE);
    mw->ed_cancel     = make_control(mw, L"BUTTON", i18n_text(STR_SETTINGS_CANCEL),
                                     WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_EDIT_CANCEL);

    /* ---- workspace page: the workspace list ---------------------- */
    {
        const ColSpec cols[] = {
            { STR_COL_WORKSPACE, 320 },
            { STR_COL_APPS,       60 },
            { STR_COL_ID,        200 },
        };
        mw->list = make_listview(mw, IDC_LISTVIEW, cols,
                                 (int)(sizeof(cols) / sizeof(cols[0])));
    }

    /* ---- workspace page: the action row --------------------------
     *
     * Named after what they act on, like the window-preset page's three
     * buttons, and one action each:
     *
     *   [Run] [Close its applications]   act on the running programs
     *   [Add selected preset]            builds the workspace
     *   [New] [Delete]                   act on the definition
     *
     * There is no [Settings] here any more: the navigation strip already has
     * one, and two buttons that do the same thing in one window is how a user
     * learns to distrust the labels.
     * ------------------------------------------------------------- */
    make_button(mw, i18n_text(STR_BTN_RUN_WORKSPACE),    IDC_BTN_APPLY,    0);
    make_button(mw, i18n_text(STR_BTN_CLOSE_APPS),       IDC_BTN_CLOSE,    1);
    make_button(mw, i18n_text(STR_BTN_NEW_WORKSPACE),    IDC_BTN_NEW,      3);
    make_button(mw, i18n_text(STR_BTN_DELETE_WORKSPACE), IDC_BTN_DELETE,   4);
    make_button(mw, i18n_text(STR_BTN_UNBIND_SLOT),      IDC_BTN_UNBIND_SLOT, 7);

    /* [Edit selected workspace]: the row was one short until stage 5 gave the
     * workspace something to edit. Its id is its own - it opened as [Settings]
     * once, and reusing that number for a different action is how a stale
     * reference becomes a collision nobody can see (res/resource.h). */
    make_button(mw, i18n_text(STR_BTN_EDIT_WORKSPACE),   IDC_BTN_EDIT_WORKSPACE, 5);
    /* The slot list: one column, because what it shows is one thing - the
     * presets this workspace is made of, in the order they will run. */
    mw->sec_slots = make_control(mw, L"STATIC", L"", SS_LEFT, 0, IDC_SEC_SLOTS);
    mw->slot_list = make_listview(mw, IDC_SLOT_LIST, kSlotCols,
                                  (int)(sizeof(kSlotCols) / sizeof(kSlotCols[0])));

    if (mw->section_font != NULL && mw->sec_slots != NULL) {
        SendMessageW(mw->sec_slots, WM_SETFONT, (WPARAM)mw->section_font, TRUE);
    }

    /* ---- placeholder pages ---------------------------------------
     *
     * Created now so the navigation is complete and honest about what is
     * not built yet, rather than navigating to an empty rectangle. The
     * batches that implement each page replace its placeholder.
     * ------------------------------------------------------------- */
    for (int p = 0; p < PAGE_COUNT; ++p) {
        if (p == PAGE_OVERVIEW || p == PAGE_SETTINGS || p == PAGE_HELP ||
            p == PAGE_WORKSPACES) {
            continue;
        }
        mw->todo[p] = make_control(mw, L"STATIC", i18n_text(STR_PAGE_TODO),
                                   SS_LEFT, 0, 0);
    }

    /* ---- the pages that own their controls ----------------------- */
    {
        /*
         * The elevation is read ONCE and handed to both pages. They describe
         * the same process, and two independent lookups is how the status bar
         * and the settings page would eventually disagree.
         */
        const BOOL elevated = sysinfo_is_elevated();

        mw->settings_page = page_settings_create(mw->hwnd, mw->hinst, mw->font,
                                                 mw_dpi(mw), elevated);
        mw->help_page = page_help_create(mw->hwnd, mw->hinst, mw->font,
                                         mw->section_font, mw_dpi(mw), elevated);
    }

    /* Headings: the larger face, applied after both exist. */
    if (mw->section_font != NULL) {
        SendMessageW(mw->sec_windows, WM_SETFONT, (WPARAM)mw->section_font, TRUE);
        SendMessageW(mw->sec_presets, WM_SETFONT, (WPARAM)mw->section_font, TRUE);
    }

    fill_monitor_combo(mw);
}

MainWindow *mainwindow_create(HINSTANCE hinst, MainWindowCmdFn on_command, void *user)
{
    MainWindow *mw = (MainWindow *)calloc(1, sizeof(MainWindow));
    if (mw == NULL) return NULL;

    mw->hinst      = hinst;
    mw->on_command = on_command;
    mw->user       = user;

    /*
     * InitCommonControlsEx, not the bare InitCommonControls(): the ListView's
     * extended styles (full-row select, grid lines) need comctl32 v6, which
     * only activates when the class is requested explicitly.
     */
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    mw->font  = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    mw->icon  = LoadIconW(NULL, IDI_APPLICATION);
    mw->accel = build_accel();

    nav_init(&mw->nav);

    /* No menu: the window is created without one, and everything the menu
     * used to reach is on the navigation strip or the accelerator table.
     *
     * The size is given in LOGICAL pixels and scaled to the system DPI:
     * CreateWindowExW takes physical pixels, so a literal 1200x900 would be
     * 800x600 of usable space at 150% scaling - which is how the overview's
     * button row ended up clipped. */
    {
        const int dpi = (int)GetDpiForSystem();
        const int win_w = MulDiv(1200, (dpi > 0) ? dpi : 96, 96);
        const int win_h = MulDiv(900,  (dpi > 0) ? dpi : 96, 96);

        mw->hwnd = CreateWindowExW(
            0, k_class, i18n_text(STR_APP_TITLE),
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, win_w, win_h,
            NULL, NULL, hinst, mw);
    }

    if (mw->hwnd == NULL) {
        if (mw->accel) DestroyAcceleratorTable(mw->accel);
        free(mw);
        return NULL;
    }

    /* The frame needs the window to exist: its metrics come from the window's
     * DPI. */
    frame_attach(&mw->frame, mw->hwnd, mw->icon, mw->font);

    create_navigation(mw);
    create_pages(mw);

    /* ---- status bar ---------------------------------------------- */
    mw->status = make_control(mw, STATUSCLASSNAMEW, L"",
                              WS_VISIBLE | SBARS_SIZEGRIP, 0, IDC_STATUSBAR);

    update_button_states(mw);
    apply_page(mw);

    /*
     * One line of geometry per run.
     *
     * It is here because a report of "everything is offset when started as
     * administrator" cannot be checked without knowing what the window itself
     * computed: the DPI it read, the caption height it derived from it, and the
     * client size it ended up with. Those three numbers say immediately whether
     * the process sees the same display the user does.
     */
    {
        RECT rc;
        GetClientRect(mw->hwnd, &rc);
        log_info(L"window: dpi=%d caption=%d btn=%d border=%d client=%dx%d",
                 mw->frame.dpi, mw->frame.height, mw->frame.btn_width,
                 mw->frame.border, (int)rc.right, (int)rc.bottom);
    }

    return mw;
}

void mainwindow_destroy(MainWindow *mw)
{
    if (mw == NULL) return;

    /* The window first: the pages own fonts, and a font must outlive every
     * control that was told to use it. Destroying the parent takes the child
     * controls with it, so afterwards there is nothing left holding them. */
    if (mw->hwnd && IsWindow(mw->hwnd)) {
        DestroyWindow(mw->hwnd);
    }

    page_settings_destroy(mw->settings_page);
    page_help_destroy(mw->help_page);

    if (mw->accel) DestroyAcceleratorTable(mw->accel);
    if (mw->section_font) DeleteObject(mw->section_font);
    free(mw);
}

/* ------------------------------------------------------------------ */
/* Visibility                                                          */
/* ------------------------------------------------------------------ */

void mainwindow_show(MainWindow *mw)
{
    if (mw == NULL || mw->hwnd == NULL) return;
    ShowWindow(mw->hwnd, SW_SHOW);
    SetForegroundWindow(mw->hwnd);

    /*
     * Lay out again after showing.
     *
     * The client area is not final at the end of CreateWindowExW: the frame is
     * applied during the first show, and a change in the CLIENT area does NOT
     * produce WM_SIZE - that message only fires when the WINDOW changes size.
     * Without this second pass the controls keep the geometry they were given
     * while the client area was still 22 px narrower, which shows up as a blank
     * strip along the edges and as list columns that no longer add up.
     */
    layout_children(mw);

    if (mw->list && nav_current(&mw->nav) == PAGE_OVERVIEW) SetFocus(mw->list);
}

void mainwindow_hide(MainWindow *mw)
{
    if (mw == NULL || mw->hwnd == NULL) return;
    ShowWindow(mw->hwnd, SW_HIDE);
}

BOOL mainwindow_is_visible(const MainWindow *mw)
{
    if (mw == NULL || mw->hwnd == NULL) return FALSE;
    return IsWindowVisible(mw->hwnd);
}

void mainwindow_activate(MainWindow *mw)
{
    if (mw == NULL || mw->hwnd == NULL) return;

    if (IsIconic(mw->hwnd)) ShowWindow(mw->hwnd, SW_RESTORE);
    ShowWindow(mw->hwnd, SW_SHOW);

    /*
     * SetForegroundWindow only works for the process that owns the current
     * foreground window. Requesting it from a freshly launched second
     * instance is exactly the case Windows blocks, so the caller should
     * also call AllowSetForegroundWindow before asking. This call still
     * succeeds when we already have the right, and is harmless otherwise.
     */
    SetForegroundWindow(mw->hwnd);
    BringWindowToTop(mw->hwnd);
}

/* ------------------------------------------------------------------ */
/* Content                                                             */
/* ------------------------------------------------------------------ */

void mainwindow_set_workspaces(MainWindow *mw, const WorkspaceSet *set)
{
    if (mw == NULL) return;

    /* Keep the selection across a reload: the list is rebuilt wholesale, so
     * without this the user loses their place every time a workspace is
     * added or renamed. */
    char keep[WS_ID_MAX];
    strncpy_s(keep, sizeof(keep), mw->selected_id, _TRUNCATE);

    mw->row_count = 0;
    if (set) {
        const int n = (set->count < WS_MAX) ? set->count : WS_MAX;
        for (int i = 0; i < n; ++i) {
            /* Field by field, deliberately: assigning the Workspace would copy
             * 22 KB of slots that this list never looks at. */
            strncpy_s(mw->rows[i].id, sizeof(mw->rows[i].id),
                      set->items[i].id, _TRUNCATE);
            strncpy_s(mw->rows[i].name, sizeof(mw->rows[i].name),
                      set->items[i].name, _TRUNCATE);
            mw->rows[i].slot_count = set->items[i].slot_count;
        }
        mw->row_count = n;
    }

    list_reload(mw);

    if (keep[0]) {
        select_by_id(mw, keep);
    } else if (mw->row_count > 0) {
        /* Nothing was selected before: select the first row so the action
         * buttons are usable immediately. */
        LVITEMW item;
        ZeroMemory(&item, sizeof(item));
        item.mask = LVIF_STATE;
        item.iItem = 0;
        item.state = LVIS_SELECTED | LVIS_FOCUSED;
        item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(mw->list, LVM_SETITEMSTATE, 0, (LPARAM)&item);
        remember_selection(mw);
    }

    update_button_states(mw);
}

/* ------------------------------------------------------------------ */
/* Overview selections                                                 */
/* ------------------------------------------------------------------ */

/*
 * Which row is selected, recorded by IDENTITY (an HWND, or a rule id) rather
 * than by row index.
 *
 * That matters because both lists are rebuilt wholesale on every refresh: a
 * stored index would quietly point at a different row - or at nothing - the
 * moment a window opened or a preset was added.
 */
static void remember_window_selection(MainWindow *mw)
{
    mw->sel_window = 0;
    if (mw->win_list == NULL) return;

    const int row = ListView_GetNextItem(mw->win_list, -1, LVNI_SELECTED);
    if (row < 0 || row >= mw->win_count) return;

    mw->sel_window = mw->win_rows[row].hwnd;
}

static void remember_preset_selection(MainWindow *mw, HWND list)
{
    mw->sel_preset[0] = '\0';
    if (list == NULL) return;

    const int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (row < 0 || row >= mw->preset_count) return;

    strncpy_s(mw->sel_preset, sizeof(mw->sel_preset),
              mw->preset_rows[row].id, _TRUNCATE);
}

/* The row a preset id refers to, or NULL. The lists are rebuilt on every
 * refresh, so an id is the only stable way to find a row again. */
static const MwPresetRow *find_preset_row(const MainWindow *mw, const char *id)
{
    if (mw == NULL || id == NULL || id[0] == '\0') return NULL;

    for (int i = 0; i < mw->preset_count; ++i) {
        if (_stricmp(mw->preset_rows[i].id, id) == 0) return &mw->preset_rows[i];
    }
    return NULL;
}

/* Select row 0 of a list. Used when nothing was selected before, so the action
 * buttons are usable the moment the page appears. */
static void select_first_row(HWND list)
{
    /* The mechanics live in ui/pagelist.c; this stays as the call site's name
     * for them. */
    pagelist_select_first(list);
}

/* Re-select a row after a rebuild, so [Refresh] does not lose the user's place. */
static void reselect_window_row(MainWindow *mw)
{
    if (mw->win_list == NULL || mw->sel_window == 0) return;

    for (int i = 0; i < mw->win_count; ++i) {
        if (mw->win_rows[i].hwnd != mw->sel_window) continue;

        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask      = LVIF_STATE;
        it.iItem     = i;
        it.state     = LVIS_SELECTED | LVIS_FOCUSED;
        it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(mw->win_list, LVM_SETITEMSTATE, i, (LPARAM)&it);
        SendMessageW(mw->win_list, LVM_ENSUREVISIBLE, i, FALSE);
        return;
    }
}

static void reselect_preset_row(MainWindow *mw, HWND list)
{
    if (list == NULL || mw->sel_preset[0] == '\0') return;

    for (int i = 0; i < mw->preset_count; ++i) {
        if (_stricmp(mw->preset_rows[i].id, mw->sel_preset) != 0) continue;

        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask      = LVIF_STATE;
        it.iItem     = i;
        it.state     = LVIS_SELECTED | LVIS_FOCUSED;
        it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(list, LVM_SETITEMSTATE, i, (LPARAM)&it);
        SendMessageW(list, LVM_ENSUREVISIBLE, i, FALSE);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Status line helpers                                                 */
/* ------------------------------------------------------------------ */

/* Fill one list with a single "nothing here" line, so an empty list reads as
 * "nothing to show" rather than as a control that failed to load. */
static void list_show_empty(HWND list, StrId message)
{
    if (list == NULL) return;

    LVITEMW item;
    ZeroMemory(&item, sizeof(item));
    item.mask    = LVIF_TEXT;
    item.iItem   = 0;
    item.pszText = (LPWSTR)i18n_text(message);
    SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item);
}

/* Section heading: "Running windows (3)". */
static void set_section_text(HWND heading, StrId format, int count)
{
    if (heading == NULL) return;

    wchar_t text[128];
    _snwprintf_s(text, 128, _TRUNCATE, i18n_text(format), count);
    SetWindowTextW(heading, text);
}

void mainwindow_set_window_rows(MainWindow *mw, const MwWindowRow *rows, int count)
{
    if (mw == NULL) return;

    mw->win_count = 0;
    if (rows != NULL) {
        const int n = (count < MW_ROW_MAX) ? count : MW_ROW_MAX;
        for (int i = 0; i < n; ++i) {
            mw->win_rows[i] = rows[i];
        }
        mw->win_count = n;
    }

    if (mw->win_list != NULL) {
        SendMessageW(mw->win_list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(mw->win_list);

        for (int i = 0; i < mw->win_count; ++i) {
            /* Not const: ListView_SetItemText takes an LPWSTR, and these rows
             * are our own copy anyway. */
            MwWindowRow *r = &mw->win_rows[i];

            LVITEMW item;
            ZeroMemory(&item, sizeof(item));
            item.mask    = LVIF_TEXT;
            item.iItem   = i;
            item.pszText = r->app;
            const int row = (int)SendMessageW(mw->win_list, LVM_INSERTITEMW, 0,
                                              (LPARAM)&item);
            if (row < 0) continue;

            ListView_SetItemText(mw->win_list, row, 1, r->title);
            ListView_SetItemText(mw->win_list, row, 2, r->state);
            ListView_SetItemText(mw->win_list, row, 3, r->size);
        }

        if (mw->win_count == 0) list_show_empty(mw->win_list, STR_NO_WINDOWS);

        SendMessageW(mw->win_list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(mw->win_list, NULL, TRUE);

        reselect_window_row(mw);

        /* Nothing was selected before: pick the first row so [Anchor selected
         * window] is usable immediately, exactly like the workspace list. */
        if (mw->sel_window == 0 && mw->win_count > 0) {
            select_first_row(mw->win_list);
            remember_window_selection(mw);
        }

        fill_target_combo(mw);
    }

    set_section_text(mw->sec_windows, STR_OVERVIEW_WINDOWS, mw->win_count);
}

/* Fill ONE preset list. There are two of them - the overview's and the preset
 * page's - and they always show the same rows. */
static void fill_preset_list(HWND list, MwPresetRow *rows, int count)
{
    if (list == NULL) return;

    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);

    for (int i = 0; i < count; ++i) {
        MwPresetRow *r = &rows[i];

        LVITEMW item;
        ZeroMemory(&item, sizeof(item));
        item.mask    = LVIF_TEXT;
        item.iItem   = i;
        item.pszText = r->app;
        const int row = (int)SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item);
        if (row < 0) continue;

        ListView_SetItemText(list, row, 1, r->label);
        ListView_SetItemText(list, row, 2, r->zone);
        ListView_SetItemText(list, row, 3, r->monitor);
        ListView_SetItemText(list, row, 4, r->state);
    }

    if (count == 0) list_show_empty(list, STR_NO_PRESETS);

    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, NULL, TRUE);
}

void mainwindow_set_preset_rows(MainWindow *mw, const MwPresetRow *rows, int count)
{
    if (mw == NULL) return;

    mw->preset_count = 0;
    if (rows != NULL) {
        const int n = (count < MW_ROW_MAX) ? count : MW_ROW_MAX;
        for (int i = 0; i < n; ++i) {
            mw->preset_rows[i] = rows[i];
        }
        mw->preset_count = n;
    }

    fill_preset_list(mw->preset_list,      mw->preset_rows, mw->preset_count);
    fill_preset_list(mw->preset_page_list, mw->preset_rows, mw->preset_count);

    reselect_preset_row(mw, mw->preset_list);
    reselect_preset_row(mw, mw->preset_page_list);

    if (mw->sel_preset[0] == '\0' && mw->preset_count > 0) {
        select_first_row(mw->preset_list);
        remember_preset_selection(mw, mw->preset_list);
    }

    set_section_text(mw->sec_presets, STR_OVERVIEW_PRESETS, mw->preset_count);

    /* If the preset the editor has open was deleted elsewhere (the command line,
     * say), close the editor rather than let it write to something that is
     * gone. */
    if (mw->ed_open && find_preset_row(mw, mw->ed_rule_id) == NULL) {
        editor_close(mw);
    }
}

/* Keep the [Create preset] target box in step with the window list. It offers
 * the same rows, so "create" can mean "remember where this one is now" without
 * sending the user back to the overview. */
static void fill_target_combo(MainWindow *mw)
{
    if (mw->cb_target == NULL) return;

    const LRESULT keep = SendMessageW(mw->cb_target, CB_GETCURSEL, 0, 0);

    SendMessageW(mw->cb_target, CB_RESETCONTENT, 0, 0);

    for (int i = 0; i < mw->win_count; ++i) {
        wchar_t label[MW_APP_MAX + MW_TEXT_MAX + 8];
        _snwprintf_s(label, MW_APP_MAX + MW_TEXT_MAX + 8, _TRUNCATE,
                     L"%ls  \u2014  %ls", mw->win_rows[i].app, mw->win_rows[i].title);

        const LRESULT idx = SendMessageW(mw->cb_target, CB_ADDSTRING, 0, (LPARAM)label);
        if (idx < 0) continue;
        SendMessageW(mw->cb_target, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);
    }

    /* Keep the user's choice across a refresh when the row still exists. */
    if (keep != CB_ERR && keep < (LRESULT)mw->win_count) {
        SendMessageW(mw->cb_target, CB_SETCURSEL, (WPARAM)keep, 0);
    } else if (mw->win_count > 0) {
        SendMessageW(mw->cb_target, CB_SETCURSEL, 0, 0);
    }
}

/* The monitor selector is filled with a placeholder and left disabled: the
 * position is reserved and this is the only change needed to make it live once
 * multi-monitor placement has been tested on real hardware. */
/*
 * Fill the monitor list from the real monitor set (2026-10-09).
 *
 * Until now this added ONE hard-coded line - "1 (primary)" - while the comment
 * beside the control claimed "the list is real". It was not, and it could not
 * be while multi-monitor placement was untested. It is tested now, so the
 * list is built from monitor_enumerate().
 *
 * WHAT THE CHOICE MEANS - and what it does not
 *
 *   The editor's numbers are FRACTIONS of a screen, and a window preset is NOT
 *   bound to any particular monitor: it is applied on whichever screen its
 *   window turns up on (measured 2026-10-08, section 8.9). So this dropdown
 *   cannot mean "the monitor this preset belongs to" - no such thing exists.
 *
 *   What it does mean: WHICH SCREEN SHAPE TO PREVIEW ON. That is worth having
 *   on a mixed-DPI desk, where the same 0,0,0.5,1 is 1280x1600 on the 150%
 *   panel and 1280x1440 on the 100% one - and it changes nothing that is
 *   stored, because fractions are all that get saved.
 */
static void fill_monitor_combo(MainWindow *mw)
{
    if (mw->cb_monitor == NULL) return;

    SendMessageW(mw->cb_monitor, CB_RESETCONTENT, 0, 0);

    MonitorFingerprint mons[WAC_MON_MAX];
    const int n = monitor_enumerate(mons, WAC_MON_MAX);

    /* The screen the main window is on is the useful default: it is the one the
     * user is looking at, and the one the numbers were read from. */
    int want = -1;
    {
        const HMONITOR hm = MonitorFromWindow(mw->hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        ZeroMemory(&mi, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if (hm != NULL && GetMonitorInfoW(hm, &mi)) {
            for (int i = 0; i < n; ++i) {
                if (mons[i].virtual_x == mi.rcMonitor.left &&
                    mons[i].virtual_y == mi.rcMonitor.top) {
                    want = i;
                    break;
                }
            }
        }
    }
    if (want < 0) want = monitor_primary_index(mons, n);
    if (want < 0) want = 0;

    for (int i = 0; i < n; ++i) {
        const wchar_t *name = (mons[i].friendly_name[0] != L'\0')
                                  ? mons[i].friendly_name
                                  : mons[i].device_name;

        wchar_t label[192];
        _snwprintf_s(label, 192, _TRUNCATE, i18n_text(STR_MONITOR_ITEM),
                     name, mons[i].width, mons[i].height);

        const LRESULT idx = SendMessageW(mw->cb_monitor, CB_ADDSTRING, 0, (LPARAM)label);
        if (idx >= 0) {
            /* The item carries its monitor INDEX rather than being assumed to
             * be one: the list order is monitor_enumerate()'s, and storing the
             * index makes that explicit instead of implied by position. */
            SendMessageW(mw->cb_monitor, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);
        }
    }

    if (n > 0) {
        SendMessageW(mw->cb_monitor, CB_SETCURSEL, (WPARAM)want, 0);
        EnableWindow(mw->cb_monitor, TRUE);
    } else {
        /* Nothing to choose from: stay disabled rather than offer an empty list,
         * which reads as a broken control. */
        EnableWindow(mw->cb_monitor, FALSE);
    }
}

/*
 * What the monitor box itself reports: the selected row, and the monitor index
 * stored on that row. Both are -1 when nothing is selected, which is exactly
 * what monitor_selection_index() expects to be handed.
 */
static void monitor_box_selection(const MainWindow *mw, int *out_sel, int *out_data)
{
    *out_sel  = -1;
    *out_data = -1;

    if (mw == NULL || mw->cb_monitor == NULL) return;

    const LRESULT sel = SendMessageW(mw->cb_monitor, CB_GETCURSEL, 0, 0);
    if (sel < 0) return;   /* CB_ERR: nothing chosen */

    *out_sel  = (int)sel;
    *out_data = (int)SendMessageW(mw->cb_monitor, CB_GETITEMDATA, (WPARAM)sel, 0);
}

int mainwindow_selected_monitor_index(const MainWindow *mw)
{
    int sel = -1, data = -1;
    monitor_box_selection(mw, &sel, &data);

    MonitorFingerprint mons[WAC_MON_MAX];
    const int n = monitor_enumerate(mons, WAC_MON_MAX);

    return monitor_selection_index(sel, data, n);
}

/*
 * The screen shape the editor's preview should draw.
 *
 * Returns FALSE when the box has no usable selection, and the caller then keeps
 * the window's own monitor - which is what the editor did before this box was
 * connected, so a failure here degrades to the previous behaviour rather than
 * to a wrong picture.
 */
static BOOL editor_preview_monitor(const MainWindow *mw, int *out_w, int *out_h)
{
    if (mw == NULL || mw->cb_monitor == NULL || out_w == NULL || out_h == NULL) {
        return FALSE;
    }

    int sel = -1, data = -1;
    monitor_box_selection(mw, &sel, &data);

    MonitorFingerprint mons[WAC_MON_MAX];
    const int n = monitor_enumerate(mons, WAC_MON_MAX);

    /* The same decision the [Move to the selected monitor] button makes, taken
     * in one place: whatever the box selects means one monitor to both of
     * them. */
    const int idx = monitor_selection_index(sel, data, n);
    if (idx < 0) return FALSE;

    *out_w = mons[idx].width;
    *out_h = mons[idx].height;
    return TRUE;
}

void mainwindow_set_prompt_suppressed(MainWindow *mw, BOOL suppressed)
{
    if (mw == NULL) return;
    mw->no_prompt = suppressed ? TRUE : FALSE;
}
void mainwindow_set_status(MainWindow *mw, const wchar_t *text)
{
    if (mw == NULL || mw->status == NULL) return;
    SendMessageW(mw->status, SB_SETTEXTW, 0, (LPARAM)(text ? text : L""));
}

void mainwindow_set_active(MainWindow *mw, const char *workspace_id)
{
    if (mw == NULL) return;

    if (workspace_id && workspace_id[0]) {
        strncpy_s(mw->active_id, sizeof(mw->active_id), workspace_id, _TRUNCATE);
    } else {
        mw->active_id[0] = '\0';
    }

    /* Only the first column changes, but a full reload is simpler and the
     * list is small. */
    list_reload(mw);
    if (mw->selected_id[0]) select_by_id(mw, mw->selected_id);
}

const char *mainwindow_selected_id(const MainWindow *mw)
{
    if (mw == NULL || mw->selected_id[0] == '\0') return NULL;
    return mw->selected_id;
}

unsigned long long mainwindow_selected_window(const MainWindow *mw)
{
    return (mw == NULL) ? 0ULL : mw->sel_window;
}

const char *mainwindow_selected_preset_id(const MainWindow *mw)
{
    if (mw == NULL || mw->sel_preset[0] == '\0') return NULL;
    return mw->sel_preset;
}

const char *mainwindow_editor_rule_id(const MainWindow *mw)
{
    if (mw == NULL || !mw->ed_open || mw->ed_rule_id[0] == '\0') return NULL;
    return mw->ed_rule_id;
}

unsigned long long mainwindow_target_window(const MainWindow *mw)
{
    if (mw == NULL || mw->cb_target == NULL) return 0ULL;

    const LRESULT sel = SendMessageW(mw->cb_target, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) return 0ULL;

    const LRESULT idx = SendMessageW(mw->cb_target, CB_GETITEMDATA, (WPARAM)sel, 0);
    if (idx == CB_ERR || idx < 0 || idx >= (LRESULT)mw->win_count) return 0ULL;

    return mw->win_rows[idx].hwnd;
}

void mainwindow_show_create_page(MainWindow *mw, const MwWindowRow *rows, int count)
{
    if (mw == NULL) return;

    mw->create_count = 0;
    if (rows != NULL) {
        const int n = (count < MW_CREATE_MAX) ? count : MW_CREATE_MAX;
        for (int i = 0; i < n; ++i) {
            mw->create_rows[i] = rows[i];
        }
        mw->create_count = n;
    }
    mw->create_pick = 0ULL;
    mw->create_pick_row = -1;

    if (mw->create_list != NULL) {
        SendMessageW(mw->create_list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(mw->create_list);

        for (int i = 0; i < mw->create_count; ++i) {
            MwWindowRow *r = &mw->create_rows[i];

            LVITEMW item;
            ZeroMemory(&item, sizeof(item));
            item.mask    = LVIF_TEXT;
            item.iItem   = i;
            item.pszText = r->app;
            const int row = (int)SendMessageW(mw->create_list, LVM_INSERTITEMW, 0,
                                              (LPARAM)&item);
            if (row < 0) continue;

            ListView_SetItemText(mw->create_list, row, 1, r->title);
            ListView_SetItemText(mw->create_list, row, 2, r->state);
            ListView_SetItemText(mw->create_list, row, 3, r->size);
        }

        if (mw->create_count == 0) list_show_empty(mw->create_list, STR_NO_WINDOWS);

        SendMessageW(mw->create_list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(mw->create_list, NULL, TRUE);

        /* Preselect the first row so [Create] is usable without a click. */
        if (mw->create_count > 0) {
            select_first_row(mw->create_list);
            mw->create_pick = mw->create_rows[0].hwnd;
            mw->create_pick_row = 0;
        }
    }

    mw->create_open = TRUE;

    /*
     * nav_push(), NOT nav_go().
     *
     * This is a SUB-page of the preset page. The top-level navigation now
     * REPLACES the stack (nav_set), so using it here dropped the preset page
     * out from under the picker: Back then landed on the overview instead of
     * the page the user came from, and the editor opened on top of a page it
     * does not belong to - overlapping the overview's own controls.
     */
    nav_push(&mw->nav, PAGE_CREATE_PRESET);
    apply_page(mw);
}

unsigned long long mainwindow_create_pick(const MainWindow *mw)
{
    return (mw == NULL) ? 0ULL : mw->create_pick;
}

const wchar_t *mainwindow_create_pick_path(const MainWindow *mw)
{
    if (mw == NULL || mw->create_pick_row < 0 ||
        mw->create_pick_row >= mw->create_count) {
        return NULL;
    }
    const wchar_t *p = mw->create_rows[mw->create_pick_row].path;
    return (p[0] != L'\0') ? p : NULL;
}

BOOL mainwindow_create_page_open(const MainWindow *mw)
{
    return (mw != NULL && mw->create_open) ? TRUE : FALSE;
}

void mainwindow_show_create_page_indexing(MainWindow *mw)
{
    if (mw == NULL) return;

    /* The ordinary page, then its empty-list line replaced: the list is empty
     * because the index is still being built, not because there is nothing to
     * choose from - and those two must not look the same. */
    mainwindow_show_create_page(mw, NULL, 0);

    if (mw->create_list != NULL) {
        list_show_empty(mw->create_list, STR_MSG_INDEXING);
    }
}

void mainwindow_close_create_page(MainWindow *mw)
{
    if (mw == NULL || !mw->create_open) return;

    mw->create_open = FALSE;
    mw->create_pick = 0ULL;
    mw->create_pick_row = -1;
    nav_back(mw);
}

BOOL mainwindow_editor_values(const MainWindow *mw, double *x, double *y,
                              double *w, double *h)
{
    if (mw == NULL || !mw->ed_open) return FALSE;
    if (x == NULL || y == NULL || w == NULL || h == NULL) return FALSE;

    double v[EDIT_FIELD_COUNT];
    if (!read_fields((MainWindow *)mw, v)) return FALSE;

    /* The zone also has to make sense as a rectangle: four parseable numbers
     * can still describe one that hangs off the screen, and saving that would
     * look like the tool did nothing the next time it was applied. */
    if (!uiformat_zone_valid(v[EDIT_FIELD_X], v[EDIT_FIELD_Y],
                             v[EDIT_FIELD_W], v[EDIT_FIELD_H])) {
        return FALSE;
    }

    *x = v[EDIT_FIELD_X];
    *y = v[EDIT_FIELD_Y];
    *w = v[EDIT_FIELD_W];
    *h = v[EDIT_FIELD_H];
    return TRUE;
}

void mainwindow_editor_close(MainWindow *mw)
{
    editor_close(mw);
}

BOOL mainwindow_editor_pin_wanted(const MainWindow *mw)
{
    if (mw == NULL || mw->ed_pin == NULL) return FALSE;
    return (SendMessageW(mw->ed_pin, BM_GETCHECK, 0, 0) == BST_CHECKED) ? TRUE : FALSE;
}

BOOL mainwindow_editor_open_for(MainWindow *mw, const char *rule_id)
{
    if (mw == NULL || rule_id == NULL) return FALSE;

    const MwPresetRow *row = find_preset_row(mw, rule_id);
    if (row == NULL) return FALSE;

    editor_open(mw, row);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Settings page                                                       */
/* ------------------------------------------------------------------ */

void mainwindow_settings_load(MainWindow *mw, const Settings *s)
{
    if (mw == NULL) return;
    page_settings_load(mw->settings_page, s);
}

void mainwindow_settings_read(const MainWindow *mw, Settings *out)
{
    if (mw == NULL) return;
    page_settings_read(mw->settings_page, out);
}

/* ------------------------------------------------------------------ */
/* The workspace page's slot list                                      */
/* ------------------------------------------------------------------ */

void mainwindow_set_slot_rows(MainWindow *mw, const MwSlotRow *rows, int count)
{
    if (mw == NULL) return;

    mw->slot_row_count = 0;
    if (rows != NULL) {
        const int n = (count < MW_SLOT_MAX) ? count : MW_SLOT_MAX;
        for (int i = 0; i < n; ++i) mw->slot_rows[i] = rows[i];
        mw->slot_row_count = n;
    }

    if (mw->sec_slots != NULL) {
        set_section_text(mw->sec_slots, STR_SEC_SLOTS_FMT, mw->slot_row_count);
    }

    if (mw->slot_list == NULL) return;

    SendMessageW(mw->slot_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(mw->slot_list);

    for (int i = 0; i < mw->slot_row_count; ++i) {
        const MwSlotRow *r = &mw->slot_rows[i];

        /* "1. Notepad" - the position matters because a sequential workspace
         * runs them in that order, and the row order IS the run order. */
        wchar_t text[MW_TEXT_MAX + 16];
        _snwprintf_s(text, MW_TEXT_MAX + 16, _TRUNCATE, L"%ls. %ls",
                     r->order, r->preset);

        LVITEMW item;
        ZeroMemory(&item, sizeof(item));
        item.mask    = LVIF_TEXT;
        item.iItem   = i;
        item.pszText = text;
        SendMessageW(mw->slot_list, LVM_INSERTITEMW, 0, (LPARAM)&item);

        /* Column 1: the monitor binding (stage 7). Left blank for an unbound
         * slot and skipped entirely rather than writing an empty string - an
         * empty cell and a never-written one look identical to the user, so
         * this keeps the list to one code path. */
        if (r->monitor[0] != L'\0') {
            LVITEMW sub;
            ZeroMemory(&sub, sizeof(sub));
            sub.mask     = LVIF_TEXT;
            sub.iItem    = i;
            sub.iSubItem = 1;
            sub.pszText  = (LPWSTR)r->monitor;
            SendMessageW(mw->slot_list, LVM_SETITEMW, 0, (LPARAM)&sub);
        }
    }

    if (mw->slot_row_count == 0) list_show_empty(mw->slot_list, STR_NO_PRESETS);

    SendMessageW(mw->slot_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(mw->slot_list, NULL, TRUE);

    /* Selected to start with, so [Remove selected preset] is usable without a
     * click - the same thing the other lists do. */
    if (mw->slot_row_count > 0) select_first_row(mw->slot_list);

    update_button_states(mw);
}

int mainwindow_selected_slot(const MainWindow *mw)
{
    if (mw == NULL || mw->slot_list == NULL) return -1;

    const int row = ListView_GetNextItem(mw->slot_list, -1, LVNI_SELECTED);
    return (row >= 0 && row < mw->slot_row_count) ? row : -1;
}

void mainwindow_set_busy(MainWindow *mw, BOOL busy)
{
    if (mw == NULL) return;
    mw->busy = busy;
    update_button_states(mw);
}

HWND mainwindow_hwnd(const MainWindow *mw)
{
    return mw ? mw->hwnd : NULL;
}

BOOL mainwindow_translate_accel(MainWindow *mw, MSG *msg)
{
    if (mw == NULL || msg == NULL || mw->hwnd == NULL || mw->accel == NULL) {
        return FALSE;
    }

    /*
     * There is deliberately NO Alt+Left accelerator.
     *
     * It was implemented (handling both the main window and its child controls,
     * consuming the message so the system would not beep) and then removed at
     * the user's request: on the real machine it still produced the system's
     * "unhandled key" sound with no visible effect. The [Back] button on the
     * page title row does the same job with something the user can see and
     * click, so the shortcut was not worth more digging.
     *
     * Left here as a note so the next person does not re-add it as an
     * "obvious improvement".
     */

    /*
     * TranslateAcceleratorW sends WM_COMMAND straight to the window when a key
     * matches, and returns 0 when it does not - in which case the caller must
     * carry on with TranslateMessage/DispatchMessage.
     *
     * Menu items that are currently greyed out are not translated, so a
     * disabled action stays unreachable by its shortcut as well. That is the
     * behaviour we want: the window never offers an action that would do
     * nothing.
     */
    return TranslateAcceleratorW(mw->hwnd, mw->accel, msg) != 0;
}
