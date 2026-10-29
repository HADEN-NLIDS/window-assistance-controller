/*
 * mainwindow.h - the main window, which is this program's primary entry point.
 *
 * DESIGN DECISION (see docs/分段式编程计划.md, stage 2)
 *
 *   The main window is THE entry point, not a tray menu. Editing presets is
 *   a frequent action, and making the user right-click a tray icon for every
 *   one of them is too deep a path. So: double-click the exe, get a normal
 *   window, everything reachable from it. The tray icon is an optional
 *   convenience for keeping the hotkey alive while minimised.
 *
 * WHAT IT OWNS
 *
 *   The window, its menu, the workspace list, the button row and the status
 *   bar. It does NOT own the workspace data: the caller keeps the
 *   WorkspaceSet and hands it over with mainwindow_set_workspaces(). That
 *   keeps persistence decisions out of the UI.
 *
 * LIFETIME
 *
 *   Created once at startup and lives for the whole run (possibly hidden).
 *   It is deliberately NOT part of the "edit mode" resources that
 *   release_edit_resources() tears down: rebuilding a window every time the
 *   user leaves an edit session would be slower and leak-prone. See
 *   docs/架构与数据结构.md section 1.2.
 */

#ifndef WAC_MAINWINDOW_H
#define WAC_MAINWINDOW_H

#include <windows.h>

#include "page_settings.h"   /* Settings - the window carries it for the page */
#include "workspace.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Commands the window sends to its owner.
 *
 * The window does not start workspaces or open the editor itself. It reports
 * what the user asked for, and main.c decides what that means - which keeps
 * the state machine in one place.
 *
 * There is deliberately no MW_CMD_SETTINGS any more: settings are a PAGE, so
 * reaching them is navigation (nav_go) rather than something the owner has to
 * be asked for.
 */
typedef enum {
    MW_CMD_NONE = 0,
    MW_CMD_APPLY,          /* run the selected workspace */
    MW_CMD_EDIT,           /* "Edit" - open the preset editor */
    MW_CMD_ADD_SLOT,       /* add the overview's selected preset to the
                            * selected workspace */
    MW_CMD_REMOVE_SLOT,    /* take the selected preset out of it again */
    MW_CMD_UNBIND_SLOT,    /* forget which monitor the selected slot belongs to
                            * (stage 7). The slot then applies wherever its
                            * window turns up, exactly as before the feature */
    MW_CMD_EDIT_OVERLAY,   /* enter the full-screen edit mode for the
                            * selected workspace (stage 6) */
    MW_CMD_CLOSE_WORKSPACE,/* stop the applications of this workspace */
    MW_CMD_NEW,            /* create a workspace */
    MW_CMD_RENAME,
    MW_CMD_DELETE,
    MW_CMD_QUIT,
    MW_CMD_ABOUT,
    MW_CMD_REFRESH,        /* re-read what is on screen; no state change */
    MW_CMD_ANCHOR,         /* save a preset for the selected window */
    MW_CMD_MOVE_TO_MONITOR,/* move every window matching the SELECTED preset to
                            * the monitor selected in the overview's monitor
                            * box. One shot: it places windows and stores
                            * nothing (2026-10-09, user request - "把选中预设
                            * 全部移动到主屏"; the target became the SELECTED
                            * monitor on 2026-10-10, hence the name) */
    MW_CMD_DELETE_PRESET,  /* remove the selected preset */
    MW_CMD_APPLY_ALL,      /* put every matching window back */
    MW_CMD_SAVE_PRESET,    /* write the editor's values back to the preset */
    MW_CMD_CREATE_PRESET,  /* create from the snapshot page's selection */
    MW_CMD_BEGIN_CREATE,   /* take a window snapshot and open the picker */
    MW_CMD_BROWSE_APPS,    /* [More...] on that page: the installed-application
                            * list (stage 8). It has no window to anchor, so
                            * main.c creates a preset with a starting zone */
    MW_CMD_SETTINGS_APPLY, /* write the settings page back to the config file */
    MW_CMD_SETTINGS_CANCEL,/* discard what is on the settings page */
    MW_CMD_OPEN_STARTUP    /* open the Startup folder in Explorer (guide page) */
} MainWindowCommand;

typedef void (*MainWindowCmdFn)(MainWindowCommand cmd, const char *workspace_id,
                                void *user);

typedef struct MainWindow MainWindow;

