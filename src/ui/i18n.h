/*
 * i18n.h - user-interface text, in one place, in more than one language.
 *
 * WHY A TABLE INSTEAD OF LITERALS
 *
 *   Every user-visible string in this program goes through i18n_text(). That
 *   is more work than writing L"Settings" inline, and it is worth it for two
 *   reasons: a translator can see every string without reading the logic,
 *   and adding a language touches one file rather than thirty call sites.
 *
 * THE X-MACRO
 *
 *   The list below is expanded twice - once into the StrId enum, once into
 *   the table in i18n.c. Adding a string is a one-line change, and the enum
 *   and the table cannot drift apart because neither is written by hand.
 *
 *   When adding an ID: put it in the block it belongs to and give it both an
 *   English and a Chinese form in i18n.c. A missing entry is a compile error,
 *   not a runtime blank, because the table is built from designated
 *   initialisers and the array has a fixed size.
 *
 * WHERE THE TEXT LIVES
 *
 *   In i18n.c as ordinary C string literals, compiled with /utf-8. NOT in the
 *   .rc file: rc.exe needs /c65001 for UTF-8 and the failure mode when that
 *   flag is missing is a misleading parse error far from the real line (see
 *   TRAPS.md #14). Keeping translatable text in C keeps one
 *   encoding rule for the whole project.
 *
 * FALLBACK
 *
 *   If a string is missing in the selected language, the English form is
 *   used. A half-translated build shows English in the gaps rather than
 *   blanks or ID numbers.
 */

#ifndef WAC_I18N_H
#define WAC_I18N_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* The string list                                                     */
/*                                                                     */
/* I18N_STRINGS(X) - X(id, english)                                    */
/*                                                                     */
/* Grouped by where the string appears, so a translator can work one   */
/* dialog at a time.                                                   */
/* ------------------------------------------------------------------ */

/*
 * TWO KINDS OF STRING - keep them apart.
 *
 *   I18N_TEMPLATES(X)  contain placeholders. They are ONLY valid as the
 *                      FORMAT argument of a printf-family call. Passing one
 *                      as a %ls ARGUMENT prints its placeholder literally -
 *                      which is exactly how the literal "%hs" sat in the
 *                      status bar (TRAPS.md #23): the template
 *                      for STR_STATUS_HOTKEY was handed over as an argument
 *                      instead of being used as the format.
 *
 *   I18N_STRINGS(X)    are finished text, displayed as-is.
 *
 * PLACEHOLDER RULE: %ls (wide string) and %d (int) only. Never %hs / %s -
 * those take a narrow char*, which would force every call site to convert
 * first. One rule means the argument type is never in doubt, and the rule is
 * checked mechanically: tests/t14_i18nfmt.c walks I18N_TEMPLATES and fails on
 * any other specifier, so a new template with the wrong one breaks the test
 * run instead of the user's screen.
 */
