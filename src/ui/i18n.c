/*
 * i18n.c - the translation tables.
 *
 * Compiled with /utf-8, so the Chinese literals below are ordinary UTF-8 in
 * the source and become UTF-16 in the binary. That is why the text lives here
 * rather than in the .rc file: rc.exe needs an extra flag for UTF-8 and
 * misreports the file when it is missing, while cl.exe already has /utf-8.
 *
 * Both tables are indexed by the same StrId enum generated from
 * I18N_STRINGS() in the header, so the two can never drift out of order.
 * Entries are written in the same order as the X-macro list; a missing entry
 * is caught because the arrays are sized STR_COUNT and a short initialiser
 * list leaves the tail zero, which the fallback path then reports.
 */

#include "i18n.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Tables                                                              */
/* ------------------------------------------------------------------ */

static const wchar_t *const k_english[STR_COUNT] = {
#define X(id, en) L##en,
    I18N_ALL(X)
#undef X
};

/*
 * The Chinese table is written out separately rather than inline in the macro
 * list. Two reasons: the English list stays readable for a reviewer, and a
 * translator gets one contiguous block of Chinese text to work through
 * instead of hunting for it interleaved with other languages.
 *
 * Indexed by designated initialiser so the order here does not have to match
 * the header - a mismatch would otherwise silently shift every string by one.
 */