/*
 * Register the window class. Safe to call more than once; returns FALSE only
 * if registration genuinely failed.
 */
BOOL mainwindow_register_class(HINSTANCE hinst);

/* Create the window. Returns NULL on failure. The window starts hidden. */
MainWindow *mainwindow_create(HINSTANCE hinst, MainWindowCmdFn on_command,
                              void *user);

void mainwindow_destroy(MainWindow *mw);

/* ---- visibility --------------------------------------------------- */

void mainwindow_show(MainWindow *mw);
void mainwindow_hide(MainWindow *mw);
BOOL mainwindow_is_visible(const MainWindow *mw);

/*
 * Bring an existing window to the front, restoring it if minimised.
 * Used by the single-instance path: a second launch must surface the
 * running instance rather than silently doing nothing.
 */
void mainwindow_activate(MainWindow *mw);

/* ---- content ------------------------------------------------------ */

/*
 * Replace the displayed list. The window copies what it needs and does not
 * keep the pointer, so the caller may free or mutate the set afterwards.
 */
void mainwindow_set_workspaces(MainWindow *mw, const WorkspaceSet *set);

/*
 * Suppress prompts (2026-10-10).
 *
 * Set by main.c while the Windows session is ending. The program must never ask
 * a question at that moment - a modal box during shutdown is what makes Windows
 * tell the user that this application is preventing shutdown - so the editor's
 * "discard unsaved changes?" becomes an automatic yes and the caller saves and
 * goes. Nothing else about the window changes.
 */
void mainwindow_set_prompt_suppressed(MainWindow *mw, BOOL suppressed);
/* Status bar text. NULL or empty clears the pane. */
void mainwindow_set_status(MainWindow *mw, const wchar_t *text);

/* Mark a workspace row as "currently applied", or pass NULL to clear.
 * Drawn as a leading marker so the user can see what is running. */
void mainwindow_set_active(MainWindow *mw, const char *workspace_id);

/* Id of the selected row, or NULL when nothing is selected.
 * The returned pointer is owned by the window and valid until the selection
 * changes. */
const char *mainwindow_selected_id(const MainWindow *mw);

/* Enable or disable the action buttons (used while a launch is in flight). */
void mainwindow_set_busy(MainWindow *mw, BOOL busy);

/* ---- overview page rows ------------------------------------------- */

/*
 * Rows for the two overview lists.
 *
 * The CALLER assembles them. The window deliberately knows nothing about
 * win_enum, rules or monitors: it receives text and shows it. That keeps the
 * "what should this list say" logic testable on its own instead of buried in a
 * window procedure, and it is why this header does not include rule.h.
 */
#define MW_ROW_MAX   64
#define MW_APP_MAX   64
#define MW_TEXT_MAX  128

/*
 * The preset picker gets a much larger ceiling than the overview lists.
 *
 * A machine has a few hundred installed applications (343 on the development
 * machine) against a dozen windows, and the picker is where the user looks for
 * one by name. 256 covers every entry that can actually be launched here, with
 * room to spare; anything beyond it is reported rather than silently dropped.
 *
 * The cost is array size, not speed: a row is ~700 bytes, so this is ~180 KB
 * inside MainWindow, which is allocated once.
 */
#define MW_CREATE_MAX 256

typedef struct {
    wchar_t app[MW_APP_MAX];      /* executable file name */
    wchar_t title[MW_TEXT_MAX];   /* window caption */
    wchar_t state[MW_TEXT_MAX];   /* matched preset, or "no preset" */
    wchar_t size[48];             /* "1280x1600" */
    unsigned long long hwnd;      /* kept for the actions added in a later batch */

    /*
     * Full path of an executable, for a row that describes a program which is
     * NOT running (the installed-application list, stage 8). Empty for a row
     * built from a live window - those are anchored by hwnd instead.
     *
     * The two cases are told apart by which of the two is filled in, and the
     * create path branches on exactly that: a live window is anchored where it
     * is, a program that is not running gets a preset with a default zone for
     * the user to edit.
     */
    wchar_t path[MW_TEXT_MAX * 2];
} MwWindowRow;