#define I18N_TEMPLATES(X)                                                   \
    X(STATUS_ACTIVE,        "Active: %ls")                                  \
    X(STATUS_HOTKEY,        "exit hotkey: %ls")                             \
    X(MSG_CONFIRM_DELETE,   "Delete the workspace \"%ls\"?")                \
    X(MSG_ABOUT_WSCOUNT,    "Workspaces: %d")                               \
    X(MSG_ABOUT_HOTKEY,     "Exit hotkey: %ls")                             \
    X(OVERVIEW_WINDOWS,     "Running windows (%d)")                         \
    X(OVERVIEW_PRESETS,     "Window presets (%d)")                          \
    X(MATCH_OK,             "matched \"%ls\"")                              \
    X(MSG_CONFIRM_DEL_PRESET, "Delete the preset \"%ls\"?")                 \
    X(MSG_ANCHORED,         "Preset saved for \"%ls\"")                     \
    X(MSG_APPLIED_FMT,      "%d placed, %d denied (elevated), %d failed")   \
    X(EDIT_PIXELS,          "pixels: %d,%d  %dx%d")                         \
    X(EDIT_NORMALIZED,      "stored: %ls")                                  \
    X(EDIT_REAL_FMT,        "window: %ls  (%ls of the screen)")             \
    X(PIN_SET_FMT,          "pinned to %ls")                                \
    X(MSG_PIN_GONE_FMT,     "%d not placed: the screen they are pinned to is not connected") \
    X(EDIT_TARGET_FMT,      "Target: %ls")                                  \
    X(STATUS_PERM,          "rights: %ls")                                  \
    X(MSG_CONFIRM_CLOSE_WS, "End the programs \"%ls\" started?")            \
    X(SEC_SLOTS_FMT,        "Presets in this workspace (%d)")               \
    X(EDIT_EXIT_FMT,        "Left edit mode (%ls)")                         \
    X(MONITOR_ITEM,         "%ls  %dx%d")                                   \
    X(MSG_RUN_DONE,         "%d placed, %d denied (elevated), %d failed, %d skipped") \
    X(MSG_MOVED_FMT,        "%d moved to the selected monitor, %d denied, %d failed") \
    X(MSG_SIZE_LOCKED_FMT,  "\"%ls\" cannot be resized - it will only be moved from now on") \
    X(MSG_SLOTS_UNAVAILABLE_FMT, "%d slot(s) unusable: the monitor they were recorded on is not connected") \
    X(MSG_SLOT_BLOCKED_FMT, "This entry was recorded on a monitor that is not connected now.\n\n" \
                            "Monitor: %ls  (%dx%d at %d,%d)\n\n" \
                            "What this means: the workspace skips this entry when it runs. " \
                            "The preset and the stored position are NOT changed.\n\n" \
                            "What you can do: plug that monitor back in, or use " \
                            "[Unbind monitor] so the window is placed wherever it opens.") \
    X(SIDEBAR_COUNT_FMT,    "%d applications")                              \
    X(EDIT_OVERLAY_TITLE,   "Edit mode  -  monitor %d   %dx%d")             \
    X(EDIT_OVERLAY_RECT_FMT,"overlay %d,%d - %d,%d   (rcMonitor, taskbar included)") \
    X(PLACEHOLDER_SIZE_FMT, "%d x %d")                                      \
    X(PLACEHOLDER_LAST_FMT, "%d x %d   (last placed: %d x %d)")             \
    X(PLACEHOLDER_MORE_FMT, "%d more preset(s) do not fit on this screen")  \
    X(MSG_EDIT_APPLIED_FMT, "%d preset(s) updated from the canvas")         \
    X(MSG_EDIT_PICKED_FMT,  "%d preset(s) picked up from the screen")       \