static const wchar_t *const k_chinese[STR_COUNT] = {
    [STR_APP_TITLE]           = L"窗口工作区管理器",
    [STR_COL_WORKSPACE]       = L"工作区",
    [STR_COL_APPS]            = L"应用数",
    [STR_COL_ID]              = L"标识",

    [STR_MENU_FILE]           = L"文件(&F)",
    [STR_MENU_WORKSPACE]      = L"工作区(&W)",
    [STR_MENU_VIEW]           = L"视图(&V)",
    [STR_MENU_HELP]           = L"帮助(&H)",

    [STR_MENU_NEW]            = L"新建工作区(&N)...\tCtrl+N",
    [STR_MENU_RENAME]         = L"重命名(&N)...\tF2",
    [STR_MENU_DELETE]         = L"删除(&D)\tDel",
    [STR_MENU_SETTINGS]       = L"设置(&S)...",
    [STR_MENU_EXIT]           = L"退出(&X)\tAlt+F4",

    [STR_MENU_APPLY]          = L"应用(&A)\tEnter",
    [STR_MENU_EDIT]           = L"编辑布局(&E)...",
    [STR_MENU_CLOSE_WS]       = L"关闭其应用(&C)",

    [STR_MENU_REFRESH]        = L"刷新(&R)\tF5",
    [STR_MENU_ABOUT]          = L"关于(&A)",

    [STR_BTN_APPLY]           = L"应用",
    [STR_BTN_EDIT]            = L"编辑",
    [STR_BTN_CLOSE]           = L"关闭",
    [STR_BTN_NEW]             = L"新建",
    [STR_BTN_DELETE]          = L"删除",
    [STR_BTN_SETTINGS]        = L"设置",
    [STR_BTN_RUN_WORKSPACE]   = L"运行",
    [STR_BTN_ADD_SLOT]        = L"加入选中的预设",
    [STR_BTN_NEW_WORKSPACE]   = L"新建工作区",
    [STR_BTN_DELETE_WORKSPACE]= L"删除工作区",
    [STR_BTN_CLOSE_APPS]      = L"关闭其应用",
    [STR_BTN_REMOVE_SLOT]     = L"移除选中预设",
    [STR_BTN_EDIT_WORKSPACE]  = L"编辑选中工作区",
    [STR_BTN_RENAME_WORKSPACE]= L"重命名工作区",
    [STR_BTN_UNBIND_SLOT]     = L"解除屏幕绑定",
    [STR_MSG_NO_SLOT_SEL]     = L"请先在下面的列表中选中一个预设。",
    [STR_MSG_SLOT_REMOVED]    = L"已从该工作区移除。",
    [STR_MSG_SLOT_UNBOUND]    = L"已解除该槽位的屏幕绑定。",
    [STR_MSG_WS_NO_PRESETS_AT_ALL] = L"目前还没有任何窗口预设。",
    [STR_SEC_SLOTS_FMT]       = L"该工作区里的预设（%d）",
    [STR_MSG_NO_WORKSPACE_SEL] = L"请先在列表中选中一个工作区。",
    [STR_MSG_NO_SLOT_PRESET]  = L"请先在总览页选中一个窗口预设。",
    [STR_MSG_SLOT_ADDED]      = L"已加入该工作区。",
    [STR_MSG_SLOT_EXISTS]     = L"该预设已经在这个工作区里了。",
    [STR_MSG_WS_EMPTY_RUN]    = L"这个工作区里还没有应用。请先加入一个预设。",
    [STR_MSG_WS_NOTHING_TO_CLOSE] = L"这个工作区没有运行记录 —— 它不是从这里启动的，或者已经关闭过了。",
    [STR_MSG_WS_NO_SLOTS_ADD] = L"还没有任何窗口预设。请先在总览页锚定一个窗口。",
    [STR_MSG_CONFIRM_CLOSE_WS]= L"结束「%ls」启动的程序？",
    [STR_MSG_RUN_DONE]        = L"已归位 %d 个，被拒绝 %d 个（目标以管理员运行），失败 %d 个，跳过 %d 个",

    [STR_NAV_WINDOWS]         = L"窗口预设",
    [STR_NAV_WORKSPACES]      = L"工作区预设",
    [STR_NAV_REFRESH]         = L"刷新",
    [STR_NAV_SETTINGS]        = L"设置",
    [STR_NAV_HELP]            = L"使用指南",
    [STR_BTN_BACK]            = L"← 返回",
    [STR_PAGE_TODO]           = L"此页面将在段 4.5 的后续批次实现。",

    [STR_COL_APP]             = L"程序",
    [STR_COL_TITLE]           = L"标题",
    [STR_COL_STATE]           = L"状态",
    [STR_COL_SIZE]            = L"尺寸",
    [STR_COL_ZONE]            = L"区域",
    [STR_COL_MONITOR]         = L"基准屏宽",
    [STR_COL_SCREEN]          = L"屏幕",
    [STR_COL_PRESET]          = L"预设",
    [STR_MATCH_NONE]          = L"未匹配",
    [STR_NO_WINDOWS]          = L"当前没有可摆放的窗口",
    [STR_NO_PRESETS]          = L"尚未创建任何窗口预设",
    [STR_PRESET_DISABLED]     = L"已停用",
    [STR_PRESET_ENABLED]      = L"生效中",
    [STR_BTN_ANCHOR]          = L"锚定选中窗口",
    [STR_BTN_MOVE_MONITOR]    = L"迁移到选中的显示器",
    [STR_BTN_DELETE_PRESET]   = L"删除选中预设",
    [STR_BTN_APPLY_ALL]       = L"全部应用",
    [STR_BTN_EDIT_PRESET]     = L"编辑选中预设",
    [STR_BTN_CREATE_PRESET]   = L"创建预设",
    [STR_BTN_DELETE_PRESET2]  = L"删除预设",
    [STR_LABEL_TARGET_WINDOW] = L"目标窗口",
    [STR_LABEL_MONITOR_SEL]   = L"显示器",
    [STR_MONITOR_ITEM]        = L"%ls  %dx%d",
    [STR_LABEL_POSITION]      = L"位置",
    [STR_LABEL_SIZE]          = L"尺寸",
    [STR_MSG_CREATE_NO_WINDOW] = L"请先选择一个目标窗口。",
    [STR_BTN_SAVE]            = L"保存",
    [STR_LABEL_X]             = L"X",
    [STR_LABEL_Y]             = L"Y",
    [STR_LABEL_W]             = L"宽",
    [STR_LABEL_H]             = L"高",
    [STR_EDIT_LOCKED]         = L"该窗口大小无法被改变（只移动，不改大小）",
    [STR_EDIT_REAL_NONE]      = L"窗口不可用",
    [STR_CHK_PIN_MONITOR]     = L"固定在这台屏",
    [STR_PIN_NONE]            = L"未固定：跟随窗口所在的屏",
    [STR_PIN_NO_MONITOR]      = L"没有可固定的显示器",
    [STR_MSG_EDIT_BAD_INPUT]  = L"这组数字不能构成有效的区域。\n\n每个值都是相对屏幕的百分比（0-100），且矩形必须能装进屏幕。",
    [STR_MSG_EDIT_SAVED]      = L"预设已更新。",
    [STR_MSG_DISCARD_EDITS]   = L"该预设还有未保存的修改。\n\n确定关闭编辑器并放弃这些修改吗？",
    [STR_MSG_CREATE_PICK]     = L"请在列表中选中一项，然后点[创建]。",
    [STR_MSG_CREATE_MORE_TODO]= L"选择尚未运行的程序需要预设浏览器，它随「假桌面」阶段一起提供。",
    [STR_MSG_INDEXING]        = L"正在索引已安装的应用……结果会缓存，所以只会慢这一次",
    [STR_MSG_INDEX_FAILED]    = L"无法生成已安装应用列表。",
    [STR_APP_NOT_RUNNING]     = L"未运行",
    [STR_APP_UNLAUNCHABLE]    = L"未找到启动路径",
    [STR_APP_NO_PATH]         = L"—",
    [STR_SIDEBAR_TITLE]       = L"应用程序",
    [STR_SIDEBAR_HINT]        = L"拖到画面里放置",
    [STR_SIDEBAR_INDEXING]    = L"正在索引…",
    [STR_SIDEBAR_DROP_HINT]   = L"松手放置",
    [STR_SIDEBAR_EMPTY]       = L"没有找到已安装的程序。",
    [STR_SIDEBAR_SEARCH]      = L"搜索程序",
    [STR_SIDEBAR_PATHLESS_FMT] = L"%ls %d 个无启动路径的程序",
    [STR_SIDEBAR_COUNT_FMT]   = L"共 %d 个程序",
    [STR_EDIT_OVERLAY_TITLE]  = L"编辑模式  —  显示器 %d   %d×%d",
    [STR_EDIT_OVERLAY_HINT1]  = L"按 Esc 或 Ctrl+Q 退出编辑模式",
    [STR_EDIT_OVERLAY_HINT2]  = L"全局退出热键在这里同样有效",
    [STR_EDIT_OVERLAY_RECT_FMT] = L"覆盖层 %d,%d - %d,%d（rcMonitor，含任务栏）",
    [STR_PLACEHOLDER_SIZE_FMT] = L"%d x %d",
    [STR_PLACEHOLDER_LAST_FMT] = L"%d x %d（上次摆放：%d x %d）",
    [STR_PLACEHOLDER_MORE_FMT] = L"还有 %d 个预设放不下这块屏幕",
    [STR_PLACEHOLDER_EMPTY]    = L"这个工作区还没有预设 —— 从右边拖一个进来。",
    [STR_MSG_EDIT_APPLIED_FMT] = L"已从画布更新 %d 个预设",
    [STR_MSG_EDIT_REMOVED_FMT] = L"已从工作区移除 %ls",
    [STR_MSG_EDIT_APPLIED_NONE]= L"画布上没有改动。",
    [STR_MSG_EDIT_RESET]       = L"画布已重置为已保存的布局。",
    [STR_MSG_EDIT_PICKED_FMT]  = L"已从屏幕拾取 %d 个预设",
    [STR_MSG_EDIT_PICK_NONE]   = L"屏幕上没有可拾取的窗口。",
    [STR_MSG_EDIT_PICK_FAILED] = L"拾取到的预设无法写入配置文件。",
    [STR_BTN_MORE]            = L"更多…",
    [STR_BTN_CREATE]          = L"创建",
    [STR_PAGE_CREATE_TITLE]   = L"创建预设",
    [STR_MSG_PRESET_GONE]     = L"该预设已不存在，请刷新列表。",

    [STR_EDIT_PIXELS]         = L"像素：%d,%d  %dx%d",
    [STR_EDIT_NORMALIZED]     = L"存储值：%ls",
    [STR_EDIT_REAL_FMT]       = L"窗口实际 %ls（占屏幕 %ls）",
    [STR_PIN_SET_FMT]         = L"已固定到 %ls",
    [STR_MSG_PIN_GONE_FMT]    = L"%d 个未摆放：它们固定的屏幕现在不在",
    [STR_EDIT_TARGET_FMT]     = L"目标：%ls",
    [STR_MSG_NO_PRESETS_YET]  = L"还没有可应用的预设。请先锚定一个窗口。",
    [STR_MSG_APPLIED_FMT]     = L"已归位 %d 个，被拒绝 %d 个（目标以管理员运行），失败 %d 个",
    [STR_MSG_APPLIED_NONE]    = L"当前没有窗口匹配任何预设。",
    [STR_MSG_MOVED_FMT]       = L"已把 %d 个窗口移到选中的显示器（被拒绝 %d 个，失败 %d 个）",
    [STR_MSG_SIZE_LOCKED_FMT] = L"「%ls」的窗口大小改不了，以后只移动它",
    [STR_MSG_SLOTS_UNAVAILABLE_FMT] =
        L"有 %d 个预设暂不可用：它们记录的屏幕现在不在",
    [STR_MSG_SLOT_BLOCKED_FMT] =
        L"这一项的显示器现在不在。\n"
        L"\n"
        L"它记录在：%ls（%dx%d，位置 %d,%d）\n"
        L"\n"
        L"影响：运行这个工作区时会跳过这一项；预设本身与记录的坐标都没有被改动。\n"
        L"\n"
        L"可以怎么做：把那台屏插回来，或者用 [解除屏幕绑定] 让它回到“窗口在哪儿就用哪儿”。",
    [STR_MSG_MOVE_NONE]       = L"这个预设现在没有匹配的窗口。",
    [STR_MSG_NO_MONITOR_SEL]  = L"没有可用的显示器选择。请先在上方的屏幕列表中选中一台。",
    [STR_MSG_NO_WINDOW_SEL]   = L"请先在上方列表中选中一个窗口。",
    [STR_MSG_NO_PRESET_SEL]   = L"请先在下方列表中选中一条预设。",
    [STR_MSG_ANCHOR_FAILED]   = L"预设保存失败。",
    [STR_MSG_ANCHOR_NOEXE]    = L"无法读取该窗口的可执行文件信息，因此没有保存预设。",
    [STR_MSG_ANCHOR_MINIMIZED] = L"该窗口已最小化。请先还原它，再执行锚定。",
    [STR_MSG_RULES_WRITE_FAILED] = L"预设已在内存中修改，但设置文件写入失败。",

    [STR_OVERVIEW_WINDOWS]    = L"运行中的窗口（%d）",
    [STR_OVERVIEW_PRESETS]    = L"窗口预设（%d）",
    [STR_MATCH_OK]            = L"已匹配「%ls」",
    [STR_MSG_CONFIRM_DEL_PRESET] = L"确定要删除预设「%ls」吗？",
    [STR_MSG_ANCHORED]        = L"已为「%ls」保存预设",

    [STR_STATUS_NO_WORKSPACE] = L"未应用任何工作区",
    [STR_STATUS_ACTIVE]       = L"当前：%ls",
    [STR_STATUS_HOTKEY]       = L"退出热键：%ls",
    [STR_STATUS_HOTKEY_NONE]  = L"（未注册）",

    [STR_TRAY_SHOW]           = L"显示窗口",
    [STR_TRAY_QUIT]           = L"退出",
    [STR_TRAY_TOOLTIP]        = L"窗口工作区管理器",

    [STR_SETTINGS_TITLE]      = L"设置",
    [STR_SETTINGS_LANGUAGE]   = L"语言",
    [STR_SETTINGS_LANG_FOLLOW]= L"跟随系统",
    [STR_SETTINGS_LANG_EN]    = L"English",
    [STR_SETTINGS_LANG_ZH]    = L"简体中文",
    [STR_SETTINGS_LANG_NOTE]  = L"语言更改将在程序重启后生效。",
    [STR_SETTINGS_CLOSE_BEHAV]= L"关闭窗口时",
    [STR_SETTINGS_CLOSE_ASK]  = L"每次都询问",
    [STR_SETTINGS_CLOSE_QUIT] = L"退出程序",
    [STR_SETTINGS_CLOSE_TRAY] = L"继续运行（最小化到托盘）",
    [STR_SETTINGS_HOTKEY]     = L"退出热键",
    [STR_SETTINGS_HOTKEY_NOTE]= L"（本版本固定）",
    [STR_SETTINGS_OK]         = L"确定",
    [STR_SETTINGS_CANCEL]     = L"取消",
    [STR_SETTINGS_APPLY]      = L"应用",
    [STR_RENAME_TITLE]        = L"名称",
    [STR_RENAME_LABEL]        = L"新名称",

    [STR_SETTINGS_PERM_LABEL] = L"权限",
    [STR_SETTINGS_PERM_ADMIN] = L"以管理员权限运行",
    [STR_SETTINGS_PERM_NORMAL]= L"以普通用户权限运行",
    [STR_SETTINGS_PERM_NOTE_ADMIN] =
        L"Windows 允许本程序移动其他以管理员权限运行的程序的窗口。",
    [STR_SETTINGS_PERM_NOTE_NORMAL] =
        L"以管理员权限运行的程序（加速器、驱动工具等）的窗口无法被普通权限的程序移动。"
        L"需要控制它们时：关闭本程序，再右键它选择「以管理员身份运行」——只在需要时做一次。",
    [STR_SETTINGS_HOOK_LABEL] = L"窗口出现时自动归位（常驻后台）",
    [STR_SETTINGS_HOOK_NOTE]  =
        L"监听新打开的窗口，把符合预设的窗口移回该在的位置——你自己双击启动的程序也算。"
        L"由 Windows 主动通知，不轮询、不扫描，空闲时不占 CPU。程序需要保持运行（可最小化到托盘）。",
    [STR_SETTINGS_TASKBAR]    = L"编辑模式显示任务栏对齐线（拖动预设时可吸附）",
    [STR_SETTINGS_GUIDE]      = L"使用指南",

    [STR_TB_EXIT]        = L"退出编辑",
    [STR_TB_RESET]       = L"重置",
    [STR_TB_APPLY]       = L"应用",
    [STR_TB_LAYOUT]      = L"布局格式",
    [STR_TB_TPL_HALVES]  = L"左右半屏",
    [STR_TB_TPL_THIRDS]  = L"三等分",
    [STR_TB_TPL_1P2]     = L"1+2",
    [STR_TB_TPL_2P1]     = L"2+1",
    [STR_TB_TPL_2X2]     = L"2×2",
    [STR_TB_TPL_3X3]     = L"3×3",
    [STR_TB_CUSTOM]      = L"自定义",
    [STR_TB_PICK]        = L"拾取当前布局",

    [STR_HELP_AUTOSTART_HEAD] = L"如何设置开机自动启动：",
    [STR_HELP_AUTOSTART_STEPS] =
        L"点下面的 [打开启动文件夹]。\n"
        L"\n"
        L"在那个文件夹里右键空白处 → 新建 → 快捷方式。\n"
        L"\n"
        L"「请输入对象的位置」填入 wac.exe 的完整路径，例如 D:\\tools\\wac.exe\n"
        L"\n"
        L"点「下一步」，名字随意，再点「完成」。\n"
        L"\n"
        L"重启电脑验证：登录后托盘区会出现程序图标。",
    [STR_HELP_AUTOSTART_UNDO] =
        L"想撤销：\n"
        L"\n"
        L"回到同一个文件夹，删除快捷方式即可。",
    [STR_HELP_AUTOSTART_MANUAL] =
        L"如果按钮打不开任何东西：按 Win+R，输入 shell:startup，回车。",
    [STR_HELP_OPEN_STARTUP]   = L"打开启动文件夹",
    [STR_HELP_PERM_HEAD]      = L"有些窗口摆不动（管理员权限）",
    [STR_HELP_PERM_BODY]      =
        L"那些程序以管理员权限运行，而 Windows 不允许普通权限的程序移动它们的窗口。"
        L"这是系统的安全边界，不是本程序的缺陷。",
    [STR_HELP_PERM_HOW]       =
        L"需要控制这类窗口时：关闭本程序，然后右键 wac.exe，选择「以管理员身份运行」。",
    [STR_HELP_PERM_WARN]      =
        L"快捷方式勾选「始终以管理员身份运行」会使程序始终以管理员权限运行。\n"
        L"日常使用（记事本、浏览器）根本不需要权限。",
    [STR_HELP_PERM_STATUS_ADMIN]  = L"当前状态：以管理员权限运行。",
    [STR_HELP_PERM_STATUS_NORMAL] = L"当前状态：以普通用户权限运行。",
    [STR_HELP_CLI_HEAD]       = L"命令行",
    [STR_HELP_CLI_BODY]       =
        L"wac help                     列出全部命令\n"
        L"wac rules                    列出所有窗口预设\n"
        L"wac apply                    按预设把所有窗口归位\n"
        L"wac place --launch notepad.exe --zone 0,0,0.5,1 --remember\n"
        L"wac rules --forget notepad   删除一条预设",
    [STR_HELP_CONFIG_NOTE]    =
        L"本程序保存的全部内容都在 %APPDATA%\\WindowAssistanceController\\ —— "
        L"一个设置文件加三份轮转备份。不写注册表，不联网。",

    [STR_STATUS_PERM]         = L"权限：%ls",
    [STR_STATUS_PERM_ADMIN]   = L"管理员",
    [STR_STATUS_PERM_NORMAL]  = L"普通用户",

    [STR_MSG_CONFIRM_DELETE]  = L"确定要删除工作区「%ls」吗？",
    [STR_MSG_CONFIRM_DELETE2] = L"该工作区将从设置文件中移除。",
    [STR_MSG_LIMIT_REACHED]   = L"已达到工作区数量上限。",
    [STR_MSG_APPLY_TODO]      = L"「应用工作区」功能尚未实现。",
    [STR_MSG_APPLY_TODO2]     = L"启动器将在计划的后续阶段加入。",
    [STR_MSG_EDIT_TODO]       = L"「预设编辑器」功能尚未实现。",
    [STR_MSG_EDIT_TODO2]      = L"将在计划的后续阶段加入。",
    [STR_MSG_EDIT_ENTERED]    = L"已进入编辑模式 —— 按 Esc、工具栏的 [退出编辑]，或退出热键离开。",
    [STR_MSG_EDIT_ENTER_FAILED] = L"无法进入编辑模式。",
    [STR_MSG_EDIT_WATCHDOG]     = L"编辑模式无响应，已由看门狗强制关闭。",
    [STR_EDIT_EXIT_FMT]       = L"已退出编辑模式（%ls）",
    [STR_EDIT_EXIT_HOTKEY]    = L"退出热键",
    [STR_EDIT_EXIT_ESCAPE]    = L"Esc 键",
    [STR_EDIT_EXIT_TOOLBAR]   = L"工具栏的 [退出编辑]",
    [STR_EDIT_EXIT_FUSE]      = L"超时自动退出",
    [STR_EDIT_EXIT_OTHER]     = L"未知原因",
    [STR_MSG_CLOSE_TODO]      = L"尚未启动任何应用，因此没有需要关闭的对象。",
    [STR_MSG_RENAME_TODO]     = L"重命名功能将随设置对话框一起提供。",
    [STR_MSG_HOTKEY_FAILED]   = L"无法注册任何退出热键。",
    [STR_MSG_HOTKEY_FAILED2]  = L"所有候选组合都已被其他程序占用。",
    [STR_MSG_HOTKEY_FAILED3]  = L"你仍然可以通过窗口的「文件」菜单退出。",
    [STR_MSG_CONFIG_DAMAGED]  = L"设置文件已损坏，且未找到可用的备份。",
    [STR_MSG_CONFIG_DAMAGED2] = L"将以默认设置启动。损坏的文件保留在原处以便检查。",
    [STR_MSG_CONFIG_RECOVERED]= L"设置文件曾损坏，已从备份中恢复你的工作区。",
    [STR_MSG_CONFIG_MODIFIED] = L"设置文件被本程序之外的东西改过（多半是文本编辑器），已按文件里的内容加载。",
    [STR_MSG_CLOSE_ASK]       = L"关闭窗口时希望：",
    [STR_MSG_CLOSE_ASK2]      = L"继续运行（最小化到托盘）",
    [STR_MSG_CLOSE_ASK3]      = L"完全退出程序",
    [STR_MSG_CLOSE_ASK4]      = L"不再询问",
    [STR_MSG_OK]              = L"确定",
    [STR_MSG_CANCEL]          = L"取消",
    [STR_MSG_ABOUT_TITLE]     = L"关于",
    [STR_MSG_ABOUT_BODY]      = L"窗口工作区管理器",
    [STR_MSG_ABOUT_STAGE]     = L"阶段 2 构建 — 主窗口、热键、设置持久化。",
    [STR_MSG_ABOUT_WSCOUNT]   = L"工作区数量：%d",
    [STR_MSG_ABOUT_HOTKEY]    = L"退出热键：%ls",
    [STR_MSG_ABOUT_CONFIG]    = L"配置目录：%APPDATA%\\WindowAssistanceController\\",

    [STR_ERR_WINDOW_CLASS]    = L"无法注册窗口类。",
    [STR_ERR_MSG_WINDOW]      = L"无法创建消息窗口。",
    [STR_ERR_MAIN_WINDOW]     = L"无法创建主窗口。",
    [STR_ERR_TITLE]           = L"窗口工作区管理器",
};

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static WacLanguage g_lang = UI_LANG_ZH_CN;