typedef struct {
    wchar_t app[MW_APP_MAX];      /* executable file name */
    wchar_t label[MW_TEXT_MAX];   /* display name */
    wchar_t zone[64];             /* normalized zone, formatted for reading */
    wchar_t monitor[64];          /* monitor the size was measured on */
    wchar_t state[32];            /* active / disabled */
    char    id[64];               /* rule id, for the actions added later */

    /*
     * The same zone as numbers, for the hand-editing panel.
     *
     * The formatted string above is for the list; the editor needs the actual
     * values, and re-parsing the display text would make the round trip
     * lossy - "23.5%" does not carry four decimals of precision.
     */
    double  x, y, w, h;

    /*
     * TRUE when the preset has learned that its window refuses to be resized
     * (rule.zone.lock_size). The editor then disables the two size fields and
     * says why; the VALUES stay exactly as they were, because the numbers are
     * still the truth about where the window goes - they are just no longer
     * something the user can change here (2026-10-10, task B).
     */
    BOOL    locked_size;

    /*
     * TRUE when the preset names a screen (rule.mon_set): its windows are sent
     * there wherever they open, instead of being placed relative to whichever
     * screen they turn up on (2026-10-10).
     */
    BOOL    pinned;
} MwPresetRow;

/*
 * Replace the contents of the two overview lists. Both are rebuilt wholesale,
 * for the same reason the workspace list is: with a few dozen rows the cost is
 * irrelevant and a rebuild cannot leave a stale row behind.
 */
void mainwindow_set_window_rows(MainWindow *mw, const MwWindowRow *rows, int count);
void mainwindow_set_preset_rows(MainWindow *mw, const MwPresetRow *rows, int count);

/*
 * Current selections on the overview page.
 *
 * The window id is the HWND the row was built from (0 when nothing is
 * selected); the preset id is the rule id, or NULL. Both survive a reload:
 * the lists are rebuilt wholesale on every refresh, and a row INDEX would
 * point at a different row afterwards.
 */
unsigned long long mainwindow_selected_window(const MainWindow *mw);
const char *mainwindow_selected_preset_id(const MainWindow *mw);

/*
 * The monitor the overview's monitor box has selected, as an index into the
 * CURRENT monitor_enumerate() order - or -1 when there is nothing usable to
 * act on (no selection, an unpopulated box, no monitors, a stale index).
 *
 * Until 2026-10-10 the box only chose the shape the editor's preview draws
 * (editor_preview_monitor); the decision itself is monitor_selection_index()
 * in monitor.h, so both readers mean the same thing by "the selected monitor".
 * A caller that gets -1 must report that, never quietly substitute the primary
 * - the control says "the selected monitor", and swapping in another one is
 * the same "it did not take effect" the user reported.
 */
int mainwindow_selected_monitor_index(const MainWindow *mw);

/*
 * The [Create preset] picker page.
 *
 * It shows a SNAPSHOT taken when the button was pressed, not a live list:
 * picking a window means "remember where it is right now", and a list that
 * reshuffles while the user reads it would make that promise meaningless.
 *
 * The caller enumerates the windows and hands the rows over.
 */
void mainwindow_show_create_page(MainWindow *mw, const MwWindowRow *rows, int count);

/*
 * The same page, opened while the application index is still being built.
 *
 * An empty list with an explanation beats a blank page: the user pressed a
 * button and has to see that something is happening. The worker's completion
 * message replaces the contents when it arrives.
 */
void mainwindow_show_create_page_indexing(MainWindow *mw);

/* The executable path of the selected row, or NULL. See MwWindowRow.path. */
const wchar_t *mainwindow_create_pick_path(const MainWindow *mw);

/* TRUE while the picker page is on screen: the index worker's completion
 * message uses it to decide whether the list it filled is still being looked
 * at, or whether the user has moved on and nothing should be redrawn. */
BOOL mainwindow_create_page_open(const MainWindow *mw);

/* The window chosen on the picker page, or 0. */
unsigned long long mainwindow_create_pick(const MainWindow *mw);

/* Leave the picker page (after creating, or on Cancel). */
void mainwindow_close_create_page(MainWindow *mw);

/*
 * The window chosen in the [Create preset] target box, or 0 when the box is
 * empty. The combo is filled from the same rows the overview list shows.
 */
unsigned long long mainwindow_target_window(const MainWindow *mw);

/* ---- the hand-editing panel --------------------------------------- */

/*
 * The preset currently open in the editor, or NULL when it is closed.
 * The pointer is owned by the window and valid until the editor closes.
 */
const char *mainwindow_editor_rule_id(const MainWindow *mw);