/* The enum and both tables expand this: templates first, then finished text. */
#define I18N_ALL(X)   I18N_TEMPLATES(X) I18N_STRINGS(X)
#define I18N_STRINGS(X)                                                     \
    /* ---- main window: title, menu, columns -------------------- */       \
    X(APP_TITLE,            "Window Assistance Controller")                 \
    X(COL_WORKSPACE,        "Workspace")                                    \
    X(COL_APPS,             "Apps")                                         \
    X(COL_ID,               "Id")                                           \
                                                                            \
    X(MENU_FILE,            "&File")                                        \
    X(MENU_WORKSPACE,       "&Workspace")                                   \
    X(MENU_VIEW,            "&View")                                        \
    X(MENU_HELP,            "&Help")                                        \
                                                                            \
    X(MENU_NEW,             "&New workspace...\tCtrl+N")                    \
    X(MENU_RENAME,          "Re&name...\tF2")                               \
    X(MENU_DELETE,          "&Delete\tDel")                                 \
    X(MENU_SETTINGS,        "&Settings...")                                 \
    X(MENU_EXIT,            "E&xit\tAlt+F4")                                \
                                                                            \
    X(MENU_APPLY,           "&Apply\tEnter")                                \
    X(MENU_EDIT,            "&Edit layout...")                              \
    X(MENU_CLOSE_WS,        "&Close its applications")                      \
                                                                            \
    X(MENU_REFRESH,         "&Refresh\tF5")                                 \
    X(MENU_ABOUT,           "&About")                                       \
                                                                            \
    /* ---- main window: buttons -------------------------------- */       \
    X(BTN_APPLY,            "Apply")                                        \
    X(BTN_EDIT,             "Edit")                                         \
    X(BTN_CLOSE,            "Close")                                        \
    X(BTN_NEW,              "New")                                          \
    X(BTN_DELETE,           "Delete")                                       \
    X(BTN_SETTINGS,         "Settings")                                     \
    X(BTN_RUN_WORKSPACE,    "Run")                                          \
    X(BTN_ADD_SLOT,         "Add selected preset")                          \
    X(BTN_NEW_WORKSPACE,    "New workspace")                                \
    X(BTN_DELETE_WORKSPACE, "Delete workspace")                             \
    X(BTN_CLOSE_APPS,       "Close its applications")                       \
    X(BTN_REMOVE_SLOT,      "Remove selected preset")                       \
    X(BTN_EDIT_WORKSPACE,   "Edit selected workspace")                      \
    X(BTN_RENAME_WORKSPACE, "Rename workspace")                             \
    X(BTN_UNBIND_SLOT,      "Unbind monitor")                               \
    X(MSG_NO_SLOT_SEL,      "Select a preset in the list below first.")     \
    X(MSG_SLOT_REMOVED,     "Removed from the workspace.")                  \
    X(MSG_SLOT_UNBOUND,     "Monitor binding removed.")                     \
    X(MSG_WS_NO_PRESETS_AT_ALL, "There are no window presets at all yet.")  \
    X(MSG_NO_WORKSPACE_SEL, "Select a workspace in the list first.")        \
    X(MSG_NO_SLOT_PRESET,   "Select a window preset on the Overview page first.") \
    X(MSG_SLOT_ADDED,       "Added to the workspace.")                      \
    X(MSG_SLOT_EXISTS,      "That preset is already in this workspace.")    \
    X(MSG_WS_EMPTY_RUN,     "This workspace has no applications yet. Add a preset to it first.") \
    X(MSG_WS_NOTHING_TO_CLOSE, "Nothing recorded for this workspace - it was not started from here, or it has already been closed.") \
    X(MSG_WS_NO_SLOTS_ADD,  "There are no window presets yet. Anchor a window on the Overview page first.")                                     \
                                                                            \
    /* ---- main window: navigation strip ----------------------- */       \
    X(NAV_WINDOWS,          "Window presets")                               \
    X(NAV_WORKSPACES,       "Workspace presets")                            \
    X(NAV_REFRESH,          "Refresh")                                      \
    X(NAV_SETTINGS,         "Settings")                                     \
    X(NAV_HELP,             "User guide")                                   \
    X(BTN_BACK,             "< Back")                                       \
    X(PAGE_TODO,            "This page arrives in a later batch of stage 4.5.") \
                                                                            \
    /* ---- overview page: columns and states ------------------- */       \
    X(COL_APP,              "Application")                                  \
    X(COL_TITLE,            "Title")                                        \
    X(COL_STATE,            "State")                                        \
    X(COL_SIZE,             "Size")                                         \
    X(COL_ZONE,             "Zone")                                         \
    X(COL_MONITOR,          "Ref. width")                                   \
    X(COL_SCREEN,           "Screen")                                       \
    X(COL_PRESET,           "Preset")                                       \
    X(MATCH_NONE,           "no preset")                                    \
    X(NO_WINDOWS,           "no placeable windows right now")               \
    X(NO_PRESETS,           "no window presets yet")                        \
    X(PRESET_DISABLED,      "disabled")                                     \
    X(PRESET_ENABLED,       "active")                                       \
    X(BTN_ANCHOR,           "Anchor selected window")                       \
    X(BTN_MOVE_MONITOR,     "Move to the selected monitor")                 \
    X(BTN_DELETE_PRESET,    "Delete selected preset")                       \
    X(BTN_APPLY_ALL,        "Apply every preset")                           \
    X(BTN_EDIT_PRESET,      "Edit selected preset")                         \
    X(BTN_CREATE_PRESET,    "Create preset")                                \
    X(BTN_DELETE_PRESET2,   "Delete preset")                                \
    X(LABEL_TARGET_WINDOW,  "Target window")                                \
    X(LABEL_MONITOR_SEL,    "Monitor")                                      \
    X(LABEL_POSITION,       "Position")                                     \
    X(LABEL_SIZE,           "Size")                                         \
    X(MSG_CREATE_NO_WINDOW, "Choose a target window first.")                \
    X(BTN_SAVE,             "Save")                                         \
    X(LABEL_X,              "X")                                            \
    X(LABEL_Y,              "Y")                                            \
    X(LABEL_W,              "Width")                                        \
    X(LABEL_H,              "Height")                                       \
    X(EDIT_LOCKED,          "This window cannot be resized; it will only be moved.") \
    X(EDIT_REAL_NONE,       "window unavailable")                           \
    X(CHK_PIN_MONITOR,      "Pin to this screen")                           \
    X(PIN_NONE,             "not pinned: follows the window's own screen")  \
    X(PIN_NO_MONITOR,       "no screen to pin to")                          \
    X(MSG_EDIT_BAD_INPUT,   "Those numbers do not describe a valid zone.\n\nEach value is a percentage of the screen (0-100), and the rectangle has to fit inside it.") \
    X(MSG_EDIT_SAVED,       "Preset updated.")                              \
    X(MSG_DISCARD_EDITS,    "This preset has unsaved changes.\n\nClose the editor and lose them?") \
    X(MSG_CREATE_PICK,      "Pick one from the list, then press Create.") \
    X(MSG_CREATE_MORE_TODO, "Choosing a program that is not running yet needs the preset browser, which arrives with the fake-desktop stage.") \
    X(MSG_INDEXING,         "Indexing the installed applications... the result is cached, so this happens once") \
    X(MSG_INDEX_FAILED,     "The installed-application list could not be built.") \
    X(APP_NOT_RUNNING,      "not running")                                  \
    X(APP_UNLAUNCHABLE,     "no launch path")                               \
    X(APP_NO_PATH,          "-")                                            \
    /* ---- edit mode: the application sidebar (stage 8, task 2b) --- */     \
    X(SIDEBAR_TITLE,        "Applications")                                 \
    X(SIDEBAR_HINT,         "drag onto the canvas to place")                \
    X(SIDEBAR_INDEXING,     "indexing...")                                  \
    X(SIDEBAR_DROP_HINT,    "release to place")                             \
    X(SIDEBAR_EMPTY,        "No installed applications were found.")        \
    X(SIDEBAR_SEARCH,       "Search applications")                          \
    X(PLACEHOLDER_EMPTY,    "This workspace has no presets yet - drag one in from the right.") \
    X(MSG_EDIT_APPLIED_NONE,"Nothing on the canvas changed.")               \
    X(MSG_EDIT_RESET,       "Canvas reset to the stored layout.")           \
    X(MSG_EDIT_PICK_NONE,   "Nothing on this screen could be picked up.")   \
    X(MSG_EDIT_PICK_FAILED, "The picked-up presets could not be written.")  \
    /* ---- edit mode: the screen's own text (stage 6, translated 2026-10-10) */ \
    X(EDIT_OVERLAY_HINT1,   "Esc or Ctrl+Q   leave edit mode")              \
    X(EDIT_OVERLAY_HINT2,   "the global exit hotkey also works here")       \
    X(BTN_MORE,             "More...")                                      \
    X(BTN_CREATE,           "Create")                                       \
    X(PAGE_CREATE_TITLE,    "Create preset")                                \
    X(MSG_PRESET_GONE,      "That preset no longer exists; refresh the list.") \
    X(MSG_NO_PRESETS_YET,   "There are no presets to apply yet. Anchor a window first.") \
    X(MSG_APPLIED_NONE,     "No open window matched a preset.")             \
    X(MSG_MOVE_NONE,        "No window matches that preset right now.")     \
    X(MSG_NO_MONITOR_SEL,   "No monitor is selected. Pick one in the screen list first.") \
    X(MSG_NO_WINDOW_SEL,    "Select a window in the top list first.")       \
    X(MSG_NO_PRESET_SEL,    "Select a preset in the bottom list first.")    \
    X(MSG_ANCHOR_FAILED,    "The preset could not be saved.")               \
    X(MSG_ANCHOR_NOEXE,     "That window's executable could not be read, so no preset was saved.") \
    X(MSG_ANCHOR_MINIMIZED, "That window is minimised. Restore it first, then anchor it.") \
    X(MSG_RULES_WRITE_FAILED, "The preset was changed in memory but the settings file could not be written.") \
                                                                            \
    /* ---- status bar ------------------------------------------ */       \
    X(STATUS_NO_WORKSPACE,  "No workspace applied")                         \
    X(STATUS_HOTKEY_NONE,   "(not registered)")                             \
                                                                            \
    /* ---- tray ------------------------------------------------ */       \
    X(TRAY_SHOW,            "Show window")                                  \
    X(TRAY_QUIT,            "Quit")                                         \
    X(TRAY_TOOLTIP,         "Window Assistance Controller")                 \
                                                                            \
    /* ---- settings dialog ------------------------------------- */       \
    X(SETTINGS_TITLE,       "Settings")                                     \
    X(SETTINGS_LANGUAGE,    "Language")                                     \
    X(SETTINGS_LANG_FOLLOW, "Follow Windows")                               \
    X(SETTINGS_LANG_EN,     "English")                                      \
    X(SETTINGS_LANG_ZH,     "简体中文")                                     \
    X(SETTINGS_LANG_NOTE,   "Changes take effect after restarting the program.") \
    X(SETTINGS_CLOSE_BEHAV, "When the window is closed")                    \
    X(SETTINGS_CLOSE_ASK,   "Ask every time")                               \
    X(SETTINGS_CLOSE_QUIT,  "Quit the program")                             \
    X(SETTINGS_CLOSE_TRAY,  "Keep running (hide to tray)")                  \
    X(SETTINGS_HOTKEY,      "Exit hotkey")                                  \
    X(SETTINGS_HOTKEY_NOTE, "(fixed in this build)")                        \
    X(SETTINGS_OK,          "OK")                                           \
    X(SETTINGS_CANCEL,      "Cancel")                                       \
    X(SETTINGS_APPLY,       "Apply")                                        \
    X(RENAME_TITLE,         "Name")                                         \
    X(RENAME_LABEL,         "New name")                                     \
                                                                            \
    /* ---- settings page: permissions, the reserved listener switch ---- */ \
    X(SETTINGS_PERM_LABEL,  "Permissions")                                  \
    X(SETTINGS_PERM_ADMIN,  "running as administrator")                     \
    X(SETTINGS_PERM_NORMAL, "running as a standard user")                   \
    X(SETTINGS_PERM_NOTE_ADMIN, "Windows lets this program move the windows of other elevated programs as well.") \
    X(SETTINGS_PERM_NOTE_NORMAL, "Windows blocks a standard program from moving the windows of an elevated one - accelerators and driver tools are usually elevated. To control those: close this program and start it again with \"Run as administrator\". Do that only when you need it.") \
    X(SETTINGS_HOOK_LABEL,  "Put windows back as they appear (stay in the background)") \
    X(SETTINGS_HOOK_NOTE,   "Watches for windows as they open and puts the ones a preset matches back in place - including programs you start yourself. Windows tells it when a window appears, so it never polls or scans. The program has to be running for this; it can sit in the tray.") \
    X(SETTINGS_GUIDE,       "User guide")                                   \
                                                                            \
    /* ---- the edit toolbar (stage 6) --------------------------- */        \
    X(TB_EXIT,        "Leave edit mode")                                    \
    X(TB_RESET,       "Reset")                                              \
    X(TB_APPLY,       "Apply")                                              \
    X(TB_LAYOUT,      "Layout format")                                      \
    X(TB_TPL_HALVES,  "Halves")                                             \
    X(TB_TPL_THIRDS,  "Thirds")                                             \
    X(TB_TPL_2X2,     "2x2")                                                \
    X(TB_TPL_3X3,     "3x3")                                                \
    X(TB_CUSTOM,      "Custom")                                             \
    X(TB_PICK,        "Pick up this layout")                                \
                                                                            \
    /* ---- user-guide page -------------------------------------- */       \
    X(HELP_AUTOSTART_HEAD,  "How to start it with Windows:")                \
    X(HELP_AUTOSTART_STEPS, "Press [Open startup folder] below.\n\nRight-click an empty spot in that folder and choose New then Shortcut.\n\nFor the location enter the full path of wac.exe for example D:\\tools\\wac.exe\n\nChoose Next then any name then Finish.\n\nRestart Windows to confirm: the tray icon appears after you sign in.") \
    X(HELP_AUTOSTART_UNDO,  "To undo it:\n\nDelete that shortcut from the same folder.") \
    X(HELP_AUTOSTART_MANUAL, "If the button does not open anything: press Win+R and type shell:startup then press Enter.") \
    X(HELP_OPEN_STARTUP,    "Open startup folder")                          \
    X(HELP_PERM_HEAD,       "Some windows will not move (administrator rights)") \
    X(HELP_PERM_BODY,       "Those programs run elevated and Windows does not let a standard program move an elevated window. This is the security boundary of the system rather than a fault of this program.") \
    X(HELP_PERM_HOW,        "When you need to control such a window: close this program and then right-click wac.exe and choose Run as administrator.") \
    X(HELP_PERM_WARN,       "Ticking \"always run as administrator\" on a shortcut makes the program run elevated every time.\nDaily use - a notepad or a browser - never needs it.") \
    X(HELP_PERM_STATUS_ADMIN,  "Current status: running as administrator.")  \
    X(HELP_PERM_STATUS_NORMAL, "Current status: running as a standard user.") \
    X(HELP_CLI_HEAD,        "Command line")                                 \
    X(HELP_CLI_BODY,        "wac help                     list every command\nwac rules                    list the window presets\nwac apply                    put every matching window back\nwac place --launch notepad.exe --zone 0,0,0.5,1 --remember\nwac rules --forget notepad   delete one preset") \
    X(HELP_CONFIG_NOTE,     "Everything this program saves lives in %APPDATA%\\WindowAssistanceController\\ - one settings file with three rotating backups. No registry keys and no network access.") \
                                                                            \
    /* ---- status bar: permission state ------------------------- */       \
    X(STATUS_PERM_ADMIN,    "administrator")                                \
    X(STATUS_PERM_NORMAL,   "standard user")                                \
                                                                            \
    /* ---- message boxes --------------------------------------- */       \
    X(MSG_CONFIRM_DELETE2,  "This removes it from the settings file.")      \
    X(MSG_LIMIT_REACHED,    "The workspace limit has been reached.")        \
    X(MSG_APPLY_TODO,       "Applying a workspace is not implemented yet.") \
    X(MSG_APPLY_TODO2,      "The launcher arrives in a later stage of the plan.") \
    X(MSG_EDIT_TODO,        "The preset editor is not implemented yet.")    \
    X(MSG_EDIT_TODO2,       "It arrives in a later stage of the plan.")     \
    /* ---- edit mode (stage 6) --------------------------------- */        \
    X(MSG_EDIT_ENTERED,     "Edit mode. Leave with Esc, the toolbar's exit button, or the exit hotkey.") \
    X(MSG_EDIT_ENTER_FAILED,"Could not start edit mode.")                   \
    X(EDIT_EXIT_HOTKEY,     "the exit hotkey")                              \
    X(EDIT_EXIT_ESCAPE,     "Esc")                                          \
    X(EDIT_EXIT_TOOLBAR,    "the toolbar's exit button")                    \
    X(EDIT_EXIT_FUSE,       "the time limit")                               \
    X(EDIT_EXIT_OTHER,      "an unknown reason")                            \
    X(MSG_CLOSE_TODO,       "Nothing has been launched yet, so there is nothing to close.") \
    X(MSG_RENAME_TODO,      "Renaming arrives with the settings dialog.")   \
    X(MSG_HOTKEY_FAILED,    "Could not register any exit hotkey.")          \
    X(MSG_HOTKEY_FAILED2,   "Every candidate is already in use by another program.") \
    X(MSG_HOTKEY_FAILED3,   "You can still exit from the window's File menu.") \
    X(MSG_CONFIG_DAMAGED,   "The settings file is damaged and no backup was found.") \
    X(MSG_CONFIG_DAMAGED2,  "Starting from defaults. The damaged file is left in place.") \
    X(MSG_CONFIG_RECOVERED, "The settings file was damaged. Your workspaces were recovered from a backup.") \
    X(MSG_CONFIG_MODIFIED,  "The settings file was changed outside this program (a text editor, most likely). Its contents were used as they are.") \
    X(MSG_CLOSE_ASK,        "What should closing the window do?")           \
    X(MSG_CLOSE_ASK2,       "Keep running (hide to tray)")                  \
    X(MSG_CLOSE_ASK3,       "Quit the program")                             \
    X(MSG_CLOSE_ASK4,       "Do not ask again")                             \
    X(MSG_OK,               "OK")                                           \
    X(MSG_CANCEL,           "Cancel")                                       \
    X(MSG_ABOUT_TITLE,      "About")                                        \
    X(MSG_ABOUT_BODY,       "Window Assistance Controller")                 \
    X(MSG_ABOUT_STAGE,      "Stage 2 build - window, hotkey, settings.")    \
    X(MSG_ABOUT_CONFIG,     "Config: %APPDATA%\\WindowAssistanceController\\") \
                                                                            \
    /* ---- errors ---------------------------------------------- */       \
    X(ERR_WINDOW_CLASS,     "Could not register the window class.")         \
    X(ERR_MSG_WINDOW,       "Could not create the message window.")         \
    X(ERR_MAIN_WINDOW,      "Could not create the main window.")            \
    X(ERR_TITLE,            "Window Assistance Controller")