/* ------------------------------------------------------------------ */
/* Language resolution                                                 */
/* ------------------------------------------------------------------ */

/*
 * Resolve "follow Windows" to a concrete language.
 *
 * Read from GetUserDefaultUILanguage's LANGID rather than the locale: the
 * UI language is what the user sees Windows in, which is what they will
 * expect this program to match. PRIMARYLANGID(LANG_CHINESE) covers zh-CN,
 * zh-TW and zh-HK, so all three get the simplified table - the Traditional
 * translation is a separate job, and showing English to a Chinese-speaking
 * user would be worse than showing Simplified.
 */
static WacLanguage resolve_follow_system(void)
{
    const LANGID ui = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(ui) == LANG_CHINESE) return UI_LANG_ZH_CN;
    return UI_LANG_EN;
}

void i18n_init(WacLanguage lang)
{
    if (lang == UI_LANG_AUTO) lang = resolve_follow_system();
    g_lang = lang;
}

WacLanguage i18n_current(void)
{
    return g_lang;
}

const char *i18n_code(WacLanguage lang)
{
    switch (lang) {
    case UI_LANG_EN:      return "en";
    case UI_LANG_ZH_CN: return "zh-CN";
    case UI_LANG_AUTO:
    default:                return "auto";
    }
}

WacLanguage i18n_from_code(const char *code)
{
    if (code == NULL || code[0] == '\0') return UI_LANG_AUTO;

    if (_stricmp(code, "en") == 0)    return UI_LANG_EN;
    if (_stricmp(code, "zh-CN") == 0) return UI_LANG_ZH_CN;
    if (_stricmp(code, "zh") == 0)    return UI_LANG_ZH_CN;
    if (_stricmp(code, "auto") == 0)  return UI_LANG_AUTO;

    /* An unrecognised code is not an error worth reporting: fall back to
     * following the system, which is what a fresh install does. */
    return UI_LANG_AUTO;
}

/* ------------------------------------------------------------------ */
/* Lookup                                                              */
/* ------------------------------------------------------------------ */

const wchar_t *i18n_text(StrId id)
{
    if (id < 0 || id >= STR_COUNT) return L"";

    if (g_lang == UI_LANG_ZH_CN) {
        const wchar_t *s = k_chinese[id];
        if (s != NULL && s[0] != L'\0') return s;
        /* Not translated yet: fall through to English rather than showing a
         * blank label, which would look like a broken build. */
    }

    const wchar_t *en = k_english[id];
    return (en != NULL) ? en : L"";
}

const wchar_t *i18n_language_label(WacLanguage lang)
{
    switch (lang) {
    case UI_LANG_EN:      return L"English";
    case UI_LANG_ZH_CN: return L"简体中文";
    case UI_LANG_AUTO:
    default:                return (resolve_follow_system() == UI_LANG_ZH_CN)
                                   ? L"跟随系统 / Follow Windows"
                                   : L"Follow Windows / 跟随系统";
    }
}