/*
 * Read the four coordinate fields as normalized values.
 *
 * Returns FALSE when any field cannot be parsed - the caller must say so
 * rather than save a partly-guessed rectangle. Nothing is written to `out` in
 * that case.
 */
BOOL mainwindow_editor_values(const MainWindow *mw, double *x, double *y,
                              double *w, double *h);

/* Close the editor (after a successful save, or on Cancel). */
void mainwindow_editor_close(MainWindow *mw);

/*
 * TRUE when the editor's [pin] box is ticked - "this preset names a screen".
 *
 * The SCREEN itself is not returned here on purpose: it is whatever the
 * monitor box selects, which the caller reads with
 * mainwindow_selected_monitor_index(). Two questions, two answers, no third
 * place where a monitor choice could hide.
 */
BOOL mainwindow_editor_pin_wanted(const MainWindow *mw);

/*
 * Open the editor on the preset with this rule id.
 *
 * Used right after creating one: picking a window says "this program belongs
 * somewhere", and where it happens to be at that moment is rarely the answer -
 * so the user is put straight into the fields that decide it.
 *
 * Returns FALSE when no such preset exists (the caller just created it, so
 * that means something went wrong upstream).
 */
BOOL mainwindow_editor_open_for(MainWindow *mw, const char *rule_id);

/* Native handle, for message routing and for the file dialog owner. */
HWND mainwindow_hwnd(const MainWindow *mw);

/*
 * Rows for the workspace page's second list: the presets that make up the
 * SELECTED workspace.
 *
 * Only the window shows those, and only for one workspace at a time - the one
 * highlighted in the list above. The caller rebuilds them whenever the
 * selection or the workspace changes.
 */
#define MW_SLOT_WHY_MAX 320   /* room for the blocked-row sentence (task 7.3) */
#define MW_SLOT_MAX 16

typedef struct {
    wchar_t preset[MW_TEXT_MAX];   /* display name of the preset */
    wchar_t order[16];             /* its position, as text */
    char    preset_id[64];         /* what to remove */
    wchar_t monitor[MW_TEXT_MAX];  /* stage-7 monitor binding; empty = unbound */

    /*
     * Stage 7 / task 7.3 - WHY this row cannot be used right now, as a finished
     * sentence. Empty means usable.
     *
     * main.c composes it (it owns the slot and its monitor binding); the list
     * greys a row whose text is not empty and shows this text when the user
     * clicks it. The point of putting the reason in the row rather than
     * recomputing it here is that the list and the message can then never
     * disagree about which row is the broken one.
     */
    wchar_t blocked[MW_SLOT_WHY_MAX];
} MwSlotRow;

void mainwindow_set_slot_rows(MainWindow *mw, const MwSlotRow *rows, int count);

/* Index of the selected row in that list, or -1. */
int mainwindow_selected_slot(const MainWindow *mw);

/* ---- the settings page --------------------------------------------- */

/*
 * Fill the settings page from `s`.
 *
 * Called at startup and after the settings were saved. The page keeps a copy,
 * so leaving it without pressing [Apply] can put the controls back - which is
 * what makes Cancel mean cancel.
 */
void mainwindow_settings_load(MainWindow *mw, const Settings *s);

/*
 * Read what the user picked on the settings page. `out` is filled in place:
 * only the fields the page can change are overwritten, so the caller's copy of
 * everything else (the hotkey label) survives.
 */
void mainwindow_settings_read(const MainWindow *mw, Settings *out);

/* ---- keyboard ------------------------------------------------------ */

/*
 * Give the window a chance to consume an accelerator key.
 *
 * Call this from the message loop BEFORE TranslateMessage/DispatchMessage;
 * it returns TRUE when the message was translated (the window then receives
 * the matching WM_COMMAND) and FALSE when the caller should keep processing
 * the message normally.
 *
 * WHY IT EXISTS: the menu captions advertise Ctrl+N, F2, Del and F5, but a
 * caption is only text - without an accelerator table those keys do nothing at
 * all. The menu was promising shortcuts that had never worked
 * (TRAPS.md #24).
 *
 * Disabled menu items are not translated, which is deliberate: an action that
 * is greyed out must not be reachable by its shortcut either.
 */
BOOL mainwindow_translate_accel(MainWindow *mw, MSG *msg);

#ifdef __cplusplus
}
#endif

#endif /* WAC_MAINWINDOW_H */