typedef enum {
#define X(id, en) STR_##id,
    I18N_ALL(X)
#undef X
    STR_COUNT
} StrId;

/* ------------------------------------------------------------------ */
/* Language selection                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    UI_LANG_AUTO = 0,   /* pick from the user's Windows language */
    UI_LANG_EN       = 1,
    UI_LANG_ZH_CN  = 2
} WacLanguage;

/*
 * Set the language and load its strings.
 *
 * UI_LANG_AUTO resolves to Chinese when the user's Windows UI language
 * is Chinese (any region), otherwise English. Called once at startup with the
 * value from the config file.
 */
void i18n_init(WacLanguage lang);

/* The language actually in use after resolution. */
WacLanguage i18n_current(void);

/* "en" / "zh-CN" - the value stored in the config file. */
const char *i18n_code(WacLanguage lang);

/* Parse a stored code. Unknown values fall back to UI_LANG_AUTO. */
WacLanguage i18n_from_code(const char *code);

/* The text for an id. Never returns NULL: a missing translation falls back
 * to English, and a missing English entry returns an empty string. */
const wchar_t *i18n_text(StrId id);

/*
 * The name of a language as shown in its own language, for the settings
 * dropdown. Deliberately NOT translated: a user who cannot read the current
 * UI language needs to recognise their own in the list.
 */
const wchar_t *i18n_language_label(WacLanguage lang);

#ifdef __cplusplus
}
#endif

#endif /* WAC_I18N_H */
